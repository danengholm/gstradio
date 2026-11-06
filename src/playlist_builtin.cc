// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Builtin playlist: a text file with one path per line.
//
// As in IceS 0.4 the file is re-read when it changes on disk (in sequential
// mode the current position is kept), and it wraps around at the end.
// Improvements over the original: blank lines and '#' comment lines (e.g.
// M3U headers) are skipped instead of terminating playback, randomized
// playlists are reshuffled on every pass, and SIGHUP re-reads the file.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "src/log.h"
#include "src/playlist.h"

namespace gsradio {
namespace {

class BuiltinPlaylist final : public Playlist {
 public:
    explicit BuiltinPlaylist(PlaylistConfig config) : config_(std::move(config)), rng_(std::random_device()()) {}

    std::expected<void, std::string> Load() {
        if (config_.file.empty()) {
            return std::unexpected("Playlist file is not set!");
        }
        std::ifstream in(config_.file);
        if (!in) {
            return std::unexpected(std::format("Could not open playlist file: {}", config_.file));
        }

        std::vector<Entry> entries;
        std::string line;
        for (int lineno = 1; std::getline(in, line); ++lineno) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#') continue;
            entries.push_back({std::move(line), lineno});
        }
        entries_ = std::move(entries);

        std::error_code ec;
        mtime_ = std::filesystem::last_write_time(config_.file, ec);
        if (config_.randomize) {
            log::Debug("Randomizing playlist...");
            std::ranges::shuffle(entries_, rng_);
        }
        return {};
    }

    std::optional<std::string> Next() override {
        // If the playlist changed on disk (and we are not randomising) reload it,
        // keeping our position.
        if (!config_.randomize) {
            std::error_code ec;
            auto mtime = std::filesystem::last_write_time(config_.file, ec);
            if (!ec && mtime != mtime_) {
                log::Debug("Reopening playlist file");
                if (auto r = Load(); !r) log::Info("{}", r.error());
                if (index_ > entries_.size()) {
                    log::Debug("Reached end of playlist, rewinding");
                    index_ = 0;
                }
            }
        }

        if (index_ >= entries_.size()) {
            if (!entries_.empty()) log::Debug("Reached end of playlist, rewinding");
            index_ = 0;
            if (config_.randomize || entries_.empty()) {
                if (auto r = Load(); !r) log::Info("{}", r.error());
            }
        }
        if (entries_.empty()) {
            log::Info("Unreadable or empty playlist");
            return std::nullopt;
        }

        const Entry& e = entries_[index_++];
        lineno_ = e.lineno;
        log::Debug("Builtin playlist handler serving: {}", e.path);
        return e.path;
    }

    int Lineno() override { return lineno_; }

    void Reload() override {
        log::Debug("Reloading playlist file");
        if (auto r = Load(); !r) {
            log::Info("{}", r.error());
            return;
        }
        if (config_.randomize || index_ > entries_.size()) index_ = 0;
    }

 private:
    struct Entry {
        std::string path;
        int lineno;
    };

    PlaylistConfig config_;
    std::vector<Entry> entries_;
    std::size_t index_ = 0;
    int lineno_ = 0;
    std::filesystem::file_time_type mtime_{};
    std::mt19937 rng_;
};

}  // namespace

std::expected<std::unique_ptr<Playlist>, std::string> MakeBuiltinPlaylist(const PlaylistConfig& config) {
    log::Debug("Initializing builtin playlist handler...");
    auto pl = std::make_unique<BuiltinPlaylist>(config);
    if (auto r = pl->Load(); !r) {
        log::Info("Could not find a valid playlist file.");
        return std::unexpected(r.error());
    }
    return pl;
}

}  // namespace gsradio
