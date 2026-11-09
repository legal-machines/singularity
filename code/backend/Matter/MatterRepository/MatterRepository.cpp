#include "Platform/Trace/Trace.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "WorkProduct/Types/ReviewTablesJson.hpp"
#include "DocumentReview/Types/DocumentReviewJson.hpp"
#include "ExternalResearch/ResearchTools/ResearchTools.hpp"

#include "Context/ContextRegistry/ContextRegistry.hpp"
#include "Context/ContextRetriever/ContextRetriever.hpp"
#include "WorkProduct/FindingParser/FindingParser.hpp"
#include "Context/SourceMarkers/SourceMarkers.hpp"
#include "Document/DocumentCache/DocumentCache.hpp"
#include "Document/DocumentChunker/DocumentChunker.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Document/Types/DocumentActionSerialization.hpp"
#include "Document/Types/DocumentSemantics.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/ProjectStore/ProjectStore.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Events/Events.hpp"
#include "Platform/FileIdentity/FileIdentity.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Routing/Observability/OperationEvent.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace singularity::matters {
namespace {

using Json = nlohmann::json;

Json work_product_origin_to_json(const WorkProductOrigin &origin);
WorkProductOrigin work_product_origin_from_json(const Json &value);


std::filesystem::path previous_path(const std::filesystem::path &path) {
  return path.string() + ".previous";
}

std::filesystem::path unique_temporary_path(const std::filesystem::path &path) {
  std::random_device seed;
  std::mt19937_64 generator(seed());
  return path.string() + ".tmp-" + std::to_string(generator());
}

std::filesystem::path unique_corrupt_path(const std::filesystem::path &path) {
  std::random_device seed;
  std::mt19937_64 generator(seed());
  return path.string() + ".corrupt-" + std::to_string(generator());
}

void atomic_replace(const std::filesystem::path &source, const std::filesystem::path &destination) {
  if (const auto failure = platform::replace_file(source, destination))
    throw std::runtime_error("Unable to replace matter data: " + *failure);
}

Json read_json_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Unable to read matter data: " + path.string());
  }
  return Json::parse(file);
}

// The id of a turn, a document, a chat, an event or a Work Product: 16 digits
// that are 64 bits from the system's secure source (Platform/SecureRandom). A
// generator seeded with one 32-bit draw had 2^32 ids to give, and the newer of
// two equal ids overwrote the older object.
std::string generate_id(const char *prefix) {
  return std::string(prefix) + platform::secure_random_hex(8);
}

const char *task_status_to_string(TaskStatus status) {
  switch (status) {
    case TaskStatus::Success:
      return "success";
    case TaskStatus::Error:
      return "error";
  }
  return "success";
}

TaskStatus task_status_from_string(const std::string &value) {
  return value == "error" ? TaskStatus::Error : TaskStatus::Success;
}

DocumentSourceMode document_source_mode_from_string(const std::string &value) {
  return value == "referenced" ? DocumentSourceMode::Referenced :
                                 DocumentSourceMode::Managed;
}

DocumentExtractionState document_extraction_state_from_string(
    const std::string &value) {
  if (value == "ready") return DocumentExtractionState::Ready;
  if (value == "empty") return DocumentExtractionState::Empty;
  if (value == "ocr_required") return DocumentExtractionState::OcrRequired;
  if (value == "unsupported") return DocumentExtractionState::Unsupported;
  if (value == "unreadable") return DocumentExtractionState::Unreadable;
  return DocumentExtractionState::NotExtracted;
}

documents::DocumentFormat document_format_from_string(const std::string &value,
                                                       const std::string &name) {
  if (const auto format = documents::document_format_from_string(value)) return *format;
  return documents::detect_document_format(name);
}

DocumentLifecycleState document_lifecycle_state_from_string(const std::string& value) {
	if (value == "ready") return DocumentLifecycleState::Ready;
	if (value == "changed") return DocumentLifecycleState::Changed;
	if (value == "needs_attention") return DocumentLifecycleState::NeedsAttention;
	if (value == "unsupported") return DocumentLifecycleState::Unsupported;
	if (value == "unavailable") return DocumentLifecycleState::Unavailable;
	return DocumentLifecycleState::Processing;
}

Json usage_to_json(const providers::UsageMetadata &usage) {
  return Json{
      {"inputTokens", usage.input_tokens ? Json(*usage.input_tokens) : Json(nullptr)},
      {"outputTokens", usage.output_tokens ? Json(*usage.output_tokens) : Json(nullptr)},
      {"totalTokens", usage.total_tokens ? Json(*usage.total_tokens) : Json(nullptr)},
      {"costMicros", usage.cost_micros ? Json(*usage.cost_micros) : Json(nullptr)},
      {"costCurrency", usage.cost_currency ? Json(*usage.cost_currency) : Json(nullptr)},
      {"costClassification",
       usage.cost_classification ? Json(*usage.cost_classification) : Json(nullptr)},
      {"costMethodology", usage.cost_methodology ? Json(*usage.cost_methodology) : Json(nullptr)},
  };
}

providers::UsageMetadata usage_from_json(const Json &value) {
  providers::UsageMetadata usage;
  if (!value.is_object()) return usage;
  if (value.contains("inputTokens") && value.at("inputTokens").is_number_integer()) {
    usage.input_tokens = value.at("inputTokens").get<long long>();
  }
  if (value.contains("outputTokens") && value.at("outputTokens").is_number_integer()) {
    usage.output_tokens = value.at("outputTokens").get<long long>();
  }
  if (value.contains("totalTokens") && value.at("totalTokens").is_number_integer()) {
    usage.total_tokens = value.at("totalTokens").get<long long>();
  }
  if (value.contains("costMicros") && value.at("costMicros").is_number_integer()) {
    usage.cost_micros = value.at("costMicros").get<long long>();
  }
  if (value.contains("costCurrency") && value.at("costCurrency").is_string()) {
    usage.cost_currency = value.at("costCurrency").get<std::string>();
  }
  if (value.contains("costClassification") && value.at("costClassification").is_string()) {
    usage.cost_classification = value.at("costClassification").get<std::string>();
  }
  if (value.contains("costMethodology") && value.at("costMethodology").is_string()) {
    usage.cost_methodology = value.at("costMethodology").get<std::string>();
  }
  return usage;
}

Json privacy_metadata_to_json(
    const protected_api::papinian::PrivacyMetadata &metadata) {
  return Json{
      {"mode", metadata.mode},
      {"papinianGuardVersion", metadata.papinian_guard_version},
      {"papinianNerVersion", metadata.papinian_ner_version},
      {"protectedCount", metadata.protected_count},
      {"categoryCounts", metadata.category_counts},
      {"responseIntegrity", metadata.response_integrity},
	  {"finalPayloadValidated", metadata.final_payload_validated},
	  {"finalPayloadSha256", metadata.final_payload_sha256},
	  {"protectedProjectEntityIds", metadata.protected_project_entity_ids},
	  {"responseIssueCode", metadata.response_issue_code},
	  {"responseMarkerCandidate", metadata.response_marker_candidate},
	  {"responseCanonicalMarker", metadata.response_canonical_marker},
	  {"responseMarkerType", metadata.response_marker_type},
	  {"responseJsonPointer", metadata.response_json_pointer},
	  {"responseNormalizationOutcome", metadata.response_normalization_outcome},
	  {"responseRepairClass", metadata.response_repair_class},
	  {"responseRepairAttempts", metadata.response_repair_attempts},
  };
}

// A model call as the Matter keeps it for its bill: only what a bill shows -
// its time and route, the provider's own id for the request, the
// fingerprints of what was sent and what came back, its tokens and cost -
// never a marker, passage, relation or table of what left.
Json billed_call_to_json(const protected_api::papinian::OutboundCall &call) {
  Json value{{"time", call.time}, {"providerId", call.provider_id}, {"modelId", call.model_id},
      {"protectionMode", call.protection_mode}};
  const auto text = [&value](const char *key, const std::string &said) {
    if (said.empty() == false) value[key] = said;
  };
  const auto number = [&value](const char *key, const std::optional<long long> &said) {
    if (said.has_value()) value[key] = *said;
  };
  text("servedBy", call.served_by);
  text("providerRequestId", call.provider_request_id);
  text("payloadSha256", call.payload_sha256);
  text("responseSha256", call.response_sha256);
  number("inputTokens", call.input_tokens);
  number("cachedInputTokens", call.cached_input_tokens);
  number("outputTokens", call.output_tokens);
  number("reasoningTokens", call.reasoning_tokens);
  number("costMicros", call.cost_micros);
  text("costCurrency", call.cost_currency);
  text("costClassification", call.cost_classification);
  text("costMethodology", call.cost_methodology);
  // Its place in its computer's work record, and the provider's own record of
  // its cost once asked.
  text("recordChain", call.record_chain);
  number("recordIndex", call.record_index);
  if (call.confirmation.has_value()) {
    const protected_api::papinian::CostConfirmation &confirmed = *call.confirmation;
    Json confirmation{{"source", confirmed.source}, {"currency", confirmed.currency}, {"provider", confirmed.provider},
        {"model", confirmed.model}, {"createdAt", confirmed.created_at}, {"confirmedAt", confirmed.confirmed_at},
        {"recordSha256", confirmed.record_sha256}};
    if (confirmed.cost_micros) confirmation["costMicros"] = *confirmed.cost_micros;
    if (confirmed.input_tokens) confirmation["inputTokens"] = *confirmed.input_tokens;
    if (confirmed.output_tokens) confirmation["outputTokens"] = *confirmed.output_tokens;
    value["confirmation"] = std::move(confirmation);
  }
  return value;
}

protected_api::papinian::OutboundCall billed_call_from_json(const Json &value) {
  protected_api::papinian::OutboundCall call;
  call.kind = "model";
  const auto text = [&value](const char *key) {
    return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>() : std::string();
  };
  const auto number = [&value](const char *key) -> std::optional<long long> {
    if (value.contains(key) && value.at(key).is_number_integer()) return value.at(key).get<long long>();
    return std::nullopt;
  };
  call.time = text("time");
  call.provider_id = text("providerId");
  call.model_id = text("modelId");
  call.protection_mode = text("protectionMode").empty() ? std::string("off") : text("protectionMode");
  call.served_by = text("servedBy");
  call.provider_request_id = text("providerRequestId");
  call.payload_sha256 = text("payloadSha256");
  call.response_sha256 = text("responseSha256");
  call.input_tokens = number("inputTokens");
  call.cached_input_tokens = number("cachedInputTokens");
  call.output_tokens = number("outputTokens");
  call.reasoning_tokens = number("reasoningTokens");
  call.cost_micros = number("costMicros");
  call.cost_currency = text("costCurrency");
  call.cost_classification = text("costClassification");
  call.cost_methodology = text("costMethodology");
  call.record_chain = text("recordChain");
  call.record_index = number("recordIndex");
  if (value.contains("confirmation") && value.at("confirmation").is_object()) {
    const Json &kept = value.at("confirmation");
    const auto keptText = [&kept](const char *key) {
      return kept.contains(key) && kept.at(key).is_string() ? kept.at(key).get<std::string>() : std::string();
    };
    const auto keptNumber = [&kept](const char *key) -> std::optional<long long> {
      if (kept.contains(key) && kept.at(key).is_number_integer()) return kept.at(key).get<long long>();
      return std::nullopt;
    };
    protected_api::papinian::CostConfirmation confirmed;
    confirmed.source = keptText("source");
    confirmed.cost_micros = keptNumber("costMicros");
    confirmed.currency = keptText("currency");
    confirmed.input_tokens = keptNumber("inputTokens");
    confirmed.output_tokens = keptNumber("outputTokens");
    confirmed.provider = keptText("provider");
    confirmed.model = keptText("model");
    confirmed.created_at = keptText("createdAt");
    confirmed.confirmed_at = keptText("confirmedAt");
    confirmed.record_sha256 = keptText("recordSha256");
    call.confirmation = std::move(confirmed);
  }
  return call;
}

// The calls a task's record keeps for its bill: those it kept before, and the
// model calls of this run's privacy record not among them yet, each once, in
// the order made.
std::vector<protected_api::papinian::OutboundCall> billed_calls_of(const MatterTask &task) {
  std::vector<protected_api::papinian::OutboundCall> calls = task.billed_calls;
  const auto isSame = [](const protected_api::papinian::OutboundCall &one, const protected_api::papinian::OutboundCall &other) {
    return one.time == other.time && one.payload_sha256 == other.payload_sha256
        && one.provider_request_id == other.provider_request_id;
  };
  for (const protected_api::papinian::OutboundCall &call : task.privacy_metadata.outbound_calls) {
    if (call.kind != "model") continue;
    if (std::any_of(calls.begin(), calls.end(), [&](const auto &kept) { return isSame(kept, call); })) continue;
    calls.push_back(call);
  }
  std::stable_sort(calls.begin(), calls.end(), [](const auto &one, const auto &other) { return one.time < other.time; });
  return calls;
}

protected_api::papinian::PrivacyMetadata privacy_metadata_from_json(
    const Json &value, const std::string &legacy_mode) {
  protected_api::papinian::PrivacyMetadata metadata;
  metadata.mode = legacy_mode;
  if (!value.is_object()) return metadata;
  metadata.mode = value.value("mode", legacy_mode);
  metadata.papinian_guard_version =
      value.value("papinianGuardVersion", metadata.papinian_guard_version);
  metadata.papinian_ner_version =
      value.value("papinianNerVersion", metadata.papinian_ner_version);
  metadata.protected_count = value.value("protectedCount", std::size_t{0});
  metadata.response_integrity =
      value.value("responseIntegrity", metadata.response_integrity);
	metadata.final_payload_validated = value.value("finalPayloadValidated", false);
	metadata.final_payload_sha256 = value.value("finalPayloadSha256", std::string());
	metadata.response_issue_code = value.value("responseIssueCode", std::string());
	metadata.response_marker_candidate = value.value("responseMarkerCandidate", std::string());
	metadata.response_canonical_marker = value.value("responseCanonicalMarker", std::string());
	metadata.response_marker_type = value.value("responseMarkerType", std::string());
	metadata.response_json_pointer = value.value("responseJsonPointer", std::string());
	metadata.response_normalization_outcome = value.value("responseNormalizationOutcome", std::string());
	metadata.response_repair_class = value.value("responseRepairClass", std::string());
	metadata.response_repair_attempts = value.value("responseRepairAttempts", std::size_t{0});
	if (value.contains("protectedProjectEntityIds") && value.at("protectedProjectEntityIds").is_array())
		for (const Json &entry : value.at("protectedProjectEntityIds")) if (entry.is_string()) metadata.protected_project_entity_ids.push_back(entry.get<std::string>());
  if (value.contains("categoryCounts") && value.at("categoryCounts").is_object()) {
    for (const auto &[category, count] : value.at("categoryCounts").items()) {
      if (count.is_number_unsigned()) {
        metadata.category_counts.emplace(category, count.get<std::size_t>());
      }
    }
  }
  return metadata;
}

Json source_reference_to_json(const context::ResolvedSourceReference &reference) {
  Json json{
      {"documentToken", reference.document_token},
      {"chunkToken", reference.chunk_token},
      {"documentId", reference.document_id},
      {"chunkId", reference.chunk_id},
      {"documentName", reference.document_name},
      {"page", reference.page ? Json(*reference.page) : Json(nullptr)},
      {"heading", reference.heading ? Json(*reference.heading) : Json(nullptr)},
  };
  if (!reference.extractor_version.empty()) json["extractorVersion"] = reference.extractor_version;
  return json;
}

std::optional<context::ResolvedSourceReference>
source_reference_from_json(const Json &value) {
  if (!value.is_object()) return std::nullopt;
  context::ResolvedSourceReference reference;
  reference.document_token = value.value("documentToken", std::string());
  reference.chunk_token = value.value("chunkToken", std::string());
  reference.document_id = value.value("documentId", std::string());
  reference.chunk_id = value.value("chunkId", std::string());
  reference.document_name = value.value("documentName", std::string());
  if (value.contains("page") && value.at("page").is_number_unsigned()) {
    reference.page = value.at("page").get<std::size_t>();
  }
  if (value.contains("heading") && value.at("heading").is_string()) {
    reference.heading = value.at("heading").get<std::string>();
  }
  if (value.contains("extractorVersion") && value.at("extractorVersion").is_string()) {
    reference.extractor_version = value.at("extractorVersion").get<std::string>();
  }
  return reference;
}

Json finding_to_json(const Finding &finding) {
  Json source_refs = Json::array();
  for (const auto &reference : finding.source_refs) {
    source_refs.push_back(source_reference_to_json(reference));
  }
  return Json{
      {"id", finding.id},
      {"matterId", finding.matter_id},
      {"title", finding.title},
      {"description", finding.description},
      {"recommendation", finding.recommendation ? Json(*finding.recommendation) : Json(nullptr)},
      {"severity", finding.severity ? Json(*finding.severity) : Json(nullptr)},
      {"sourceRefs", source_refs},
  };
}

std::optional<Finding> finding_from_json(const Json &value) {
  if (!value.is_object()) return std::nullopt;
  Finding finding;
  finding.id = value.value("id", std::string());
  finding.matter_id = value.value("matterId", std::string());
  finding.title = value.value("title", std::string());
  finding.description = value.value("description", std::string());
  if (value.contains("recommendation") && value.at("recommendation").is_string()) {
    finding.recommendation = value.at("recommendation").get<std::string>();
  }
  if (value.contains("severity") && value.at("severity").is_string()) {
    finding.severity = value.at("severity").get<std::string>();
  }
  if (value.contains("sourceRefs") && value.at("sourceRefs").is_array()) {
    for (const auto &entry : value.at("sourceRefs")) {
      if (auto reference = source_reference_from_json(entry)) {
        finding.source_refs.push_back(std::move(*reference));
      }
    }
  }
  return finding;
}

Json obligation_to_json(const Obligation &obligation) {
  Json source_refs = Json::array();
  for (const auto &reference : obligation.source_refs) {
    source_refs.push_back(source_reference_to_json(reference));
  }
  return Json{
      {"id", obligation.id},
      {"matterId", obligation.matter_id},
      {"party", obligation.party ? Json(*obligation.party) : Json(nullptr)},
      {"obligation", obligation.obligation},
      {"trigger", obligation.trigger ? Json(*obligation.trigger) : Json(nullptr)},
      {"deadline", obligation.deadline ? Json(*obligation.deadline) : Json(nullptr)},
      {"sourceRefs", source_refs},
  };
}

std::optional<Obligation> obligation_from_json(const Json &value) {
  if (!value.is_object()) return std::nullopt;
  Obligation obligation;
  obligation.id = value.value("id", std::string());
  obligation.matter_id = value.value("matterId", std::string());
  if (value.contains("party") && value.at("party").is_string()) {
    obligation.party = value.at("party").get<std::string>();
  }
  obligation.obligation = value.value("obligation", std::string());
  if (value.contains("trigger") && value.at("trigger").is_string()) {
    obligation.trigger = value.at("trigger").get<std::string>();
  }
  if (value.contains("deadline") && value.at("deadline").is_string()) {
    obligation.deadline = value.at("deadline").get<std::string>();
  }
  if (value.contains("sourceRefs") && value.at("sourceRefs").is_array()) {
    for (const auto &entry : value.at("sourceRefs")) {
      if (auto reference = source_reference_from_json(entry)) {
        obligation.source_refs.push_back(std::move(*reference));
      }
    }
  }
  return obligation;
}

Json event_to_json(const Event &event) {
  Json source_refs = Json::array();
  for (const auto &reference : event.source_refs) {
    source_refs.push_back(source_reference_to_json(reference));
  }
  return Json{
      {"id", event.id},
      {"matterId", event.matter_id},
      {"date", event.date ? Json(*event.date) : Json(nullptr)},
      {"description", event.description},
      {"sourceRefs", source_refs},
  };
}

std::optional<Event> event_from_json(const Json &value) {
  if (!value.is_object()) return std::nullopt;
  Event event;
  event.id = value.value("id", std::string());
  event.matter_id = value.value("matterId", std::string());
  if (value.contains("date") && value.at("date").is_string()) {
    event.date = value.at("date").get<std::string>();
  }
  event.description = value.value("description", std::string());
  if (value.contains("sourceRefs") && value.at("sourceRefs").is_array()) {
    for (const auto &entry : value.at("sourceRefs")) {
      if (auto reference = source_reference_from_json(entry)) {
        event.source_refs.push_back(std::move(*reference));
      }
    }
  }
  return event;
}

Json change_to_json(const Change &change) {
  Json source_refs = Json::array();
  for (const auto &reference : change.source_refs) {
    source_refs.push_back(source_reference_to_json(reference));
  }
  return Json{
      {"id", change.id},
      {"matterId", change.matter_id},
      {"category", change.category ? Json(*change.category) : Json(nullptr)},
      {"description", change.description},
      {"sourceRefs", source_refs},
  };
}

std::optional<Change> change_from_json(const Json &value) {
  if (!value.is_object()) return std::nullopt;
  Change change;
  change.id = value.value("id", std::string());
  change.matter_id = value.value("matterId", std::string());
  if (value.contains("category") && value.at("category").is_string()) {
    change.category = value.at("category").get<std::string>();
  }
  change.description = value.value("description", std::string());
  if (value.contains("sourceRefs") && value.at("sourceRefs").is_array()) {
    for (const auto &entry : value.at("sourceRefs")) {
      if (auto reference = source_reference_from_json(entry)) {
        change.source_refs.push_back(std::move(*reference));
      }
    }
  }
  return change;
}

Json issue_to_json(const Issue &issue) {
  Json source_refs = Json::array();
  for (const auto &reference : issue.source_refs) {
    source_refs.push_back(source_reference_to_json(reference));
  }
  return Json{
      {"id", issue.id},
      {"matterId", issue.matter_id},
      {"title", issue.title},
      {"description", issue.description},
      {"status", to_string(issue.status)},
      {"sourceRefs", source_refs},
      {"createdAt", issue.created_at},
      {"updatedAt", issue.updated_at},
		{"originFindingId", issue.origin_finding_id ? Json(*issue.origin_finding_id) : Json(nullptr)},
		{"revision", issue.revision},
  };
}

std::optional<Issue> issue_from_json(const Json &value, const std::string &matter_id) {
  if (!value.is_object()) return std::nullopt;
  Issue issue;
  issue.id = value.value("id", std::string());
  issue.matter_id = value.value("matterId", matter_id);
  issue.title = value.value("title", std::string());
  issue.description = value.value("description", std::string());
  issue.status = issue_status_from_string(value.value("status", std::string())).value_or(IssueStatus::Open);
  if (value.contains("sourceRefs") && value.at("sourceRefs").is_array()) {
    for (const auto &entry : value.at("sourceRefs")) {
      if (auto reference = source_reference_from_json(entry)) {
        issue.source_refs.push_back(std::move(*reference));
      }
    }
  }
  issue.created_at = value.value("createdAt", std::string());
  issue.updated_at = value.value("updatedAt", std::string());
	if (value.contains("originFindingId") && value.at("originFindingId").is_string()) {
		issue.origin_finding_id = value.at("originFindingId").get<std::string>();
	}
	issue.revision = std::max(1LL, value.value("revision", 1LL));
  return issue;
}

Json note_to_json(const MatterNote &note) {
	Json sourceRefs = Json::array();
	for (const context::ResolvedSourceReference &reference : note.source_refs) {
		sourceRefs.push_back(source_reference_to_json(reference));
	}
  return Json{
      {"id", note.id},
      {"matterId", note.matter_id},
      {"content", note.content},
		{"createdAt", note.created_at},
		{"updatedAt", note.updated_at.empty() ? note.created_at : note.updated_at},
		{"title", note.title ? Json(*note.title) : Json(nullptr)},
		{"sourceRefs", std::move(sourceRefs)},
		{"origin", work_product_origin_to_json(note.origin)},
		{"revision", note.revision},
  };
}

MatterNote note_from_json(const Json &value, const std::string &matter_id) {
  MatterNote note;
  note.id = value.value("id", std::string());
  note.matter_id = value.value("matterId", matter_id);
  note.content = value.value("content", std::string());
  note.created_at = value.value("createdAt", std::string());
	note.updated_at = value.value("updatedAt", note.created_at);
	if (value.contains("title") && value.at("title").is_string()) {
		note.title = value.at("title").get<std::string>();
	}
	if (value.contains("sourceRefs") && value.at("sourceRefs").is_array()) {
		for (const Json &entry : value.at("sourceRefs")) {
			if (auto reference = source_reference_from_json(entry)) {
				note.source_refs.push_back(std::move(*reference));
			}
		}
	}
	if (value.contains("origin")) {
		note.origin = work_product_origin_from_json(value.at("origin"));
	} else {
		note.origin.kind = WorkProductOriginKind::Legacy;
	}
	note.revision = std::max(1LL, value.value("revision", 1LL));
  return note;
}

