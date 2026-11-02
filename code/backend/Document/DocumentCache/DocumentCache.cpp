#include "Platform/Trace/Trace.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Document/DocumentCache/DocumentCache.hpp"

#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <random>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace singularity::documents {
namespace {

using Json = nlohmann::json;

std::string hex_encode(const std::string &value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    encoded.push_back(digits[byte >> 4]);
    encoded.push_back(digits[byte & 0x0f]);
  }
  return encoded;
}

std::filesystem::path cache_path(const std::filesystem::path &workspace_path,
                                 const std::string &fingerprint,
                                 const std::string &extractor_version) {
  return workspace_path / ".singularity-workspace" / "derivatives" / "documents" /
         fingerprint / ("extractor-" + hex_encode(extractor_version) + ".json");
}

std::filesystem::path unique_temporary_path(const std::filesystem::path &path) {
  std::random_device seed;
  std::mt19937_64 generator(seed());
  return path.string() + ".tmp-" + std::to_string(generator());
}

void atomic_replace(const std::filesystem::path &source, const std::filesystem::path &destination) {
  if (const auto failure = platform::replace_file(source, destination))
    throw std::runtime_error("Unable to replace document cache: " + *failure);
}

Json extraction_to_json(const DocumentExtractionResult &result,
                        const std::string &fingerprint) {
  Json pages = Json::array();
  for (const auto &page : result.structure.page_boundaries) {
    pages.push_back({{"pageNumber", page.page_number},
                     {"textOffset", page.text_offset},
                     {"textLength", page.text_length}});
  }
  Json headings = Json::array();
  for (const auto &heading : result.structure.headings) {
    headings.push_back({{"line", heading.line},
                        {"level", heading.level},
                        {"title", heading.title}});
  }
  Json diagnostics = Json::array();
  for (const auto &diagnostic : result.diagnostics) {
    diagnostics.push_back({{"code", diagnostic.code},
                           {"message", diagnostic.message}});
  }
  return Json{
      {"version", 1},
      {"fingerprint", fingerprint},
      {"format", to_string(result.format)},
      {"status", to_string(result.status)},
      {"extractorVersion", result.extractor_version},
      {"text", result.text},
      {"structure", {{"byteCount", result.structure.byte_count},
                       {"lineCount", result.structure.line_count},
                       {"paragraphCount", result.structure.paragraph_count},
                       {"pageCount", result.structure.page_count ? Json(*result.structure.page_count) : Json(nullptr)},
                       {"pageBoundaries", std::move(pages)},
                       {"headings", std::move(headings)}}},
      {"diagnostics", std::move(diagnostics)},
  };
}

DocumentFormat format_from_string(const std::string &value) {
  return document_format_from_string(value).value_or(DocumentFormat::unknown);
}

ExtractionStatus status_from_string(const std::string &value) {
  if (value == "ready") return ExtractionStatus::ready;
  if (value == "empty") return ExtractionStatus::empty;
  if (value == "ocr_required") return ExtractionStatus::ocr_required;
  if (value == "unreadable") return ExtractionStatus::unreadable;
  return ExtractionStatus::unsupported;
}

std::optional<DocumentExtractionResult>
extraction_from_json(const Json &value, const std::string &fingerprint,
                     const std::string &extractor_version) {
  if (!value.is_object() || value.value("version", 0) != 1 ||
      value.value("fingerprint", std::string()) != fingerprint ||
      value.value("extractorVersion", std::string()) != extractor_version ||
      !value.contains("structure") || !value.at("structure").is_object()) {
    return std::nullopt;
  }

  DocumentExtractionResult result;
  result.format = format_from_string(value.value("format", std::string()));
  result.status = status_from_string(value.value("status", std::string()));
  result.extractor_version = extractor_version;
  result.text = value.value("text", std::string());
  const auto &structure = value.at("structure");
  result.structure.byte_count = structure.value("byteCount", std::size_t{0});
  result.structure.line_count = structure.value("lineCount", std::size_t{0});
  result.structure.paragraph_count = structure.value("paragraphCount", std::size_t{0});
  if (structure.contains("pageCount") && structure.at("pageCount").is_number_unsigned()) {
    result.structure.page_count = structure.at("pageCount").get<std::size_t>();
  }
  if (structure.contains("pageBoundaries") && structure.at("pageBoundaries").is_array()) {
    for (const auto &page : structure.at("pageBoundaries")) {
      result.structure.page_boundaries.push_back(
          {page.value("pageNumber", std::size_t{1}),
           page.value("textOffset", std::size_t{0}),
           page.value("textLength", std::size_t{0})});
    }
  }
  if (structure.contains("headings") && structure.at("headings").is_array()) {
    for (const auto &heading : structure.at("headings")) {
      result.structure.headings.push_back(
          {heading.value("line", std::size_t{0}),
           heading.value("level", 0U),
           heading.value("title", std::string())});
    }
  }
  if (value.contains("diagnostics") && value.at("diagnostics").is_array()) {
    for (const auto &diagnostic : value.at("diagnostics")) {
      result.diagnostics.push_back(
          {diagnostic.value("code", std::string()),
           diagnostic.value("message", std::string())});
    }
  }
  return result;
}

