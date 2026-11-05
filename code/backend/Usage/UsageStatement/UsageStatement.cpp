#include "Usage/UsageStatement/UsageStatement.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Routing/RoutingLog/OutboundRecord.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <iomanip>
#include <iterator>
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

// ---- The AI work statement (D-252) --------------------------------------------------

namespace {

Json text_or_null(const std::string& value) {
  return value.empty() ? Json(nullptr) : Json(value);
}

Json number_or_null(const std::optional<long long>& value) {
  return value.has_value() ? Json(*value) : Json(nullptr);
}

// What a request was for, as the bill says it (the window's billWords).
std::string work_words(const OperationUsageEvidence& operation, const WorkStatementNames& names) {
  if (operation.operation_role == "research") return "Web research";
  if (operation.operation_type == "document_review") return "Document review";
  if (operation.operation_type == "agent_run") return "Data room watch";
  if (operation.operation_type == "review_table") return "Review table";
  if (operation.operation_type == "context_automation") return "The Matter\xE2\x80\x99s context";
  if (operation.operation_type == "memory_keeping") return "What the Matter remembers";
  if (operation.chat_id.has_value()) {
    const auto found = names.threads.find(*operation.chat_id);
    if (found != names.threads.end() && found->second.empty() == false)
      return "In \xE2\x80\x9C" + found->second + "\xE2\x80\x9D";
  }
  return "A request";
}

Json call_json(const BilledCall& call) {
  return Json{{"time", call.time},
              {"provider", call.provider_id},
              {"model", call.model_id},
              {"servedBy", text_or_null(call.served_by)},
              {"providerRequestId", text_or_null(call.provider_request_id)},
              {"sentSha256", text_or_null(call.payload_sha256)},
              {"receivedSha256", text_or_null(call.response_sha256)},
              {"inputTokens", number_or_null(call.input_tokens)},
              {"cachedInputTokens", number_or_null(call.cached_input_tokens)},
              {"outputTokens", number_or_null(call.output_tokens)},
              {"reasoningTokens", number_or_null(call.reasoning_tokens)},
              {"costMicros", number_or_null(call.cost_micros)},
              {"costCurrency", text_or_null(call.cost_currency)},
              {"costClassification", text_or_null(call.cost_classification)},
              {"costMethodology", text_or_null(call.cost_methodology)},
              {"protection", call.protection_mode.empty() ? std::string("off") : call.protection_mode},
              // Its place in its computer's work record, which the statement's
              // "record" carries (D-252).
              {"record", call.record_chain.empty() || !call.record_index
                             ? Json(nullptr)
                             : Json{{"chain", call.record_chain}, {"index", *call.record_index}}},
              // What the provider's own record says it cost, once asked.
              {"confirmation", call.confirmation.has_value()
                                   ? Json{{"source", call.confirmation->source},
                                          {"costMicros", number_or_null(call.confirmation->cost_micros)},
                                          {"currency", text_or_null(call.confirmation->currency)},
                                          {"inputTokens", number_or_null(call.confirmation->input_tokens)},
                                          {"outputTokens", number_or_null(call.confirmation->output_tokens)},
                                          {"provider", text_or_null(call.confirmation->provider)},
                                          {"createdAt", text_or_null(call.confirmation->created_at)},
                                          {"confirmedAt", text_or_null(call.confirmation->confirmed_at)},
                                          {"recordSha256", text_or_null(call.confirmation->record_sha256)}}
                                   : Json(nullptr)}};
}

Json request_json(const OperationUsageEvidence& operation, const WorkStatementNames& names) {
  Json colleague = nullptr;
  if (operation.agent_id.empty() == false) {
    const auto found = names.colleagues.find(operation.agent_id);
    std::string name = found != names.colleagues.end() ? found->second.name : std::string();
    if (name.empty()) name = operation.agent_name;
    colleague = Json{{"id", operation.agent_id},
                     {"name", name},
                     {"title", found != names.colleagues.end() ? found->second.title : std::string()}};
  }
  const Json asked = operation.asked_by_actor_id.empty()
                         ? Json(nullptr)
                         : Json{{"id", operation.asked_by_actor_id}, {"name", operation.asked_by_name}};
  const bool isPriced = (operation.cost_classification == CostClassification::Actual ||
                         operation.cost_classification == CostClassification::Estimated) &&
                        operation.usage.cost_micros.has_value() && operation.usage.cost_currency.has_value();
  const Json cost = isPriced ? Json{{"micros", *operation.usage.cost_micros},
                                    {"currency", *operation.usage.cost_currency},
                                    {"classification", to_string(operation.cost_classification)},
                                    {"methodology", operation.usage.cost_methodology.value_or(std::string())}}
                             : Json(nullptr);
  Json calls = Json::array();
  for (const BilledCall& call : operation.calls) calls.push_back(call_json(call));
  return Json{{"id", operation.operation_id},
              {"time", operation.timestamp},
              {"work", work_words(operation, names)},
              {"colleague", colleague},
              {"askedBy", asked},
              {"onItsOwn", operation.agent_initiated},
              {"model", optional_string_json(operation.model_id)},
              {"provider", optional_string_json(operation.provider_id)},
              {"execution", operation.execution_kind},
              {"status", operation.operation_status},
              {"tokens",
               {{"input", number_or_null(operation.usage.input_tokens)},
                {"output", number_or_null(operation.usage.output_tokens)},
                {"total", number_or_null(operation.usage.total_tokens)}}},
              {"cost", cost},
              {"costClassification", to_string(operation.cost_classification)},
              {"calls", std::move(calls)},
              {"callsRecorded", operation.calls_recorded}};
}

// ---- Words a person reads in the PDF ---------------------------------------------------

const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

// "Nov 4, 2026, 15:12 UTC" from "2026-11-04T15:12:04Z"; the time as given when
// it is not one.
std::string time_words(const std::string& time, bool isLong = false) {
  if (time.size() < 16 || time[4] != '-' || time[7] != '-' || time[10] != 'T') return time;
  const int month = std::atoi(time.substr(5, 2).c_str());
  const int day = std::atoi(time.substr(8, 2).c_str());
  if (month < 1 || month > 12 || day < 1) return time;
  const std::string name = kMonths[month - 1];
  return (isLong ? name : name.substr(0, 3)) + " " + std::to_string(day) + ", " + time.substr(0, 4) + ", " +
         time.substr(11, 5) + " UTC";
}

// An amount to the millionth where it has one, never rounded to a cent:
// "$0.0521", "$1.25", "EUR 0.0031".
std::string exact_amount(long long micros, const std::string& currency) {
  const bool isNegative = micros < 0;
  const long long magnitude = isNegative ? -micros : micros;
  std::string fraction = std::to_string(magnitude % 1'000'000);
  fraction = std::string(6 - fraction.size(), '0') + fraction;
  while (fraction.size() > 2 && fraction.back() == '0') fraction.pop_back();
  const std::string number = std::to_string(magnitude / 1'000'000) + "." + fraction;
  const std::string sign = isNegative ? "-" : "";
  return currency == "USD" ? sign + "$" + number : currency + " " + sign + number;
}

// The same amount to the cent, as an invoice charges it.
std::string cents_amount(long long micros, const std::string& currency) {
  const long long cents = (micros + (micros >= 0 ? 5'000 : -5'000)) / 10'000;
  const long long magnitude = cents < 0 ? -cents : cents;
  std::string fraction = std::to_string(magnitude % 100);
  if (fraction.size() < 2) fraction = "0" + fraction;
  const std::string number = (cents < 0 ? "-" : "") + std::to_string(magnitude / 100) + "." + fraction;
  return currency == "USD" ? "$" + number : currency + " " + number;
}

std::string count_words(long long count, const std::string& word) {
  std::string digits = std::to_string(count);
  for (int place = static_cast<int>(digits.size()) - 3; place > 0; place -= 3) digits.insert(static_cast<std::size_t>(place), ",");
  return digits + " " + word + (count == 1 ? "" : "s");
}

std::string grouped_number(long long value) {
  std::string digits = std::to_string(value < 0 ? -value : value);
  for (int place = static_cast<int>(digits.size()) - 3; place > 0; place -= 3) digits.insert(static_cast<std::size_t>(place), ",");
  return (value < 0 ? "-" : "") + digits;
}

std::string provider_words(const std::string& id) {
  static const std::map<std::string, std::string> known = {
      {"openai", "OpenAI"},     {"anthropic", "Anthropic"}, {"google", "Google"}, {"mistral", "Mistral"},
      {"openrouter", "OpenRouter"}, {"xai", "xAI"},        {"deepseek", "DeepSeek"}, {"meta", "Meta"},
      {"local", "this computer"}};
  std::string lower = id;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const auto found = known.find(lower);
  if (found != known.end()) return found->second;
  if (id.empty()) return id;
  std::string named = id;
  named[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(named[0])));
  return named;
}

