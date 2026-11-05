// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Embedded Perl playlist module (IceS 0.4 pm_perl.c).
//
// "use <module>" is evaluated with the playlist module directory and the
// current directory in @INC; the module must end with "1;". Subs are looked up
// in package main: ices_get_next (required), ices_init, ices_shutdown,
// ices_get_metadata, ices_get_lineno, with the legacy ices_perl_* names as
// fallbacks.
#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "src/log.h"
#include "src/perl_glue.h"
#include "src/playlist.h"

namespace ices {
namespace {

class PerlPlaylist final : public Playlist {
 public:
    explicit PerlPlaylist(PlaylistConfig config) : config_(std::move(config)) {}
    ~PerlPlaylist() override { ices_perl_term(); }

    std::expected<void, std::string> Start() {
        log::Debug("Importing perl module: {}", config_.module);
        std::array<char, 1024> err{};
        if (ices_perl_start(config_.module.c_str(), err.data(), err.size()) != 0) {
            return std::unexpected(std::string(err.data()));
        }
        FindHooks();
        if (!get_next_) {
            return std::unexpected("The playlist module must define at least the ices_get_next method");
        }
        return {};
    }

    std::expected<void, std::string> Init() {
        if (auto r = Start(); !r) return r;
        if (!init_) return {};
        auto ret = Call(init_);
        const int rc = ret ? std::atoi(ret->c_str()) : -1;
        if (rc <= 0) log::Info("Execution of 'ices_init' failed");
        if (rc < 0) return std::unexpected("ices_init failed");
        return {};
    }

    std::optional<std::string> Next() override { return Call(get_next_); }

    std::optional<std::string> Metadata() override { return get_metadata_ ? Call(get_metadata_) : std::nullopt; }

    int Lineno() override {
        if (!get_lineno_) return 0;
        auto ret = Call(get_lineno_);
        return ret ? std::atoi(ret->c_str()) : 0;
    }

    void Reload() override {
        // Restart the interpreter so changes to the module take effect.
        Shutdown();
        if (auto r = Init(); !r) {
            log::Info("Playlist module reload failed: {}", r.error());
        } else {
            log::Debug("Playlist module reloaded");
        }
    }

    void Shutdown() override {
        if (shutdown_) Call(shutdown_);
        ices_perl_stop();
        init_ = shutdown_ = get_next_ = get_metadata_ = get_lineno_ = nullptr;
    }

 private:
    static const char* Find(const char* name, const char* legacy) {
        const char* found = ices_perl_has_sub(name) ? name : ices_perl_has_sub(legacy) ? legacy : nullptr;
        if (found) log::Debug("Found method: {}", found);
        return found;
    }

    void FindHooks() {
        init_ = Find("ices_init", "ices_perl_initialize");
        shutdown_ = Find("ices_shutdown", "ices_perl_shutdown");
        get_next_ = Find("ices_get_next", "ices_perl_get_next");
        get_metadata_ = Find("ices_get_metadata", "ices_perl_get_metadata");
        get_lineno_ = Find("ices_get_lineno", "ices_perl_get_current_lineno");
    }

    static std::optional<std::string> Call(const char* function) {
        if (!function) return std::nullopt;
        log::Debug("Interpreting [{}]", function);
        std::array<char, 1024> err{};
        int died = 0;
        std::unique_ptr<char, decltype(&std::free)> ret{ices_perl_call(function, &died, err.data(), err.size()),
                                                        &std::free};
        if (died) log::Debug("perl error: {}", err.data());
        log::Debug("Done interpreting [{}]", function);
        if (!ret) return std::nullopt;
        return std::string(ret.get());
    }

    PlaylistConfig config_;
    const char* init_ = nullptr;
    const char* shutdown_ = nullptr;
    const char* get_next_ = nullptr;
    const char* get_metadata_ = nullptr;
    const char* get_lineno_ = nullptr;
};

}  // namespace

std::expected<std::unique_ptr<Playlist>, std::string> MakePerlPlaylist(const PlaylistConfig& config) {
    auto pl = std::make_unique<PerlPlaylist>(config);
    if (auto r = pl->Init(); !r) return std::unexpected(r.error());
    return pl;
}

}  // namespace ices
