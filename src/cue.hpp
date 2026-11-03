// The cue file (<basedir>/ices.cue), rewritten continuously while playing.
// Format (one item per line, as in IceS 0.4):
//   filename
//   size
//   bitrate (kbps)
//   days:hours:minutes:seconds  (track length estimated from size/bitrate)
//   percent played
//   current line in playlist
//   artist
//   title
#pragma once

#include <filesystem>

#include "source.hpp"

namespace ices {

class CueFile {
 public:
  explicit CueFile(std::filesystem::path base_directory)
      : path_(std::move(base_directory) / "ices.cue") {}

  void set_lineno(int lineno) { lineno_ = lineno; }
  void update(const InputSource& source, const TrackTags& tags);
  void remove();

 private:
  std::filesystem::path path_;
  int lineno_ = 0;
  bool warned_ = false;
};

}  // namespace ices
