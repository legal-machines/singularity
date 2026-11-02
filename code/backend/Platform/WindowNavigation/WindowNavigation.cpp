#include "Platform/WindowNavigation/WindowNavigation.hpp"

#include <cctype>
#include <optional>

namespace singularity::platform {
namespace {

std::string lowercase(std::string text) {
  for (char &character : text) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return text;
}

// Whether the path of `url` has a ".." segment in any spelling - plain,
// percent-encoded, after a backslash - which climbs out of a prefix the URL
// begins with. The query and the fragment never choose the page, so a route
// such as #/documents/.. is no climb.
bool climbs(const std::string &url) {
  const std::string path = url.substr(0, url.find_first_of("?#"));
  std::string plain;
  for (std::size_t at = 0; at < path.size(); ++at) {
    const std::string escape = path[at] == '%' && at + 2 < path.size() ? lowercase(path.substr(at + 1, 2)) : "";
    if (escape == "2e") {
      plain.push_back('.');
      at += 2;
    } else if (escape == "2f" || escape == "5c") {
      plain.push_back('/');
      at += 2;
    } else {
      plain.push_back(path[at] == '\\' ? '/' : path[at]);
    }
  }
  for (std::size_t at = plain.find("/.."); at != std::string::npos; at = plain.find("/..", at + 1))
    if (at + 3 == plain.size() || plain[at + 3] == '/') return true;
  return false;
}

// A page under `prefix`, and nothing that climbs out of it.
bool under(const std::string &url, const std::string &prefix) {
  return !prefix.empty() && url.compare(0, prefix.size(), prefix) == 0 && !climbs(url);
}

// The security origin of the pages under a prefix, as WebKit reports it for a
// frame: scheme, host, and the port - the one named, else the scheme's default.
struct Origin {
  std::string protocol;
  std::string host;
  int port = 0;
  int default_port = 0;
};

std::optional<Origin> origin_of(const std::string &prefix) {
  // Every file URL has the one origin "file", with no host and no port; the
  // folder is told apart by the URL itself.
  if (prefix.rfind("file://", 0) == 0) return Origin{"file", "", 0, 0};
  struct Scheme {
    const char *name;
    int default_port;
  };
  for (const Scheme scheme : {Scheme{"http", 80}, Scheme{"https", 443}}) {
    const std::string start = std::string(scheme.name) + "://";
    if (prefix.rfind(start, 0) != 0) continue;
    std::string authority = prefix.substr(start.size(), prefix.find_first_of("/?#", start.size()) - start.size());
    // An origin has no user name or password.
    const std::size_t user_end = authority.rfind('@');
    if (user_end != std::string::npos) authority = authority.substr(user_end + 1);
    // A port follows the last colon, unless that colon is inside an IPv6 [host].
    const std::size_t colon = authority.rfind(':');
    const std::size_t bracket = authority.rfind(']');
    const bool has_port = colon != std::string::npos && (bracket == std::string::npos || colon > bracket);
    Origin origin{scheme.name, lowercase(has_port ? authority.substr(0, colon) : authority), scheme.default_port,
        scheme.default_port};
    if (origin.host.empty()) return std::nullopt;
    if (has_port) {
      const std::string digits = authority.substr(colon + 1);
      if (digits.empty() || digits.size() > 5) return std::nullopt;
      for (const char digit : digits)
        if (!std::isdigit(static_cast<unsigned char>(digit))) return std::nullopt;
      origin.port = std::stoi(digits);
      if (origin.port > 65535) return std::nullopt;
    }
    return origin;
  }
  return std::nullopt;
}

bool same_origin(const Origin &origin, const std::string &protocol, const std::string &host, int port) {
  if (lowercase(protocol) != origin.protocol) return false;
  const std::string reported = lowercase(host);
  // WebKit may give an IPv6 host with or without its brackets.
  if (reported != origin.host && "[" + reported + "]" != origin.host) return false;
  // WebKit reports a scheme's default port as 0.
  return port == origin.port || (port == 0 && origin.port == origin.default_port);
}

} // namespace

bool window_may_show(const std::string &url, const std::vector<std::string> &allowed_prefixes, bool main_frame) {
  if (url == "about:blank") return true;
  if (!main_frame && (url == "about:srcdoc" || url.rfind("blob:", 0) == 0 || url.rfind("data:", 0) == 0)) return true;
  for (const std::string &prefix : allowed_prefixes)
    if (under(url, prefix)) return true;
  return false;
}

bool message_may_reach_native(bool main_frame, const std::string &url, const std::string &protocol,
    const std::string &host, int port, const std::vector<std::string> &allowed_prefixes) {
  if (!main_frame) return false;
  for (const std::string &prefix : allowed_prefixes) {
    if (!under(url, prefix)) continue;
    const std::optional<Origin> origin = origin_of(prefix);
    if (origin && same_origin(*origin, protocol, host, port)) return true;
  }
  return false;
}

std::string window_prefix_of(const std::string &url, bool directory) {
  if (url.rfind("file://", 0) == 0) {
    std::string folder = url.substr(0, url.find_first_of("?#"));
    if (!directory) folder = folder.substr(0, folder.rfind('/') + 1);
    if (folder.empty() || folder.back() != '/') folder.push_back('/');
    return folder;
  }
  for (const char *scheme : {"http://", "https://"}) {
    const std::string start(scheme);
    if (url.rfind(start, 0) != 0) continue;
    const std::size_t end = url.find_first_of("/?#", start.size());
    return url.substr(0, end == std::string::npos ? url.size() : end) + "/";
  }
  return {};
}

} // namespace singularity::platform