// "openai/gpt-6.1-sol" is "OpenAI GPT-6.1 Sol", as the window names it.
std::string model_words(const std::string& id) {
  const std::size_t slash = id.find('/');
  if (slash == std::string::npos) return id;
  const std::string model = id.substr(slash + 1);
  std::vector<std::string> words;
  std::size_t start = 0;
  while (start <= model.size()) {
    const std::size_t dash = model.find('-', start);
    std::string part = model.substr(start, dash == std::string::npos ? std::string::npos : dash - start);
    const bool isNumber = part.empty() == false && std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; });
    if (isNumber && words.empty() == false && std::isdigit(static_cast<unsigned char>(words.back().back()))) {
      words.back() += "." + part;
    } else if (part.empty() == false && std::isdigit(static_cast<unsigned char>(part[0])) && words.empty() == false && words.back() == "GPT") {
      words.back() = "GPT-" + part;
    } else if (part == "gpt" || part == "GPT") {
      words.push_back("GPT");
    } else if (part.empty() == false) {
      if (std::isdigit(static_cast<unsigned char>(part[0])) == 0) part[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])));
      words.push_back(part);
    }
    if (dash == std::string::npos) break;
    start = dash + 1;
  }
  std::string said = provider_words(id.substr(0, slash));
  for (const std::string& word : words) said += " " + word;
  return said;
}

// ---- A PDF a person reads: Helvetica in WinAnsi, laid out on a grid -------------------

// Each character's width in Helvetica, from its AFM, for ASCII 32 to 126.
const int kHelveticaWidths[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556,
    556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667,
    611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667,
    667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500,
    222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};

