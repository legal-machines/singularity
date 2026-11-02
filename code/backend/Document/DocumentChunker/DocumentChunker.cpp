#include "Document/DocumentChunker/DocumentChunker.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace singularity::documents {
namespace {

constexpr std::size_t kMinimumUtf8Bound = 4;

bool is_utf8_continuation(unsigned char value) {
  return (value & 0xc0U) == 0x80U;
}

std::size_t utf8_code_point_count(std::string_view text) {
  return static_cast<std::size_t>(std::count_if(
      text.begin(), text.end(), [](char value) {
        return !is_utf8_continuation(static_cast<unsigned char>(value));
      }));
}

std::size_t estimated_token_count(std::string_view text) {
  const auto code_points = utf8_code_point_count(text);
  return std::max<std::size_t>(1, (code_points + 3) / 4);
}

void sort_and_deduplicate(std::vector<std::size_t> &offsets,
                          std::size_t text_size) {
  offsets.erase(std::remove_if(offsets.begin(), offsets.end(),
                               [text_size](std::size_t offset) {
                                 return offset == 0 || offset >= text_size;
                               }),
                offsets.end());
  std::sort(offsets.begin(), offsets.end());
  offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
}

std::vector<std::size_t> line_starts(std::string_view text) {
  std::vector<std::size_t> starts{0};
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\n' && index + 1 < text.size()) {
      starts.push_back(index + 1);
    }
  }
  return starts;
}

struct Boundaries {
  std::vector<std::size_t> structural;
  std::vector<std::size_t> lines;
  std::vector<std::size_t> sentences;
  std::vector<std::size_t> whitespace;
};

Boundaries collect_boundaries(const DocumentExtractionResult &extraction) {
  Boundaries result;
  const auto text_size = extraction.text.size();

  for (const auto &page : extraction.structure.page_boundaries) {
    result.structural.push_back(page.text_offset);
  }

  const auto starts = line_starts(extraction.text);
  for (const auto &heading : extraction.structure.headings) {
    if (heading.line > 0 && heading.line <= starts.size()) {
      result.structural.push_back(starts[heading.line - 1]);
    }
  }

  for (std::size_t index = 0; index < text_size; ++index) {
    const auto value = extraction.text[index];
    if (value == '\n') {
      result.lines.push_back(index + 1);
    }
    const bool sentence_end = value == '.' || value == '?' || value == '!';
    if (sentence_end &&
        (index + 1 == text_size || extraction.text[index + 1] == ' ' ||
         extraction.text[index + 1] == '\t' ||
         extraction.text[index + 1] == '\n')) {
      auto boundary = index + 1;
      while (boundary < text_size &&
             (extraction.text[boundary] == ' ' ||
              extraction.text[boundary] == '\t')) {
        ++boundary;
      }
      result.sentences.push_back(boundary);
    }
    if (value == ' ' || value == '\t') {
      result.whitespace.push_back(index + 1);
    }
  }

  sort_and_deduplicate(result.structural, text_size);
  sort_and_deduplicate(result.lines, text_size);
  sort_and_deduplicate(result.sentences, text_size);
  sort_and_deduplicate(result.whitespace, text_size);
  return result;
}

std::optional<std::size_t>
first_after(const std::vector<std::size_t> &offsets, std::size_t start) {
  const auto found = std::upper_bound(offsets.begin(), offsets.end(), start);
  if (found == offsets.end()) {
    return std::nullopt;
  }
  return *found;
}

std::optional<std::size_t>
last_at_or_before(const std::vector<std::size_t> &offsets, std::size_t start,
                  std::size_t limit) {
  auto found = std::upper_bound(offsets.begin(), offsets.end(), limit);
  if (found == offsets.begin()) {
    return std::nullopt;
  }
  --found;
  if (*found <= start) {
    return std::nullopt;
  }
  return *found;
}

