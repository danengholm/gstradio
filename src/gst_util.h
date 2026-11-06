// Copyright 2026 The gsradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Small RAII helpers for GStreamer / GLib objects.
#ifndef GSRADIO_SRC_GST_UTIL_H_
#define GSRADIO_SRC_GST_UTIL_H_

#include <gst/gst.h>

#include <memory>
#include <string>

namespace gsradio::gst {

struct ObjectUnref {
    void operator()(gpointer p) const {
        if (p) gst_object_unref(p);
    }
};
struct SampleUnref {
    void operator()(GstSample* s) const {
        if (s) gst_sample_unref(s);
    }
};
struct MessageUnref {
    void operator()(GstMessage* m) const {
        if (m) gst_message_unref(m);
    }
};
struct TagListUnref {
    void operator()(GstTagList* t) const {
        if (t) gst_tag_list_unref(t);
    }
};
struct CapsUnref {
    void operator()(GstCaps* c) const {
        if (c) gst_caps_unref(c);
    }
};

template <class T>
using ObjectPtr = std::unique_ptr<T, ObjectUnref>;
using SamplePtr = std::unique_ptr<GstSample, SampleUnref>;
using MessagePtr = std::unique_ptr<GstMessage, MessageUnref>;
using TagListPtr = std::unique_ptr<GstTagList, TagListUnref>;
using CapsPtr = std::unique_ptr<GstCaps, CapsUnref>;

// A pipeline that is shut down (state NULL) before being released.
struct PipelineDeleter {
    void operator()(GstElement* p) const {
        if (p) {
            gst_element_set_state(p, GST_STATE_NULL);
            gst_object_unref(p);
        }
    }
};
using PipelinePtr = std::unique_ptr<GstElement, PipelineDeleter>;

inline std::string TakeString(gchar* s) {
    std::string r = s ? s : "";
    g_free(s);
    return r;
}

// Read a string tag, returning an empty string if absent.
inline std::string TagString(const GstTagList* tags, const char* tag) {
    gchar* value = nullptr;
    if (tags && gst_tag_list_get_string(tags, tag, &value)) {
        return TakeString(value);
    }
    return {};
}

// Human readable error text from an ERROR/WARNING message.
inline std::string MessageText(GstMessage* msg) {
    GError* err = nullptr;
    gchar* dbg = nullptr;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        gst_message_parse_error(msg, &err, &dbg);
    } else {
        gst_message_parse_warning(msg, &err, &dbg);
    }
    std::string text = err ? err->message : "unknown error";
    if (err) g_error_free(err);
    g_free(dbg);
    return text;
}

}  // namespace gsradio::gst

#endif  // GSRADIO_SRC_GST_UTIL_H_