Json work_product_origin_to_json(const WorkProductOrigin &origin) {
	return Json{
		{"kind", to_string(origin.kind)},
		{"referenceId", origin.reference_id ? Json(*origin.reference_id) : Json(nullptr)},
	};
}

WorkProductOrigin work_product_origin_from_json(const Json &value) {
	WorkProductOrigin origin;
	if (value.is_object() == false) return origin;
	origin.kind = work_product_origin_kind_from_string(value.value("kind", std::string()))
			.value_or(WorkProductOriginKind::Manual);
	if (value.contains("referenceId") && value.at("referenceId").is_string()) {
		origin.reference_id = value.at("referenceId").get<std::string>();
	}
	return origin;
}

Json work_product_type_to_json(const WorkProductType &type) {
	return Json{
		{"id", type.id}, {"projectId", type.project_id}, {"name", type.name},
		{"description", type.description},
		{"instructions", type.instructions ? Json(*type.instructions) : Json(nullptr)},
		{"expectedStructure", type.expected_structure ? Json(*type.expected_structure) : Json(nullptr)},
		{"isCanonical", type.is_canonical}, {"revision", type.revision},
		{"createdAt", type.created_at}, {"updatedAt", type.updated_at},
	};
}

std::optional<WorkProductType> work_product_type_from_json(
		const Json &value, const std::string &projectId) {
	if (value.is_object() == false) return std::nullopt;
	WorkProductType type;
	type.id = value.value("id", std::string());
	type.project_id = value.value("projectId", projectId);
	type.name = value.value("name", std::string());
	type.description = value.value("description", std::string());
	if (value.contains("instructions") && value.at("instructions").is_string()) {
		type.instructions = value.at("instructions").get<std::string>();
	}
	if (value.contains("expectedStructure") && value.at("expectedStructure").is_string()) {
		type.expected_structure = value.at("expectedStructure").get<std::string>();
	}
	type.is_canonical = value.value("isCanonical", false);
	type.revision = std::max(1LL, value.value("revision", 1LL));
	type.created_at = value.value("createdAt", std::string());
	type.updated_at = value.value("updatedAt", type.created_at);
	if (type.id.empty() == true || type.project_id != projectId || type.name.empty() == true
			|| type.is_canonical == true) return std::nullopt;
	return type;
}

Json work_product_to_json(const WorkProduct &product) {
	Json sourceRefs = Json::array();
	for (const context::ResolvedSourceReference &reference : product.source_refs) {
		sourceRefs.push_back(source_reference_to_json(reference));
	}
	// The typed rows this work is made of, each with its own sources - the same
	// shape a task records them in, so nothing is re-derived on either side.
	Json findings = Json::array();
	for (const Finding &finding : product.findings) findings.push_back(finding_to_json(finding));
	Json obligations = Json::array();
	for (const Obligation &obligation : product.obligations)
		obligations.push_back(obligation_to_json(obligation));
	Json events = Json::array();
	for (const Event &event : product.events) events.push_back(event_to_json(event));
	Json changes = Json::array();
	for (const Change &change : product.changes) changes.push_back(change_to_json(change));
	return Json{
		{"id", product.id}, {"projectId", product.project_id}, {"typeId", product.type_id},
		{"title", product.title}, {"content", product.content},
		{"document", product.document ? Json(*product.document) : Json(nullptr)},
		{"sourceRefs", std::move(sourceRefs)},
		{"findings", std::move(findings)}, {"obligations", std::move(obligations)},
		{"events", std::move(events)}, {"changes", std::move(changes)},
		{"origin", work_product_origin_to_json(product.origin)}, {"revision", product.revision},
		{"createdAt", product.created_at}, {"updatedAt", product.updated_at},
	};
}

std::optional<WorkProduct> work_product_from_json(const Json &value, const std::string &projectId) {
	if (value.is_object() == false) return std::nullopt;
	WorkProduct product;
	product.id = value.value("id", std::string());
	product.project_id = value.value("projectId", projectId);
	product.type_id = value.value("typeId", std::string());
	product.title = value.value("title", std::string());
	product.content = value.value("content", std::string());
	if (value.contains("document") && value.at("document").is_string())
		product.document = value.at("document").get<std::string>();
	if (value.contains("sourceRefs") && value.at("sourceRefs").is_array()) {
		for (const Json &entry : value.at("sourceRefs")) {
			if (auto reference = source_reference_from_json(entry)) {
				product.source_refs.push_back(std::move(*reference));
			}
		}
	}
	// Work saved before the rows travelled with it simply has none; a row that
	// cannot be read is left out rather than guessed at.
	if (value.contains("findings") && value.at("findings").is_array())
		for (const Json &entry : value.at("findings"))
			if (auto finding = finding_from_json(entry)) product.findings.push_back(std::move(*finding));
	if (value.contains("obligations") && value.at("obligations").is_array())
		for (const Json &entry : value.at("obligations"))
			if (auto obligation = obligation_from_json(entry)) product.obligations.push_back(std::move(*obligation));
	if (value.contains("events") && value.at("events").is_array())
		for (const Json &entry : value.at("events"))
			if (auto event = event_from_json(entry)) product.events.push_back(std::move(*event));
	if (value.contains("changes") && value.at("changes").is_array())
		for (const Json &entry : value.at("changes"))
			if (auto change = change_from_json(entry)) product.changes.push_back(std::move(*change));
	product.origin = work_product_origin_from_json(value.value("origin", Json::object()));
	product.revision = std::max(1LL, value.value("revision", 1LL));
	product.created_at = value.value("createdAt", std::string());
	product.updated_at = value.value("updatedAt", product.created_at);
	if (product.id.empty() == true || product.project_id != projectId || product.type_id.empty() == true
			|| product.title.empty() == true || product.content.empty() == true) return std::nullopt;
	return product;
}

Json work_product_issue_data_to_json(const WorkProductIssueData &issue) {
	Json sourceRefs = Json::array();
	for (const context::ResolvedSourceReference &reference : issue.source_refs) {
		sourceRefs.push_back(source_reference_to_json(reference));
	}
	return Json{
		{"description", issue.description}, {"status", issue.status}, {"sourceRefs", std::move(sourceRefs)},
	};
}

std::optional<WorkProductIssueData> work_product_issue_data_from_json(const Json &value) {
	if (value.is_object() == false) return std::nullopt;
	WorkProductIssueData issue;
	issue.description = value.value("description", std::string());
	issue.status = value.value("status", std::string());
	if (value.contains("sourceRefs") && value.at("sourceRefs").is_array()) {
		for (const Json &entry : value.at("sourceRefs")) {
			if (auto reference = source_reference_from_json(entry)) {
				issue.source_refs.push_back(std::move(*reference));
			}
		}
	}
	if (issue.description.empty() == true || issue_status_from_string(issue.status) == std::nullopt) {
		return std::nullopt;
	}
	return issue;
}

Json work_product_revision_to_json(const WorkProductRevision &revision) {
	Json sourceRefs = Json::array();
	for (const context::ResolvedSourceReference &reference : revision.source_refs) {
		sourceRefs.push_back(source_reference_to_json(reference));
	}
	return Json{
		{"id", revision.id}, {"workProductId", revision.work_product_id},
		{"projectId", revision.project_id}, {"typeId", revision.type_id},
		{"title", revision.title},
		{"content", revision.content ? Json(*revision.content) : Json(nullptr)},
		{"state", revision.state ? Json(*revision.state) : Json(nullptr)},
		{"sourceRefs", std::move(sourceRefs)},
		{"issue", revision.issue ? work_product_issue_data_to_json(*revision.issue) : Json(nullptr)},
		{"origin", work_product_origin_to_json(revision.origin)},
		{"revision", revision.revision}, {"createdAt", revision.created_at},
	};
}

std::optional<WorkProductRevision> work_product_revision_from_json(
		const Json &value, const std::string &projectId) {
	if (value.is_object() == false) return std::nullopt;
	WorkProductRevision revision;
	revision.id = value.value("id", std::string());
	revision.work_product_id = value.value("workProductId", std::string());
	revision.project_id = value.value("projectId", projectId);
	revision.type_id = value.value("typeId", std::string());
	revision.title = value.value("title", std::string());
	if (value.contains("content") && value.at("content").is_string()) {
		revision.content = value.at("content").get<std::string>();
	}
	if (value.contains("state") && value.at("state").is_string()) {
		revision.state = value.at("state").get<std::string>();
	}
	if (value.contains("sourceRefs") && value.at("sourceRefs").is_array()) {
		for (const Json &entry : value.at("sourceRefs")) {
			if (auto reference = source_reference_from_json(entry)) {
				revision.source_refs.push_back(std::move(*reference));
			}
		}
	}
	if (value.contains("issue") && value.at("issue").is_object()) {
		revision.issue = work_product_issue_data_from_json(value.at("issue"));
	}
	revision.origin = work_product_origin_from_json(value.value("origin", Json::object()));
	revision.revision = std::max(1LL, value.value("revision", 1LL));
	revision.created_at = value.value("createdAt", std::string());
	if (revision.id.empty() == true || revision.work_product_id.empty() == true
			|| revision.project_id != projectId || revision.type_id.empty() == true
			|| revision.title.empty() == true || revision.created_at.empty() == true) {
		return std::nullopt;
	}
	return revision;
}

Json document_to_json(const MatterDocument &document) {
  return Json{
      {"id", document.id},
      {"matterId", document.matter_id},
      {"name", document.name},
      {"storedName", document.stored_name},
      {"fileType", documents::to_string(document.file_type)},
      {"sourceMode", to_string(document.source_mode)},
      {"addedAt", document.added_at},
      {"sizeBytes", document.size_bytes},
      {"lastModified", document.last_modified},
      {"fingerprint", document.fingerprint},
      {"extractionStatus", to_string(document.extraction_status)},
      {"extractedCharacterCount", document.extracted_character_count},
      {"pageCount", document.page_count ? Json(*document.page_count) : Json(nullptr)},
      {"extractorVersion", document.extractor_version},
			{"relativePath", document.relative_path},
			{"version", document.version},
			{"lifecycleState", to_string(document.lifecycle_state)},
			{"available", document.available},
			{"unavailableSince", document.unavailable_since},
			{"attentionDetail", document.attention_detail ? Json(*document.attention_detail) : Json(nullptr)},
			{"filesystemDeviceId",
				document.filesystem_identity.device_id ? Json(*document.filesystem_identity.device_id) : Json(nullptr)},
			{"filesystemFileId",
				document.filesystem_identity.file_id ? Json(*document.filesystem_identity.file_id) : Json(nullptr)},
			{"filesystemCreationTimeToken", document.filesystem_identity.creation_time_token
					? Json(*document.filesystem_identity.creation_time_token) : Json(nullptr)},
  };
}

MatterDocument document_from_json(const Json &value, const std::string &matter_id) {
  MatterDocument document;
  document.id = value.value("id", std::string());
  document.matter_id = value.value("matterId", matter_id);
  document.name = value.value("name", std::string());
  document.stored_name = value.value("storedName", std::string());
  document.file_type = document_format_from_string(
      value.value("fileType", std::string()), document.name);
  document.source_mode = document_source_mode_from_string(
      value.value("sourceMode", std::string("managed")));
  document.added_at = value.value("addedAt", std::string());
  document.size_bytes = value.value("sizeBytes", static_cast<long long>(0));
  document.last_modified = value.value("lastModified", std::string());
  document.fingerprint = value.value("fingerprint", std::string());
  document.extraction_status = document_extraction_state_from_string(
      value.value("extractionStatus", std::string("not_extracted")));
  document.extracted_character_count =
      value.value("extractedCharacterCount", static_cast<long long>(0));
  if (value.contains("pageCount") && value.at("pageCount").is_number_integer()) {
    document.page_count = value.at("pageCount").get<long long>();
  }
  document.extractor_version = value.value("extractorVersion", std::string());
	document.relative_path = value.value("relativePath", std::string());
	document.version = value.value("version", 0);
	document.lifecycle_state = document_lifecycle_state_from_string(value.value("lifecycleState", std::string()));
	document.available = value.value("available", true);
	document.unavailable_since = value.value("unavailableSince", std::string());
	if (value.contains("attentionDetail") && value.at("attentionDetail").is_string()) {
		document.attention_detail = value.at("attentionDetail").get<std::string>();
	}
	if (value.contains("filesystemDeviceId") && value.at("filesystemDeviceId").is_number_unsigned()) {
		document.filesystem_identity.device_id = value.at("filesystemDeviceId").get<std::uint64_t>();
	}
	if (value.contains("filesystemFileId") && value.at("filesystemFileId").is_number_unsigned()) {
		document.filesystem_identity.file_id = value.at("filesystemFileId").get<std::uint64_t>();
	}
	if (value.contains("filesystemCreationTimeToken") && value.at("filesystemCreationTimeToken").is_string()) {
		document.filesystem_identity.creation_time_token = value.at("filesystemCreationTimeToken").get<std::string>();
	}
  return document;
}

Json document_protection_to_json(const privacy::DocumentProtectionStatus &status) {
  return Json{
      {"id", status.document_id},
      {"documentId", status.document_id},
      {"state", privacy::to_string(status.state)},
      {"processedFingerprint", status.processed_fingerprint},
      {"processedExtractorVersion", status.processed_extractor_version},
      {"nerModelVersion", status.ner_model_version},
      {"detail", status.detail},
      {"updatedAt", status.updated_at},
  };
}

privacy::DocumentProtectionStatus document_protection_from_json(const Json &value) {
  privacy::DocumentProtectionStatus status;
  status.document_id = value.value("documentId", value.value("id", std::string()));
  status.state = privacy::document_protection_state_from_string(value.value("state", std::string()));
  status.processed_fingerprint = value.value("processedFingerprint", std::string());
  status.processed_extractor_version = value.value("processedExtractorVersion", std::string());
  status.ner_model_version = value.value("nerModelVersion", std::string());
  status.detail = value.value("detail", std::string());
  status.updated_at = value.value("updatedAt", std::string());
  return status;
}

Json entity_occurrence_to_json(const privacy::EntityOccurrence &occurrence) {
  return Json{
      {"documentId", occurrence.document_id},
      {"alias", occurrence.alias},
      {"mentionCount", occurrence.mention_count},
  };
}

privacy::EntityOccurrence entity_occurrence_from_json(const Json &value) {
  privacy::EntityOccurrence occurrence;
  occurrence.document_id = value.value("documentId", std::string());
  occurrence.alias = value.value("alias", std::string());
  occurrence.mention_count = value.value("mentionCount", static_cast<long long>(0));
  return occurrence;
}

Json entity_to_json(const privacy::ProjectEntity &entity) {
  Json occurrences = Json::array();
  for (const auto &occurrence : entity.occurrences) occurrences.push_back(entity_occurrence_to_json(occurrence));
  return Json{
      {"id", entity.id},
      {"category", protected_api::papinian::to_string(entity.category)},
      {"pseudonym", entity.pseudonym},
      {"canonicalValue", entity.canonical_value},
      {"aliases", entity.aliases},
      {"lockedAliases", entity.locked_aliases},
      {"rejectedAliases", entity.rejected_aliases},
      {"contextKeys", entity.context_keys},
      {"occurrences", std::move(occurrences)},
      {"mergedIntoEntityId", entity.merged_into_entity_id ? Json(*entity.merged_into_entity_id) : Json(nullptr)},
      {"createdAt", entity.created_at},
      {"updatedAt", entity.updated_at},
  };
}

std::optional<protected_api::papinian::EntityType> entity_type_from_wire(const std::string &value) {
  for (const auto type : protected_api::papinian::all_entity_types()) {
    if (value == protected_api::papinian::to_string(type)) return type;
  }
  return std::nullopt;
}

privacy::ProjectEntity entity_from_json(const Json &value) {
  privacy::ProjectEntity entity;
  entity.id = value.value("id", std::string());
  entity.category =
      entity_type_from_wire(value.value("category", std::string())).value_or(protected_api::papinian::EntityType::Person);
  entity.pseudonym = value.value("pseudonym", std::string());
  entity.canonical_value = value.value("canonicalValue", std::string());
  if (value.contains("aliases") && value.at("aliases").is_array()) {
    entity.aliases = value.at("aliases").get<std::vector<std::string>>();
  }
  if (value.contains("rejectedAliases") && value.at("rejectedAliases").is_array()) {
    for (const Json &entry : value.at("rejectedAliases"))
      if (entry.is_string()) entity.rejected_aliases.push_back(entry.get<std::string>());
  }
  if (value.contains("lockedAliases") && value.at("lockedAliases").is_array()) {
    entity.locked_aliases = value.at("lockedAliases").get<std::vector<std::string>>();
  }
  if (value.contains("contextKeys") && value.at("contextKeys").is_array()) {
    entity.context_keys = value.at("contextKeys").get<std::vector<std::string>>();
  }
  if (value.contains("occurrences") && value.at("occurrences").is_array()) {
    for (const auto &occurrence : value.at("occurrences")) entity.occurrences.push_back(entity_occurrence_from_json(occurrence));
  }
  if (value.contains("mergedIntoEntityId") && value.at("mergedIntoEntityId").is_string()) {
    entity.merged_into_entity_id = value.at("mergedIntoEntityId").get<std::string>();
  }
  entity.created_at = value.value("createdAt", std::string());
  entity.updated_at = value.value("updatedAt", std::string());
  return entity;
}

Json entity_ambiguity_to_json(const privacy::EntityAmbiguity &ambiguity) {
  return Json{
      {"id", ambiguity.id},
      {"category", protected_api::papinian::to_string(ambiguity.category)},
      {"entityAId", ambiguity.entity_a_id},
      {"entityBId", ambiguity.entity_b_id},
      {"evidenceAliasA", ambiguity.evidence_alias_a},
      {"evidenceAliasB", ambiguity.evidence_alias_b},
      {"status", privacy::to_string(ambiguity.status)},
      {"createdAt", ambiguity.created_at},
      {"resolvedAt", ambiguity.resolved_at},
  };
}

privacy::EntityAmbiguity entity_ambiguity_from_json(const Json &value) {
  privacy::EntityAmbiguity ambiguity;
  ambiguity.id = value.value("id", std::string());
  ambiguity.category =
      entity_type_from_wire(value.value("category", std::string())).value_or(protected_api::papinian::EntityType::Person);
  ambiguity.entity_a_id = value.value("entityAId", std::string());
  ambiguity.entity_b_id = value.value("entityBId", std::string());
  ambiguity.evidence_alias_a = value.value("evidenceAliasA", std::string());
  ambiguity.evidence_alias_b = value.value("evidenceAliasB", std::string());
  ambiguity.status = privacy::ambiguity_status_from_string(value.value("status", std::string()));
  ambiguity.created_at = value.value("createdAt", std::string());
  ambiguity.resolved_at = value.value("resolvedAt", std::string());
  return ambiguity;
}

Json context_item_source_to_json(const context::ContextItemSource &source) {
  return Json{
      {"kind", context::to_string(source.kind)},
      {"documentReference", source.document_reference ? source_reference_to_json(*source.document_reference) : Json(nullptr)},
      {"chatOperationId", source.chat_operation_id},
  };
}

context::ContextItemSource context_item_source_from_json(const Json &value) {
  context::ContextItemSource source;
  source.kind = context::context_source_kind_from_string(value.value("kind", std::string()));
  if (value.contains("documentReference") && value.at("documentReference").is_object()) {
    source.document_reference = source_reference_from_json(value.at("documentReference"));
  }
  source.chat_operation_id = value.value("chatOperationId", std::string());
  return source;
}

Json context_item_to_json(const context::ContextItem &item) {
  return Json{
      {"id", item.id},
      {"section", context::to_string(item.section)},
      {"label", item.label},
      {"value", item.value},
      {"authority", context::to_string(item.authority)},
      {"source", item.source ? context_item_source_to_json(*item.source) : Json(nullptr)},
      {"revision", item.revision},
      {"createdAt", item.created_at},
      {"updatedAt", item.updated_at},
      {"updatedByActorId", item.updated_by_actor_id ? Json(*item.updated_by_actor_id) : Json(nullptr)},
      {"entityId", item.entity_id ? Json(*item.entity_id) : Json(nullptr)},
	  {"kind", context::to_string(item.kind)},
	  {"isPinned", item.is_pinned},
	  {"isLocked", item.is_locked},
	  {"isExcludedFromAi", item.is_excluded_from_ai},
  };
}

context::ContextItem context_item_from_json(const Json &value) {
  context::ContextItem item;
  item.id = value.value("id", std::string());
  item.section = context::context_section_from_string(value.value("section", std::string()));
  item.label = value.value("label", std::string());
  item.value = value.value("value", std::string());
  item.authority = context::context_item_authority_from_string(value.value("authority", std::string()));
	item.kind = value.contains("kind")
		? context::context_item_kind_from_string(value.value("kind", std::string()))
		: context::default_context_item_kind(item.section);
	item.is_pinned = value.value("isPinned", item.section == context::ContextSection::PinnedInformation);
	item.is_locked = value.value("isLocked", item.authority == context::ContextItemAuthority::Locked);
	item.is_excluded_from_ai = value.value("isExcludedFromAi", false);
  if (value.contains("source") && value.at("source").is_object()) {
    item.source = context_item_source_from_json(value.at("source"));
  }
  item.revision = value.value("revision", static_cast<long long>(1));
  item.created_at = value.value("createdAt", std::string());
  item.updated_at = value.value("updatedAt", std::string());
  if (value.contains("updatedByActorId") && value.at("updatedByActorId").is_string()) {
    item.updated_by_actor_id = value.at("updatedByActorId").get<std::string>();
  }
  if (value.contains("entityId") && value.at("entityId").is_string()) {
    item.entity_id = value.at("entityId").get<std::string>();
  }
  return item;
}

// Empty and zero fields are left out: most events carry only a few.
Json document_event_to_json(const documents::DocumentEvent &event) {
  Json value{{"id", event.id}, {"documentId", event.document_id}, {"kind", documents::to_string(event.kind)},
             {"at", event.at}, {"name", event.name}, {"relativePath", event.relative_path}};
  const auto text = [&](const char *key, const std::string &field) { if (!field.empty()) value[key] = field; };
  const auto number = [&](const char *key, long long field) { if (field != 0) value[key] = field; };
  text("fileModifiedAt", event.file_modified_at);
  text("previousName", event.previous_name);
  text("previousRelativePath", event.previous_relative_path);
  number("version", event.version);
  number("previousVersion", event.previous_version);
  text("fingerprint", event.fingerprint);
  text("previousFingerprint", event.previous_fingerprint);
  number("sizeBytes", event.size_bytes);
  number("previousSizeBytes", event.previous_size_bytes);
  text("actor", event.actor);
  text("operationId", event.operation_id);
  text("actorId", event.actor_id);
  text("deviceId", event.device_id);
  return value;
}

// Read leniently: an event of a kind this build does not know, or without an
// id, is left out rather than refusing the Project.
std::optional<documents::DocumentEvent> document_event_from_json(const Json &value) {
  if (!value.is_object()) return std::nullopt;
  const auto kind = documents::document_event_kind_from_string(value.value("kind", std::string()));
  documents::DocumentEvent event;
  event.id = value.value("id", std::string());
  if (!kind || event.id.empty()) return std::nullopt;
  event.kind = *kind;
  event.document_id = value.value("documentId", std::string());
  event.at = value.value("at", std::string());
  event.file_modified_at = value.value("fileModifiedAt", std::string());
  event.name = value.value("name", std::string());
  event.relative_path = value.value("relativePath", std::string());
  event.previous_name = value.value("previousName", std::string());
  event.previous_relative_path = value.value("previousRelativePath", std::string());
  event.version = value.value("version", 0);
  event.previous_version = value.value("previousVersion", 0);
  event.fingerprint = value.value("fingerprint", std::string());
  event.previous_fingerprint = value.value("previousFingerprint", std::string());
  event.size_bytes = value.value("sizeBytes", 0LL);
  event.previous_size_bytes = value.value("previousSizeBytes", 0LL);
  event.actor = value.value("actor", std::string());
  event.operation_id = value.value("operationId", std::string());
  event.actor_id = value.value("actorId", std::string());
  event.device_id = value.value("deviceId", std::string());
  return event;
}

Json context_item_revision_to_json(const context::ContextItemRevision &revision) {
  return Json{
      {"id", revision.id},
      {"itemId", revision.item_id},
      {"value", revision.value},
      {"authority", context::to_string(revision.authority)},
      {"actorId", revision.actor_id},
      {"createdAt", revision.created_at},
  };
}

