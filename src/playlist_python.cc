// Copyright 2026 The gstradio Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Embedded Python playlist module (IceS 0.4 pm_python.c, for Python 3).
//
// The module (default "gstradio") is imported from PYTHONPATH, the playlist
// module directory and the current directory. Functions:
//   gstradio_get_next()      required, returns the next file name
//   gstradio_init()          optional, a negative result aborts startup
//   gstradio_shutdown()      optional
//   gstradio_get_metadata()  optional, returns a title override (or None)
//   gstradio_get_lineno()    optional, returns the line number for the cue file
// Python.h must be included before any standard header.
// clang-format off
#define PY_SSIZE_T_CLEAN
#include <Python.h>
// clang-format on

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "src/build_config.h"
#include "src/log.h"
#include "src/playlist.h"

namespace gstradio {
namespace {

// Owns one reference to a Python object.
class PyRef {
 public:
    PyRef() = default;
    explicit PyRef(PyObject* object) : ptr_(object) {}
    PyRef(const PyRef&) = delete;
    PyRef& operator=(const PyRef&) = delete;
    PyRef(PyRef&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    PyRef& operator=(PyRef&& other) noexcept {
        if (this != &other) {
            Py_XDECREF(ptr_);
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }
    ~PyRef() { Py_XDECREF(ptr_); }

    PyObject* get() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

 private:
    PyObject* ptr_ = nullptr;
};

class PythonPlaylist final : public Playlist {
 public:
    explicit PythonPlaylist(PlaylistConfig config) : config_(std::move(config)) {}
    ~PythonPlaylist() override { Finalize(); }

    std::expected<void, std::string> Init() {
        SetupPath();
        // 0: don't let Python install its own signal handlers.
        Py_InitializeEx(0);
        initialized_ = true;

        log::Debug("Importing {}.py module...", config_.module);
        module_ = PyRef(PyImport_ImportModule(config_.module.c_str()));
        if (!module_) {
            PyErr_Print();
            return std::unexpected(std::format("Could not import module {}", config_.module));
        }
        FindHooks();
        if (!get_next_) {
            return std::unexpected("The playlist module must define at least the gstradio_get_next method");
        }

        if (init_) {
            PyRef res = Call(init_);
            if (res && PyLong_Check(res.get())) {
                if (const int64_t rc = PyLong_AsLongLong(res.get()); rc < 0) {
                    return std::unexpected(std::format("gstradio_init failed (returned {})", rc));
                }
            } else {
                log::Info("gstradio_init failed");
            }
        }
        return {};
    }

    std::optional<std::string> Next() override {
        PyRef res = Call(get_next_);
        if (auto s = AsString(res)) return s;
        log::Info("gstradio_get_next failed");
        return std::nullopt;
    }

    std::optional<std::string> Metadata() override {
        if (!get_metadata_) return std::nullopt;
        PyRef res = Call(get_metadata_);
        if (res && res.get() == Py_None) return std::nullopt;
        auto s = AsString(res);
        if (!s) log::Info("gstradio_get_metadata failed");
        return s;
    }

    int Lineno() override {
        if (!get_lineno_) return 0;
        PyRef res = Call(get_lineno_);
        if (res && PyLong_Check(res.get())) {
            return static_cast<int>(PyLong_AsLong(res.get()));
        }
        log::Info("gstradio_get_lineno failed");
        return 0;
    }

    void Reload() override {
        PyRef fresh(PyImport_ReloadModule(module_.get()));
        if (!fresh) {
            log::Info("Playlist module reload failed");
            PyErr_Print();
            return;
        }
        module_ = std::move(fresh);
        FindHooks();
        log::Debug("Playlist module reloaded");
    }

    void Shutdown() override {
        if (module_ && shutdown_) {
            PyRef res = Call(shutdown_);
            if (!res) log::Info("gstradio_shutdown failed");
        }
        Finalize();
    }

 private:
    void SetupPath() {
        // Python doesn't look in the current directory for modules when
        // embedded; add it and the module directory.
        std::string path;
        if (const char* old = std::getenv("PYTHONPATH"); old && *old) {
            path = std::string(old) + ":";
        }
        path += std::string(build::kModuleDir) + ":.";
        setenv("PYTHONPATH", path.c_str(), 1);
    }

    const char* FindAttr(const char* name) {
        if (!PyObject_HasAttrString(module_.get(), name)) return nullptr;
        log::Debug("Found method: {}", name);
        return name;
    }

    void FindHooks() {
        init_ = FindAttr("gstradio_init");
        shutdown_ = FindAttr("gstradio_shutdown");
        get_next_ = FindAttr("gstradio_get_next");
        get_metadata_ = FindAttr("gstradio_get_metadata");
        get_lineno_ = FindAttr("gstradio_get_lineno");
    }

    PyRef Call(const char* function) {
        log::Debug("Interpreting [{}]", function);
        PyRef ret(PyObject_CallMethod(module_.get(), function, nullptr));
        if (!ret) PyErr_Print();
        log::Debug("Done interpreting [{}]", function);
        return ret;
    }

    static std::optional<std::string> AsString(const PyRef& obj) {
        if (!obj) return std::nullopt;
        if (PyUnicode_Check(obj.get())) {
            Py_ssize_t len = 0;
            const char* s = PyUnicode_AsUTF8AndSize(obj.get(), &len);
            if (s) return std::string(s, static_cast<std::size_t>(len));
            PyErr_Print();
            return std::nullopt;
        }
        if (PyBytes_Check(obj.get())) {
            return std::string(PyBytes_AsString(obj.get()), static_cast<std::size_t>(PyBytes_Size(obj.get())));
        }
        return std::nullopt;
    }

    void Finalize() {
        if (!initialized_) return;
        module_ = PyRef();
        Py_FinalizeEx();
        initialized_ = false;
    }

    PlaylistConfig config_;
    bool initialized_ = false;
    PyRef module_;
    const char* init_ = nullptr;
    const char* shutdown_ = nullptr;
    const char* get_next_ = nullptr;
    const char* get_metadata_ = nullptr;
    const char* get_lineno_ = nullptr;
};

}  // namespace

std::expected<std::unique_ptr<Playlist>, std::string> MakePythonPlaylist(const PlaylistConfig& config) {
    auto pl = std::make_unique<PythonPlaylist>(config);
    if (auto r = pl->Init(); !r) return std::unexpected(r.error());
    return pl;
}

}  // namespace gstradio
