// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// The cue file (<basedir>/gsradio.cue), rewritten continuously while playing.
// Format (one item per line, as in IceS 0.4):
//   filename
//   size
//   bitrate (kbps)
//   days:hours:minutes:seconds  (track length estimated from size/bitrate)
//   percent played
//   current line in playlist
//   artist
//   title
#ifndef GSRADIO_SRC_CUE_H_
#define GSRADIO_SRC_CUE_H_

#include <filesystem>
#include <utility>

#include "src/source.h"

namespace gsradio {

class CueFile {
 public:
    explicit CueFile(std::filesystem::path base_directory) : path_(std::move(base_directory) / "gsradio.cue") {}
    CueFile(const CueFile&) = default;
    CueFile& operator=(const CueFile&) = default;

    void set_lineno(int lineno) { lineno_ = lineno; }
    void Update(const InputSource& source, const TrackTags& tags);
    void Remove();

 private:
    std::filesystem::path path_;
    int lineno_ = 0;
    bool warned_ = false;
};

}  // namespace gsradio

#endif  // GSRADIO_SRC_CUE_H_
