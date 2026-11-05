// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "src/decoder.h"

#include <gst/audio/audio.h>

#include <cstring>
#include <format>
#include <memory>
#include <string>

#include "src/log.h"

namespace ices {
namespace {

// Keep at most this much compressed data queued in front of the decoder.
constexpr guint64 kMaxQueuedBytes = 64 * 1024;

}  // namespace

std::expected<std::unique_ptr<Decoder>, std::string> Decoder::Create(int rate, std::uint64_t size_hint) {
    std::unique_ptr<Decoder> d{new Decoder};

    std::string caps = "audio/x-raw,format=" GST_AUDIO_NE(F32) ",layout=interleaved,channels=2";
    if (rate > 0) caps += std::format(",rate={}", rate);
    const std::string desc = std::format(
        "appsrc name=src ! decodebin ! audioconvert ! audioresample ! capsfilter "
        "caps=\"{}\" ! "
        "appsink name=sink sync=false",
        caps);

    GError* err = nullptr;
    d->pipeline_.reset(gst_parse_launch(desc.c_str(), &err));
    if (!d->pipeline_ || err) {
        std::string msg = err ? err->message : "unknown error";
        if (err) g_error_free(err);
        return std::unexpected(std::format("Could not create decoder: {}", msg));
    }

    gst::ObjectPtr<GstElement> src{gst_bin_get_by_name(GST_BIN(d->pipeline_.get()), "src")};
    gst::ObjectPtr<GstElement> sink{gst_bin_get_by_name(GST_BIN(d->pipeline_.get()), "sink")};
    d->src_ = GST_APP_SRC(src.get());
    d->sink_ = GST_APP_SINK(sink.get());

    g_object_set(d->src_, "stream-type", GST_APP_STREAM_TYPE_STREAM, "format", GST_FORMAT_BYTES, "block", FALSE,
                 "max-bytes", guint64{0}, nullptr);
    if (size_hint) {
        g_object_set(d->src_, "size", static_cast<gint64>(size_hint), nullptr);
    }
    // Bound decoded look-ahead: when full, decoding (and so reading) pauses.
    g_object_set(d->sink_, "emit-signals", FALSE, "max-buffers", guint{32}, "drop", FALSE, nullptr);

    d->bus_.reset(gst_element_get_bus(d->pipeline_.get()));
    if (gst_element_set_state(d->pipeline_.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        return std::unexpected("Could not start decoder pipeline");
    }
    return d;
}

bool Decoder::WantsInput() const { return gst_app_src_get_current_level_bytes(src_) < kMaxQueuedBytes; }

void Decoder::Push(std::span<const std::uint8_t> data) {
    if (data.empty()) return;
    GstBuffer* buf = gst_buffer_new_memdup(data.data(), data.size());
    gst_app_src_push_buffer(src_, buf);  // takes ownership
}

void Decoder::EndOfStream() { gst_app_src_end_of_stream(src_); }

Decoder::Status Decoder::Pull(std::vector<float>& out, std::chrono::milliseconds timeout) {
    const auto ns = static_cast<GstClockTime>(std::chrono::nanoseconds(timeout).count());
    gst::SamplePtr sample{gst_app_sink_try_pull_sample(sink_, ns)};
    if (!sample) {
        return gst_app_sink_is_eos(sink_) ? Status::kEos : Status::kAgain;
    }

    if (rate_ == 0) {
        GstAudioInfo info;
        if (GstCaps* caps = gst_sample_get_caps(sample.get()); caps && gst_audio_info_from_caps(&info, caps))
            rate_ = GST_AUDIO_INFO_RATE(&info);
    }

    GstBuffer* buf = gst_sample_get_buffer(sample.get());
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        const std::size_t n = map.size / sizeof(float);
        const std::size_t old = out.size();
        out.resize(old + n);
        std::memcpy(out.data() + old, map.data, n * sizeof(float));
        gst_buffer_unmap(buf, &map);
    }
    return Status::kData;
}

void Decoder::HandleTags(GstTagList* list) {
    if (auto a = gst::TagString(list, GST_TAG_ARTIST); !a.empty()) {
        tags_.artist = a;
    }
    if (auto t = gst::TagString(list, GST_TAG_TITLE); !t.empty()) tags_.title = t;
    guint br = 0;
    if (gst_tag_list_get_uint(list, GST_TAG_NOMINAL_BITRATE, &br) && br > 0) {
        bitrate_kbps_ = static_cast<int>(br / 1000);
    } else if (bitrate_kbps_ == 0 && gst_tag_list_get_uint(list, GST_TAG_BITRATE, &br) && br > 0) {
        bitrate_kbps_ = static_cast<int>(br / 1000);
    }
}

std::expected<void, std::string> Decoder::PollBus() {
    while (gst::MessagePtr msg{gst_bus_pop(bus_.get())}) {
        switch (GST_MESSAGE_TYPE(msg.get())) {
            case GST_MESSAGE_ERROR:
                return std::unexpected(std::format("Decoder error: {}", gst::MessageText(msg.get())));
            case GST_MESSAGE_WARNING:
                log::Debug("Decoder warning: {}", gst::MessageText(msg.get()));
                break;
            case GST_MESSAGE_TAG: {
                GstTagList* list = nullptr;
                gst_message_parse_tag(msg.get(), &list);
                HandleTags(list);
                gst_tag_list_unref(list);
                break;
            }
            default:
                break;
        }
    }
    return {};
}

std::optional<double> Decoder::QueryDurationSeconds() const {
    gint64 dur = 0;
    if (gst_element_query_duration(pipeline_.get(), GST_FORMAT_TIME, &dur) && dur > 0) {
        return static_cast<double>(dur) / GST_SECOND;
    }
    return std::nullopt;
}

}  // namespace ices
