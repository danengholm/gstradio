// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/streamer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "src/decoder.h"
#include "src/log.h"
#include "src/signals.h"

namespace ices {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;

constexpr std::size_t kInputChunk = 4096;             // Bytes read per iteration.
constexpr std::size_t kSendChunk = 4096;              // Bytes per shout_send().
constexpr Milliseconds kErrorDelay{999};              // Pause when every stream fails.
constexpr int kMaxConsecutiveErrors = 10;             // Track errors before giving up.
constexpr int kMaxStreamErrors = 10;                  // Send errors before giving up.
constexpr std::size_t kMaxPcmPerIteration = 1 << 16;  // Floats.
// The server may not have registered a new source yet; delay its first title.
constexpr Milliseconds kConnectMetadataDelay{500};
constexpr Seconds kDrainTimeout{2};
constexpr Seconds kCueInterval{1};
constexpr Milliseconds kDecoderWait{20};
constexpr Milliseconds kEncoderWait{10};
constexpr Milliseconds kNoWait{0};
// Encoders run asynchronously; don't feed one more than this far ahead of
// what has actually gone out, so track changes and titles stay in step.
constexpr double kMaxEncoderLead = 1.0;  // Seconds.
// Encoder latency tolerated at the end of a track.
constexpr double kTailSlack = 0.25;  // Seconds.
constexpr Seconds kTailTimeout{3};
// Fail the track if GStreamer stops making progress without reporting why.
constexpr Seconds kStallTimeout{15};

}  // namespace

std::expected<std::unique_ptr<Streamer>, std::string> Streamer::Create(const Config& config, Playlist& playlist) {
    std::unique_ptr<Streamer> streamer(new Streamer(config, playlist));
    for (const StreamConfig& sc : config.streams) {
        auto stream = ShoutStream::Create(sc);
        if (!stream) return std::unexpected(stream.error());
        (*stream)->set_on_connect([s = streamer.get()](ShoutStream& connected) {
            if (!s->song_.empty()) {
                s->metadata_.Update(connected.config(), s->song_, kConnectMetadataDelay);
            }
        });
        streamer->outputs_.push_back(Output{.stream = std::move(*stream), .config = sc});
    }

    if (config.playlist.crossfade > 0) {
        if (std::ranges::all_of(config.streams, &StreamConfig::reencode)) {
            streamer->crossfader_.emplace(config.playlist.crossfade);
        } else {
            log::Info(
                "Crossfading requires reencoding on every stream; crossfade "
                "disabled.");
        }
    }
    return streamer;
}

Streamer::Streamer(const Config& config, Playlist& playlist)
    : config_(config), playlist_(playlist), cue_(config.base_directory) {}

Streamer::~Streamer() {
    for (Output& out : outputs_) out.stream->Close();
    cue_.Remove();
}

int Streamer::Run() {
    int consecutive_errors = 0;

    while (!signals::QuitRequested()) {
        if (signals::TakeHup()) HandleHup();

        auto path = playlist_.Next();
        if (!path || path->empty()) {
            log::Info("Playlist file name is empty, shutting down.");
            DrainEncoders(false);
            return 0;
        }
        cue_.set_lineno(playlist_.Lineno());

        // Stops a playlist that only produces invalid file names from spinning.
        if (consecutive_errors > kMaxConsecutiveErrors) {
            log::Info("Exiting after {} consecutive errors.", kMaxConsecutiveErrors);
            return 1;
        }

        auto source = InputSource::Open(*path);
        if (!source) {
            log::Info("Error opening {}: {}", *path, source.error());
            ++consecutive_errors;
            continue;
        }
        if ((*source)->type() != InputType::kMp3 &&
            std::ranges::any_of(outputs_, [](const Output& out) { return !out.config.reencode; })) {
            log::Info("Cannot play {} without reencoding", *path);
            ++consecutive_errors;
            continue;
        }

        std::string error;
        switch (Play(**source, error)) {
            case Result::kOk:
                consecutive_errors = 0;
                break;
            case Result::kError:
                log::Info("Encountered error while transfering {}: {}", *path, error);
                ++consecutive_errors;
                break;
            case Result::kQuit:
                return 0;
            case Result::kFatal:
                return 1;
        }
    }
    return 0;
}

void Streamer::HandleHup() {
    log::Debug("Caught SIGHUP, cycling logfiles and reloading playlist...");
    if (auto r = log::Reopen(); !r) log::Info("{}", r.error());
    playlist_.Reload();
}

