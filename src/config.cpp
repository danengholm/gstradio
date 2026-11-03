#include "config.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <print>
#include <ranges>

#include "build_config.hpp"
#include "log.hpp"

namespace ices {

std::string_view to_string(Protocol p) {
  switch (p) {
    case Protocol::http: return "http";
    case Protocol::xaudiocast: return "xaudiocast";
    case Protocol::icy: return "icy";
  }
  std::unreachable();
}

std::string_view to_string(PlaylistType t) {
  switch (t) {
    case PlaylistType::builtin: return "builtin";
    case PlaylistType::python: return "python";
    case PlaylistType::perl: return "perl";
  }
  std::unreachable();
}

bool Config::any_reencode() const {
  return std::ranges::any_of(streams, &StreamConfig::reencode);
}

namespace {

constexpr std::string_view kIcesNamespace = "http://www.icecast.org/projects/ices";

// atoi() semantics (leading whitespace, optional sign, trailing junk ignored,
// 0 on failure) to stay compatible with existing configuration files.
int to_int(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  int value = 0;
  std::from_chars(s.data(), s.data() + s.size(), value);
  return value;
}

PlaylistType parse_playlist_type(std::string_view s) {
  if (s == "python") return PlaylistType::python;
  if (s == "perl") return PlaylistType::perl;
  return PlaylistType::builtin;
}

// ---- XML ------------------------------------------------------------------

struct XmlDocDeleter {
  void operator()(xmlDoc* d) const { xmlFreeDoc(d); }
};
using XmlDocPtr = std::unique_ptr<xmlDoc, XmlDocDeleter>;

std::string node_text(xmlDoc* doc, xmlNode* node) {
  xmlChar* raw = xmlNodeListGetString(doc, node->children, 1);
  if (!raw) return {};
  std::string text(reinterpret_cast<const char*>(raw));
  xmlFree(raw);
  return text;
}

std::string_view node_name(const xmlNode* node) {
  return reinterpret_cast<const char*>(node->name);
}

// Iterate over the element children of `parent`.
auto elements(xmlNode* parent) {
  struct Range {
    xmlNode* first;
    struct It {
      xmlNode* n;
      static xmlNode* skip(xmlNode* x) {
        while (x && x->type != XML_ELEMENT_NODE) x = x->next;
        return x;
      }
      xmlNode* operator*() const { return n; }
      It& operator++() {
        n = skip(n->next);
        return *this;
      }
      bool operator==(const It& o) const = default;
    };
    It begin() const { return It{It::skip(first)}; }
    It end() const { return It{nullptr}; }
  };
  return Range{parent->children};
}

void parse_server_node(xmlDoc* doc, xmlNode* node, StreamConfig& stream) {
  for (xmlNode* cur : elements(node)) {
    const auto name = node_name(cur);
    const auto text = node_text(doc, cur);
    if (name == "Port") {
      stream.port = to_int(text);
    } else if (name == "Hostname") {
      stream.host = text;
    } else if (name == "Password") {
      stream.password = text;
    } else if (name == "Protocol") {
      if (strcasecmp(text.c_str(), "icy") == 0)
        stream.protocol = Protocol::icy;
      else if (strcasecmp(text.c_str(), "http") == 0)
        stream.protocol = Protocol::http;
      else
        stream.protocol = Protocol::xaudiocast;
    } else {
      log::info("Unknown Server keyword: {}", name);
    }
  }
}

void parse_stream_node(xmlDoc* doc, xmlNode* node, StreamConfig& stream) {
  for (xmlNode* cur : elements(node)) {
    const auto name = node_name(cur);
    if (name == "Server") {
      parse_server_node(doc, cur, stream);
      continue;
    }
    const auto text = node_text(doc, cur);
    if (name == "Name") {
      stream.name = text;
    } else if (name == "Genre") {
      stream.genre = text;
    } else if (name == "Description") {
      stream.description = text;
    } else if (name == "URL") {
      stream.url = text;
    } else if (name == "Mountpoint") {
      stream.mount = text;
    } else if (name == "Dumpfile") {
      stream.dumpfile = text;
    } else if (name == "Bitrate") {
      stream.bitrate = to_int(text);
    } else if (name == "Public") {
      stream.is_public = to_int(text) != 0;
    } else if (name == "Reencode") {
      stream.reencode = to_int(text) != 0;
    } else if (name == "Samplerate") {
      stream.samplerate = to_int(text);
    } else if (name == "Channels") {
      stream.channels = to_int(text);
    } else {
      log::info("Unknown Stream keyword: {}", name);
    }
  }
}

void parse_execution_node(xmlDoc* doc, xmlNode* node, Config& config) {
  for (xmlNode* cur : elements(node)) {
    const auto name = node_name(cur);
    const auto text = node_text(doc, cur);
    if (name == "Background") {
      config.daemon = to_int(text) != 0;
    } else if (name == "Verbose") {
      config.verbose = to_int(text);
    } else if (name == "BaseDirectory") {
      config.base_directory = text;
    } else {
      log::info("Unknown Execution keyword: {}", name);
    }
  }
}

void parse_playlist_node(xmlDoc* doc, xmlNode* node, Config& config) {
  for (xmlNode* cur : elements(node)) {
    const auto name = node_name(cur);
    const auto text = node_text(doc, cur);
    if (name == "Randomize") {
      config.playlist.randomize = to_int(text) != 0;
    } else if (name == "Type") {
      config.playlist.type = parse_playlist_type(text);
    } else if (name == "File") {
      config.playlist.file = text;
    } else if (name == "Module") {
      config.playlist.module = text;
    } else if (name == "Crossfade") {
      config.playlist.crossfade = std::max(0, to_int(text));
    } else {
      log::info("Unknown playlist keyword: {}", name);
    }
  }
}

// ---- Command line ---------------------------------------------------------

// Options that take no argument, and all known options.
constexpr std::string_view kFlagOptions = "BRrsVvi";
constexpr std::string_view kKnownOptions = "BbCcdDFfghHiMmNnPpRrSstuVv";

std::optional<std::string> find_configfile_option(int argc, char** argv) {
  std::optional<std::string> result;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg.size() < 2 || arg[0] != '-') continue;
    if (kFlagOptions.contains(arg[1])) continue;
    if (arg[1] == 'c' && i + 1 < argc) result = argv[i + 1];
    ++i;  // skip this option's argument
  }
  return result;
}

