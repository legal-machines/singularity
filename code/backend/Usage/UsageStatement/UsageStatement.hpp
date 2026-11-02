#pragma once

#include "Usage/UsageAccounting/UsageAccounting.hpp"

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace singularity::usage {

enum class StructuredStatementFormat { Json, Csv };

struct StatementBundleResult {
  std::filesystem::path pdf_path;
  std::filesystem::path structured_path;
  std::string structured_format;
  std::string generated_at;
  std::string content_hash;
  long long operation_count = 0;
};

[[nodiscard]] StructuredStatementFormat
structured_statement_format_from_string(const std::string& value);
[[nodiscard]] nlohmann::json build_statement(const UsageSummary& summary,
                                             const std::string& project_id,
                                             const std::string& generated_at);
[[nodiscard]] bool verify_statement_hash(const nlohmann::json& statement);

// Writes one PDF and one structured companion from the same in-memory
// statement and therefore the same manifest/content hash. Existing files are
// never overwritten. Generated artifacts are exports, not Project state.
[[nodiscard]] StatementBundleResult
write_statement_bundle(const std::filesystem::path& destination_directory,
                       const nlohmann::json& statement,
                       StructuredStatementFormat structured_format);

} // namespace singularity::usage
