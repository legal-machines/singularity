#include "Usage/UsageStatement/UsageStatement.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace singularity::usage {
namespace {

using Json = nlohmann::json;

Json optional_string_json(const std::optional<std::string>& value) {
  return value.has_value() ? Json(*value) : Json(nullptr);
}

Json optional_integer_json(const std::optional<long long>& value) {
  return value.has_value() ? Json(*value) : Json(nullptr);
}

Json operation_json(const OperationUsageEvidence& operation) {
  return Json{
      {"projectId", operation.project_id},
      {"operationId", operation.operation_id},
      {"taskId", operation.task_id},
      {"attempt", operation.attempt},
      {"time", operation.timestamp},
      {"providerId", optional_string_json(operation.provider_id)},
      {"modelId", optional_string_json(operation.model_id)},
      {"executionKind", operation.execution_kind},
      {"operationType", operation.operation_type},
      {"operationRole", operation.operation_role},
      {"operationStatus", operation.operation_status},
      {"chatId", optional_string_json(operation.chat_id)},
      {"consiliumId", optional_string_json(operation.consilium_id)},
      {"workflowRunId", optional_string_json(operation.workflow_run_id)},
      {"workProductIds", operation.work_product_ids},
      {"privacy",
       {
           {"mode", operation.privacy_mode},
           {"protectedExternal", operation.protected_external},
       }},
      {"usage",
       {
           {"inputTokens", optional_integer_json(operation.usage.input_tokens)},
           {"outputTokens", optional_integer_json(operation.usage.output_tokens)},
           {"totalTokens", optional_integer_json(operation.usage.total_tokens)},
       }},
      {"cost",
       {
           {"classification", to_string(operation.cost_classification)},
           {"amountMicros", optional_integer_json(operation.usage.cost_micros)},
           {"currency", optional_string_json(operation.usage.cost_currency)},
           {"methodology", optional_string_json(operation.usage.cost_methodology)},
       }},
      {"evidenceStatus", operation.evidence_status},
  };
}

Json totals_json(const UsageSummary& summary) {
  Json currencies = Json::array();
  for (const CurrencyTotal& total : summary.currency_totals) {
    currencies.push_back({
        {"currency", total.currency},
        {"actualMicros", optional_integer_json(total.actual_micros)},
        {"actualDisplay", total.actual_micros
                              ? Json(format_cost(*total.actual_micros, total.currency))
                              : Json(nullptr)},
        {"estimatedMicros", optional_integer_json(total.estimated_micros)},
        {"estimatedDisplay", total.estimated_micros
                                 ? Json(format_cost(*total.estimated_micros, total.currency))
                                 : Json(nullptr)},
    });
  }
  return Json{
      {"operationCount", summary.operation_count},
      {"inputTokens", summary.input_tokens},
      {"outputTokens", summary.output_tokens},
      {"totalTokens", summary.total_tokens},
      {"classificationCounts",
       {
           {"actual", summary.actual_count},
           {"estimated", summary.estimated_count},
           {"usageOnly", summary.usage_only_count},
           {"unavailable", summary.unavailable_count},
       }},
      {"currencies", std::move(currencies)},
  };
}

std::string csv_escape(const std::string& value) {
  std::string result = "\"";
  for (char character : value) {
    if (character == '"')
      result += "\"\"";
    else
      result += character;
  }
  result += '"';
  return result;
}

std::string json_scalar_text(const Json& value) {
  if (value.is_null())
    return "";
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_boolean())
    return value.get<bool>() ? "true" : "false";
  return value.dump();
}

void write_csv_row(std::ostringstream& output, const std::vector<std::string>& values) {
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index > 0) output << ',';
    output << csv_escape(values[index]);
  }
  output << '\n';
}

