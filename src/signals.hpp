// Signal handling as in IceS 0.4:
//   SIGINT/SIGTERM  clean shutdown
//   SIGHUP          reopen the log file and reload the playlist module
//   SIGUSR1         skip to the next track
//   SIGPIPE/SIGIO/SIGALRM ignored
// Handlers only set flags; the streaming loop acts on them.
#pragma once

#include <chrono>

namespace ices::signals {

void install();

[[nodiscard]] bool quit_requested();
void request_quit();
// Each returns true once per delivered signal (flag is cleared on read).
[[nodiscard]] bool take_hup();
[[nodiscard]] bool take_skip();
[[nodiscard]] bool skip_pending();

// Sleep for up to `duration`, returning early if a shutdown is requested.
void interruptible_sleep(std::chrono::milliseconds duration);

}  // namespace ices::signals
