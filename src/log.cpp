#include "log.hpp"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <print>

namespace ices::log {
namespace {

struct State {
  std::mutex mutex;
  std::FILE* file = nullptr;
  std::filesystem::path path;
};

State& state() {
  static State s;
  return s;
}

std::atomic<int> g_verbose{0};
std::atomic<bool> g_daemon{false};

std::expected<void, std::string> open_locked(State& s) {
  std::FILE* fp = std::fopen(s.path.c_str(), "a");
  if (!fp) {
    return std::unexpected(std::format("Error while opening {}, error: {}", s.path.string(),
                                       std::strerror(errno)));
  }
  setvbuf(fp, nullptr, _IOLBF, 0);
  s.file = fp;
  return {};
}

void close_locked(State& s) {
  if (s.file) {
    std::fclose(s.file);
    s.file = nullptr;
  }
}

}  // namespace

void set_verbose(int level) { g_verbose = level; }
bool verbose() { return g_verbose.load() > 0; }
void set_daemon(bool daemon) { g_daemon = daemon; }

std::expected<void, std::string> open(const std::filesystem::path& base_directory) {
  auto& s = state();
  std::scoped_lock lock(s.mutex);
  close_locked(s);
  s.path = base_directory / "ices.log";
  return open_locked(s);
}

std::expected<void, std::string> reopen() {
  auto& s = state();
  std::scoped_lock lock(s.mutex);
  if (s.path.empty()) return {};
  close_locked(s);
  return open_locked(s);
}

void close() {
  auto& s = state();
  std::scoped_lock lock(s.mutex);
  close_locked(s);
}

void write(std::string_view message, bool is_debug) {
  auto& s = state();
  const std::string_view prefix = is_debug ? "DEBUG: " : "";
  // Messages from libraries sometimes carry their own newline already.
  while (!message.empty() && message.back() == '\n') message.remove_suffix(1);

  std::scoped_lock lock(s.mutex);
  if (s.file) {
    // Unlike the original, timestamp file entries: it makes the log useful.
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto local = std::chrono::zoned_time{std::chrono::current_zone(), now};
    std::println(s.file, "[{:%Y-%m-%d %H:%M:%S}] {}{}", local, prefix, message);
  }
  if (!g_daemon) {
    std::println(stdout, "{}{}", prefix, message);
    std::fflush(stdout);
  }
}

}  // namespace ices::log
