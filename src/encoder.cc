// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/encoder.h"

#include <gst/audio/audio.h>
#if GSRADIO_HAVE_LIBLAME
#include <lame/lame.h>
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "src/log.h"

namespace gsradio {
namespace {

constexpr std::array kMpeg1Bitrates{32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
// MPEG-2 also allows 144 kbps, but lamemp3enc only accepts the common set.
constexpr std::array kMpeg2Bitrates{8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160};
constexpr std::array kMpeg25Bitrates{8, 16, 24, 32, 40, 48, 56, 64};
constexpr std::array kMp3Rates{8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};

// LAME's default lowpass frequency for a CBR bitrate (optimum_bandwidth).
int LameLowpass(int kbps) {
    struct Entry {
        int kbps;
        int lowpass;
    };
    static constexpr std::array<Entry, 17> kTable{{{8, 2000},
                                                   {16, 3700},
                                                   {24, 3900},
                                                   {32, 5500},
                                                   {40, 7000},
                                                   {48, 7500},
                                                   {56, 10000},
                                                   {64, 11000},
                                                   {80, 13500},
                                                   {96, 15100},
                                                   {112, 15600},
                                                   {128, 17000},
                                                   {160, 17500},
                                                   {192, 18600},
                                                   {224, 19400},
                                                   {256, 19700},
                                                   {320, 20500}}};
    auto best = std::ranges::min_element(kTable, {}, [kbps](const Entry& e) { return std::abs(e.kbps - kbps); });
    return best->lowpass;
}

}  // namespace

int LameDefaultSamplerate(int in, int bitrate_kbps, int channels) {
#if GSRADIO_HAVE_LIBLAME
    // Ask LAME itself, exactly as IceS 0.4 did after lame_init_params().
    if (lame_global_flags* lame = lame_init()) {
        lame_set_in_samplerate(lame, in);
        lame_set_brate(lame, bitrate_kbps);
        if (channels == 1) lame_set_mode(lame, MONO);
        int rate = 0;
        if (lame_init_params(lame) >= 0) rate = lame_get_out_samplerate(lame);
        lame_close(lame);
        if (rate > 0) return rate;
    }
#endif
    // Approximation of LAME's optimum_bandwidth/optimum_samplefreq logic.
    const int lowpass = LameLowpass(channels == 1 ? bitrate_kbps * 2 : bitrate_kbps);

    int suggested = 44100;
    if (in >= 48000) {
        suggested = 48000;
    } else if (in >= 44100) {
        suggested = 44100;
    } else if (in >= 32000) {
        suggested = 32000;
    } else if (in >= 24000) {
        suggested = 24000;
    } else if (in >= 22050) {
        suggested = 22050;
    } else if (in >= 16000) {
        suggested = 16000;
    } else if (in >= 12000) {
        suggested = 12000;
    } else if (in >= 11025) {
        suggested = 11025;
    } else if (in >= 8000) {
        suggested = 8000;
    }

    if (lowpass <= 15960) suggested = 44100;
    if (lowpass <= 15250) suggested = 32000;
    if (lowpass <= 11220) suggested = 24000;
    if (lowpass <= 9970) suggested = 22050;
    if (lowpass <= 7230) suggested = 16000;
    if (lowpass <= 5420) suggested = 12000;
    if (lowpass <= 4510) suggested = 11025;
    if (lowpass <= 3970) suggested = 8000;

    // Never upsample beyond the input.
    if (in < suggested) {
        if (in > 44100) return 48000;
        if (in > 32000) return 44100;
        if (in > 24000) return 32000;
        if (in > 22050) return 24000;
        if (in > 16000) return 22050;
        if (in > 12000) return 16000;
        if (in > 11025) return 12000;
        if (in > 8000) return 11025;
        return 8000;
    }
    return suggested;
}

int ValidMp3Samplerate(int rate) {
    return *std::ranges::min_element(kMp3Rates, {}, [rate](int r) { return std::abs(r - rate); });
}

int ValidCbrBitrate(int kbps, int samplerate) {
    auto nearest = [kbps](std::span<const int> table) {
        return *std::ranges::min_element(table, {}, [kbps](int b) { return std::abs(b - kbps); });
    };
    if (samplerate >= 32000) return nearest(kMpeg1Bitrates);
    if (samplerate >= 16000) return nearest(kMpeg2Bitrates);
    return nearest(kMpeg25Bitrates);
}

std::expected<std::unique_ptr<Encoder>, std::string> Encoder::Create(int in_rate, int out_rate, int out_channels,
                                                                     int bitrate_kbps) {
    std::unique_ptr<Encoder> e{new Encoder};
    e->in_rate_ = in_rate;

    const std::string desc = std::format(
      "appsrc name=src format=time is-live=false block=false "
      "caps=\"audio/x-raw,format=" GST_AUDIO_NE(F32) ","
      "layout=interleaved,channels=2,channel-mask=(bitmask)0x3,rate={}\" ! "
      "audioconvert ! audioresample ! "
      "capsfilter caps=\"audio/x-raw,rate={},channels={}\" ! "
      "lamemp3enc name=enc target=bitrate cbr=true bitrate={} ! "
      "capsfilter caps=\"audio/mpeg,rate={},channels={}\" ! "
      "appsink name=sink sync=false",
      in_rate, out_rate, out_channels, bitrate_kbps, out_rate, out_channels);

    GError* err = nullptr;
    e->pipeline_.reset(gst_parse_launch(desc.c_str(), &err));
    if (!e->pipeline_ || err) {
        std::string msg = err ? err->message : "unknown error";
        if (err) g_error_free(err);
        return std::unexpected(std::format("Could not create MP3 encoder: {}", msg));
    }
    gst::ObjectPtr<GstElement> src{gst_bin_get_by_name(GST_BIN(e->pipeline_.get()), "src")};
    gst::ObjectPtr<GstElement> sink{gst_bin_get_by_name(GST_BIN(e->pipeline_.get()), "sink")};
    e->src_ = GST_APP_SRC(src.get());
    e->sink_ = GST_APP_SINK(sink.get());
    e->bus_.reset(gst_element_get_bus(e->pipeline_.get()));

    if (gst_element_set_state(e->pipeline_.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        return std::unexpected("Could not start MP3 encoder pipeline");
    }
    log::Debug("MP3 encoder: {} Hz stereo in, {} Hz {} ch out, {} kbps CBR", in_rate, out_rate, out_channels,
               bitrate_kbps);
    return e;
}

void Encoder::Push(std::span<const float> samples) {
    if (samples.empty()) return;
    const std::uint64_t frames = samples.size() / 2;
    GstBuffer* buf = gst_buffer_new_memdup(samples.data(), samples.size_bytes());
    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(frames_pushed_, GST_SECOND, in_rate_);
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(frames, GST_SECOND, in_rate_);
    frames_pushed_ += frames;
    gst_app_src_push_buffer(src_, buf);
}

void Encoder::AppendSample(GstSample* sample, std::vector<std::uint8_t>& out) {
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        out.insert(out.end(), map.data, map.data + map.size);
        gst_buffer_unmap(buf, &map);
    }
}

void Encoder::PullAvailable(std::vector<std::uint8_t>& out, std::chrono::milliseconds wait) {
    auto timeout = static_cast<GstClockTime>(std::chrono::nanoseconds(wait).count());
    while (gst::SamplePtr s{gst_app_sink_try_pull_sample(sink_, timeout)}) {
        AppendSample(s.get(), out);
        timeout = 0;
    }
}

void Encoder::Drain(std::vector<std::uint8_t>& out, std::chrono::milliseconds timeout) {
    gst_app_src_end_of_stream(src_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        gst::SamplePtr s{gst_app_sink_try_pull_sample(sink_, 50 * GST_MSECOND)};
        if (s) {
            AppendSample(s.get(), out);
        } else if (gst_app_sink_is_eos(sink_)) {
            break;
        }
    }
}

std::expected<void, std::string> Encoder::PollBus() {
    while (gst::MessagePtr msg{
        gst_bus_pop_filtered(bus_.get(), static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))}) {
        if (GST_MESSAGE_TYPE(msg.get()) == GST_MESSAGE_ERROR) {
            return std::unexpected(std::format("Encoder error: {}", gst::MessageText(msg.get())));
        }
        log::Debug("Encoder warning: {}", gst::MessageText(msg.get()));
    }
    return {};
}

}  // namespace gsradio
