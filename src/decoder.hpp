// GStreamer decoding pipeline for one track:
//
//   appsrc ! decodebin ! audioconvert ! audioresample
//          ! audio/x-raw,format=F32,layout=interleaved,channels=2[,rate=R]
//          ! appsink
//
// Compressed bytes are pushed in by the streaming loop; interleaved stereo
// float PCM is pulled out. Handles anything GStreamer can decode (Ogg Vorbis,
// FLAC, MP3, ...).
#pragma once

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gst_util.hpp"
#include "source.hpp"

namespace ices {

class Decoder {
 public:
  // `rate` of 0 keeps the track's native sample rate.
  static std::expected<std::unique_ptr<Decoder>, std::string> create(int rate,
                                                                     std::uint64_t size_hint);
  ~Decoder() = default;

  // Whether the decoder wants more input right now (bounded look-ahead).
  [[nodiscard]] bool wants_input() const;
  void push(std::span<const std::uint8_t> data);
  void end_of_stream();

  enum class Status { data, again, eos };
  // Pull decoded PCM (appended to `out`). Blocks for at most `timeout`.
  Status pull(std::vector<float>& out, std::chrono::milliseconds timeout);

  // Process pending bus messages; returns an error description on failure.
  std::expected<void, std::string> poll_bus();

  [[nodiscard]] int rate() const { return rate_; }
  [[nodiscard]] const TrackTags& tags() const { return tags_; }
  [[nodiscard]] int bitrate_kbps() const { return bitrate_kbps_; }
  [[nodiscard]] std::optional<double> duration_seconds() const;

 private:
  Decoder() = default;
  void handle_tags(GstTagList* list);

  gst::PipelinePtr pipeline_;
  GstAppSrc* src_ = nullptr;   // owned by pipeline
  GstAppSink* sink_ = nullptr; // owned by pipeline
  gst::ObjectPtr<GstBus> bus_;
  int rate_ = 0;
  TrackTags tags_;
  int bitrate_kbps_ = 0;
};

}  // namespace ices