context::ContextItemRevision context_item_revision_from_json(const Json &value) {
  context::ContextItemRevision revision;
  revision.id = value.value("id", std::string());
  revision.item_id = value.value("itemId", std::string());
  revision.value = value.value("value", std::string());
  revision.authority = context::context_item_authority_from_string(value.value("authority", std::string()));
  revision.actor_id = value.value("actorId", std::string());
  revision.created_at = value.value("createdAt", std::string());
  return revision;
}

Json context_suggestion_to_json(const context::ContextSuggestion &suggestion) {
  return Json{
      {"id", suggestion.id},
      {"section", context::to_string(suggestion.section)},
      {"label", suggestion.label},
      {"value", suggestion.value},
      {"targetItemId", suggestion.target_item_id ? Json(*suggestion.target_item_id) : Json(nullptr)},
      {"source", context_item_source_to_json(suggestion.source)},
	  {"entityId", suggestion.entity_id ? Json(*suggestion.entity_id) : Json(nullptr)},
      {"status", context::to_string(suggestion.status)},
      {"reason", context::to_string(suggestion.reason)},
      {"createdAt", suggestion.created_at},
      {"resolvedAt", suggestion.resolved_at},
      {"resolvedByActorId", suggestion.resolved_by_actor_id ? Json(*suggestion.resolved_by_actor_id) : Json(nullptr)},
  };
}

context::ContextSuggestion context_suggestion_from_json(const Json &value) {
  context::ContextSuggestion suggestion;
  suggestion.id = value.value("id", std::string());
  suggestion.section = context::context_section_from_string(value.value("section", std::string()));
  suggestion.label = value.value("label", std::string());
  suggestion.value = value.value("value", std::string());
  if (value.contains("targetItemId") && value.at("targetItemId").is_string()) {
    suggestion.target_item_id = value.at("targetItemId").get<std::string>();
  }
  if (value.contains("source") && value.at("source").is_object()) {
    suggestion.source = context_item_source_from_json(value.at("source"));
  }
	if (value.contains("entityId") && value.at("entityId").is_string()) {
		suggestion.entity_id = value.at("entityId").get<std::string>();
	}
  suggestion.status = context::context_suggestion_status_from_string(value.value("status", std::string()));
  if (value.contains("reason") && value.at("reason").is_string())
    suggestion.reason = context::context_suggestion_reason_from_string(value.at("reason").get<std::string>());
  suggestion.created_at = value.value("createdAt", std::string());
  suggestion.resolved_at = value.value("resolvedAt", std::string());
  if (value.contains("resolvedByActorId") && value.at("resolvedByActorId").is_string()) {
    suggestion.resolved_by_actor_id = value.at("resolvedByActorId").get<std::string>();
  }
  return suggestion;
}

Json context_item_snapshot_to_json(const context::ContextItemSnapshot &snapshot) {
	return Json{
			{"itemId", snapshot.item_id},
			{"revision", snapshot.revision},
			{"section", context::to_string(snapshot.section)},
			{"kind", context::to_string(snapshot.kind)},
			{"label", snapshot.label},
			{"value", snapshot.value},
			{"authority", context::to_string(snapshot.authority)},
			{"source", snapshot.source ? context_item_source_to_json(*snapshot.source) : Json(nullptr)},
			{"entityId", snapshot.entity_id ? Json(*snapshot.entity_id) : Json(nullptr)},
			{"isPinned", snapshot.is_pinned}, {"isLocked", snapshot.is_locked},
			{"isExcludedFromAi", snapshot.is_excluded_from_ai},
	};
}

context::ContextItemSnapshot context_item_snapshot_from_json(const Json &value) {
	context::ContextItemSnapshot snapshot;
	snapshot.item_id = value.value("itemId", std::string());
	snapshot.revision = value.value("revision", 1LL);
	snapshot.section = context::context_section_from_string(value.value("section", std::string()));
	snapshot.kind = context::context_item_kind_from_string(value.value("kind", std::string()));
	snapshot.label = value.value("label", std::string());
	snapshot.value = value.value("value", std::string());
	snapshot.authority = context::context_item_authority_from_string(value.value("authority", std::string()));
	if (value.contains("source") && value.at("source").is_object()) {
		snapshot.source = context_item_source_from_json(value.at("source"));
	}
	if (value.contains("entityId") && value.at("entityId").is_string()) {
		snapshot.entity_id = value.at("entityId").get<std::string>();
	}
	snapshot.is_pinned = value.value("isPinned", false);
	snapshot.is_locked = value.value("isLocked", false);
	snapshot.is_excluded_from_ai = value.value("isExcludedFromAi", false);
	return snapshot;
}

Json tool_call_evidence_to_json(const tools::ToolCallEvidence &evidence) {
	return Json{
		{"id", evidence.id},
		{"toolId", evidence.tool_id},
		{"capability", evidence.capability},
		{"effect", tools::to_string(evidence.effect)},
		{"outcome", tools::to_string(evidence.outcome)},
		{"effectiveMode", tools::to_string(evidence.effective_mode)},
		{"explicitApprovalRequired", evidence.explicit_approval_required},
		{"explicitApprovalGranted", evidence.explicit_approval_granted.has_value()
			? Json(*evidence.explicit_approval_granted) : Json(nullptr)},
		{"startedAt", evidence.started_at},
		{"completedAt", evidence.completed_at},
		{"normalizedIntentDigest", evidence.normalized_intent_digest},
		{"errorCategory", evidence.error_category.empty() ? Json(nullptr) : Json(evidence.error_category)},
		{"rawQueryUtf8Bytes", evidence.raw_query_utf8_bytes ? Json(*evidence.raw_query_utf8_bytes) : Json(nullptr)},
		{"canonicalQueryUtf8Bytes",
			evidence.canonical_query_utf8_bytes ? Json(*evidence.canonical_query_utf8_bytes) : Json(nullptr)},
		{"intentCanonicalized",
			evidence.intent_canonicalized ? Json(*evidence.intent_canonicalized) : Json(nullptr)},
		{"estimatedInputTokens",
			evidence.estimated_input_tokens ? Json(*evidence.estimated_input_tokens) : Json(nullptr)},
		{"modelContextWindowTokens",
			evidence.model_context_window_tokens ? Json(*evidence.model_context_window_tokens) : Json(nullptr)},
		{"budgetClassification",
			evidence.budget_classification ? Json(*evidence.budget_classification) : Json(nullptr)},
		{"externalProviderId", evidence.external_provider_id ? Json(*evidence.external_provider_id) : Json(nullptr)},
		{"externalModelId", evidence.external_model_id ? Json(*evidence.external_model_id) : Json(nullptr)},
		{"externalUsage", evidence.external_usage ? usage_to_json(*evidence.external_usage) : Json(nullptr)},
	};
}

std::optional<tools::ToolCallEvidence> tool_call_evidence_from_json(const Json &value) {
	if (!value.is_object()) return std::nullopt;
	const std::optional<tools::ToolEffectClass> effect = tools::tool_effect_class_from_string(
		value.value("effect", std::string()));
	const std::optional<tools::ToolOutcome> outcome = tools::tool_outcome_from_string(
		value.value("outcome", std::string()));
	const std::optional<tools::ProjectActionMode> effectiveMode = tools::project_action_mode_from_string(
		value.value("effectiveMode", std::string()));
	if (!effect || !outcome || !effectiveMode) return std::nullopt;
	tools::ToolCallEvidence evidence;
	evidence.id = value.value("id", std::string());
	evidence.tool_id = value.value("toolId", std::string());
	evidence.capability = value.value("capability", std::string());
	evidence.effect = *effect;
	evidence.outcome = *outcome;
	evidence.effective_mode = *effectiveMode;
	evidence.explicit_approval_required = value.value("explicitApprovalRequired", false);
	if (value.contains("explicitApprovalGranted") && value.at("explicitApprovalGranted").is_boolean()) {
		evidence.explicit_approval_granted = value.at("explicitApprovalGranted").get<bool>();
	}
	evidence.started_at = value.value("startedAt", std::string());
	evidence.completed_at = value.value("completedAt", std::string());
	evidence.normalized_intent_digest = value.value("normalizedIntentDigest", std::string());
	if (value.contains("errorCategory") && value.at("errorCategory").is_string()) {
		evidence.error_category = value.at("errorCategory").get<std::string>();
	}
	if (value.contains("rawQueryUtf8Bytes") && value.at("rawQueryUtf8Bytes").is_number_unsigned()) {
		evidence.raw_query_utf8_bytes = value.at("rawQueryUtf8Bytes").get<std::size_t>();
	}
	if (value.contains("canonicalQueryUtf8Bytes") && value.at("canonicalQueryUtf8Bytes").is_number_unsigned()) {
		evidence.canonical_query_utf8_bytes = value.at("canonicalQueryUtf8Bytes").get<std::size_t>();
	}
	if (value.contains("intentCanonicalized") && value.at("intentCanonicalized").is_boolean()) {
		evidence.intent_canonicalized = value.at("intentCanonicalized").get<bool>();
	}
	if (value.contains("estimatedInputTokens") && value.at("estimatedInputTokens").is_number_unsigned()) {
		evidence.estimated_input_tokens = value.at("estimatedInputTokens").get<std::size_t>();
	}
	if (value.contains("modelContextWindowTokens") && value.at("modelContextWindowTokens").is_number_unsigned()) {
		evidence.model_context_window_tokens = value.at("modelContextWindowTokens").get<std::size_t>();
	}
	if (value.contains("budgetClassification") && value.at("budgetClassification").is_string()) {
		evidence.budget_classification = value.at("budgetClassification").get<std::string>();
	}
	if (value.contains("externalProviderId") && value.at("externalProviderId").is_string())
		evidence.external_provider_id = value.at("externalProviderId").get<std::string>();
	if (value.contains("externalModelId") && value.at("externalModelId").is_string())
		evidence.external_model_id = value.at("externalModelId").get<std::string>();
	if (value.contains("externalUsage") && value.at("externalUsage").is_object())
		evidence.external_usage = usage_from_json(value.at("externalUsage"));
	return evidence;
}

Json operation_event_to_json(const AgentOperationEvent &event) {
	return Json{{"id", event.id}, {"sequence", event.sequence}, {"type", event.type},
		{"createdAt", event.created_at}, {"operationRevision", event.operation_revision},
		{"message", event.message},
		{"subjectId", event.subject_id ? Json(*event.subject_id) : Json(nullptr)},
		{"outcome", event.outcome ? Json(*event.outcome) : Json(nullptr)}};
}

std::optional<AgentOperationEvent> operation_event_from_json(const Json &value) {
	if (!value.is_object()) return std::nullopt;
	AgentOperationEvent event;
	event.id = value.value("id", std::string());
	event.sequence = value.value("sequence", std::uint64_t(0));
	event.type = value.value("type", std::string());
	event.created_at = value.value("createdAt", std::string());
	event.operation_revision = value.value("operationRevision", std::uint64_t(0));
	event.message = value.value("message", std::string());
	if (value.contains("subjectId") && value.at("subjectId").is_string())
		event.subject_id = value.at("subjectId").get<std::string>();
	if (value.contains("outcome") && value.at("outcome").is_string())
		event.outcome = value.at("outcome").get<std::string>();
	if (event.id.empty() || event.type.empty() || event.created_at.empty()) return std::nullopt;
	return event;
}

Json operation_approval_to_json(const std::optional<tools::ToolApprovalRequest> &approval) {
	if (!approval) return nullptr;
	return Json{{"approvalId", approval->approval_id}, {"operationId", approval->operation_id},
		{"projectId", approval->project_id}, {"summary", approval->summary},
		{"effect", tools::to_string(approval->effect)}, {"toolId", approval->tool_id},
		{"intentDigest", approval->intent_digest},
		{"targetDocumentId", approval->target_document_id ? Json(*approval->target_document_id) : Json(nullptr)},
		{"targetVersion", approval->target_version ? Json(*approval->target_version) : Json(nullptr)},
		{"targetFingerprint", approval->target_fingerprint ? Json(*approval->target_fingerprint) : Json(nullptr)},
		{"changeKind", approval->change_kind}, {"removedText", approval->removed_text},
		{"addedText", approval->added_text}, {"columns", approval->columns}, {"rows", approval->rows},
		// The whole change the person is asked to approve, so a Project reopened
		// after a quit still shows exactly what the approval covers.
		{"change", approval->change_detail_json.empty() ? Json(nullptr)
			: Json::parse(approval->change_detail_json, nullptr, false)}};
}

std::optional<tools::ToolApprovalRequest> operation_approval_from_json(const Json &value) {
	if (!value.is_object()) return std::nullopt;
	const auto effect = tools::tool_effect_class_from_string(value.value("effect", std::string()));
	if (!effect) return std::nullopt;
	tools::ToolApprovalRequest approval;
	approval.approval_id = value.value("approvalId", std::string());
	approval.operation_id = value.value("operationId", std::string());
	approval.project_id = value.value("projectId", std::string());
	approval.summary = value.value("summary", std::string());
	approval.effect = *effect;
	approval.tool_id = value.value("toolId", std::string());
	approval.intent_digest = value.value("intentDigest", std::string());
	if (value.contains("targetDocumentId") && value.at("targetDocumentId").is_string())
		approval.target_document_id = value.at("targetDocumentId").get<std::string>();
	if (value.contains("targetVersion") && value.at("targetVersion").is_number_integer())
		approval.target_version = value.at("targetVersion").get<int>();
	if (value.contains("targetFingerprint") && value.at("targetFingerprint").is_string())
		approval.target_fingerprint = value.at("targetFingerprint").get<std::string>();
	approval.change_kind = value.value("changeKind", std::string());
	approval.removed_text = value.value("removedText", std::string());
	approval.added_text = value.value("addedText", std::string());
	if (value.contains("columns") && value.at("columns").is_array())
		for (const Json &column : value.at("columns")) if (column.is_string()) approval.columns.push_back(column.get<std::string>());
	if (value.contains("rows") && value.at("rows").is_array())
		for (const Json &row : value.at("rows")) {
			if (!row.is_array()) continue;
			std::vector<std::string> cells;
			for (const Json &cell : row) if (cell.is_string()) cells.push_back(cell.get<std::string>());
			approval.rows.push_back(std::move(cells));
		}
	if (value.contains("change") && value.at("change").is_object()) approval.change_detail_json = value.at("change").dump();
	if (approval.approval_id.empty() || approval.operation_id.empty() || approval.project_id.empty())
		return std::nullopt;
	return approval;
}

// A question the model put to the lawyer, as the lawyer was shown it and as
// they answered it.
Json lawyer_question_to_json(const LawyerQuestionRecord &record) {
	return Json{{"id", record.id}, {"title", record.title ? Json(*record.title) : Json(nullptr)},
		{"questions", record.questions}, {"answers", record.answers}, {"state", record.state},
		{"askedAt", record.asked_at}, {"answeredAt", record.answered_at ? Json(*record.answered_at) : Json(nullptr)}};
}

// An entry that cannot be read is dropped, never guessed at: it belongs to a
// damaged file or a newer Singularity, and the rest of the task still loads.
std::optional<LawyerQuestionRecord> lawyer_question_from_json(const Json &value) {
	if (!value.is_object()) return std::nullopt;
	const auto text = [&](const char *key) -> std::optional<std::string> {
		if (!value.contains(key) || !value.at(key).is_string()) return std::nullopt;
		return value.at(key).get<std::string>();
	};
	LawyerQuestionRecord record;
	record.id = text("id").value_or(std::string());
	record.title = text("title");
	if (!value.contains("questions") || !value.at("questions").is_array()) return std::nullopt;
	record.questions = value.at("questions");
	if (value.contains("answers") && value.at("answers").is_object()) record.answers = value.at("answers");
	record.state = text("state").value_or(std::string("asked"));
	record.asked_at = text("askedAt").value_or(std::string());
	record.answered_at = text("answeredAt");
	if (record.id.empty()) return std::nullopt;
	if (record.state != "asked" && record.state != "answered" && record.state != "left_to_model") return std::nullopt;
	return record;
}