std::string statement_csv(const Json& statement) {
  std::ostringstream output;
  output << "recordType,projectId,operationId,taskId,attempt,time,providerId,modelId,executionKind,"
            "operationType,operationRole,operationStatus,chatId,consiliumId,workflowRunId,"
            "workProductIds,"
            "privacyMode,protectedExternal,inputTokens,outputTokens,totalTokens,costClassification,"
            "costAmountMicros,costCurrency,costMethodology,evidenceStatus,periodStart,periodEnd,"
            "generatedAt,contentHash,operationHash,statementMethodology,manifestSource,"
            "manifestOperationCount,statementTotals\n";
  std::vector<std::string> statement_values(35);
  statement_values[0] = "statement";
  statement_values[1] = statement.at("projectId").get<std::string>();
  statement_values[26] = statement.at("period").at("start").get<std::string>();
  statement_values[27] = statement.at("period").at("end").get<std::string>();
  statement_values[28] = statement.at("generatedAt").get<std::string>();
  statement_values[29] = statement.at("contentHash").get<std::string>();
  statement_values[31] = statement.at("methodology").dump();
  statement_values[32] = statement.at("manifest").at("source").get<std::string>();
  statement_values[33] = json_scalar_text(statement.at("manifest").at("operationCount"));
  statement_values[34] = statement.at("totals").dump();
  write_csv_row(output, statement_values);

  for (std::size_t operation_index = 0;
       operation_index < statement.at("operations").size(); ++operation_index) {
    const Json& operation = statement.at("operations").at(operation_index);
    const Json& privacy = operation.at("privacy");
    const Json& usage = operation.at("usage");
    const Json& cost = operation.at("cost");
    const std::vector<std::string> values{
        "operation",
        json_scalar_text(operation.at("projectId")),
        json_scalar_text(operation.at("operationId")),
        json_scalar_text(operation.at("taskId")),
        json_scalar_text(operation.at("attempt")),
        json_scalar_text(operation.at("time")),
        json_scalar_text(operation.at("providerId")),
        json_scalar_text(operation.at("modelId")),
        json_scalar_text(operation.at("executionKind")),
        json_scalar_text(operation.at("operationType")),
        json_scalar_text(operation.at("operationRole")),
        json_scalar_text(operation.at("operationStatus")),
        json_scalar_text(operation.at("chatId")),
        json_scalar_text(operation.at("consiliumId")),
        json_scalar_text(operation.at("workflowRunId")),
        operation.at("workProductIds").dump(),
        json_scalar_text(privacy.at("mode")),
        json_scalar_text(privacy.at("protectedExternal")),
        json_scalar_text(usage.at("inputTokens")),
        json_scalar_text(usage.at("outputTokens")),
        json_scalar_text(usage.at("totalTokens")),
        json_scalar_text(cost.at("classification")),
        json_scalar_text(cost.at("amountMicros")),
        json_scalar_text(cost.at("currency")),
        json_scalar_text(cost.at("methodology")),
        json_scalar_text(operation.at("evidenceStatus")),
        statement.at("period").at("start").get<std::string>(),
        statement.at("period").at("end").get<std::string>(),
        statement.at("generatedAt").get<std::string>(),
        statement.at("contentHash").get<std::string>(),
        statement.at("manifest").at("operationHashes").at(operation_index).at("sha256")
            .get<std::string>(),
        statement.at("methodology").dump(),
        statement.at("manifest").at("source").get<std::string>(),
        json_scalar_text(statement.at("manifest").at("operationCount")),
        statement.at("totals").dump(),
    };
    write_csv_row(output, values);
  }
  return output.str();
}

std::string pdf_escape(const std::string& value) {
  std::string result;
  for (unsigned char character : value) {
    if (character == '(' || character == ')' || character == '\\')
      result += '\\';
    if (character >= 32 && character <= 126)
      result += static_cast<char>(character);
    else if (character == '\t')
      result += ' ';
    else
      result += '?';
  }
  return result;
}

void append_wrapped(std::vector<std::string>& lines, const std::string& value,
                    std::size_t width = 92) {
  if (value.size() <= width) {
    lines.push_back(value);
    return;
  }
  std::size_t start = 0;
  while (start < value.size()) {
    std::size_t end = std::min(start + width, value.size());
    if (end < value.size()) {
      const std::size_t space = value.rfind(' ', end);
      if (space != std::string::npos && space > start)
        end = space;
    }
    lines.push_back(value.substr(start, end - start));
    start = end;
    while (start < value.size() && value[start] == ' ')
      ++start;
  }
}

