// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Playlist module interface (IceS 0.4 playlist/playlist.c).
#ifndef GSTRADIO_SRC_PLAYLIST_H_
#define GSTRADIO_SRC_PLAYLIST_H_

#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "src/config.h"

namespace gstradio {

class Playlist {
 public:
    Playlist() = default;
    Playlist(const Playlist&) = delete;
    Playlist& operator=(const Playlist&) = delete;
    virtual ~Playlist() = default;

    // Path of the next file to play; nullopt or empty ends streaming.
    virtual std::optional<std::string> Next() = 0;
    // Title override for the current track (nullopt: build one from tags).
    virtual std::optional<std::string> Metadata() { return std::nullopt; }
    // Current playlist line, for the cue file.
    virtual int Lineno() { return 0; }
    // Reload the module (SIGHUP).
    virtual void Reload() {}
    virtual void Shutdown() {}
};

[[nodiscard]] std::expected<std::unique_ptr<Playlist>, std::string> MakePlaylist(const Config& config);

std::expected<std::unique_ptr<Playlist>, std::string> MakeBuiltinPlaylist(const PlaylistConfig& config);
std::expected<std::unique_ptr<Playlist>, std::string> MakePythonPlaylist(const PlaylistConfig& config);
std::expected<std::unique_ptr<Playlist>, std::string> MakePerlPlaylist(const PlaylistConfig& config);

}  // namespace gstradio

#endif  // GSTRADIO_SRC_PLAYLIST_H_
