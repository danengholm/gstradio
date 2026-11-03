// Embedded Python playlist module (IceS 0.4 pm_python.c, for Python 3).
//
// The module (default "ices") is imported from PYTHONPATH, the playlist
// module directory and the current directory. Functions:
//   ices_get_next()      required, returns the next file name
//   ices_init()          optional, a negative result aborts startup
//   ices_shutdown()      optional
//   ices_get_metadata()  optional, returns a title override (or None)
//   ices_get_lineno()    optional, returns the line number for the cue file
// The pre-0.2.1 names ices_python_initialize, ices_python_shutdown,
// ices_python_get_next, ices_python_get_metadata and
// ices_python_get_current_lineno are accepted as fallbacks.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstdlib>

#include "build_config.hpp"
#include "log.hpp"
#include "playlist.hpp"

namespace ices {
namespace {

struct PyRef {
  PyObject* p = nullptr;
  PyRef() = default;
  explicit PyRef(PyObject* o) : p(o) {}
  PyRef(const PyRef&) = delete;
  PyRef& operator=(const PyRef&) = delete;
  PyRef(PyRef&& o) noexcept : p(std::exchange(o.p, nullptr)) {}
  PyRef& operator=(PyRef&& o) noexcept {
    if (this != &o) {
      Py_XDECREF(p);
      p = std::exchange(o.p, nullptr);
    }
    return *this;
  }
  ~PyRef() { Py_XDECREF(p); }
  explicit operator bool() const { return p != nullptr; }
};

class PythonPlaylist final : public Playlist {
 public:
  explicit PythonPlaylist(PlaylistConfig config) : config_(std::move(config)) {}
  ~PythonPlaylist() override { finalize(); }

  std::expected<void, std::string> init() {
    setup_path();
    // 0: don't let Python install its own signal handlers.
    Py_InitializeEx(0);
    initialized_ = true;

    log::debug("Importing {}.py module...", config_.module);
    module_ = PyRef(PyImport_ImportModule(config_.module.c_str()));
    if (!module_) {
      PyErr_Print();
      return std::unexpected(std::format("Could not import module {}", config_.module));
    }
    find_hooks();
    if (!get_next_) return std::unexpected("The playlist module must define at least the ices_get_next method");

    if (init_) {
      PyRef res = call(init_);
      if (res && PyLong_Check(res.p)) {
        if (const long rc = PyLong_AsLong(res.p); rc < 0)
          return std::unexpected(std::format("ices_init failed (returned {})", rc));
      } else {
        log::info("ices_init failed");
      }
    }
    return {};
  }

  std::optional<std::string> next() override {
    PyRef res = call(get_next_);
    if (auto s = as_string(res)) return s;
    log::info("ices_get_next failed");
    return std::nullopt;
  }

  std::optional<std::string> metadata() override {
    if (!get_metadata_) return std::nullopt;
    PyRef res = call(get_metadata_);
    if (res && res.p == Py_None) return std::nullopt;
    auto s = as_string(res);
    if (!s) log::info("ices_get_metadata failed");
    return s;
  }

  int lineno() override {
    if (!get_lineno_) return 0;
    PyRef res = call(get_lineno_);
    if (res && PyLong_Check(res.p)) return static_cast<int>(PyLong_AsLong(res.p));
    log::info("ices_get_lineno failed");
    return 0;
  }

  void reload() override {
    PyRef fresh(PyImport_ReloadModule(module_.p));
    if (!fresh) {
      log::info("Playlist module reload failed");
      PyErr_Print();
      return;
    }
    module_ = std::move(fresh);
    find_hooks();
    log::debug("Playlist module reloaded");
  }

  void shutdown() override {
    if (module_ && shutdown_) {
      PyRef res = call(shutdown_);
      if (!res) log::info("ices_shutdown failed");
    }
    finalize();
  }

 private:
  void setup_path() {
    // Python doesn't look in the current directory for modules when
    // embedded; add it and the module directory.
    std::string path;
    if (const char* old = std::getenv("PYTHONPATH"); old && *old) path = std::string(old) + ":";
    path += std::string(build::kModuleDir) + ":.";
    setenv("PYTHONPATH", path.c_str(), 1);
  }

  const char* find_attr(const char* name, const char* legacy) {
    const char* found = nullptr;
    if (PyObject_HasAttrString(module_.p, name))
      found = name;
    else if (PyObject_HasAttrString(module_.p, legacy))
      found = legacy;
    if (found) log::debug("Found method: {}", found);
    return found;
  }

  void find_hooks() {
    init_ = find_attr("ices_init", "ices_python_initialize");
    shutdown_ = find_attr("ices_shutdown", "ices_python_shutdown");
    get_next_ = find_attr("ices_get_next", "ices_python_get_next");
    get_metadata_ = find_attr("ices_get_metadata", "ices_python_get_metadata");
    get_lineno_ = find_attr("ices_get_lineno", "ices_python_get_current_lineno");
  }

  PyRef call(const char* function) {
    log::debug("Interpreting [{}]", function);
    PyRef ret(PyObject_CallMethod(module_.p, function, nullptr));
    if (!ret) PyErr_Print();
    log::debug("Done interpreting [{}]", function);
    return ret;
  }

  static std::optional<std::string> as_string(const PyRef& obj) {
    if (!obj) return std::nullopt;
    if (PyUnicode_Check(obj.p)) {
      Py_ssize_t len = 0;
      const char* s = PyUnicode_AsUTF8AndSize(obj.p, &len);
      if (s) return std::string(s, static_cast<std::size_t>(len));
      PyErr_Print();
      return std::nullopt;
    }
    if (PyBytes_Check(obj.p))
      return std::string(PyBytes_AsString(obj.p), static_cast<std::size_t>(PyBytes_Size(obj.p)));
    return std::nullopt;
  }

  void finalize() {
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

std::expected<std::unique_ptr<Playlist>, std::string> make_python_playlist(
    const PlaylistConfig& config) {
  auto pl = std::make_unique<PythonPlaylist>(config);
  if (auto r = pl->init(); !r) return std::unexpected(r.error());
  return pl;
}

}  // namespace ices