std::expected<void, ExitRequest> parse_command_line(Config& config, int argc, char** argv) {
  // Each -m after the first creates a new stream; subsequent stream options
  // apply to it.
  std::size_t current = 0;
  int mounts_seen = 0;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-?") {
      print_usage();
      return std::unexpected(ExitRequest{0});
    }
    if (arg == "--version") {
      print_version();
      return std::unexpected(ExitRequest{0});
    }
    if (arg.size() != 2 || arg[0] != '-') {
      std::println(stderr, "Unknown argument: {}", arg);
      print_usage();
      return std::unexpected(ExitRequest{1});
    }
    const char opt = arg[1];
    if (!kKnownOptions.contains(opt)) {
      std::println(stderr, "Unknown option: {}", arg);
      print_usage();
      return std::unexpected(ExitRequest{1});
    }
    std::string value;
    if (!kFlagOptions.contains(opt)) {
      if (i + 1 >= argc) {
        std::println(stderr, "Option {} requires an argument!", opt);
        print_usage();
        return std::unexpected(ExitRequest{1});
      }
      value = argv[++i];
    }

    StreamConfig& stream = config.streams[current];
    switch (opt) {
      case 'B': config.daemon = true; break;
      case 'b': stream.bitrate = to_int(value); break;
      case 'C': config.playlist.crossfade = std::max(0, to_int(value)); break;
      case 'c': break;  // handled before the config file was parsed
      case 'd': stream.description = value; break;
      case 'D': config.base_directory = value; break;
      case 'F': config.playlist.file = value; break;
      case 'f': stream.dumpfile = value; break;
      case 'g': stream.genre = value; break;
      case 'h': stream.host = value; break;
      case 'H': stream.samplerate = to_int(value); break;
      case 'i': stream.protocol = Protocol::icy; break;  // "use icy headers"
      case 'M': config.playlist.module = value; break;
      case 'm':
        if (mounts_seen++ > 0) {
          config.streams.emplace_back();
          current = config.streams.size() - 1;
        }
        config.streams[current].mount = value;
        break;
      case 'N': stream.channels = to_int(value); break;
      case 'n': stream.name = value; break;
      case 'P': stream.password = value; break;
      case 'p': stream.port = to_int(value); break;
      case 'R': stream.reencode = true; break;
      case 'r': config.playlist.randomize = true; break;
      case 'S': config.playlist.type = parse_playlist_type(value); break;
      case 's': stream.is_public = false; break;
      case 't':
        if (value == "http")
          stream.protocol = Protocol::http;
        else if (value == "xaudiocast")
          stream.protocol = Protocol::xaudiocast;
        else if (value == "icy")
          stream.protocol = Protocol::icy;
        else {
          std::println(stderr, "Unknown protocol {}. Use 'http', 'xaudiocast' or 'icy'.", value);
          return std::unexpected(ExitRequest{1});
        }
        break;
      case 'u': stream.url = value; break;
      case 'V': print_version(); return std::unexpected(ExitRequest{0});
      case 'v': config.verbose = std::max(config.verbose, 1); break;
      default:
        std::println(stderr, "Unknown option: {}", arg);
        print_usage();
        return std::unexpected(ExitRequest{1});
    }
  }
  return {};
}

bool readable(const std::string& path) { return !path.empty() && ::access(path.c_str(), R_OK) == 0; }

}  // namespace

