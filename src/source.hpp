// An input track: a file, FIFO or stdin ("-").
//
// Bytes are read by us (not by GStreamer) so that the very same chunks can be
// relayed verbatim to pass-through streams and pushed into the decoder for
// re-encoded streams, as IceS 0.4 did.
//
// MP3 input is analysed the way IceS 0.4 did it: ID3v2/v1 tags are parsed
// and excluded from the stream, garbage before the first frame is skipped,
// trailing junk and truncated frames are trimmed, and bitrate / sample rate /
// channels are extracted so that lazy re-encoding can be decided.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ices {

struct TrackTags {
  std::string artist;
  std::string title;
};

enum class InputType { mp3, other };

class InputSource {
 public:
  static std::expected<std::unique_ptr<InputSource>, std::string> open(const std::string& path);
  ~InputSource();
  InputSource(const InputSource&) = delete;
  InputSource& operator=(const InputSource&) = delete;

  // Read the next chunk of stream data. Returns 0 at end of input.
  std::expected<std::size_t, std::string> read(std::span<std::uint8_t> out);

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] InputType type() const { return type_; }
  [[nodiscard]] bool is_regular_file() const { return regular_; }
  // Size of the streamable part of the file (0 if unknown, e.g. a FIFO).
  [[nodiscard]] std::uint64_t filesize() const { return filesize_; }
  // Current position in the file.
  [[nodiscard]] std::uint64_t bytes_read() const { return file_offset_ - (buf_.size() - pos_); }
  // kbps; 0 means VBR or unknown.
  [[nodiscard]] int bitrate() const { return bitrate_; }
  [[nodiscard]] int samplerate() const { return samplerate_; }
  [[nodiscard]] int channels() const { return channels_; }
  [[nodiscard]] const TrackTags& tags() const { return tags_; }

  // Information learned while decoding non-MP3 input.
  void set_bitrate(int kbps) { bitrate_ = kbps; }
  void merge_tags(const TrackTags& t);

 private:
  InputSource() = default;
  // Make sure at least `n` unread bytes are buffered. False on EOF/error.
  bool fill(std::size_t n);
  void parse_id3v1();
  void parse_id3v2(std::size_t size);
  bool find_mp3_sync();
  void trim_mp3_end(const struct Mp3Info& info);

  std::string path_;
  int fd_ = -1;
  bool owns_fd_ = false;
  bool regular_ = false;
  InputType type_ = InputType::other;
  std::uint64_t filesize_ = 0;
  std::uint64_t file_offset_ = 0;  // bytes consumed from fd
  std::vector<std::uint8_t> buf_;  // look-ahead buffer
  std::size_t pos_ = 0;            // read position within buf_
  int bitrate_ = 0;
  int samplerate_ = 0;
  int channels_ = 2;
  TrackTags tags_;
};

}  // namespace ices