// UTF-8 as WinAnsi bytes: ASCII and Latin-1 as they are, the typographic
// quotes, dashes, the ellipsis and the bullet at their places, anything else
// as "?".
std::string win_ansi(const std::string& utf8) {
  std::string bytes;
  for (std::size_t index = 0; index < utf8.size();) {
    const unsigned char lead = static_cast<unsigned char>(utf8[index]);
    unsigned int point = lead;
    std::size_t length = 1;
    if (lead >= 0xF0) { length = 4; point = lead & 0x07; }
    else if (lead >= 0xE0) { length = 3; point = lead & 0x0F; }
    else if (lead >= 0xC0) { length = 2; point = lead & 0x1F; }
    if (length > 1) {
      if (index + length > utf8.size()) { bytes += '?'; break; }
      for (std::size_t more = 1; more < length; ++more) point = (point << 6) | (static_cast<unsigned char>(utf8[index + more]) & 0x3F);
    }
    index += length;
    if (point < 0x80) bytes += static_cast<char>(point);
    else if (point >= 0xA0 && point <= 0xFF) bytes += static_cast<char>(point);
    else if (point == 0x2018) bytes += '\x91';
    else if (point == 0x2019) bytes += '\x92';
    else if (point == 0x201C) bytes += '\x93';
    else if (point == 0x201D) bytes += '\x94';
    else if (point == 0x2022) bytes += '\x95';
    else if (point == 0x2013) bytes += '\x96';
    else if (point == 0x2014) bytes += '\x97';
    else if (point == 0x2026) bytes += '\x85';
    else if (point == 0x20AC) bytes += '\x80';
    else bytes += '?';
  }
  return bytes;
}

enum class Face { Regular = 1, Bold = 2, Code = 3 };

double text_width(const std::string& winAnsi, Face face, double size) {
  double units = 0;
  for (const char character : winAnsi) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (face == Face::Code) units += 600;
    else if (byte >= 32 && byte <= 126) units += kHelveticaWidths[byte - 32];
    else if (byte == 0x91 || byte == 0x92) units += 222;
    else if (byte == 0x93 || byte == 0x94) units += 333;
    else if (byte == 0x97 || byte == 0x85) units += 1000;
    else units += 556;
  }
  return units * size / 1000.0;
}

std::string pdf_text(const std::string& winAnsi) {
  std::string escaped;
  for (const char character : winAnsi) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte == '(' || byte == ')' || byte == '\\') {
      escaped += '\\';
      escaped += static_cast<char>(byte);
    } else if (byte < 32 || byte >= 127) {
      char octal[5];
      std::snprintf(octal, sizeof(octal), "\\%03o", byte);
      escaped += octal;
    } else {
      escaped += static_cast<char>(byte);
    }
  }
  return escaped;
}

struct Run {
  double x = 0;
  double y = 0;
  Face face = Face::Regular;
  double size = 9;
  double gray = 0;
  std::string text;  // WinAnsi
};

// Lines down US Letter pages with even margins: each line drops by its own
// leading, and a block that would not fit begins a new page.
class Pages {
public:
  static constexpr double kLeft = 54;
  static constexpr double kRight = 558;
  static constexpr double kTop = 738;
  static constexpr double kBottom = 66;

  Pages() { pages_.emplace_back(); }

  void keep(double height) {
    if (y_ - height < kBottom) {
      pages_.emplace_back();
      y_ = kTop;
    }
  }
  void drop(double leading) {
    keep(leading);
    y_ -= leading;
  }
  void text(double x, const std::string& utf8, Face face, double size, double gray = 0) {
    pages_.back().push_back(Run{x, y_, face, size, gray, win_ansi(utf8)});
  }
  void right(const std::string& utf8, Face face, double size, double gray = 0) {
    const std::string bytes = win_ansi(utf8);
    pages_.back().push_back(Run{kRight - text_width(bytes, face, size), y_, face, size, gray, bytes});
  }
  // Words wrapped to a width, each line dropping by the leading.
  void wrapped(double x, const std::string& utf8, Face face, double size, double leading, double width,
               double gray = 0) {
    std::string line;
    std::size_t start = 0;
    const auto flush = [&]() {
      drop(leading);
      text(x, line, face, size, gray);
      line.clear();
    };
    while (start <= utf8.size()) {
      const std::size_t space = utf8.find(' ', start);
      const std::string word = utf8.substr(start, space == std::string::npos ? std::string::npos : space - start);
      const std::string tried = line.empty() ? word : line + " " + word;
      if (line.empty() == false && text_width(win_ansi(tried), face, size) > width) {
        flush();
        line = word;
      } else {
        line = tried;
      }
      if (space == std::string::npos) break;
      start = space + 1;
    }
    if (line.empty() == false) flush();
  }
  double y() const { return y_; }
  const std::vector<std::vector<Run>>& all() const { return pages_; }

private:
  std::vector<std::vector<Run>> pages_;
  double y_ = kTop;
};

