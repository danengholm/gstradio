// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// A libshout connection to one mountpoint. Connection and error handling
// follow IceS 0.4 stream.c: connections are opened lazily on first send,
// retried at most once per second, and every failure increments a
// consecutive error counter.
#ifndef GSTRADIO_SRC_SHOUT_STREAM_H_
#define GSTRADIO_SRC_SHOUT_STREAM_H_

#include <shout/shout.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "src/config.h"

namespace gstradio {

class ShoutStream {
 public:
    enum class SendResult {
        kOk,
        kFailed,    // New error, see last_error().
        kDeferred,  // Not connected and still inside the reconnect back-off.
    };

    static std::expected<std::unique_ptr<ShoutStream>, std::string> Create(const StreamConfig& config);

    ShoutStream(const ShoutStream&) = delete;
    ShoutStream& operator=(const ShoutStream&) = delete;
    ~ShoutStream();

    // User agent sent to the server.
    static std::string UserAgent();

    // Public URL of the mountpoint, for log messages.
    std::string Url() const;

    // Sends data, connecting first if necessary.
    SendResult Send(std::span<const std::uint8_t> data);
    bool IsConnected();
    void Close();
    // Advertised audio parameters; they take effect on the next connection.
    void SetAudioFormat(int samplerate, int channels);

    const StreamConfig& config() const { return config_; }
    int errors() const { return errs_; }
    void ResetErrors() { errs_ = 0; }
    const std::string& last_error() const { return last_error_; }

    // Called after every successful (re)connection.
    void set_on_connect(std::function<void(ShoutStream&)> callback) { on_connect_ = std::move(callback); }

 private:
    ShoutStream(const StreamConfig& config, shout_t* shout);
    SendResult Connect();

    StreamConfig config_;
    shout_t* shout_;
    int errs_ = 0;
    std::int64_t connect_delay_ = 0;  // Earliest time (s) of the next attempt.
    std::string last_error_;
    std::function<void(ShoutStream&)> on_connect_;
};

// Applies the server settings of `config` to a shout_t.
void ConfigureShout(shout_t* shout, const StreamConfig& config);

}  // namespace gstradio

#endif  // GSTRADIO_SRC_SHOUT_STREAM_H_
