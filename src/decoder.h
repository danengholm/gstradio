// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// GStreamer decoding pipeline for one track:
//
//   appsrc ! decodebin ! audioconvert ! audioresample
//          ! audio/x-raw,format=F32,layout=interleaved,channels=2[,rate=R]
//          ! appsink
//
// Compressed bytes are pushed in by the streaming loop; interleaved stereo
// float PCM is pulled out. Handles anything GStreamer can decode (Ogg Vorbis,
// FLAC, MP3, ...).
#ifndef GSRADIO_SRC_DECODER_H_
#define GSRADIO_SRC_DECODER_H_

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

#include "src/gst_util.h"
#include "src/source.h"

namespace gsradio {

class Decoder {
 public:
    // `rate` of 0 keeps the track's native sample rate.
    static std::expected<std::unique_ptr<Decoder>, std::string> Create(int rate, std::uint64_t size_hint);
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    ~Decoder() = default;

    // Whether the decoder wants more input right now (bounded look-ahead).
    [[nodiscard]] bool WantsInput() const;
    void Push(std::span<const std::uint8_t> data);
    void EndOfStream();

    enum class Status { kData, kAgain, kEos };
    // Pull decoded PCM (appended to `out`). Blocks for at most `timeout`.
    Status Pull(std::vector<float>& out, std::chrono::milliseconds timeout);

    // Process pending bus messages; returns an error description on failure.
    std::expected<void, std::string> PollBus();

    [[nodiscard]] int rate() const { return rate_; }
    [[nodiscard]] const TrackTags& tags() const { return tags_; }
    [[nodiscard]] int bitrate_kbps() const { return bitrate_kbps_; }
    [[nodiscard]] std::optional<double> QueryDurationSeconds() const;

 private:
    Decoder() = default;
    void HandleTags(GstTagList* list);

    gst::PipelinePtr pipeline_;
    GstAppSrc* src_ = nullptr;    // owned by pipeline
    GstAppSink* sink_ = nullptr;  // owned by pipeline
    gst::ObjectPtr<GstBus> bus_;
    int rate_ = 0;
    TrackTags tags_;
    int bitrate_kbps_ = 0;
};

}  // namespace gsradio

#endif  // GSRADIO_SRC_DECODER_H_