std::expected<void, std::string> parse_config_file(Config& config, const std::string& path) {
  XmlDocPtr doc{xmlReadFile(path.c_str(), nullptr, XML_PARSE_NONET | XML_PARSE_NOBLANKS)};
  if (!doc) return std::unexpected(std::format("XML Parser: Error while parsing {}", path));

  xmlNode* root = xmlDocGetRootElement(doc.get());
  if (!root) return std::unexpected(std::format("XML Parser: Empty document {}", path));

  if (!xmlSearchNsByHref(doc.get(), root,
                         reinterpret_cast<const xmlChar*>(kIcesNamespace.data()))) {
    return std::unexpected("XML Parser: Document of invalid type, no ices namespace found");
  }
  if (node_name(root) != "Configuration") {
    return std::unexpected(
        "XML Parser: Document of invalid type, root node is not 'Configuration'");
  }

  int nstreams = 0;
  for (xmlNode* cur : elements(root)) {
    const auto name = node_name(cur);
    if (name == "Stream") {
      // The first stream is preallocated (and may already hold defaults).
      if (nstreams++ > 0) config.streams.emplace_back();
      parse_stream_node(doc.get(), cur, config.streams.back());
    } else if (name == "Playlist") {
      parse_playlist_node(doc.get(), cur, config);
    } else if (name == "Execution") {
      parse_execution_node(doc.get(), cur, config);
    } else {
      log::info("Unknown Node: {}", name);
    }
  }
  return {};
}

std::expected<Config, ExitRequest> load_config(int argc, char** argv) {
  Config config;

  const auto explicit_file = find_configfile_option(argc, argv);
  if (explicit_file) config.configfile = *explicit_file;

  // Look for the file as given, then in the system configuration directory.
  std::string realname;
  if (readable(config.configfile)) {
    realname = config.configfile;
  } else if (auto sys = (std::filesystem::path(build::kEtcDir) / config.configfile).string();
             readable(sys)) {
    realname = sys;
  }

  if (!realname.empty()) {
    xmlInitParser();
    auto parsed = parse_config_file(config, realname);
    xmlCleanupParser();
    if (!parsed) {
      std::println(stderr, "{}", parsed.error());
      return std::unexpected(ExitRequest{1});
    }
  } else if (explicit_file) {
    // The original silently ignored a missing -c file, which hides typos.
    std::println(stderr, "Could not open configuration file {}", config.configfile);
    return std::unexpected(ExitRequest{1});
  }

  if (auto r = parse_command_line(config, argc, argv); !r) return std::unexpected(r.error());
  return config;
}

void print_usage() {
  std::print(
      "This is ices {} (gsradio {})\n"
      "ices <options>\n"
      "Options:\n"
      "\t-B (Background (daemon mode))\n"
      "\t-b <stream bitrate>\n"
      "\t-C <crossfade seconds>\n"
      "\t-c <configfile>\n"
      "\t-D <base directory>\n"
      "\t-d <stream description>\n"
      "\t-f <dumpfile on server>\n"
      "\t-F <playlist>\n"
      "\t-g <stream genre>\n"
      "\t-h <host>\n"
      "\t-i (use icy headers)\n"
      "\t-M <interpreter module>\n"
      "\t-m <mountpoint>\n"
      "\t-n <stream name>\n"
      "\t-p <port>\n"
      "\t-P <password>\n"
      "\t-R (activate reencoding)\n"
      "\t-r (randomize playlist)\n"
      "\t-s (private stream)\n"
      "\t-S <perl|python|builtin>\n"
      "\t-t <http|xaudiocast|icy>\n"
      "\t-u <stream url>\n"
      "\t-v (verbose output)\n"
      "\t-H <reencoded sample rate>\n"
      "\t-N <reencoded number of channels>\n"
      "\t-V (display version)\n",
      build::kIcesVersion, build::kPackageVersion);
}

void print_version() {
  std::string features = "GStreamer LAME libxml Vorbis FLAC MP3";
  if constexpr (build::kHavePerl) features += " Perl";
  if constexpr (build::kHavePython) features += " python";
  std::print("ices {} (gsradio {})\nFeatures: {}\nSystem configuration file: {}/ices.conf\n",
             build::kIcesVersion, build::kPackageVersion, features, build::kEtcDir);
  if constexpr (build::kHavePerl || build::kHavePython)
    std::print("Playlist module directory: {}\n", build::kModuleDir);
}

void log_config(const Config& config) {
  log::debug("Base directory: {}  Background: {}  Verbose: {}", config.base_directory,
             config.daemon ? 1 : 0, config.verbose);
  log::debug("Playlist: type {}, file {}, module {}, randomize {}, crossfade {}s",
             to_string(config.playlist.type), config.playlist.file, config.playlist.module,
             config.playlist.randomize ? 1 : 0, config.playlist.crossfade);
  for (auto [i, s] : std::views::enumerate(config.streams)) {
    log::debug("Stream: {}", i);
    log::debug("Host: {}:{} (protocol: {})", s.host, s.port, to_string(s.protocol));
    log::debug("Mount: {}, Password: {}", s.mount, s.password);
    log::debug("Name: {}\tURL: {}", s.name, s.url);
    log::debug("Genre: {}\tDesc: {}", s.genre, s.description);
    log::debug("Bitrate: {}\tPublic: {}", s.bitrate, s.is_public ? 1 : 0);
    log::debug("Reencode: {}\tSamplerate: {}\tChannels: {}", s.reencode ? 1 : 0, s.samplerate,
               s.channels);
    log::debug("Dump file: {}", s.dumpfile.value_or("(null)"));
  }
}

}  // namespace ices
