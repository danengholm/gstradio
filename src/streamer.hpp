// The streaming loop (IceS 0.4 stream.c).
//
// For every playlist entry the input is opened and, per stream, one of two
// paths is chosen:
//   raw    - the file's MP3 data is relayed unchanged (no re-encoding, or the
//            file already matches the stream's bitrate/sample rate/channels)
//   encode - the file is decoded by GStreamer and re-encoded with LAME at the
//            stream's settings (always the case for Ogg Vorbis and FLAC)
// Chunks are read once and fanned out to every stream; libshout paces the
// output to real time.
#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "config.hpp"
#include "crossfade.hpp"
#include "cue.hpp"
#include "metadata.hpp"
#include "playlist.hpp"
#include "shout_stream.hpp"
#include "source.hpp"

namespace ices {

class Streamer {
 public:
  Streamer(const Config& config, Playlist& playlist);
  ~Streamer();

  // Stream until the playlist ends, a fatal error occurs or a shutdown is
  // requested. Returns the process exit status.
  int run();

 private:
  enum class Result { ok, error, quit, fatal };

  Result play(InputSource& source, std::string& error);
  void choose_modes(const InputSource& source);
  [[nodiscard]] bool needs_reencoding(const InputSource& source, const ShoutStream& stream) const;
  std::expected<void, std::string> ensure_encoder(ShoutStream& stream);
  std::expected<void, std::string> encode(std::span<const float> pcm);
  Result send_pending(std::span<const std::uint8_t> raw);
  Result drain_encoders(bool only_leaving_encode_mode);
  void update_metadata();
  void handle_hup();

  const Config& config_;
  Playlist& playlist_;
  std::vector<std::unique_ptr<ShoutStream>> streams_;
  MetadataUpdater metadata_;
  CueFile cue_;
  std::optional<Crossfader> crossfader_;
  int pcm_rate_ = 0;  // decode rate shared by all encoders, fixed once known
  std::string song_;  // current title
};

}  // namespace ices
