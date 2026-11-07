// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/mp3.h"

#include <array>

namespace gstradio::mp3 {
namespace {

constexpr int kModeMono = 3;

// [MPEG-1 | MPEG-2/2.5][layer-1][index]
constexpr std::array<std::array<std::array<int, 15>, 3>, 2> kBitrates{{
    {{
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
    }},
    {{
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    }},
}};

constexpr std::array<std::array<int, 4>, 3> kSampleRates{{
    {44100, 48000, 32000, 0},
    {22050, 24000, 16000, 0},
    {11025, 8000, 8000, 0},
}};

constexpr std::array<std::string_view, 5> kModeNames{"stereo", "j-stereo", "dual-ch", "mono", "multi-ch"};
constexpr std::array<std::string_view, 3> kLayerNames{"I", "II", "III"};
constexpr std::array<std::string_view, 3> kVersionNames{"MPEG-1", "MPEG-2 LSF", "MPEG-2.5"};

}  // namespace

std::optional<Header> ParseHeader(std::span<const std::uint8_t> buf) {
    if (buf.size() < 4) return std::nullopt;
    if (((buf[0] << 4) | ((buf[1] >> 4) & 0xE)) != 0xFFE) return std::nullopt;

    Header h;
    switch ((buf[1] >> 3) & 0x3) {
        case 3:
            h.version = 0;
            break;
        case 2:
            h.version = 1;
            break;
        case 0:
            h.version = 2;
            break;
        default:
            return std::nullopt;
    }

    const int bitrate_idx = (buf[2] >> 4) & 0xF;
    const int samplerate_idx = (buf[2] >> 2) & 0x3;
    h.mode = (buf[3] >> 6) & 0x3;
    h.layer = 4 - ((buf[1] >> 1) & 0x3);
    h.emphasis = buf[3] & 0x3;

    if (bitrate_idx == 0xF || samplerate_idx == 0x3 || h.layer == 4 || h.emphasis == 2) {
        return std::nullopt;
    }

    h.error_protection = !(buf[1] & 0x1);
    h.bitrate = kBitrates[h.version == 0 ? 0 : 1][h.layer - 1][bitrate_idx];
    h.samplerate = kSampleRates[h.version][samplerate_idx];
    h.padding = (buf[2] >> 1) & 0x1;
    h.extension = buf[2] & 0x1;
    h.mode_ext = (buf[3] >> 4) & 0x3;
    h.copyright = (buf[3] >> 3) & 0x1;
    h.original = (buf[3] >> 2) & 0x1;
    h.channels = h.mode == kModeMono ? 1 : 2;
    return h;
}

bool SameStream(const Header& a, const Header& b) {
    return a.version == b.version && a.layer == b.layer && a.samplerate == b.samplerate;
}

std::size_t FrameLength(const Header& h) {
    if (h.bitrate == 0 || h.samplerate == 0) return 0;
    const int pad = h.padding ? 1 : 0;
    if (h.layer == 1) {
        return static_cast<std::size_t>((12000 * h.bitrate / h.samplerate + pad) * 4);
    }
    if (h.layer == 3 && h.version > 0) {
        return static_cast<std::size_t>(72000 * h.bitrate / h.samplerate + pad);
    }
    return static_cast<std::size_t>(144000 * h.bitrate / h.samplerate + pad);
}

std::size_t VbrTagOffset(const Header& h) {
    if (h.version == 0) return h.channels == 1 ? 21 : 36;
    return h.channels == 1 ? 13 : 21;
}

std::string_view VersionName(const Header& h) { return kVersionNames.at(h.version); }
std::string_view LayerName(const Header& h) { return kLayerNames.at(h.layer - 1); }
std::string_view ModeName(const Header& h) { return kModeNames.at(h.mode); }

std::optional<std::size_t> Id3v2Size(std::span<const std::uint8_t> b) {
    if (b.size() < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') {
        return std::nullopt;
    }
    if (b[3] == 0xFF || b[4] == 0xFF) return std::nullopt;
    for (int i = 6; i < 10; ++i)
        if (b[i] & 0x80) return std::nullopt;  // size must be synchsafe
    std::size_t size =
        (std::size_t{b[6]} << 21) | (std::size_t{b[7]} << 14) | (std::size_t{b[8]} << 7) | std::size_t{b[9]};
    size += 10;
    if (b[3] >= 4 && (b[5] & 0x10)) size += 10;  // footer present
    return size;
}

}  // namespace gstradio::mp3
