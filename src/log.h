// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Logging, modelled on IceS 0.4's log.c:
//  * every message goes to <basedir>/gsradio.log (opened in append mode) and,
//    unless running as a daemon, to stdout;
//  * debug messages are only emitted in verbose mode and carry a "DEBUG: "
//    prefix;
//  * SIGHUP closes and reopens the log file (for log rotation).
#ifndef GSRADIO_SRC_LOG_H_
#define GSRADIO_SRC_LOG_H_

#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace gsradio::log {

void SetVerbose(int level);
[[nodiscard]] bool IsVerbose();
void SetDaemon(bool daemon);

std::expected<void, std::string> Open(const std::filesystem::path& base_directory);
std::expected<void, std::string> Reopen();
void Close();

void Write(std::string_view message, bool is_debug);

template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
    Write(std::format(fmt, std::forward<Args>(args)...), false);
}

template <class... Args>
void Debug(std::format_string<Args...> fmt, Args&&... args) {
    if (IsVerbose()) Write(std::format(fmt, std::forward<Args>(args)...), true);
}

}  // namespace gsradio::log

#endif  // GSRADIO_SRC_LOG_H_
