// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// MPEG audio frame header parsing (port of IceS 0.4 mp3.c).
// Reference: http://mpgedit.org/mpgedit/mpeg_format/mpeghdr.htm
#ifndef GSRADIO_SRC_MP3_H_
#define GSRADIO_SRC_MP3_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ices::mp3 {

struct Header {
    int version = 0;  // 0 = MPEG-1, 1 = MPEG-2 LSF, 2 = MPEG-2.5
    int layer = 0;    // 1..3
    bool error_protection = false;
    int bitrate = 0;  // kbps, 0 = free format
    int samplerate = 0;
    bool padding = false;
    bool extension = false;
    int mode = 0;  // 0 stereo, 1 joint, 2 dual, 3 mono
    int mode_ext = 0;
    bool copyright = false;
    bool original = false;
    int emphasis = 0;
    int channels = 2;
};

// Whether two frame headers belong to the same stream (same MPEG version,
// layer and sample rate).
bool SameStream(const Header& a, const Header& b);

// Parse a 4 byte frame header; nullopt if it is not a valid header.
[[nodiscard]] std::optional<Header> ParseHeader(std::span<const std::uint8_t> bytes);

// Length in bytes of a frame with this header, or 0 if unknown (free format).
[[nodiscard]] std::size_t FrameLength(const Header& h);

// Offset of a Xing/VBRI tag within the first frame, relative to its start.
[[nodiscard]] std::size_t VbrTagOffset(const Header& h);

[[nodiscard]] std::string_view VersionName(const Header& h);
[[nodiscard]] std::string_view LayerName(const Header& h);
[[nodiscard]] std::string_view ModeName(const Header& h);

// ID3v2 tag size (including header and footer) if `bytes` begins with one.
[[nodiscard]] std::optional<std::size_t> Id3v2Size(std::span<const std::uint8_t> bytes);

}  // namespace ices::mp3

#endif  // GSRADIO_SRC_MP3_H_
