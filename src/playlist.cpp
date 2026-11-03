#include "playlist.hpp"

#include "build_config.hpp"
#include "log.hpp"

namespace ices {

#if !ICES_PYTHON_ENABLED
std::expected<std::unique_ptr<Playlist>, std::string> make_python_playlist(const PlaylistConfig&) {
  return std::unexpected("This binary has no support for embedded python");
}
#endif

#if !ICES_PERL_ENABLED
std::expected<std::unique_ptr<Playlist>, std::string> make_perl_playlist(const PlaylistConfig&) {
  return std::unexpected("This binary has no support for embedded perl");
}
#endif

std::expected<std::unique_ptr<Playlist>, std::string> make_playlist(const Config& config) {
  log::debug("Initializing playlist handler...");
  switch (config.playlist.type) {
    case PlaylistType::builtin: return make_builtin_playlist(config.playlist);
    case PlaylistType::python: return make_python_playlist(config.playlist);
    case PlaylistType::perl: return make_perl_playlist(config.playlist);
  }
  return std::unexpected("Unknown playlist module!");
}

}  // namespace ices