std::string pdf_of(const Pages& pages, const std::string& footer) {
  const std::vector<std::vector<Run>>& all = pages.all();
  const std::size_t count = all.size();
  // 1 catalog, 2 pages, 3-5 fonts, then a page and its content for each.
  std::vector<std::string> objects(6 + count * 2);
  objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
  std::ostringstream kids;
  for (std::size_t page = 0; page < count; ++page) kids << 6 + page * 2 << " 0 R ";
  objects[2] = "<< /Type /Pages /Count " + std::to_string(count) + " /Kids [ " + kids.str() + "] >>";
  objects[3] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
  objects[4] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>";
  objects[5] = "<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>";
  for (std::size_t page = 0; page < count; ++page) {
    const std::size_t pageId = 6 + page * 2;
    objects[pageId] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R "
                      "/F2 4 0 R /F3 5 0 R >> >> /Contents " +
                      std::to_string(pageId + 1) + " 0 R >>";
    std::ostringstream content;
    content << std::fixed << std::setprecision(2);
    std::vector<Run> runs = all[page];
    const std::string footerText = win_ansi(footer);
    runs.push_back(Run{Pages::kLeft, 40, Face::Regular, 7.5, 0.45, footerText});
    const std::string number = win_ansi("Page " + std::to_string(page + 1) + " of " + std::to_string(count));
    runs.push_back(Run{Pages::kRight - text_width(number, Face::Regular, 7.5), 40, Face::Regular, 7.5, 0.45, number});
    for (const Run& run : runs) {
      content << "BT " << run.gray << " g /F" << static_cast<int>(run.face) << ' ' << run.size << " Tf " << run.x << ' '
              << run.y << " Td (" << pdf_text(run.text) << ") Tj ET\n";
    }
    const std::string stream = content.str();
    objects[pageId + 1] = "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "endstream";
  }
  std::ostringstream pdf;
  pdf << "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<long long> offsets(objects.size(), 0);
  for (std::size_t index = 1; index < objects.size(); ++index) {
    offsets[index] = static_cast<long long>(pdf.tellp());
    pdf << index << " 0 obj\n" << objects[index] << "\nendobj\n";
  }
  const long long xref = static_cast<long long>(pdf.tellp());
  pdf << "xref\n0 " << objects.size() << "\n0000000000 65535 f \n";
  for (std::size_t index = 1; index < objects.size(); ++index)
    pdf << std::setw(10) << std::setfill('0') << offsets[index] << " 00000 n \n";
  pdf << "trailer\n<< /Size " << objects.size() << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
  return pdf.str();
}

// Whose work a request was, as the statement groups it: an AI colleague, a
// member's own request, or Singularity keeping the Matter's context.
std::string worker_key(const Json& request) {
  if (request.at("colleague").is_object()) return "colleague:" + request.at("colleague").at("id").get<std::string>();
  if (request.at("askedBy").is_object()) return "person:" + request.at("askedBy").at("id").get<std::string>();
  return "upkeep";
}

std::string worker_words(const Json& request) {
  if (request.at("colleague").is_object()) {
    const Json& colleague = request.at("colleague");
    const std::string name = colleague.value("name", std::string()).empty() ? std::string("An AI colleague")
                                                                            : colleague.at("name").get<std::string>();
    const std::string title = colleague.value("title", std::string());
    return title.empty() ? name + ", AI colleague" : name + ", " + title;
  }
  if (request.at("askedBy").is_object()) {
    const std::string name = request.at("askedBy").value("name", std::string());
    return (name.empty() ? std::string("A member") : name) + "\xE2\x80\x99s own requests";
  }
  return "Singularity, keeping the Matter\xE2\x80\x99s context";
}

