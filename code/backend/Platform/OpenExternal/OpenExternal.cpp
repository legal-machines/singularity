#include "Platform/OpenExternal/OpenExternal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// shellapi.h depends on types and macros declared by windows.h.
#include <shellapi.h>

#include "Platform/WindowsText/WindowsText.hpp"
#else
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;
#endif

namespace singularity::platform {
namespace {

bool is_https(const std::string &url) { return url.rfind("https://", 0) == 0; }

#if defined(_WIN32)
// Empty for text that is not valid UTF-8: nothing is opened then.
std::wstring utf8_to_wide(const std::string &value) {
  try {
    return windows::utf8_to_wide(value);
  } catch (const std::invalid_argument &) {
    return {};
  }
}
#endif

} // namespace

bool open_external_https(const std::string &url) {
  if (!is_https(url)) {
    return false;
  }
#if defined(_WIN32)
  const auto wide_url = utf8_to_wide(url);
  if (wide_url.empty()) {
    return false;
  }
  const auto result = reinterpret_cast<std::intptr_t>(
      ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL));
  return result > 32;
#else
#if defined(__APPLE__)
  const char *program = "/usr/bin/open";
#else
  const char *program = "xdg-open";
#endif
  char *arguments[] = {const_cast<char *>(program),
                       const_cast<char *>(url.c_str()), nullptr};
  pid_t child = 0;
  if (posix_spawnp(&child, program, nullptr, nullptr, arguments, environ) != 0) {
    return false;
  }
  std::thread([child] {
    int status = 0;
    waitpid(child, &status, 0);
  }).detach();
  return true;
#endif
}

namespace {

// Every byte but the unreserved ones, percent-encoded (RFC 3986, RFC 6068).
std::string percent_encoded(const std::string &text) {
  static const char *digits = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char character : text) {
    if (std::isalnum(character) != 0 || character == '-' || character == '.' || character == '_' || character == '~') {
      out += static_cast<char>(character);
    } else {
      out += '%';
      out += digits[character >> 4];
      out += digits[character & 0x0F];
    }
  }
  return out;
}

// Line breaks as mail wants them: CR LF.
std::string with_crlf(const std::string &text) {
  std::string out;
  for (std::size_t at = 0; at < text.size(); ++at) {
    if (text[at] == '\r') continue;
    if (text[at] == '\n') out += "\r\n";
    else out += text[at];
  }
  return out;
}

// The text cut at a character's boundary, never inside a UTF-8 sequence.
std::string cut_utf8(const std::string &text, std::size_t most) {
  if (text.size() <= most) return text;
  std::size_t at = most;
  while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
  return text.substr(0, at);
}

constexpr std::size_t kMostMailLink = 1900;

bool open_link(const std::string &link) {
#if defined(_WIN32)
  const auto wide = utf8_to_wide(link);
  if (wide.empty()) return false;
  const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
#else
#if defined(__APPLE__)
  const char *program = "/usr/bin/open";
#else
  const char *program = "xdg-open";
#endif
  char *arguments[] = {const_cast<char *>(program), const_cast<char *>(link.c_str()), nullptr};
  pid_t child = 0;
  if (posix_spawnp(&child, program, nullptr, nullptr, arguments, environ) != 0) return false;
  std::thread([child] {
    int status = 0;
    waitpid(child, &status, 0);
  }).detach();
  return true;
#endif
}

} // namespace

bool is_plain_mail_address(const std::string &address) {
  if (address.empty() || address.size() > 254) return false;
  const std::size_t at = address.find('@');
  if (at == std::string::npos || at == 0 || at + 1 >= address.size() || address.find('@', at + 1) != std::string::npos) return false;
  for (const char character : address) {
    const auto code = static_cast<unsigned char>(character);
    if (code <= 0x20 || code == 0x7F) return false;
    switch (character) {
      case '?': case '&': case '#': case '%': case ',': case ';': case '<': case '>': case '"': case '\'': case '`':
      case '\\': case '(': case ')': case '[': case ']': case ':':
        return false;
      default:
        break;
    }
  }
  return true;
}

MailCompose mail_compose_link(const std::string &to, const std::string &subject, const std::string &body) {
  MailCompose compose;
  if (!is_plain_mail_address(to)) return compose;
  const std::string head = "mailto:" + percent_encoded(to) + "?subject=" + percent_encoded(with_crlf(cut_utf8(subject, 300)));
  const std::string with_body = head + "&body=" + percent_encoded(with_crlf(body));
  if (with_body.size() <= kMostMailLink) {
    compose.link = with_body;
    compose.carries_body = true;
  } else {
    compose.link = head;
  }
  return compose;
}

MailComposed open_mail_compose(const std::string &to, const std::string &subject, const std::string &body) {
  MailComposed composed;
  const MailCompose compose = mail_compose_link(to, subject, body);
  if (compose.link.rfind("mailto:", 0) != 0) return composed;
  composed.carries_body = compose.carries_body;
  composed.opened = open_link(compose.link);
  return composed;
}

const char *file_manager_name() {
#if defined(_WIN32)
  return "File Explorer";
#elif defined(__APPLE__)
  return "Finder";
#else
  return "the file manager";
#endif
}

