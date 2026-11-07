// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/shout_stream.h"

#include <cstdint>
#include <ctime>
#include <format>
#include <memory>
#include <string>

#include "src/build_config.h"
#include "src/log.h"

namespace gstradio {

std::string ShoutStream::UserAgent() {
    return std::format("gstradio/{} libshout/{}", build::kPackageVersion, shout_version(nullptr, nullptr, nullptr));
}

void ConfigureShout(shout_t* s, const StreamConfig& c) {
    shout_set_host(s, c.host.c_str());
    shout_set_port(s, static_cast<std::uint16_t>(c.port));
    shout_set_password(s, c.password.c_str());
    shout_set_content_format(s, SHOUT_FORMAT_MP3, SHOUT_USAGE_AUDIO, nullptr);
    switch (c.protocol) {
        case Protocol::kIcy:
            shout_set_protocol(s, SHOUT_PROTOCOL_ICY);
            break;
        case Protocol::kHttp:
            shout_set_protocol(s, SHOUT_PROTOCOL_HTTP);
            break;
        case Protocol::kXaudiocast:
            shout_set_protocol(s, SHOUT_PROTOCOL_XAUDIOCAST);
            break;
    }
    shout_set_mount(s, c.mount.c_str());
    shout_set_agent(s, ShoutStream::UserAgent().c_str());
}

std::expected<std::unique_ptr<ShoutStream>, std::string> ShoutStream::Create(const StreamConfig& config) {
    shout_t* shout = shout_new();
    if (!shout) return std::unexpected("Could not create shout interface");
    return std::unique_ptr<ShoutStream>(new ShoutStream(config, shout));
}

ShoutStream::ShoutStream(const StreamConfig& config, shout_t* shout) : config_(config), shout_(shout) {
    ConfigureShout(shout_, config_);
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
    shout_close(shout_);
    shout_free(shout_);
}

std::string ShoutStream::Url() const {
    const auto& m = config_.mount;
    return std::format("http://{}:{}{}{}", config_.host, config_.port, (!m.empty() && m.front() == '/') ? "" : "/", m);
}

void ShoutStream::SetAudioFormat(int samplerate, int channels) {
    shout_set_audio_info(shout_, SHOUT_AI_SAMPLERATE, std::to_string(samplerate).c_str());
    shout_set_audio_info(shout_, SHOUT_AI_CHANNELS, std::to_string(channels).c_str());
}

bool ShoutStream::IsConnected() { return shout_get_connected(shout_) == SHOUTERR_CONNECTED; }

void ShoutStream::Close() {
    if (IsConnected()) shout_close(shout_);
}

ShoutStream::SendResult ShoutStream::Connect() {
    const std::int64_t now = std::time(nullptr);
    if (connect_delay_ > now) return SendResult::kDeferred;

    if (shout_open(shout_) != SHOUTERR_SUCCESS) {
        last_error_ = std::format("Mount failed on {}, error: {}", Url(), shout_get_error(shout_));
        connect_delay_ = now + 1;
        ++errs_;
        return SendResult::kFailed;
    }
    log::Info("Mounted on {}", Url());
    if (on_connect_) on_connect_(*this);
    return SendResult::kOk;
}

ShoutStream::SendResult ShoutStream::Send(std::span<const std::uint8_t> data) {
    if (!IsConnected()) {
        if (auto r = Connect(); r != SendResult::kOk) return r;
    }

    shout_sync(shout_);
    if (shout_send(shout_, data.data(), data.size()) == SHOUTERR_SUCCESS) {
        errs_ = 0;
        return SendResult::kOk;
    }
    last_error_ = std::format("Libshout reported send error, disconnecting: {}", shout_get_error(shout_));
    shout_close(shout_);
    ++errs_;
    return SendResult::kFailed;
}

}  // namespace gstradio
