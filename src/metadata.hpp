// Title streaming (IceS 0.4 metadata.c).
//
// The song title is, in order of preference: the playlist module's
// ices_get_metadata() result, "Artist - Title" / "Title" from the file's
// tags, or the file name without directory and extension.
//
// Updates are sent from a background thread (the original forked) so a slow
// server never stalls the audio. The first update after a (re)connection is
// delayed by 0.5 s so the server has registered the new source.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "config.hpp"
#include "source.hpp"

namespace ices {

[[nodiscard]] std::string clean_filename(std::string_view path);
[[nodiscard]] std::string compose_song_title(const std::optional<std::string>& playlist_metadata,
                                             const TrackTags& tags, std::string_view path);

class MetadataUpdater {
 public:
  MetadataUpdater();
  ~MetadataUpdater();
  MetadataUpdater(const MetadataUpdater&) = delete;
  MetadataUpdater& operator=(const MetadataUpdater&) = delete;

  void update(const StreamConfig& stream, std::string song,
              std::chrono::milliseconds delay = std::chrono::milliseconds{0});

 private:
  struct Job {
    StreamConfig stream;
    std::string song;
    std::chrono::steady_clock::time_point not_before;
  };
  void run(std::stop_token stop);

  std::mutex mutex_;
  std::condition_variable_any cv_;
  std::deque<Job> jobs_;
  std::uint64_t generation_ = 0;  // bumped whenever jobs_ changes
  std::jthread worker_;  // last: started after the members above exist
};

}  // namespace ices
