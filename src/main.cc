// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// gsradio - stream MP3 audio to an Icecast server.
// A C++23/GStreamer reimplementation of IceS 0.4 (ices0), including its
// on-the-fly transcoding of Ogg Vorbis, FLAC and MP3 to MP3.
#include <fcntl.h>
#include <gst/gst.h>
#include <shout/shout.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>

#include "src/config.h"
#include "src/log.h"
#include "src/playlist.h"
#include "src/signals.h"
#include "src/streamer.h"

namespace gsradio {
namespace {

std::filesystem::path PidfilePath(const Config& config) {
    return std::filesystem::path(config.base_directory) / "gsradio.pid";
}

// Detaches from the terminal. The parent reports the child's pid and exits.
void Daemonize(const Config& config) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::println(stderr, "ERROR: Cannot fork(), that means no daemon, sorry!");
        return;
    }
    if (pid > 0) {
        std::println("Into the land of the dreaded daemons we go... (pid: {})", pid);
        std::fflush(stdout);
        ::_exit(0);
    }
    ::setsid();
    if (int fd = ::open("/dev/null", O_RDWR); fd >= 0) {
        ::dup2(fd, STDIN_FILENO);
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) ::close(fd);
    }
    std::ofstream(PidfilePath(config)) << ::getpid();
}

bool HaveLame() {
    if (GstElementFactory* f = gst_element_factory_find("lamemp3enc")) {
        gst_object_unref(f);
        return true;
    }
    return false;
}

// Runs the configured playlist; returns the process exit status.
int Stream(const Config& config) {
    LogConfig(config);
    if (config.AnyReencode() && !HaveLame()) {
        log::Info(
            "Reencoding requires the GStreamer lamemp3enc element "
            "(gst-plugins-good), which was not found.");
        return 1;
    }
    auto playlist = MakePlaylist(config);
    if (!playlist) {
        log::Info("Initialization of playlist handler failed. [{}]", playlist.error());
        return 1;
    }
    int status = 1;
    if (auto streamer = Streamer::Create(config, **playlist); !streamer) {
        log::Info("{}", streamer.error());
    } else {
        log::Debug("Startup complete");
        status = (*streamer)->Run();
    }
    (*playlist)->Shutdown();
    return status;
}

int Main(int argc, char** argv) {
    signals::Install();

    auto config = LoadConfig(argc, argv);
    if (!config) return config.error().code;

    log::SetVerbose(config->verbose);
    log::SetTimestampFormat(config->timestamp_format);
    if (config->daemon) Daemonize(*config);
    log::SetDaemon(config->daemon);

    if (auto r = log::Open(config->base_directory); !r) {
        log::Info("{}", r.error());
    }
    log::Info("Logfile opened");

    // GStreamer starts threads, so it must be initialised after forking.
    gst_init(nullptr, nullptr);
    shout_init();

    const int status = Stream(*config);

    log::Info("gsradio exiting...");
    log::Close();
    if (config->daemon) {
        std::error_code ec;
        std::filesystem::remove(PidfilePath(*config), ec);
    }
    shout_shutdown();
    gst_deinit();
    return status;
}

}  // namespace
}  // namespace gsradio

int main(int argc, char** argv) { return gsradio::Main(argc, argv); }