void write_cache(const std::filesystem::path &path, const Json &value) {
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = unique_temporary_path(path);
  try {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Unable to write document cache.");
    file << value.dump();
    file.flush();
    if (!file) throw std::runtime_error("Unable to flush document cache.");
    file.close();
    if (file.fail()) throw std::runtime_error("Unable to close document cache.");
    atomic_replace(temporary, path);
  } catch (...) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    throw;
  }
}

} // namespace

DocumentCache::DocumentCache(std::filesystem::path workspace_path,
                             const DocumentExtractor &extractor)
    : workspace_path_(std::move(workspace_path)), extractor_(extractor) {}

PreparedDocument DocumentCache::prepare(
    const std::filesystem::path &source_path, DocumentFormat format,
    const std::string &extractor_version) const {
  std::error_code size_error;
  const auto size = std::filesystem::file_size(source_path, size_error);
  if (size_error) {
    throw std::runtime_error("Unable to inspect document size: " + source_path.string());
  }

  PreparedDocument prepared;
  prepared.size_bytes = static_cast<long long>(size);
  prepared.last_modified = file_last_modified_token(source_path);
  prepared.fingerprint = platform::sha256_hex_digest_of_file(source_path);
  if (const auto cached = load(prepared.fingerprint, extractor_version)) {
    prepared.extraction = *cached;
    prepared.cache_hit = true;
    return prepared;
  }

  prepared.extraction = extractor_.extract({source_path, format});
  if (prepared.extraction.extractor_version.empty()) {
    prepared.extraction.extractor_version = extractor_version;
  }
  if (prepared.extraction.extractor_version != extractor_version) {
    throw std::runtime_error("Document extractor version changed during extraction.");
  }
  // Extraction re-reads the path after hashing; bytes that changed in between
  // must never be stored under the earlier content hash.
  if (platform::sha256_hex_digest_of_file(source_path) != prepared.fingerprint) {
    throw std::runtime_error("Document changed while it was being read: " + source_path.string());
  }
  write_cache(cache_path(workspace_path_, prepared.fingerprint, extractor_version),
              extraction_to_json(prepared.extraction, prepared.fingerprint));
  return prepared;
}

std::optional<DocumentExtractionResult> DocumentCache::load(
    const std::string &fingerprint,
    const std::string &extractor_version) const {
  if (fingerprint.empty() || extractor_version.empty()) return std::nullopt;
  platform::trace::Span span("evidence", "document_cache.load");
  std::ifstream file(cache_path(workspace_path_, fingerprint, extractor_version),
                     std::ios::binary);
  if (!file) return std::nullopt;
  try {
    return extraction_from_json(Json::parse(file), fingerprint, extractor_version);
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

bool DocumentCache::holds(const std::string &fingerprint,
                          const std::string &extractor_version) const {
  if (fingerprint.empty() || extractor_version.empty()) return false;
  std::error_code error;
  const auto size = std::filesystem::file_size(
      cache_path(workspace_path_, fingerprint, extractor_version), error);
  return !error && size > 0;
}

std::string file_last_modified_token(const std::filesystem::path &path) {
  std::error_code time_error;
  const auto time = std::filesystem::last_write_time(path, time_error);
  if (time_error) {
    throw std::runtime_error("Unable to inspect document modification time: " +
                             path.string());
  }
  return std::to_string(
      static_cast<long long>(time.time_since_epoch().count()));
}

} // namespace singularity::documents