std::string work_statement_pdf(const Json& file, const std::string& signedName) {
  const Json& statement = file.at("statement");
  const Json& signature = file.at("signature");
  Pages pages;
  const double column = 186;
  const double width = Pages::kRight - column - 74;

  pages.drop(4);
  pages.text(Pages::kLeft, "AI work statement", Face::Bold, 18);
  pages.drop(26);
  pages.text(Pages::kLeft, statement.value("matter", Json::object()).value("name", std::string()), Face::Regular, 12);
  pages.drop(16);
  pages.text(Pages::kLeft, statement.value("firm", std::string()), Face::Regular, 10, 0.35);
  pages.drop(14);
  pages.text(Pages::kLeft, statement.at("period").value("name", std::string()), Face::Regular, 10, 0.35);
  pages.drop(14);
  pages.text(Pages::kLeft, "Issued " + time_words(statement.value("issuedAt", std::string()), true), Face::Regular, 10, 0.35);

  // What it came to, in one sentence.
  std::string total;
  for (const Json& each : statement.at("totals")) {
    const std::string currency = each.at("currency").get<std::string>();
    total += (total.empty() ? "" : " and ") + exact_amount(each.at("micros").get<long long>(), currency);
  }
  std::string invoice;
  for (const Json& each : statement.at("totals")) {
    invoice += (invoice.empty() ? "" : " and ") +
               cents_amount(each.at("micros").get<long long>(), each.at("currency").get<std::string>());
  }
  const long long requestCount = statement.value("requestCount", 0LL);
  const std::string sentence = count_words(requestCount, "request") + " to AI models, " +
                               grouped_number(statement.at("tokens").value("total", 0LL)) + " tokens" +
                               (total.empty() ? std::string(". No cost was recorded.")
                                              : ", at cost " + total + ". On the invoice: " + invoice + ".");
  pages.drop(10);
  pages.wrapped(Pages::kLeft, sentence, Face::Regular, 10.5, 15, Pages::kRight - Pages::kLeft);

  // Each worker's requests, the most costly worker first, each worker's in the
  // order made.
  std::vector<std::string> order;
  std::map<std::string, std::vector<const Json*>> byWorker;
  std::map<std::string, long long> spent;
  for (const Json& request : statement.at("requests")) {
    const std::string key = worker_key(request);
    if (byWorker.count(key) == 0) order.push_back(key);
    byWorker[key].push_back(&request);
    if (request.at("cost").is_object()) spent[key] += request.at("cost").at("micros").get<long long>();
  }
  std::stable_sort(order.begin(), order.end(), [&](const std::string& one, const std::string& other) { return spent[one] > spent[other]; });
  for (const std::string& key : order) {
    const std::vector<const Json*>& requests = byWorker[key];
    pages.drop(24);
    pages.keep(40);
    pages.text(Pages::kLeft, worker_words(*requests.front()), Face::Bold, 10.5);
    std::map<std::string, long long> subtotal;
    for (const Json* request : requests)
      if (request->at("cost").is_object())
        subtotal[request->at("cost").at("currency").get<std::string>()] += request->at("cost").at("micros").get<long long>();
    std::string subtotalWords;
    for (const auto& [currency, micros] : subtotal) subtotalWords += (subtotalWords.empty() ? "" : " and ") + exact_amount(micros, currency);
    pages.right(subtotalWords.empty() ? std::string("No cost") : subtotalWords, Face::Bold, 10.5);
    for (const Json* requestPointer : requests) {
      const Json& request = *requestPointer;
      const std::size_t calls = request.at("calls").size();
      pages.keep(30 + static_cast<double>(calls) * 10);
      pages.drop(17);
      pages.text(Pages::kLeft, time_words(request.at("time").get<std::string>()), Face::Regular, 9, 0.35);
      std::string what = request.at("work").get<std::string>();
      if (request.at("colleague").is_object()) {
        if (request.value("onItsOwn", false)) what += ", on its own";
        else if (request.at("askedBy").is_object()) what += ", asked by " + request.at("askedBy").value("name", std::string("a member"));
      }
      pages.text(column, what, Face::Regular, 9.5);
      const Json& cost = request.at("cost");
      pages.right(cost.is_object() ? exact_amount(cost.at("micros").get<long long>(), cost.at("currency").get<std::string>())
                                   : std::string("No cost"),
                  Face::Regular, 9.5);
      std::string detail = request.at("model").is_string() ? model_words(request.at("model").get<std::string>())
                                                           : std::string("No model connected");
      if (request.at("provider").is_string() && request.at("model").is_string() &&
          request.at("model").get<std::string>().rfind(request.at("provider").get<std::string>() + "/", 0) != 0)
        detail += " through " + provider_words(request.at("provider").get<std::string>());
      if (request.at("tokens").at("total").is_number_integer())
        detail += ", " + grouped_number(request.at("tokens").at("total").get<long long>()) + " tokens";
      if (cost.is_object() && cost.value("classification", std::string()) == "estimated") detail += ", estimated from the price list";
      if (request.value("status", std::string()) == "error") detail += ", did not finish";
      if (request.value("execution", std::string()) == "local") detail += ", on the firm\xE2\x80\x99s own computer";
      pages.wrapped(column, detail, Face::Regular, 8.5, 12, width, 0.4);
      for (const Json& call : request.at("calls")) {
        std::string evidence = call.at("providerRequestId").is_string() ? call.at("providerRequestId").get<std::string>()
                                                                         : std::string("no request id");
        if (call.at("costMicros").is_number_integer() && call.at("costCurrency").is_string())
          evidence += "  " + exact_amount(call.at("costMicros").get<long long>(), call.at("costCurrency").get<std::string>());
        // The fingerprints' first twelve characters, enough to find them in the
        // signed file, which holds them whole.
        if (call.at("sentSha256").is_string()) evidence += "  sent " + call.at("sentSha256").get<std::string>().substr(0, 12);
        if (call.at("receivedSha256").is_string()) evidence += "  received " + call.at("receivedSha256").get<std::string>().substr(0, 12);
        // What the provider's own record says, where it was asked (D-252).
        if (call.contains("confirmation") && call.at("confirmation").is_object() &&
            call.at("confirmation").at("costMicros").is_number_integer() && call.at("confirmation").at("currency").is_string())
          evidence += "  confirmed " + exact_amount(call.at("confirmation").at("costMicros").get<long long>(),
                                                    call.at("confirmation").at("currency").get<std::string>());
        pages.wrapped(column, evidence, Face::Code, 7, 10, width + 74, 0.4);
      }
      if (calls == 0 && request.value("callsRecorded", false) == false && request.at("model").is_string())
        pages.wrapped(column, "Its calls ran before Singularity kept their evidence.", Face::Regular, 8, 11, width, 0.4);
    }
  }

  // How it was counted, and how to check it.
  pages.drop(30);
  pages.keep(120);
  pages.text(Pages::kLeft, "How each line was counted", Face::Bold, 10.5);
  for (const Json& said : statement.at("method")) {
    pages.drop(4);
    pages.wrapped(Pages::kLeft, said.get<std::string>(), Face::Regular, 9, 13, Pages::kRight - Pages::kLeft, 0.2);
  }
  pages.drop(26);
  pages.keep(110);
  pages.text(Pages::kLeft, "How to check this statement", Face::Bold, 10.5);
  pages.drop(4);
  pages.wrapped(Pages::kLeft,
                "A computer of " + statement.value("firm", std::string("the firm")) +
                    " signed this statement when it was issued, with its own key, whose code is " +
                    signature.value("keyCode", std::string()) + ". Its fingerprint (SHA-256) is:",
                Face::Regular, 9, 13, Pages::kRight - Pages::kLeft, 0.2);
  pages.drop(14);
  pages.text(Pages::kLeft, signature.value("statementSha256", std::string()), Face::Code, 8.5);
  pages.drop(8);
  pages.wrapped(Pages::kLeft,
                "Its signed file, " + signedName +
                    ", holds every line above with the full fingerprints. Open it at legalmachines.org/check to see "
                    "that it is unchanged and whose key signed it. The check runs in your browser: the file is not "
                    "sent anywhere.",
                Face::Regular, 9, 13, Pages::kRight - Pages::kLeft, 0.2);

  const std::string footer = statement.value("matter", Json::object()).value("name", std::string()) +
                             ", AI work statement, " + statement.at("period").value("name", std::string());
  return pdf_of(pages, footer);
}