// The PDF is read by a person, so each operation's cost and evidence are said
// in words rather than as the statement's codes.
std::string cost_words(const std::string& classification) {
  if (classification == "actual")
    return "Actual";
  if (classification == "estimated")
    return "Estimated";
  if (classification == "usage_only")
    return "Tokens only, no price reported";
  return "Cost unavailable";
}

std::string evidence_words(const std::string& status) {
  if (status == "reconciled")
    return "Confirmed by the routing record";
  if (status == "needs_review")
    return "Differs from the routing record";
  return "Operation record only";
}

std::vector<std::string> statement_pdf_lines(const Json& statement) {
  std::vector<std::string> lines;
  lines.push_back("Matter AI Usage Statement");
  lines.push_back("Billing-support evidence generated by Singularity");
  lines.push_back("");
  lines.push_back("Matter ID: " + statement.at("projectId").get<std::string>());
  lines.push_back("Period: " + statement.at("period").at("start").get<std::string>() + " to " +
                  statement.at("period").at("end").get<std::string>() + " (end exclusive)");
  lines.push_back("Generated: " + statement.at("generatedAt").get<std::string>());
  lines.push_back("Content hash (SHA-256): " + statement.at("contentHash").get<std::string>());
  lines.push_back("");
  lines.push_back("Classification and methodology");
  for (const Json& item : statement.at("methodology"))
    append_wrapped(lines, "- " + item.get<std::string>());
  lines.push_back("");
  const Json& totals = statement.at("totals");
  lines.push_back("Totals");
  lines.push_back(
      "Operations: " + std::to_string(totals.at("operationCount").get<long long>()) +
      " | Input tokens: " + std::to_string(totals.at("inputTokens").get<long long>()) +
      " | Output tokens: " + std::to_string(totals.at("outputTokens").get<long long>()) +
      " | Total tokens: " + std::to_string(totals.at("totalTokens").get<long long>()));
  for (const Json& currency : totals.at("currencies")) {
    std::string row = currency.at("currency").get<std::string>() + ": ";
    row += currency.at("actualDisplay").is_string()
               ? currency.at("actualDisplay").get<std::string>() + " actual"
               : "actual unavailable";
    row += " | ";
    row += currency.at("estimatedDisplay").is_string()
               ? currency.at("estimatedDisplay").get<std::string>() + " estimated"
               : "estimate unavailable";
    lines.push_back(row);
  }
  lines.push_back("");
  lines.push_back("Included operations");
  for (const Json& operation : statement.at("operations")) {
    const Json& usage = operation.at("usage");
    const Json& cost = operation.at("cost");
    const std::string classification = cost.at("classification").get<std::string>();
    std::string first = operation.at("time").get<std::string>() + " | " +
                        operation.at("operationId").get<std::string>() + " | " +
                        json_scalar_text(operation.at("providerId")) + " / " +
                        json_scalar_text(operation.at("modelId")) + " | " +
                        json_scalar_text(operation.at("executionKind"));
    append_wrapped(lines, first);
    std::string second = "  Tokens: " + json_scalar_text(usage.at("inputTokens")) + " input, " +
                         json_scalar_text(usage.at("outputTokens")) + " output, " +
                         json_scalar_text(usage.at("totalTokens")) +
                         " total | Cost: " + cost_words(classification);
    // An amount is printed only when it is the operation's cost: a partial
    // total, some of whose calls came back unpriced, is not.
    if ((classification == "actual" || classification == "estimated") &&
        cost.at("amountMicros").is_number_integer() && cost.at("currency").is_string()) {
      second += " (" +
                format_cost(cost.at("amountMicros").get<long long>(),
                            cost.at("currency").get<std::string>()) +
                ")";
    }
    second += " | Evidence: " + evidence_words(operation.at("evidenceStatus").get<std::string>());
    append_wrapped(lines, second);
  }
  lines.push_back("");
  lines.push_back("Manifest");
  lines.push_back("Operation rows: " +
                  std::to_string(statement.at("manifest").at("operationCount").get<long long>()));
  append_wrapped(lines, "Operation IDs: " + statement.at("manifest").at("operationIds").dump());
  lines.push_back("");
  append_wrapped(lines,
                 "This statement is billing-support evidence. It is not an audit certification, a "
                 "legal conclusion, a claim of admissibility, or a signed record.");
  return lines;
}