bool reveal_in_file_manager(const std::string &path) {
#if defined(_WIN32)
  const auto wide_path = utf8_to_wide(path);
  if (wide_path.empty()) {
    return false;
  }
  const std::wstring arguments = L"/select,\"" + wide_path + L"\"";
  const auto result = reinterpret_cast<std::intptr_t>(
      ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(),
                    nullptr, SW_SHOWNORMAL));
  return result > 32;
#else
#if defined(__APPLE__)
  const char *program = "/usr/bin/open";
  const std::string target = path;
  char *arguments[] = {const_cast<char *>(program), const_cast<char *>("-R"),
                       const_cast<char *>(target.c_str()), nullptr};
#else
  const char *program = "xdg-open";
  const std::string target = std::filesystem::path(path).parent_path().string();
  char *arguments[] = {const_cast<char *>(program),
                       const_cast<char *>(target.c_str()), nullptr};
#endif
  pid_t child = 0;
  if (posix_spawnp(&child, program, nullptr, nullptr, arguments, environ) != 0) {
    return false;
  }
  std::thread([child] {
    int status = 0;
    waitpid(child, &status, 0);
  }).detach();
  return true;
#endif
}

namespace {

std::string lowercase_extension(const std::filesystem::path &path) {
  std::string extension = path.extension().string();
  if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
      [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return extension;
}

} // namespace

bool may_open_as_document(const std::string& path) {
  static const std::set<std::string> documents{
      "pdf", "doc", "docx", "dot", "dotx", "rtf", "odt", "pages", "txt", "text", "md", "markdown", "csv", "tsv",
      "xls", "xlsx", "xlt", "xltx", "ods", "numbers", "ppt", "pptx", "pot", "potx", "odp", "key", "json", "xml",
      "html", "htm", "png", "jpg", "jpeg", "gif", "tif", "tiff", "heic", "bmp", "webp", "msg", "eml", "emlx",
  };
  std::error_code error;
  // The path is UTF-8, as every caller's is: read as such on every system,
  // never in Windows' own code page, where a name outside it - a member's
  // Cyrillic folder - would not be found and so never opened.
  const std::filesystem::path given = std::filesystem::u8path(path);
  const std::filesystem::path real = std::filesystem::canonical(given, error);
  if (error || !std::filesystem::is_regular_file(real, error) || error) return false;
  return documents.count(lowercase_extension(real)) > 0
      && documents.count(lowercase_extension(given)) > 0;
}

bool is_launchable_package(const std::string& path) {
  static const std::set<std::string> packages{
      "app", "bundle", "framework", "plugin", "kext", "prefpane", "saver", "qlgenerator", "pkg", "mpkg", "workflow",
      "action", "service", "appex", "xpc", "mdimporter", "wdgt", "scptd", "osax",
  };
  std::error_code error;
  const std::filesystem::path real = std::filesystem::weakly_canonical(std::filesystem::path(path), error);
  return packages.count(lowercase_extension(std::filesystem::path(path))) > 0
      || (!error && packages.count(lowercase_extension(real)) > 0);
}

bool open_directory_in_file_manager(const std::string &path) {
  std::error_code error;
  if (!std::filesystem::is_directory(path, error) || error) return false;
  // A package is launched by "open": shown in its folder instead.
  if (is_launchable_package(path)) return reveal_in_file_manager(path);
#if defined(_WIN32)
  const auto wide_path = utf8_to_wide(path);
  if (wide_path.empty()) return false;
  const auto result = reinterpret_cast<std::intptr_t>(
      ShellExecuteW(nullptr, L"open", wide_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
#else
#if defined(__APPLE__)
  const char *program = "/usr/bin/open";
#else
  const char *program = "xdg-open";
#endif
  const std::string target = path;
  char *arguments[] = {const_cast<char *>(program), const_cast<char *>(target.c_str()), nullptr};
  pid_t child = 0;
  if (posix_spawnp(&child, program, nullptr, nullptr, arguments, environ) != 0) return false;
  std::thread([child] { int status = 0; waitpid(child, &status, 0); }).detach();
  return true;
#endif
}

bool open_in_default_application(const std::string& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return false;
  // Only a document is handed over; a script, an app or anything else would run.
  if (!may_open_as_document(path)) return false;
#if defined(_WIN32)
  const auto wide_path = utf8_to_wide(path);
  if (wide_path.empty()) return false;
  const auto result = reinterpret_cast<std::intptr_t>(
      ShellExecuteW(nullptr, L"open", wide_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
#else
#if defined(__APPLE__)
  const char* program = "/usr/bin/open";
#else
  const char* program = "xdg-open";
#endif
  const std::string target = path;
  char* arguments[] = {const_cast<char*>(program), const_cast<char*>(target.c_str()), nullptr};
  pid_t child = 0;
  if (posix_spawnp(&child, program, nullptr, nullptr, arguments, environ) != 0) return false;
  std::thread([child] { int status = 0; waitpid(child, &status, 0); }).detach();
  return true;
#endif
}

} // namespace singularity::platform