double Streamer::EncoderLead(const Output& out) {
    if (!out.encoder || out.encode_bitrate <= 0) return 0;
    return out.pushed_seconds - static_cast<double>(out.consumed_bytes) / (out.encode_bitrate * 125.0);
}

double Streamer::MaxEncoderLead() const {
    double lead = 0;
    for (const Output& out : outputs_) {
        if (out.mode == Output::Mode::kEncode) {
            lead = std::max(lead, EncoderLead(out));
        }
    }
    return lead;
}

bool Streamer::NeedsReencoding(const InputSource& src, const Output& out) {
    const StreamConfig& c = out.config;
    return src.type() != InputType::kMp3 || src.bitrate() != c.bitrate ||
           (c.samplerate > 0 && src.samplerate() != c.samplerate) || (c.channels > 0 && src.channels() != c.channels);
}

void Streamer::ChooseModes(const InputSource& src) {
    for (Output& out : outputs_) {
        StreamConfig& c = out.config;
        // Like LAME, pick an output rate for unset streams from the first track.
        if (c.reencode && c.samplerate <= 0 && src.samplerate() > 0) {
            c.samplerate = LameDefaultSamplerate(src.samplerate(), c.bitrate, c.channels == 1 ? 1 : 2);
        }
        // Don't reencode if the source is MP3 with matching parameters (unless
        // crossfading, which needs every sample to pass through the mixer).
        const bool encode = c.reencode && (crossfader_ || NeedsReencoding(src, out));
        out.mode = encode ? Output::Mode::kEncode : Output::Mode::kRaw;
    }
}

std::expected<void, std::string> Streamer::EnsureEncoder(Output& out) {
    if (out.encoder) return {};
    StreamConfig& c = out.config;
    const int channels = c.channels == 1 ? 1 : 2;
    const int rate =
        c.samplerate > 0 ? ValidMp3Samplerate(c.samplerate) : LameDefaultSamplerate(pcm_rate_, c.bitrate, channels);
    c.samplerate = rate;
    const int bitrate = ValidCbrBitrate(c.bitrate, rate);
    if (bitrate != c.bitrate) {
        log::Info("{}: {} kbps is not a valid MP3 bitrate at {} Hz, using {} kbps", c.mount, c.bitrate, rate, bitrate);
    }

    auto encoder = Encoder::Create(pcm_rate_, rate, channels, bitrate);
    if (!encoder) return std::unexpected(encoder.error());
    out.encoder = std::move(*encoder);
    out.encode_bitrate = bitrate;
    out.pushed_seconds = 0;
    out.consumed_bytes = 0;
    out.stream->SetAudioFormat(rate, channels);
    return {};
}

std::expected<void, std::string> Streamer::Encode(std::span<const float> pcm) {
    for (Output& out : outputs_) {
        if (out.mode != Output::Mode::kEncode) continue;
        if (auto r = EnsureEncoder(out); !r) return r;
        out.encoder->Push(pcm);
        out.pushed_seconds += static_cast<double>(pcm.size() / 2) / pcm_rate_;
    }
    return {};
}

// Sends `raw` to pass-through streams and each encoding stream's pending
// output, interleaving the streams chunk by chunk so libshout's pacing of
// one stream doesn't starve the others.
Streamer::Result Streamer::SendPending(std::span<const std::uint8_t> raw) {
    struct Transfer {
        ShoutStream* stream;
        std::span<const std::uint8_t> data;
        std::size_t offset = 0;
        bool failed = false;
    };
    std::vector<Transfer> transfers;
    for (Output& out : outputs_) {
        if (out.mode == Output::Mode::kRaw && !raw.empty()) {
            transfers.push_back({out.stream.get(), raw});
        } else if (out.mode == Output::Mode::kEncode && !out.pending.empty()) {
            transfers.push_back({out.stream.get(), out.pending});
        }
    }

    Result result = Result::kOk;
    while (result == Result::kOk) {
        bool attempted = false;
        bool any_ok = false;
        for (Transfer& t : transfers) {
            if (t.offset >= t.data.size()) continue;
            attempted = true;
            t.failed = false;
            const auto chunk = t.data.subspan(t.offset, std::min(kSendChunk, t.data.size() - t.offset));
            const ShoutStream::SendResult rc = t.stream->Send(chunk);
            if (rc == ShoutStream::SendResult::kOk) {
                t.offset += chunk.size();
                any_ok = true;
                continue;
            }
            t.failed = true;
            if (t.stream->errors() > kMaxStreamErrors) {
                log::Info("Too many stream errors, giving up");
                result = Result::kFatal;
                break;
            }
            if (rc == ShoutStream::SendResult::kFailed) {
                log::Info("Error during send: {}", t.stream->last_error());
            }
        }
        if (!attempted || result != Result::kOk) break;

        if (any_ok) {
            // Healthy streams carry on; failed ones lose this data.
            for (Transfer& t : transfers) {
                if (t.failed) t.offset = t.data.size();
            }
        } else {
            // Every stream failed: pause before trying to reconnect.
            signals::InterruptibleSleep(kErrorDelay);
            if (signals::QuitRequested()) result = Result::kQuit;
        }
    }
    for (Output& out : outputs_) {
        if (out.mode == Output::Mode::kEncode) {
            out.consumed_bytes += out.pending.size();
        }
        out.pending.clear();
    }
    return result;
}