std::string build_pdf(const Json& statement) {
  const std::vector<std::string> all_lines = statement_pdf_lines(statement);
  const std::size_t lines_per_page = 48;
  const std::size_t page_count =
      std::max<std::size_t>(1, (all_lines.size() + lines_per_page - 1) / lines_per_page);
  const int font_regular_id = 3;
  const int font_bold_id = 4;
  const int first_page_id = 5;
  std::vector<std::string> objects(5 + page_count * 2);
  objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
  std::ostringstream kids;
  for (std::size_t page = 0; page < page_count; ++page)
    kids << first_page_id + static_cast<int>(page * 2) << " 0 R ";
  objects[2] =
      "<< /Type /Pages /Count " + std::to_string(page_count) + " /Kids [ " + kids.str() + "] >>";
  objects[font_regular_id] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
  objects[font_bold_id] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>";

  for (std::size_t page = 0; page < page_count; ++page) {
    const int page_id = first_page_id + static_cast<int>(page * 2);
    const int content_id = page_id + 1;
    objects[page_id] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                       "/Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " +
                       std::to_string(content_id) + " 0 R >>";
    std::ostringstream content;
    content << "BT\n50 750 Td\n";
    const std::size_t begin = page * lines_per_page;
    const std::size_t end = std::min(begin + lines_per_page, all_lines.size());
    for (std::size_t index = begin; index < end; ++index) {
      const bool is_title = index == 0;
      const bool is_heading =
          all_lines[index] == "Classification and methodology" || all_lines[index] == "Totals" ||
          all_lines[index] == "Included operations" || all_lines[index] == "Manifest";
      content << (is_title ? "/F2 16 Tf\n" : (is_heading ? "/F2 11 Tf\n" : "/F1 9 Tf\n"));
      content << '(' << pdf_escape(all_lines[index]) << ") Tj\n0 -14 Td\n";
    }
    content << "ET\n";
    const std::string stream = content.str();
    objects[content_id] =
        "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "endstream";
  }

  std::ostringstream pdf;
  pdf << "%PDF-1.4\n";
  std::vector<long long> offsets(objects.size(), 0);
  for (std::size_t index = 1; index < objects.size(); ++index) {
    offsets[index] = static_cast<long long>(pdf.tellp());
    pdf << index << " 0 obj\n" << objects[index] << "\nendobj\n";
  }
  const long long xref = static_cast<long long>(pdf.tellp());
  pdf << "xref\n0 " << objects.size() << "\n0000000000 65535 f \n";
  for (std::size_t index = 1; index < objects.size(); ++index) {
    pdf << std::setw(10) << std::setfill('0') << offsets[index] << " 00000 n \n";
  }
  pdf << "trailer\n<< /Size " << objects.size() << " /Root 1 0 R >>\nstartxref\n"
      << xref << "\n%%EOF\n";
  return pdf.str();
}

// Written the way every file Singularity writes is (Platform/AtomicFile): flushed
// to the disk before it counts as written, a full disk said as DiskFull in plain
// words, and no path in any message.
void write_file(const std::filesystem::path& path, const std::string& bytes) {
  try {
    platform::write_file_durably(path, bytes);
  } catch (const platform::DiskFull&) {
    throw;
  } catch (const std::exception&) {
    throw std::runtime_error("Singularity could not finish the usage statement.");
  }
}

std::string safe_period_date(const std::string& timestamp) {
  return timestamp.size() >= 10 ? timestamp.substr(0, 10) : "period";
}

} // namespace

StructuredStatementFormat structured_statement_format_from_string(const std::string& value) {
  if (value == "json")
    return StructuredStatementFormat::Json;
  if (value == "csv")
    return StructuredStatementFormat::Csv;
  throw std::runtime_error("Choose JSON or CSV for the structured statement.");
}

