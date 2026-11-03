#include "shout_stream.hpp"

#include <ctime>
#include <format>

#include "build_config.hpp"
#include "log.hpp"

namespace ices {

std::string ShoutStream::user_agent() {
  return std::format("ices/{} (gsradio/{}) libshout/{}", build::kIcesVersion,
                     build::kPackageVersion, shout_version(nullptr, nullptr, nullptr));
}

void configure_shout(shout_t* s, const StreamConfig& c) {
  shout_set_host(s, c.host.c_str());
  shout_set_port(s, static_cast<unsigned short>(c.port));
  shout_set_password(s, c.password.c_str());
  shout_set_content_format(s, SHOUT_FORMAT_MP3, SHOUT_USAGE_AUDIO, nullptr);
  switch (c.protocol) {
    case Protocol::icy: shout_set_protocol(s, SHOUT_PROTOCOL_ICY); break;
    case Protocol::http: shout_set_protocol(s, SHOUT_PROTOCOL_HTTP); break;
    case Protocol::xaudiocast: shout_set_protocol(s, SHOUT_PROTOCOL_XAUDIOCAST); break;
  }
  shout_set_mount(s, c.mount.c_str());
  shout_set_agent(s, ShoutStream::user_agent().c_str());
}

ShoutStream::ShoutStream(StreamConfig config, std::size_t index)
    : config_(std::move(config)), index_(index), shout_(shout_new()) {
  if (!shout_) throw std::runtime_error("Could not create shout interface");
  configure_shout(shout_, config_);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  if (config_.dumpfile) shout_set_dumpfile(shout_, config_.dumpfile->c_str());
#pragma GCC diagnostic pop
  shout_set_meta(shout_, SHOUT_META_NAME, config_.name.c_str());
  shout_set_meta(shout_, SHOUT_META_URL, config_.url.c_str());
  shout_set_meta(shout_, SHOUT_META_GENRE, config_.genre.c_str());
  shout_set_meta(shout_, SHOUT_META_DESCRIPTION, config_.description.c_str());
  shout_set_audio_info(shout_, SHOUT_AI_BITRATE, std::to_string(config_.bitrate).c_str());
  shout_set_public(shout_, config_.is_public ? 1 : 0);
}

ShoutStream::~ShoutStream() {
  if (shout_) {
    shout_close(shout_);
    shout_free(shout_);
  }
}

std::string ShoutStream::url() const {
  const auto& m = config_.mount;
  return std::format("http://{}:{}{}{}", config_.host, config_.port,
                     (!m.empty() && m.front() == '/') ? "" : "/", m);
}

void ShoutStream::set_audio_format(int samplerate, int channels) {
  shout_set_audio_info(shout_, SHOUT_AI_SAMPLERATE, std::to_string(samplerate).c_str());
  shout_set_audio_info(shout_, SHOUT_AI_CHANNELS, std::to_string(channels).c_str());
}

bool ShoutStream::connected() { return shout_get_connected(shout_) == SHOUTERR_CONNECTED; }

void ShoutStream::close() {
  if (connected()) shout_close(shout_);
}

ShoutStream::SendResult ShoutStream::connect() {
  const std::int64_t now = std::time(nullptr);
  if (connect_delay_ > now) return SendResult::deferred;

  if (shout_open(shout_) != SHOUTERR_SUCCESS) {
    last_error_ = std::format("Mount failed on {}, error: {}", url(), shout_get_error(shout_));
    connect_delay_ = now + 1;
    ++errs_;
    return SendResult::failed;
  }
  log::info("Mounted on {}", url());
  if (on_connect) on_connect(*this);
  return SendResult::ok;
}

ShoutStream::SendResult ShoutStream::send(std::span<const std::uint8_t> data) {
  if (!connected())
    if (auto r = connect(); r != SendResult::ok) return r;

  shout_sync(shout_);
  if (shout_send(shout_, data.data(), data.size()) == SHOUTERR_SUCCESS) {
    errs_ = 0;
    return SendResult::ok;
  }
  last_error_ = std::format("Libshout reported send error, disconnecting: {}",
                            shout_get_error(shout_));
  shout_close(shout_);
  ++errs_;
  return SendResult::failed;
}

}  // namespace ices