// A name a file can have on every computer: no character a folder forbids, at
// most 80 characters, never empty.
std::string file_safe(const std::string& name) {
  std::string safe;
  for (const char character : name) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 32 || std::string("/\\:*?\"<>|").find(character) != std::string::npos) safe += ' ';
    else safe += character;
  }
  while (safe.empty() == false && (safe.back() == ' ' || safe.back() == '.')) safe.pop_back();
  std::size_t start = 0;
  while (start < safe.size() && safe[start] == ' ') ++start;
  safe = safe.substr(start);
  if (safe.size() > 80) {
    std::size_t cut = 80;
    while (cut > 0 && (static_cast<unsigned char>(safe[cut]) & 0xC0) == 0x80) --cut;
    safe = safe.substr(0, cut);
  }
  return safe.empty() ? std::string("Matter") : safe;
}

} // namespace

Json build_work_statement(const UsageSummary& summary, const WorkStatementNames& names, const std::string& issued_at,
                          const WorkRecordExcerpt& record) {
  Json requests = Json::array();
  for (const OperationUsageEvidence& operation : summary.operations) requests.push_back(request_json(operation, names));
  Json totals = Json::array();
  for (const CurrencyTotal& total : summary.currency_totals) {
    const long long actual = total.actual_micros.value_or(0);
    const long long estimated = total.estimated_micros.value_or(0);
    totals.push_back(Json{{"currency", total.currency},
                          {"micros", actual + estimated},
                          {"actualMicros", actual},
                          {"estimatedMicros", estimated}});
  }
  Json method = Json::array({
      "Each cost is what the AI provider reported for that request, or, where a line says so, what its price "
      "list gives for the tokens it reported. Nothing is added to it.",
      "Each request is listed with the provider\xE2\x80\x99s own id for every call it made: the provider\xE2\x80\x99s "
      "records hold that call\xE2\x80\x99s time, model, tokens and cost.",
      "The fingerprints (SHA-256) are of exactly what was sent and what came back. They prove which request a "
      "cost belongs to without showing a word of it.",
      "Amounts in different currencies are kept apart; none is converted.",
  });
  Json statement{
      {"kind", "Singularity AI work statement"},
      {"version", 1},
      {"firm", names.firm},
      {"matter", {{"id", names.matter_id}, {"name", names.matter_name}}},
      {"period",
       {{"name", names.period_name}, {"start", summary.period.start}, {"end", summary.period.end}, {"endExclusive", true}}},
      {"issuedAt", issued_at},
      {"requestCount", summary.operation_count},
      {"tokens", {{"input", summary.input_tokens}, {"output", summary.output_tokens}, {"total", summary.total_tokens}}},
      {"totals", std::move(totals)},
      {"requests", std::move(requests)},
  };
  if (record.chains.empty() == false) {
    method.push_back(
        "Each call was written into its computer\xE2\x80\x99s record as it was made: a chain in which every entry holds "
        "the one before it, which Legal Machines timestamped from its fingerprint alone. legalmachines.org/check "
        "finds each call there.");
    Json chains = Json::array();
    for (const WorkRecordExcerpt::Chain& chain : record.chains) {
      Json entries = Json::array();
      for (const WorkRecordExcerpt::Entry& entry : chain.entries)
        entries.push_back(Json{{"index", entry.index}, {"leaf", entry.leaf}, {"prev", entry.prev}, {"hash", entry.hash}});
      Json stamps = Json::array();
      for (const WorkRecordExcerpt::Stamp& stamp : chain.stamps)
        stamps.push_back(Json{{"index", stamp.index}, {"hash", stamp.hash}, {"time", stamp.time}, {"keyId", stamp.key_id},
                              {"signature", stamp.signature}});
      chains.push_back(Json{{"chain", chain.id}, {"keyCode", chain.key_code}, {"entries", std::move(entries)},
                            {"stamps", std::move(stamps)}});
    }
    statement["record"] = Json{{"chains", std::move(chains)}};
  }
  statement["method"] = std::move(method);
  return statement;
}

