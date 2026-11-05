// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/source.h"

#include <fcntl.h>
#include <gst/tag/tag.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "src/gst_util.h"
#include "src/log.h"
#include "src/mp3.h"

namespace ices {

struct Mp3Info {
    mp3::Header header;
};

namespace {

constexpr std::size_t kInitialRead = 4096;
// Give up looking for MPEG sync after this much garbage; the data is then
// handed to GStreamer which may still know what it is.
constexpr std::size_t kMaxSyncSearch = 1 << 20;

bool StartsWith(std::span<const std::uint8_t> b, std::string_view magic, std::size_t at = 0) {
    return b.size() >= at + magic.size() && std::memcmp(b.data() + at, magic.data(), magic.size()) == 0;
}

// Containers whose payload must never be mistaken for MPEG frames.
bool IsKnownNonMp3(std::span<const std::uint8_t> b) {
    static constexpr std::array<std::string_view, 8> kMagic{"OggS", "fLaC", "RIFF", "FORM",
                                                            "wvpk", "MAC ", "MThd", "\x30\x26\xB2\x75"};
    return std::ranges::any_of(kMagic, [&](auto m) { return StartsWith(b, m); }) || StartsWith(b, "ftyp", 4);
}

TrackTags TagsFromList(const GstTagList* list) {
    return {gst::TagString(list, GST_TAG_ARTIST), gst::TagString(list, GST_TAG_TITLE)};
}

}  // namespace

InputSource::~InputSource() {
    if (owns_fd_ && fd_ >= 0) ::close(fd_);
}

void InputSource::MergeTags(const TrackTags& t) {
    if (!t.artist.empty()) tags_.artist = t.artist;
    if (!t.title.empty()) tags_.title = t.title;
}

bool InputSource::Fill(std::size_t n) {
    while (buf_.size() - pos_ < n) {
        // Never read past the streamable end of the file.
        std::size_t want = std::max<std::size_t>(n - (buf_.size() - pos_), kInitialRead);
        if (filesize_ && file_offset_ + want > filesize_) {
            want = filesize_ - file_offset_;
        }
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

std::expected<std::unique_ptr<InputSource>, std::string> InputSource::Open(const std::string& path) {
    std::unique_ptr<InputSource> src{new InputSource};
    src->path_ = path;

    if (path == "-") {
        log::Debug("Reading audio from stdin");
        src->fd_ = STDIN_FILENO;
    } else {
        src->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (src->fd_ < 0) {
            return std::unexpected(std::format("Error opening: {}", std::strerror(errno)));
        }
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
    src->ParseId3v1();

    errno = 0;
    if (!src->Fill(1)) {
        return std::unexpected(std::format("Error reading header: {}", errno ? std::strerror(errno) : "empty file"));
    }
    src->Fill(kInitialRead);

    // Not MP3 after all: rewind and hand everything to the GStreamer decoder.
    auto treat_as_other = [&](std::string_view why) {
        log::Debug("{}: {}, will decode with GStreamer", path, why);
        src->type_ = InputType::kOther;
        src->filesize_ = full_size;
        src->tags_ = {};
        src->pos_ = 0;
        src->bitrate_ = src->samplerate_ = 0;
        src->channels_ = 2;
        return std::move(src);
    };

    if (IsKnownNonMp3(src->buf_)) return treat_as_other("not an MPEG audio file");

    if (auto size = mp3::Id3v2Size(src->buf_)) {
        src->ParseId3v2(*size);
        // FLAC and friends occasionally carry an ID3v2 tag too.
        if (src->Fill(8) && IsKnownNonMp3(std::span(src->buf_).subspan(src->pos_))) {
            return treat_as_other("not an MPEG audio file");
        }
    }

    if (!src->FindMp3Sync()) return treat_as_other("couldn't find MPEG synch");
    return src;
}

void InputSource::ParseId3v1() {
    if (!regular_ || filesize_ < 128) return;
    std::array<std::uint8_t, 128> tag{};
    if (::pread(fd_, tag.data(), tag.size(), static_cast<off_t>(filesize_ - 128)) != 128) {
        return;
    }
    if (!StartsWith(tag, "TAG")) return;
    filesize_ -= 128;  // don't stream the tag
    gst::TagListPtr list{gst_tag_list_new_from_id3v1(tag.data())};
    tags_ = TagsFromList(list.get());
    log::Debug("ID3v1: Title: {}", tags_.title);
    log::Debug("ID3v1: Artist: {}", tags_.artist);
}

void InputSource::ParseId3v2(std::size_t size) {
    log::Debug("ID3v2: version {}.{}. Tag size is {} bytes.", buf_[3], buf_[4], size);
    if (!Fill(size)) {
        log::Info("Error reading ID3v2, skipping");
        pos_ = buf_.size();
        return;
    }
    GstBuffer* gbuf = gst_buffer_new_memdup(buf_.data() + pos_, size);
    gst::TagListPtr list{gst_tag_list_from_id3v2_tag(gbuf)};
    gst_buffer_unref(gbuf);
    if (list) {
        // ID3v2 wins over ID3v1, field by field.
        MergeTags(TagsFromList(list.get()));
        log::Debug("ID3v2: Artist: {}  Title: {}", tags_.artist, tags_.title);
    }
    pos_ += size;
}

bool InputSource::FindMp3Sync() {
    std::size_t skipped = 0;
    while (skipped < kMaxSyncSearch && Fill(4)) {
        const auto here = std::span(buf_).subspan(pos_);
        auto h = mp3::ParseHeader(here);
        std::size_t len = h ? mp3::FrameLength(*h) : 0;
        if (h && len) {
            bool accept = false;
            bitrate_ = h->bitrate;
            const std::size_t tag_off = mp3::VbrTagOffset(*h);
            if (Fill(tag_off + 4) && (StartsWith(std::span(buf_).subspan(pos_), "Xing", tag_off) ||
                                      StartsWith(std::span(buf_).subspan(pos_), "VBRI", tag_off))) {
                log::Debug("VBR tag found");
                bitrate_ = 0;
                accept = true;
            } else if (!Fill(len + 4)) {
                accept = true;  // single frame at end of file
            } else if (auto next = mp3::ParseHeader(std::span(buf_).subspan(pos_ + len));
                       next && mp3::SameStream(*h, *next)) {
                if (h->bitrate != next->bitrate) {
                    log::Debug(
                        "Bit rate of first frame ({}) doesn't match second frame ({}), "
                        "assuming VBR",
                        h->bitrate, next->bitrate);
                    bitrate_ = 0;
                }
                accept = true;
            } else {
                log::Debug("Bad frame at offset {}", bytes_read());
            }

            if (accept) {
                if (skipped) {
                    log::Debug("Skipped {} bytes of garbage before MP3", skipped);
                }
                type_ = InputType::kMp3;
                samplerate_ = h->samplerate;
                channels_ = h->channels;
                if (bitrate_) {
                    log::Debug("{} layer {}, {} kbps, {} Hz, {}", mp3::VersionName(*h), mp3::LayerName(*h), h->bitrate,
                               h->samplerate, mp3::ModeName(*h));
                } else {
                    log::Debug("{} layer {}, VBR, {} Hz, {}", mp3::VersionName(*h), mp3::LayerName(*h), h->samplerate,
                               mp3::ModeName(*h));
                }
                TrimMp3End(Mp3Info{*h});
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
void InputSource::TrimMp3End(const Mp3Info& info) {
    if (!regular_ || !filesize_) return;
    const std::uint64_t cur = file_offset_;
    std::uint64_t end = filesize_;
    std::array<std::uint8_t, 4096> chunk{};

    while (end > cur) {
        const std::uint64_t start = end > cur + chunk.size() ? end - chunk.size() : cur;
        const auto len = static_cast<std::size_t>(end - start);
        if (::pread(fd_, chunk.data(), len, static_cast<off_t>(start)) != static_cast<ssize_t>(len)) {
            log::Debug("Error reading MP3 while trimming end");
            return;
        }
        end = start;
        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(len) - 4; i >= 0; --i) {
            auto match = mp3::ParseHeader(std::span(chunk).subspan(static_cast<std::size_t>(i)));
            if (!match) continue;
            const std::size_t framelen = mp3::FrameLength(*match);
            if (!framelen || !mp3::SameStream(info.header, *match) || (bitrate_ && bitrate_ != match->bitrate)) {
                continue;
            }
            const std::uint64_t frame_start = start + static_cast<std::uint64_t>(i);
            if (frame_start + framelen < filesize_) {
                filesize_ = frame_start + framelen;
                log::Debug("Trimmed file to {} bytes", filesize_);
            } else if (frame_start + framelen > filesize_) {
                log::Debug("Trimmed short frame ({} bytes missing) at offset {}", frame_start + framelen - filesize_,
                           frame_start);
                filesize_ = frame_start;
            }
            return;
        }
    }
}

std::expected<std::size_t, std::string> InputSource::Read(std::span<std::uint8_t> out) {
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
    if (r < 0) {
        return std::unexpected(std::format("Read error: {}", std::strerror(errno)));
    }
    file_offset_ += static_cast<std::uint64_t>(r);
    return static_cast<std::size_t>(r);
}

}  // namespace ices