Json build_statement(const UsageSummary& summary, const std::string& project_id,
                     const std::string& generated_at) {
  Json operations = Json::array();
  Json operation_ids = Json::array();
  Json operation_hashes = Json::array();
  for (const OperationUsageEvidence& operation : summary.operations) {
    Json row = operation_json(operation);
    operations.push_back(row);
    operation_ids.push_back(operation.operation_id);
    operation_hashes.push_back({
        {"operationId", operation.operation_id},
        {"sha256", platform::sha256_hex_digest(row.dump())},
    });
  }

  Json statement{
      {"schemaVersion", 1},
      {"statementType", "project_ai_usage_statement"},
      {"projectId", project_id},
      {"period",
       {{"start", summary.period.start}, {"end", summary.period.end}, {"endExclusive", true}}},
      {"generatedAt", generated_at},
      {"methodology",
       Json::array({
           "Actual means provider-reported monetary evidence explicitly classified as actual.",
           "Estimated means provider-reported or provider-sourced monetary evidence explicitly "
           "classified as estimated; it is not a provider charge.",
           "Usage only means reliable token or runtime usage exists but no reliable monetary "
           "amount exists.",
           "Unavailable means neither reliable monetary evidence nor usage suitable for costing "
           "was available.",
           "Amounts in different currencies are kept separate. Singularity performs no currency "
           "conversion in this statement.",
       })},
      {"totals", totals_json(summary)},
      {"operations", std::move(operations)},
      {"manifest",
       {
           {"schemaVersion", 1},
           {"source", "canonical_project_operations_reconciled_with_routing_and_provider_evidence"},
           {"operationCount", summary.operation_count},
           {"operationIds", std::move(operation_ids)},
           {"operationHashes", std::move(operation_hashes)},
       }},
  };
  statement["contentHashAlgorithm"] = "sha256";
  statement["contentHash"] = platform::sha256_hex_digest(statement.dump());
  return statement;
}

bool verify_statement_hash(const Json& statement) {
  if (statement.is_object() == false ||
      statement.value("contentHashAlgorithm", std::string()) != "sha256" ||
      statement.contains("contentHash") == false ||
      statement.at("contentHash").is_string() == false) {
    return false;
  }
  Json content = statement;
  const std::string expected = content.at("contentHash").get<std::string>();
  content.erase("contentHash");
  return platform::sha256_hex_digest(content.dump()) == expected;
}

StatementBundleResult write_statement_bundle(const std::filesystem::path& destination_directory,
                                             const Json& statement,
                                             StructuredStatementFormat structured_format) {
  if (verify_statement_hash(statement) == false)
    throw std::runtime_error("The usage statement hash is invalid.");
  if (std::filesystem::is_directory(destination_directory) == false) {
    throw std::runtime_error("Choose a folder for the usage statement.");
  }

  const std::string base_name =
      "Singularity-Matter-AI-Usage-Statement-" +
      safe_period_date(statement.at("period").at("start").get<std::string>()) + "-to-" +
      safe_period_date(statement.at("period").at("end").get<std::string>());
  const std::filesystem::path pdf_path = destination_directory / (base_name + ".pdf");
  const std::string extension =
      structured_format == StructuredStatementFormat::Json ? "json" : "csv";
  const std::filesystem::path structured_path =
      destination_directory / (base_name + "." + extension);
  if (std::filesystem::exists(pdf_path) || std::filesystem::exists(structured_path)) {
    throw std::runtime_error("A usage statement for this period already exists in that folder.");
  }

  const std::string pdf = build_pdf(statement);
  const std::string structured = structured_format == StructuredStatementFormat::Json
                                     ? statement.dump(2)
                                     : statement_csv(statement);
  // Both files or neither: a disk without room for the two is refused before
  // either is begun.
  platform::require_free_space(destination_directory, pdf.size() + structured.size());
  try {
    write_file(pdf_path, pdf);
    write_file(structured_path, structured);
  } catch (...) {
    std::error_code cleanup_error;
    std::filesystem::remove(pdf_path, cleanup_error);
    std::filesystem::remove(structured_path, cleanup_error);
    throw;
  }

  StatementBundleResult result;
  result.pdf_path = pdf_path;
  result.structured_path = structured_path;
  result.structured_format = extension;
  result.generated_at = statement.at("generatedAt").get<std::string>();
  result.content_hash = statement.at("contentHash").get<std::string>();
  result.operation_count = statement.at("manifest").at("operationCount").get<long long>();
  return result;
}

} // namespace singularity::usage
