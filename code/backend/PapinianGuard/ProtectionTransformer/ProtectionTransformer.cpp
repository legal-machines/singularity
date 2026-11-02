#include "PapinianGuard/ProtectionTransformer/ProtectionTransformer.hpp"

#include "PapinianGuard/DeterministicDetector/DeterministicDetector.hpp"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace singularity::protected_api {
namespace {

std::string to_lower_ascii(const std::string &value) {
  std::string result = value;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::string placeholder_prefix(const std::string &label) {
  if (label == to_string(Category::MonetaryAmounts)) return "AMOUNT";
  if (label == to_string(Category::Emails)) return "EMAIL";
  if (label == to_string(Category::PhoneNumbers)) return "PHONE";
  if (label == to_string(Category::Urls)) return "URL";
  if (label == to_string(Category::Identifiers)) return "ID";
  return "TERM";
}

} // namespace

TransformResult transform(const std::string &text, const Policy &policy) {
  TransformResult result;
  const auto matches = detect(text, policy);
  if (matches.empty()) {
    result.transformed_text = text;
    return result;
  }

  // The same original value (case-insensitively) always maps to the same
  // placeholder within this call, so a repeated email address or custom
  // term collapses to one placeholder instead of a new one per occurrence.
  std::unordered_map<std::string, std::string> value_to_placeholder;
  std::unordered_map<std::string, int> counters;

  std::string output;
  output.reserve(text.size());
  std::size_t cursor = 0;

  for (const auto &match : matches) {
    output.append(text, cursor, match.start - cursor);

    const std::string prefix = placeholder_prefix(match.label);
    const std::string dedupe_key = match.label + "\x1f" + to_lower_ascii(match.value);

    const auto existing = value_to_placeholder.find(dedupe_key);
    std::string placeholder;
    if (existing != value_to_placeholder.end()) {
      placeholder = existing->second;
    } else {
      const int next = ++counters[prefix];
      placeholder = prefix + "_" + std::to_string(next);
      value_to_placeholder.emplace(dedupe_key, placeholder);
      result.mapping.emplace(placeholder, match.value);
    }

    output.append(placeholder);
    cursor = match.start + match.length;
  }
  output.append(text, cursor, text.size() - cursor);

  result.transformed_text = std::move(output);
  return result;
}

std::string restore(const std::string &text, const std::unordered_map<std::string, std::string> &mapping) {
  if (mapping.empty()) return text;

  // Longest placeholder first, so e.g. "AMOUNT_10" is fully replaced before
  // "AMOUNT_1" is searched for - otherwise the latter would match as a
  // prefix of the former and corrupt the restored text.
  std::vector<std::pair<std::string, std::string>> entries(mapping.begin(), mapping.end());
  std::sort(entries.begin(), entries.end(),
           [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });

  std::string output = text;
  for (const auto &[placeholder, original] : entries) {
    std::size_t position = 0;
    while ((position = output.find(placeholder, position)) != std::string::npos) {
      output.replace(position, placeholder.size(), original);
      position += original.size();
    }
  }
  return output;
}

} // namespace singularity::protected_api
