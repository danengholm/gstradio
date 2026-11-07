// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/signals.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <thread>

namespace gstradio::signals {
namespace {

std::atomic<bool> g_quit{false};
std::atomic<bool> g_hup{false};
std::atomic<bool> g_skip{false};

static_assert(std::atomic<bool>::is_always_lock_free, "signal flags must be lock free");

extern "C" void OnQuit(int) { g_quit.store(true, std::memory_order_relaxed); }
extern "C" void OnHup(int) { g_hup.store(true, std::memory_order_relaxed); }
extern "C" void OnUsr1(int) { g_skip.store(true, std::memory_order_relaxed); }

void SetHandler(int sig, void (*handler)(int), int flags) {
    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    sa.sa_handler = handler;
    sigaction(sig, &sa, nullptr);
}

}  // namespace

void Install() {
    SetHandler(SIGPIPE, SIG_IGN, 0);
    SetHandler(SIGIO, SIG_IGN, 0);
    SetHandler(SIGALRM, SIG_IGN, 0);
    SetHandler(SIGINT, OnQuit, 0);
    SetHandler(SIGTERM, OnQuit, 0);
    SetHandler(SIGHUP, OnHup, SA_RESTART);
    SetHandler(SIGUSR1, OnUsr1, SA_RESTART);
}

bool QuitRequested() { return g_quit.load(std::memory_order_relaxed); }
void RequestQuit() { g_quit.store(true); }
bool TakeHup() { return g_hup.exchange(false); }
bool TakeSkip() { return g_skip.exchange(false); }
bool SkipPending() { return g_skip.load(std::memory_order_relaxed); }

void InterruptibleSleep(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (!QuitRequested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        std::this_thread::sleep_for(
            std::min<std::chrono::steady_clock::duration>(deadline - now, std::chrono::milliseconds(50)));
    }
}

}  // namespace gstradio::signals
