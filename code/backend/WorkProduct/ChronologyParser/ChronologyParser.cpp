#include "WorkProduct/ChronologyParser/ChronologyParser.hpp"

#include <array>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace singularity::context {
namespace {

constexpr std::string_view kBlockOpen = "[EVENT]";
constexpr std::string_view kBlockClose = "[/EVENT]";

// Recognized field keys, lowercase, with their trailing colon.
constexpr std::array<std::string_view, 3> kFieldKeys = {
    "date:", "description:", "sources:",
};

std::string trim(std::string_view value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) return {};
  const auto end = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(begin, end - begin + 1));
}

std::string to_lower(std::string_view value) {
  std::string result(value);
  for (auto &character : result) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return result;
}

// If `line` begins with one of the recognized field keys (case-insensitive),
// returns that key without its trailing colon and the rest of the line.
std::optional<std::pair<std::string, std::string>> match_field(const std::string &line) {
  const auto lower = to_lower(line);
  for (const auto &key : kFieldKeys) {
    if (lower.size() >= key.size() && lower.compare(0, key.size(), key) == 0) {
      return std::make_pair(std::string(key.substr(0, key.size() - 1)),
                            line.substr(key.size()));
    }
  }
  return std::nullopt;
}

// Splits an "[EVENT]" block body into its recognized key/value fields. A
// line that does not start a new recognized key extends the current key's
// value on a new line - so a multi-sentence Description is not truncated at
// its first line break.
std::unordered_map<std::string, std::string> parse_fields(const std::string &body) {
  std::unordered_map<std::string, std::string> values;
  std::string current_key;
  std::size_t line_start = 0;
  while (line_start <= body.size()) {
    auto line_end = body.find('\n', line_start);
    const bool last_line = line_end == std::string::npos;
    if (last_line) line_end = body.size();
    const auto line = body.substr(line_start, line_end - line_start);

    if (auto field = match_field(line)) {
      current_key = field->first;
      values[current_key] = trim(field->second);
    } else if (!current_key.empty()) {
      const auto continuation = trim(line);
      if (!continuation.empty()) {
        auto &existing = values[current_key];
        if (!existing.empty()) existing += "\n";
        existing += continuation;
      }
    }

    if (last_line) break;
    line_start = line_end + 1;
  }
  return values;
}

std::optional<std::string> optional_field(
    const std::unordered_map<std::string, std::string> &values, const char *key) {
  const auto found = values.find(key);
  if (found == values.end() || found->second.empty()) return std::nullopt;
  return found->second;
}

} // namespace

std::vector<ParsedEvent>
parse_chronology(const std::string &response_text, const SourceMarkerRegistry &supplied_markers) {
  std::vector<ParsedEvent> events;
  std::size_t cursor = 0;
  while (true) {
    const auto open = response_text.find(kBlockOpen, cursor);
    if (open == std::string::npos) break;
    const auto body_start = open + kBlockOpen.size();
    const auto close = response_text.find(kBlockClose, body_start);
    if (close == std::string::npos) {
      // Unterminated block: nothing after this point can be trusted as a
      // deliberate, complete Event - stop rather than guess.
      break;
    }
    const auto body = response_text.substr(body_start, close - body_start);
    cursor = close + kBlockClose.size();

    const auto fields = parse_fields(body);
    const auto description = optional_field(fields, "description");
    if (!description) {
      // Not a valid Event block - skip it silently. The response text
      // itself is never touched, so nothing useful is lost (Master Spec
      // Sec.73: store/display as structured chronology only when parsing
      // succeeds).
      continue;
    }

    ParsedEvent event;
    // Preserved exactly as supplied - never parsed into a calendar type,
    // never normalized to a more precise form, never fabricated when
    // absent (Master Spec Sec.73: "No fake dates").
    event.date = optional_field(fields, "date");
    event.description = *description;
    if (const auto sources = fields.find("sources"); sources != fields.end()) {
      event.source_refs =
          cited_references(supplied_markers.validate_response(sources->second));
    }
    events.push_back(std::move(event));
  }
  return events;
}

} // namespace singularity::context