std::string work_statement_bytes(const Json& statement) {
  return statement.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json signed_work_statement(const Json& statement, const std::string& public_key, const std::string& key_code,
                           const std::function<std::string(const std::string&)>& sign) {
  const std::string bytes = work_statement_bytes(statement);
  return Json{{"format", "singularity-ai-work-statement"},
              {"formatVersion", 1},
              {"statement", statement},
              {"signature",
               {{"algorithm", "Ed25519"},
                {"publicKey", public_key},
                {"keyCode", key_code},
                {"statementSha256", platform::sha256_hex_digest(bytes)},
                {"value", sign(bytes)}}},
              {"check", "https://legalmachines.org/check"}};
}

bool work_statement_print_holds(const Json& signed_file) {
  if (signed_file.is_object() == false || signed_file.contains("statement") == false ||
      signed_file.contains("signature") == false || signed_file.at("signature").is_object() == false)
    return false;
  const Json& signature = signed_file.at("signature");
  if (signature.contains("statementSha256") == false || signature.at("statementSha256").is_string() == false) return false;
  return platform::sha256_hex_digest(work_statement_bytes(signed_file.at("statement"))) ==
         signature.at("statementSha256").get<std::string>();
}

WorkStatementFiles write_work_statement(const std::filesystem::path& folder, const Json& signed_file) {
  if (work_statement_print_holds(signed_file) == false)
    throw std::runtime_error("The statement does not match its own fingerprint.");
  std::error_code error;
  if (std::filesystem::is_directory(folder, error) == false) throw std::runtime_error("Choose a folder for the statement.");
  const Json& statement = signed_file.at("statement");
  const std::string base = file_safe(statement.at("matter").value("name", std::string("Matter"))) + " - AI work - " +
                           file_safe(statement.at("period").value("name", std::string("statement")));
  std::filesystem::path pdfPath;
  std::filesystem::path signedPath;
  for (int number = 1; number < 1000; ++number) {
    const std::string name = number == 1 ? base : base + " (" + std::to_string(number) + ")";
    pdfPath = folder / std::filesystem::u8path(name + ".pdf");
    signedPath = folder / std::filesystem::u8path(name + ".signed.json");
    if (std::filesystem::exists(pdfPath, error) == false && std::filesystem::exists(signedPath, error) == false) break;
    if (number == 999) throw std::runtime_error("That folder holds too many statements of this name. Choose another.");
  }
  const std::string signedName = signedPath.filename().u8string();
  const std::string pdf = work_statement_pdf(signed_file, signedName);
  const std::string json = signed_file.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
  // Both files or neither.
  platform::require_free_space(folder, pdf.size() + json.size());
  try {
    write_file(pdfPath, pdf);
    write_file(signedPath, json);
  } catch (...) {
    std::error_code cleanup;
    std::filesystem::remove(pdfPath, cleanup);
    std::filesystem::remove(signedPath, cleanup);
    throw;
  }
  WorkStatementFiles files;
  files.pdf_path = pdfPath;
  files.signed_path = signedPath;
  files.statement_sha256 = signed_file.at("signature").at("statementSha256").get<std::string>();
  files.request_count = statement.value("requestCount", 0LL);
  return files;
}

namespace work_record {
namespace {

const char* const kLeafKeys[] = {"costCurrency", "costMicros", "inputTokens", "model", "outputTokens",
                                 "provider", "providerRequestId", "receivedSha256", "sentSha256", "time"};

bool is_hex64(const std::string& text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string new_chain_id() {
  std::random_device source;
  static const char* const hex = "0123456789abcdef";
  std::string id;
  for (int index = 0; index < 16; ++index) id += hex[source() % 16];
  return id;
}

void append_line(const std::filesystem::path& path, const Json& value) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  out << value.dump(-1, ' ', false, Json::error_handler_t::replace) << "\n";
  out.flush();
  if (!out) throw std::runtime_error("The work record could not be written.");
}

std::vector<Json> read_lines(const std::filesystem::path& path) {
  std::vector<Json> lines;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    const Json value = Json::parse(line, nullptr, false);
    // A line cut off by a crash is passed over.
    if (value.is_object()) lines.push_back(value);
  }
  return lines;
}

std::unique_ptr<Chain>& installed() {
  static std::unique_ptr<Chain> chain;
  return chain;
}

}  // namespace

Json leaf_fields(const BilledCall& call) {
  return Json{{"costCurrency", text_or_null(call.cost_currency)},
              {"costMicros", number_or_null(call.cost_micros)},
              {"inputTokens", number_or_null(call.input_tokens)},
              {"model", call.model_id},
              {"outputTokens", number_or_null(call.output_tokens)},
              {"provider", call.provider_id},
              {"providerRequestId", text_or_null(call.provider_request_id)},
              {"receivedSha256", text_or_null(call.response_sha256)},
              {"sentSha256", text_or_null(call.payload_sha256)},
              {"time", call.time}};
}

Json leaf_fields_of_statement_call(const Json& call) {
  Json fields = Json::object();
  for (const char* key : kLeafKeys) fields[key] = call.is_object() && call.contains(key) ? call.at(key) : Json(nullptr);
  return fields;
}

std::string leaf_of(const Json& fields) { return platform::sha256_hex_digest(work_statement_bytes(fields)); }

std::string link_of(const std::string& prev, const std::string& leaf) { return platform::sha256_hex_digest(prev + leaf); }

Chain::Chain(std::filesystem::path folder) : folder_(std::move(folder)) {}

void Chain::load() {
  if (loaded_) return;
  loaded_ = true;
  std::error_code error;
  std::filesystem::create_directories(folder_, error);
  std::string namedText;
  {
    std::ifstream in(folder_ / "chain.json", std::ios::binary);
    namedText.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const Json named = Json::parse(namedText, nullptr, false);
  if (named.is_object() && named.contains("id") && named.at("id").is_string()) id_ = named.at("id").get<std::string>();
  if (id_.empty()) {
    id_ = new_chain_id();
    platform::write_file_durably(folder_ / "chain.json", Json{{"id", id_}, {"format", "singularity-work-record"}}.dump(2) + "\n");
  }
  for (const Json& line : read_lines(folder_ / "entries.jsonl")) {
    Entry entry;
    entry.index = line.value("index", -1LL);
    entry.time = line.value("time", std::string());
    entry.leaf = line.value("leaf", std::string());
    entry.prev = line.value("prev", std::string());
    entry.hash = line.value("hash", std::string());
    if (entry.index >= 0 && is_hex64(entry.hash)) entries_.push_back(std::move(entry));
  }
  for (const Json& line : read_lines(folder_ / "stamps.jsonl")) {
    Stamp stamp;
    stamp.index = line.value("index", -1LL);
    stamp.hash = line.value("hash", std::string());
    stamp.time = line.value("time", std::string());
    stamp.key_id = line.value("keyId", std::string());
    stamp.signature = line.value("signature", std::string());
    if (stamp.index >= 0 && is_hex64(stamp.hash)) stamps_.push_back(std::move(stamp));
  }
}

std::string Chain::id() {
  const std::lock_guard<std::mutex> lock(mutex_);
  load();
  return id_;
}

void Chain::append(protected_api::papinian::OutboundCall& call) {
  try {
    const std::lock_guard<std::mutex> lock(mutex_);
    load();
    Entry entry;
    entry.index = entries_.empty() ? 0 : entries_.back().index + 1;
    entry.time = call.time;
    entry.leaf = leaf_of(leaf_fields(billed_call_of(call)));
    entry.prev = entries_.empty() ? kGenesis : entries_.back().hash;
    entry.hash = link_of(entry.prev, entry.leaf);
    append_line(folder_ / "entries.jsonl", Json{{"index", entry.index}, {"time", entry.time}, {"leaf", entry.leaf},
                                                {"prev", entry.prev}, {"hash", entry.hash}});
    entries_.push_back(entry);
    written_ = std::chrono::steady_clock::now();
    call.record_chain = id_;
    call.record_index = entry.index;
  } catch (...) {
  }
}

std::optional<Entry> Chain::head() {
  const std::lock_guard<std::mutex> lock(mutex_);
  load();
  if (entries_.empty()) return std::nullopt;
  return entries_.back();
}

std::vector<Entry> Chain::entries(long long from, long long to) {
  const std::lock_guard<std::mutex> lock(mutex_);
  load();
  std::vector<Entry> found;
  for (const Entry& entry : entries_)
    if (entry.index >= from && entry.index <= to) found.push_back(entry);
  return found;
}

std::vector<Stamp> Chain::stamps() {
  const std::lock_guard<std::mutex> lock(mutex_);
  load();
  return stamps_;
}

bool Chain::add_stamp(const Stamp& stamp) {
  const std::lock_guard<std::mutex> lock(mutex_);
  load();
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& kept) { return kept.index == stamp.index; });
  if (entry == entries_.end() || entry->hash != stamp.hash || stamp.signature.empty()) return false;
  try {
    append_line(folder_ / "stamps.jsonl", Json{{"index", stamp.index}, {"hash", stamp.hash}, {"time", stamp.time},
                                               {"keyId", stamp.key_id}, {"signature", stamp.signature}});
  } catch (...) {
    return false;
  }
  stamps_.push_back(stamp);
  return true;
}

