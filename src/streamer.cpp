#include "streamer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ranges>

#include "decoder.hpp"
#include "log.hpp"
#include "signals.hpp"

namespace ices {

using namespace std::chrono_literals;

namespace {

constexpr std::size_t kInputChunk = 4096;   // bytes read per iteration
constexpr std::size_t kSendChunk = 4096;    // bytes per shout_send
constexpr auto kErrorDelay = 999ms;         // pause when every stream fails
constexpr int kMaxConsecutiveErrors = 10;   // track errors before giving up
constexpr int kMaxStreamErrors = 10;        // send errors before giving up
constexpr std::size_t kMaxPcmPerIteration = 1 << 16;  // floats
// Encoders run asynchronously; don't feed one more than this far ahead of
// what has actually gone out, so track changes and titles stay in step.
constexpr double kMaxEncoderLead = 1.0;     // seconds
constexpr double kTailSlack = 0.25;         // encoder latency tolerated at track end
constexpr auto kTailTimeout = 3s;
// Fail the track if GStreamer stops making progress without reporting why.
constexpr auto kStallTimeout = 15s;

}  // namespace

Streamer::Streamer(const Config& config, Playlist& playlist)
    : config_(config), playlist_(playlist), cue_(config.base_directory) {
  for (auto [i, sc] : std::views::enumerate(config.streams)) {
    auto stream = std::make_unique<ShoutStream>(sc, static_cast<std::size_t>(i));
    stream->on_connect = [this](ShoutStream& s) {
      // The server may not have registered us yet; delay the first update.
      if (!song_.empty()) metadata_.update(s.config(), song_, 500ms);
    };
    streams_.push_back(std::move(stream));
  }

  if (config.playlist.crossfade > 0) {
    if (std::ranges::all_of(config.streams, &StreamConfig::reencode))
      crossfader_.emplace(config.playlist.crossfade);
    else
      log::info("Crossfading requires reencoding on every stream; crossfade disabled.");
  }
}

Streamer::~Streamer() {
  for (auto& s : streams_) s->close();
  cue_.remove();
}

int Streamer::run() {
  int consecutive_errors = 0;

  while (!signals::quit_requested()) {
    if (signals::take_hup()) handle_hup();

    auto path = playlist_.next();
    if (!path || path->empty()) {
      log::info("Playlist file name is empty, shutting down.");
      drain_encoders(false);
      return 0;
    }
    cue_.set_lineno(playlist_.lineno());

    // Stops a playlist that only produces invalid file names from spinning.
    if (consecutive_errors > kMaxConsecutiveErrors) {
      log::info("Exiting after {} consecutive errors.", kMaxConsecutiveErrors);
      return 1;
    }

    auto source = InputSource::open(*path);
    if (!source) {
      log::info("Error opening {}: {}", *path, source.error());
      ++consecutive_errors;
      continue;
    }
    if ((*source)->type() != InputType::mp3 &&
        std::ranges::any_of(streams_, [](auto& s) { return !s->config().reencode; })) {
      log::info("Cannot play {} without reencoding", *path);
      ++consecutive_errors;
      continue;
    }

    std::string error;
    switch (play(**source, error)) {
      case Result::ok: consecutive_errors = 0; break;
      case Result::error:
        log::info("Encountered error while transfering {}: {}", *path, error);
        ++consecutive_errors;
        break;
      case Result::quit: return 0;
      case Result::fatal: return 1;
    }
  }
  return 0;
}

void Streamer::handle_hup() {
  log::debug("Caught SIGHUP, cycling logfiles and reloading playlist...");
  if (auto r = log::reopen(); !r) log::info("{}", r.error());
  playlist_.reload();
}

bool Streamer::needs_reencoding(const InputSource& src, const ShoutStream& stream) const {
  const auto& c = stream.config();
  return src.type() != InputType::mp3 || src.bitrate() != c.bitrate ||
         (c.samplerate > 0 && src.samplerate() != c.samplerate) ||
         (c.channels > 0 && src.channels() != c.channels);
}

void Streamer::choose_modes(const InputSource& src) {
  for (auto& s : streams_) {
    auto& c = s->config();
    // Like LAME, pick an output rate for unset streams from the first track.
    if (c.reencode && c.samplerate <= 0 && src.samplerate() > 0)
      c.samplerate = lame_default_samplerate(src.samplerate(), c.bitrate, c.channels == 1 ? 1 : 2);

    // Don't reencode if the source is MP3 with matching parameters (unless
    // crossfading, which needs every sample to pass through the mixer).
    s->mode = (c.reencode && (crossfader_ || needs_reencoding(src, *s))) ? ShoutStream::Mode::encode
                                                                        : ShoutStream::Mode::raw;
  }
}

std::expected<void, std::string> Streamer::ensure_encoder(ShoutStream& s) {
  if (s.encoder) return {};
  auto& c = s.config();
  const int channels = c.channels == 1 ? 1 : 2;
  const int rate = c.samplerate > 0 ? valid_mp3_samplerate(c.samplerate)
                                    : lame_default_samplerate(pcm_rate_, c.bitrate, channels);
  c.samplerate = rate;
  const int bitrate = valid_cbr_bitrate(c.bitrate, rate);
  if (bitrate != c.bitrate)
    log::info("{}: {} kbps is not a valid MP3 bitrate at {} Hz, using {} kbps", c.mount,
              c.bitrate, rate, bitrate);

  auto enc = Encoder::create(pcm_rate_, rate, channels, bitrate);
  if (!enc) return std::unexpected(enc.error());
  s.encoder = std::move(*enc);
  s.encode_rate = rate;
  s.encode_channels = channels;
  s.encode_bitrate = bitrate;
  s.pushed_seconds = 0;
  s.consumed_bytes = 0;
  s.set_audio_format(rate, channels);
  return {};
}

std::expected<void, std::string> Streamer::encode(std::span<const float> pcm) {
  for (auto& s : streams_) {
    if (s->mode != ShoutStream::Mode::encode) continue;
    if (auto r = ensure_encoder(*s); !r) return r;
    s->encoder->push(pcm);
    s->pushed_seconds += static_cast<double>(pcm.size() / 2) / pcm_rate_;
  }
  return {};
}

// Send `raw` to pass-through streams and each encoding stream's pending
// output, interleaving the streams chunk by chunk so libshout's pacing of
// one stream doesn't starve the others.
Streamer::Result Streamer::send_pending(std::span<const std::uint8_t> raw) {
  struct Out {
    ShoutStream* stream;
    std::span<const std::uint8_t> data;
    std::size_t offset = 0;
    bool failed = false;
  };
  std::vector<Out> outs;
  for (auto& s : streams_) {
    if (s->mode == ShoutStream::Mode::raw && !raw.empty())
      outs.push_back({s.get(), raw});
    else if (s->mode == ShoutStream::Mode::encode && !s->pending.empty())
      outs.push_back({s.get(), s->pending});
  }

  Result result = Result::ok;
  while (result == Result::ok) {
    bool attempted = false, any_ok = false;
    for (auto& o : outs) {
      if (o.offset >= o.data.size()) continue;
      attempted = true;
      o.failed = false;
      const auto chunk = o.data.subspan(o.offset, std::min(kSendChunk, o.data.size() - o.offset));
      const auto rc = o.stream->send(chunk);
      if (rc == ShoutStream::SendResult::ok) {
        o.offset += chunk.size();
        any_ok = true;
        continue;
      }
      o.failed = true;
      if (o.stream->errors() > kMaxStreamErrors) {
        log::info("Too many stream errors, giving up");
        result = Result::fatal;
        break;
      }
      if (rc == ShoutStream::SendResult::failed)
        log::info("Error during send: {}", o.stream->last_error());
    }
    if (!attempted || result != Result::ok) break;

    if (any_ok) {
      // Healthy streams carry on; failed ones lose this data.
      for (auto& o : outs)
        if (o.failed) o.offset = o.data.size();
    } else {
      // Every stream failed: pause before trying to reconnect.
      signals::interruptible_sleep(kErrorDelay);
      if (signals::quit_requested()) result = Result::quit;
    }
  }
  for (auto& s : streams_) {
    if (s->mode == ShoutStream::Mode::encode) s->consumed_bytes += s->pending.size();
    s->pending.clear();
  }
  return result;
}

Streamer::Result Streamer::drain_encoders(bool only_leaving_encode_mode) {
  bool any = false;
  for (auto& s : streams_) {
    if (!s->encoder) continue;
    if (only_leaving_encode_mode && s->mode == ShoutStream::Mode::encode) continue;
    s->encoder->drain(s->pending, 2s);
    s->encoder.reset();
    // Send the tail through the encode path even if the stream now goes raw.
    if (!s->pending.empty()) any = true;
  }
  if (!any) return Result::ok;

  std::vector<ShoutStream::Mode> saved;
  for (auto& s : streams_) {
    saved.push_back(s->mode);
    s->mode = ShoutStream::Mode::encode;
  }
  const Result r = send_pending({});
  for (auto [s, m] : std::views::zip(streams_, saved)) s->mode = m;
  return r;
}

void Streamer::update_metadata() {
  for (auto& s : streams_)
    if (s->connected()) metadata_.update(s->config(), song_);
}

Streamer::Result Streamer::play(InputSource& src, std::string& error) {
  choose_modes(src);
  // Streams switching to pass-through must flush their encoder first.
  if (auto r = drain_encoders(true); r != Result::ok) return r;
  const bool decode = std::ranges::any_of(
      streams_, [](auto& s) { return s->mode == ShoutStream::Mode::encode; });

  log::info("Playing {}", src.path());

  const auto playlist_title = playlist_.metadata();
  // Tags of non-MP3 files only become known once decoding has started.
  bool title_known = playlist_title || src.type() == InputType::mp3 || !decode;
  if (title_known) {
    song_ = compose_song_title(playlist_title, src.tags(), src.path());
    update_metadata();
  }
  for (auto& s : streams_) s->reset_errors();

  std::unique_ptr<Decoder> decoder;
  if (decode) {
    // All encoders share one decode rate, fixed by the first decoded track.
    // It must be at least every stream's output rate (rates chosen later
    // are derived from it and never exceed it). If nothing is known yet the
    // track's native rate is used.
    if (pcm_rate_ == 0) {
      int rate = src.samplerate();
      for (auto& s : streams_)
        if (s->config().reencode && s->config().samplerate > 0)
          rate = std::max(rate, valid_mp3_samplerate(s->config().samplerate));
      pcm_rate_ = rate;
    }
    auto d = Decoder::create(pcm_rate_, src.filesize());
    if (!d) {
      error = d.error();
      return Result::error;
    }
    decoder = std::move(*d);
  }

  cue_.update(src, src.tags());
  auto last_cue = std::chrono::steady_clock::now();

  std::array<std::uint8_t, kInputChunk> buf{};
  std::vector<float> pcm;
  bool input_done = false;
  bool decoder_done = !decoder;
  bool first_pcm = true;
  std::optional<std::chrono::steady_clock::time_point> tail_deadline;
  auto last_progress = std::chrono::steady_clock::now();
  auto encoder_lead = [&] {
    double lead = 0;
    for (auto& s : streams_)
      if (s->mode == ShoutStream::Mode::encode) lead = std::max(lead, s->encoder_lead());
    return lead;
  };

  while (true) {
    if (signals::quit_requested()) return Result::quit;
    if (signals::take_skip()) {
      log::debug("Caught SIGUSR1, skipping to next track...");
      break;
    }
    if (signals::take_hup()) handle_hup();

    // 1. Read input (bounded look-ahead when decoding).
    std::span<const std::uint8_t> raw;
    if (!input_done && (!decoder || decoder->wants_input())) {
      auto n = src.read(buf);
      if (!n) {
        error = n.error();
        return Result::error;
      }
      if (*n == 0) {
        log::debug("Done sending");
        input_done = true;
        if (decoder) decoder->end_of_stream();
      } else {
        raw = std::span(buf).first(*n);
        if (decoder) decoder->push(raw);
      }
    }

    // 2. Decode, crossfade and re-encode.
    if (decoder && !decoder_done && encoder_lead() < kMaxEncoderLead) {
      if (auto r = decoder->poll_bus(); !r) {
        error = r.error();
        return Result::error;
      }
      pcm.clear();
      auto timeout = raw.empty() ? 20ms : 0ms;
      while (pcm.size() < kMaxPcmPerIteration) {
        const auto st = decoder->pull(pcm, timeout);
        timeout = 0ms;
        if (st == Decoder::Status::eos) decoder_done = true;
        if (st != Decoder::Status::data) break;
      }

      if (!pcm.empty()) {
        if (first_pcm) {
          first_pcm = false;
          // Tag messages are posted just ahead of the first buffer.
          if (auto r = decoder->poll_bus(); !r) {
            error = r.error();
            return Result::error;
          }
          if (pcm_rate_ == 0) pcm_rate_ = decoder->rate();
          if (decoder->bitrate_kbps() && !src.bitrate()) src.set_bitrate(decoder->bitrate_kbps());
          if (crossfader_) {
            if (!crossfader_->ready()) crossfader_->set_rate(pcm_rate_);
            auto secs = decoder->duration_seconds();
            if (!secs && src.filesize() && src.bitrate())
              secs = static_cast<double>(src.filesize()) / (src.bitrate() * 128.0);
            crossfader_->new_track(secs);
          }
          src.merge_tags(decoder->tags());
          if (!title_known) {
            song_ = compose_song_title(playlist_title, src.tags(), src.path());
            update_metadata();
            title_known = true;
          }
        }
        last_progress = std::chrono::steady_clock::now();
        if (crossfader_) crossfader_->process(pcm);
        if (auto r = encode(pcm); !r) {
          error = r.error();
          return Result::error;
        }
      }
    }

    // 3. Collect encoder output and send everything. A stream that is far
    // enough ahead (or finishing the track) waits briefly for its encoder.
    const bool finishing = input_done && decoder_done;
    for (auto& s : streams_) {
      if (s->mode != ShoutStream::Mode::encode || !s->encoder) continue;
      if (auto r = s->encoder->poll_bus(); !r) {
        s->encoder.reset();
        error = r.error();
        return Result::error;
      }
      const double lead = s->encoder_lead();
      const bool wait = lead >= kMaxEncoderLead || (finishing && lead > kTailSlack);
      const auto before = s->pending.size();
      s->encoder->pull_available(s->pending, wait ? 10ms : 0ms);
      if (s->pending.size() != before) last_progress = std::chrono::steady_clock::now();
    }
    if (auto r = send_pending(raw); r != Result::ok) return r;

    const auto now = std::chrono::steady_clock::now();
    // Reading from a FIFO may legitimately block for a long time, but once
    // the input is exhausted (or encoders are waiting) progress must follow.
    if ((input_done || encoder_lead() >= kMaxEncoderLead) && !finishing &&
        now - last_progress > kStallTimeout) {
      error = "decoder/encoder stalled";
      return Result::error;
    }
    if (now - last_cue >= 1s) {
      cue_.update(src, src.tags());
      last_cue = now;
    }

    if (finishing) {
      if (encoder_lead() <= kTailSlack) break;
      if (!tail_deadline) tail_deadline = now + kTailTimeout;
      if (now >= *tail_deadline) {
        log::debug("Encoder did not catch up at end of track, continuing");
        break;
      }
    }
  }

  if (!title_known) {
    if (decoder) src.merge_tags(decoder->tags());
    song_ = compose_song_title(playlist_title, src.tags(), src.path());
    update_metadata();
  }
  return Result::ok;
}

}  // namespace ices