Streamer::Result Streamer::DrainEncoders(bool only_leaving_encode_mode) {
    bool any = false;
    for (Output& out : outputs_) {
        if (!out.encoder) continue;
        if (only_leaving_encode_mode && out.mode == Output::Mode::kEncode) {
            continue;
        }
        out.encoder->Drain(out.pending, kDrainTimeout);
        out.encoder.reset();
        if (!out.pending.empty()) any = true;
    }
    if (!any) return Result::kOk;

    // Send the tails through the encode path even if a stream now goes raw.
    std::vector<Output::Mode> saved;
    for (Output& out : outputs_) {
        saved.push_back(out.mode);
        out.mode = Output::Mode::kEncode;
    }
    const Result r = SendPending({});
    for (auto [out, mode] : std::views::zip(outputs_, saved)) out.mode = mode;
    return r;
}

void Streamer::UpdateMetadata() {
    for (Output& out : outputs_) {
        if (out.stream->IsConnected()) metadata_.Update(out.config, song_);
    }
}

Streamer::Result Streamer::Play(InputSource& src, std::string& error) {
    ChooseModes(src);
    // Streams switching to pass-through must flush their encoder first.
    if (Result r = DrainEncoders(true); r != Result::kOk) return r;
    const bool decode =
        std::ranges::any_of(outputs_, [](const Output& out) { return out.mode == Output::Mode::kEncode; });

    log::Info("Playing {}", src.path());

    const std::optional<std::string> playlist_title = playlist_.Metadata();
    // Tags of non-MP3 files only become known once decoding has started.
    bool title_known = playlist_title || src.type() == InputType::kMp3 || !decode;
    if (title_known) {
        song_ = ComposeSongTitle(playlist_title, src.tags(), src.path());
        UpdateMetadata();
    }
    for (Output& out : outputs_) out.stream->ResetErrors();

    std::unique_ptr<Decoder> decoder;
    if (decode) {
        // All encoders share one decode rate, fixed by the first decoded track.
        // It must be at least every stream's output rate (rates chosen later
        // are derived from it and never exceed it). If nothing is known yet the
        // track's native rate is used.
        if (pcm_rate_ == 0) {
            int rate = src.samplerate();
            for (const Output& out : outputs_) {
                if (out.config.reencode && out.config.samplerate > 0) {
                    rate = std::max(rate, ValidMp3Samplerate(out.config.samplerate));
                }
            }
            pcm_rate_ = rate;
        }
        auto d = Decoder::Create(pcm_rate_, src.filesize());
        if (!d) {
            error = d.error();
            return Result::kError;
        }
        decoder = std::move(*d);
    }

    cue_.Update(src, src.tags());
    Clock::time_point last_cue = Clock::now();

    std::array<std::uint8_t, kInputChunk> buf{};
    std::vector<float> pcm;
    bool input_done = false;
    bool decoder_done = !decoder;
    bool first_pcm = true;
    std::optional<Clock::time_point> tail_deadline;
    Clock::time_point last_progress = Clock::now();

    while (true) {
        if (signals::QuitRequested()) return Result::kQuit;
        if (signals::TakeSkip()) {
            log::Debug("Caught SIGUSR1, skipping to next track...");
            break;
        }
        if (signals::TakeHup()) HandleHup();

        // 1. Read input (bounded look-ahead when decoding).
        std::span<const std::uint8_t> raw;
        if (!input_done && (!decoder || decoder->WantsInput())) {
            auto n = src.Read(buf);
            if (!n) {
                error = n.error();
                return Result::kError;
            }
            if (*n == 0) {
                log::Debug("Done sending");
                input_done = true;
                if (decoder) decoder->EndOfStream();
            } else {
                raw = std::span(buf).first(*n);
                if (decoder) decoder->Push(raw);
            }
        }

        // 2. Decode, crossfade and re-encode.
        if (decoder && !decoder_done && MaxEncoderLead() < kMaxEncoderLead) {
            if (auto r = decoder->PollBus(); !r) {
                error = r.error();
                return Result::kError;
            }
            pcm.clear();
            Milliseconds timeout = raw.empty() ? kDecoderWait : kNoWait;
            while (pcm.size() < kMaxPcmPerIteration) {
                const Decoder::Status st = decoder->Pull(pcm, timeout);
                timeout = kNoWait;
                if (st == Decoder::Status::kEos) decoder_done = true;
                if (st != Decoder::Status::kData) break;
            }

            if (!pcm.empty()) {
                if (first_pcm) {
                    first_pcm = false;
                    // Tag messages are posted just ahead of the first buffer.
                    if (auto r = decoder->PollBus(); !r) {
                        error = r.error();
                        return Result::kError;
                    }
                    if (pcm_rate_ == 0) pcm_rate_ = decoder->rate();
                    if (decoder->bitrate_kbps() && !src.bitrate()) {
                        src.set_bitrate(decoder->bitrate_kbps());
                    }
                    if (crossfader_) {
                        if (!crossfader_->ready()) crossfader_->SetRate(pcm_rate_);
                        std::optional<double> secs = decoder->QueryDurationSeconds();
                        if (!secs && src.filesize() && src.bitrate()) {
                            secs = static_cast<double>(src.filesize()) / (src.bitrate() * 128.0);
                        }
                        crossfader_->NewTrack(secs);
                    }
                    src.MergeTags(decoder->tags());
                    if (!title_known) {
                        song_ = ComposeSongTitle(playlist_title, src.tags(), src.path());
                        UpdateMetadata();
                        title_known = true;
                    }
                }
                last_progress = Clock::now();
                if (crossfader_) crossfader_->Process(pcm);
                if (auto r = Encode(pcm); !r) {
                    error = r.error();
                    return Result::kError;
                }
            }
        }

        // 3. Collect encoder output and send everything. A stream that is far
        // enough ahead (or finishing the track) waits briefly for its encoder.
        const bool finishing = input_done && decoder_done;
        for (Output& out : outputs_) {
            if (out.mode != Output::Mode::kEncode || !out.encoder) continue;
            if (auto r = out.encoder->PollBus(); !r) {
                out.encoder.reset();
                error = r.error();
                return Result::kError;
            }
            const double lead = EncoderLead(out);
            const bool wait = lead >= kMaxEncoderLead || (finishing && lead > kTailSlack);
            const std::size_t before = out.pending.size();
            out.encoder->PullAvailable(out.pending, wait ? kEncoderWait : kNoWait);
            if (out.pending.size() != before) last_progress = Clock::now();
        }
        if (Result r = SendPending(raw); r != Result::kOk) return r;

        const Clock::time_point now = Clock::now();
        // Reading from a FIFO may legitimately block for a long time, but once
        // the input is exhausted (or encoders are waiting) progress must follow.
        if ((input_done || MaxEncoderLead() >= kMaxEncoderLead) && !finishing && now - last_progress > kStallTimeout) {
            error = "decoder/encoder stalled";
            return Result::kError;
        }
        if (now - last_cue >= kCueInterval) {
            cue_.Update(src, src.tags());
            last_cue = now;
        }

        if (finishing) {
            if (MaxEncoderLead() <= kTailSlack) break;
            if (!tail_deadline) tail_deadline = now + kTailTimeout;
            if (now >= *tail_deadline) {
                log::Debug("Encoder did not catch up at end of track, continuing");
                break;
            }
        }
    }

    if (!title_known) {
        if (decoder) src.MergeTags(decoder->tags());
        song_ = ComposeSongTitle(playlist_title, src.tags(), src.path());
        UpdateMetadata();
    }
    return Result::kOk;
}

}  // namespace ices
