// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/crossfade.h"

#include <algorithm>

#include "src/log.h"

namespace gstradio {

void Crossfader::SetRate(int rate) {
    fade_frames_ = static_cast<std::size_t>(seconds_) * static_cast<std::size_t>(rate);
    ring_.assign(fade_frames_ * 2, 0.0f);
    fpos_ = flen_ = new_track_ = 0;
    log::Debug("Crossfading {} seconds between tracks", seconds_);
}

void Crossfader::NewTrack(std::optional<double> track_seconds) {
    if (skip_next_) {
        skip_next_ = false;
        return;
    }
    if (track_seconds) {
        const int secs = static_cast<int>(*track_seconds);
        if (secs < 10 || secs <= seconds_ * 2) {
            log::Debug("crossfade: not fading short track of {} secs", secs);
            skip_next_ = true;
            return;
        }
    }
    new_track_ = fade_frames_;
}

void Crossfader::Process(std::vector<float>& io) {
    if (!fade_frames_) return;
    const std::size_t n = fade_frames_;
    std::size_t ilen = io.size() / 2;
    std::size_t i = 0;  // output frame index
    std::size_t j = 0;  // input frame index

    // Only fade if the hold-back buffer has been filled completely.
    if (flen_ < n) new_track_ = 0;

    // Fade the held tail of the previous track into the new one.
    while (ilen && new_track_ > 0) {
        const float weight = static_cast<float>(new_track_) / static_cast<float>(n);
        for (int c = 0; c < 2; ++c) {
            io[i * 2 + c] = ring_[fpos_ * 2 + c] * weight + io[i * 2 + c] * (1.0f - weight);
        }
        ++i;
        fpos_ = (fpos_ + 1) % n;
        --ilen;
        if (--new_track_ == 0) flen_ = 0;
    }
    j = i;

    // Refill the hold-back buffer (these samples are not output yet).
    while (ilen && flen_ < n) {
        std::size_t clen = std::min({ilen, n - flen_, n - fpos_});
        std::copy_n(io.begin() + static_cast<std::ptrdiff_t>(j * 2), clen * 2,
                    ring_.begin() + static_cast<std::ptrdiff_t>(fpos_ * 2));
        fpos_ = (fpos_ + clen) % n;
        j += clen;
        flen_ += clen;
        ilen -= clen;
    }

    // Steady state: output the oldest held frame, hold the newest input frame.
    while (ilen) {
        for (int c = 0; c < 2; ++c) {
            const float in = io[j * 2 + c];  // read first: i may equal j
            io[i * 2 + c] = ring_[fpos_ * 2 + c];
            ring_[fpos_ * 2 + c] = in;
        }
        fpos_ = (fpos_ + 1) % n;
        ++i;
        ++j;
        --ilen;
    }

    io.resize(i * 2);
}

}  // namespace gstradio
