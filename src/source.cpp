#include "source.hpp"

#include <fcntl.h>
#include <gst/tag/tag.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <string_view>

#include "gst_util.hpp"
#include "log.hpp"
#include "mp3.hpp"

namespace ices {

struct Mp3Info {
  mp3::Header header;
};

namespace {

constexpr std::size_t kInitialRead = 4096;
// Give up looking for MPEG sync after this much garbage; the data is then
// handed to GStreamer which may still know what it is.
constexpr std::size_t kMaxSyncSearch = 1 << 20;

bool starts_with(std::span<const std::uint8_t> b, std::string_view magic, std::size_t at = 0) {
  return b.size() >= at + magic.size() && std::memcmp(b.data() + at, magic.data(), magic.size()) == 0;
}

// Containers whose payload must never be mistaken for MPEG frames.
bool is_known_non_mp3(std::span<const std::uint8_t> b) {
  static constexpr std::array<std::string_view, 8> kMagic{
      "OggS", "fLaC", "RIFF", "FORM", "wvpk", "MAC ", "MThd", "\x30\x26\xB2\x75"};
  return std::ranges::any_of(kMagic, [&](auto m) { return starts_with(b, m); }) ||
         starts_with(b, "ftyp", 4);
}

TrackTags tags_from_list(const GstTagList* list) {
  return {gst::tag_string(list, GST_TAG_ARTIST), gst::tag_string(list, GST_TAG_TITLE)};
}

}  // namespace

InputSource::~InputSource() {
  if (owns_fd_ && fd_ >= 0) ::close(fd_);
}

void InputSource::merge_tags(const TrackTags& t) {
  if (!t.artist.empty()) tags_.artist = t.artist;
  if (!t.title.empty()) tags_.title = t.title;
}

bool InputSource::fill(std::size_t n) {
  while (buf_.size() - pos_ < n) {
    // Never read past the streamable end of the file.
    std::size_t want = std::max<std::size_t>(n - (buf_.size() - pos_), kInitialRead);
    if (filesize_ && file_offset_ + want > filesize_) want = filesize_ - file_offset_;
    if (want == 0) return false;
    const std::size_t old = buf_.size();
    buf_.resize(old + want);
    ssize_t r;
    do {
      r = ::read(fd_, buf_.data() + old, want);
    } while (r < 0 && errno == EINTR);
    buf_.resize(old + static_cast<std::size_t>(std::max<ssize_t>(r, 0)));
    if (r <= 0) return false;
    file_offset_ += static_cast<std::uint64_t>(r);
  }
  return true;
}

std::expected<std::unique_ptr<InputSource>, std::string> InputSource::open(const std::string& path) {
  std::unique_ptr<InputSource> src{new InputSource};
  src->path_ = path;

  if (path == "-") {
    log::debug("Reading audio from stdin");
    src->fd_ = STDIN_FILENO;
  } else {
    src->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (src->fd_ < 0) return std::unexpected(std::format("Error opening: {}", std::strerror(errno)));
    src->owns_fd_ = true;
  }

  struct stat st{};
  if (::fstat(src->fd_, &st) == 0 && S_ISREG(st.st_mode)) {
    src->regular_ = true;
    src->filesize_ = static_cast<std::uint64_t>(st.st_size);
  }

  // ID3v1 must be detected before anything is read so the tag (which shrinks
  // the streamable size) can never end up in the look-ahead buffer.
  const std::uint64_t full_size = src->filesize_;
  src->parse_id3v1();

  errno = 0;
  if (!src->fill(1))
    return std::unexpected(
        std::format("Error reading header: {}", errno ? std::strerror(errno) : "empty file"));
  src->fill(kInitialRead);

  // Not MP3 after all: rewind and hand everything to the GStreamer decoder.
  auto treat_as_other = [&](std::string_view why) {
    log::debug("{}: {}, will decode with GStreamer", path, why);
    src->type_ = InputType::other;
    src->filesize_ = full_size;
    src->tags_ = {};
    src->pos_ = 0;
    src->bitrate_ = src->samplerate_ = 0;
    src->channels_ = 2;
    return std::move(src);
  };

  if (is_known_non_mp3(src->buf_)) return treat_as_other("not an MPEG audio file");

  if (auto size = mp3::id3v2_size(src->buf_)) {
    src->parse_id3v2(*size);
    // FLAC and friends occasionally carry an ID3v2 tag too.
    if (src->fill(8) && is_known_non_mp3(std::span(src->buf_).subspan(src->pos_)))
      return treat_as_other("not an MPEG audio file");
  }

  if (!src->find_mp3_sync()) return treat_as_other("couldn't find MPEG synch");
  return src;
}

void InputSource::parse_id3v1() {
  if (!regular_ || filesize_ < 128) return;
  std::array<std::uint8_t, 128> tag{};
  if (::pread(fd_, tag.data(), tag.size(), static_cast<off_t>(filesize_ - 128)) != 128) return;
  if (!starts_with(tag, "TAG")) return;
  filesize_ -= 128;  // don't stream the tag
  gst::TagListPtr list{gst_tag_list_new_from_id3v1(tag.data())};
  tags_ = tags_from_list(list.get());
  log::debug("ID3v1: Title: {}", tags_.title);
  log::debug("ID3v1: Artist: {}", tags_.artist);
}

void InputSource::parse_id3v2(std::size_t size) {
  log::debug("ID3v2: version {}.{}. Tag size is {} bytes.", buf_[3], buf_[4], size);
  if (!fill(size)) {
    log::info("Error reading ID3v2, skipping");
    pos_ = buf_.size();
    return;
  }
  GstBuffer* gbuf = gst_buffer_new_memdup(buf_.data() + pos_, size);
  gst::TagListPtr list{gst_tag_list_from_id3v2_tag(gbuf)};
  gst_buffer_unref(gbuf);
  if (list) {
    // ID3v2 wins over ID3v1, field by field.
    merge_tags(tags_from_list(list.get()));
    log::debug("ID3v2: Artist: {}  Title: {}", tags_.artist, tags_.title);
  }
  pos_ += size;
}

bool InputSource::find_mp3_sync() {
  std::size_t skipped = 0;
  while (skipped < kMaxSyncSearch && fill(4)) {
    const auto here = std::span(buf_).subspan(pos_);
    auto h = mp3::parse_header(here);
    std::size_t len = h ? mp3::frame_length(*h) : 0;
    if (h && len) {
      bool accept = false;
      bitrate_ = h->bitrate;
      const std::size_t tag_off = mp3::vbr_tag_offset(*h);
      if (fill(tag_off + 4) && (starts_with(std::span(buf_).subspan(pos_), "Xing", tag_off) ||
                                starts_with(std::span(buf_).subspan(pos_), "VBRI", tag_off))) {
        log::debug("VBR tag found");
        bitrate_ = 0;
        accept = true;
      } else if (!fill(len + 4)) {
        accept = true;  // single frame at end of file
      } else if (auto next = mp3::parse_header(std::span(buf_).subspan(pos_ + len));
                 next && h->same_stream(*next)) {
        if (h->bitrate != next->bitrate) {
          log::debug("Bit rate of first frame ({}) doesn't match second frame ({}), assuming VBR",
                     h->bitrate, next->bitrate);
          bitrate_ = 0;
        }
        accept = true;
      } else {
        log::debug("Bad frame at offset {}", bytes_read());
      }

      if (accept) {
        if (skipped) log::debug("Skipped {} bytes of garbage before MP3", skipped);
        type_ = InputType::mp3;
        samplerate_ = h->samplerate;
        channels_ = h->channels;
        if (bitrate_)
          log::debug("{} layer {}, {} kbps, {} Hz, {}", mp3::version_name(*h), mp3::layer_name(*h),
                     h->bitrate, h->samplerate, mp3::mode_name(*h));
        else
          log::debug("{} layer {}, VBR, {} Hz, {}", mp3::version_name(*h), mp3::layer_name(*h),
                     h->samplerate, mp3::mode_name(*h));
        trim_mp3_end(Mp3Info{*h});
        return true;
      }
    }
    ++pos_;
    ++skipped;
  }
  return false;
}

// Trim trailing junk and a truncated final frame, which otherwise cause
// audible chirps between songs (IceS 0.3+ behaviour).
void InputSource::trim_mp3_end(const Mp3Info& info) {
  if (!regular_ || !filesize_) return;
  const std::uint64_t cur = file_offset_;
  std::uint64_t end = filesize_;
  std::array<std::uint8_t, 4096> chunk{};

  while (end > cur) {
    const std::uint64_t start = end > cur + chunk.size() ? end - chunk.size() : cur;
    const auto len = static_cast<std::size_t>(end - start);
    if (::pread(fd_, chunk.data(), len, static_cast<off_t>(start)) != static_cast<ssize_t>(len)) {
      log::debug("Error reading MP3 while trimming end");
      return;
    }
    end = start;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(len) - 4; i >= 0; --i) {
      auto match = mp3::parse_header(std::span(chunk).subspan(static_cast<std::size_t>(i)));
      if (!match) continue;
      const std::size_t framelen = mp3::frame_length(*match);
      if (!framelen || !info.header.same_stream(*match) ||
          (bitrate_ && bitrate_ != match->bitrate))
        continue;
      const std::uint64_t frame_start = start + static_cast<std::uint64_t>(i);
      if (frame_start + framelen < filesize_) {
        filesize_ = frame_start + framelen;
        log::debug("Trimmed file to {} bytes", filesize_);
      } else if (frame_start + framelen > filesize_) {
        log::debug("Trimmed short frame ({} bytes missing) at offset {}",
                   frame_start + framelen - filesize_, frame_start);
        filesize_ = frame_start;
      }
      return;
    }
  }
}

std::expected<std::size_t, std::string> InputSource::read(std::span<std::uint8_t> out) {
  // Serve look-ahead data first.
  if (pos_ < buf_.size()) {
    const std::size_t n = std::min(out.size(), buf_.size() - pos_);
    std::memcpy(out.data(), buf_.data() + pos_, n);
    pos_ += n;
    if (pos_ == buf_.size()) {
      buf_.clear();
      pos_ = 0;
    }
    return n;
  }
  std::size_t want = out.size();
  if (filesize_) {
    if (file_offset_ >= filesize_) return 0;
    want = static_cast<std::size_t>(std::min<std::uint64_t>(want, filesize_ - file_offset_));
  }
  ssize_t r;
  do {
    r = ::read(fd_, out.data(), want);
  } while (r < 0 && errno == EINTR);
  if (r < 0) return std::unexpected(std::format("Read error: {}", std::strerror(errno)));
  file_offset_ += static_cast<std::uint64_t>(r);
  return static_cast<std::size_t>(r);
}

}  // namespace ices