std::size_t utf8_safe_limit(std::string_view text, std::size_t start,
                            std::size_t proposed) {
  auto result = std::min(proposed, text.size());
  while (result > start && result < text.size() &&
         is_utf8_continuation(static_cast<unsigned char>(text[result]))) {
    --result;
  }
  return result;
}

std::size_t choose_end(std::string_view text, const Boundaries &boundaries,
                       std::size_t start, std::size_t maximum_bytes) {
  const auto hard_end =
      utf8_safe_limit(text, start, std::min(text.size(), start + maximum_bytes));

  // Page and heading starts are hard logical boundaries when reachable within
  // the current chunk; this prevents page/section metadata from becoming
  // ambiguous. Within that range, prefer the latest softer boundary.
  if (const auto structural = first_after(boundaries.structural, start);
      structural.has_value() && *structural <= hard_end) {
    return *structural;
  }
  if (hard_end == text.size()) {
    return hard_end;
  }
  for (const auto *candidates : {&boundaries.lines, &boundaries.sentences,
                                 &boundaries.whitespace}) {
    if (const auto selected = last_at_or_before(*candidates, start, hard_end)) {
      return *selected;
    }
  }
  if (hard_end <= start) {
    throw std::runtime_error("Unable to split document at a UTF-8 boundary");
  }
  return hard_end;
}

std::optional<std::size_t>
page_at(const DocumentStructure &structure, std::size_t source_offset) {
  std::optional<std::size_t> page;
  std::size_t best_offset = 0;
  bool found = false;
  for (const auto &boundary : structure.page_boundaries) {
    if (boundary.text_offset <= source_offset &&
        (!found || boundary.text_offset >= best_offset)) {
      page = boundary.page_number;
      best_offset = boundary.text_offset;
      found = true;
    }
  }
  return page;
}

std::optional<std::string>
heading_at(const DocumentExtractionResult &extraction,
           const std::vector<std::size_t> &starts, std::size_t source_offset) {
  std::optional<std::string> heading;
  std::size_t best_offset = 0;
  bool found = false;
  for (const auto &candidate : extraction.structure.headings) {
    if (candidate.line == 0 || candidate.line > starts.size()) {
      continue;
    }
    const auto offset = starts[candidate.line - 1];
    if (offset <= source_offset && (!found || offset >= best_offset)) {
      heading = candidate.title;
      best_offset = offset;
      found = true;
    }
  }
  return heading;
}

} // namespace

std::vector<DocumentChunk>
chunk_document(const std::string &document_id,
               const std::string &source_fingerprint,
               const DocumentExtractionResult &extraction,
               const DocumentChunkOptions &options) {
  if (document_id.empty()) {
    throw std::invalid_argument("Document ID must not be empty");
  }
  if (source_fingerprint.empty()) {
    throw std::invalid_argument("Source fingerprint must not be empty");
  }
  if (options.maximum_bytes < kMinimumUtf8Bound) {
    throw std::invalid_argument("Chunk maximum must be at least four bytes");
  }
  if (!extraction.ai_ready() || extraction.text.empty()) {
    return {};
  }

  const auto boundaries = collect_boundaries(extraction);
  const auto starts = line_starts(extraction.text);
  std::vector<DocumentChunk> chunks;
  std::size_t source_start = 0;
  while (source_start < extraction.text.size()) {
    const auto source_end = choose_end(extraction.text, boundaries, source_start,
                                       options.maximum_bytes);
    auto text = extraction.text.substr(source_start, source_end - source_start);
    chunks.push_back({document_id,
                      document_id + ":" + source_fingerprint + ":" +
                          std::to_string(source_start) + "-" +
                          std::to_string(source_end),
                      std::move(text),
                      page_at(extraction.structure, source_start),
                      heading_at(extraction, starts, source_start),
                      source_start,
                      source_end,
                      0});
    chunks.back().estimated_size = estimated_token_count(chunks.back().text);
    chunks.back().extractor_version = extraction.extractor_version;
    source_start = source_end;
  }
  return chunks;
}

} // namespace singularity::documents
