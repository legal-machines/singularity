#include "Provider/ModelClassification/ModelClassification.hpp"

#include <algorithm>
#include <cctype>

namespace singularity::providers {
namespace {

std::string to_lower(const std::string &value) {
  std::string result = value;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return result;
}

bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

} // namespace

std::optional<std::string> classify_model_exclusion(const std::string &model_id) {
  const auto lowered = to_lower(model_id);
  // Well-known, stable provider naming conventions - every provider in
  // this registry (and every OpenAI-compatible gateway modeled on the same
  // wire format) names embedding/moderation/audio/image models this way.
  // This is a pattern heuristic over naming conventions, not a frozen list
  // of specific model ids: it keeps working as new chat models ship and
  // new non-text model ids appear, as long as the provider keeps naming
  // modalities the way every provider observed so far does.
  if (contains(lowered, "embedding")) return "embedding";
  if (contains(lowered, "moderation")) return "moderation";
  if (contains(lowered, "whisper") || contains(lowered, "transcribe") || contains(lowered, "tts") ||
      contains(lowered, "audio")) {
    return "audio";
  }
  if (contains(lowered, "dall-e") || contains(lowered, "dalle") || contains(lowered, "image")) {
    return "image";
  }
  return std::nullopt;
}

} // namespace singularity::providers
