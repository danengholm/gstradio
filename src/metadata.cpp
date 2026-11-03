#include "metadata.hpp"

#include <shout/shout.h>

#include <algorithm>
#include <memory>

#include "log.hpp"
#include "shout_stream.hpp"

namespace ices {

std::string clean_filename(std::string_view path) {
  if (auto slash = path.rfind('/'); slash != std::string_view::npos) path.remove_prefix(slash + 1);
  if (auto dot = path.rfind('.'); dot != std::string_view::npos) path = path.substr(0, dot);
  return std::string(path);
}

std::string compose_song_title(const std::optional<std::string>& playlist_metadata,
                               const TrackTags& tags, std::string_view path) {
  if (playlist_metadata && !playlist_metadata->empty()) return *playlist_metadata;
  if (!tags.title.empty())
    return tags.artist.empty() ? tags.title : std::format("{} - {}", tags.artist, tags.title);
  return clean_filename(path);
}

MetadataUpdater::MetadataUpdater() : worker_([this](std::stop_token st) { run(st); }) {}

MetadataUpdater::~MetadataUpdater() {
  worker_.request_stop();
  cv_.notify_all();
}

void MetadataUpdater::update(const StreamConfig& stream, std::string song,
                             std::chrono::milliseconds delay) {
  if (delay.count()) log::debug("Delaying metadata update...");
  {
    std::scoped_lock lock(mutex_);
    // A newer title for the same mount supersedes any queued one.
    std::erase_if(jobs_, [&](const Job& j) {
      return j.stream.host == stream.host && j.stream.port == stream.port &&
             j.stream.mount == stream.mount;
    });
    jobs_.push_back({stream, std::move(song), std::chrono::steady_clock::now() + delay});
    ++generation_;
  }
  cv_.notify_all();
}

void MetadataUpdater::run(std::stop_token stop) {
  struct ShoutFree {
    void operator()(shout_t* s) const { shout_free(s); }
  };
  struct MetaFree {
    void operator()(shout_metadata_t* m) const { shout_metadata_free(m); }
  };

  while (!stop.stop_requested()) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      if (!cv_.wait(lock, stop, [&] { return !jobs_.empty(); })) return;
      // Wait until the earliest job is due (or something changes).
      auto due = std::ranges::min_element(jobs_, {}, &Job::not_before);
      if (due->not_before > std::chrono::steady_clock::now()) {
        // Re-evaluate early if a new job arrives in the meantime.
        const auto gen = generation_;
        cv_.wait_until(lock, stop, due->not_before, [&] { return generation_ != gen; });
        continue;
      }
      job = std::move(*due);
      jobs_.erase(due);
    }

    // A separate (unconnected) handle: metadata goes over its own request,
    // so this never touches the streaming connection.
    std::unique_ptr<shout_t, ShoutFree> shout{shout_new()};
    std::unique_ptr<shout_metadata_t, MetaFree> md{shout_metadata_new()};
    if (!shout || !md) {
      log::debug("Error allocating metadata structure");
      continue;
    }
    configure_shout(shout.get(), job.stream);
    if (shout_metadata_add(md.get(), "song", job.song.c_str()) != SHOUTERR_SUCCESS) {
      log::debug("Error adding info to metadata structure");
      continue;
    }
    if (shout_set_metadata_utf8(shout.get(), md.get()) == SHOUTERR_SUCCESS)
      log::debug("Updated metadata on {} to: {}", job.stream.mount, job.song);
    else
      log::debug("Updating metadata on {} failed: {}", job.stream.mount,
                 shout_get_error(shout.get()));
  }
}

}  // namespace ices
