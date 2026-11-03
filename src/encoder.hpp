// Per-stream MP3 encoder:
//
//   appsrc (F32 stereo, decode rate) ! audioconvert ! audioresample
//          ! audio/x-raw,rate=S,channels=N ! lamemp3enc (CBR) ! appsink
//
// The encoder is long-lived: it keeps running across track changes, so
// consecutive re-encoded tracks are joined gaplessly.
#pragma once

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gst_util.hpp"

namespace ices {

// The output sample rate LAME picks by default for a given input rate and
// bitrate (queried from liblame when available).
[[nodiscard]] int lame_default_samplerate(int input_rate, int bitrate_kbps, int channels);

// Snap `bitrate_kbps` to the nearest CBR bitrate valid for `samplerate`.
[[nodiscard]] int valid_cbr_bitrate(int bitrate_kbps, int samplerate);

// Snap a requested output sample rate to one MP3 supports.
[[nodiscard]] int valid_mp3_samplerate(int samplerate);

class Encoder {
 public:
  static std::expected<std::unique_ptr<Encoder>, std::string> create(int in_rate, int out_rate,
                                                                     int out_channels,
                                                                     int bitrate_kbps);
  ~Encoder() = default;

  // Push interleaved stereo float samples at the input rate.
  void push(std::span<const float> samples);
  // Append whatever encoded MP3 data is ready to `out`, waiting up to
  // `wait` for the first buffer.
  void pull_available(std::vector<std::uint8_t>& out,
                      std::chrono::milliseconds wait = std::chrono::milliseconds{0});
  // Flush everything; the encoder cannot be used afterwards.
  void drain(std::vector<std::uint8_t>& out, std::chrono::milliseconds timeout);
  std::expected<void, std::string> poll_bus();

  [[nodiscard]] int in_rate() const { return in_rate_; }

 private:
  Encoder() = default;
  void append_sample(GstSample* sample, std::vector<std::uint8_t>& out);

  gst::PipelinePtr pipeline_;
  GstAppSrc* src_ = nullptr;
  GstAppSink* sink_ = nullptr;
  gst::ObjectPtr<GstBus> bus_;
  int in_rate_ = 0;
  std::uint64_t frames_pushed_ = 0;
};

}  // namespace ices