Json task_to_json(const MatterTask &task) {
  Json source_references = Json::array();
  for (const auto &reference : task.source_references) {
    source_references.push_back(source_reference_to_json(reference));
  }

  Json quoted_passages = Json::array();
  for (const context::QuotedPassage &quoted : task.quoted_passages) {
    quoted_passages.push_back(Json{{"quote", quoted.quote}, {"documentId", quoted.document_id},
        {"chunkId", quoted.chunk_id}, {"documentName", quoted.document_name},
        {"verified", quoted.verified}, {"start", static_cast<long long>(quoted.start)},
        {"length", static_cast<long long>(quoted.length)}, {"rebound", quoted.rebound},
        {"attributed", quoted.attributed}});
  }  Json context_snapshot = Json::array();
  for (const context::ContextItemSnapshot &snapshot : task.context_snapshot) {
    context_snapshot.push_back(context_item_snapshot_to_json(snapshot));
  }
	Json tool_call_evidence = Json::array();
	for (const tools::ToolCallEvidence &evidence : task.tool_call_evidence) {
		tool_call_evidence.push_back(tool_call_evidence_to_json(evidence));
	}
	Json external_evidence = Json::array();
	for (const research::ExternalEvidenceReference &evidence : task.external_evidence)
		external_evidence.push_back(research::external_evidence_to_json(evidence));
	Json document_role_bindings = Json::array();
	for (const documents::DocumentRoleAssignment &assignment : task.document_role_bindings)
		document_role_bindings.push_back(documents::document_role_assignment_to_json(assignment));
	Json document_action_evidence = Json::array();
	for (const documents::AppliedDocumentActionEvidence &evidence : task.document_action_evidence)
		document_action_evidence.push_back(documents::document_action_evidence_to_json(evidence));
	Json unavailable_source_documents = Json::array();
	for (const UnavailableSourceDocument &entry : task.unavailable_source_documents) {
		unavailable_source_documents.push_back({{"documentId", entry.document_id},
			{"documentName", entry.document_name}, {"reason", entry.reason}});
	}
	Json operation_events = Json::array();
	for (const AgentOperationEvent &event : task.operation_events)
		operation_events.push_back(operation_event_to_json(event));
	Json lawyer_questions = Json::array();
	for (const LawyerQuestionRecord &record : task.lawyer_questions)
		lawyer_questions.push_back(lawyer_question_to_json(record));
	Json document_operation_checkpoint = nullptr;
	if (task.document_operation_checkpoint) {
		Json checkpoint_evidence = Json::array();
		for (const auto &entry : task.document_operation_checkpoint->external_evidence)
			checkpoint_evidence.push_back(research::external_evidence_to_json(entry));
		document_operation_checkpoint = {
			{"schemaVersion", task.document_operation_checkpoint->schema_version},
			{"stage", task.document_operation_checkpoint->stage},
			{"draft", task.document_operation_checkpoint->draft},
			{"mutationCommit", task.document_operation_checkpoint->mutation_commit},
			{"externalEvidence", std::move(checkpoint_evidence)},
			{"progress", {{"modelTurnsUsed", task.document_operation_checkpoint->progress.model_turns_used},
				{"providerRequestsUsed", task.document_operation_checkpoint->progress.provider_requests_used},
				{"successfulToolCallsUsed", task.document_operation_checkpoint->progress.successful_tool_calls_used},
				{"searchCallsUsed", task.document_operation_checkpoint->progress.search_calls_used},
				{"openCallsUsed", task.document_operation_checkpoint->progress.open_calls_used},
				{"itemDeltasProposed", task.document_operation_checkpoint->progress.item_deltas_proposed},
				{"itemDeltasAccepted", task.document_operation_checkpoint->progress.item_deltas_accepted},
				{"itemDeltasRejected", task.document_operation_checkpoint->progress.item_deltas_rejected},
				{"incompleteResponseRepairs", task.document_operation_checkpoint->progress.incomplete_response_repairs},
				{"protectedResponseRepairs", task.document_operation_checkpoint->progress.protected_response_repairs},
				{"fullActionRegenerations", task.document_operation_checkpoint->progress.full_action_regenerations},
				{"documentMutationsCommitted", task.document_operation_checkpoint->progress.document_mutations_committed}}}};
	}
  Json findings = Json::array();
  for (const auto &finding : task.findings) {
    findings.push_back(finding_to_json(finding));
  }
  Json obligations = Json::array();
  for (const auto &obligation : task.obligations) {
    obligations.push_back(obligation_to_json(obligation));
  }
  Json events = Json::array();
  for (const auto &event : task.events) {
    events.push_back(event_to_json(event));
  }
  Json changes = Json::array();
  for (const auto &change : task.changes) {
    changes.push_back(change_to_json(change));
  }
  Json value = Json{
      {"id", task.id},
      {"createdAt", task.created_at},
	  {"operationType", task.operation_type}, {"operationRole", task.operation_role},
	  {"agentOperationId", task.agent_operation_id ? Json(*task.agent_operation_id) : Json(nullptr)},
	  {"operationExecutionState", task.operation_execution_state},
	  {"operationRevision", task.operation_revision},
	  {"turnContractRevision", task.turn_contract_revision},
	  {"operationStage", task.operation_stage},
	  {"operationStartedAt", task.operation_started_at},
	  {"operationUpdatedAt", task.operation_updated_at},
	  {"operationActivity", task.operation_activity},
	  {"operationCancelRequested", task.operation_cancel_requested},
	  {"operationResumeAllowed", task.operation_resume_allowed},
	  {"operationDeviceId", task.operation_device_id},
	  {"agentId", task.agent_id},
	  {"agentInitiated", task.agent_initiated},
	  {"authorActorId", task.author_actor_id},
	  {"authorName", task.author_name},
	  {"mentions", task.mentions},
	  {"answersMessageId", task.answers_message_id},
	  {"operationApproval", operation_approval_to_json(task.operation_approval)},
	  {"rejectedApproval", operation_approval_to_json(task.rejected_approval)},
	  {"lawyerQuestions", std::move(lawyer_questions)},
	  {"operationEvents", std::move(operation_events)},
	  {"documentOperationCheckpoint", std::move(document_operation_checkpoint)},
	  {"aiSetupSource", task.ai_setup_source},
	  {"requestedProviderId", task.requested_provider_id ? Json(*task.requested_provider_id) : Json(nullptr)},
	  {"requestedModelId", task.requested_model_id ? Json(*task.requested_model_id) : Json(nullptr)},
      {"chatId", task.chat_id ? Json(*task.chat_id) : Json(nullptr)},
      {"prompt", task.prompt},
      {"legalTaskPresetId", task.legal_task_preset_id},
      {"legalTaskPresetVersion", task.legal_task_preset_version},
      {"promptProfileId", task.prompt_profile_id},
      {"promptProfileVersion", task.prompt_profile_version},
      {"selectedDocumentIds", task.selected_document_ids},
	  {"professionalCapability", task.professional_capability},
	  {"professionalOperations", task.professional_operations},
	  {"professionalSourceDocumentIds", task.professional_source_document_ids},
	  {"professionalTargetDocumentId", task.professional_target_document_id
		  ? Json(*task.professional_target_document_id) : Json(nullptr)},
	  {"professionalDestinationFormat", task.professional_destination_format},
	  {"professionalDestinationFileName", task.professional_destination_file_name
		  ? Json(*task.professional_destination_file_name) : Json(nullptr)},
	  {"professionalWriteIntent", task.professional_write_intent},
      {"attempt", task.attempt},
      {"rootTaskId", task.root_task_id ? Json(*task.root_task_id) : Json(nullptr)},
      // Always written (even as null) so task_from_json() can tell "captured
      // as empty" apart from "predates capture entirely" purely by whether
      // this key is present at all - see MatterTask::
      // matter_instructions_snapshot_captured.
      {"matterInstructionsSnapshot",
       task.matter_instructions_snapshot ? Json(*task.matter_instructions_snapshot) : Json(nullptr)},
      {"responseText", task.response_text ? Json(*task.response_text) : Json(nullptr)},
      {"providerId", task.provider_id ? Json(*task.provider_id) : Json(nullptr)},
      {"modelId", task.model_id ? Json(*task.model_id) : Json(nullptr)},
	  {"executionKind", task.execution_kind}, {"runtimeId", task.runtime_id},
	  {"runtimeVersion", task.runtime_version}, {"runtimeSourceCommit", task.runtime_source_commit},
	  {"runtimePlatform", task.runtime_platform}, {"runtimeBackend", task.runtime_backend},
	  {"runtimeModelId", task.runtime_model_id},
      {"protectedApiMode", task.protected_api_mode},
	  {"protectedApiApplied", task.protected_api_applied},
      {"privacyMetadata", privacy_metadata_to_json(task.privacy_metadata)},
      {"status", task_status_to_string(task.status)},
      {"stoppedByUser", task.stopped_by_user},
      {"error", task.error ? Json(*task.error) : Json(nullptr)},
      {"errorCode", task.error_code ? Json(providers::to_string(*task.error_code)) : Json(nullptr)},
      {"retryable",
       task.error_code ? Json(providers::is_retryable(*task.error_code)) : Json(nullptr)},
      {"usage", usage_to_json(task.usage)},
      {"sourceReferences", source_references},
      {"quotedPassages", quoted_passages},
      {"contextSnapshot", context_snapshot},
	  {"toolCallEvidence", tool_call_evidence},
	  {"researchMode", task.research_mode},
	  {"externalEvidence", external_evidence},
	  {"documentRoleBindings", document_role_bindings},
	  {"documentActionEvidence", document_action_evidence},
	  {"resultContextItemIds", task.result_context_item_ids},
	  {"resultContextSuggestionIds", task.result_context_suggestion_ids},
	  {"resultWorkProductIds", task.result_work_product_ids},
	  {"directDocumentWorkDocumentId", task.direct_document_work_document_id
		  ? Json(*task.direct_document_work_document_id) : Json(nullptr)},
	  {"directDocumentWorkTableId", task.direct_document_work_table_id
		  ? Json(*task.direct_document_work_table_id) : Json(nullptr)},
	  {"directDocumentWorkBannerRowIndex", task.direct_document_work_banner_row_index
		  ? Json(*task.direct_document_work_banner_row_index) : Json(nullptr)},
	  {"directDocumentWorkTitle", task.direct_document_work_title
		  ? Json(*task.direct_document_work_title) : Json(nullptr)},
	  {"directDocumentWorkSeverity", task.direct_document_work_severity
		  ? Json(*task.direct_document_work_severity) : Json(nullptr)},
	  {"directDocumentWorkUpdated", task.direct_document_work_updated
		  ? Json(*task.direct_document_work_updated) : Json(nullptr)},
	  {"directDocumentWorkMemo", task.direct_document_work_memo
		  ? Json(*task.direct_document_work_memo) : Json(nullptr)},
	  {"workProductDocument", task.work_product_document
		  ? Json(*task.work_product_document) : Json(nullptr)},
	  {"artifactDocumentId", task.artifact_document_id ? Json(*task.artifact_document_id) : Json(nullptr)},
	  {"artifactTitle", task.artifact_title ? Json(*task.artifact_title) : Json(nullptr)},
	  {"artifactFormat", task.artifact_format ? Json(*task.artifact_format) : Json(nullptr)},
	  {"artifactChangeKind", task.artifact_change_kind ? Json(*task.artifact_change_kind) : Json(nullptr)},
	  {"artifactResultingVersion", task.artifact_resulting_version
		  ? Json(*task.artifact_resulting_version) : Json(nullptr)},
	  {"artifactChange", task.artifact_change ? Json::parse(*task.artifact_change, nullptr, false) : Json(nullptr)},
      {"findings", findings},
      {"obligations", obligations},
      {"events", events},
      {"changes", changes},
      {"consiliumGroupId", task.consilium_group_id ? Json(*task.consilium_group_id) : Json(nullptr)},
      {"consiliumRole",
       task.consilium_role ? Json(to_string(*task.consilium_role)) : Json(nullptr)},
      {"consiliumParticipantId",
       task.consilium_participant_id ? Json(*task.consilium_participant_id) : Json(nullptr)},
      {"consiliumParticipantRole",
       task.consilium_participant_role ? Json(*task.consilium_participant_role) : Json(nullptr)},
      {"consiliumConfigurationId",
       task.consilium_configuration_id ? Json(*task.consilium_configuration_id) : Json(nullptr)},
      {"consiliumStageId", task.consilium_stage_id ? Json(*task.consilium_stage_id) : Json(nullptr)},
      {"consiliumStageName",
       task.consilium_stage_name ? Json(*task.consilium_stage_name) : Json(nullptr)},
      {"workflowRunId", task.workflow_run_id ? Json(*task.workflow_run_id) : Json(nullptr)},
      {"workflowId", task.workflow_id ? Json(*task.workflow_id) : Json(nullptr)},
      {"workflowVersion", task.workflow_version ? Json(*task.workflow_version) : Json(nullptr)},
      {"workflowStepId", task.workflow_step_id ? Json(*task.workflow_step_id) : Json(nullptr)},
      {"contextBudgetExcludedMaterial", task.context_budget_excluded_material},
      {"temporalRelations", [&] {
        Json relations = Json::array();
        for (const auto &relation : task.temporal_relations) {
          Json evidence = Json::array();
          for (const auto &dating : relation.evidence)
            evidence.push_back(Json{{"source", dating.source}, {"written", dating.written}});
          relations.push_back(Json{{"statement", relation.statement}, {"documentIds", relation.document_ids},
              {"evidence", std::move(evidence)}});
        }
        return relations;
      }()},
      {"requestCoverage", [&] {
          Json sources = Json::array();
          for (const SourceCoverage &source : task.request_coverage.sources)
            sources.push_back(Json{{"documentId", source.document_id}, {"supplied", source.supplied},
                {"retrieved", source.retrieved}, {"explicitlySelected", source.explicitly_selected}});
          return Json{
            {"contextItems", task.request_coverage.context_items},
            {"omittedContextItemIds", task.request_coverage.omitted_context_item_ids},
            {"conversationTurns", task.request_coverage.conversation_turns},
            {"turnsInFull", task.request_coverage.turns_in_full},
            {"turnsAsRecords", task.request_coverage.turns_as_records},
            {"omittedTurns", task.request_coverage.omitted_turns},
            {"sources", std::move(sources)},
            {"relevanceSelection", task.request_coverage.relevance_selection},
            {"relevanceCandidateDocuments", task.request_coverage.relevance_candidate_documents}};
        }()},
      {"unavailableSourceDocuments", unavailable_source_documents},
      // Where each restored amount or date stands in responseText, and the
      // digest of the fact it is: offsets and a digest only - never a symbol,
      // never a value, never a mapping (FactAnchor).
      {"factAnchors", [&] {
        Json anchors = Json::array();
        for (const protected_api::papinian::FactAnchor &anchor : task.fact_anchors)
          anchors.push_back(Json{{"start", anchor.start}, {"length", anchor.length}, {"key", anchor.key}});
        return anchors;
      }()},
  };
  // Papinian Cycle 4 (DESIGN4 §4.5): written only when there is something to
  // keep, so a task without them is stored exactly as before. Fact keys,
  // value keys and anchors only - never a symbol, never a mapping.
  if (!task.derived_facts.empty()) {
    Json derived = Json::array();
    for (const PersistedDerivedFact &fact : task.derived_facts)
      derived.push_back(Json{{"factKey", fact.fact_key}, {"type", fact.type}, {"valueKey", fact.value_key},
          {"label", fact.label}, {"ast", fact.ast}, {"roots", fact.roots}});
    value["derivedFacts"] = std::move(derived);
  }
  if (task.protected_template) value["protectedTemplate"] = *task.protected_template;
  // Written only for work kept on this computer, so every other task is stored
  // exactly as before.
  if (task.local_only) value["localOnly"] = true;
  // Written only for a turn of a document review, so every other task is
  // stored exactly as before.
  if (!task.document_review_id.empty()) value["documentReviewId"] = task.document_review_id;
  // Written only for an AI colleague's own model call, so every other task is
  // stored exactly as before.
  if (!task.work_chat_id.empty()) value["workChatId"] = task.work_chat_id;
  // What a message carries - the message it quotes, the documents it hands
  // over, the reactions to it - and its edit and deletion: written only where a
  // message has them, so every other task is stored exactly as before.
  if (!task.quote_message_id.empty()) value["quoteMessageId"] = task.quote_message_id;
  if (!task.attachment_document_ids.empty()) value["attachmentDocumentIds"] = task.attachment_document_ids;
  if (!task.reactions.empty()) {
    Json reactions = Json::object();
    for (const auto &[emoji, who] : task.reactions)
      if (!who.empty()) reactions[emoji] = who;
    if (!reactions.empty()) value["reactions"] = reactions;
  }
  // When it reached each other member, only once one has.
  if (!task.received.empty()) {
    Json received = Json::object();
    for (const auto &[actor, at] : task.received)
      if (!actor.empty() && !at.empty()) received[actor] = at;
    if (!received.empty()) value["received"] = received;
  }
  if (!task.edited_at.empty()) value["editedAt"] = task.edited_at;
  if (!task.deleted_at.empty()) value["deletedAt"] = task.deleted_at;
  if (!task.not_checked.empty()) value["notChecked"] = task.not_checked;
  if (!task.ask_options.empty()) value["askOptions"] = task.ask_options;
  if (!task.answer_option.empty()) value["answerOption"] = task.answer_option;
  if (!task.thread_event.empty()) value["threadEvent"] = task.thread_event;
  if (!task.place.empty()) {
    Json place{{"documentId", task.place.document_id}};
    if (!task.place.section.empty()) place["section"] = task.place.section;
    if (!task.place.heading.empty()) place["heading"] = task.place.heading;
    if (!task.place.quote.empty()) place["quote"] = task.place.quote;
    value["place"] = place;
  }
  // A signed person's message: the key and signature of the computer that wrote
  // it (Chat/MessageProvenance).
  if (!task.signature.empty())
    value["signature"] = Json{{"publicKey", task.signer_public_key}, {"value", task.signature}};
  // An operation's signed origin (Chat/OperationOrigin).
  if (task.origin)
    value["origin"] = Json{{"actorId", task.origin->actor_id}, {"by", task.origin->by},
        {"agentId", task.origin->agent_id}, {"ceiling", task.origin->ceiling}, {"localOnly", task.origin->local_only},
        {"signature", Json{{"publicKey", task.origin->public_key}, {"value", task.origin->signature}}}};
  // Each model call's billing evidence, written only for an operation that
  // made one, so every other task is stored exactly as before.
  {
    const std::vector<protected_api::papinian::OutboundCall> billed = billed_calls_of(task);
    if (billed.empty() == false) {
      Json calls = Json::array();
      for (const protected_api::papinian::OutboundCall &call : billed) calls.push_back(billed_call_to_json(call));
      value["billedCalls"] = std::move(calls);
    }
  }
  return value;
}

