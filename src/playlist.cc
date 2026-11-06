// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/playlist.h"

#include <string>

#include "src/build_config.h"
#include "src/log.h"

namespace gsradio {

#if !GSRADIO_PYTHON_ENABLED
std::expected<std::unique_ptr<Playlist>, std::string> MakePythonPlaylist(const PlaylistConfig&) {
    return std::unexpected("This binary has no support for embedded python");
}
#endif

#if !GSRADIO_PERL_ENABLED
std::expected<std::unique_ptr<Playlist>, std::string> MakePerlPlaylist(const PlaylistConfig&) {
    return std::unexpected("This binary has no support for embedded perl");
}
#endif

std::expected<std::unique_ptr<Playlist>, std::string> MakePlaylist(const Config& config) {
    log::Debug("Initializing playlist handler...");
    switch (config.playlist.type) {
        case PlaylistType::kBuiltin:
            return MakeBuiltinPlaylist(config.playlist);
        case PlaylistType::kPython:
            return MakePythonPlaylist(config.playlist);
        case PlaylistType::kPerl:
            return MakePerlPlaylist(config.playlist);
    }
    return std::unexpected("Unknown playlist module!");
}

}  // namespace gsradio
