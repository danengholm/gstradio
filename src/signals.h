// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Signal handling as in IceS 0.4:
//   SIGINT/SIGTERM  clean shutdown
//   SIGHUP          reopen the log file and reload the playlist module
//   SIGUSR1         skip to the next track
//   SIGPIPE/SIGIO/SIGALRM ignored
// Handlers only set flags; the streaming loop acts on them.
#ifndef GSTRADIO_SRC_SIGNALS_H_
#define GSTRADIO_SRC_SIGNALS_H_

#include <chrono>

namespace gstradio::signals {

void Install();

[[nodiscard]] bool QuitRequested();
void RequestQuit();
// Each returns true once per delivered signal (flag is cleared on read).
[[nodiscard]] bool TakeHup();
[[nodiscard]] bool TakeSkip();
[[nodiscard]] bool SkipPending();

// Sleep for up to `duration`, returning early if a shutdown is requested.
void InterruptibleSleep(std::chrono::milliseconds duration);

}  // namespace gstradio::signals

#endif  // GSTRADIO_SRC_SIGNALS_H_
