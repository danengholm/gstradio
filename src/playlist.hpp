// Playlist module interface (IceS 0.4 playlist/playlist.c).
#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "config.hpp"

namespace ices {

class Playlist {
 public:
  virtual ~Playlist() = default;
  // Path of the next file to play; nullopt or empty ends streaming.
  virtual std::optional<std::string> next() = 0;
  // Title override for the current track (nullopt: build one from tags).
  virtual std::optional<std::string> metadata() { return std::nullopt; }
  // Current playlist line, for the cue file.
  virtual int lineno() { return 0; }
  // Reload the module (SIGHUP).
  virtual void reload() {}
  virtual void shutdown() {}
};

[[nodiscard]] std::expected<std::unique_ptr<Playlist>, std::string> make_playlist(
    const Config& config);

std::expected<std::unique_ptr<Playlist>, std::string> make_builtin_playlist(
    const PlaylistConfig& config);
std::expected<std::unique_ptr<Playlist>, std::string> make_python_playlist(
    const PlaylistConfig& config);
std::expected<std::unique_ptr<Playlist>, std::string> make_perl_playlist(
    const PlaylistConfig& config);

}  // namespace ices
