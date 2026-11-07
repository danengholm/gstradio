// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Crossfader (port of IceS 0.4 crossfade.c).
//
// The last N seconds of output are held back in a ring buffer. When a new
// track starts, the held tail is mixed linearly into the beginning of the new
// track. Tracks shorter than max(10 s, 2 * N) are not faded into, and neither
// is the track following them.
#ifndef GSTRADIO_SRC_CROSSFADE_H_
#define GSTRADIO_SRC_CROSSFADE_H_

#include <cstddef>
#include <optional>
#include <vector>

namespace gstradio {

class Crossfader {
 public:
    explicit Crossfader(int seconds) : seconds_(seconds) {}
    Crossfader(const Crossfader&) = default;
    Crossfader& operator=(const Crossfader&) = default;

    [[nodiscard]] int seconds() const { return seconds_; }
    [[nodiscard]] bool ready() const { return fade_frames_ != 0; }
    // Must be called once with the (fixed) PCM rate before processing.
    void SetRate(int rate);
    // Announce a track change; `track_seconds` is the track length if known.
    void NewTrack(std::optional<double> track_seconds);
    // Process interleaved stereo samples in place; output may be shorter than
    // input while the hold-back buffer fills.
    void Process(std::vector<float>& samples);

 private:
    int seconds_;
    std::size_t fade_frames_ = 0;
    std::vector<float> ring_;  // stereo frames
    std::size_t fpos_ = 0;     // ring position (frames)
    std::size_t flen_ = 0;     // frames held
    std::size_t new_track_ = 0;
    bool skip_next_ = false;
};

}  // namespace gstradio

#endif  // GSTRADIO_SRC_CROSSFADE_H_
