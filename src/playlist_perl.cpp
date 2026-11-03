// Embedded Perl playlist module (IceS 0.4 pm_perl.c).
//
// "use <module>" is evaluated with the playlist module directory and the
// current directory in @INC; the module must end with "1;". Subs are looked up
// in package main: ices_get_next (required), ices_init, ices_shutdown,
// ices_get_metadata, ices_get_lineno, with the legacy ices_perl_* names as
// fallbacks.
#include <array>
#include <cstdlib>
#include <memory>

#include "log.hpp"
#include "perl_glue.h"
#include "playlist.hpp"

namespace ices {
namespace {

class PerlPlaylist final : public Playlist {
 public:
  explicit PerlPlaylist(PlaylistConfig config) : config_(std::move(config)) {}
  ~PerlPlaylist() override { ices_perl_term(); }

  std::expected<void, std::string> start() {
    log::debug("Importing perl module: {}", config_.module);
    std::array<char, 1024> err{};
    if (ices_perl_start(config_.module.c_str(), err.data(), err.size()) != 0)
      return std::unexpected(std::string(err.data()));
    find_hooks();
    if (!get_next_) return std::unexpected("The playlist module must define at least the ices_get_next method");
    return {};
  }

  std::expected<void, std::string> init() {
    if (auto r = start(); !r) return r;
    if (!init_) return {};
    auto ret = call(init_);
    const int rc = ret ? std::atoi(ret->c_str()) : -1;
    if (rc <= 0) log::info("Execution of 'ices_init' failed");
    if (rc < 0) return std::unexpected("ices_init failed");
    return {};
  }

  std::optional<std::string> next() override { return call(get_next_); }

  std::optional<std::string> metadata() override {
    return get_metadata_ ? call(get_metadata_) : std::nullopt;
  }

  int lineno() override {
    if (!get_lineno_) return 0;
    auto ret = call(get_lineno_);
    return ret ? std::atoi(ret->c_str()) : 0;
  }

  void reload() override {
    // Restart the interpreter so changes to the module take effect.
    shutdown();
    if (auto r = init(); !r) log::info("Playlist module reload failed: {}", r.error());
    else log::debug("Playlist module reloaded");
  }

  void shutdown() override {
    if (shutdown_) call(shutdown_);
    ices_perl_stop();
    init_ = shutdown_ = get_next_ = get_metadata_ = get_lineno_ = nullptr;
  }

 private:
  static const char* find(const char* name, const char* legacy) {
    const char* found = ices_perl_has_sub(name) ? name : ices_perl_has_sub(legacy) ? legacy : nullptr;
    if (found) log::debug("Found method: {}", found);
    return found;
  }

  void find_hooks() {
    init_ = find("ices_init", "ices_perl_initialize");
    shutdown_ = find("ices_shutdown", "ices_perl_shutdown");
    get_next_ = find("ices_get_next", "ices_perl_get_next");
    get_metadata_ = find("ices_get_metadata", "ices_perl_get_metadata");
    get_lineno_ = find("ices_get_lineno", "ices_perl_get_current_lineno");
  }

  static std::optional<std::string> call(const char* function) {
    if (!function) return std::nullopt;
    log::debug("Interpreting [{}]", function);
    std::array<char, 1024> err{};
    int died = 0;
    std::unique_ptr<char, decltype(&std::free)> ret{
        ices_perl_call(function, &died, err.data(), err.size()), &std::free};
    if (died) log::debug("perl error: {}", err.data());
    log::debug("Done interpreting [{}]", function);
    if (!ret) return std::nullopt;
    return std::string(ret.get());
  }

  PlaylistConfig config_;
  const char* init_ = nullptr;
  const char* shutdown_ = nullptr;
  const char* get_next_ = nullptr;
  const char* get_metadata_ = nullptr;
  const char* get_lineno_ = nullptr;
};

}  // namespace

std::expected<std::unique_ptr<Playlist>, std::string> make_perl_playlist(
    const PlaylistConfig& config) {
  auto pl = std::make_unique<PerlPlaylist>(config);
  if (auto r = pl->init(); !r) return std::unexpected(r.error());
  return pl;
}

}  // namespace ices
