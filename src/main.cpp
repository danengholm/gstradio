// ices - stream MP3 audio to an Icecast server.
// A C++23/GStreamer reimplementation of IceS 0.4 (ices0), including its
// on-the-fly transcoding of Ogg Vorbis, FLAC and MP3 to MP3.
#include <fcntl.h>
#include <gst/gst.h>
#include <shout/shout.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>

#include "config.hpp"
#include "log.hpp"
#include "playlist.hpp"
#include "signals.hpp"
#include "streamer.hpp"

namespace {

using namespace ices;

void write_pidfile(const Config& config) {
  const auto path = std::filesystem::path(config.base_directory) / "ices.pid";
  std::ofstream(path) << ::getpid();
}

// Detach from the terminal. The parent reports the child's pid and exits.
void daemonize(const Config& config) {
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
  write_pidfile(config);
}

bool have_lame() {
  if (GstElementFactory* f = gst_element_factory_find("lamemp3enc")) {
    gst_object_unref(f);
    return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  signals::install();

  auto config = load_config(argc, argv);
  if (!config) return config.error().code;

  log::set_verbose(config->verbose);
  if (config->daemon) daemonize(*config);
  log::set_daemon(config->daemon);

  if (auto r = log::open(config->base_directory); !r) log::info("{}", r.error());
  log::info("Logfile opened");

  // GStreamer starts threads, so it must be initialised after forking.
  gst_init(nullptr, nullptr);
  shout_init();

  int status = 1;
  {
    log_config(*config);
    if (config->any_reencode() && !have_lame()) {
      log::info("Reencoding requires the GStreamer lamemp3enc element (gst-plugins-good), "
                "which was not found.");
    } else if (auto playlist = make_playlist(*config); !playlist) {
      log::info("Initialization of playlist handler failed. [{}]", playlist.error());
    } else {
      log::debug("Startup complete");
      {
        Streamer streamer(*config, **playlist);
        status = streamer.run();
      }
      (*playlist)->shutdown();
    }
  }

  log::info("Ices Exiting...");
  log::close();
  if (config->daemon) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(config->base_directory) / "ices.pid", ec);
  }
  shout_shutdown();
  gst_deinit();
  return status;
}