MatterTask task_from_json(const Json &value) {
  MatterTask task;
  task.id = value.value("id", std::string());
  task.created_at = value.value("createdAt", std::string());
	task.operation_type = value.value("operationType", std::string("matter_task"));
	task.operation_role = value.value("operationRole", std::string("reasoning"));
	if (value.contains("agentOperationId") && value.at("agentOperationId").is_string())
		task.agent_operation_id = value.at("agentOperationId").get<std::string>();
	task.operation_execution_state = value.value("operationExecutionState", std::string("completed"));
	task.operation_revision = value.value("operationRevision", task.agent_operation_id ? std::uint64_t(1) : std::uint64_t(0));
	task.turn_contract_revision = value.value("turnContractRevision", std::uint64_t(0));
	task.operation_stage = value.value("operationStage", task.operation_execution_state);
	task.operation_started_at = value.value("operationStartedAt", task.created_at);
	task.operation_updated_at = value.value("operationUpdatedAt", task.operation_started_at);
	task.operation_activity = value.value("operationActivity", std::string());
	task.operation_cancel_requested = value.value("operationCancelRequested", false);
	task.operation_resume_allowed = value.value("operationResumeAllowed", false);
	task.operation_device_id = value.value("operationDeviceId", std::string());
	task.agent_id = value.value("agentId", std::string());
	task.agent_initiated = value.value("agentInitiated", false);
	// Anything but a plain false keeps the work here.
	task.local_only = value.contains("localOnly") && !(value.at("localOnly").is_boolean() && !value.at("localOnly").get<bool>());
	task.author_actor_id = value.value("authorActorId", std::string());
	task.author_name = value.value("authorName", std::string());
	task.answers_message_id = value.value("answersMessageId", std::string());
	if (value.contains("mentions") && value.at("mentions").is_array())
		for (const Json &mention : value.at("mentions"))
			if (mention.is_string()) task.mentions.push_back(mention.get<std::string>());
	// Anything else in its place is no signature at all.
	if (value.contains("signature") && value.at("signature").is_object()) {
		const Json &signed_by = value.at("signature");
		if (signed_by.contains("publicKey") && signed_by.at("publicKey").is_string() && signed_by.contains("value")
				&& signed_by.at("value").is_string()) {
			task.signer_public_key = signed_by.at("publicKey").get<std::string>();
			task.signature = signed_by.at("value").get<std::string>();
		}
	}
	// An origin is read as it stands, so a changed one does not verify; one that
	// does not read as an origin at all keeps the work here.
	if (value.contains("origin")) {
		const Json &origin = value.at("origin");
		OperationOriginStatement statement;
		statement.local_only = true;
		if (origin.is_object()) {
			const auto text = [&](const char *key) {
				return origin.contains(key) && origin.at(key).is_string() ? origin.at(key).get<std::string>() : std::string();
			};
			statement.actor_id = text("actorId");
			statement.by = text("by");
			statement.agent_id = text("agentId");
			statement.ceiling = text("ceiling");
			statement.local_only = !(origin.contains("localOnly") && origin.at("localOnly").is_boolean()
				&& !origin.at("localOnly").get<bool>());
			if (origin.contains("signature") && origin.at("signature").is_object()) {
				const Json &signed_by = origin.at("signature");
				if (signed_by.contains("publicKey") && signed_by.at("publicKey").is_string() && signed_by.contains("value")
						&& signed_by.at("value").is_string()) {
					statement.public_key = signed_by.at("publicKey").get<std::string>();
					statement.signature = signed_by.at("value").get<std::string>();
				}
			}
		}
		task.origin = std::move(statement);
	}
	// Absent from every task written before a Matter kept its calls' billing
	// evidence, and from any that made no model call.
	if (value.contains("billedCalls") && value.at("billedCalls").is_array()) {
		for (const Json &saved : value.at("billedCalls"))
			if (saved.is_object()) task.billed_calls.push_back(billed_call_from_json(saved));
	}
	if (value.contains("operationApproval"))
		task.operation_approval = operation_approval_from_json(value.at("operationApproval"));
	if (value.contains("rejectedApproval"))
		task.rejected_approval = operation_approval_from_json(value.at("rejectedApproval"));
	// Absent from every task written before an operation could ask the lawyer.
	if (value.contains("lawyerQuestions") && value.at("lawyerQuestions").is_array()) {
		for (const Json &saved : value.at("lawyerQuestions")) {
			const std::optional<LawyerQuestionRecord> record = lawyer_question_from_json(saved);
			if (record) task.lawyer_questions.push_back(*record);
		}
	}
	if (value.contains("operationEvents") && value.at("operationEvents").is_array()) {
		for (const Json &saved : value.at("operationEvents")) {
			const auto event = operation_event_from_json(saved);
			if (event) task.operation_events.push_back(*event);
		}
	}
	if (value.contains("documentOperationCheckpoint")
			&& value.at("documentOperationCheckpoint").is_object()) {
		const Json &saved = value.at("documentOperationCheckpoint");
		documents::DocumentOperationCheckpoint checkpoint;
		checkpoint.schema_version = saved.value("schemaVersion", 1);
		checkpoint.stage = saved.value("stage", std::string("drafting"));
		if (saved.contains("draft") && saved.at("draft").is_object()) checkpoint.draft = saved.at("draft");
		if (saved.contains("mutationCommit") && saved.at("mutationCommit").is_object())
			checkpoint.mutation_commit = saved.at("mutationCommit");
		if (saved.contains("externalEvidence") && saved.at("externalEvidence").is_array()) {
			for (const Json &entry : saved.at("externalEvidence")) {
				const auto evidence = research::external_evidence_from_json(entry);
				if (evidence) checkpoint.external_evidence.push_back(*evidence);
			}
		}
		if (saved.contains("progress") && saved.at("progress").is_object()) {
			const Json &progress = saved.at("progress");
			checkpoint.progress.model_turns_used = progress.value("modelTurnsUsed", std::size_t(0));
			checkpoint.progress.provider_requests_used = progress.value(
				"providerRequestsUsed", checkpoint.progress.model_turns_used);
			checkpoint.progress.successful_tool_calls_used = progress.value("successfulToolCallsUsed", std::size_t(0));
			checkpoint.progress.search_calls_used = progress.value("searchCallsUsed", std::size_t(0));
			checkpoint.progress.open_calls_used = progress.value("openCallsUsed", std::size_t(0));
			checkpoint.progress.item_deltas_proposed = progress.value("itemDeltasProposed", std::size_t(0));
			checkpoint.progress.item_deltas_accepted = progress.value("itemDeltasAccepted", std::size_t(0));
			checkpoint.progress.item_deltas_rejected = progress.value("itemDeltasRejected", std::size_t(0));
			checkpoint.progress.incomplete_response_repairs = progress.value("incompleteResponseRepairs", std::size_t(0));
			checkpoint.progress.protected_response_repairs = progress.value("protectedResponseRepairs", std::size_t(0));
			checkpoint.progress.full_action_regenerations = progress.value("fullActionRegenerations", std::size_t(0));
			checkpoint.progress.document_mutations_committed = progress.value("documentMutationsCommitted", std::size_t(0));
		}
		task.document_operation_checkpoint = std::move(checkpoint);
	}
	task.ai_setup_source = value.value("aiSetupSource", std::string());
	if (value.contains("requestedProviderId") && value.at("requestedProviderId").is_string())
		task.requested_provider_id = value.at("requestedProviderId").get<std::string>();
	if (value.contains("requestedModelId") && value.at("requestedModelId").is_string())
		task.requested_model_id = value.at("requestedModelId").get<std::string>();
  if (value.contains("chatId") && value.at("chatId").is_string()) {
    task.chat_id = value.at("chatId").get<std::string>();
  }
  task.prompt = value.value("prompt", std::string());
  task.legal_task_preset_id =
      value.value("legalTaskPresetId", std::string("general"));
  task.legal_task_preset_version = value.value("legalTaskPresetVersion", 1);
  task.prompt_profile_id =
      value.value("promptProfileId", std::string("legal-machines"));
  task.prompt_profile_version = value.value("promptProfileVersion", 1);
  if (value.contains("selectedDocumentIds") &&
      value.at("selectedDocumentIds").is_array()) {
    for (const auto &document_id : value.at("selectedDocumentIds")) {
      if (document_id.is_string()) {
        task.selected_document_ids.push_back(document_id.get<std::string>());
      }
    }
  }
	task.professional_capability = value.value("professionalCapability", std::string("conversation"));
	if (value.contains("professionalOperations") && value.at("professionalOperations").is_array())
		for (const Json &entry : value.at("professionalOperations"))
			if (entry.is_string()) task.professional_operations.push_back(entry.get<std::string>());
	if (value.contains("professionalSourceDocumentIds")
			&& value.at("professionalSourceDocumentIds").is_array())
		for (const Json &entry : value.at("professionalSourceDocumentIds"))
			if (entry.is_string()) task.professional_source_document_ids.push_back(entry.get<std::string>());
	if (value.contains("professionalTargetDocumentId")
			&& value.at("professionalTargetDocumentId").is_string())
		task.professional_target_document_id = value.at("professionalTargetDocumentId").get<std::string>();
	task.professional_destination_format = value.value(
		"professionalDestinationFormat", std::string("unknown"));
	if (value.contains("professionalDestinationFileName")
			&& value.at("professionalDestinationFileName").is_string())
		task.professional_destination_file_name =
			value.at("professionalDestinationFileName").get<std::string>();
	task.professional_write_intent = value.value("professionalWriteIntent", false);
  // Absent on a pre-WP5-D task: it is its own chain's first and only
  // attempt.
  task.attempt = value.value("attempt", 1);
  if (value.contains("rootTaskId") && value.at("rootTaskId").is_string()) {
    task.root_task_id = value.at("rootTaskId").get<std::string>();
  }
  // Key absence (a pre-WP5-D manifest) means "predates capture" - leave
  // matter_instructions_snapshot_captured false and the snapshot unset, so
  // resolve_retry_matter_instructions() falls back to the Matter's current
  // instructions rather than a fabricated historical value. Key presence
  // (even with a null value, meaning "captured, no instructions") means the
  // snapshot is authoritative.
  if (value.contains("matterInstructionsSnapshot")) {
    task.matter_instructions_snapshot_captured = true;
    if (value.at("matterInstructionsSnapshot").is_string()) {
      task.matter_instructions_snapshot = value.at("matterInstructionsSnapshot").get<std::string>();
    }
  }
  if (value.contains("responseText") && value.at("responseText").is_string()) {
    task.response_text = value.at("responseText").get<std::string>();
  }
  if (value.contains("providerId") && value.at("providerId").is_string()) {
    task.provider_id = value.at("providerId").get<std::string>();
  }
  if (value.contains("modelId") && value.at("modelId").is_string()) {
    task.model_id = value.at("modelId").get<std::string>();
  }
	task.execution_kind = value.value("executionKind", std::string());
	task.runtime_id = value.value("runtimeId", std::string());
	task.runtime_version = value.value("runtimeVersion", std::string());
	task.runtime_source_commit = value.value("runtimeSourceCommit", std::string());
	task.runtime_platform = value.value("runtimePlatform", std::string());
	task.runtime_backend = value.value("runtimeBackend", std::string());
	task.runtime_model_id = value.value("runtimeModelId", std::string());
  task.protected_api_mode = value.value("protectedApiMode", std::string("off"));
  task.protected_api_applied = value.value("protectedApiApplied", false);
  task.stopped_by_user = value.value("stoppedByUser", false);
  task.privacy_metadata = privacy_metadata_from_json(
      value.contains("privacyMetadata") ? value.at("privacyMetadata") : Json(),
      task.protected_api_mode);
  task.status = task_status_from_string(value.value("status", std::string("success")));
  if (value.contains("error") && value.at("error").is_string()) {
    task.error = value.at("error").get<std::string>();
  }
  if (value.contains("errorCode") && value.at("errorCode").is_string()) {
    task.error_code =
        providers::task_error_code_from_string(value.at("errorCode").get<std::string>());
  } else if (task.status == TaskStatus::Error) {
    // A pre-WP5-B persisted error task has no errorCode field at all -
    // classify it Unknown rather than leaving the pair partially set. A change
    // the lawyer rejected before the rejection had its own code is still a
    // rejection, never a failure to retry.
    task.error_code = value.value("operationStage", std::string()) == "rejected"
        ? providers::TaskErrorCode::ChangeRejected
        : providers::TaskErrorCode::Unknown;
  }
  // A rejection an older Singularity saved again before rejections had their own
  // code carries "unknown" rather than no code at all. The stage "rejected" is
  // written only when the lawyer declines a change, so it is still that
  // decision - never a failure offered for retry.
  if (task.status == TaskStatus::Error && task.error_code == providers::TaskErrorCode::Unknown
      && value.value("operationStage", std::string()) == "rejected") {
    task.error_code = providers::TaskErrorCode::ChangeRejected;
  }
  if (value.contains("usage")) {
    task.usage = usage_from_json(value.at("usage"));
  }
  if (value.contains("sourceReferences") && value.at("sourceReferences").is_array()) {
    for (const auto &entry : value.at("sourceReferences")) {
      if (auto reference = source_reference_from_json(entry)) {
        task.source_references.push_back(std::move(*reference));
      }
    }
  }
  if (value.contains("quotedPassages") && value.at("quotedPassages").is_array()) {
    for (const Json &entry : value.at("quotedPassages")) {
      if (!entry.is_object()) continue;
      context::QuotedPassage quoted;
      quoted.quote = entry.value("quote", std::string());
      quoted.document_id = entry.value("documentId", std::string());
      quoted.chunk_id = entry.value("chunkId", std::string());
      quoted.document_name = entry.value("documentName", std::string());
      quoted.verified = entry.value("verified", false);
      quoted.start = static_cast<std::size_t>(entry.value("start", 0LL));
      quoted.length = static_cast<std::size_t>(entry.value("length", 0LL));
      quoted.rebound = entry.value("rebound", false);
      // An older record did not say whether the answer attributed the words
      // to a source; unknown is never shown as a source problem.
      quoted.attributed = entry.value("attributed", false);
      task.quoted_passages.push_back(std::move(quoted));
    }
  }
  if (value.contains("contextSnapshot") && value.at("contextSnapshot").is_array()) {
    for (const Json &entry : value.at("contextSnapshot")) {
      if (entry.is_object()) task.context_snapshot.push_back(context_item_snapshot_from_json(entry));
    }
  }
	if (value.contains("toolCallEvidence") && value.at("toolCallEvidence").is_array()) {
		for (const Json &entry : value.at("toolCallEvidence")) {
			const std::optional<tools::ToolCallEvidence> evidence = tool_call_evidence_from_json(entry);
			if (evidence.has_value()) task.tool_call_evidence.push_back(*evidence);
		}
	}
	task.research_mode = value.value("researchMode", std::string("auto"));
	if (value.contains("externalEvidence") && value.at("externalEvidence").is_array()) {
		for (const Json &entry : value.at("externalEvidence")) {
			const std::optional<research::ExternalEvidenceReference> evidence =
				research::external_evidence_from_json(entry);
			if (evidence.has_value()) task.external_evidence.push_back(*evidence);
		}
	}
	if (value.contains("documentRoleBindings") && value.at("documentRoleBindings").is_array()) {
		for (const Json &entry : value.at("documentRoleBindings")) {
			const std::optional<documents::DocumentRoleAssignment> assignment =
				documents::document_role_assignment_from_json(entry);
			if (!assignment) continue;
			task.document_role_bindings.push_back(*assignment);
		}
	}
	if (value.contains("documentActionEvidence") && value.at("documentActionEvidence").is_array()) {
		for (const Json &entry : value.at("documentActionEvidence")) {
			const std::optional<documents::AppliedDocumentActionEvidence> evidence =
				documents::document_action_evidence_from_json(entry);
			if (evidence) task.document_action_evidence.push_back(*evidence);
		}
	}
	if (value.contains("resultContextItemIds") && value.at("resultContextItemIds").is_array())
		for (const Json &entry : value.at("resultContextItemIds")) if (entry.is_string()) task.result_context_item_ids.push_back(entry.get<std::string>());
	if (value.contains("resultContextSuggestionIds") && value.at("resultContextSuggestionIds").is_array())
		for (const Json &entry : value.at("resultContextSuggestionIds")) if (entry.is_string()) task.result_context_suggestion_ids.push_back(entry.get<std::string>());
	if (value.contains("resultWorkProductIds") && value.at("resultWorkProductIds").is_array())
		for (const Json &entry : value.at("resultWorkProductIds")) if (entry.is_string()) task.result_work_product_ids.push_back(entry.get<std::string>());
	if (value.contains("directDocumentWorkDocumentId") && value.at("directDocumentWorkDocumentId").is_string())
		task.direct_document_work_document_id = value.at("directDocumentWorkDocumentId").get<std::string>();
	if (value.contains("directDocumentWorkTableId") && value.at("directDocumentWorkTableId").is_string())
		task.direct_document_work_table_id = value.at("directDocumentWorkTableId").get<std::string>();
	if (value.contains("directDocumentWorkBannerRowIndex") && value.at("directDocumentWorkBannerRowIndex").is_number_unsigned())
		task.direct_document_work_banner_row_index = value.at("directDocumentWorkBannerRowIndex").get<std::size_t>();
	if (value.contains("directDocumentWorkTitle") && value.at("directDocumentWorkTitle").is_string())
		task.direct_document_work_title = value.at("directDocumentWorkTitle").get<std::string>();
	if (value.contains("directDocumentWorkSeverity") && value.at("directDocumentWorkSeverity").is_string())
		task.direct_document_work_severity = value.at("directDocumentWorkSeverity").get<std::string>();
	if (value.contains("directDocumentWorkUpdated") && value.at("directDocumentWorkUpdated").is_boolean())
		task.direct_document_work_updated = value.at("directDocumentWorkUpdated").get<bool>();
	if (value.contains("directDocumentWorkMemo") && value.at("directDocumentWorkMemo").is_string())
		task.direct_document_work_memo = value.at("directDocumentWorkMemo").get<std::string>();
	if (value.contains("workProductDocument") && value.at("workProductDocument").is_string())
		task.work_product_document = value.at("workProductDocument").get<std::string>();
	if (value.contains("artifactDocumentId") && value.at("artifactDocumentId").is_string())
		task.artifact_document_id = value.at("artifactDocumentId").get<std::string>();
	if (value.contains("artifactTitle") && value.at("artifactTitle").is_string())
		task.artifact_title = value.at("artifactTitle").get<std::string>();
	if (value.contains("artifactFormat") && value.at("artifactFormat").is_string())
		task.artifact_format = value.at("artifactFormat").get<std::string>();
	if (value.contains("artifactChangeKind") && value.at("artifactChangeKind").is_string())
		task.artifact_change_kind = value.at("artifactChangeKind").get<std::string>();
	if (value.contains("artifactResultingVersion") && value.at("artifactResultingVersion").is_number_integer())
		task.artifact_resulting_version = value.at("artifactResultingVersion").get<int>();
	if (value.contains("artifactChange") && value.at("artifactChange").is_object())
		task.artifact_change = value.at("artifactChange").dump();
  if (value.contains("findings") && value.at("findings").is_array()) {
    for (const auto &entry : value.at("findings")) {
      if (auto finding = finding_from_json(entry)) {
        task.findings.push_back(std::move(*finding));
      }
    }
  }
  if (value.contains("obligations") && value.at("obligations").is_array()) {
    for (const auto &entry : value.at("obligations")) {
      if (auto obligation = obligation_from_json(entry)) {
        task.obligations.push_back(std::move(*obligation));
      }
    }
  }
  if (value.contains("events") && value.at("events").is_array()) {
    for (const auto &entry : value.at("events")) {
      if (auto event = event_from_json(entry)) {
        task.events.push_back(std::move(*event));
      }
    }
  }
  if (value.contains("changes") && value.at("changes").is_array()) {
    for (const auto &entry : value.at("changes")) {
      if (auto change = change_from_json(entry)) {
        task.changes.push_back(std::move(*change));
      }
    }
  }
  // Absent on every pre-Phase-10 manifest - both stay nullopt, meaning "not
  // a Consilium task", the correct reading for history that predates the
  // feature.
  if (value.contains("consiliumGroupId") && value.at("consiliumGroupId").is_string()) {
    task.consilium_group_id = value.at("consiliumGroupId").get<std::string>();
  }
  if (value.contains("consiliumRole") && value.at("consiliumRole").is_string()) {
    task.consilium_role = consilium_role_from_string(value.at("consiliumRole").get<std::string>());
  }
  // Absent on every manifest written before a saved Configuration became the
  // execution plan - all five stay nullopt, the correct reading for a legacy
  // targets-only Consilium run, which genuinely had no participant identity.
  const auto read_optional_string = [&value](const char *key) -> std::optional<std::string> {
    if (value.contains(key) && value.at(key).is_string()) return value.at(key).get<std::string>();
    return std::nullopt;
  };
  task.consilium_participant_id = read_optional_string("consiliumParticipantId");
  task.consilium_participant_role = read_optional_string("consiliumParticipantRole");
  task.consilium_configuration_id = read_optional_string("consiliumConfigurationId");
  task.consilium_stage_id = read_optional_string("consiliumStageId");
  task.consilium_stage_name = read_optional_string("consiliumStageName");
  // Absent on every pre-Phase-10-WP10-B manifest - all four stay nullopt,
  // meaning "not a Completor task", the correct reading for history that
  // predates the feature.
  if (value.contains("workflowRunId") && value.at("workflowRunId").is_string()) {
    task.workflow_run_id = value.at("workflowRunId").get<std::string>();
  }
  if (value.contains("workflowId") && value.at("workflowId").is_string()) {
    task.workflow_id = value.at("workflowId").get<std::string>();
  }
  if (value.contains("workflowVersion") && value.at("workflowVersion").is_number_integer()) {
    task.workflow_version = value.at("workflowVersion").get<int>();
  }
  if (value.contains("workflowStepId") && value.at("workflowStepId").is_string()) {
    task.workflow_step_id = value.at("workflowStepId").get<std::string>();
  }
  task.context_budget_excluded_material = value.value("contextBudgetExcludedMaterial", false);
  if (value.contains("temporalRelations") && value.at("temporalRelations").is_array()) {
    for (const Json &entry : value.at("temporalRelations")) {
      if (!entry.is_object()) continue;
      context::temporal_context::TemporalRelation relation;
      relation.statement = entry.value("statement", std::string());
      if (entry.contains("documentIds") && entry.at("documentIds").is_array())
        for (const Json &id : entry.at("documentIds"))
          if (id.is_string()) relation.document_ids.push_back(id.get<std::string>());
      if (entry.contains("evidence") && entry.at("evidence").is_array())
        for (const Json &dating : entry.at("evidence"))
          if (dating.is_object())
            relation.evidence.push_back({dating.value("source", std::string()), dating.value("written", std::string())});
      if (!relation.statement.empty()) task.temporal_relations.push_back(std::move(relation));
    }
  }
  if (value.contains("requestCoverage") && value.at("requestCoverage").is_object()) {
    const Json &coverage = value.at("requestCoverage");
    const auto count = [&](const char *key) {
      return coverage.contains(key) && coverage.at(key).is_number_unsigned()
          ? coverage.at(key).get<std::size_t>() : std::size_t{0};
    };
    const auto ids = [&](const char *key) {
      std::vector<std::string> result;
      if (coverage.contains(key) && coverage.at(key).is_array())
        for (const Json &entry : coverage.at(key))
          if (entry.is_string()) result.push_back(entry.get<std::string>());
      return result;
    };
    task.request_coverage.context_items = count("contextItems");
    task.request_coverage.omitted_context_item_ids = ids("omittedContextItemIds");
    task.request_coverage.conversation_turns = count("conversationTurns");
    task.request_coverage.turns_in_full = ids("turnsInFull");
    task.request_coverage.turns_as_records = ids("turnsAsRecords");
    task.request_coverage.omitted_turns = ids("omittedTurns");
    task.request_coverage.relevance_selection = coverage.value("relevanceSelection", false);
    task.request_coverage.relevance_candidate_documents = count("relevanceCandidateDocuments");
    if (coverage.contains("sources") && coverage.at("sources").is_array())
      for (const Json &entry : coverage.at("sources")) {
        if (!entry.is_object()) continue;
        SourceCoverage source;
        source.document_id = entry.value("documentId", std::string());
        source.supplied = entry.value("supplied", std::size_t{0});
        source.retrieved = entry.value("retrieved", std::size_t{0});
        source.explicitly_selected = entry.value("explicitlySelected", false);
        task.request_coverage.sources.push_back(std::move(source));
      }
  }
  if (value.contains("unavailableSourceDocuments") && value.at("unavailableSourceDocuments").is_array()) {
    for (const Json &entry : value.at("unavailableSourceDocuments")) {
      task.unavailable_source_documents.push_back({
          entry.value("documentId", std::string()),
          entry.value("documentName", std::string()),
          entry.value("reason", std::string()),
      });
    }
  }
  // Additive: a task written before fact anchors reads with none.
  if (value.contains("factAnchors") && value.at("factAnchors").is_array()) {
    for (const Json &entry : value.at("factAnchors")) {
      if (!entry.is_object() || !entry.contains("start") || !entry.at("start").is_number_unsigned()
          || !entry.contains("length") || !entry.at("length").is_number_unsigned()) continue;
      protected_api::papinian::FactAnchor anchor{entry.at("start").get<std::size_t>(),
          entry.at("length").get<std::size_t>(), entry.value("key", std::string())};
      if (anchor.length > 0 && protected_api::papinian::fact_anchor_marks::is_key(anchor.key))
        task.fact_anchors.push_back(std::move(anchor));
    }
  }
  // Additive (DESIGN4 §4.5): a task written before derived facts and protected
  // templates reads with none.
  if (value.contains("derivedFacts") && value.at("derivedFacts").is_array()) {
    for (const Json &entry : value.at("derivedFacts")) {
      if (!entry.is_object()) continue;
      PersistedDerivedFact fact;
      fact.fact_key = entry.value("factKey", std::string());
      if (fact.fact_key.empty()) continue;
      fact.type = entry.value("type", std::string());
      fact.value_key = entry.value("valueKey", std::string());
      fact.label = entry.value("label", std::string());
      fact.ast = entry.value("ast", std::string());
      if (entry.contains("roots") && entry.at("roots").is_array())
        for (const Json &root : entry.at("roots"))
          if (root.is_string()) fact.roots.push_back(root.get<std::string>());
      task.derived_facts.push_back(std::move(fact));
    }
  }
  if (value.contains("protectedTemplate") && value.at("protectedTemplate").is_string())
    task.protected_template = value.at("protectedTemplate").get<std::string>();
  if (value.contains("documentReviewId") && value.at("documentReviewId").is_string())
    task.document_review_id = value.at("documentReviewId").get<std::string>();
  if (value.contains("workChatId") && value.at("workChatId").is_string())
    task.work_chat_id = value.at("workChatId").get<std::string>();
  if (value.contains("quoteMessageId") && value.at("quoteMessageId").is_string())
    task.quote_message_id = value.at("quoteMessageId").get<std::string>();
  if (value.contains("attachmentDocumentIds") && value.at("attachmentDocumentIds").is_array())
    for (const Json &id : value.at("attachmentDocumentIds"))
      if (id.is_string()) task.attachment_document_ids.push_back(id.get<std::string>());
  if (value.contains("reactions") && value.at("reactions").is_object())
    for (const auto &[emoji, who] : value.at("reactions").items()) {
      if (!who.is_array()) continue;
      std::vector<std::string> actors;
      for (const Json &actor : who)
        if (actor.is_string()) actors.push_back(actor.get<std::string>());
      if (!actors.empty()) task.reactions[emoji] = actors;
    }
  if (value.contains("received") && value.at("received").is_object())
    for (const auto &[actor, at] : value.at("received").items())
      if (!actor.empty() && at.is_string() && !at.get<std::string>().empty()) task.received[actor] = at.get<std::string>();
  if (value.contains("editedAt") && value.at("editedAt").is_string()) task.edited_at = value.at("editedAt").get<std::string>();
  if (value.contains("deletedAt") && value.at("deletedAt").is_string()) task.deleted_at = value.at("deletedAt").get<std::string>();
  if (value.contains("notChecked") && value.at("notChecked").is_string())
    task.not_checked = value.at("notChecked").get<std::string>();
  if (value.contains("askOptions") && value.at("askOptions").is_array())
    for (const Json &option : value.at("askOptions"))
      if (option.is_string()) task.ask_options.push_back(option.get<std::string>());
  if (value.contains("answerOption") && value.at("answerOption").is_string())
    task.answer_option = value.at("answerOption").get<std::string>();
  if (value.contains("threadEvent") && value.at("threadEvent").is_string())
    task.thread_event = value.at("threadEvent").get<std::string>();
  if (value.contains("place") && value.at("place").is_object()) {
    const Json &place = value.at("place");
    const auto text_of = [&place](const char *key) {
      return place.contains(key) && place.at(key).is_string() ? place.at(key).get<std::string>() : std::string();
    };
    task.place.document_id = text_of("documentId");
    task.place.section = text_of("section");
    task.place.heading = text_of("heading");
    task.place.quote = text_of("quote");
  }
  return task;
}

Json ai_setup_override_to_json(const AiSetupRoleOverride &override_entry) {
  return Json{
      {"role", override_entry.role},
      {"providerId", override_entry.provider_id},
      {"modelId", override_entry.model_id},
  };
}

std::optional<AiSetupRoleOverride> ai_setup_override_from_json(const Json &value) {
  if (!value.is_object()) return std::nullopt;
  AiSetupRoleOverride override_entry;
  override_entry.role = value.value("role", std::string());
  override_entry.provider_id = value.value("providerId", std::string());
  override_entry.model_id = value.value("modelId", std::string());
  if (override_entry.role.empty() || override_entry.provider_id.empty() || override_entry.model_id.empty()) {
    return std::nullopt;
  }
  return override_entry;
}

std::vector<AiSetupRoleOverride> ai_setup_overrides_from_json(const Json &value) {
  std::vector<AiSetupRoleOverride> overrides;
  if (!value.is_array()) return overrides;
  for (const auto &entry : value) {
    if (auto parsed = ai_setup_override_from_json(entry)) overrides.push_back(std::move(*parsed));
  }
  return overrides;
}

Json consilium_participant_to_json(const consilium::Participant& participant) {
	Json explicitModel = nullptr;
	if (participant.explicit_model) {
		explicitModel = Json{{"providerId", participant.explicit_model->provider_id},
			{"modelId", participant.explicit_model->model_id}};
	}
	return Json{{"id", participant.id}, {"role", participant.role},
		{"aiSetupRole", participant.ai_setup_role}, {"explicitModel", std::move(explicitModel)}};
}

std::optional<consilium::Participant> consilium_participant_from_json(const Json& value) {
	if (value.is_object() == false) return std::nullopt;
	consilium::Participant participant;
	participant.id = value.value("id", std::string());
	participant.role = value.value("role", std::string());
	participant.ai_setup_role = value.value("aiSetupRole", std::string());
	if (value.contains("explicitModel") && value.at("explicitModel").is_object()) {
		const Json& model = value.at("explicitModel");
		const std::string providerId = model.value("providerId", std::string());
		const std::string modelId = model.value("modelId", std::string());
		if (providerId.empty() == false && modelId.empty() == false) {
			participant.explicit_model = routing::ModelTarget{providerId, modelId};
		}
	}
	if (participant.id.empty() || participant.role.empty() || participant.ai_setup_role.empty()) {
		return std::nullopt;
	}
	return participant;
}

Json consilium_configuration_to_json(const consilium::Configuration& configuration) {
	Json participants = Json::array();
	for (const consilium::Participant& participant : configuration.participants) {
		participants.push_back(consilium_participant_to_json(participant));
	}
	Json stages = Json::array();
	for (const consilium::PlanStage& stage : configuration.stages) {
		stages.push_back(Json{{"id", stage.id}, {"name", stage.name},
			{"mode", consilium::to_string(stage.mode)}, {"participantIds", stage.participant_ids}});
	}
	return Json{
		{"id", configuration.id}, {"projectId", configuration.project_id},
		{"name", configuration.name}, {"objective", configuration.objective},
		{"participants", std::move(participants)}, {"stages", std::move(stages)},
		{"sourcePolicy", Json{{"mode", consilium::to_string(configuration.source_policy.mode)},
			{"selectedDocumentIds", configuration.source_policy.selected_document_ids},
			{"requiresSelection", configuration.source_policy.requires_selection}}},
		{"revision", configuration.revision}, {"createdAt", configuration.created_at},
		{"updatedAt", configuration.updated_at}, {"actorId", configuration.actor_id},
		{"operationId", configuration.operation_id},
	};
}

std::optional<consilium::Configuration> consilium_configuration_from_json(
		const Json& value, const std::string& projectId) {
	if (value.is_object() == false) return std::nullopt;
	consilium::Configuration configuration;
	configuration.id = value.value("id", std::string());
	configuration.project_id = projectId;
	configuration.name = value.value("name", std::string());
	configuration.objective = value.value("objective", std::string());
	configuration.revision = value.value("revision", 1LL);
	configuration.created_at = value.value("createdAt", std::string());
	configuration.updated_at = value.value("updatedAt", std::string());
	configuration.actor_id = value.value("actorId", std::string());
	configuration.operation_id = value.value("operationId", std::string());
	if (value.contains("participants") && value.at("participants").is_array()) {
		for (const Json& entry : value.at("participants")) {
			if (auto participant = consilium_participant_from_json(entry)) {
				configuration.participants.push_back(std::move(*participant));
			}
		}
	}
	if (value.contains("stages") && value.at("stages").is_array()) {
		for (const Json& entry : value.at("stages")) {
			if (entry.is_object() == false) continue;
			const auto mode = consilium::stage_mode_from_string(entry.value("mode", std::string()));
			if (mode == std::nullopt) continue;
			consilium::PlanStage stage;
			stage.id = entry.value("id", std::string());
			stage.name = entry.value("name", std::string());
			stage.mode = *mode;
			if (entry.contains("participantIds") && entry.at("participantIds").is_array()) {
				for (const Json& participantId : entry.at("participantIds")) {
					if (participantId.is_string()) stage.participant_ids.push_back(participantId.get<std::string>());
				}
			}
			if (stage.id.empty() == false && stage.name.empty() == false) {
				configuration.stages.push_back(std::move(stage));
			}
		}
	}
	if (value.contains("sourcePolicy") && value.at("sourcePolicy").is_object()) {
		const Json& policy = value.at("sourcePolicy");
		const auto mode = consilium::source_mode_from_string(policy.value("mode", std::string()));
		if (mode) configuration.source_policy.mode = *mode;
		configuration.source_policy.requires_selection = policy.value("requiresSelection", false);
		if (policy.contains("selectedDocumentIds") && policy.at("selectedDocumentIds").is_array()) {
			for (const Json& documentId : policy.at("selectedDocumentIds")) {
				if (documentId.is_string()) {
					configuration.source_policy.selected_document_ids.push_back(documentId.get<std::string>());
				}
			}
		}
	}
	if (configuration.id.empty() || configuration.name.empty()) return std::nullopt;
	return configuration;
}

Json memory_document_digests_to_json(const std::vector<MemoryDocumentDigest> &digests) {
  Json values = Json::array();
  for (const MemoryDocumentDigest &digest : digests) {
    values.push_back(Json{{"documentId", digest.document_id},
                          {"fingerprint", digest.fingerprint},
                          {"digest", digest.digest},
                          {"readAt", digest.read_at}});
  }
  return values;
}

// Read leniently: an entry that does not name its document is dropped, never
// a reason to refuse the Project.
std::vector<MemoryDocumentDigest> memory_document_digests_from_json(const Json &value) {
  std::vector<MemoryDocumentDigest> digests;
  if (!value.is_array()) return digests;
  for (const Json &entry : value) {
    if (!entry.is_object()) continue;
    MemoryDocumentDigest digest;
    digest.document_id = entry.value("documentId", std::string());
    digest.fingerprint = entry.value("fingerprint", std::string());
    digest.digest = entry.value("digest", std::string());
    digest.read_at = entry.value("readAt", std::string());
    if (!digest.document_id.empty()) digests.push_back(std::move(digest));
  }
  return digests;
}

// A Project that has chosen nothing about its appearance stores null, so an
// untouched Project's record stays exactly what it was.
Json project_appearance_to_json(const ProjectAppearance &appearance) {
  if (appearance.empty()) return nullptr;
  return Json{{"shortName", appearance.short_name},
              {"markGlyph", appearance.mark_glyph},
              {"markText", appearance.mark_text},
              {"markGround", appearance.mark_ground},
              {"markShape", appearance.mark_shape}};
}

ProjectAppearance project_appearance_from_json(const Json &value) {
  ProjectAppearance appearance;
  if (!value.is_object()) return appearance;
  const auto text = [&](const char *key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
  };
  appearance.short_name = text("shortName");
  appearance.mark_glyph = text("markGlyph");
  appearance.mark_text = text("markText");
  appearance.mark_ground = text("markGround");
  appearance.mark_shape = text("markShape");
  return appearance;
}

