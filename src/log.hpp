// Logging, modelled on IceS 0.4's log.c:
//  * every message goes to <basedir>/ices.log (opened in append mode) and,
//    unless running as a daemon, to stdout;
//  * debug messages are only emitted in verbose mode and carry a "DEBUG: "
//    prefix;
//  * SIGHUP closes and reopens the log file (for log rotation).
#pragma once

#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ices::log {

void set_verbose(int level);
[[nodiscard]] bool verbose();
void set_daemon(bool daemon);

std::expected<void, std::string> open(const std::filesystem::path& base_directory);
std::expected<void, std::string> reopen();
void close();

void write(std::string_view message, bool is_debug);

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
  write(std::format(fmt, std::forward<Args>(args)...), false);
}

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
  if (verbose()) write(std::format(fmt, std::forward<Args>(args)...), true);
}

}  // namespace ices::log
