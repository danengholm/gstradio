// One output stream: a libshout connection plus its encoding state.
// Connection and error handling follow IceS 0.4 stream.c: connections are
// opened lazily on first send, retried at most once per second, and every
// failure increments a consecutive error counter.
#pragma once

#include <shout/shout.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "config.hpp"
#include "encoder.hpp"

namespace ices {

class ShoutStream {
 public:
  ShoutStream(StreamConfig config, std::size_t index);
  ~ShoutStream();
  ShoutStream(const ShoutStream&) = delete;
  ShoutStream& operator=(const ShoutStream&) = delete;

  [[nodiscard]] const StreamConfig& config() const { return config_; }
  [[nodiscard]] StreamConfig& config() { return config_; }
  [[nodiscard]] std::size_t index() const { return index_; }
  [[nodiscard]] std::string url() const;
  [[nodiscard]] static std::string user_agent();

  enum class SendResult {
    ok,
    failed,    // new error, see last_error()
    deferred,  // not connected and still inside the reconnect back-off
  };
  // Send data, connecting first if necessary.
  SendResult send(std::span<const std::uint8_t> data);
  [[nodiscard]] bool connected();
  void close();
  // Advertised audio parameters (take effect on the next connection).
  void set_audio_format(int samplerate, int channels);

  [[nodiscard]] int errors() const { return errs_; }
  void reset_errors() { errs_ = 0; }
  [[nodiscard]] const std::string& last_error() const { return last_error_; }

  // Called after every successful (re)connection.
  std::function<void(ShoutStream&)> on_connect;

  // --- encoding state, managed by the streamer ---
  enum class Mode { raw, encode };
  Mode mode = Mode::raw;
  std::unique_ptr<Encoder> encoder;
  std::vector<std::uint8_t> pending;  // data waiting to be sent
  int encode_rate = 0;                // effective output sample rate
  int encode_channels = 0;
  int encode_bitrate = 0;
  // Pacing of the asynchronous encoder: audio pushed in vs. data consumed
  // (sent or dropped) on the way out.
  double pushed_seconds = 0;
  std::uint64_t consumed_bytes = 0;
  [[nodiscard]] double encoder_lead() const {
    if (!encoder || encode_bitrate <= 0) return 0;
    return pushed_seconds - static_cast<double>(consumed_bytes) / (encode_bitrate * 125.0);
  }

 private:
  SendResult connect();

  StreamConfig config_;
  std::size_t index_;
  shout_t* shout_ = nullptr;
  int errs_ = 0;
  std::int64_t connect_delay_ = 0;  // earliest time (s) for the next attempt
  std::string last_error_;
};

// Apply the server/stream settings of `config` to a shout_t.
void configure_shout(shout_t* shout, const StreamConfig& config);

}  // namespace ices