Json matter_to_json(const Matter &matter) {
  Json documents = Json::array();
  for (const auto &document : matter.documents) {
    documents.push_back(document_to_json(document));
  }
  Json notes = Json::array();
  for (const auto &note : matter.notes) {
    notes.push_back(note_to_json(note));
  }
  Json issues = Json::array();
  for (const auto &issue : matter.issues) {
    issues.push_back(issue_to_json(issue));
  }
	Json work_product_types = Json::array();
	for (const auto &type : matter.work_product_types) {
		work_product_types.push_back(work_product_type_to_json(type));
	}
	Json work_products = Json::array();
	for (const auto &product : matter.work_products) {
		work_products.push_back(work_product_to_json(product));
	}
	Json work_product_revisions = Json::array();
	for (const auto &revision : matter.work_product_revisions) {
		work_product_revisions.push_back(work_product_revision_to_json(revision));
	}
	Json consilium_configurations = Json::array();
	for (const consilium::Configuration& configuration : matter.consilium_configurations) {
		consilium_configurations.push_back(consilium_configuration_to_json(configuration));
	}
  Json tasks = Json::array();
  for (const auto &task : matter.tasks) {
    tasks.push_back(task_to_json(task));
  }
  Json chats = Json::array();
  for (const auto &chat : matter.chats) {
    Json chat_ai_setup_overrides = Json::array();
    for (const auto &override_entry : chat.ai_setup_overrides) {
      chat_ai_setup_overrides.push_back(ai_setup_override_to_json(override_entry));
    }
    chats.push_back({{"id", chat.id},
                     {"projectId", chat.project_id.empty() ? matter.id : chat.project_id},
                     {"title", chat.title}, {"titleAuthority", to_string(chat.title_authority)},
                     {"createdAt", chat.created_at}, {"updatedAt", chat.updated_at},
                     {"revision", chat.revision},
                     {"createdByActorId", chat.created_by_actor_id},
                     {"updatedByActorId", chat.updated_by_actor_id},
                     {"operationId", chat.operation_id}, {"archived", chat.archived},
                     {"deletedAt", chat.deleted_at}, {"deletedByActorId", chat.deleted_by_actor_id},
                     {"compaction", chat.compaction
                         ? Json{{"createdAt", chat.compaction->created_at},
                                {"createdByActorId", chat.compaction->created_by_actor_id},
                                {"coveredTaskIds", chat.compaction->covered_task_ids},
                                {"briefRecord", chat.compaction->brief_record},
                                {"record", chat.compaction->record},
                                {"citedDocumentIds", chat.compaction->cited_document_ids},
                                {"workProductIds", chat.compaction->work_product_ids},
                                {"artifactDocumentIds", chat.compaction->artifact_document_ids},
                                {"unresolved", chat.compaction->unresolved}}
                         : Json(nullptr)},
                     {"aiSetupOverrides", std::move(chat_ai_setup_overrides)},
                     {"selectedDocumentIds", chat.selected_document_ids},
                     {"draftText", chat.draft_text},
                     {"workingDocumentId", chat.working_document_id},
                     {"agentId", chat.agent_id},
                     {"instructions", chat.instructions}});
  }
  Json matter_ai_setup_overrides = Json::array();
  for (const auto &override_entry : matter.ai_setup_overrides) {
    matter_ai_setup_overrides.push_back(ai_setup_override_to_json(override_entry));
  }
  Json entities = Json::array();
  for (const auto &entity : matter.entities) entities.push_back(entity_to_json(entity));
  Json entity_ambiguities = Json::array();
  for (const auto &ambiguity : matter.entity_ambiguities) entity_ambiguities.push_back(entity_ambiguity_to_json(ambiguity));
  Json document_protection = Json::array();
  for (const auto &status : matter.document_protection) document_protection.push_back(document_protection_to_json(status));
  Json context_items = Json::array();
  for (const auto &item : matter.context_items) context_items.push_back(context_item_to_json(item));
  Json context_suggestions = Json::array();
  for (const auto &suggestion : matter.context_suggestions) context_suggestions.push_back(context_suggestion_to_json(suggestion));
  Json context_item_revisions = Json::array();
  for (const auto &revision : matter.context_item_revisions) {
    context_item_revisions.push_back(context_item_revision_to_json(revision));
  }
  Json document_events = Json::array();
  for (const auto &event : matter.document_events) document_events.push_back(document_event_to_json(event));
  Json value = Json{
      {"id", matter.id},
      {"name", matter.name},
      {"description", matter.description ? Json(*matter.description) : Json(nullptr)},
      {"instructions", matter.instructions ? Json(*matter.instructions) : Json(nullptr)},
      {"appearance", project_appearance_to_json(matter.appearance)},
      {"memoryDocumentDigests", memory_document_digests_to_json(matter.memory_document_digests)},
      {"directoryPath", matter.directory_path ? Json(*matter.directory_path) : Json(nullptr)},
      {"createdAt", matter.created_at},
      {"updatedAt", matter.updated_at},
      {"archivedAt", matter.archived_at},
      {"archivedBy", matter.archived_by_actor_id},
      {"documents", std::move(documents)},
      {"notes", std::move(notes)},
      {"issues", std::move(issues)},
		{"workProductTypes", std::move(work_product_types)},
		{"workProducts", std::move(work_products)},
		{"workProductRevisions", std::move(work_product_revisions)},
		{"consiliumConfigurations", std::move(consilium_configurations)},
      {"chats", std::move(chats)},
      {"tasks", std::move(tasks)},
      {"aiSetupOverrides", std::move(matter_ai_setup_overrides)},
      {"entities", std::move(entities)},
      {"entityAmbiguities", std::move(entity_ambiguities)},
      {"documentProtection", std::move(document_protection)},
      {"contextItems", std::move(context_items)},
      {"contextSuggestions", std::move(context_suggestions)},
      {"contextItemRevisions", std::move(context_item_revisions)},
      {"documentEvents", std::move(document_events)},
  };
  // Written only when a Project has Review Tables, so every other Project is
  // stored exactly as before.
  if (!matter.review_tables.empty() || !matter.review_rows.empty()) {
    Json reviewTables = Json::array();
    for (const ReviewTable &table : matter.review_tables) reviewTables.push_back(review_json::table_to_json(table));
    Json reviewRows = Json::array();
    for (const ReviewRow &row : matter.review_rows) reviewRows.push_back(review_json::row_to_json(row));
    value["reviewTables"] = std::move(reviewTables);
    value["reviewRows"] = std::move(reviewRows);
  }
  // Written only when a Project has document reviews, so every other Project
  // is stored exactly as before.
  if (!matter.document_reviews.empty()) {
    Json reviews = Json::array();
    for (const document_review::DocumentReview &review : matter.document_reviews)
      reviews.push_back(document_review::reviewToJson(review));
    value["documentReviews"] = std::move(reviews);
  }
  // Written only when a Matter has assignment records, so every other Matter
  // is stored exactly as before.
  if (!matter.assignment_records.empty()) {
    Json records = Json::array();
    for (const document_review::AssignmentRecord &record : matter.assignment_records)
      records.push_back(document_review::assignmentToJson(record));
    value["assignmentRecords"] = std::move(records);
  }
  // Written only when a Matter has letters filed to it, so every other Matter
  // is stored exactly as before.
  if (!matter.correspondence.empty()) {
    Json letters = Json::array();
    for (const correspondence::Letter &letter : matter.correspondence) letters.push_back(correspondence::letter_to_json(letter));
    value["correspondence"] = std::move(letters);
  }
  // Written only when the Matter has rules of its intake.
  if (!matter.correspondence_intake.empty()) {
    Json rules = Json::array();
    for (const correspondence::IntakeRule &rule : matter.correspondence_intake) rules.push_back(correspondence::intake_rule_to_json(rule));
    value["correspondenceIntake"] = std::move(rules);
  }
  // Written only when a member recorded time on the Matter, so every other
  // Matter is stored exactly as before.
  if (!matter.time_entries.empty()) {
    Json entries = Json::array();
    for (const TimeEntry &entry : matter.time_entries) entries.push_back(time_entry_to_json(entry));
    value["timeEntries"] = std::move(entries);
  }
  // Papinian Cycle 4 (DESIGN4 §4.5, §8.3): written only when there is
  // something to keep, so a Project without them is stored exactly as before.
  // Counts, keys and offsets only - never a value, never a mapping.
  const PersistedObservationLedger &ledger = matter.observation_ledger;
  if (!ledger.empty()) {
    value["observationLedger"] = Json{{"byRoot", ledger.by_root}, {"byCalendarRoot", ledger.by_calendar_root},
        {"byGroupKey", ledger.by_group_key}, {"answered", ledger.answered}, {"zeroDivisors", ledger.zero_divisors}};
    if (!ledger.preload_by_root.empty()) value["observationLedger"]["preloadByRoot"] = ledger.preload_by_root;
    if (!ledger.order_edges.empty()) value["observationLedger"]["orderEdges"] = ledger.order_edges;
  }
  if (!matter.written_file_anchors.empty()) {
    Json files = Json::object();
    for (const auto &[digest, anchors] : matter.written_file_anchors) {
      Json facts = Json::array();
      for (const WrittenFileFact &fact : anchors.facts)
        facts.push_back(Json{{"start", fact.start}, {"length", fact.length}, {"factKey", fact.fact_key},
            {"kind", fact.kind}, {"valueKey", fact.value_key}});
      Json spans = Json::array();
      for (const WrittenFileSpan &span : anchors.model_spans)
        spans.push_back(Json{{"start", span.start}, {"length", span.length}});
      files[digest] = Json{{"facts", std::move(facts)}, {"modelSpans", std::move(spans)}};
    }
    value["writtenFileAnchors"] = std::move(files);
  }
  return value;
}

Json matter_manifest_record(const Matter &matter) {
  Json value = matter_to_json(matter);
  if (matter.storage_cutover) value["storage"] = Json{{"kind", "lm"}};
  return value;
}

Matter matter_from_json(const Json &value) {
  Matter matter;
  matter.id = value.value("id", std::string());
  matter.storage_cutover = value.contains("storage") && value.at("storage").is_object()
      && value.at("storage").value("kind", std::string()) == "lm";
  matter.name = value.value("name", std::string());
  if (value.contains("description") && value.at("description").is_string()) {
    matter.description = value.at("description").get<std::string>();
  }
  if (value.contains("instructions") && value.at("instructions").is_string()) {
    matter.instructions = value.at("instructions").get<std::string>();
  }
  if (value.contains("appearance")) matter.appearance = project_appearance_from_json(value.at("appearance"));
  if (value.contains("memoryDocumentDigests")) {
    matter.memory_document_digests = memory_document_digests_from_json(value.at("memoryDocumentDigests"));
  }
  if (value.contains("directoryPath") && value.at("directoryPath").is_string()) {
    matter.directory_path = value.at("directoryPath").get<std::string>();
  }
  matter.created_at = value.value("createdAt", std::string());
  matter.updated_at = value.value("updatedAt", std::string());
  if (value.contains("archivedAt") && value.at("archivedAt").is_string()) matter.archived_at = value.at("archivedAt").get<std::string>();
  if (value.contains("archivedBy") && value.at("archivedBy").is_string())
    matter.archived_by_actor_id = value.at("archivedBy").get<std::string>();
  if (value.contains("documents") && value.at("documents").is_array()) {
    for (const auto &document : value.at("documents")) {
      matter.documents.push_back(document_from_json(document, matter.id));
    }
  }
  if (value.contains("notes") && value.at("notes").is_array()) {
    for (const auto &note : value.at("notes")) {
      matter.notes.push_back(note_from_json(note, matter.id));
    }
  }
  if (value.contains("issues") && value.at("issues").is_array()) {
    for (const auto &issue : value.at("issues")) {
      if (auto parsed = issue_from_json(issue, matter.id)) {
        matter.issues.push_back(std::move(*parsed));
      }
    }
  }
	if (value.contains("workProductTypes") && value.at("workProductTypes").is_array()) {
		for (const auto &type : value.at("workProductTypes")) {
			if (auto parsed = work_product_type_from_json(type, matter.id)) {
				matter.work_product_types.push_back(std::move(*parsed));
			}
		}
	}
	if (value.contains("workProducts") && value.at("workProducts").is_array()) {
		for (const auto &product : value.at("workProducts")) {
			if (auto parsed = work_product_from_json(product, matter.id)) {
				matter.work_products.push_back(std::move(*parsed));
			}
		}
	}
	if (value.contains("workProductRevisions") && value.at("workProductRevisions").is_array()) {
		for (const auto &revision : value.at("workProductRevisions")) {
			if (auto parsed = work_product_revision_from_json(revision, matter.id)) {
				matter.work_product_revisions.push_back(std::move(*parsed));
			}
		}
	}
	if (value.contains("consiliumConfigurations") && value.at("consiliumConfigurations").is_array()) {
		for (const Json& configuration : value.at("consiliumConfigurations")) {
			if (auto parsed = consilium_configuration_from_json(configuration, matter.id)) {
				matter.consilium_configurations.push_back(std::move(*parsed));
			}
		}
	}
  if (value.contains("chats") && value.at("chats").is_array()) {
    for (const auto &entry : value.at("chats")) {
      if (!entry.is_object()) continue;
      MatterChat chat;
      chat.id = entry.value("id", std::string());
      chat.project_id = entry.value("projectId", matter.id);
      chat.title = entry.value("title", std::string());
      chat.title_authority = entry.contains("titleAuthority")
          ? chat_title_authority_from_string(entry.value("titleAuthority", std::string()))
          : (chat.title == "New chat" ? ChatTitleAuthority::Default : ChatTitleAuthority::User);
      chat.created_at = entry.value("createdAt", std::string());
      chat.updated_at = entry.value("updatedAt", chat.created_at);
      chat.revision = std::max(1LL, entry.value("revision", 1LL));
      chat.created_by_actor_id = entry.value("createdByActorId", std::string("legacy"));
      chat.updated_by_actor_id = entry.value("updatedByActorId", chat.created_by_actor_id);
      chat.operation_id = entry.value("operationId", std::string("legacy_chat_record_") + chat.id);
      chat.archived = entry.value("archived", false);
      chat.deleted_at = entry.value("deletedAt", std::string());
      chat.deleted_by_actor_id = entry.value("deletedByActorId", std::string());
      if (entry.contains("compaction") && entry.at("compaction").is_object()) {
        const Json &compacted = entry.at("compaction");
        matters::ChatCompaction compaction;
        compaction.created_at = compacted.value("createdAt", std::string());
        compaction.created_by_actor_id = compacted.value("createdByActorId", std::string());
        compaction.covered_task_ids = compacted.value("coveredTaskIds", std::vector<std::string>{});
        // A compaction written before the brief form existed stands for both.
        compaction.brief_record = compacted.value("briefRecord", std::string());
        compaction.record = compacted.value("record", std::string());
        compaction.cited_document_ids = compacted.value("citedDocumentIds", std::vector<std::string>{});
        compaction.work_product_ids = compacted.value("workProductIds", std::vector<std::string>{});
        compaction.artifact_document_ids = compacted.value("artifactDocumentIds", std::vector<std::string>{});
        compaction.unresolved = compacted.value("unresolved", std::vector<std::string>{});
        chat.compaction = std::move(compaction);
      }
      if (entry.contains("aiSetupOverrides")) {
        chat.ai_setup_overrides = ai_setup_overrides_from_json(entry.at("aiSetupOverrides"));
      }
      if (entry.contains("selectedDocumentIds") && entry.at("selectedDocumentIds").is_array()) {
        for (const auto& document_id : entry.at("selectedDocumentIds")) {
          if (document_id.is_string()) chat.selected_document_ids.push_back(document_id.get<std::string>());
        }
      }
      chat.draft_text = entry.value("draftText", std::string());
      chat.working_document_id = entry.value("workingDocumentId", std::string());
      chat.agent_id = entry.value("agentId", std::string());
      chat.instructions = entry.value("instructions", std::string());
      if (!chat.id.empty() && !chat.title.empty()) matter.chats.push_back(std::move(chat));
    }
  }
  if (value.contains("tasks") && value.at("tasks").is_array()) {
    for (const auto &task : value.at("tasks")) {
      matter.tasks.push_back(task_from_json(task));
    }
  }
  // Deterministic compatibility for Alpha Project task history. Ungrouped
  // tasks remain the one canonical request/result/provenance/usage records;
  // this only assigns their chatId and supplies one stable Main chat identity.
  // No task evidence is copied into Chat storage.
  // Only a request somebody made in a conversation is history of a chat. Work
  // Singularity does for the Project on its own - keeping its memory, naming a
  // chat - is an operation of no chat, and is never given one.
  const auto is_ungrouped_request = [](const MatterTask &task) {
    return task.chat_id.has_value() == false && task.operation_type == "matter_task";
  };
  const bool has_ungrouped_tasks = std::any_of(matter.tasks.begin(), matter.tasks.end(), is_ungrouped_request);
  if (has_ungrouped_tasks) {
    const std::string compatibility_chat_id = "chat_legacy_main_" + matter.id;
    auto compatibility_chat = std::find_if(matter.chats.begin(), matter.chats.end(),
        [&](const MatterChat &chat) { return chat.id == compatibility_chat_id; });
    if (compatibility_chat == matter.chats.end()) {
      MatterChat chat;
      chat.id = compatibility_chat_id;
      chat.project_id = matter.id;
      chat.title = "Main chat";
      chat.title_authority = ChatTitleAuthority::Default;
      chat.created_at = matter.created_at;
      chat.updated_at = matter.created_at;
      chat.created_by_actor_id = "legacy";
      chat.updated_by_actor_id = "legacy";
      chat.operation_id = "legacy_chat_group_" + matter.id;
      matter.chats.push_back(std::move(chat));
      compatibility_chat = std::prev(matter.chats.end());
    }
    for (MatterTask &task : matter.tasks) {
      if (is_ungrouped_request(task) == false) continue;
      task.chat_id = compatibility_chat_id;
      if (compatibility_chat->created_at.empty() || task.created_at < compatibility_chat->created_at) {
        compatibility_chat->created_at = task.created_at;
      }
      if (task.created_at > compatibility_chat->updated_at) compatibility_chat->updated_at = task.created_at;
    }
  }
  if (value.contains("aiSetupOverrides")) {
    matter.ai_setup_overrides = ai_setup_overrides_from_json(value.at("aiSetupOverrides"));
  }
  if (value.contains("entities") && value.at("entities").is_array()) {
    for (const auto &entity : value.at("entities")) matter.entities.push_back(entity_from_json(entity));
  }
  if (value.contains("entityAmbiguities") && value.at("entityAmbiguities").is_array()) {
    for (const auto &ambiguity : value.at("entityAmbiguities")) {
      matter.entity_ambiguities.push_back(entity_ambiguity_from_json(ambiguity));
    }
  }
  if (value.contains("documentProtection") && value.at("documentProtection").is_array()) {
    for (const auto &status : value.at("documentProtection")) {
      matter.document_protection.push_back(document_protection_from_json(status));
    }
  }
  if (value.contains("contextItems") && value.at("contextItems").is_array()) {
    for (const auto &item : value.at("contextItems")) matter.context_items.push_back(context_item_from_json(item));
  }
  if (value.contains("contextSuggestions") && value.at("contextSuggestions").is_array()) {
    for (const auto &suggestion : value.at("contextSuggestions")) {
      matter.context_suggestions.push_back(context_suggestion_from_json(suggestion));
    }
  }
  if (value.contains("contextItemRevisions") && value.at("contextItemRevisions").is_array()) {
    for (const auto &revision : value.at("contextItemRevisions")) {
      matter.context_item_revisions.push_back(context_item_revision_from_json(revision));
    }
  }
  if (value.contains("documentEvents") && value.at("documentEvents").is_array()) {
    for (const auto &entry : value.at("documentEvents")) {
      if (auto event = document_event_from_json(entry)) matter.document_events.push_back(std::move(*event));
    }
  }
  if (value.contains("reviewTables") && value.at("reviewTables").is_array())
    for (const Json &entry : value.at("reviewTables")) {
      ReviewTable table = review_json::table_from_json(entry);
      if (!table.id.empty()) matter.review_tables.push_back(std::move(table));
    }
  if (value.contains("reviewRows") && value.at("reviewRows").is_array())
    for (const Json &entry : value.at("reviewRows")) {
      ReviewRow row = review_json::row_from_json(entry);
      if (!row.id.empty()) matter.review_rows.push_back(std::move(row));
    }
  if (value.contains("documentReviews") && value.at("documentReviews").is_array())
    for (const Json &entry : value.at("documentReviews")) {
      document_review::DocumentReview review = document_review::reviewFromJson(entry);
      if (!review.id.empty()) matter.document_reviews.push_back(std::move(review));
    }
  if (value.contains("assignmentRecords") && value.at("assignmentRecords").is_array())
    for (const Json &entry : value.at("assignmentRecords")) {
      document_review::AssignmentRecord record = document_review::assignmentFromJson(entry);
      if (!record.id.empty()) matter.assignment_records.push_back(std::move(record));
    }
  if (value.contains("correspondence") && value.at("correspondence").is_array())
    for (const Json &entry : value.at("correspondence")) {
      correspondence::Letter letter = correspondence::letter_from_json(entry);
      if (!letter.id.empty()) matter.correspondence.push_back(std::move(letter));
    }
  if (value.contains("correspondenceIntake") && value.at("correspondenceIntake").is_array())
    for (const Json &entry : value.at("correspondenceIntake")) {
      correspondence::IntakeRule rule = correspondence::intake_rule_from_json(entry);
      if (!rule.mailbox.empty() && !rule.from.empty()) matter.correspondence_intake.push_back(std::move(rule));
    }
  if (value.contains("timeEntries") && value.at("timeEntries").is_array())
    for (const Json &entry : value.at("timeEntries")) {
      TimeEntry kept = time_entry_from_json(entry);
      if (!kept.id.empty()) matter.time_entries.push_back(std::move(kept));
    }
  // Additive (DESIGN4 §4.5, §8.3): a Project written before them reads with an
  // empty ledger and no written-file anchors.
  if (value.contains("observationLedger") && value.at("observationLedger").is_object()) {
    const Json &ledger = value.at("observationLedger");
    const auto counts = [&ledger](const char *name, std::map<std::string, std::size_t> &into) {
      if (!ledger.contains(name) || !ledger.at(name).is_object()) return;
      for (const auto &[key, count] : ledger.at(name).items())
        if (count.is_number_unsigned()) into[key] = count.get<std::size_t>();
    };
    const auto keys = [&ledger](const char *name, std::vector<std::string> &into) {
      if (!ledger.contains(name) || !ledger.at(name).is_array()) return;
      for (const Json &key : ledger.at(name))
        if (key.is_string()) into.push_back(key.get<std::string>());
    };
    counts("byRoot", matter.observation_ledger.by_root);
    counts("byCalendarRoot", matter.observation_ledger.by_calendar_root);
    counts("byGroupKey", matter.observation_ledger.by_group_key);
    keys("answered", matter.observation_ledger.answered);
    keys("zeroDivisors", matter.observation_ledger.zero_divisors);
    counts("preloadByRoot", matter.observation_ledger.preload_by_root);
    keys("orderEdges", matter.observation_ledger.order_edges);
  }
  if (value.contains("writtenFileAnchors") && value.at("writtenFileAnchors").is_object()) {
    for (const auto &[digest, entry] : value.at("writtenFileAnchors").items()) {
      if (!entry.is_object()) continue;
      WrittenFileAnchors anchors;
      if (entry.contains("facts") && entry.at("facts").is_array())
        for (const Json &fact : entry.at("facts")) {
          if (!fact.is_object() || !fact.contains("start") || !fact.at("start").is_number_unsigned()
              || !fact.contains("length") || !fact.at("length").is_number_unsigned()) continue;
          anchors.facts.push_back({fact.at("start").get<std::size_t>(), fact.at("length").get<std::size_t>(),
              fact.value("factKey", std::string()), fact.value("kind", std::string()),
              fact.value("valueKey", std::string())});
        }
      if (entry.contains("modelSpans") && entry.at("modelSpans").is_array())
        for (const Json &span : entry.at("modelSpans")) {
          if (!span.is_object() || !span.contains("start") || !span.at("start").is_number_unsigned()
              || !span.contains("length") || !span.at("length").is_number_unsigned()) continue;
          anchors.model_spans.push_back({span.at("start").get<std::size_t>(), span.at("length").get<std::size_t>()});
        }
      matter.written_file_anchors[digest] = std::move(anchors);
    }
  }
  return matter;
}

std::vector<Matter> matters_from_json(const Json &data) {
  if (!data.is_object() || !data.contains("matters") || !data.at("matters").is_array()) {
    throw std::runtime_error("Matter manifest has an invalid schema");
  }
  std::vector<Matter> matters;
  for (const auto &entry : data.at("matters")) {
    matters.push_back(matter_from_json(entry));
  }
  return matters;
}

enum class PhysicalSourceState { Current, Missing, Unreadable };

struct PhysicalSourceRead {
  PhysicalSourceState state = PhysicalSourceState::Missing;
  std::string fingerprint;
  documents::DocumentExtractionResult extraction;
};

// The one read-boundary check every model-evidence materialization goes
// through: resolve the document's current physical location, require a real
// regular file (never following a symlink), then let DocumentCache::prepare()
// hash its current bytes - an unchanged file reuses its derivative, a changed
// one is re-extracted under its new content hash. Never persists registry
// state; the watcher/pipeline still owns that.
PhysicalSourceRead read_current_physical_source(const std::filesystem::path &workspace_path,
    const Matter &matter, const MatterDocument &document, const documents::DocumentCache &cache) {
  PhysicalSourceRead read;
  std::filesystem::path path;
  if (document.source_mode == DocumentSourceMode::Referenced) {
    if (!matter.directory_path || matter.directory_path->empty()
        || !documents::registry::is_relative_path_safe(*matter.directory_path, document.relative_path))
      return read;
    path = std::filesystem::path(*matter.directory_path) / document.relative_path;
  } else {
    const auto managed = managed_document_path(workspace_path, matter.id, document.stored_name);
    if (!managed) return read;
    path = *managed;
  }
  std::error_code statusError;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, statusError)) || statusError)
    return read;
  const std::string extractorVersion = documents::local_extractor_version(document.file_type);
  try {
    const documents::PreparedDocument prepared = cache.prepare(path, document.file_type,
        extractorVersion.empty() ? "unsupported/1" : extractorVersion);
    read.fingerprint = prepared.fingerprint;
    read.extraction = prepared.extraction;
    read.state = prepared.extraction.ai_ready() ? PhysicalSourceState::Current : PhysicalSourceState::Unreadable;
  } catch (const std::exception &) {
    std::error_code recheckError;
    read.state = std::filesystem::is_regular_file(std::filesystem::symlink_status(path, recheckError)) && !recheckError
        ? PhysicalSourceState::Unreadable : PhysicalSourceState::Missing;
  }
  return read;
}
} // namespace

