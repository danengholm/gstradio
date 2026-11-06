// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Configuration model and parsing (IceS 0.4 setup.c / ices_config.c).
//
// Precedence: hard-coded defaults, then the XML configuration file, then the
// command line. Each <Stream> element (or each -m option after the first)
// creates an additional stream fed from the same playlist.
#ifndef GSRADIO_SRC_CONFIG_H_
#define GSRADIO_SRC_CONFIG_H_

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gsradio {

enum class Protocol { kHttp, kXaudiocast, kIcy };
enum class PlaylistType { kBuiltin, kPython, kPerl };

[[nodiscard]] std::string_view ToString(Protocol p);
[[nodiscard]] std::string_view ToString(PlaylistType t);

struct StreamConfig {
    // Server
    std::string host = "127.0.0.1";
    int port = 8000;
    std::string password = "letmein";
    Protocol protocol = Protocol::kHttp;
    // Stream
    std::string mount = "/gsradio";
    std::optional<std::string> dumpfile;
    std::string name = "Default stream name";
    std::string genre = "Default genre";
    std::string description = "Default description";
    std::string url = "http://www.icecast.org/";
    bool is_public = true;
    // Encoding
    int bitrate = 128;
    bool reencode = false;
    int samplerate = -1;  // <= 0: choose like LAME would
    int channels = -1;    // <= 0: stereo
};

struct PlaylistConfig {
    PlaylistType type = PlaylistType::kBuiltin;
    bool randomize = false;
    std::string file = "playlist.txt";
    std::string module = "gsradio";
    int crossfade = 0;  // seconds, 0 = disabled
};

struct Config {
    std::string configfile = "gsradio.conf";
    bool daemon = false;
    int verbose = 0;
    std::string base_directory = "/tmp";
    PlaylistConfig playlist;
    std::vector<StreamConfig> streams{StreamConfig{}};

    [[nodiscard]] bool AnyReencode() const;
};

// Returned when the program should exit instead of running (e.g. -V, bad
// option); `code` is the process exit status.
struct ExitRequest {
    int code;
};

// Build the complete configuration from argv. Diagnostics are printed to
// stderr/stdout as appropriate.
[[nodiscard]] std::expected<Config, ExitRequest> LoadConfig(int argc, char** argv);

// Parse an XML configuration file into `config` (exposed for testing).
[[nodiscard]] std::expected<void, std::string> ParseConfigFile(Config& config, const std::string& path);

void PrintUsage();
void PrintVersion();
// Log the effective configuration in verbose mode.
void LogConfig(const Config& config);

}  // namespace gsradio

#endif  // GSRADIO_SRC_CONFIG_H_