bool Chain::needs_stamp() {
  const std::lock_guard<std::mutex> lock(mutex_);
  load();
  if (entries_.empty()) return false;
  return stamps_.empty() || stamps_.back().index < entries_.back().index;
}

std::optional<std::chrono::steady_clock::time_point> Chain::last_written() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return written_;
}

Chain* this_computer() { return installed().get(); }

void install(const std::filesystem::path& folder) {
  installed() = std::make_unique<Chain>(folder);
  routing::outbound::work_record_sink() = [](protected_api::papinian::OutboundCall& call) {
    if (Chain* chain = this_computer()) chain->append(call);
  };
}

WorkRecordExcerpt excerpt_for(Chain& chain, const std::string& key_code, const UsageSummary& summary) {
  WorkRecordExcerpt excerpt;
  const std::string id = chain.id();
  std::optional<long long> first;
  std::optional<long long> last;
  for (const OperationUsageEvidence& operation : summary.operations) {
    for (const BilledCall& call : operation.calls) {
      if (call.record_chain != id || !call.record_index) continue;
      first = first ? std::min(*first, *call.record_index) : *call.record_index;
      last = last ? std::max(*last, *call.record_index) : *call.record_index;
    }
  }
  if (!first || !last) return excerpt;
  const std::vector<Stamp> stamps = chain.stamps();
  // The first stamp at or after the last call covers them all.
  std::optional<long long> to;
  for (const Stamp& stamp : stamps)
    if (stamp.index >= *last && (!to || stamp.index < *to)) to = stamp.index;
  WorkRecordExcerpt::Chain carried;
  carried.id = id;
  carried.key_code = key_code;
  for (const Entry& entry : chain.entries(*first, to.value_or(*last)))
    carried.entries.push_back(WorkRecordExcerpt::Entry{entry.index, entry.leaf, entry.prev, entry.hash});
  for (const Stamp& stamp : stamps)
    if (stamp.index >= *first && stamp.index <= to.value_or(*last)) carried.stamps.push_back(stamp);
  excerpt.chains.push_back(std::move(carried));
  return excerpt;
}

}  // namespace work_record

} // namespace singularity::usage