nlohmann::json serialize_matter_storage(const Matter &matter) {
  return matter_to_json(matter);
}

Matter deserialize_matter_storage(const nlohmann::json &value) {
  return matter_from_json(value);
}

context::ContextItem deserialize_context_item_storage(const nlohmann::json &value) {
  return context_item_from_json(value);
}

nlohmann::json serialize_context_item_storage(const context::ContextItem &item) {
  return context_item_to_json(item);
}

const char *to_string(DocumentSourceMode mode) noexcept {
  switch (mode) {
    case DocumentSourceMode::Managed:
      return "managed";
    case DocumentSourceMode::Referenced:
      return "referenced";
  }
  return "managed";
}

const char *to_string(DocumentExtractionState status) noexcept {
  switch (status) {
    case DocumentExtractionState::NotExtracted:
      return "not_extracted";
    case DocumentExtractionState::Ready:
      return "ready";
    case DocumentExtractionState::Empty:
      return "empty";
    case DocumentExtractionState::OcrRequired:
      return "ocr_required";
    case DocumentExtractionState::Unsupported:
      return "unsupported";
    case DocumentExtractionState::Unreadable:
      return "unreadable";
  }
  return "not_extracted";
}

DocumentExtractionState
to_document_extraction_state(documents::ExtractionStatus status) noexcept {
  switch (status) {
    case documents::ExtractionStatus::ready:
      return DocumentExtractionState::Ready;
    case documents::ExtractionStatus::empty:
      return DocumentExtractionState::Empty;
    case documents::ExtractionStatus::ocr_required:
      return DocumentExtractionState::OcrRequired;
    case documents::ExtractionStatus::unsupported:
      return DocumentExtractionState::Unsupported;
    case documents::ExtractionStatus::unreadable:
      return DocumentExtractionState::Unreadable;
  }
  return DocumentExtractionState::NotExtracted;
}

const char* to_string(DocumentLifecycleState state) noexcept {
	switch (state) {
		case DocumentLifecycleState::Ready:
			return "ready";
		case DocumentLifecycleState::Processing:
			return "processing";
		case DocumentLifecycleState::Changed:
			return "changed";
		case DocumentLifecycleState::NeedsAttention:
			return "needs_attention";
		case DocumentLifecycleState::Unsupported:
			return "unsupported";
		case DocumentLifecycleState::Unavailable:
			return "unavailable";
	}
	return "processing";
}

DocumentLifecycleState document_lifecycle_state(const MatterDocument& document) noexcept {
	if (document.source_mode == DocumentSourceMode::Referenced) {
		if (document.available == false) return DocumentLifecycleState::Unavailable;
		return document.lifecycle_state;
	}
	switch (document.extraction_status) {
		case DocumentExtractionState::Ready:
			return DocumentLifecycleState::Ready;
		case DocumentExtractionState::Unsupported:
			return DocumentLifecycleState::Unsupported;
		case DocumentExtractionState::NotExtracted:
			return DocumentLifecycleState::Processing;
		case DocumentExtractionState::Empty:
		case DocumentExtractionState::OcrRequired:
		case DocumentExtractionState::Unreadable:
			return DocumentLifecycleState::NeedsAttention;
	}
	return DocumentLifecycleState::Processing;
}

const char *to_string(IssueStatus status) noexcept {
  switch (status) {
    case IssueStatus::Open:
      return "open";
    case IssueStatus::Reviewed:
      return "reviewed";
    case IssueStatus::Resolved:
      return "resolved";
  }
  return "open";
}

std::optional<IssueStatus> issue_status_from_string(const std::string &value) noexcept {
  if (value == "open") return IssueStatus::Open;
  if (value == "reviewed") return IssueStatus::Reviewed;
  if (value == "resolved") return IssueStatus::Resolved;
  return std::nullopt;
}

const char* to_string(WorkProductOriginKind kind) noexcept {
	switch (kind) {
		case WorkProductOriginKind::Manual:
			return "manual";
		case WorkProductOriginKind::ChatTask:
			return "chat_task";
		case WorkProductOriginKind::Finding:
			return "finding";
		case WorkProductOriginKind::Legacy:
			return "legacy";
		case WorkProductOriginKind::Agent:
			return "agent";
		case WorkProductOriginKind::ReviewTable:
			return "review_table";
	}
	return "manual";
}

std::optional<WorkProductOriginKind> work_product_origin_kind_from_string(
		const std::string& value) noexcept {
	if (value == "manual") return WorkProductOriginKind::Manual;
	if (value == "chat_task") return WorkProductOriginKind::ChatTask;
	if (value == "finding") return WorkProductOriginKind::Finding;
	if (value == "legacy") return WorkProductOriginKind::Legacy;
	if (value == "agent") return WorkProductOriginKind::Agent;
	if (value == "review_table") return WorkProductOriginKind::ReviewTable;
	return std::nullopt;
}

const char *to_string(ChatTitleAuthority authority) noexcept {
  switch (authority) {
    case ChatTitleAuthority::Default:
      return "default";
    case ChatTitleAuthority::Automatic:
      return "automatic";
    case ChatTitleAuthority::User:
      return "user";
  }
  return "default";
}

ChatTitleAuthority chat_title_authority_from_string(const std::string &value) noexcept {
  if (value == "automatic") return ChatTitleAuthority::Automatic;
  if (value == "user") return ChatTitleAuthority::User;
  return ChatTitleAuthority::Default;
}

const char *to_string(ConsiliumRole role) noexcept {
  switch (role) {
    case ConsiliumRole::Member:
      return "member";
    case ConsiliumRole::Synthesis:
      return "synthesis";
  }
  return "member";
}

std::optional<ConsiliumRole> consilium_role_from_string(const std::string &value) noexcept {
  if (value == "member") return ConsiliumRole::Member;
  if (value == "synthesis") return ConsiliumRole::Synthesis;
  return std::nullopt;
}

Issue promote_finding_to_issue(const Finding &finding, const std::string &matter_id,
                               const std::string &id, const std::string &created_at) {
  Issue issue;
  issue.id = id;
  issue.matter_id = matter_id;
  issue.title = finding.title;
  issue.description = finding.description;
  issue.status = IssueStatus::Open;
  issue.source_refs = finding.source_refs;
  issue.created_at = created_at;
  issue.updated_at = created_at;
	issue.origin_finding_id = finding.id;
  return issue;
}

std::vector<MatterTask> consilium_members(const std::vector<MatterTask> &tasks,
                                          const std::string &group_id) {
  std::vector<MatterTask> result;
  for (const auto &task : tasks) {
    if (task.consilium_group_id == group_id && task.consilium_role == ConsiliumRole::Member) {
      result.push_back(task);
    }
  }
  return result;
}

std::string build_consilium_deliberation_prompt(const std::string &objective,
                                               const std::vector<MatterTask> &answered,
                                               const std::string &participant_role) {
  std::string prompt;
  prompt += "You are one participant in a legal Consilium.";
  if (!participant_role.empty()) {
    prompt += " Your role in it is: ";
    prompt += participant_role;
    prompt += ".";
  }
  prompt += "\n\nThe objective put to the panel:\n";
  prompt += objective;
  prompt += "\n\nParticipants who have already answered, reproduced verbatim:\n\n";
  for (const auto &earlier : answered) {
    prompt += "--- ";
    prompt += earlier.consilium_participant_role.value_or(std::string("participant"));
    prompt += " (";
    prompt += earlier.provider_id.value_or("unknown provider");
    prompt += "/";
    prompt += earlier.model_id.value_or("unknown model");
    prompt += ") ---\n";
    prompt += earlier.response_text.value_or("");
    prompt += "\n\n";
  }
  prompt += "Answer the objective from your own role. Say plainly where you agree with a "
            "participant above and where you do not, and why. Do not repeat a point already "
            "made unless you are disputing or qualifying it, and do not add facts none of the "
            "supplied material states.";
  return prompt;
}

std::string build_consilium_synthesis_prompt(const std::string &original_prompt,
                                              const std::vector<MatterTask> &members) {
  std::string prompt;
  prompt += "Consilium synthesis request: the following independent legal task was sent, "
            "separately and without cross-visibility, to each of ";
  prompt += std::to_string(members.size());
  prompt += " connected models. Their answers are reproduced verbatim below.\n\n";
  prompt += "Original task:\n";
  prompt += original_prompt;
  prompt += "\n\n";
  for (const auto &member : members) {
    prompt += "--- ";
    prompt += member.provider_id.value_or("unknown provider");
    prompt += "/";
    prompt += member.model_id.value_or("unknown model");
    prompt += " ---\n";
    prompt += member.response_text.value_or("");
    prompt += "\n\n";
  }
  prompt += "Synthesize a single reconciled answer to the original task. Explicitly note any "
            "material disagreement between the models rather than silently choosing one. Do "
            "not present a point as agreed if only one model made it, and do not add facts "
            "none of the models stated.";
  return prompt;
}

std::optional<ExportSourceType> export_source_type_from_string(const std::string &value) noexcept {
  if (value == "task") return ExportSourceType::Task;
  if (value == "note") return ExportSourceType::Note;
	if (value == "work_product") return ExportSourceType::WorkProduct;
  return std::nullopt;
}

std::string resolve_export_content(const Matter &matter, ExportSourceType source_type,
                                   const std::string &source_id) {
  if (source_type == ExportSourceType::Task) {
    const auto task = std::find_if(matter.tasks.begin(), matter.tasks.end(),
                                   [&](const MatterTask &candidate) { return candidate.id == source_id; });
    if (task == matter.tasks.end() || task->status != TaskStatus::Success || !task->response_text) {
      throw std::runtime_error("This result no longer exists.");
    }
    return *task->response_text;
  }
  const auto note = std::find_if(matter.notes.begin(), matter.notes.end(),
                                 [&](const MatterNote &candidate) { return candidate.id == source_id; });
  if (note != matter.notes.end()) return note->content;
	if (source_type == ExportSourceType::Note) {
		throw std::runtime_error("This note no longer exists.");
	}
	const auto product = std::find_if(matter.work_products.begin(), matter.work_products.end(),
			[&](const WorkProduct &candidate) { return candidate.id == source_id; });
	if (product != matter.work_products.end()) return product->content;
	const auto issue = std::find_if(matter.issues.begin(), matter.issues.end(),
			[&](const Issue &candidate) { return candidate.id == source_id; });
	if (issue != matter.issues.end()) {
		throw std::runtime_error("This structured Issue is not available as a text export.");
	}
	throw std::runtime_error("This Work Product no longer exists.");
}

void write_export_file(const std::filesystem::path &path, const std::string &content) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    throw std::runtime_error("Could not write the export file.");
  }
  file << content;
  file.flush();
  if (!file) {
    throw std::runtime_error("Could not write the export file.");
  }
  file.close();
  if (file.fail()) {
    throw std::runtime_error("Could not write the export file.");
  }
}

std::string generate_matter_id() { return generate_id("mt_"); }
std::string generate_document_id() { return generate_id("doc_"); }
std::string generate_document_event_id() { return generate_id("dev_"); }
std::string generate_task_id() { return generate_id("tsk_"); }
std::string generate_review_table_id() { return generate_id("rt_"); }
std::string generate_review_column_id() { return generate_id("col_"); }
std::string generate_review_run_id() { return generate_id("rtr_"); }
std::string generate_chat_id() { return generate_id("chat_"); }
std::string generate_chat_operation_id() { return generate_id("cho_"); }
std::string generate_finding_id() { return generate_id("fnd_"); }
std::string generate_obligation_id() { return generate_id("obl_"); }
std::string generate_event_id() { return generate_id("evt_"); }
std::string generate_change_id() { return generate_id("chg_"); }
std::string generate_note_id() { return generate_id("note_"); }
std::string generate_issue_id() { return generate_id("iss_"); }
std::string generate_work_product_id() { return generate_id("wp_"); }
std::string generate_work_product_type_id() { return generate_id("wpt_"); }
std::string generate_work_product_revision_id() { return generate_id("wpr_"); }
std::string generate_consilium_configuration_id() { return generate_id("csc_"); }
std::string generate_consilium_participant_id() { return generate_id("csp_"); }
std::string generate_consilium_stage_id() { return generate_id("css_"); }
std::string generate_consilium_operation_id() { return generate_id("cso_"); }
std::string generate_consilium_group_id() { return generate_id("csl_"); }
std::string generate_workflow_run_id() { return generate_id("wfr_"); }

std::string task_group_id(const MatterTask &task) {
  return task.root_task_id.value_or(task.id);
}

int next_attempt_number(const std::vector<MatterTask> &tasks, const MatterTask &existing) {
  const auto group_id = task_group_id(existing);
  int highest_attempt = 0;
  for (const auto &task : tasks) {
    if (task_group_id(task) == group_id) {
      highest_attempt = std::max(highest_attempt, task.attempt);
    }
  }
  return highest_attempt + 1;
}

std::vector<MatterTask> exclude_chain(const std::vector<MatterTask> &tasks,
                                      const std::string &chain_id) {
  std::vector<MatterTask> result;
  result.reserve(tasks.size());
  for (const auto &task : tasks) {
    if (task_group_id(task) != chain_id) result.push_back(task);
  }
  return result;
}

std::optional<std::string> resolve_retry_matter_instructions(
    const MatterTask &original, const std::optional<std::string> &current_matter_instructions) {
  if (original.matter_instructions_snapshot_captured) {
    return original.matter_instructions_snapshot;
  }
  return current_matter_instructions;
}

bool has_committed_document_change(const MatterTask &task) {
  if (!task.document_action_evidence.empty()) return true;
  return task.document_operation_checkpoint && task.document_operation_checkpoint->stage == "committed";
}

bool is_retry_eligible(const MatterTask &task) {
  // A request whose change was already written - stopped or failed in a
  // later step - is never run again as it was: a new attempt would only
  // write it twice.
  if (has_committed_document_change(task)) return false;
  if (task.status == TaskStatus::Success && !task.stopped_by_user) return false;
  if (task.error_code && !providers::is_retryable(*task.error_code)) return false;
  return true;
}

bool is_same_model_target(const MatterTask &task, const std::string &provider_id,
                          const std::string &model_id) {
  return task.provider_id.has_value() && task.model_id.has_value() &&
         *task.provider_id == provider_id && *task.model_id == model_id;
}

std::filesystem::path manifest_path(const std::filesystem::path &workspace_path) {
  return workspace_path / ".singularity-workspace" / "matters.json";
}

bool is_valid_matter_id(const std::string &matter_id) {
  if (matter_id.empty() || matter_id.size() > 128) return false;
  return std::all_of(matter_id.begin(), matter_id.end(), [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
        || (character >= '0' && character <= '9') || character == '_' || character == '-';
  });
}

std::filesystem::path documents_directory(const std::filesystem::path &workspace_path,
                                          const std::string &matter_id) {
  if (!is_valid_matter_id(matter_id)) throw std::invalid_argument("This Project's identity is not valid.");
  return workspace_path / ".singularity-workspace" / "documents" / matter_id;
}

std::optional<std::filesystem::path> managed_document_path(const std::filesystem::path &workspace_path,
                                                           const std::string &matter_id,
                                                           const std::string &stored_name) {
  if (!is_valid_matter_id(matter_id) || stored_name.empty() || stored_name == "." || stored_name == ".."
      || stored_name.find_first_of(std::string("/\\\0", 3)) != std::string::npos)
    return std::nullopt;
  const std::filesystem::path name(stored_name);
  if (name.is_absolute() || name.has_parent_path() || name.filename() != name) return std::nullopt;
  return documents_directory(workspace_path, matter_id) / name;
}

bool holds_managed_copy(const std::filesystem::path &workspace_path, const std::string &matter_id,
                        const MatterDocument &document) {
  if (document.source_mode != DocumentSourceMode::Managed) return false;
  const auto kept = managed_document_path(workspace_path, matter_id, document.stored_name);
  std::error_code missing;
  return kept.has_value() && std::filesystem::is_regular_file(*kept, missing);
}

// Raw "<workspace>/.singularity-workspace/matters.json" read/write, unaware of .lm.
// load_matters/save_matters below are the public facade: for a Matter that has been
// cut over to portable .lm truth, they transparently read/write .lm instead, so
// every existing caller (Matter/Activity/Chat/ Document/Routing/WorkProduct -
// anything that already does load-modify-save through this repository) gets correct
// .lm-canonical behavior with no call-site changes. matters.json as it was last
// parsed, for as long as it is the same file: a Project read (matter.load) happens
// many times in one request, and each one parsed the whole manifest again. A file
// replaced since - every write is an atomic replacement, so a new file - or changed
// in place is parsed again, and this process's own writes let go of it at once.
namespace {

// The file's own identity is part of the stamp on every platform: a
// replacement of the same size whose time lands in the same tick - what a
// sync client or another computer replacing matters.json can do - is still
// a new file (its own inode, or its own NTFS file id and creation time).
struct ManifestStamp {
  bool present = false;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type modified{};
  std::optional<std::uint64_t> device_id;
  std::optional<std::uint64_t> file_id;
  std::optional<std::string> creation_time_token;
  bool operator==(const ManifestStamp &other) const {
    return present == other.present && size == other.size && modified == other.modified
        && device_id == other.device_id && file_id == other.file_id
        && creation_time_token == other.creation_time_token;
  }
};

ManifestStamp manifest_stamp_of(const std::filesystem::path &path) {
  ManifestStamp stamp;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return stamp;
  stamp.size = std::filesystem::file_size(path, error);
  if (error) return stamp;
  stamp.modified = std::filesystem::last_write_time(path, error);
  if (error) return stamp;
  platform::FileIdentity identity;
  if (platform::read_file_identity(path, identity)) {
    stamp.device_id = identity.device_id;
    stamp.file_id = identity.file_id;
    stamp.creation_time_token = identity.creation_time_token;
  }
  stamp.present = true;
  return stamp;
}

struct KeptManifest {
  std::mutex mutex;
  std::filesystem::path path;
  ManifestStamp stamp;
  std::shared_ptr<const std::vector<Matter>> matters;
};

KeptManifest &kept_manifest() {
  static KeptManifest *kept = new KeptManifest();
  return *kept;
}

void forget_kept_manifest() {
  KeptManifest &kept = kept_manifest();
  const std::lock_guard<std::mutex> lock(kept.mutex);
  kept.matters.reset();
}

std::vector<Matter> parse_manifest_keeping_it(const std::filesystem::path &path) {
  const ManifestStamp stamp = manifest_stamp_of(path);
  KeptManifest &kept = kept_manifest();
  if (stamp.present) {
    const std::lock_guard<std::mutex> lock(kept.mutex);
    if (kept.matters && kept.path == path && kept.stamp == stamp) return *kept.matters;
  }
  auto parsed = std::make_shared<const std::vector<Matter>>(matters_from_json(read_json_file(path)));
  // Kept only if the file did not change while it was being read.
  if (stamp.present && manifest_stamp_of(path) == stamp) {
    const std::lock_guard<std::mutex> lock(kept.mutex);
    kept.path = path;
    kept.stamp = stamp;
    kept.matters = parsed;
  }
  return *parsed;
}

} // namespace

static std::vector<Matter> read_legacy_manifest(const std::filesystem::path &workspace_path) {
  std::error_code exists_error;
  const auto path = manifest_path(workspace_path);
  if (!std::filesystem::is_regular_file(path, exists_error)) {
    const auto recovery = previous_path(path);
    if (!std::filesystem::is_regular_file(recovery, exists_error)) {
      return {};
    }

    // A prior process may have stopped after moving a damaged/current file
    // aside but before promoting the recovery copy. Validate and restore the
    // primary so later saves have a normal base again.
    const auto recovered = matters_from_json(read_json_file(recovery));
    const auto recovery_temporary = unique_temporary_path(path);
    try {
      std::filesystem::copy_file(recovery, recovery_temporary,
                                 std::filesystem::copy_options::overwrite_existing);
      atomic_replace(recovery_temporary, path);
    } catch (...) {
      std::error_code cleanup_error;
      std::filesystem::remove(recovery_temporary, cleanup_error);
      throw;
    }
    return recovered;
  }

  try {
    return parse_manifest_keeping_it(path);
  } catch (const std::exception &primary_error) {
    forget_kept_manifest();
    const auto recovery = previous_path(path);
    try {
      (void)matters_from_json(read_json_file(recovery));
    } catch (const std::exception &) {
      throw std::runtime_error("Matter manifest is corrupt or unreadable and could not be recovered: " +
                               path.string() + " (" + primary_error.what() + ")");
    }

    // Do not leave the registry in a state where reads appear to work from
    // `.previous` but every subsequent save refuses to overwrite the still
    // corrupt primary manifest. Preserve the damaged bytes under a unique
    // sibling name, restore the last-known-good file as the active manifest,
    // and surface the recovery once. Retrying the user action then operates
    // on the repaired primary file.
    const auto preserved = unique_corrupt_path(path);
    atomic_replace(path, preserved);

    const auto recovery_temporary = unique_temporary_path(path);
    try {
      std::filesystem::copy_file(recovery, recovery_temporary,
                                 std::filesystem::copy_options::overwrite_existing);
      atomic_replace(recovery_temporary, path);
    } catch (...) {
      std::error_code cleanup_error;
      std::filesystem::remove(recovery_temporary, cleanup_error);
      throw;
    }

    throw std::runtime_error(
        "Matter manifest was corrupt. Singularity preserved the damaged file at " +
        preserved.string() + " and restored the previous valid copy. Retry the action.");
  }
}

static void write_legacy_manifest(const std::filesystem::path &workspace_path, const std::vector<Matter> &matters) {
  // What was kept describes the file this call replaces, whatever the outcome.
  struct ForgetKept {
    ~ForgetKept() { forget_kept_manifest(); }
  } forgetKept;
  forget_kept_manifest();
  const auto directory = workspace_path / ".singularity-workspace";
  platform::ensure_record_directory(directory);

  Json list = Json::array();
  for (const auto &matter : matters) {
    list.push_back(matter_manifest_record(matter));
  }
  const Json data{{"version", 2}, {"matters", std::move(list)}};

  // Written the way every file Singularity writes is (Platform/AtomicFile): room
  // asked for first, so a full disk refuses the save as DiskFull in plain words
  // before anything is begun; the bytes on the disk before the rename that puts
  // them in place; no path in any message.
  const auto path = manifest_path(workspace_path);
  const std::string text = data.dump(2);
  platform::require_room_for_record(path, text.size());
  const auto temporary_path = unique_temporary_path(path);

  try {
    platform::write_file_durably(temporary_path, text);

    std::error_code exists_error;
    if (std::filesystem::is_regular_file(path, exists_error)) {
      // Refuse to overwrite the only evidence of a corrupt manifest. The
      // caller can surface this as a recoverable workspace error and the
      // original bytes remain available for repair.
      {
        // Read here without read_json_file(), whose message names the file:
        // a save of the Project list says what failed in plain words only.
        std::ifstream current(path, std::ios::binary);
        if (!current) {
          throw std::runtime_error(
              "Singularity could not read its current list of Projects, so the list was not saved. Nothing was lost.");
        }
        (void)matters_from_json(Json::parse(current));
      }
      const auto recovery_temporary = unique_temporary_path(previous_path(path));
      try {
        platform::stage_copy_of_file(path, recovery_temporary);
        platform::commit_staged_file(recovery_temporary, previous_path(path),
                                     platform::StagedCommitMode::ReplaceExisting, false);
      } catch (...) {
        std::error_code recovery_cleanup_error;
        std::filesystem::remove(recovery_temporary, recovery_cleanup_error);
        throw;
      }
    }
    platform::commit_staged_file(temporary_path, path, platform::StagedCommitMode::ReplaceExisting, false);
  } catch (...) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
    throw;
  }
}

// Whether .lm - not matters.json - is canonical for this Matter's full
// content: true once its directory_path has a portable Project folder
// set up. Checked fresh against the filesystem every time, never
// cached, so it always reflects the real cutover state.
static bool has_cutover_project_data(const Matter &matter) {
  if (!matter.directory_path || matter.directory_path->empty()) return false;
  return persistence::has_project_data(std::filesystem::path(*matter.directory_path));
}

