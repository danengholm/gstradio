// MPEG audio frame header parsing (port of IceS 0.4 mp3.c).
// Reference: http://mpgedit.org/mpgedit/mpeg_format/mpeghdr.htm
#pragma once

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

  [[nodiscard]] bool same_stream(const Header& o) const {
    return version == o.version && layer == o.layer && samplerate == o.samplerate;
  }
};

// Parse a 4 byte frame header; nullopt if it is not a valid header.
[[nodiscard]] std::optional<Header> parse_header(std::span<const std::uint8_t> bytes);

// Length in bytes of a frame with this header, or 0 if unknown (free format).
[[nodiscard]] std::size_t frame_length(const Header& h);

// Offset of a Xing/VBRI tag within the first frame, relative to its start.
[[nodiscard]] std::size_t vbr_tag_offset(const Header& h);

[[nodiscard]] std::string_view version_name(const Header& h);
[[nodiscard]] std::string_view layer_name(const Header& h);
[[nodiscard]] std::string_view mode_name(const Header& h);

// ID3v2 tag size (including header and footer) if `bytes` begins with one.
[[nodiscard]] std::optional<std::size_t> id3v2_size(std::span<const std::uint8_t> bytes);

}  // namespace ices::mp3
