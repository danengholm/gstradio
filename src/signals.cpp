#include "signals.hpp"

#include <atomic>
#include <csignal>
#include <thread>

namespace ices::signals {
namespace {

std::atomic<bool> g_quit{false};
std::atomic<bool> g_hup{false};
std::atomic<bool> g_skip{false};

static_assert(std::atomic<bool>::is_always_lock_free, "signal flags must be lock free");

extern "C" void on_quit(int) { g_quit.store(true, std::memory_order_relaxed); }
extern "C" void on_hup(int) { g_hup.store(true, std::memory_order_relaxed); }
extern "C" void on_usr1(int) { g_skip.store(true, std::memory_order_relaxed); }

void set_handler(int sig, void (*handler)(int), int flags) {
  struct sigaction sa{};
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = flags;
  sa.sa_handler = handler;
  sigaction(sig, &sa, nullptr);
}

}  // namespace

void install() {
  set_handler(SIGPIPE, SIG_IGN, 0);
  set_handler(SIGIO, SIG_IGN, 0);
  set_handler(SIGALRM, SIG_IGN, 0);
  set_handler(SIGINT, on_quit, 0);
  set_handler(SIGTERM, on_quit, 0);
  set_handler(SIGHUP, on_hup, SA_RESTART);
  set_handler(SIGUSR1, on_usr1, SA_RESTART);
}

bool quit_requested() { return g_quit.load(std::memory_order_relaxed); }
void request_quit() { g_quit.store(true); }
bool take_hup() { return g_hup.exchange(false); }
bool take_skip() { return g_skip.exchange(false); }
bool skip_pending() { return g_skip.load(std::memory_order_relaxed); }

void interruptible_sleep(std::chrono::milliseconds duration) {
  using namespace std::chrono_literals;
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (!quit_requested()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(deadline - now, 50ms));
  }
}

}  // namespace ices::signals