// What a cut-over Matter leaves behind in matters.json: a locator only (id +
// directoryPath), marked as a cut-over locator, never a content snapshot. It
// is enough for load_matters() to find and hydrate .lm on the next read, never
// enough to be mistaken for canonical content if .lm becomes unreachable.
static Matter locator_stub(const Matter &matter) {
  Matter stub;
  stub.id = matter.id;
  stub.directory_path = matter.directory_path;
  stub.storage_cutover = true;
  return stub;
}

// A locator written before the cut-over marker existed: an id and a location
// and nothing else a Project record always has.
static bool is_unmarked_locator(const Matter &matter) {
  return matter.name.empty() && matter.created_at.empty() && matter.directory_path
      && !matter.directory_path->empty();
}

// The stand-in a cut-over Project is listed as while its data cannot be used:
// recognizable by its folder name, carrying why, and unchangeable.
static Matter unavailable_project(const Matter &locator, ProjectDataState state, std::string reason) {
  Matter stub = locator_stub(locator);
  if (stub.directory_path) stub.name = std::filesystem::path(*stub.directory_path).filename().u8string();
  stub.data_state = state;
  stub.data_state_reason = std::move(reason);
  return stub;
}

ProjectDataError::ProjectDataError(ProjectDataState state, const std::string &message)
    : std::runtime_error(message),
      code_(state == ProjectDataState::Damaged ? "project_data_damaged" : "project_data_unavailable") {}

void require_readable(const Matter &matter) {
  if (matter.data_state == ProjectDataState::Damaged)
    throw ProjectDataError(matter.data_state,
        "This Matter's Singularity data can't be read right now. Singularity reads it again by itself.");
  if (matter.data_state == ProjectDataState::Unavailable)
    throw ProjectDataError(matter.data_state,
        "This Matter's folder can't be reached right now. Singularity reads it again by itself.");
}

// One entry of matters.json made into what the workspace lists for it: a
// cut-over Project's live .lm state, or the stand-in that says why its data
// cannot be used. load_matters() and load_matter() both list a Project
// through this, so the two can never disagree about it.
static void hydrate_listed_matter(Matter &matter) {
  const bool cutover = matter.storage_cutover || is_unmarked_locator(matter);
  if (!cutover && has_cutover_project_data(matter) == false) {
    // Not a cut-over Project as far as this Node can see - unless the reason
    // it cannot see is that the folder refuses to be read, in which case its
    // manifest content is not evidence of anything current either.
    if (matter.directory_path && !matter.directory_path->empty()
        && persistence::probe_project_data(std::filesystem::path(*matter.directory_path))
            == persistence::ProjectDataPresence::AccessDenied)
      matter = unavailable_project(matter, ProjectDataState::Unavailable, "access_denied");
    return;
  }
  if (!matter.directory_path || matter.directory_path->empty()) {
    matter = unavailable_project(matter, ProjectDataState::Damaged, "missing_location");
    return;
  }
  const std::filesystem::path directory(*matter.directory_path);
  std::error_code status_error;
  const bool is_directory = std::filesystem::is_directory(directory, status_error);
  const bool access_denied = status_error == std::errc::permission_denied
      || status_error == std::errc::operation_not_permitted;
  if (access_denied) {
    // Access this Node no longer has is never corruption and never missing
    // data: nothing about the Project's own data is known, and nothing about
    // it may be changed or set up again.
    matter = unavailable_project(matter, ProjectDataState::Unavailable, "access_denied");
    return;
  }
  if (!is_directory) {
    matter = unavailable_project(matter, ProjectDataState::Unavailable, "folder_missing");
    return;
  }
  const persistence::ProjectDataPresence presence = persistence::probe_project_data(directory);
  if (presence == persistence::ProjectDataPresence::AccessDenied) {
    matter = unavailable_project(matter, ProjectDataState::Unavailable, "access_denied");
    return;
  }
  if (presence == persistence::ProjectDataPresence::Absent) {
    matter = unavailable_project(matter, ProjectDataState::Unavailable, "data_missing");
    return;
  }
  try {
    Matter loaded = persistence::load_project(directory).project;
    if (loaded.id != matter.id) {
      // The folder's .lm belongs to another Project: never silently listed
      // under this Project's identity.
      matter = unavailable_project(matter, ProjectDataState::Damaged, "identity_mismatch");
      return;
    }
    loaded.data_state = ProjectDataState::Ready;
    matter = std::move(loaded);
  } catch (const persistence::ProjectDataAccessDenied &) {
    matter = unavailable_project(matter, ProjectDataState::Unavailable, "access_denied");
  } catch (const persistence::ProjectDataRequiresNewerVersion &) {
    // Perfectly good data this build is simply too old to read. It is unavailable to
    // this Singularity, never damaged: the answer is a newer Singularity, never
    // repairing or starting over.
    matter = unavailable_project(matter, ProjectDataState::Unavailable, "requires_newer_version");
  } catch (const std::exception &) {
    // Corruption, an unsupported format version or a caught-mid-write
    // concurrent change: listed by its folder name with its state, never
    // replaced by an editable stand-in.
    matter = unavailable_project(matter, ProjectDataState::Damaged, "unreadable");
  }
}

std::vector<Matter> load_matters(const std::filesystem::path &workspace_path) {
  platform::trace::Span span("persist", "matters.load");
  std::vector<Matter> matters = read_legacy_manifest(workspace_path);
  span.add("projects", matters.size());
  std::vector<std::filesystem::path> listed;
  for (Matter &matter : matters) {
    if (matter.directory_path && !matter.directory_path->empty())
      listed.emplace_back(std::filesystem::path(*matter.directory_path));
    hydrate_listed_matter(matter);
  }
  persistence::keep_only_projects(listed);
  return matters;
}

std::optional<Matter> load_matter(const std::filesystem::path &workspace_path, const std::string &id) {
  platform::trace::Span span("persist", "matter.load");
  std::vector<Matter> listed = read_legacy_manifest(workspace_path);
  const auto found = std::find_if(listed.begin(), listed.end(), [&](const Matter &entry) { return entry.id == id; });
  if (found == listed.end()) return std::nullopt;
  Matter matter = std::move(*found);
  hydrate_listed_matter(matter);
  return matter;
}

std::vector<std::string> list_matter_ids(const std::filesystem::path &workspace_path) {
  std::vector<std::string> ids;
  for (const Matter &listed : read_legacy_manifest(workspace_path)) ids.push_back(listed.id);
  return ids;
}

// The refusals save_matters() applies to every Project it is given, for one.
static void require_savable(const Matter &matter) {
  // Refused before anything is written: a change to a Project whose data is
  // unavailable or damaged is never stored anywhere - not in .lm, which cannot
  // be read, and never in matters.json, where it would later be lost.
  if (matter.data_state == ProjectDataState::Unavailable || matter.data_state == ProjectDataState::Damaged) {
    // Pointing the Project at another location (relinking a moved folder) is
    // the one change allowed: its locator is all the workspace keeps.
    Matter candidate = matter;
    Matter unchanged = unavailable_project(matter, matter.data_state, matter.data_state_reason);
    candidate.name.clear();
    candidate.updated_at.clear();
    unchanged.name.clear();
    unchanged.updated_at.clear();
    if (matter_to_json(candidate) != matter_to_json(unchanged))
      throw ProjectDataError(matter.data_state, matter.data_state == ProjectDataState::Damaged
          ? "This Project's Singularity data is damaged, so this change was not saved. Repair or restore the Project's "
            ".lm data, then try again."
          : "This Project's folder or Singularity data is not available, so this change was not saved. Reconnect the "
            "folder, then try again.");
  }
}

static void require_reachable(const Matter &matter) {
  if ((matter.storage_cutover || matter.data_state == ProjectDataState::Ready)
      && matter.data_state != ProjectDataState::Unavailable && matter.data_state != ProjectDataState::Damaged
      && has_cutover_project_data(matter) == false)
    throw ProjectDataError(ProjectDataState::Unavailable,
        "This Project's folder or Singularity data is no longer available, so this change was not saved. Reconnect "
        "the folder, then try again.");
}

// Writes one Project where it belongs and returns what matters.json keeps for
// it: its locator once .lm is canonical, otherwise its whole legacy record.
static Matter persist_and_list(const Matter &matter) {
  if (matter.data_state == ProjectDataState::Unavailable || matter.data_state == ProjectDataState::Damaged)
    return locator_stub(matter);
  // A populated storage_baseline is proof that THIS in-memory object was
  // actually hydrated from .lm's real content (by load_matters()'s own
  // overlay above) before this save - every real load-modify-save cycle
  // has one. Its absence despite has_cutover_project_data() being true
  // means directory_path changed mid-transaction to a location that
  // already has ITS OWN .lm since this object was loaded (only
  // assign_folder does this) - writing this incompletely-known object
  // there now would silently overwrite real content with whatever
  // incomplete fields this caller happens to be carrying. Fall back to
  // matters.json for this one save instead; the next normal
  // load-modify-save cycle hydrates and reconciles it safely.
  if (has_cutover_project_data(matter) == false || matter.storage_baseline == nullptr) return matter;
  // .lm is canonical for this Matter's full content once cut over: persist
  // the caller's desired state there, never into matters.json, so a
  // workspace-wide save can never reintroduce a migrated snapshot into
  // legacy storage. Diffing against the caller's own captured baseline
  // (rather than a freshly re-loaded one) means a concurrent writer's .lm
  // changes are detected as a conflict instead of silently overwritten.
  Matter desired = matter;
  persistence::save_project(desired, platform::resolve_actor_identity());
  return locator_stub(matter);
}

void save_matter(const std::filesystem::path &workspace_path, const Matter &matter) {
  platform::trace::Span span("persist", "matter.save");
  require_savable(matter);
  require_reachable(matter);
  const Matter listed_as = persist_and_list(matter);
  // Whoever shows this Matter reads it again, instead of polling.
  platform::events::publish("matter.changed", nlohmann::json{{"projectId", matter.id}});
  std::vector<Matter> listed = read_legacy_manifest(workspace_path);
  const auto found = std::find_if(listed.begin(), listed.end(), [&](const Matter &entry) { return entry.id == matter.id; });
  if (found == listed.end()) {
    listed.push_back(listed_as);
  } else if (matter_manifest_record(*found) != matter_manifest_record(listed_as)) {
    *found = listed_as;
  } else {
    span.add("manifest", "unchanged");
    return;
  }
  write_legacy_manifest(workspace_path, listed);
}

void save_matters(const std::filesystem::path &workspace_path, const std::vector<Matter> &matters) {
  platform::trace::Span span("persist", "matters.save", {{"projects", matters.size()}});
  for (const Matter &matter : matters) require_savable(matter);
  for (const Matter &matter : matters) require_reachable(matter);
  std::vector<Matter> legacy_view;
  legacy_view.reserve(matters.size());
  for (const Matter &matter : matters) legacy_view.push_back(persist_and_list(matter));
  write_legacy_manifest(workspace_path, legacy_view);
  for (const Matter &matter : matters) platform::events::publish("matter.changed", nlohmann::json{{"projectId", matter.id}});
}

bool refresh_document_derivatives(const std::filesystem::path &workspace_path,
                                  const std::string &matter_id,
                                  MatterDocument &document) {
  const auto before = document_to_json(document);
  document.matter_id = matter_id;
  if (document.file_type == documents::DocumentFormat::unknown) {
    document.file_type = documents::detect_document_format(document.name);
  }
  const auto extractor_version = documents::local_extractor_version(document.file_type);
  const auto cache_version = extractor_version.empty() ? "unsupported/1" : extractor_version;
  const auto managed = managed_document_path(workspace_path, matter_id, document.stored_name);

  try {
    // A stored name that is no plain file name is no copy of ours: unreadable,
    // and nothing outside the store is opened for it.
    if (!managed) throw std::runtime_error("This document's recorded name is not a file in Singularity's store.");
    const auto &source_path = *managed;
    const documents::LocalDocumentExtractor extractor;
    const documents::DocumentCache cache(workspace_path, extractor);
    const auto prepared = cache.prepare(source_path, document.file_type, cache_version);
    document.size_bytes = prepared.size_bytes;
    document.last_modified = prepared.last_modified;
    document.fingerprint = prepared.fingerprint;
    document.extraction_status =
        to_document_extraction_state(prepared.extraction.status);
    document.extracted_character_count =
        static_cast<long long>(documents::utf8_character_count(prepared.extraction.text));
    document.page_count = prepared.extraction.structure.page_count
                              ? std::optional<long long>(static_cast<long long>(
                                    *prepared.extraction.structure.page_count))
                              : std::nullopt;
    document.extractor_version = prepared.extraction.extractor_version;
  } catch (const std::exception &) {
    // A missing or unreadable managed source must revoke AI readiness. The
    // old fingerprint is cleared so no caller can resolve stale derivatives.
    document.size_bytes = 0;
    document.last_modified.clear();
    document.fingerprint.clear();
    document.extraction_status = DocumentExtractionState::Unreadable;
    document.extracted_character_count = 0;
    document.page_count.reset();
    document.extractor_version = cache_version;
  }
  return before != document_to_json(document);
}

bool refresh_matter_documents(const std::filesystem::path &workspace_path,
                              Matter &matter) {
  bool changed = false;
  for (auto &document : matter.documents) {
		// Phase 08 correction item 4: refresh_document_derivatives() above
		// assumes the Managed copy path (documents_directory(workspace_path,
		// matter.id) / document.stored_name, which is empty for a Referenced
		// document). Calling it on a Referenced document would fail to open that
		// non-existent path, clear its real fingerprint and mark it Unreadable -
		// silently corrupting state Document/DocumentRegistry.hpp's own pipeline
		// (process_pending_documents()) already correctly computed from the real
		// Project folder. Referenced documents are refreshed exclusively by that
		// pipeline; this legacy Managed-oriented function must never touch them.
		if (document.source_mode != DocumentSourceMode::Managed) continue;
    changed = refresh_document_derivatives(workspace_path, matter.id, document) || changed;
  }
  return changed;
}

std::vector<context::RetrievedContextChunk> retrieve_document_context(
    const std::filesystem::path &workspace_path, const Matter &matter,
    const std::string &current_request,
    const std::vector<std::string> &selected_document_ids,
    std::vector<UnavailableSourceDocument> *unavailable_source_documents_out,
    SelectedSourceDelivery delivery) {
  platform::trace::Span span("evidence", "retrieve_document_context",
      {{"projectDocuments", matter.documents.size()}, {"selectedDocuments", selected_document_ids.size()},
       {"queryBytes", current_request.size()}});
  context::ContextRetrievalRequest request;
  request.query = current_request;
  request.selected_document_ids = selected_document_ids;
  const documents::LocalDocumentExtractor extractor;
  const documents::DocumentCache cache(workspace_path, extractor);
	// Phase 08 correction item 4: once a Matter has a real Project folder (the new
	// Beta filesystem contract - Document/DocumentRegistry.hpp), Referenced documents
	// (real files under that folder) are the canonical Project document corpus. A
	// Managed document (Phase 03's explicit copy-in, living in
	// .singularity-workspace/documents/<matterId>/, not the real Project folder) is
	// legacy-compatibility material, not equivalent current Project source - excluded
	// here so it can never silently participate as a second, hidden canonical corpus
	// alongside the real one. A Matter with no real folder yet has no Referenced
	// documents at all, and keeps exactly its pre-Phase-08 behavior: Managed remains
	// fully readable, since it is this Matter's only real source material -
	// "historical/legacy compatibility remains readable where required" is preserved,
	// not broken, by this boundary.
	const bool hasRealProjectFolder = matter.directory_path.has_value() && matter.directory_path->empty() == false;
	const std::unordered_set<std::string> selectedIds(
		selected_document_ids.begin(), selected_document_ids.end());
	// A Referenced document whose file is gone keeps its Ready extraction and
	// fingerprint (so a returning file recovers), which means the derivative
	// cache would still serve its last text. It is not current Project source:
	// excluded here, Selected mode fails closed in ContextRetriever, and
	// Relevant mode simply no longer draws from it.
	const auto isCandidate = [&](const MatterDocument &document) {
		if (!selectedIds.empty() && selectedIds.count(document.id) == 0) return false;
		if (document_lifecycle_state(document) == DocumentLifecycleState::Unavailable) return false;
		return !hasRealProjectFolder || document.source_mode == DocumentSourceMode::Referenced;
	};
	const std::size_t documentCount = static_cast<std::size_t>(std::count_if(
		matter.documents.begin(), matter.documents.end(), isCandidate));
	routing::observability::emit({routing::observability::OperationEventKind::SourceReadStart,
		"Reading the resolved source set",
		{{"documentCount", std::to_string(documentCount)},
		 {"mode", selectedIds.empty() ? "relevant" : "selected"}}, std::nullopt});
	std::size_t processedCount = 0;
	const auto reportProcessed = [&] {
		++processedCount;
		routing::observability::emit({routing::observability::OperationEventKind::SourceReadProgress,
			"A source was read",
			{{"completed", std::to_string(processedCount)}, {"total", std::to_string(documentCount)}},
			std::nullopt});
	};
	const auto reportUnavailable = [&](const MatterDocument &document, const std::string &reason) {
		if (unavailable_source_documents_out != nullptr)
			unavailable_source_documents_out->push_back({document.id, document.name, reason});
	};
	const bool selectedMode = !selectedIds.empty();

	// CACHED EXTRACTION IS NEVER AUTHORITY FOR CURRENT SOURCE CONTENT. Nothing
	// becomes model evidence until the document's current physical bytes are
	// verified: selected sources before retrieval, relevance candidates only
	// once retrieval actually picks them (never a whole-Project rescan).
	std::unordered_set<std::string> verifiedIds;
	std::unordered_map<std::string, std::string> materializedFingerprints;
	const auto verifyAndMaterialize = [&](const MatterDocument &document)
			-> std::optional<context::RetrievalDocument> {
		const PhysicalSourceRead read = read_current_physical_source(workspace_path, matter, document, cache);
		verifiedIds.insert(document.id);
		if (read.state != PhysicalSourceState::Current) {
			const bool missing = read.state == PhysicalSourceState::Missing;
			routing::observability::emit({routing::observability::OperationEventKind::SourceReadProgress,
				missing ? "A source is no longer in the Project folder" : "A source could not be read",
				{{"documentId", document.id}, {"sourceState", missing ? "missing" : "unreadable"}}, std::nullopt});
			const std::string reason = missing ? std::string("This document is no longer in the Project folder.")
				: document.attention_detail.value_or("This document couldn't be read.");
			if (selectedMode)
				throw std::runtime_error("The selected document \"" + document.name + "\" is not available: " + reason);
			reportUnavailable(document, reason);
			return std::nullopt;
		}
		if (read.fingerprint != document.fingerprint)
			routing::observability::emit({routing::observability::OperationEventKind::SourceReadProgress,
				"A source changed since the last Project sync; its current content was read",
				{{"documentId", document.id}, {"sourceState", "changed_since_sync"}}, std::nullopt});
		materializedFingerprints[document.id] = read.fingerprint;
		return context::RetrievalDocument{document.id, document.name,
			documents::chunk_document(document.id, read.fingerprint, read.extraction), placed_name(document)};
	};

	// A document the Project's reading has not reached yet (new in the folder,
	// or saved again) is not a document that could not be read. The request
	// itself waits for that reading first (matter::wait_for_documents_read()
	// in MatterRequestExecution.cpp); one still unread after the wait is either
	// read here from its current bytes - a document the lawyer selected - or
	// named as still being read, while the request goes on with the rest.
	const auto isStillBeingRead = [](const MatterDocument &document) {
		const DocumentLifecycleState state = document_lifecycle_state(document);
		return document.source_mode == DocumentSourceMode::Referenced && document.available
			&& (state == DocumentLifecycleState::Processing || state == DocumentLifecycleState::Changed);
	};

  for (const auto &document : matter.documents) {
		if (!isCandidate(document)) continue;
    if (document.extraction_status != DocumentExtractionState::Ready) {
			if (isStillBeingRead(document)) {
				if (selectedMode) {
					if (auto current = verifyAndMaterialize(document)) request.documents.push_back(std::move(*current));
				} else {
					reportUnavailable(document, "This document was still being read when the request was sent.");
				}
				reportProcessed();
				continue;
			}
      // A genuine read/processing failure (Phase 27's source-problem
      // Error) - never the corpus-scoping exclusion below, which is
      // intentional scope, not a failure.
      reportUnavailable(document, document.attention_detail.value_or("This document couldn't be read."));
		reportProcessed();
      continue;
    }
		if (selectedMode) {
			if (auto current = verifyAndMaterialize(document)) request.documents.push_back(std::move(*current));
			reportProcessed();
			continue;
		}
    const auto extraction = cache.load(document.fingerprint,
                                       document.extractor_version);
    if (!extraction || !extraction->ai_ready()) {
			// No derivative for the registry's fingerprint (typically a change
			// the watcher recorded but the pipeline has not re-extracted yet):
			// the current physical file decides, not the missing cache entry.
			if (auto current = verifyAndMaterialize(document)) request.documents.push_back(std::move(*current));
			reportProcessed();
      continue;
    }
    request.documents.push_back(
        {document.id, document.name,
         documents::chunk_document(document.id, document.fingerprint,
                                   *extraction), placed_name(document)});
		materializedFingerprints[document.id] = document.fingerprint;
		reportProcessed();
  }
	// A request whose own delivery is bounded by the executing model's
	// capacity (prepare_task_request(): PromptAssembler admits what the
	// resolved capacity holds) receives every passage of an explicitly selected
	// document as a candidate - a fixed passage count must never stand in for
	// that capacity. The fixed window stays for automatic Relevant retrieval,
	// where it bounds what is sent to what the request actually points at, and
	// for callers that build a prompt with no capacity budget of their own.
	if (selectedMode && delivery == SelectedSourceDelivery::EveryPassage) {
		std::size_t selectedPassages = 0;
		for (const context::RetrievalDocument &document : request.documents) selectedPassages += document.chunks.size();
		request.maximum_chunks = std::max(request.maximum_chunks, selectedPassages);
	}
	// The same caller asking the whole Project: every passage the request
	// genuinely points at is a candidate, best first, and that caller's own
	// capacity-first admission decides how much is sent (ContextRetriever.hpp).
	// A fixed eight-passage window here is what made a data-room-wide question
	// arrive with one excerpt from eight of thirty documents.
	if ((!selectedMode && delivery == SelectedSourceDelivery::EveryPassage)
			|| delivery == SelectedSourceDelivery::RelevanceWithin) {
		request.maximum_chunks = std::max(request.maximum_chunks, context::kRelevanceCandidateCeiling);
		request.relevance_floor_share = context::kRelevanceFloorShare;
	}
	auto retrieved = context::ContextRetriever::retrieve(request);
	// Relevance candidates were ranked from cached derivatives; before any of
	// them becomes evidence, verify exactly the documents retrieval chose. A
	// missing or changed one is dropped/replaced and retrieval re-runs, so the
	// returned chunks always come from verified current bytes.
	for (bool reranked = true; reranked && !selectedMode;) {
		reranked = false;
		std::vector<std::string> unverified;
		for (const auto &chunk : retrieved)
			if (verifiedIds.count(chunk.document_id) == 0
					&& std::find(unverified.begin(), unverified.end(), chunk.document_id) == unverified.end())
				unverified.push_back(chunk.document_id);
		for (const std::string &documentId : unverified) {
			const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
				[&](const MatterDocument &entry) { return entry.id == documentId; });
			const auto slot = std::find_if(request.documents.begin(), request.documents.end(),
				[&](const context::RetrievalDocument &entry) { return entry.document_id == documentId; });
			if (document == matter.documents.end() || slot == request.documents.end()) continue;
			const std::string rankedFingerprint = materializedFingerprints[documentId];
			auto current = verifyAndMaterialize(*document);
			if (!current) {
				request.documents.erase(slot);
				reranked = true;
			} else if (materializedFingerprints[documentId] != rankedFingerprint) {
				*slot = std::move(*current);
				reranked = true;
			}
		}
		if (reranked) retrieved = context::ContextRetriever::retrieve(request);
	}
	routing::observability::emit({routing::observability::OperationEventKind::SourceReadEnd,
		"The resolved source set is ready",
		{{"documentCount", std::to_string(documentCount)}, {"chunkCount", std::to_string(retrieved.size())}},
		std::nullopt});
	return retrieved;
}

std::string build_document_context(
    const std::filesystem::path &workspace_path, const Matter &matter,
    const std::string &current_request,
    const std::vector<std::string> &selected_document_ids) {
  const auto chunks = retrieve_document_context(workspace_path, matter, current_request,
                                                selected_document_ids);
  return context::SourceMarkerRegistry(chunks).format_all(chunks);
}

} // namespace singularity::matters
