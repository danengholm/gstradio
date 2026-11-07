// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

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
#ifndef GSTRADIO_SRC_STREAMER_H_
#define GSTRADIO_SRC_STREAMER_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "src/config.h"
#include "src/crossfade.h"
#include "src/cue.h"
#include "src/encoder.h"
#include "src/metadata.h"
#include "src/playlist.h"
#include "src/shout_stream.h"
#include "src/source.h"

namespace gstradio {

class Streamer {
 public:
    static std::expected<std::unique_ptr<Streamer>, std::string> Create(const Config& config, Playlist& playlist);

    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;
    ~Streamer();

    // Streams until the playlist ends, a fatal error occurs or a shutdown is
    // requested. Returns the process exit status.
    int Run();

 private:
    enum class Result { kOk, kError, kQuit, kFatal };

    // Per-stream state: the connection plus how the current track is sent.
    struct Output {
        enum class Mode { kRaw, kEncode };

        std::unique_ptr<ShoutStream> stream;
        StreamConfig config;  // Effective settings; sample rate resolved lazily.
        Mode mode = Mode::kRaw;
        std::unique_ptr<Encoder> encoder;
        std::vector<std::uint8_t> pending;  // Data waiting to be sent.
        int encode_bitrate = 0;
        // Pacing of the asynchronous encoder: audio pushed in vs. data consumed
        // (sent or dropped) on the way out.
        double pushed_seconds = 0;
        std::uint64_t consumed_bytes = 0;
    };

    Streamer(const Config& config, Playlist& playlist);

    // Seconds of audio an output's encoder holds beyond what has gone out.
    static double EncoderLead(const Output& out);
    static bool NeedsReencoding(const InputSource& source, const Output& out);

    Result Play(InputSource& source, std::string& error);
    void ChooseModes(const InputSource& source);
    std::expected<void, std::string> EnsureEncoder(Output& out);
    std::expected<void, std::string> Encode(std::span<const float> pcm);
    Result SendPending(std::span<const std::uint8_t> raw);
    Result DrainEncoders(bool only_leaving_encode_mode);
    double MaxEncoderLead() const;
    void UpdateMetadata();
    void HandleHup();

    const Config& config_;
    Playlist& playlist_;
    std::vector<Output> outputs_;
    MetadataUpdater metadata_;
    CueFile cue_;
    std::optional<Crossfader> crossfader_;
    int pcm_rate_ = 0;  // Decode rate shared by all encoders, fixed once known.
    std::string song_;  // Current title.
};

}  // namespace gstradio

#endif  // GSTRADIO_SRC_STREAMER_H_
