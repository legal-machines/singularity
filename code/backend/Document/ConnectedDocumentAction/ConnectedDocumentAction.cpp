#include "Document/ConnectedDocumentAction/ConnectedDocumentAction.hpp"

#include "Document/DocumentConversationIntent/DocumentConversationIntent.hpp"
#include "Document/DocumentPatchTool/DocumentPatchTool.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Document/DocumentStructuralReader/DocumentStructuralReader.hpp"
#include "Document/Types/DocumentLocator.hpp"
#include "ExternalResearch/ContextBudget/ContextBudget.hpp"
#include "ExternalResearch/ResearchIntent/ResearchIntent.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"
#include "Routing/MemoryDiagnostics/MemoryDiagnostics.hpp"
#include "Routing/Observability/OperationEvent.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace singularity::documents {
namespace {

using Json = nlohmann::json;

class ResearchCoverageError final : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Shared with connected_document_draft_model_tool_definition()'s own schema
// (its maxLength on the identical field) and upsert_connected_document_draft_
// item()'s own fail-closed enforcement below - a single real conservative
// ceiling per free-text field, never a schema-only promise a non-strict
// provider could silently exceed. See phase_aware_reserved_output_tokens()'s
// own Drafting/Correction derivation for why this needed a real number at all.
// Sized for one Body table cell's worth of professional prose - a risk-
// register finding, not a full memo - generous relative to every real value
// this file's own golden fixture (ChatExecution.test.cpp's golden_structured_
// result()) ever produces (its longest column-9 rationale is ~155 characters)
// while staying tight enough for phase_aware_reserved_output_tokens() to
// derive a real Drafting/Correction reservation meaningfully smaller than an
// arbitrary flat guess on a genuinely small local context window.
constexpr std::size_t kMaxDraftFieldValueLength = 1500;
constexpr std::size_t kMaxDraftSummaryLength = 600;
constexpr std::size_t kMaxDraftMethodologyNoteLength = 400;
// A CONSERVATIVE RESERVATION ESTIMATE (not a proven bound) for the small,
// fixed per-field JSON envelope buildItemJson() emits around one real cell's
// own text (handle/label/editable/structuralRole/placeholder/gridSpan keys
// and braces/quotes/commas) - used only by prepare_connected_document_
// action()'s own compact_context_mode probe to approximate "if every real
// row of this document were shown in full, would it fit," never as a claim
// about any one compiled turn's exact byte count.
constexpr std::size_t kPerFieldStructuralOverheadTokens = 20;

const char *field_disposition_string(DocumentFieldDisposition disposition) {
	return disposition == DocumentFieldDisposition::Write ? "write" : "leave_unchanged";
}

std::optional<DocumentFieldDisposition> field_disposition_from_string(const std::string &value) {
	if (value == "write") return DocumentFieldDisposition::Write;
	if (value == "leave_unchanged") return DocumentFieldDisposition::LeaveUnchanged;
	return std::nullopt;
}

std::string trim_ascii_space(const std::string &value) {
	std::size_t first = 0;
	while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
	std::size_t last = value.size();
	while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
	return value.substr(first, last - first);
}

// is_template_placeholder() is now the single canonical definition in
// Document/Types/DocumentSemantics.hpp (DirectDocumentWork.cpp needed the
// exact same rule at a third call site - see that header's own comment for
// the full rationale/history) - normal unqualified lookup here still finds
// it via the enclosing singularity::documents namespace.

// utf8_character_count() is likewise now the single canonical definition in
// Document/Types/DocumentSemantics.hpp (MatterRepository.cpp and
// DirectDocumentWork.cpp need the exact same real-Unicode-character-count
// rule every draft field's own JSON-schema maxLength below relies on) -
// normal unqualified lookup here still finds it via the enclosing
// singularity::documents namespace.

std::string visible_field_label(
		const ConnectedDocumentActionPlan &plan, const std::string &fieldHandle) {
	const auto label = plan.field_labels.find(fieldHandle);
	if (label != plan.field_labels.end() && !label->second.empty()) return label->second;
	const auto locator = plan.field_handles.find(fieldHandle);
	return locator != plan.field_handles.end() ? locator->second.column_key.value_or("") : std::string();
}

bool field_is_placeholder(
		const ConnectedDocumentActionPlan &plan, const std::string &fieldHandle) {
	const auto known = plan.field_placeholder_states.find(fieldHandle);
	return known != plan.field_placeholder_states.end() && known->second;
}

const DocumentFieldDispositionBinding *field_disposition_binding(
		const ConnectedDocumentActionPlan &plan, const std::string &fieldHandle) {
	const auto found = std::find_if(plan.draft->field_dispositions.begin(),
		plan.draft->field_dispositions.end(), [&](const auto &binding) {
			return binding.field_handle == fieldHandle;
		});
	return found == plan.draft->field_dispositions.end() ? nullptr : &*found;
}

std::optional<std::string> selected_output_region(const ConnectedDocumentActionPlan &plan) {
	if (plan.draft->selected_output_region_handles.size() == 1)
		return plan.draft->selected_output_region_handles.front();
	// In-memory compatibility for existing one-field callers constructed
	// before the typed region contract. This is never used for a multi-field
	// region: there, absence of explicit dispositions must remain a hard stop.
	if (plan.draft->required_outputs.size() != 1) return std::nullopt;
	const RequiredOutputObligation &output = plan.draft->required_outputs.front();
	const auto editable = plan.item_editable_field_handles.find(output.item_handle);
	if (editable == plan.item_editable_field_handles.end() || editable->second.size() != 1
			|| editable->second.front() != output.field_handle) return std::nullopt;
	return output.item_handle;
}

bool is_selected_output_region(
		const ConnectedDocumentActionPlan &plan, const std::string &itemHandle) {
	if (!plan.draft->selected_output_region_handles.empty())
		return std::find(plan.draft->selected_output_region_handles.begin(),
			plan.draft->selected_output_region_handles.end(), itemHandle)
			!= plan.draft->selected_output_region_handles.end();
	return std::any_of(plan.draft->required_outputs.begin(), plan.draft->required_outputs.end(),
		[&](const auto &output) { return output.item_handle == itemHandle; });
}

bool is_skipped_output_region(
		const ConnectedDocumentActionPlan &plan, const std::string &itemHandle) {
	return std::find(plan.draft->skipped_output_region_handles.begin(),
		plan.draft->skipped_output_region_handles.end(), itemHandle)
		!= plan.draft->skipped_output_region_handles.end();
}

// THE runtime's own single real answer to "could an output agenda call
// finalize the agenda right now" (document.outputs.define's own final=true
// requires at least one real selected region, across this and any earlier
// call - an all-skipped or wholly-untouched agenda has nothing to draft).
// define_connected_document_output_agenda()'s own empty_output_agenda
// rejection and compile_connected_document_operation_state()'s own
// output_agenda_can_finalize (which TurnContract.hpp's compile_turn_contract()
// reads to decide whether the model may still be offered a bare final=true
// close-out with nothing new selected this call) must both consume this one
// predicate, never re-derive it independently - they were previously two
// separate copies of the identical underlying boolean, agreeing only because
// nobody had touched one without the other yet.
bool output_agenda_finalizable(const ConnectedDocumentActionPlan &plan) {
	return !plan.draft->selected_output_region_handles.empty();
}

// A region no longer belongs in the current candidate window once the model
// has decided about it either way (selected as a real required output, or
// explicitly skipped as not part of this request) - see DocumentActionDraft
// ::skipped_output_region_handles' own comment on why these are two
// genuinely different facts, never one conflated "already handled" flag.
bool is_output_region_considered(
		const ConnectedDocumentActionPlan &plan, const std::string &itemHandle) {
	return is_selected_output_region(plan, itemHandle) || is_skipped_output_region(plan, itemHandle);
}

std::optional<DocumentFieldDisposition> effective_field_disposition(
		const ConnectedDocumentActionPlan &plan, const std::string &fieldHandle) {
	if (const auto *binding = field_disposition_binding(plan, fieldHandle)) return binding->disposition;
	const auto region = selected_output_region(plan);
	if (!region || plan.draft->required_outputs.size() != 1) return std::nullopt;
	return plan.draft->required_outputs.front().field_handle == fieldHandle
		? std::optional<DocumentFieldDisposition>(DocumentFieldDisposition::Write) : std::nullopt;
}

bool line_reproduces_structural_label(const std::string &line, const std::string &label) {
	std::string trimmed = trim_ascii_space(line);
	const std::string structuralLabel = trim_ascii_space(label);
	// Model prose commonly renders an embedded heading with lightweight
	// Markdown. Ignore only presentation punctuation before comparing the
	// exact runtime-known label; no vocabulary or language-specific rule is
	// involved.
	while (!trimmed.empty() && (trimmed.front() == '#' || trimmed.front() == '*'))
		trimmed = trim_ascii_space(trimmed.substr(1));
	if (structuralLabel.empty() || trimmed.size() < structuralLabel.size()
			|| trimmed.compare(0, structuralLabel.size(), structuralLabel) != 0) return false;
	if (trimmed.size() == structuralLabel.size()) return true;
	const unsigned char next = static_cast<unsigned char>(trimmed[structuralLabel.size()]);
	return std::isspace(next) || next == ':' || next == '-';
}

bool proposed_value_reproduces_region_label(const ConnectedDocumentActionPlan &plan,
		const std::string &itemHandle, const std::string &value) {
	const auto fields = plan.item_field_handles.find(itemHandle);
	if (fields == plan.item_field_handles.end()) return false;
	std::size_t start = 0;
	while (start <= value.size()) {
		const std::size_t end = value.find('\n', start);
		const std::string line = value.substr(start,
			end == std::string::npos ? std::string::npos : end - start);
		for (const std::string &fieldHandle : fields->second)
			if (line_reproduces_structural_label(line, visible_field_label(plan, fieldHandle))) return true;
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return false;
}

// A real lawyer's own prompt names a Project document by its descriptive
// title, never its file extension ("the '… - …
// document", never "...…docx") - a live design-partner acceptance run
// proved the exact-filename match below used to require silently fell back
// to the legacy chat-only path for this reason alone, on every real prompt
// that named its target document the way people actually write. Comparing
// against the name with its extension removed as well costs nothing when a
// prompt does happen to include the extension (that still matches via
// `instruction.find(document.name)` first) and never introduces a new
// silent ambiguity: two Project documents that collide once their
// extensions are stripped already fail closed via the existing "Apply one
// atomic Project-document action at a time" check below, exactly as two
// full-name collisions always would have.
std::string without_extension(const std::string &name) {
	const std::size_t dot = name.find_last_of('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

std::size_t last_cue(const std::string &text, std::initializer_list<const char *> cues) {
	std::size_t result = std::string::npos;
	for (const char *cue : cues) {
		const std::size_t position = text.rfind(cue);
		if (position != std::string::npos && (result == std::string::npos || position > result)) result = position;
	}
	return result;
}

bool is_methodology_reference(const std::string &instruction, const std::string &name) {
	std::size_t position = instruction.find(name);
	if (position == std::string::npos) {
		const std::string stem = without_extension(name);
		position = stem.empty() || stem == name ? std::string::npos : instruction.find(stem);
	}
	if (position == std::string::npos) return false;
	const std::size_t start = position > 180 ? position - 180 : 0;
	std::string prefix = instruction.substr(start, position - start);
	std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char value) {
		return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : static_cast<char>(value);
	});
	const std::size_t methodologyCue = last_cue(prefix, {"\320\277\321\200\320\260\320\262\320\270\320\273", "\320\274\320\265\321\202\320\276\320\264\320\276\320\273\320\276\320\263", "rules", "methodolog"});
	const std::size_t mutationCue = last_cue(prefix,
		{"\320\262\320\275\320\265\321\201\320\270 \320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217", "\320\276\320\261\320\275\320\276\320\262\320\270", "\320\264\320\276\320\261\320\260\320\262", "\320\270\321\201\320\277\321\200\320\260\320\262", "\320\277\320\265\321\200\320\265\320\277\320\270\321\210", "\320\275\320\260\320\277\320\270\321\210",
			"update", "change", "add", "edit", "revise", "replace", "write"});
	// A sentence may mention research "rules" before naming the actual file
	// to update ("rules ..., and update Status in Worklist.csv"). The closest
	// role cue owns the file reference; otherwise that target was incorrectly
	// treated as methodology and the model never received its action schema.
	return methodologyCue != std::string::npos
		&& (mutationCue == std::string::npos || methodologyCue > mutationCue);
}

std::filesystem::path document_path(const matters::Matter &project, const matters::MatterDocument &document) {
	if (document.source_mode != matters::DocumentSourceMode::Referenced || !project.directory_path
			|| !registry::is_relative_path_safe(*project.directory_path, document.relative_path))
		throw std::runtime_error("A document action requires a real file inside the active Project.");
	return std::filesystem::path(*project.directory_path) / document.relative_path;
}

// The structural model for a document_id already proven (by whichever
// caller obtained it - a pre-resolved DocumentLocator, or a targetDocument
// name already checked by is_known_candidate_document_name()) to be this
// plan's default target or one of its other_candidate_documents.
const DocumentSemanticModel &model_for_document(
		const ConnectedDocumentActionPlan &plan, const std::string &documentId) {
	if (documentId == plan.target_document_id) return plan.target_model;
	for (const DocumentActionCandidate &candidate : plan.other_candidate_documents)
		if (candidate.document_id == documentId) return candidate.model;
	throw std::logic_error("model_for_document() called for an unresolved document_id.");
}

// Searches exactly one candidate document's own structural model for one
// table_id - never across candidates: table_id is only ever unique WITHIN
// one document (every CSV's sole table is literally "csv"; a DOCX's Nth
// table is "table-<N>" independent of which file it is in), so two
// different candidate Project documents legitimately sharing the same
// table_id string must never be compared against each other. Which
// document a proposal's locator resolves against is decided once, from its
// action's own targetDocument (see resolve_document_action_proposal()), not
// re-guessed per locator. Still fails closed (ambiguous) on the narrower,
// genuine case of one document's own model somehow repeating a table_id.
const DocumentTable &resolve_table(const DocumentSemanticModel &model, const std::string &tableId) {
	const DocumentTable *result = nullptr;
	for (const DocumentTable &table : model.tables) {
		if (table.table_id != tableId) continue;
		if (result != nullptr) throw std::runtime_error("A proposed table reference is ambiguous.");
		result = &table;
	}
	if (result == nullptr) throw std::runtime_error("A proposed table reference does not resolve.");
	return *result;
}

// Whether `name` names any document this operation may legitimately target -
// the plan's own default or one of its other_candidate_documents. Replaces a
// strict equality check against one pre-selected name: which of several
// named Project documents the model actually means to change is its own
// judgment (CORE PRODUCT LAW), proven here only against real candidates Singularity
// itself resolved and structurally read - never a name the model
// invented.
// Names are unique within a plan (a shared file name is shown as its Project-
// relative path); a name that still matches two candidates never resolves.
bool is_known_candidate_document_name(const ConnectedDocumentActionPlan &plan, const std::string &name) {
	const auto matches = static_cast<std::size_t>(name == plan.target_document_name)
		+ static_cast<std::size_t>(std::count_if(plan.other_candidate_documents.begin(), plan.other_candidate_documents.end(),
			[&](const DocumentActionCandidate &candidate) { return candidate.document_name == name; }));
	return matches == 1;
}

// The real document_id behind a name already proven known by
// is_known_candidate_document_name() above - never called otherwise.
std::string candidate_document_id_for_name(const ConnectedDocumentActionPlan &plan, const std::string &name) {
	if (name == plan.target_document_name) return plan.target_document_id;
	for (const DocumentActionCandidate &candidate : plan.other_candidate_documents)
		if (candidate.document_name == name) return candidate.document_id;
	throw std::logic_error("candidate_document_id_for_name() called for an unproven document name.");
}

// The inverse of candidate_document_id_for_name(): the display name for a
// document_id resolve_cell_locator() already proved belongs to this plan's
// default target or one of its other_candidate_documents.
std::string candidate_document_name_for_id(const ConnectedDocumentActionPlan &plan, const std::string &documentId) {
	if (documentId == plan.target_document_id) return plan.target_document_name;
	for (const DocumentActionCandidate &candidate : plan.other_candidate_documents)
		if (candidate.document_id == documentId) return candidate.document_name;
	throw std::logic_error("candidate_document_name_for_id() called for an unresolved document_id.");
}

// Backend-issued document handles: "D1" is the plan's default target, then
// "D2", "D3"... its other candidates in order. Provider-facing tool schemas and
// tool arguments name documents only by handle. A document's name appears only
// in the request data beside its handle, where the Papinian boundary protects
// it, so no Project document name ever sits in a schema that boundary does not
// transform.
std::string candidate_document_handle_for_id(const ConnectedDocumentActionPlan &plan, const std::string &documentId) {
	if (documentId == plan.target_document_id) return "D1";
	for (std::size_t index = 0; index < plan.other_candidate_documents.size(); ++index)
		if (plan.other_candidate_documents[index].document_id == documentId) return "D" + std::to_string(index + 2);
	throw std::logic_error("candidate_document_handle_for_id() called for an unresolved document_id.");
}

std::optional<std::string> candidate_document_id_for_handle(const ConnectedDocumentActionPlan &plan,
		const std::string &handle) {
	if (handle.size() < 2 || handle.front() != 'D' || handle[1] == '0') return std::nullopt;
	std::size_t number = 0;
	for (std::size_t position = 1; position < handle.size(); ++position) {
		if (std::isdigit(static_cast<unsigned char>(handle[position])) == 0) return std::nullopt;
		number = number * 10 + static_cast<std::size_t>(handle[position] - '0');
		if (number > plan.other_candidate_documents.size() + 1) return std::nullopt;
	}
	if (number == 1) return plan.target_document_id;
	return plan.other_candidate_documents[number - 2].document_id;
}

struct CandidateVersionGuard {
	int version = 0;
	std::string fingerprint;
};

// The expected-version/fingerprint mutation guard for a document_id
// resolve_document_action_proposal() already proved every operation in this
// one atomic action shares.
CandidateVersionGuard candidate_version_guard_for_id(
		const ConnectedDocumentActionPlan &plan, const std::string &documentId) {
	if (documentId == plan.target_document_id) return {plan.target_version, plan.target_fingerprint};
	for (const DocumentActionCandidate &candidate : plan.other_candidate_documents)
		if (candidate.document_id == documentId) return {candidate.version, candidate.fingerprint};
	throw std::logic_error("candidate_version_guard_for_id() called for an unresolved document_id.");
}

Json model_cell_locator_json(const DocumentLocator &locator) {
	if (!locator.table_id || !locator.row_key || !locator.column_key)
		throw std::runtime_error("A draft field does not resolve to an editable document cell.");
	return {{"tableId", *locator.table_id}, {"rowKey", *locator.row_key},
		{"columnKey", *locator.column_key}};
}

// `documentId` is never guessed from the locator itself (a bare tableId such
// as a CSV's universal "csv", or a DOCX's positional "table-0", is only ever
// unique within one document) - every caller already knows which candidate
// document this locator belongs to, either because it holds a pre-resolved
// DocumentLocator (whose own document_id it read this from) or because it
// already resolved the action's own targetDocument name (see
// resolve_document_action_proposal()).
DocumentLocator resolve_cell_locator(const ConnectedDocumentActionPlan &plan, const std::string &documentId,
		const Json &value, const DocumentTableCell **cell = nullptr) {
	if (!value.is_object() || value.size() != 3 || !value.contains("tableId")
			|| !value.contains("rowKey") || !value.contains("columnKey")
			|| !value.at("tableId").is_string() || !value.at("rowKey").is_string()
			|| !value.at("columnKey").is_string())
		throw std::runtime_error("A document action locator is malformed.");
	const std::string tableId = value.at("tableId").get<std::string>();
	const std::string rowKey = value.at("rowKey").get<std::string>();
	const std::string columnKey = value.at("columnKey").get<std::string>();
	const DocumentTable &table = resolve_table(model_for_document(plan, documentId), tableId);
	if (std::count(table.column_headers.begin(), table.column_headers.end(), columnKey) != 1)
		throw std::runtime_error("A proposed column reference does not resolve uniquely.");
	const DocumentTableRow *row = table.find_row(rowKey);
	if (row == nullptr) throw std::runtime_error("A proposed row reference does not resolve uniquely.");
	const DocumentTableCell *resolvedCell = row->cell_for_column(columnKey);
	if (resolvedCell == nullptr) throw std::runtime_error("A proposed cell reference does not resolve.");
	if (cell != nullptr) *cell = resolvedCell;
	DocumentLocator locator;
	locator.document_id = documentId;
	locator.table_id = tableId;
	locator.row_key = rowKey;
	locator.column_key = columnKey;
	return locator;
}

const research::ExternalEvidenceReference &resolve_external_evidence(const std::string &id,
		const std::vector<research::ExternalEvidenceReference> &available) {
	const auto found = std::find_if(available.begin(), available.end(),
		[&](const auto &entry) { return entry.evidence_id == id; });
	if (found == available.end())
		throw std::runtime_error("A document action cited external evidence not obtained by this operation.");
	return *found;
}

DocumentMethodologyEvidence resolve_methodology_evidence(
		const ConnectedDocumentActionPlan &plan, const Json &value) {
	if (!plan.methodology_document_id || !plan.methodology_fingerprint || !plan.methodology_model)
		throw std::runtime_error("The model proposed methodology evidence the user did not delegate.");
	if (!value.is_object() || value.size() != 2 || !value.contains("slideId")
			|| !value.at("slideId").is_string() || !value.contains("note") || !value.at("note").is_string())
		throw std::runtime_error("A methodology evidence reference is malformed.");
	const std::string slideId = value.at("slideId").get<std::string>();
	const auto slide = std::find_if(plan.methodology_model->slides.begin(), plan.methodology_model->slides.end(),
		[&](const DocumentSlide &candidate) { return candidate.slide_id == slideId; });
	if (slide == plan.methodology_model->slides.end())
		throw std::runtime_error("A methodology evidence reference does not resolve structurally.");
	DocumentLocator locator;
	locator.document_id = *plan.methodology_document_id;
	locator.slide_id = slideId;
	return {*plan.methodology_document_id, locator, *plan.methodology_fingerprint,
		value.at("note").get<std::string>()};
}

Json parse_document_action_result(const std::string &modelResult, bool &normalizedFence) {
	Json direct = Json::parse(modelResult, nullptr, false);
	if (!direct.is_discarded()) return direct;

	// Some OpenAI-compatible models return the one requested JSON object in
	// an explicit JSON code fence after a prose lead-in. Accept only that
	// narrow, unambiguous transport shape: one json fence, one closing fence,
	// and no competing object outside it. Semantic/action validation below is
	// unchanged and still resolves every field against trusted backend truth.
	const std::string opening = "```json";
	const std::size_t open = modelResult.find(opening);
	if (open == std::string::npos || modelResult.find(opening, open + opening.size()) != std::string::npos)
		return Json::value_t::discarded;
	const std::size_t content = open + opening.size();
	const std::size_t close = modelResult.find("```", content);
	if (close == std::string::npos || modelResult.find("```", close + 3) != std::string::npos)
		return Json::value_t::discarded;
	const std::string prefix = modelResult.substr(0, open);
	const std::string suffix = modelResult.substr(close + 3);
	if (prefix.find('{') != std::string::npos || prefix.find('}') != std::string::npos
			|| suffix.find('{') != std::string::npos || suffix.find('}') != std::string::npos)
		return Json::value_t::discarded;
	Json fenced = Json::parse(modelResult.substr(content, close - content), nullptr, false);
	if (!fenced.is_discarded()) normalizedFence = true;
	return fenced;
}

struct ResolvedDocumentActionProposal {
	std::string response_text;
	Json patch_operations = Json::array();
	std::vector<DocumentActionRationale> rationales;
	std::vector<research::ExternalEvidenceReference> referenced_external_evidence;
	bool normalized_fence = false;
};

bool contains_model_authored_citation(const std::string &text) {
	std::string normalized = text;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
		return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
	});
	return normalized.find("[source:") != std::string::npos
		|| normalized.find("[methodology:") != std::string::npos
		|| normalized.find("[external:") != std::string::npos
		|| normalized.find("ext_") != std::string::npos
		|| normalized.find("http://") != std::string::npos
		|| normalized.find("https://") != std::string::npos
		|| normalized.find("www.") != std::string::npos;
}

std::string join(const std::vector<std::string> &values, const std::string &separator) {
	std::ostringstream stream;
	for (std::size_t index = 0; index < values.size(); ++index) {
		if (index != 0) stream << separator;
		stream << values[index];
	}
	return stream.str();
}

std::string date_part(const std::string &value) {
	return value.size() >= 10 ? value.substr(0, 10) : value;
}

// A verified, human-readable rendering of ONE canonical retrieved source -
// title/authority, useful metadata, and its real URL. Never an internal
// locator, never an opaque handle - the model itself is never trusted to
// author this (see contains_model_authored_citation() above); Singularity always
// derives it from the canonical ExternalEvidenceReference the model merely
// selected by S-handle.
std::string human_external_citation(const research::ExternalEvidenceReference &evidence) {
	std::string authority;
	if (evidence.court) authority = *evidence.court;
	else if (evidence.title) authority = *evidence.title;
	else if (evidence.domain) authority = *evidence.domain;
	else authority = "External source";
	if (evidence.title && *evidence.title != authority) authority += ", " + *evidence.title;
	std::vector<std::string> metadata;
	std::string sourceClass = research::to_string(evidence.source_class);
	std::replace(sourceClass.begin(), sourceClass.end(), '_', ' ');
	metadata.push_back(std::move(sourceClass));
	if (evidence.jurisdiction) metadata.push_back(*evidence.jurisdiction);
	if (evidence.case_number) metadata.push_back("case " + *evidence.case_number);
	if (evidence.decision_date) metadata.push_back("decision " + *evidence.decision_date);
	else if (evidence.publication_date) metadata.push_back("published " + *evidence.publication_date);
	if (!evidence.retrieved_at.empty()) metadata.push_back("retrieved " + date_part(evidence.retrieved_at));
	return authority + " (" + join(metadata, "; ") + "): " + evidence.url;
}

// Deliberately narrow: this is the ONLY backend rendering that may be
// concatenated onto a model-authored professional value, and only for the one
// operation per rationale explicitly marked renderEvidence=true (an operation
// whose target artifact field is itself intended to hold rationale/sources/
// citations - see compile_connected_document_turn_data()'s own instruction).
// It renders ONLY verified external citations from canonical retrieved
// evidence - never project/methodology locators (those stay exclusively in
// DocumentActionEvidence.rationales, never in document text). Produces an
// empty string (no-op) when there is nothing verified to cite - an
// unsupported/not-established/not-applicable rationale never gets a
// fabricated citation.
std::string render_verified_external_citations(const DocumentActionRationale &rationale,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence) {
	if (rationale.external_support_status != ExternalSupportStatus::Supported) return {};
	std::vector<std::string> citations;
	for (const auto &id : rationale.external_evidence_ids)
		citations.push_back(human_external_citation(resolve_external_evidence(id, availableExternalEvidence)));
	if (citations.empty()) return {};
	return "\n\n" + join(citations, "\n");
}

struct NumericToken {
	std::size_t start = 0;
	std::size_t end = 0;
	double value = 0.0;
	std::string currency;
};

std::vector<NumericToken> numeric_tokens(const std::string &text) {
	static const std::regex pattern(
		"(RUB|USD|EUR|GBP|₽|\\$|€|£)?\\s*([0-9]+(?:[\\.,][0-9]+)?)\\s*(billion|million|thousand|bln|mln|bn|\320\274\320\273\321\200\320\264|\320\274\320\273\320\275|\321\202\321\213\321\201|m|k)?",
		std::regex_constants::icase);
	std::vector<NumericToken> result;
	for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
		std::string number = (*it)[2].str();
		std::replace(number.begin(), number.end(), ',', '.');
		double value = 0.0;
		try { value = std::stod(number); } catch (...) { continue; }
		std::string scale = (*it)[3].str();
		std::transform(scale.begin(), scale.end(), scale.begin(), [](unsigned char c) {
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
		});
		if (scale == "billion" || scale == "bln" || scale == "bn" || scale == "\320\274\320\273\321\200\320\264") value *= 1'000'000'000.0;
		else if (scale == "million" || scale == "mln" || scale == "m" || scale == "\320\274\320\273\320\275") value *= 1'000'000.0;
		else if (scale == "thousand" || scale == "k" || scale == "\321\202\321\213\321\201") value *= 1'000.0;
		std::string currency = (*it)[1].str();
		std::transform(currency.begin(), currency.end(), currency.begin(), [](unsigned char c) {
			return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
		});
		result.push_back({static_cast<std::size_t>(it->position()),
			static_cast<std::size_t>(it->position() + it->length()), value, currency});
	}
	return result;
}

bool same_number(double left, double right) {
	return std::fabs(left - right) <= std::max(1.0, std::max(std::fabs(left), std::fabs(right))) * 1e-9;
}

bool number_is_supplied(const NumericToken &value, const std::vector<NumericToken> &trusted) {
	return std::any_of(trusted.begin(), trusted.end(), [&](const auto &candidate) {
		return same_number(value.value, candidate.value)
			&& (value.currency.empty() || candidate.currency.empty() || value.currency == candidate.currency);
	});
}

std::string trusted_numeric_text(const ConnectedDocumentActionPlan &plan,
		const DocumentActionRationale &rationale) {
	std::string text;
	for (const auto &locator : rationale.project_evidence) {
		if (!locator.table_id || !locator.row_key) continue;
		const DocumentTable &table = resolve_table(model_for_document(plan, locator.document_id), *locator.table_id);
		const auto *row = table.find_row(*locator.row_key);
		if (row != nullptr) for (const auto &cell : row->cells) text += " " + cell.text;
	}
	if (plan.methodology_model) for (const auto &entry : rationale.methodology_evidence) {
		if (!entry.locator.slide_id) continue;
		const auto slide = std::find_if(plan.methodology_model->slides.begin(), plan.methodology_model->slides.end(),
			[&](const auto &candidate) { return candidate.slide_id == *entry.locator.slide_id; });
		if (slide == plan.methodology_model->slides.end()) continue;
		for (const auto &block : slide->text_blocks) text += " " + block.text;
		for (const auto &table : slide->tables) for (const auto &row : table.rows)
			for (const auto &cell : row.cells) text += " " + cell.text;
	}
	return text;
}

struct NumericComparisonIssue {
	double left = 0.0;
	double right = 0.0;
	std::string left_currency;
	std::string right_currency;
	std::string claimed_relation;
	std::string verified_relation;
};

std::optional<NumericComparisonIssue> impossible_numeric_comparison(const std::string &statement,
		const std::vector<NumericToken> &trusted) {
	enum class Relation { Greater, GreaterOrEqual, Less, LessOrEqual };
	struct Phrase { const char *text; Relation relation; bool symbol; };
	static const std::vector<Phrase> phrases{
		{"greater than", Relation::Greater, false}, {"exceeds", Relation::Greater, false},
		{"exceed", Relation::Greater, false}, {"above", Relation::Greater, false},
		{"\320\277\321\200\320\265\320\262\321\213\321\210\320\260\320\265\321\202", Relation::Greater, false}, {"\320\277\321\200\320\265\320\262\321\213\321\210\320\260\321\216\321\202", Relation::Greater, false},
		{"\320\261\320\276\320\273\321\214\321\210\320\265", Relation::Greater, false}, {"\320\262\321\213\321\210\320\265", Relation::Greater, false},
		{"less than", Relation::Less, false}, {"below", Relation::Less, false},
		{"under", Relation::Less, false}, {"\320\274\320\265\320\275\321\214\321\210\320\265", Relation::Less, false},
		{"\320\275\320\270\320\266\320\265", Relation::Less, false}, {">", Relation::Greater, true}, {"<", Relation::Less, true},
	};
	std::string normalized = statement;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
		return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
	});
	const auto values = numeric_tokens(statement);
	for (const auto &phrase : phrases) {
		std::size_t position = 0;
		while ((position = normalized.find(phrase.text, position)) != std::string::npos) {
			const std::string preceding = normalized.substr(position > 16 ? position - 16 : 0,
				std::min<std::size_t>(16, position));
			auto endsWith = [&](const std::string &suffix) {
				return preceding.size() >= suffix.size()
					&& preceding.compare(preceding.size() - suffix.size(), suffix.size(), suffix) == 0;
			};
			if (!phrase.symbol && (endsWith("not ") || endsWith("\320\275\320\265 ") || endsWith("\320\275\320\265  "))) {
				position += std::string(phrase.text).size();
				continue;
			}
			Relation effectiveRelation = phrase.relation;
			const std::size_t phraseEnd = position + std::string(phrase.text).size();
			if (phrase.symbol && phraseEnd < normalized.size() && normalized[phraseEnd] == '=')
				effectiveRelation = phrase.relation == Relation::Greater
					? Relation::GreaterOrEqual : Relation::LessOrEqual;
			const NumericToken *left = nullptr;
			const NumericToken *right = nullptr;
			for (const auto &token : values) {
				if (token.end <= position) left = &token;
				if (token.start >= phraseEnd) { right = &token; break; }
			}
			if (left != nullptr && right != nullptr
					&& (!phrase.symbol || position - left->end <= 8)
					&& position - left->end <= 120 && right->start - position <= 120
					&& statement.substr(left->end, right->start - left->end).find_first_of(".!?;\n") == std::string::npos
					&& (left->currency.empty() || right->currency.empty() || left->currency == right->currency)
					&& number_is_supplied(*left, trusted) && number_is_supplied(*right, trusted)) {
				const bool trueComparison = effectiveRelation == Relation::Greater ? left->value > right->value
					: effectiveRelation == Relation::GreaterOrEqual ? left->value >= right->value
					: effectiveRelation == Relation::Less ? left->value < right->value
					: left->value <= right->value;
				if (!trueComparison) {
					const std::string claimedRelation = effectiveRelation == Relation::Greater ? ">"
						: effectiveRelation == Relation::GreaterOrEqual ? ">="
						: effectiveRelation == Relation::Less ? "<" : "<=";
					const std::string verifiedRelation = same_number(left->value, right->value) ? "="
						: left->value > right->value ? ">" : "<";
					return NumericComparisonIssue{left->value, right->value, left->currency, right->currency,
						claimedRelation, verifiedRelation};
				}
			}
			position += std::string(phrase.text).size();
		}
	}
	return std::nullopt;
}

std::string numeric_comparison_message(const NumericComparisonIssue &issue,
		const std::string &rationaleId, const Json &locator) {
	const auto operand = [](double value, const std::string &currency) {
		return Json{{"value", value}, {"currency", currency.empty() ? Json(nullptr) : Json(currency)}};
	};
	const Json correctionTarget{
		{"rationaleId", rationaleId},
		{"locator", locator},
		{"normalizedOperands", Json::array({operand(issue.left, issue.left_currency),
			operand(issue.right, issue.right_currency)})},
		{"claimedRelation", issue.claimed_relation},
		{"verifiedRelation", issue.verified_relation},
	};
	return "A proposed rationale contains a deterministically false numeric comparison. Correction target: "
		+ correctionTarget.dump()
		+ ". Correct or remove only this exact comparison while preserving the other requested rows.";
}

ResolvedDocumentActionProposal resolve_document_action_proposal(
		const ConnectedDocumentActionPlan &plan, const std::string &modelResult,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence) {
	ResolvedDocumentActionProposal resolved;
	const Json root = parse_document_action_result(modelResult, resolved.normalized_fence);
	if (root.is_discarded() || !root.is_object() || root.size() != 2 || !root.contains("answer")
			|| !root.at("answer").is_string() || !root.contains("documentActions")
			|| !root.at("documentActions").is_array() || root.at("documentActions").size() != 1)
		throw std::runtime_error("The model did not return the required structured document action proposal.");
	resolved.response_text = root.at("answer").get<std::string>();
	if (contains_model_authored_citation(resolved.response_text))
		throw std::runtime_error("The model put citation tokens or source URLs in the answer instead of returning typed evidence references.");
	const Json &action = root.at("documentActions").front();
	if (!action.is_object() || action.size() != 3
			|| !is_known_candidate_document_name(plan, action.value("targetDocument", std::string()))
			|| !action.contains("rationales") || !action.at("rationales").is_array()
			|| action.at("rationales").empty() || !action.contains("operations") || !action.at("operations").is_array()
			|| action.at("operations").empty())
		throw std::runtime_error("The model returned an invalid target document action.");
	// The declared targetDocument names ONE atomic action's real file - every
	// operation below must resolve inside that same document, never a
	// different candidate the model also had visibility into (see
	// resolve_table()'s own ambiguity guard for the narrower case of one
	// colliding table_id).
	const std::string declaredTargetDocumentId =
		candidate_document_id_for_name(plan, action.value("targetDocument", std::string()));

	std::map<std::string, std::size_t> rationaleIndexes;
	for (const Json &proposal : action.at("rationales")) {
		if (!proposal.is_object() || proposal.size() != 6 || !proposal.contains("id")
				|| !proposal.at("id").is_string() || proposal.at("id").get<std::string>().empty()
				|| !proposal.contains("summary") || !proposal.at("summary").is_string()
				|| proposal.at("summary").get<std::string>().empty()
				|| !proposal.contains("projectEvidence") || !proposal.at("projectEvidence").is_array()
				|| !proposal.contains("methodologyEvidence") || !proposal.at("methodologyEvidence").is_array()
				|| !proposal.contains("externalEvidenceIds") || !proposal.at("externalEvidenceIds").is_array()
				|| !proposal.contains("externalSupportStatus") || !proposal.at("externalSupportStatus").is_string())
			throw std::runtime_error("A proposed document-action rationale is malformed.");
		DocumentActionRationale rationale;
		rationale.id = proposal.at("id").get<std::string>();
		if (rationaleIndexes.count(rationale.id) != 0)
			throw std::runtime_error("A document-action rationale id is duplicated.");
		rationale.summary = proposal.at("summary").get<std::string>();
		const auto status = external_support_status_from_string(
			proposal.at("externalSupportStatus").get<std::string>());
		if (!status || *status == ExternalSupportStatus::Unspecified)
			throw std::runtime_error("A rationale has no valid external support status.");
		rationale.external_support_status = *status;
		for (const Json &entry : proposal.at("projectEvidence"))
			rationale.project_evidence.push_back(resolve_cell_locator(plan, declaredTargetDocumentId, entry));
		for (const Json &entry : proposal.at("methodologyEvidence"))
			rationale.methodology_evidence.push_back(resolve_methodology_evidence(plan, entry));
		std::set<std::string> evidenceIds;
		for (const Json &entry : proposal.at("externalEvidenceIds")) {
			if (!entry.is_string() || entry.get<std::string>().empty())
				throw std::runtime_error("An external evidence id is malformed.");
			const auto &evidence = resolve_external_evidence(entry.get<std::string>(), availableExternalEvidence);
			if (!evidenceIds.insert(evidence.evidence_id).second)
				throw std::runtime_error("An external evidence id is duplicated in a rationale.");
			rationale.external_evidence_ids.push_back(evidence.evidence_id);
			if (std::find_if(resolved.referenced_external_evidence.begin(), resolved.referenced_external_evidence.end(),
				[&](const auto &known) { return known.evidence_id == evidence.evidence_id; })
				== resolved.referenced_external_evidence.end()) resolved.referenced_external_evidence.push_back(evidence);
		}
		if (rationale.external_support_status == ExternalSupportStatus::Supported) {
			if (rationale.external_evidence_ids.empty()) {
				const bool retrievedAvailable = std::any_of(availableExternalEvidence.begin(), availableExternalEvidence.end(),
					[](const auto &entry) { return entry.content_kind == research::ExternalEvidenceContentKind::RetrievedContent; });
				if (retrievedAvailable)
					throw std::runtime_error("A rationale claims external support but cites no current-operation evidence.");
				throw ResearchCoverageError("A rationale claims external support but no retrieved current-operation evidence is available.");
			}
			for (const auto &id : rationale.external_evidence_ids)
				if (resolve_external_evidence(id, availableExternalEvidence).content_kind
						!= research::ExternalEvidenceContentKind::RetrievedContent)
					throw ResearchCoverageError("A rationale claims external support from a search result that was not opened/retrieved. Use web.open or mark the conclusion not_established.");
		} else if (rationale.external_support_status == ExternalSupportStatus::NotEstablished) {
			for (const auto &id : rationale.external_evidence_ids)
				if (resolve_external_evidence(id, availableExternalEvidence).content_kind
						!= research::ExternalEvidenceContentKind::RetrievedContent)
					throw ResearchCoverageError(
						"A rationale marked not_established cites material that was not opened/retrieved. Use web.open or omit that evidence ID.");
		} else if (!rationale.external_evidence_ids.empty()) {
			throw std::runtime_error("A rationale marked not_applicable must not cite external evidence.");
		}
		rationaleIndexes.emplace(rationale.id, resolved.rationales.size());
		resolved.rationales.push_back(std::move(rationale));
	}
	if (plan.requires_retrieved_external_evidence
			&& std::none_of(availableExternalEvidence.begin(), availableExternalEvidence.end(),
				[](const auto &entry) {
					return entry.content_kind == research::ExternalEvidenceContentKind::RetrievedContent;
				}))
		throw ResearchCoverageError(
			"This research-required document action has only discovery results. Open a relevant source before finalizing, or fail without changing the document.");

	std::set<std::string> usedRationales;
	// Per rationale, how many of its operations are the one explicit
	// rationale/sources citation operation (renderEvidence=true). Zero is
	// valid (an ordinary substantive field has nowhere for a citation to go);
	// more than one is not (only one operation can be "the" citation field).
	std::map<std::string, std::size_t> citationRenderedCounts;
	std::set<std::string> changedCells;
	std::map<std::string, std::vector<std::pair<std::string, Json>>> proposedTextByRationale;
	for (const Json &proposal : action.at("operations")) {
		if (!proposal.is_object() || proposal.size() != 5
				|| proposal.value("kind", std::string()) != "replaceCellText"
				|| !proposal.contains("locator") || !proposal.contains("newValue")
				|| !proposal.at("newValue").is_string() || !proposal.contains("rationaleId")
				|| !proposal.at("rationaleId").is_string() || !proposal.contains("renderEvidence")
				|| !proposal.at("renderEvidence").is_boolean())
			throw std::runtime_error("A proposed document operation is malformed.");
		const std::string modelText = proposal.at("newValue").get<std::string>();
		if (contains_model_authored_citation(modelText))
			throw std::runtime_error("A document operation contains model-authored citation tokens or source URLs; Singularity must render them from canonical evidence.");
		const DocumentTableCell *currentCell = nullptr;
		// Resolved only inside the action's own declared targetDocument (see
		// declaredTargetDocumentId above) - an operation actually meant for a
		// different candidate document fails closed here (its locator simply
		// does not resolve in this one), never silently reaching across into
		// another Project document.
		const DocumentLocator locator = resolve_cell_locator(plan, declaredTargetDocumentId,
			proposal.at("locator"), &currentCell);
		const std::string rationaleId = proposal.at("rationaleId").get<std::string>();
		const auto rationaleIndex = rationaleIndexes.find(rationaleId);
		if (rationaleIndex == rationaleIndexes.end())
			throw std::runtime_error("A document operation references an unknown rationale.");
		const std::string cellKey = document_locator_to_json(locator).dump();
		if (!changedCells.insert(cellKey).second)
			throw std::runtime_error("More than one operation targets the same document cell.");
		usedRationales.insert(rationaleId);
		DocumentActionRationale &rationale = resolved.rationales[rationaleIndex->second];
		if (rationale.project_evidence.empty()) rationale.project_evidence.push_back(locator);
		// The durable document field carries the model's own professional value
		// (WORK) - project/methodology locators and internal handles never
		// appear in it (they stay exclusively in the canonical rationale, i.e.
		// INSPECT). The one operation per rationale explicitly marked
		// renderEvidence=true is the sole exception: its target artifact field
		// is itself meant to hold rationale/sources/citations, so Singularity may
		// append a verified, human-readable citation derived from canonical
		// retrieved evidence - never an internal locator or opaque handle.
		std::string value = modelText;
		if (proposal.at("renderEvidence").get<bool>()) {
			++citationRenderedCounts[rationaleId];
			value += render_verified_external_citations(rationale, availableExternalEvidence);
		}
		proposedTextByRationale[rationaleId].push_back({modelText, document_locator_to_json(locator)});
		resolved.patch_operations.push_back({{"kind", "replaceCellText"},
			{"locator", document_locator_to_json(locator)}, {"expectedOldValue", currentCell->text},
			{"newValue", value}, {"rationaleId", rationaleId}});
	}
	if (usedRationales.size() != rationaleIndexes.size())
		throw std::runtime_error("A document-action rationale is not linked to any operation.");
	if (std::any_of(citationRenderedCounts.begin(), citationRenderedCounts.end(),
			[](const auto &entry) { return entry.second > 1; }))
		throw std::runtime_error("A rationale may have at most one renderEvidence=true (rationale/sources) operation.");
	if (rationaleIndexes.size() > 1) {
		// Distinct professional reasoning per item, proven from the canonical
		// rationale summary (always required, always present) - never from
		// whether a rationale happened to render a citation, so this still
		// catches copy-pasted boilerplate even when every item is an ordinary
		// substantive field with zero renderEvidence=true operations.
		std::set<std::string> summaries;
		for (const auto &rationale : resolved.rationales) summaries.insert(rationale.summary);
		if (summaries.size() == 1)
			throw std::runtime_error(
				"A multi-item document action repeated one generic explanation across distinct items. Explain each item from its own cited Project facts.");
	}
	for (const auto &rationale : resolved.rationales) {
		const auto trusted = numeric_tokens(trusted_numeric_text(plan, rationale));
		const Json rationaleLocator = rationale.project_evidence.empty()
			? Json::object() : document_locator_to_json(rationale.project_evidence.front());
		if (const auto issue = impossible_numeric_comparison(rationale.summary, trusted))
			throw std::runtime_error(numeric_comparison_message(*issue, rationale.id, rationaleLocator));
		for (const auto &[text, locator] : proposedTextByRationale[rationale.id])
			if (const auto issue = impossible_numeric_comparison(text, trusted))
				throw std::runtime_error(numeric_comparison_message(*issue, rationale.id, locator));
	}
	return resolved;
}

const research::ExternalEvidenceReference &resolve_external_alias(const std::string &alias,
		const std::vector<research::ExternalEvidenceReference> &available) {
	if (alias.size() < 2 || alias.front() != 'S'
			|| !std::all_of(alias.begin() + 1, alias.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
		throw std::runtime_error("An external evidence alias is malformed.");
	std::size_t index = 0;
	try { index = static_cast<std::size_t>(std::stoull(alias.substr(1))); }
	catch (...) { throw std::runtime_error("An external evidence alias is malformed."); }
	if (index == 0 || index > available.size())
		throw std::runtime_error("An external evidence alias does not resolve in this operation.");
	return available[index - 1];
}

std::string assembled_draft_result(const ConnectedDocumentActionPlan &plan,
		const std::string &answer) {
	Json rationales = Json::array();
	Json operations = Json::array();
	// Each already-Accepted item recorded its own real targetDocument (see
	// upsert_connected_document_draft_item()) - reuse the first one so a
	// single-document draft (the overwhelming common case) round-trips
	// correctly through resolve_document_action_proposal() below. If items
	// were somehow accepted against more than one candidate document, that
	// function's own per-operation document check fails this closed with a
	// clear correction rather than silently mixing two files' cells into one
	// atomic action.
	std::optional<std::string> targetDocument;
	for (const auto &[itemHandle, item] : plan.draft->items) {
		(void)itemHandle;
		if (item.state != DocumentDraftItemState::Accepted || !item.accepted_action) continue;
		if (!targetDocument) targetDocument = item.accepted_action->at("targetDocument").get<std::string>();
		for (const auto &rationale : item.accepted_action->at("rationales")) rationales.push_back(rationale);
		for (const auto &operation : item.accepted_action->at("operations")) operations.push_back(operation);
	}
	return Json{{"answer", answer}, {"documentActions", Json::array({Json{
		{"targetDocument", targetDocument.value_or(plan.target_document_name)},
		{"rationales", std::move(rationales)}, {"operations", std::move(operations)}}})}}.dump();
}

std::size_t accepted_draft_item_count(const ConnectedDocumentActionPlan &plan) {
	return static_cast<std::size_t>(std::count_if(plan.draft->items.begin(), plan.draft->items.end(),
		[](const auto &entry) { return entry.second.state == DocumentDraftItemState::Accepted; }));
}

bool required_output_satisfied(const ConnectedDocumentActionPlan &plan,
		const RequiredOutputObligation &obligation) {
	const auto item = plan.draft->items.find(obligation.item_handle);
	if (item == plan.draft->items.end() || item->second.state != DocumentDraftItemState::Accepted
			|| !item->second.accepted_action) return false;
	const auto locator = plan.field_handles.find(obligation.field_handle);
	if (locator == plan.field_handles.end()) return false;
	const Json expected = model_cell_locator_json(locator->second);
	for (const Json &operation : item->second.accepted_action->value("operations", Json::array())) {
		if (operation.is_object() && operation.value("kind", std::string()) == "replaceCellText"
				&& operation.value("locator", Json::object()) == expected) return true;
	}
	return false;
}

bool all_required_outputs_satisfied(const ConnectedDocumentActionPlan &plan) {
	if (!plan.draft->output_agenda_defined || plan.draft->required_outputs.empty()) return false;
	std::set<std::string> selectedRegions(plan.draft->selected_output_region_handles.begin(),
		plan.draft->selected_output_region_handles.end());
	if (selectedRegions.empty())
		for (const RequiredOutputObligation &output : plan.draft->required_outputs)
			selectedRegions.insert(output.item_handle);
	for (const std::string &itemHandle : selectedRegions) {
		const auto editable = plan.item_editable_field_handles.find(itemHandle);
		if (editable == plan.item_editable_field_handles.end() || editable->second.empty()) return false;
		for (const std::string &fieldHandle : editable->second)
			if (!effective_field_disposition(plan, fieldHandle)) return false;
	}
	return std::all_of(plan.draft->required_outputs.begin(), plan.draft->required_outputs.end(),
		[&](const RequiredOutputObligation &obligation) {
			return required_output_satisfied(plan, obligation);
		});
}

// CORE PRODUCT LAW: how many structural regions the request concerns is the
// model's judgment, captured by its finalized output agenda. Once every exact
// WRITE obligation in that agenda has a validated accepted delta, Drafting is
// structurally complete and ProfessionalReview owns the next judgment. The
// legacy per-upsert `final` flag is a model hint/persisted compatibility
// signal, not a second source of professional truth: making final=false block
// this transition can leave no editable item and no legal next action after
// the last required delta has already been accepted.
bool draft_is_complete(const ConnectedDocumentActionPlan &plan) {
	return all_required_outputs_satisfied(plan);
}

const char *draft_item_state_string(DocumentDraftItemState state) {
	switch (state) {
		case DocumentDraftItemState::Pending: return "pending";
		case DocumentDraftItemState::NeedsResearch: return "needs_research";
		case DocumentDraftItemState::NeedsCorrection: return "needs_correction";
		case DocumentDraftItemState::Accepted: return "accepted";
	}
	return "pending";
}

DocumentDraftItemState draft_item_state_from_string(const std::string &value) {
	if (value == "needs_research") return DocumentDraftItemState::NeedsResearch;
	if (value == "needs_correction") return DocumentDraftItemState::NeedsCorrection;
	if (value == "accepted") return DocumentDraftItemState::Accepted;
	return DocumentDraftItemState::Pending;
}

// THE RUNTIME OWNS THE AGENDA. THE MODEL OWNS THE JUDGMENT: whether
// `documentId` (one of plan.draft->required_evidence's own obligations) is
// satisfied. Inspecting a table (DocumentActionDraft::expanded_table_ids)
// proves the region was looked at; it does NOT by itself prove the model
// considers its review sufficient for the professional task - so this also
// requires the model's own explicit sufficiency declaration for this exact
// document (DocumentActionDraft::evidence_declared_sufficient, set only by
// request_connected_document_table_inspection() on a call that itself
// inspects one of THIS document's own tables, never a bare declaration with
// no real inspection behind it). Singularity never asks "enough" or "the right
// region" beyond that - the MODEL alone decides which region(s) to inspect,
// whether to inspect more before declaring, and what the facts mean. A
// required document with no tables at all (nothing to ground) is vacuously
// satisfied - this can only happen for a document format this plan never
// actually populated table_handles for, never a real gap in a real
// DOCX/CSV/PPTX Project document.
// Merges [start, end) into objective's own inspected_ranges (runtime-owned,
// always clamped/real - see request_connected_document_table_inspection())
// via a standard sort-and-merge-adjacent-or-overlapping pass. Deliberately
// re-sorts from scratch on every call rather than maintaining insertion
// order: a real objective accumulates at most a handful of windows, so the
// O(n log n) cost is negligible next to the correctness of never hand-rolling
// a single-pass merge's edge cases.
void merge_evidence_coverage_range(EvidenceObjective &objective, std::size_t start, std::size_t end) {
	if (start >= end) return;
	std::vector<EvidenceCoverageRange> all = objective.inspected_ranges;
	all.push_back({start, end});
	std::sort(all.begin(), all.end(),
		[](const EvidenceCoverageRange &a, const EvidenceCoverageRange &b) { return a.start_row < b.start_row; });
	std::vector<EvidenceCoverageRange> merged;
	for (const EvidenceCoverageRange &range : all) {
		if (!merged.empty() && range.start_row <= merged.back().end_row)
			merged.back().end_row = std::max(merged.back().end_row, range.end_row);
		else
			merged.push_back(range);
	}
	objective.inspected_ranges = std::move(merged);
}

// EVIDENCE SUFFICIENCY IS NOT A BOOLEAN WITHOUT COVERAGE: whether every
// EvidenceObjective the model itself bound to one of documentId's own tables
// has reached the completeness its own coverage mode requires. EXHAUSTIVE
// requires inspected_ranges to cover that table's real row count with no gap
// (merge_evidence_coverage_range() already guarantees non-overlap, so a
// simple length sum against the real row count is an exact completeness
// test); TARGETED requires either that same full coverage OR an explicit
// model-authored closure (EvidenceObjective::closed - see document.inspect's
// own evidenceObjectiveClosure argument). A document with no declared
// objective at all has no coverage obligation beyond document_evidence_
// satisfied()'s own pre-existing inspect-and-declare check - this is purely
// additive, model-opt-in state. A stale/foreign table_handle (a checkpoint
// restored against a document that no longer has that table) never blocks -
// see restore_connected_document_draft_checkpoint()'s own fail-closed
// handling of the objective itself, not this read-only query.
bool document_evidence_coverage_complete(const ConnectedDocumentActionPlan &plan, const std::string &documentId) {
	for (const auto &[id, objective] : plan.draft->evidence_objectives) {
		if (objective.document_id != documentId) continue;
		const auto table = plan.table_handles.find(objective.table_handle);
		if (table == plan.table_handles.end()) continue;
		std::size_t coveredRows = 0;
		for (const EvidenceCoverageRange &range : objective.inspected_ranges)
			coveredRows += range.end_row - range.start_row;
		if (coveredRows >= table->second.row_count) continue;
		if (objective.coverage_mode == EvidenceCoverageMode::Targeted && objective.closed) continue;
		return false;
	}
	return true;
}

struct EvidenceCoverageGap {
	std::size_t start_row = 0;
	std::size_t size = 0;
};

// The first uncovered range in an objective's own bound table, in row order -
// inspected_ranges is already sorted/non-overlapping (see merge_evidence_
// coverage_range()), so a single linear scan finds it deterministically.
std::optional<EvidenceCoverageGap> first_evidence_coverage_gap(
		const EvidenceObjective &objective, std::size_t totalRows) {
	std::size_t cursor = 0;
	for (const EvidenceCoverageRange &range : objective.inspected_ranges) {
		if (range.start_row > cursor) return EvidenceCoverageGap{cursor, range.start_row - cursor};
		cursor = std::max(cursor, range.end_row);
	}
	if (cursor < totalRows) return EvidenceCoverageGap{cursor, totalRows - cursor};
	return std::nullopt;
}

// The exact, actionable "evidence_coverage_incomplete" detail (Sec.7 of the
// governing directive): every one of documentId's own objectives still short
// of the completeness its coverage mode requires, each with its own real
// table, totalRows, inspected-row count and a concrete, boundedly-sized next
// document.inspect call the model may submit verbatim. Empty when nothing is
// incomplete - shared by request_connected_document_table_inspection()'s own
// trailing message and define_connected_document_output_agenda()'s
// required_evidence_unresolved rejection, so neither can ever describe this
// differently from the other.
std::string describe_incomplete_evidence_coverage(
		const ConnectedDocumentActionPlan &plan, const std::string &documentId) {
	std::string message;
	for (const auto &[id, objective] : plan.draft->evidence_objectives) {
		if (objective.document_id != documentId) continue;
		const auto table = plan.table_handles.find(objective.table_handle);
		if (table == plan.table_handles.end()) continue;
		std::size_t coveredRows = 0;
		for (const EvidenceCoverageRange &range : objective.inspected_ranges)
			coveredRows += range.end_row - range.start_row;
		if (coveredRows >= table->second.row_count) continue;
		if (objective.coverage_mode == EvidenceCoverageMode::Targeted && objective.closed) continue;
		const bool exhaustive = objective.coverage_mode == EvidenceCoverageMode::Exhaustive;
		message += " evidence_coverage_incomplete: objective \"" + id + "\" (" + objective.description
			+ ") on table " + objective.table_handle + " is " + (exhaustive ? "exhaustive" : "targeted")
			+ " - totalRows=" + std::to_string(table->second.row_count)
			+ ", inspected=" + std::to_string(coveredRows) + ".";
		if (!exhaustive)
			message += " Close it explicitly with evidenceObjectiveClosure (id/closureReason/"
				"unresolvedQuestions/scopeLimitations) if you intend to stop short, or continue inspecting.";
		const auto gap = first_evidence_coverage_gap(objective, table->second.row_count);
		if (gap)
			message += " Next: document.inspect tableHandle=" + objective.table_handle
				+ " startRow=" + std::to_string(gap->start_row)
				+ " rowCount=" + std::to_string(std::min<std::size_t>(10, gap->size)) + ".";
	}
	return message;
}

bool document_evidence_satisfied(const ConnectedDocumentActionPlan &plan, const std::string &documentId) {
	bool hasAnyTable = false;
	bool hasInspectedTable = false;
	for (const auto &[handle, summary] : plan.table_handles) {
		if (summary.document_id != documentId) continue;
		hasAnyTable = true;
		if (plan.draft->expanded_table_ids.count(handle) != 0) hasInspectedTable = true;
	}
	if (!hasAnyTable) return true;
	if (!hasInspectedTable || plan.draft->evidence_declared_sufficient.count(documentId) == 0) return false;
	return document_evidence_coverage_complete(plan, documentId);
}

// Whether `tableHandle` must be inspected via document.inspect before its
// own rows may appear as real items, for either of two INDEPENDENT reasons:
// (1) the overall resolved model capacity could not fit the full Project
// structure (ConnectedDocumentActionPlan::compact_context_mode - a context-
// CAPACITY decision), or (2) THE RUNTIME OWNS THE AGENDA - this table
// belongs to a document the instruction explicitly required be studied
// (ConnectedDocumentActionPlan::required_evidence - a TASK-DISCIPLINE
// decision, deliberately independent of whether that document's content
// would otherwise have fit directly). A required document is never merely
// dumped into context because it happened to fit; the model must take the
// same observable, auditable document.inspect action either way. Already-
// expanded tables are never re-collapsed.
bool table_requires_inspection(const ConnectedDocumentActionPlan &plan, const std::string &tableHandle) {
	if (plan.compact_context_mode) return true;
	if (plan.draft->expanded_table_ids.count(tableHandle) != 0) return false;
	const auto summary = plan.table_handles.find(tableHandle);
	if (summary == plan.table_handles.end()) return false;
	for (const auto &obligation : plan.draft->required_evidence)
		if (obligation.document_id == summary->second.document_id) return true;
	return false;
}

// Whether tableHandle should remain nameable via document.inspect AT ALL -
// either because table_requires_inspection() says its own rows are not yet
// revealed, or because it belongs to a required-evidence document not yet
// marked sufficient (see document_evidence_satisfied()) even though every
// one of that document's own tables has already been expanded once. Without
// this second reason a model that thoroughly inspects every real table of a
// required document before ever declaring evidenceSufficient=true is left
// with no admissible table handle left to name for that declaration at all
// - a real design-partner acceptance run hit this concretely (a genuinely
// thorough real model exhaustively inspected all 15 real tables across two
// required documents without once declaring sufficiency for either, then
// had no legal next action Singularity's own turn contract could offer it).
// Never changes is_item_collapsed()'s own, narrower question of whether an
// already-expanded table's real rows still count as collapsed - they do
// not, regardless of this; only whether the HANDLE stays selectable for a
// further document.inspect call (a fresh row window, or a bare sufficiency
// declaration on the same one).
bool table_selectable_for_inspection(const ConnectedDocumentActionPlan &plan, const std::string &tableHandle) {
	if (table_requires_inspection(plan, tableHandle)) return true;
	const auto summary = plan.table_handles.find(tableHandle);
	if (summary == plan.table_handles.end()) return false;
	for (const auto &obligation : plan.draft->required_evidence)
		if (obligation.document_id == summary->second.document_id
				&& !document_evidence_satisfied(plan, obligation.document_id))
			return true;
	return false;
}

// A table handle's own original discovery-order index ("T<n>" -> n), never
// its lexicographic string order (which is not numerically stable - "T10"
// sorts before "T2" as plain text). This is purely a stable, deterministic
// navigation key - it carries no legal/semantic weight, exactly like a
// document's own row order already does not.
unsigned long table_handle_discovery_order(const std::string &tableHandle) {
	std::size_t digitsStart = tableHandle.size();
	while (digitsStart > 0 && std::isdigit(static_cast<unsigned char>(tableHandle[digitsStart - 1])))
		--digitsStart;
	if (digitsStart == tableHandle.size()) return 0;
	return std::strtoul(tableHandle.c_str() + digitsStart, nullptr, 10);
}

// Every table of `documentId` that still requires inspection, in stable
// discovery order - the single shared source of truth for both what
// compile_connected_document_turn_data() may reveal as a table-map page and
// what request_connected_document_table_map_next_page() advances through, so
// the two can never disagree on the document's own real table-map size.
std::vector<std::string> eligible_table_handles_for_document(
		const ConnectedDocumentActionPlan &plan, const std::string &documentId) {
	std::vector<std::string> handles;
	for (const auto &[tableHandle, summary] : plan.table_handles)
		if (summary.document_id == documentId && table_selectable_for_inspection(plan, tableHandle))
			handles.push_back(tableHandle);
	std::sort(handles.begin(), handles.end(), [](const std::string &a, const std::string &b) {
		return table_handle_discovery_order(a) < table_handle_discovery_order(b);
	});
	return handles;
}

// Shared by compile_connected_document_turn_data() and compile_connected_
// document_operation_state() so the model-visible turn text and the
// TurnContract-enforced handle whitelist can never diverge on which items
// are actually withheld. Withholds an item for either of two independent
// reasons: (1) an explicit required-evidence declaration is mandatory but
// has not yet been made (plan.requires_evidence_agenda), or (2) its own
// table itself requires inspection (see table_requires_inspection() above -
// either context-capacity or agenda-driven). THE MODEL cannot rationally
// judge a required document's own review "sufficient" (document.inspect's
// evidenceSufficient argument) without ever being shown that document's own
// real row content first - a real design-partner acceptance run against a
// real frontier model with a real large context window (compact_context_
// mode never applied) proved that a THIRD, cross-document reason this
// function used to also check here (withholding document D's own items
// while some OTHER required document remained ungrounded) was an outright
// deadlock whenever more than one document was required at once: each
// document was always "some OTHER required document" relative to the
// other, so a model that declared two documents required could inspect
// either one any number of times and never see a single real row of
// either. Visibility (this function) is therefore now scoped ONLY to an
// item's own table; see document_grounding_complete_for_drafting() below
// for the separate, still fully operation-wide question of whether a
// visible item may actually be DRAFTED INTO yet.
// SINGULARITY OWNS THE COVERAGE STATE, NEVER THE MODEL'S OWN MEMORY OF WHAT IT
// ALREADY SAW: whether tableHandle's own rows should stay disposable (one
// bounded window visible at a time, replaced rather than accumulated) even
// when the operation's overall capacity would otherwise permit exposing
// everything at once. compact_context_mode already needed this (a small
// model's real turn budget); a table under active EvidenceObjective coverage
// tracking needs it for an independent reason a real production run
// surfaced concretely: is_item_collapsed()'s own pre-existing fallback for
// non-compact operations - "expanded once, visible forever" - meant a single
// document.inspect call against one wide table (see EvidenceObjective's own
// exhaustive coverage, which by design asks for several such calls) made
// every one of that table's real rows a permanently-visible item from then
// on, re-including (and Papinian re-protecting) the SAME already-seen
// content on every later turn regardless of which rows were ever actually
// requested. Scoped to objective-bound tables only - an ordinary table with
// no declared coverage question keeps today's unchanged full-exposure
// behavior once opened.
bool table_uses_disposable_window(const ConnectedDocumentActionPlan &plan, const std::string &tableHandle) {
	return plan.compact_context_mode || plan.draft->table_evidence_objective.count(tableHandle) != 0;
}

bool is_item_collapsed(const ConnectedDocumentActionPlan &plan, const std::string &itemHandle) {
	if (plan.requires_evidence_agenda && !plan.draft->evidence_agenda_defined) return true;
	const auto tableHandle = plan.item_table_handle.find(itemHandle);
	if (tableHandle == plan.item_table_handle.end()) return false;
	if (table_uses_disposable_window(plan, tableHandle->second)) {
		// The operation's own already-selected output region (document.outputs.
		// define's own docstring: "The complete agenda becomes immutable" once
		// final=true) must never be paged back out of view by this table's
		// disposable row window narrowing for a later, unrelated reason - a
		// real Phase 1-28 local-only acceptance run proved this reachable: a
		// document.methodology.reveal turn grows the prompt enough to trigger
		// real context compaction (see the research loop's own
		// operationContextCompactionHandler), which narrows active_row_start/
		// active_row_count to fit the model's real remaining budget - silently
		// collapsing the already-committed row right back out from under
		// Drafting, which then finds zero real editable items anywhere
		// (constrain_draft_capabilities()'s own "no item with an editable
		// capability" invariant) and fails the entire operation over a target
		// it had already locked in three turns earlier. The disposable window
		// exists to bound how much of a STILL-UNDECIDED table is shown during
		// evidence/output planning, never to hide the operation's own
		// already-committed real work target during drafting.
		if (plan.draft->output_agenda_defined && is_selected_output_region(plan, itemHandle)) return false;
		const auto rowIndex = plan.item_table_row_index.find(itemHandle);
		if (!plan.draft->active_table_id
				|| *plan.draft->active_table_id != tableHandle->second
				|| rowIndex == plan.item_table_row_index.end()
				|| rowIndex->second < plan.draft->active_row_start
				|| rowIndex->second >= plan.draft->active_row_start + plan.draft->active_row_count)
			return true;
		return false;
	}
	return table_requires_inspection(plan, tableHandle->second);
}

// THE RUNTIME OWNS THE AGENDA: whether drafting into ANY candidate document
// may proceed at all right now - operation-wide, deliberately never scoped
// to one item's own document, because the model itself was told (see
// compile_connected_document_turn_data()'s own instruction text) that every
// required document must be grounded "before you draft into ANY candidate
// document, regardless of which one you end up drafting into". An item can
// be visible (is_item_collapsed() above says no) for review - reading a
// required document's own existing content is exactly what inspecting it is
// for - while still not yet draftable, because grounding review and
// committing a change are different questions once more than one document
// is simultaneously required.
bool document_grounding_complete_for_drafting(const ConnectedDocumentActionPlan &plan) {
	return std::all_of(plan.draft->required_evidence.begin(), plan.draft->required_evidence.end(),
		[&](const RequiredEvidenceObligation &obligation) {
			return document_evidence_satisfied(plan, obligation.document_id);
		});
}

// One atomic action commits into exactly one real Project document - see
// resolve_document_action_proposal()'s own comment ("every operation below
// must resolve inside that same document, never a different candidate the
// model also had visibility into"). Which one is THE MODEL's own judgment,
// never Singularity's to pre-pick (item6/item7: any real candidate - not only
// plan.target_document_id, itself no more than a positional default - may
// legitimately be the one the model actually commits to) - so nothing here
// may ever exclude a document before the model has chosen at all.
//
// The real commitment point is document.outputs.define, not a later
// document.draft.upsert: upsert_connected_document_draft_item() already
// refuses every call until plan.draft->output_agenda_defined, so by the
// time any item can ever be accepted, required_outputs already names one
// specific item/field (and define_connected_document_output_agenda()'s own
// "one atomic output agenda may target only one Project document" check
// already guarantees every one of them shares a single document). A live
// GPT-5.1 acceptance run proved why the FIRST-accepted-item version of this
// check was still wrong: the model defined its real output agenda against
// the destination first, then submitted one exploratory, non-required
// upsert into the source it had just reviewed before ever touching the
// destination - a first-accepted-item rule then locked the operation onto
// the source, permanently excluding the destination's own required-output
// item even though it was still genuinely pending, unsatisfied, and
// otherwise perfectly legal to draft into. Deriving commitment from the
// output agenda itself instead means the real deliverable's own document
// is fixed the moment the model names it, before any upsert - a document
// the model still could have committed to earlier by simply drafting into
// it, but only ever needs one authoritative source of truth for which.
std::optional<std::string> operation_committed_document_id(const ConnectedDocumentActionPlan &plan) {
	if (!plan.draft->required_outputs.empty()) {
		const auto tableHandle = plan.item_table_handle.find(plan.draft->required_outputs.front().item_handle);
		if (tableHandle != plan.item_table_handle.end()) {
			const auto summary = plan.table_handles.find(tableHandle->second);
			if (summary != plan.table_handles.end()) return summary->second.document_id;
		}
	}
	return std::nullopt;
}

bool item_document_is_draft_target(const ConnectedDocumentActionPlan &plan, const std::string &itemHandle) {
	const auto tableHandle = plan.item_table_handle.find(itemHandle);
	if (tableHandle == plan.item_table_handle.end()) return false;
	const auto summary = plan.table_handles.find(tableHandle->second);
	if (summary == plan.table_handles.end()) return false;
	const std::optional<std::string> committed = operation_committed_document_id(plan);
	return !committed || *committed == summary->second.document_id;
}

// A pure function of `plan` alone (no evidence/issue input needed - none of
// its conditions ever depend on either), so this is the single source of
// truth for BOTH compile_connected_document_operation_state()'s own
// projection.frontier AND compile_connected_document_turn_data()'s
// phase-scoped instruction gating below - the two must never independently
// re-derive this and risk diverging on which capabilities a turn actually
// exposes. Mirrors compile_connected_document_operation_state()'s own
// frontier logic exactly (see its own comments for why each branch exists
// and why the order matters); kept in sync by construction, not convention,
// since that function now calls this one instead of repeating it.
// Whether the existing Professional Completion Challenge has already
// recorded a Pass for plan.draft's own CURRENT revision - the one condition
// connected_document_operation_frontier() (frontier/tool-exposure gating)
// and connected_document_draft_completion_issue() (the actual completion
// gate ResearchToolLoop's own finalTextIssue callback consults before ever
// letting the whole operation return) must agree on, so neither can decide
// "professionally complete" independently of the other. A Pass recorded
// against an earlier revision never counts - see
// submit_connected_document_professional_review()'s own routing, which
// bumps ++plan.draft->revision on redraft/replan/evidence changes
// specifically so a stale Pass can never authorize completion of work the
// model has not actually been re-challenged on.
bool professional_review_passed_current_revision(const ConnectedDocumentActionPlan &plan) {
	return plan.draft->professional_review
		&& plan.draft->professional_review_reviewed_revision == plan.draft->revision
		&& plan.draft->professional_review->verdict == ProfessionalReviewVerdict::Pass;
}

research::OperationWorkFrontier connected_document_operation_frontier(const ConnectedDocumentActionPlan &plan) {
	const auto researchItem = std::find_if(plan.draft->items.begin(), plan.draft->items.end(),
		[](const auto &entry) { return entry.second.state == DocumentDraftItemState::NeedsResearch; });
	if (researchItem != plan.draft->items.end()) return research::OperationWorkFrontier::NeedsResearch;
	// A structurally-complete draft is not yet a professionally-complete one
	// - the Professional Completion Challenge is a second, independent gate
	// that must record a Pass at the draft's CURRENT revision before Complete
	// is ever reached. See submit_connected_document_professional_review's
	// own routing: a non-Pass verdict resets whichever state makes this same
	// draft_is_complete check false again (finished, output agenda or
	// evidence sufficiency), so this frontier naturally falls through to
	// wherever that verdict's own issues actually need fixing.
	if (draft_is_complete(plan)) {
		if (professional_review_passed_current_revision(plan)) return research::OperationWorkFrontier::Complete;
		return research::OperationWorkFrontier::ProfessionalReview;
	}
	if (plan.requires_evidence_agenda && !plan.draft->evidence_agenda_defined)
		return research::OperationWorkFrontier::PlanningEvidence;
	if (!plan.draft->output_agenda_defined && document_grounding_complete_for_drafting(plan))
		return research::OperationWorkFrontier::PlanningOutputs;
	const bool correction = std::any_of(plan.draft->items.begin(), plan.draft->items.end(),
		[](const auto &entry) { return entry.second.state == DocumentDraftItemState::NeedsCorrection; });
	return correction ? research::OperationWorkFrontier::Correction : research::OperationWorkFrontier::Drafting;
}

} // namespace

// Substrings a real prompt quoted verbatim (plain "..." or Cyrillic
// typographic «...»), in order. A minimum length of 4 bytes is not a
// linguistic judgment - it only excludes a stray/empty quote pair from
// matching every document's stem trivially; it is not tuned to any
// particular script or vocabulary.
std::vector<std::string> quoted_phrases(const std::string &text) {
	static const std::vector<std::pair<std::string, std::string>> markers{
		{"\"", "\""}, {"\xc2\xab", "\xc2\xbb"}};
	std::vector<std::string> phrases;
	for (const auto &[open, close] : markers) {
		std::size_t position = 0;
		while (true) {
			const std::size_t start = text.find(open, position);
			if (start == std::string::npos) break;
			const std::size_t end = text.find(close, start + open.size());
			if (end == std::string::npos) break;
			std::string phrase = text.substr(start + open.size(), end - start - open.size());
			if (phrase.size() >= 4) phrases.push_back(std::move(phrase));
			position = end + close.size();
		}
	}
	return phrases;
}

// A non-blocking hint only (see prepare_connected_document_action()'s own
// comment): whether `instruction` plausibly refers to the Project document
// `name`. Bidirectional on purpose - a real design-partner prompt quoted a
// document by a shortened, descriptive phrase missing that file's own date
// prefix ("…" for a file actually named
// "…"), so the
// document's own (stemmed) name being found inside the instruction is not
// the only real-world direction; a quoted phrase from the instruction being
// found inside the document's own stem catches that shortened-reference
// case too. Never used to exclude a document - only to order candidates.
bool instruction_names_document(const std::string &instruction, const std::string &name) {
	if (instruction.find(name) != std::string::npos) return true;
	const std::string stem = without_extension(name);
	if (stem.empty()) return false;
	if (stem != name && instruction.find(stem) != std::string::npos) return true;
	for (const std::string &phrase : quoted_phrases(instruction))
		if (stem.find(phrase) != std::string::npos) return true;
	return false;
}

std::optional<ConnectedDocumentActionPlan> prepare_connected_document_action(
		const std::string &instruction, const matters::Matter &project,
		std::optional<std::size_t> availableContextWindowTokens, const std::vector<std::string> &selectedDocumentIds,
		const std::optional<std::string> &targetDocumentId) {
	// A Project having mutation-capable documents must never mean every
	// message is about them - see instruction_bypasses_document_engagement()'s
	// own header comment for why a bare greeting must never engage this
	// path just because the Project happens to have an editable document.
	if (instruction_bypasses_document_engagement(instruction)) return std::nullopt;
	// The request's source boundary: a document outside it is never read,
	// sent or changed by this plan, whatever the instruction names.
	const auto selected = [&](const matters::MatterDocument &document) {
		return selectedDocumentIds.empty()
			|| std::find(selectedDocumentIds.begin(), selectedDocumentIds.end(), document.id) != selectedDocumentIds.end();
	};
	const auto mayChange = [&](const matters::MatterDocument &document) {
		return targetDocumentId ? document.id == *targetDocumentId : selected(document);
	};
	const matters::MatterDocument *methodology = nullptr;
	std::vector<const matters::MatterDocument *> namedCandidates;
	for (const matters::MatterDocument &document : project.documents) {
		if (!instruction_names_document(instruction, document.name)) continue;
		if (is_methodology_reference(instruction, document.name)) {
			if (!selected(document)) continue;
			if (methodology != nullptr) throw std::runtime_error("Name one methodology file for this action.");
			methodology = &document;
		} else if (mayChange(document) && document_format_capability(document.file_type).supports_cell_or_paragraph_mutation) {
			namedCandidates.push_back(&document);
		}
	}
	// CORE PRODUCT LAW: which Project document a request is actually about is
	// legal/document judgment - the model's, never decided by Singularity
	// matching the instruction's text against a filename. A name mention only
	// narrows candidates when it resolves to at least one; a real prompt
	// naming two documents (a source to read and a register to draft into)
	// used to hard-fail here ("Apply one atomic Project-document action at a
	// time") the moment both happened to be named - both now become
	// candidates instead, and that atomicity guarantee is enforced later, at
	// the moment of an actual proposed mutation (see resolve_document_action_
	// proposal()'s own per-operation document check), not at selection time.
	// When nothing was named at all, every mutation-capable Project document
	// becomes a candidate so the request can still ground itself from real
	// structure instead of silently doing nothing. A document no longer in the
	// Project folder keeps its record (for move/restore reconciliation) but is
	// never an unnamed candidate - otherwise one deleted file would fail every
	// later request with a "named document" error the user never caused.
	std::vector<const matters::MatterDocument *> candidates = namedCandidates;
	if (candidates.empty()) {
		for (const matters::MatterDocument &document : project.documents) {
			if (&document == methodology) continue;
			if (!document.available || !mayChange(document)) continue;
			if (document_format_capability(document.file_type).supports_cell_or_paragraph_mutation)
				candidates.push_back(&document);
		}
	}
	if (candidates.empty()) return std::nullopt;
	for (const matters::MatterDocument *candidate : candidates)
		if (!candidate->available)
			throw std::runtime_error("A named document is not currently available in the Project folder.");
	if (methodology != nullptr && !methodology->available)
		throw std::runtime_error("A named document is not currently available in the Project folder.");

	ConnectedDocumentActionPlan plan;
	plan.requires_retrieved_external_evidence =
		research::resolve_research_mode(instruction) == research::ResearchMode::Required;

	// THE RUNTIME OWNS THE AGENDA / THE MODEL OWNS THE JUDGMENT: explicitly
	// naming more than one candidate document makes an explicit required-
	// evidence declaration mandatory (document.evidence.define) before any
	// item becomes visible - see is_item_collapsed(). This is a purely
	// structural fact (instruction_names_document() - existing, generic
	// reference resolution, never legal-relevance inference); it never
	// decides WHICH of those documents actually are required source
	// evidence. A named document can be a required source, a destination, a
	// template or optional context - that distinction is the model's own
	// judgment (define_connected_document_evidence_agenda()), never a
	// blanket "every other named document is evidence" backend inference. A
	// single named document, or none, carries no such obligation.
	plan.requires_evidence_agenda = namedCandidates.size() > 1;

	// Two Project documents can share a file name in different folders. Every
	// name the model sees - and every name an action resolves back to a
	// document - must identify exactly one document, so a shared file name is
	// shown as the document's Project-relative path.
	const auto display_name_for = [&](const matters::MatterDocument &document) {
		const auto shares_name = [&](const matters::MatterDocument *other) {
			return other != nullptr && other != &document && other->name == document.name;
		};
		const bool shared = std::any_of(candidates.begin(), candidates.end(), shares_name) || shares_name(methodology);
		return shared && !document.relative_path.empty() ? document.relative_path : document.name;
	};

	if (methodology != nullptr) {
		plan.methodology_document_id = methodology->id;
		plan.methodology_document_name = display_name_for(*methodology);
		plan.methodology_fingerprint = methodology->fingerprint;
		plan.roles.assign(methodology->id, DocumentRole::DelegatedMethodology);
		DocumentExtractionRequest request{document_path(project, *methodology), methodology->file_type};
		plan.methodology_model = read_document_structure(request);
		if (plan.methodology_model->status != ExtractionStatus::ready
				&& plan.methodology_model->status != ExtractionStatus::empty)
			throw std::runtime_error("Singularity could not structurally read the delegated methodology document.");
		std::size_t methodologyIndex = 1;
		for (const DocumentSlide &slide : plan.methodology_model->slides) {
			const std::string handle = "M" + std::to_string(methodologyIndex++);
			DocumentLocator locator;
			locator.document_id = *plan.methodology_document_id;
			locator.slide_id = slide.slide_id;
			plan.methodology_handles.emplace(handle, locator);
		}
	}

	// Product-owner correction: which document, and which row(s) within it, a
	// request is actually about is legal/document judgment - the model's job,
	// not Singularity's. Every row of every candidate document becomes a
	// structural item regardless of whether the instruction happens to quote
	// its row key or that document's own filename; connected_document_draft_
	// completion_issue() only requires the model to accept the item(s) it
	// decides are in scope, so exposing every row here costs nothing and
	// never silently hides the one row (or the one document) a natural-
	// language instruction did not happen to name verbatim.
	std::size_t itemIndex = 1;
	std::size_t fieldIndex = 1;
	std::size_t tableIndex = 1;
	// Only the default target's own item/field text is registered below -
	// this dump is only this diagnostic build's regression guard against a
	// prior real memory bug (a document's content silently duplicated into
	// the prompt), unrelated to what the model actually receives (see
	// compile_connected_document_turn_context() at the end of this function
	// for that).
	Json defaultTargetItems = Json::array();
	// Every candidate is read structurally up front. A named candidate that
	// cannot be read fails this action truthfully; an unnamed one (every
	// mutation-capable Project document when nothing was named) that cannot be
	// read - an unsafe path, a password-protected or damaged file - is
	// unavailable material for this candidate, never a failure of a request
	// that did not name it.
	std::vector<std::pair<const matters::MatterDocument *, DocumentSemanticModel>> readableCandidates;
	for (const matters::MatterDocument *candidate : candidates) {
		std::optional<DocumentSemanticModel> read;
		try {
			DocumentExtractionRequest request{document_path(project, *candidate), candidate->file_type};
			read = read_document_structure(request);
		} catch (const std::exception &) {
			if (!namedCandidates.empty()) throw;
		}
		if (!read || (read->status != ExtractionStatus::ready && read->status != ExtractionStatus::empty)) {
			if (!namedCandidates.empty())
				throw std::runtime_error("Singularity could not structurally read the named target document.");
			continue;
		}
		readableCandidates.emplace_back(candidate, std::move(*read));
	}
	if (readableCandidates.empty()) return std::nullopt;
	for (std::size_t candidateIndex = 0; candidateIndex < readableCandidates.size(); ++candidateIndex) {
		const matters::MatterDocument &document = *readableCandidates[candidateIndex].first;
		DocumentSemanticModel model = std::move(readableCandidates[candidateIndex].second);

		for (const DocumentTable &table : model.tables) {
			const std::string tableHandle = "T" + std::to_string(tableIndex++);
			{
				DocumentTableSummary summary;
				summary.document_id = document.id;
				summary.table_id = table.table_id;
				summary.row_count = table.rows.size();
				std::set<std::string> seenHeaderLabels;
				for (const DocumentTableRow &row : table.rows) {
					for (const DocumentTableCell &cell : row.cells) {
						if (cell.role == DocumentCellRole::TitleOrBanner
								&& !summary.title_or_banner_text && !cell.text.empty())
							summary.title_or_banner_text = cell.text;
						else if (cell.role == DocumentCellRole::HeaderLabel && !cell.text.empty()
								&& seenHeaderLabels.insert(cell.text).second)
							summary.header_labels.push_back(cell.text);
					}
				}
				plan.table_handles.emplace(tableHandle, std::move(summary));
			}
			std::size_t tableRowIndex = 0;
			std::map<std::string, std::string> visibleLabelsByColumn;
			for (const DocumentTableRow &row : table.rows) {
				for (const DocumentTableCell &cell : row.cells)
					if (cell.role == DocumentCellRole::HeaderLabel && !cell.column_key.empty()
							&& !cell.text.empty()) visibleLabelsByColumn[cell.column_key] = cell.text;
				Json fields = Json::array();
				std::vector<std::string> handles;
				std::vector<std::string> editableHandles;
				for (const DocumentTableCell &cell : row.cells) {
					// DocxExtractor.cpp derives a cell's column_key positionally from
					// the table's own first row - a data row with more cells than
					// that first row has (e.g. a section-title row whose single
					// gridSpan-merged cell spans every column the data rows below it
					// use separately, a common real-document pattern proven live by a
					// design-partner acceptance run's own glossary table) leaves the
					// extra cell(s) with an empty column_key. Such a cell has no
					// resolvable name a lawyer or a model could ever address by, so it
					// can never become a real field: skipping it here also avoids
					// std::string::find("") - which the C++ standard guarantees always
					// "matches" at position 0 - silently marking every such cell
					// editable below regardless of the instruction's real content, and
					// avoids ever constructing a locator resolve_cell_locator() could
					// not itself resolve later (see its own std::count() column check).
					if (cell.column_key.empty()) continue;
					const std::string handle = "F" + std::to_string(fieldIndex++);
					DocumentLocator locator;
					locator.document_id = document.id;
					locator.table_id = table.table_id;
					locator.row_key = row.row_key;
					locator.column_key = cell.column_key;
					plan.field_handles.emplace(handle, locator);
					const auto structuralLabel = visibleLabelsByColumn.find(cell.column_key);
					const std::string visibleLabel = cell.role == DocumentCellRole::HeaderLabel
						? cell.text : structuralLabel == visibleLabelsByColumn.end()
							? cell.column_key : structuralLabel->second;
					plan.field_labels.emplace(handle, visibleLabel);
					plan.field_placeholder_states.emplace(handle, is_template_placeholder(cell.text));
					handles.push_back(handle);
					// CORE PRODUCT LAW: whether THIS field is the right one to
					// change for THIS request is legal/document judgment - the
					// model's job, not Singularity's, so every field is a real,
					// visible structural candidate regardless of what the
					// instruction happens to say. Office Structure Law draws a
					// narrower, different line: Singularity DOES own a cell's FORMAL
					// role (title/banner, metadata/severity, header/label, body -
					// see DocumentCellRole's own comment), derived only from
					// generic OOXML formatting evidence, never from what the text
					// says - and an ordinary drafting action may only ever write
					// to a body cell. A structural label/title/metadata cell is
					// visible (the model needs to read "…" to
					// understand what column-1 means) but is never offered as an
					// editable target through the normal drafting tool, exactly
					// like an existing read-only evidence field already was not.
					const bool editable = cell.role == DocumentCellRole::Body;
					if (editable) editableHandles.push_back(handle);
					fields.push_back({{"handle", handle}, {"label", visibleLabel}, {"value", cell.text},
						{"placeholder", is_template_placeholder(cell.text)},
						{"editable", editable}, {"structuralRole", to_string(cell.role)},
						{"gridSpan", cell.grid_span}});
				}
				if (handles.empty()) continue;
				const std::string itemHandle = "I" + std::to_string(itemIndex++);
				plan.item_field_handles.emplace(itemHandle, std::move(handles));
				plan.item_editable_field_handles.emplace(itemHandle, std::move(editableHandles));
				plan.item_table_handle.emplace(itemHandle, tableHandle);
				plan.item_table_row_index.emplace(itemHandle, tableRowIndex++);
				plan.draft->items.emplace(itemHandle, DocumentDraftItem{});
				if (candidateIndex == 0) defaultTargetItems.push_back({{"handle", itemHandle}, {"fields", std::move(fields)}});
			}
		}

		plan.roles.assign(document.id, DocumentRole::TargetWork);
		if (candidateIndex == 0) {
			plan.target_document_id = document.id;
			plan.target_document_name = display_name_for(document);
			plan.target_fingerprint = document.fingerprint;
			plan.target_version = document.version;
			plan.target_model = std::move(model);
		} else {
			plan.other_candidate_documents.push_back({document.id, display_name_for(document),
				document.fingerprint, document.version, std::move(model)});
		}
	}

	// Keep local structure in the local prompt. The existing outbound guard
	// protects the complete assembled request and owns its one restoration
	// map. Pre-masking individual documents here would orphan their maps and
	// also incorrectly send placeholders to local models.
	const Json defaultTargetJson{{"role", "target_work"}, {"name", plan.target_document_name},
		{"availableAction", "document.draft.upsert"}, {"items", std::move(defaultTargetItems)}};
	routing::memory_diagnostics::register_document_payloads(defaultTargetJson.dump(), "");
	// Capability-first context delivery (CORE PRODUCT LAW, non-deferrable):
	// decided exactly once, here, from the resolved model's real capacity -
	// never reactively mid-task, and never guessed from provider/model name.
	// Every table starts expanded (compact_context_mode stays false, exactly
	// today's pre-existing behavior) whenever the caller could not resolve a
	// real capacity (availableContextWindowTokens unset) or the full
	// structure already fits comfortably within it.
	if (availableContextWindowTokens) {
		// Measure the document's own REAL FULL structural content directly -
		// every real cell of every real table across every real candidate
		// document - never a compiled turn's own size. Before the Working-Set
		// migration, compile_connected_document_turn_context() itself dumped
		// every currently-unblocked item's full content at once, so measuring
		// a "steady state" (every table expanded, every agenda declared) turn
		// was a real, if indirect, proxy for the document's true size (a real
		// corporate-risk-register destination with 91 real structural items,
		// dwarfing every other component, only appeared once its own required-
		// evidence obligation was satisfied). That proxy is no longer valid:
		// compile_connected_document_turn_data()'s own Pass 1 (WorkingSet.hpp's
		// architecture) now ALWAYS narrows any compiled turn to at most one
		// item per real candidate document, regardless of expansion/agenda
		// state - a real chat_execution_tests acceptance fixture proved this
		// concretely: reusing the narrowed turn compiler as this probe made
		// compact_context_mode never activate for ANY document size, silently
		// defeating the very capacity check it exists to make. Measuring the
		// document's own real cell text directly is immune to how narrowly (or
		// widely) any one compiled turn happens to project it - it answers the
		// question this decision actually needs: is even ONE fully-expanded row
		// of this document's own real structure, delivered without paging,
		// something this model's real capacity can ever comfortably hold.
		std::size_t fullContextTokens = 0;
		auto measureModel = [&](const documents::DocumentSemanticModel &model) {
			for (const DocumentTable &table : model.tables)
				for (const DocumentTableRow &row : table.rows)
					for (const DocumentTableCell &cell : row.cells)
						fullContextTokens += research::estimate_tokens(cell.text) + kPerFieldStructuralOverheadTokens;
		};
		measureModel(plan.target_model);
		for (const DocumentActionCandidate &candidate : plan.other_candidate_documents)
			measureModel(candidate.model);
		// Conservative: reserve at least half of the resolved model's real
		// capacity for the system prompt, Project context, conversation
		// history, tool schemas and reserved model output - none of which
		// this function can see or budget for itself (prepare_task_request()
		// in MatterRequestExecution.cpp resolves the model BEFORE any of
		// that is assembled, so a tighter, precise fit is not available at
		// this one up-front plan/no-plan decision point).
		const std::size_t documentMapBudgetTokens = *availableContextWindowTokens / 2;
		if (fullContextTokens > documentMapBudgetTokens) plan.compact_context_mode = true;
	}
	plan.model_instruction = compile_connected_document_turn_context(plan, {});
	return plan;
}

bool has_document_action_result_shape(const std::string &modelResult) {
	bool normalizedFence = false;
	const Json root = parse_document_action_result(modelResult, normalizedFence);
	return root.is_object() && root.size() == 2 && root.contains("answer")
		&& root.at("answer").is_string() && root.contains("documentActions")
		&& root.at("documentActions").is_array() && root.at("documentActions").size() == 1;
}

const providers::GenerationOutputContract &connected_document_action_output_contract() {
	static const Json locator{
		{"type", "object"},
		{"properties", {
			{"tableId", {{"type", "string"}}}, {"rowKey", {{"type", "string"}}},
			{"columnKey", {{"type", "string"}}},
		}},
		{"required", Json::array({"tableId", "rowKey", "columnKey"})},
		{"additionalProperties", false},
	};
	static const Json methodologyEvidence{
		{"type", "object"},
		{"properties", {{"slideId", {{"type", "string"}}}, {"note", {{"type", "string"}}}}},
		{"required", Json::array({"slideId", "note"})},
		{"additionalProperties", false},
	};
	static const Json rationale{
		{"type", "object"},
		{"properties", {
			{"id", {{"type", "string"}, {"minLength", 1}}},
			{"summary", {{"type", "string"}, {"minLength", 1}}},
			{"projectEvidence", {{"type", "array"}, {"items", locator}}},
			{"methodologyEvidence", {{"type", "array"}, {"items", methodologyEvidence}}},
			{"externalEvidenceIds", {{"type", "array"}, {"items", {{"type", "string"}, {"minLength", 1}}}}},
			{"externalSupportStatus", {{"type", "string"},
				{"enum", Json::array({"supported", "not_established", "not_applicable"})}}},
		}},
		{"required", Json::array({"id", "summary", "projectEvidence", "methodologyEvidence",
			"externalEvidenceIds", "externalSupportStatus"})},
		{"additionalProperties", false},
	};
	static const Json operation{
		{"type", "object"},
		{"properties", {
			{"kind", {{"type", "string"}, {"enum", Json::array({"replaceCellText"})}}},
			{"locator", locator}, {"newValue", {{"type", "string"}}},
			{"rationaleId", {{"type", "string"}, {"minLength", 1}}},
			{"renderEvidence", {{"type", "boolean"}}},
		}},
		{"required", Json::array({"kind", "locator", "newValue", "rationaleId", "renderEvidence"})},
		{"additionalProperties", false},
	};
	static const providers::GenerationOutputContract contract{
		"singularity_connected_document_action",
		Json{
			{"type", "object"},
			{"properties", {
				{"answer", {{"type", "string"}}},
				{"documentActions", {{"type", "array"}, {"minItems", 1}, {"maxItems", 1},
					{"items", {{"type", "object"}, {"properties", {
						{"targetDocument", {{"type", "string"}}},
						{"rationales", {{"type", "array"}, {"minItems", 1}, {"items", rationale}}},
						{"operations", {{"type", "array"}, {"minItems", 1}, {"items", operation}}},
					}}, {"required", Json::array({"targetDocument", "rationales", "operations"})},
					{"additionalProperties", false}}}}},
			}},
			{"required", Json::array({"answer", "documentActions"})},
			{"additionalProperties", false},
		},
	};
	return contract;
}

providers::ModelToolDefinition connected_document_draft_model_tool_definition() {
	// Every free-text field below previously had no maxLength/maxItems at
	// all - a genuine contract defect once a phase-aware output reservation
	// needs to derive a real conservative ceiling from "the maximum legal
	// payload shape for the currently permitted action" (see
	// phase_aware_reserved_output_tokens()'s own Drafting/Correction
	// derivation): an unbounded schema has no such maximum to derive, only
	// an arbitrary flat guess. These bounds are generous real professional
	// ceilings, not the theoretical common case - one substantive Body cell
	// finding (newValue), one item-level summary, one methodology citation
	// note - and are enforced backend-side by upsert_connected_document_
	// draft_item() exactly like every other schema bound here already is.
	const Json methodologyEvidence{{"type", "object"}, {"properties", {
		{"handle", {{"type", "string"}}},
		{"note", {{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxDraftMethodologyNoteLength}}}}},
		{"required", Json::array({"handle", "note"})}, {"additionalProperties", false}};
	const Json update{{"type", "object"}, {"properties", {
		{"fieldHandle", {{"type", "string"}}},
		{"newValue", {{"type", "string"}, {"maxLength", kMaxDraftFieldValueLength}}},
		{"renderEvidence", {{"type", "boolean"}}}}},
		{"required", Json::array({"fieldHandle", "newValue", "renderEvidence"})},
		{"additionalProperties", false}};
	return providers::ModelToolDefinition{
		"lm_document_draft_upsert",
		"Validate and retain one item-sized delta in the current backend-owned document draft. "
		"Use compact handles exactly as supplied. A later call for the same item replaces only that item. "
		"The finalized output agenda defines the required drafting scope; once every mapped WRITE field has "
		"an accepted delta, Singularity advances to Professional Review. Set final=true when you believe this is "
		"the last delta, or false otherwise; this hint never overrides structured agenda completion.",
		Json{{"type", "object"}, {"properties", {
			{"itemHandle", {{"type", "string"}}},
			{"summary", {{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxDraftSummaryLength}}},
			{"projectEvidence", {{"type", "array"}, {"minItems", 1},
				{"items", {{"type", "string"}}}}},
			{"methodologyEvidence", {{"type", "array"}, {"items", methodologyEvidence}}},
			{"externalEvidence", {{"type", "array"}, {"items", {{"type", "string"}}}}},
			{"externalSupportStatus", {{"type", "string"},
				{"enum", Json::array({"supported", "not_established", "not_applicable"})}}},
			{"updates", {{"type", "array"}, {"minItems", 1}, {"items", update}}},
			{"final", {{"type", "boolean"}}},
		}}, {"required", Json::array({"itemHandle", "summary", "projectEvidence",
			"methodologyEvidence", "externalEvidence", "externalSupportStatus", "updates", "final"})},
		{"additionalProperties", false}},
		Json{{"properties", {
			{"summary", true},
			{"methodologyEvidence", {{"items", {{"properties", {{"note", true}}}}}}},
			{"updates", {{"items", {{"properties", {{"newValue", true}}}}}}},
		}}},
	};
}

providers::ModelToolDefinition connected_document_research_request_model_tool_definition() {
	return providers::ModelToolDefinition{
		"lm_document_draft_request_research",
		"Request retrieval of one issued research candidate for a pending item when the currently retrieved "
		"evidence cannot support a professional conclusion. Singularity validates the item, source and remaining budget.",
		Json{{"type", "object"}, {"properties", {
			{"itemHandle", {{"type", "string"}}},
			{"sourceHandle", {{"type", "string"}}},
			{"reason", {{"type", "string"}, {"minLength", 1}, {"maxLength", 800}}},
		}}, {"required", Json::array({"itemHandle", "sourceHandle", "reason"})}, {"additionalProperties", false}},
		Json{{"properties", {{"reason", true}}}},
	};
}

providers::ModelToolDefinition connected_document_inspect_model_tool_definition() {
	const Json evidenceObjective{{"type", Json::array({"object", "null"})}, {"properties", {
		{"id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 40}}},
		{"description", {{"type", "string"}, {"minLength", 1}, {"maxLength", 400}}},
		{"coverageMode", {{"type", "string"}, {"enum", Json::array({"exhaustive", "targeted"})}}},
	}}, {"required", Json::array({"id", "description", "coverageMode"})}, {"additionalProperties", false}};
	const Json evidenceObjectiveClosure{{"type", Json::array({"object", "null"})}, {"properties", {
		{"id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 40}}},
		{"closureReason", {{"type", "string"}, {"minLength", 1}, {"maxLength", 400}}},
		{"unresolvedQuestions", {{"type", "array"}, {"items", {{"type", "string"}}}}},
		{"scopeLimitations", {{"type", "array"}, {"items", {{"type", "string"}}}}},
	}}, {"required", Json::array({"id", "closureReason", "unresolvedQuestions", "scopeLimitations"})},
		{"additionalProperties", false}};
	return providers::ModelToolDefinition{
		"lm_document_inspect",
		"Reveal a bounded row window from one collapsed table before drafting against it. Use startRow and "
		"rowCount to page through a large table. When moving away from an already-visible window, preserve "
		"your own semantic judgment as a concise workingEvidenceNote; leave workingEvidenceTables null - Singularity"
		" already knows which exact window is being replaced and binds the note to it automatically. Only "
		"set workingEvidenceTables explicitly to cite additional already-inspected tables beyond that one. "
		"Calling this again on the EXACT window already visible (same tableHandle/startRow/rowCount) is only "
		"rejected when nothing else changes; to update evidenceSufficient, evidenceObjective or "
		"evidenceObjectiveClosure for content already visible, call it again on that same window with the "
		"change - you never need to re-page away and back just to record a judgment about what you can already "
		"see. THE MODEL OWNS WHAT EVIDENCE IS RELEVANT; SINGULARITY OWNS THE COVERAGE STATE OF IT: the first time a table "
		"matters to a specific professional question, set evidenceObjective (a short stable id, your own "
		"description of the question, and coverageMode) to bind that question to this table; repeat the same id "
		"on later calls into the same table to continue it, no need to resupply description/coverageMode. Use "
		"\"exhaustive\" when the question needs completeness, enumeration or negative evidence - identify all "
		"matching items, find every exception, determine what was or was not provided, review a register/list/"
		"inventory in full, conclude something is absent - and \"targeted\" for an ordinary bounded factual read. "
		"An exhaustive objective's document cannot become evidenceSufficient until every row of its own bound "
		"table has been shown to you at least once (Singularity tracks this - you never need to remember which "
		"pages you already read); if you deliberately stop a targeted objective short of full coverage, set "
		"evidenceObjectiveClosure with your own closureReason. Most tables need no evidenceObjective at all. "
		"When this table belongs to a document under required-evidence review, set evidenceSufficient=true only "
		"once you judge your review of that document's own content complete enough to proceed - Singularity never "
		"decides this for you, and never requires reading an entire document, only that you say so explicitly.",
		// OpenAI-family strict function calling requires every property in
		// "properties" to also appear in "required" - a live acceptance run
		// against a real strict-mode endpoint confirmed this concretely (a 400
		// "Missing 'evidenceSufficient'" rejection before the model ever got a
		// chance to answer). Every other property here is genuinely optional
		// from Singularity's own domain perspective (see the handler's own
		// arguments.contains(...) checks, unchanged), so that optionality is
		// expressed the strict-compatible way instead: always present, with
		// "null" added to its own type so the model may still omit it in
		// effect. ResearchToolLoop.hpp's own tool-call normalization strips
		// any such explicit null back to a plain absent key before this
		// definition's handler ever sees it, so every check below stays
		// correct for both a caller that never sends the key at all (every
		// existing test, the LOCAL hard-JSON-Schema path) and one that always
		// sends it with null.
		Json{{"type", "object"}, {"properties", {
			{"tableHandle", {{"type", "string"}}},
			{"startRow", {{"type", Json::array({"integer", "null"})}, {"minimum", 0}}},
			{"rowCount", {{"type", Json::array({"integer", "null"})}, {"minimum", 1}, {"maximum", 50}}},
			{"workingEvidenceNote", {{"type", Json::array({"string", "null"})}, {"maxLength", 2400}}},
			{"workingEvidenceTables", {{"type", Json::array({"array", "null"})}, {"items", {{"type", "string"}}}}},
			{"evidenceSufficient", {{"type", Json::array({"boolean", "null"})}}},
			{"evidenceObjective", evidenceObjective},
			{"evidenceObjectiveClosure", evidenceObjectiveClosure},
		}}, {"required", Json::array({"tableHandle", "startRow", "rowCount", "workingEvidenceNote",
			"workingEvidenceTables", "evidenceSufficient", "evidenceObjective", "evidenceObjectiveClosure"})},
			{"additionalProperties", false}},
		Json{{"properties", {{"tableHandle", true}, {"workingEvidenceNote", true}}}},
	};
}

providers::ModelToolDefinition connected_document_evidence_agenda_model_tool_definition() {
	return providers::ModelToolDefinition{
		"lm_document_evidence_define",
		"Your instruction named more than one Project document. Declare which of them, if any, are required "
		"source evidence you must inspect before drafting - not every named document is source material to "
		"study; one may be the document you are updating, a template, or optional context. List only the "
		"documentHandle(s) of the documents whose real content must be inspected before you draft. Return an "
		"empty list if none of the other named documents require review beyond the one you intend to update.",
		Json{{"type", "object"}, {"properties", {
			{"requiredSourceDocumentHandles", {{"type", "array"}, {"items", {{"type", "string"}}}}},
		}}, {"required", Json::array({"requiredSourceDocumentHandles"})}, {"additionalProperties", false}},
		Json{{"properties", {{"requiredSourceDocumentHandles", true}}}},
	};
}

DocumentDraftToolResult define_connected_document_evidence_agenda(
		const ConnectedDocumentActionPlan &plan, const Json &arguments) {
	if (plan.draft->evidence_agenda_defined)
		return {false, "evidence_agenda_already_defined: the required-evidence agenda is already owned by the runtime."};
	if (!arguments.is_object() || arguments.size() != 1 || !arguments.contains("requiredSourceDocumentHandles")
			|| !arguments.at("requiredSourceDocumentHandles").is_array())
		return {false, "invalid_evidence_agenda: requiredSourceDocumentHandles must be an array."};
	std::vector<RequiredEvidenceObligation> declared;
	std::set<std::string> seenDocumentIds;
	for (const Json &entry : arguments.at("requiredSourceDocumentHandles")) {
		if (!entry.is_string())
			return {false, "invalid_evidence_agenda: every entry must be a document handle string."};
		const std::string handle = entry.get<std::string>();
		// CORE PRODUCT LAW: the model names a real candidate document by the
		// handle Singularity issued for it; Singularity only resolves that handle to the
		// canonical document it already read and structurally proved exists,
		// never guesses or infers relevance.
		const std::optional<std::string> documentId = candidate_document_id_for_handle(plan, handle);
		if (!documentId)
			return {false, "unknown_document: \"" + handle + "\" is not one of this operation's candidate document handles."};
		if (seenDocumentIds.insert(*documentId).second)
			declared.push_back({*documentId, candidate_document_name_for_id(plan, *documentId)});
	}
	plan.draft->required_evidence = std::move(declared);
	plan.draft->evidence_agenda_defined = true;
	++plan.draft->revision;
	return {true, "Required-evidence agenda accepted with "
		+ std::to_string(plan.draft->required_evidence.size()) + " required source document(s)."};
}

std::set<std::string> currently_visible_methodology_handles(const DocumentActionDraft &draft) {
	// A reveal is the model's explicit dependency selection for the current
	// professional drafting objective. Until the domain owns a narrower,
	// source-backed slide-to-objective dependency relation, hiding an already-
	// selected slide behind an arbitrary physical recency count would discard
	// required methodology, not compact optional context. Physical capacity is
	// validated later against the exact prepared request; it must not redefine
	// which selected professional dependencies are present here.
	return draft.revealed_methodology_handles;
}

// Reveals one or more never-before-revealed methodology slides - see
// DocumentActionDraft::revealed_methodology_handles' own comment on why a
// real methodology deck is shown collapsed (handle only) by default, and
// revealed_methodology_recency retains stable audit order only. Leaving the
// current projection never makes a semantically inspected slide unrevealed,
// and re-reveal is deliberately rejected.
providers::ModelToolDefinition connected_document_methodology_reveal_model_tool_definition() {
	return providers::ModelToolDefinition{
		"lm_document_methodology_reveal",
		"Reveal one or more not-yet-inspected methodology slides' own full text before citing them in "
		"methodologyEvidence. \"methodologyHandles\" lists every slide not yet semantically revealed - their own real "
		"slide number is the only clue to what each holds until revealed. A real governed acceptance run proved "
		"revealing an entire real methodology deck one slide per separate call cost many minutes of otherwise "
		"avoidable sequential round trips for content that easily fits one turn - reveal every slide you expect "
		"to need in ONE call by listing them all in \"methodologyHandles\" rather than calling this repeatedly. "
		"Every accepted handle remains durably revealed, model-visible and citable for this operation. Reveal only "
		"slides you judge relevant; Singularity validates the exact prepared request against physical model capacity "
		"rather than silently discarding a selected methodology dependency.",
		Json{{"type", "object"}, {"properties", {
			{"methodologyHandles", {{"type", "array"}, {"minItems", 1}, {"uniqueItems", true},
				{"items", {{"type", "string"}}}}},
		}}, {"required", Json::array({"methodologyHandles"})}, {"additionalProperties", false}},
		Json{{"properties", {{"methodologyHandles", true}}}},
	};
}

DocumentDraftToolResult reveal_connected_document_methodology(
		const ConnectedDocumentActionPlan &plan, const Json &arguments) {
	if (!arguments.is_object() || arguments.size() != 1 || !arguments.contains("methodologyHandles")
			|| !arguments.at("methodologyHandles").is_array() || arguments.at("methodologyHandles").empty())
		return {false, "invalid_arguments: methodologyHandles (a non-empty array of strings) is required."};
	std::vector<std::string> handles;
	std::set<std::string> uniqueHandles;
	for (const Json &entry : arguments.at("methodologyHandles")) {
		if (!entry.is_string())
			return {false, "invalid_arguments: methodologyHandles must be an array of plain string handles."};
		const std::string handle = entry.get<std::string>();
		if (!uniqueHandles.insert(handle).second)
			return {false, "duplicate_methodology_handle: methodologyHandles must contain unique handles."};
		handles.push_back(handle);
	}
	for (const std::string &handle : handles)
		if (plan.methodology_handles.find(handle) == plan.methodology_handles.end())
			return {false, "unknown_methodology_handle: \"" + handle + "\" does not exist."};
	std::vector<std::string> alreadyRevealed;
	for (const std::string &handle : handles)
		if (plan.draft->revealed_methodology_handles.count(handle) != 0)
			alreadyRevealed.push_back(handle);
	if (!alreadyRevealed.empty())
		return {false, "already_revealed: " + join(alreadyRevealed, ", ")
			+ " already semantically revealed for this operation."};
	auto &recency = plan.draft->revealed_methodology_recency;
	for (const std::string &handle : handles) {
		plan.draft->revealed_methodology_handles.insert(handle);
		recency.erase(std::remove(recency.begin(), recency.end(), handle), recency.end());
		recency.push_back(handle);
	}
	++plan.draft->revision;
	return {true, "Methodology slide(s) " + join(handles, ", ")
		+ " semantically revealed for this operation. Verbatim visibility may remain bounded, but reveal state is durable."};
}

providers::ModelToolDefinition connected_document_output_agenda_model_tool_definition() {
	// No "id": a required output's canonical identity is backend-assigned
	// (see define_connected_document_output_agenda()'s own comment) - it
	// exists only for Singularity's own internal bookkeeping and diagnostic
	// messages, never something a later turn must reference back, so making
	// the model invent one bought nothing but a real, observed failure mode
	// (a live acceptance run against openai/gpt-4o-mini reused the same
	// arbitrary label across two real, otherwise-valid selections and lost
	// the whole operation to duplicate_output_target).
	const Json output{{"type", "object"}, {"properties", {
		{"description", {{"type", "string"}, {"minLength", 1}, {"maxLength", 400}}},
		{"itemHandle", {{"type", "string"}}},
		{"fieldHandle", {{"type", "string"}}},
	}}, {"required", Json::array({"description", "itemHandle", "fieldHandle"})},
		{"additionalProperties", false}};
	const Json fieldDisposition{{"type", "object"}, {"properties", {
		{"fieldHandle", {{"type", "string"}}},
		{"disposition", {{"type", "string"},
			{"enum", Json::array({"write", "leave_unchanged"})}}},
		{"reason", {{"type", Json::array({"string", "null"})}, {"maxLength", 400}}},
	}}, {"required", Json::array({"fieldHandle", "disposition", "reason"})},
		{"additionalProperties", false}};
	const Json region{{"type", "object"}, {"properties", {
		{"regionHandle", {{"type", "string"}}},
		{"fieldDispositions", {{"type", "array"}, {"minItems", 1}, {"items", fieldDisposition}}},
	}}, {"required", Json::array({"regionHandle", "fieldDispositions"})},
		{"additionalProperties", false}};
	return providers::ModelToolDefinition{
		"lm_document_outputs_define",
		"A currently visible region is a CANDIDATE, never an implied requirement: select only the region(s) that "
		"are genuinely part of a professional deliverable the user's own request actually asked for, and map each "
		"to its complete typed field binding before drafting - never touch a candidate merely because it happened "
		"to be visible. When the deliverable is a NEW item not already described anywhere in this document (a risk, "
		"a clause, an entry that has no existing row of its own yet), you MUST select a region whose fields report "
		"placeholder=true (an unfilled template slot, e.g. bracketed markers like \"[TBD]\"/\"[●]\") rather than "
		"a region whose fields already hold real, unrelated content - overwriting a populated field with unrelated "
		"text destroys real, already-documented material and is only ever correct when this request is explicitly "
		"about correcting or updating that exact existing item. If no placeholder region fits, say so in your "
		"answer rather than overwriting an unrelated one. Map every distinct professional deliverable to the exact "
		"editable Body field where it belongs, and explicitly mark every other editable field in each selected "
		"region leave_unchanged with a reason. Each region's own fieldDispositions list is scoped to that one "
		"region only: it must contain "
		"exactly that region's own editable fields, one disposition each, and never a field belonging to a "
		"different region or item - a region you did not select needs no entry in \"regions\" at all. Field "
		"labels/headings are structural metadata: never reproduce this field's label or a sibling field's label "
		"inside generated field text. Singularity keeps every write field unresolved until that exact field is "
		"accepted and never infers a destination from prose, and assigns each accepted output's own canonical id "
		"itself - never invent or resupply one. A currently visible candidate you judge is NOT part of this "
		"request belongs in \"skippedRegions\" (its bare regionHandle only, no fieldDispositions) so Singularity never "
		"re-shows it to you again - never select a region just because it is visible, and never silently omit one "
		"either (an omitted-but-not-skipped candidate reappears next turn). Every region you actually name goes in EXACTLY ONE of \"regions\" or \"skippedRegions\", never both: selecting a region in \"regions\" already means you are done considering it, so it must never also appear in \"skippedRegions\" in that same call or any later one - putting a region in \"regions\" is not a two-step process where it also needs marking as skipped once you have written its field dispositions. A large task may need more currently-"
		"visible candidates than fit in one call: submit outputs/regions/skippedRegions for whatever is visible "
		"now with final=false, then call document.inspect to reveal more and repeat - Singularity remembers every "
		"region you already selected or skipped across calls, so only a compact summary of them (never their full "
		"field detail again) stays visible while you keep deciding. Set final=true the moment every deliverable "
		"this request actually concerns already has a selected region across this and every earlier call, "
		"regardless of whether other candidates remain unconsidered - outputs/regions/skippedRegions may all be "
		"empty on that exact call if nothing new remains to decide. The complete agenda becomes immutable at that "
		"point and drafting begins.",
		Json{{"type", "object"}, {"properties", {
			{"outputs", {{"type", "array"}, {"items", output}}},
			{"regions", {{"type", "array"}, {"items", region}}},
			{"skippedRegions", {{"type", "array"}, {"items", {{"type", "string"}}}}},
			{"final", {{"type", "boolean"}}},
		}}, {"required", Json::array({"outputs", "regions", "skippedRegions", "final"})},
			{"additionalProperties", false}},
		Json{{"properties", {
			{"outputs", {{"items", {{"properties", {{"description", true}}}}}}},
			{"regions", {{"items", {{"properties", {{"fieldDispositions", {{"items",
				{{"properties", {{"reason", true}}}}}}}}}}}}},
		}}},
	};
}

DocumentDraftToolResult define_connected_document_output_agenda(
		const ConnectedDocumentActionPlan &plan, const Json &arguments) {
	if (plan.draft->output_agenda_defined)
		return {false, "output_agenda_already_defined: the required outputs are already owned by the runtime."};
	// "final" is a new, optional (for backward compatibility with every
	// caller that predates it - this file's own tests included) third key.
	// Its absence means true: every pre-existing single-call caller keeps
	// today's exact immediately-finalized behavior unchanged. Only an
	// explicit final=false opts a turn into cross-call accumulation - see
	// this function's own tail (the accumulated-append, not replace) and
	// connected_document_output_agenda_model_tool_definition()'s own updated
	// description. A real model always sends it (the schema requires it).
	const bool hasFinalKey = arguments.is_object() && arguments.contains("final") && arguments.at("final").is_boolean();
	const bool isFinal = !hasFinalKey || arguments.at("final").get<bool>();
	// "skippedRegions" is the same kind of new, optional (backward-
	// compatible) key as "final" - a real model always sends it (the schema
	// requires it); a caller that predates it (this file's own pre-existing
	// tests included) simply never skips anything.
	const bool hasSkippedKey = arguments.is_object() && arguments.contains("skippedRegions")
		&& arguments.at("skippedRegions").is_array();
	const bool alreadyAccumulated = !plan.draft->selected_output_region_handles.empty();
	// A call may legally submit nothing new only when it finalizes an agenda
	// that already has at least one region from an earlier call - "I have
	// nothing further to add, close it" - never as the very first call ever
	// (that would finalize an empty, meaningless agenda) and never on a
	// final=false call (which would make no progress at all).
	const bool contentMayBeEmptyThisCall = isFinal && alreadyAccumulated;
	const std::size_t expectedTypedSize = 2 + (hasFinalKey ? 1 : 0) + (hasSkippedKey ? 1 : 0);
	const bool typedBinding = arguments.is_object() && arguments.size() == expectedTypedSize
		&& arguments.contains("outputs") && arguments.at("outputs").is_array()
		&& (contentMayBeEmptyThisCall || !arguments.at("outputs").empty()
			|| (hasSkippedKey && !arguments.at("skippedRegions").empty()))
		&& arguments.contains("regions") && arguments.at("regions").is_array()
		&& (contentMayBeEmptyThisCall || !arguments.at("regions").empty()
			|| (hasSkippedKey && !arguments.at("skippedRegions").empty()));
	// Compatibility is deliberately narrow: a pre-contract caller may still
	// use the old one-property shape only when its selected item has exactly
	// one editable field. There is then no sibling that could disappear from
	// the agenda, so the sole field has one unambiguous implicit WRITE
	// disposition. Multi-field regions must always use the typed binding.
	// Never available alongside an explicit final/skippedRegions key - the
	// legacy shape predates cross-call accumulation entirely.
	const bool legacyOneFieldBinding = !hasFinalKey && !hasSkippedKey && arguments.is_object() && arguments.size() == 1
		&& arguments.contains("outputs") && arguments.at("outputs").is_array()
		&& !arguments.at("outputs").empty();
	if (!typedBinding && !legacyOneFieldBinding)
		return {false, "invalid_output_agenda: non-empty outputs/regions or skippedRegions are required "
			"(outputs/regions may be empty only on a final=true call that already has an earlier selected region)."};
	for (const RequiredEvidenceObligation &obligation : plan.draft->required_evidence)
		if (!document_evidence_satisfied(plan, obligation.document_id))
			return {false, "required_evidence_unresolved: inspect every required document before defining outputs."
				+ describe_incomplete_evidence_coverage(plan, obligation.document_id)};

	std::vector<std::string> selectedRegions;
	std::vector<DocumentFieldDispositionBinding> dispositions;
	// Seeded from every already-accumulated field/region (an earlier, not-
	// yet-finalized final=false call) so this call's own duplicate and
	// cross-document checks see the complete real picture, never just its
	// own delta - a region or field already claimed two calls ago is just as
	// real a duplicate as one claimed in the same call.
	std::set<std::string> dispositionFields;
	for (const DocumentFieldDispositionBinding &binding : plan.draft->field_dispositions)
		dispositionFields.insert(binding.field_handle);
	std::optional<std::string> outputDocumentId;
	if (!plan.draft->required_outputs.empty()) {
		const auto lockedLocator = plan.field_handles.find(plan.draft->required_outputs.front().field_handle);
		if (lockedLocator != plan.field_handles.end()) outputDocumentId = lockedLocator->second.document_id;
	}
	auto addRegion = [&](const std::string &regionHandle, const Json *declaredDispositions)
			-> std::optional<std::string> {
		const auto region = plan.item_field_handles.find(regionHandle);
		const auto editable = plan.item_editable_field_handles.find(regionHandle);
		if (region == plan.item_field_handles.end() || editable == plan.item_editable_field_handles.end()
				|| editable->second.empty() || is_item_collapsed(plan, regionHandle))
			return "invalid_output_region: regionHandle must name one currently issued region with editable Body fields.";
		if (std::find(selectedRegions.begin(), selectedRegions.end(), regionHandle) != selectedRegions.end()
				|| std::find(plan.draft->selected_output_region_handles.begin(),
					plan.draft->selected_output_region_handles.end(), regionHandle)
					!= plan.draft->selected_output_region_handles.end())
			return "duplicate_output_region: a structural region may be selected only once, including in an "
				"earlier document.outputs.define call this same operation already accepted.";
		// The select and skip paths must enforce the SAME "already considered,
		// never re-shown" durability skipRegion() below already enforces in
		// its own direction (a region already selected in an earlier call
		// cannot now be skipped - a same-call conflict is caught by
		// skipRegion()'s own selectedRegions check, since regions is always
		// processed before skippedRegions within one call) - this is the
		// missing reverse direction: a region this operation already
		// explicitly skipped in an earlier accepted call must never become
		// silently selectable later either. A real focused proof run found
		// this gap concretely: the runtime accepted a later call re-selecting
		// a region an earlier final=false call had just explicitly skipped,
		// contradicting this same tool's own model-facing instruction ("Singularity
		// remembers every region you already selected or skipped across
		// calls... never re-shows it to you again"). Reachable only by
		// bypassing the real grammar enum (constrain_output_agenda_
		// capabilities() already excludes an already-considered region from
		// regionHandle's own enum while the agenda is still accumulating) -
		// this is real defense-in-depth, the same layer every sibling check
		// in this function already provides.
		if (is_skipped_output_region(plan, regionHandle))
			return "duplicate_output_region: \"" + regionHandle + "\" was already explicitly skipped, including in "
				"an earlier document.outputs.define call this same operation already accepted, and cannot be "
				"selected now.";
		const auto firstLocator = plan.field_handles.find(editable->second.front());
		if (firstLocator == plan.field_handles.end())
			return "invalid_output_region: an editable field no longer resolves in the selected region.";
		if (!outputDocumentId) outputDocumentId = firstLocator->second.document_id;
		if (*outputDocumentId != firstLocator->second.document_id)
			return "cross_document_output_agenda: one atomic output agenda may target only one Project document.";
		selectedRegions.push_back(regionHandle);
		// The runtime already knows this region's own real editable field set
		// (editable->second) - every rejection below names it explicitly,
		// plus exactly which submitted field handle(s) are the problem, so a
		// bounded correction attempt has a concrete, field-exact fact to act
		// on instead of a bare category name. A live GPT-5.1 acceptance run
		// proved a generic "every disposition must target an editable field
		// in its selected region" message insufficient: the model had
		// listed dispositions for two dozen OTHER regions' own fields
		// alongside this region's real F130/F131, the message never said
		// which ones were wrong or what this region's own real set was, and
		// its one bounded correction attempt repeated the identical
		// out-of-region field list verbatim.
		const std::string ownFieldsList = join(editable->second, ", ");
		if (declaredDispositions == nullptr) {
			if (editable->second.size() != 1)
				return "incomplete_field_dispositions: region " + regionHandle + " has more than one editable "
					"field (" + ownFieldsList + ") - a multi-field region requires an explicit disposition for "
					"every one of them.";
			dispositionFields.insert(editable->second.front());
			dispositions.push_back({regionHandle, editable->second.front(),
				DocumentFieldDisposition::Write, std::nullopt});
			return std::nullopt;
		}
		if (!declaredDispositions->is_array() || declaredDispositions->empty())
			return "invalid_field_disposition: region " + regionHandle + " needs a non-empty fieldDispositions "
				"array covering exactly its own editable fields (" + ownFieldsList + ").";
		std::set<std::string> regionDispositionFields;
		std::vector<std::string> foreignFields;
		for (const Json &entry : *declaredDispositions) {
			if (!entry.is_object() || entry.size() < 2 || entry.size() > 3 || !entry.contains("fieldHandle")
					|| !entry.at("fieldHandle").is_string() || !entry.contains("disposition")
					|| !entry.at("disposition").is_string()
					|| (entry.contains("reason") && !entry.at("reason").is_null()
						&& !entry.at("reason").is_string()))
				return "invalid_field_disposition: every region field needs fieldHandle, disposition and nullable reason.";
			const std::string fieldHandle = entry.at("fieldHandle").get<std::string>();
			const auto disposition = field_disposition_from_string(entry.at("disposition").get<std::string>());
			const std::optional<std::string> reason = entry.contains("reason") && entry.at("reason").is_string()
				? std::optional<std::string>(entry.at("reason").get<std::string>()) : std::nullopt;
			if (!disposition)
				return "invalid_field_disposition: \"" + fieldHandle + "\" has an unrecognized disposition - "
					"use \"write\" or \"leave_unchanged\".";
			if (std::find(editable->second.begin(), editable->second.end(), fieldHandle)
					== editable->second.end()) {
				foreignFields.push_back(fieldHandle);
				continue;
			}
			if (!regionDispositionFields.insert(fieldHandle).second || !dispositionFields.insert(fieldHandle).second)
				return "duplicate_field_disposition: \"" + fieldHandle + "\" already has a disposition in this "
					"agenda.";
			if (reason && reason->size() > 400)
				return "invalid_field_disposition: \"" + fieldHandle + "\"'s own reason exceeds its bounded size.";
			if (*disposition == DocumentFieldDisposition::LeaveUnchanged
					&& (!reason || trim_ascii_space(*reason).empty()))
				return "invalid_field_disposition: \"" + fieldHandle + "\" is leave_unchanged but has no reason.";
			dispositions.push_back({regionHandle, fieldHandle, *disposition, reason});
		}
		if (!foreignFields.empty())
			return "invalid_field_disposition: region " + regionHandle + " only owns these editable fields ("
				+ ownFieldsList + ") - remove the disposition(s) for " + join(foreignFields, ", ")
				+ ", which belong to a different region and must never appear here.";
		const std::set<std::string> expectedFields(editable->second.begin(), editable->second.end());
		if (regionDispositionFields != expectedFields) {
			std::vector<std::string> missing;
			for (const std::string &field : editable->second)
				if (regionDispositionFields.count(field) == 0) missing.push_back(field);
			return "incomplete_field_dispositions: region " + regionHandle + " is missing a disposition for "
				+ join(missing, ", ") + " - every one of its own editable fields (" + ownFieldsList
				+ ") needs exactly one.";
		}
		return std::nullopt;
	};
	// A CANDIDATE the model explicitly judged is not part of this request's
	// own required deliverables - see DocumentActionDraft::
	// skipped_output_region_handles' own comment on why this must be a real,
	// separate, canonical decision rather than silent omission (silence
	// would just re-show the same candidate forever - see is_item_collapsed
	// ()'s own visibility rule, unchanged by this call).
	std::vector<std::string> skippedRegions;
	auto skipRegion = [&](const std::string &regionHandle) -> std::optional<std::string> {
		if (plan.item_field_handles.find(regionHandle) == plan.item_field_handles.end()
				|| is_item_collapsed(plan, regionHandle))
			return "invalid_output_region: skippedRegions must name a currently issued, currently visible region.";
		// A real, reproducible local-model behavior (observed across repeated
		// governed Qwen3-30B-A3B acceptance runs, unchanged even after this
		// tool's own instructions were made maximally explicit that a region
		// belongs in exactly one of "regions"/"skippedRegions") lists a region
		// it just gave complete, real field dispositions to in "regions" a
		// second time in "skippedRegions" - not a genuine contradiction in
		// intent (the model plainly means "I am done deciding this region",
		// which selecting it with real content already establishes), just a
		// redundant restatement. This regionHandle's own real disposition is
		// already unambiguous from addRegion() above; silently treating the
		// redundant skippedRegions entry as a no-op (never adding it to
		// skippedRegions, never erroring) preserves that one real decision
		// instead of rejecting genuine, complete, unambiguous work over a
		// contradiction that was never actually ambiguous.
		if (std::find(selectedRegions.begin(), selectedRegions.end(), regionHandle) != selectedRegions.end())
			return std::nullopt;
		if (std::find(skippedRegions.begin(), skippedRegions.end(), regionHandle) != skippedRegions.end()
				|| is_output_region_considered(plan, regionHandle))
			return "duplicate_output_region: \"" + regionHandle
				+ "\" was already considered (selected or skipped), including in an earlier call.";
		skippedRegions.push_back(regionHandle);
		return std::nullopt;
	};

	if (typedBinding) {
		for (const Json &entry : arguments.at("regions")) {
			if (!entry.is_object() || entry.size() != 2 || !entry.contains("regionHandle")
					|| !entry.at("regionHandle").is_string() || !entry.contains("fieldDispositions"))
				return {false, "invalid_output_region: every selected region needs regionHandle and fieldDispositions."};
			if (const auto error = addRegion(entry.at("regionHandle").get<std::string>(),
					&entry.at("fieldDispositions"))) return {false, *error};
		}
		if (hasSkippedKey) {
			for (const Json &entry : arguments.at("skippedRegions")) {
				if (!entry.is_string())
					return {false, "invalid_output_region: skippedRegions must be an array of region handles."};
				if (const auto error = skipRegion(entry.get<std::string>())) return {false, *error};
			}
		}
	}

	std::vector<RequiredOutputObligation> outputs;
	// Seeded from every already-accumulated output (an earlier final=false
	// call) for the same cross-call-real-duplicate reason dispositionFields
	// is seeded above.
	std::set<std::string> ids;
	std::set<std::string> fields;
	for (const RequiredOutputObligation &existing : plan.draft->required_outputs) {
		ids.insert(existing.id);
		fields.insert(existing.field_handle);
	}
	for (const Json &entry : arguments.at("outputs")) {
		// "id" is no longer part of the real model-facing schema (see
		// connected_document_output_agenda_model_tool_definition()'s own
		// comment: a live acceptance run against openai/gpt-4o-mini lost an
		// otherwise-valid cross-call selection to a reused arbitrary label,
		// and nothing downstream ever needs to reference this id back - see
		// connected_document_draft_completion_issue()'s own use of it, which
		// is diagnostic-message-only). Still accepted when a caller supplies
		// it (this file's own pre-existing tests), for exact backward
		// compatibility; generated from the real itemHandle/fieldHandle
		// otherwise - already guaranteed unique by the fieldHandle
		// uniqueness check below, so this can never collide either.
		const bool hasId = entry.is_object() && entry.contains("id") && entry.at("id").is_string();
		const std::size_t expectedOutputSize = hasId ? 4 : 3;
		if (!entry.is_object() || entry.size() != expectedOutputSize
				|| (hasId && entry.at("id").get<std::string>().empty()) || !entry.contains("description")
				|| !entry.at("description").is_string() || entry.at("description").get<std::string>().empty()
				|| !entry.contains("itemHandle") || !entry.at("itemHandle").is_string()
				|| !entry.contains("fieldHandle") || !entry.at("fieldHandle").is_string())
			return {false, "invalid_output_agenda: every output needs description, itemHandle and fieldHandle."};
		const std::string itemHandle = entry.at("itemHandle").get<std::string>();
		const std::string fieldHandle = entry.at("fieldHandle").get<std::string>();
		const std::string id = hasId ? entry.at("id").get<std::string>() : (itemHandle + "-" + fieldHandle);
		const std::string description = entry.at("description").get<std::string>();
		if (id.size() > 80 || description.size() > 400)
			return {false, "invalid_output_agenda: output ids or descriptions exceed their bounded size."};
		if (!typedBinding && std::find(selectedRegions.begin(), selectedRegions.end(), itemHandle)
				== selectedRegions.end())
			if (const auto error = addRegion(itemHandle, nullptr)) return {false, *error};
		const auto editable = plan.item_editable_field_handles.find(itemHandle);
		const auto locator = plan.field_handles.find(fieldHandle);
		if (std::find(selectedRegions.begin(), selectedRegions.end(), itemHandle) == selectedRegions.end()
				|| editable == plan.item_editable_field_handles.end() || locator == plan.field_handles.end()
				|| std::find(editable->second.begin(), editable->second.end(), fieldHandle) == editable->second.end()
				|| is_item_collapsed(plan, itemHandle))
			return {false, "invalid_output_target: every required output must map to a currently issued editable Body field."};
		if (!ids.insert(id).second || !fields.insert(fieldHandle).second)
			return {false, "duplicate_output_target: output ids and Body fields must be unique."};
		if (!outputDocumentId) outputDocumentId = locator->second.document_id;
		if (*outputDocumentId != locator->second.document_id)
			return {false, "cross_document_output_agenda: one atomic output agenda may target only one Project document."};
		outputs.push_back({id, description, itemHandle, fieldHandle});
	}

	for (const DocumentFieldDispositionBinding &binding : dispositions) {
		const bool mappedForWrite = fields.count(binding.field_handle) != 0;
		if (binding.disposition == DocumentFieldDisposition::Write && !mappedForWrite)
			return {false, "unmapped_write_disposition: every field marked write needs one typed output."};
		if (binding.disposition == DocumentFieldDisposition::LeaveUnchanged && mappedForWrite)
			return {false, "conflicting_field_disposition: a leave_unchanged field cannot also be a required output."};
	}
	// A validated-but-still-rejectable proposal must leave the previously
	// accepted canonical agenda completely unchanged - this exact check used
	// to run AFTER the commits below, meaning a real final=true call that
	// skipped real regions but selected none (leaving the agenda genuinely
	// incomplete) still durably recorded those skips into plan.draft->
	// skipped_output_region_handles before being rejected, permanently
	// foreclosing those regions from ever being reconsidered on the model's
	// very next, otherwise-unrelated retry. Computed here from the exact
	// same real condition output_agenda_finalizable() checks post-commit
	// (would plan.draft->selected_output_region_handles end up empty), just
	// evaluated against this call's own still-local candidate delta first -
	// a plain vector insert can never turn "both empty" into "non-empty", so
	// this is the identical real predicate, not a second one.
	if (isFinal && plan.draft->selected_output_region_handles.empty() && selectedRegions.empty())
		return {false, "empty_output_agenda: at least one region must be selected, across this and any earlier "
			"call, before the agenda may be finalized."};
	// Append, never replace: a real production accumulation call (final=
	// false) must never lose an earlier, already-canonical selection just
	// because a later call only named its own new region(s) - the runtime
	// owns the complete cross-call agenda, not any one call's own delta. A
	// caller that never sends final (isFinal stays true on its own single
	// call) still appends onto an empty starting vector, so this is exactly
	// today's replace-from-empty behavior for every pre-existing caller.
	plan.draft->required_outputs.insert(plan.draft->required_outputs.end(),
		std::make_move_iterator(outputs.begin()), std::make_move_iterator(outputs.end()));
	plan.draft->selected_output_region_handles.insert(plan.draft->selected_output_region_handles.end(),
		std::make_move_iterator(selectedRegions.begin()), std::make_move_iterator(selectedRegions.end()));
	plan.draft->field_dispositions.insert(plan.draft->field_dispositions.end(),
		std::make_move_iterator(dispositions.begin()), std::make_move_iterator(dispositions.end()));
	plan.draft->skipped_output_region_handles.insert(plan.draft->skipped_output_region_handles.end(),
		std::make_move_iterator(skippedRegions.begin()), std::make_move_iterator(skippedRegions.end()));
	plan.draft->output_agenda_defined = isFinal;
	plan.draft->finished = false;
	++plan.draft->revision;
	if (!isFinal)
		return {true, "Selected " + std::to_string(plan.draft->selected_output_region_handles.size())
			+ " region(s) and skipped " + std::to_string(plan.draft->skipped_output_region_handles.size())
			+ " so far, not yet finalized. Call document.inspect to reveal more, or document.outputs."
			"define again with final=true once every deliverable this request concerns already has a region."};
	return {true, "Typed output binding accepted for "
		+ std::to_string(plan.draft->selected_output_region_handles.size()) + " region(s) with "
		+ std::to_string(plan.draft->required_outputs.size()) + " WRITE field(s) and "
		+ std::to_string(plan.draft->field_dispositions.size() - plan.draft->required_outputs.size())
		+ " LEAVE_UNCHANGED field(s)."};
}

DocumentDraftToolResult upsert_connected_document_draft_item(
		const ConnectedDocumentActionPlan &plan, const Json &arguments,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence) {
	std::optional<std::string> proposedItemHandle;
	if (arguments.is_object() && arguments.contains("itemHandle")
			&& arguments.at("itemHandle").is_string()) {
		proposedItemHandle = arguments.at("itemHandle").get<std::string>();
		const auto known = plan.draft->items.find(*proposedItemHandle);
		if (known != plan.draft->items.end()) {
			++known->second.proposal_attempts;
			++plan.draft->revision;
		}
	}
	auto reject = [&](const std::string &category, const std::string &message) {
		if (proposedItemHandle) {
			const auto known = plan.draft->items.find(*proposedItemHandle);
			if (known != plan.draft->items.end()) {
				// A bad later proposal never destroys a previously accepted item.
				if (known->second.state != DocumentDraftItemState::Accepted)
					known->second.state = DocumentDraftItemState::NeedsCorrection;
				known->second.last_issue_category = category;
				known->second.last_issue_message = message;
			}
		}
		return DocumentDraftToolResult{false, category + ": " + message};
	};
	try {
		if (!plan.draft->output_agenda_defined)
			throw std::runtime_error(
				"The required output agenda must be defined before any document draft delta is accepted.");
		if (!arguments.is_object() || arguments.size() != 8
				|| !arguments.contains("itemHandle") || !arguments.at("itemHandle").is_string()
				|| !arguments.contains("summary") || !arguments.at("summary").is_string()
				|| arguments.at("summary").get<std::string>().empty()
				|| utf8_character_count(arguments.at("summary").get<std::string>()) > kMaxDraftSummaryLength
				|| !arguments.contains("projectEvidence") || !arguments.at("projectEvidence").is_array()
				|| arguments.at("projectEvidence").empty()
				|| !arguments.contains("methodologyEvidence") || !arguments.at("methodologyEvidence").is_array()
				|| !arguments.contains("externalEvidence") || !arguments.at("externalEvidence").is_array()
				|| !arguments.contains("externalSupportStatus") || !arguments.at("externalSupportStatus").is_string()
				|| !arguments.contains("updates") || !arguments.at("updates").is_array()
				|| arguments.at("updates").empty()
				|| !arguments.contains("final") || !arguments.at("final").is_boolean())
			throw std::runtime_error("The document draft delta is malformed.");
		const std::string itemHandle = arguments.at("itemHandle").get<std::string>();
		const auto item = plan.item_field_handles.find(itemHandle);
		if (item == plan.item_field_handles.end())
			throw std::runtime_error("The document draft item handle does not resolve in this operation.");
		if (!is_selected_output_region(plan, itemHandle))
			throw std::runtime_error(
				"A document draft delta may target only a region selected by the typed output binding.");
		// Capability-first context delivery: TurnContract.hpp's constrain_
		// draft_capabilities() already omits a collapsed (or not-yet-
		// draftable - see document_grounding_complete_for_drafting()) item's
		// handle from the schema/instruction text the model actually sees,
		// but a handle scheme this stable is guessable - Singularity itself, not
		// just prompt shaping, must fail closed against one submitted anyway
		// (Office Structure Law's own fail-closed principle applied to
		// withheld state, not only to structural role). Visibility
		// (is_item_collapsed) and draftability (document_grounding_complete_
		// for_drafting) are deliberately two independent checks - a required
		// document's own rows may be visible for review before every
		// required document is grounded, but drafting into ANY candidate
		// document still waits for all of them, regardless of which one.
		if (is_item_collapsed(plan, itemHandle))
			throw std::runtime_error(
				"The document draft item belongs to a table that has not been revealed via document.inspect.");
		if (!document_grounding_complete_for_drafting(plan))
			throw std::runtime_error(
				"Every document you declared required evidence must be inspected and explicitly marked "
				"evidenceSufficient before drafting into any candidate document.");
		if (!item_document_is_draft_target(plan, itemHandle))
			throw std::runtime_error(
				"This operation already committed to a different Project document with its first accepted "
				"item - one atomic action may only ever draft into one real document; submit further items "
				"for that same document only.");
		const auto currentDraftItem = plan.draft->items.find(itemHandle);
		if (currentDraftItem == plan.draft->items.end()
				|| currentDraftItem->second.state == DocumentDraftItemState::Accepted)
			throw std::runtime_error("The document draft item is not pending in the current turn contract.");
		if (currentDraftItem->second.state == DocumentDraftItemState::NeedsResearch)
			throw std::runtime_error("The document draft item still requires a completed research transition.");
		const std::set<std::string> itemFields(item->second.begin(), item->second.end());
		const auto editable = plan.item_editable_field_handles.find(itemHandle);
		const std::set<std::string> editableFields = editable == plan.item_editable_field_handles.end()
			? std::set<std::string>() : std::set<std::string>(editable->second.begin(), editable->second.end());
		std::set<std::string> expectedWriteFields;
		for (const std::string &fieldHandle : editableFields) {
			const auto disposition = effective_field_disposition(plan, fieldHandle);
			if (!disposition)
				throw std::runtime_error(
					"An editable field in the selected region has no explicit disposition.");
			if (*disposition == DocumentFieldDisposition::Write) expectedWriteFields.insert(fieldHandle);
		}
		if (expectedWriteFields.empty())
			throw std::runtime_error("The selected region has no field marked WRITE.");

		Json projectEvidence = Json::array();
		for (const Json &entry : arguments.at("projectEvidence")) {
			if (!entry.is_string() || itemFields.count(entry.get<std::string>()) == 0)
				throw std::runtime_error("Project evidence must use a field handle from the same draft item.");
			projectEvidence.push_back(model_cell_locator_json(plan.field_handles.at(entry.get<std::string>())));
		}
		Json methodologyEvidence = Json::array();
		for (const Json &entry : arguments.at("methodologyEvidence")) {
			if (!entry.is_object() || entry.size() != 2 || !entry.contains("handle")
					|| !entry.at("handle").is_string() || !entry.contains("note")
					|| !entry.at("note").is_string() || entry.at("note").get<std::string>().empty()
					|| utf8_character_count(entry.at("note").get<std::string>()) > kMaxDraftMethodologyNoteLength)
				throw std::runtime_error("A methodology handle reference is malformed.");
			const auto locator = plan.methodology_handles.find(entry.at("handle").get<std::string>());
			if (locator == plan.methodology_handles.end() || !locator->second.slide_id)
				throw std::runtime_error("A methodology handle does not resolve in this operation.");
			// Backend owns this, never only the model-visible schema enum
			// (TurnContract.hpp's own constrain_draft_capabilities() already
			// restricts the enum to semantically revealed handles, but a non-
			// strict fallback caller or a malformed response must not be
			// able to bypass that by construction): under compact_context_
			// mode a never-revealed slide's content was never semantically
			// inspected. Verbatim projection is deliberately a separate,
			// bounded concern and its eviction cannot erase the durable reveal
			// decision or create a reveal cycle.
			if (plan.compact_context_mode && plan.draft->revealed_methodology_handles.count(
					entry.at("handle").get<std::string>()) == 0)
				throw std::runtime_error("A methodology handle citation references a slide that was never revealed.");
			methodologyEvidence.push_back({{"slideId", *locator->second.slide_id},
				{"note", entry.at("note").get<std::string>()}});
		}
		Json externalIds = Json::array();
		for (const Json &entry : arguments.at("externalEvidence")) {
			if (!entry.is_string()) throw std::runtime_error("An external evidence alias is malformed.");
			externalIds.push_back(resolve_external_alias(
				entry.get<std::string>(), availableExternalEvidence).evidence_id);
		}
		Json operations = Json::array();
		std::set<std::string> submittedWriteFields;
		for (const Json &entry : arguments.at("updates")) {
			if (!entry.is_object() || entry.size() != 3 || !entry.contains("fieldHandle")
					|| !entry.at("fieldHandle").is_string() || !entry.contains("newValue")
					|| !entry.at("newValue").is_string() || !entry.contains("renderEvidence")
					|| !entry.at("renderEvidence").is_boolean()
					|| utf8_character_count(entry.at("newValue").get<std::string>()) > kMaxDraftFieldValueLength)
				throw std::runtime_error("A document draft update is malformed.");
			const std::string fieldHandle = entry.at("fieldHandle").get<std::string>();
			if (itemFields.count(fieldHandle) == 0)
				throw std::runtime_error("A document draft update targeted a field outside its item.");
			if (editableFields.count(fieldHandle) == 0)
				throw std::runtime_error("A document draft update targeted a read-only evidence field.");
			const auto disposition = effective_field_disposition(plan, fieldHandle);
			if (!disposition || *disposition != DocumentFieldDisposition::Write)
				throw std::runtime_error(
					"A document draft update targeted a field not marked WRITE by the output binding.");
			if (!submittedWriteFields.insert(fieldHandle).second)
				throw std::runtime_error(
					"A document draft delta writes the same field more than once.");
			const std::string newValue = entry.at("newValue").get<std::string>();
			if (trim_ascii_space(newValue).empty() || is_template_placeholder(newValue))
				throw std::runtime_error("A WRITE field requires a substantive accepted value.");
			if (proposed_value_reproduces_region_label(plan, itemHandle, newValue))
				throw std::runtime_error(
					"Field labels/headings are structural metadata. Do not reproduce this field's label or a sibling field's label inside field text.");
			operations.push_back({{"kind", "replaceCellText"},
				{"locator", model_cell_locator_json(plan.field_handles.at(fieldHandle))},
				{"newValue", entry.at("newValue")}, {"rationaleId", itemHandle},
				{"renderEvidence", entry.at("renderEvidence")}});
		}
		if (submittedWriteFields != expectedWriteFields)
			throw std::runtime_error(
				"The item-sized delta must supply exactly one value for every field marked WRITE in this selected region.");
		const Json rationale{{"id", itemHandle}, {"summary", arguments.at("summary")},
			{"projectEvidence", std::move(projectEvidence)},
			{"methodologyEvidence", std::move(methodologyEvidence)},
			{"externalEvidenceIds", std::move(externalIds)},
			{"externalSupportStatus", arguments.at("externalSupportStatus")}};
		// This item's fields were already resolved (see plan.field_handles
		// above) against whichever candidate document they really belong to -
		// itemFields is never empty here (arguments.at("updates") required
		// minItems:1 and every fieldHandle was just proven to be in itemFields).
		const std::string itemDocumentId = plan.field_handles.at(*itemFields.begin()).document_id;
		const Json itemAction{{"targetDocument", candidate_document_name_for_id(plan, itemDocumentId)},
			{"rationales", Json::array({rationale})}, {"operations", std::move(operations)}};
		const std::string candidate = Json{{"answer", "draft"},
			{"documentActions", Json::array({itemAction})}}.dump();
		(void)resolve_document_action_proposal(plan, candidate, availableExternalEvidence);
		DocumentDraftItem &draftItem = plan.draft->items.at(itemHandle);
		draftItem.accepted_action = itemAction;
		draftItem.state = DocumentDraftItemState::Accepted;
		draftItem.last_issue_category.reset();
		draftItem.last_issue_message.reset();
		// Keep explicit final=true for wire/checkpoint compatibility. Structured
		// agenda satisfaction is authoritative, so derive the same persisted
		// signal once this accepted delta satisfies the finalized agenda rather
		// than requiring a semantically empty closing generation.
		if (arguments.at("final").get<bool>() || all_required_outputs_satisfied(plan))
			plan.draft->finished = true;
		return {true, "Accepted " + itemHandle + ". Draft progress: "
			+ std::to_string(accepted_draft_item_count(plan)) + "/"
			+ std::to_string(plan.item_field_handles.size()) + " items. "
			+ (plan.draft->finished ? "Marked final: no further item will be requested."
				: "final=false: still open for further items if this request concerns any.")};
	} catch (const ResearchCoverageError &error) {
		return reject("research_coverage", error.what());
	} catch (const std::exception &error) {
		const std::string message = error.what();
		const std::string category = message.find("numeric comparison") != std::string::npos
			? "quantitative_consistency" : message.find("evidence") != std::string::npos
				|| message.find("support") != std::string::npos ? "evidence_provenance" : "draft_validation";
		return reject(category, message);
	}
}

DocumentDraftToolResult request_connected_document_draft_research(
		const ConnectedDocumentActionPlan &plan, const Json &arguments,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
		const research::OperationExecutionProgress &progress) {
	if (!arguments.is_object() || arguments.size() != 3
			|| !arguments.contains("itemHandle") || !arguments.at("itemHandle").is_string()
			|| !arguments.contains("sourceHandle") || !arguments.at("sourceHandle").is_string()
			|| !arguments.contains("reason") || !arguments.at("reason").is_string())
		return {false, "research_transition: The research request is malformed."};
	const std::string handle = arguments.at("itemHandle").get<std::string>();
	const std::string sourceHandle = arguments.at("sourceHandle").get<std::string>();
	const std::string reason = arguments.at("reason").get<std::string>();
	const auto item = plan.draft->items.find(handle);
	if (item == plan.draft->items.end())
		return {false, "research_transition: The item handle was not issued by this operation."};
	if (item->second.state == DocumentDraftItemState::Accepted)
		return {false, "research_transition: An accepted item is sticky and cannot return to research."};
	if (item->second.state == DocumentDraftItemState::NeedsResearch)
		return {false, "research_transition: Research is already pending for this item."};
	if (reason.empty() || reason.size() > 800)
		return {false, "research_transition: Give a concise substantive evidence gap."};
	const bool anotherPending = std::any_of(plan.draft->items.begin(), plan.draft->items.end(),
		[&](const auto &entry) { return entry.first != handle
			&& entry.second.state == DocumentDraftItemState::NeedsResearch; });
	if (anotherPending)
		return {false, "research_transition: Complete the existing item research transition first."};
	research::ExternalEvidenceReference source;
	try {
		source = resolve_external_alias(sourceHandle, availableExternalEvidence);
	} catch (const std::exception &) {
		return {false, "research_transition: The research candidate handle was not issued by this operation."};
	}
	if (source.content_kind != research::ExternalEvidenceContentKind::SearchResult)
		return {false, "research_transition: The selected source is already retrieved and cannot be requested again."};
	item->second.state = DocumentDraftItemState::NeedsResearch;
	item->second.last_issue_category = "model_requested_research";
	item->second.last_issue_message = reason;
	item->second.research_search_calls_at_request = progress.search_calls_used;
	item->second.research_open_calls_at_request = progress.open_calls_used;
	item->second.requested_research_evidence_id = source.evidence_id;
	++plan.draft->revision;
	return {true, "Accepted a research transition for " + handle + "."};
}

void complete_connected_document_draft_research(
		const ConnectedDocumentActionPlan &plan,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
		const research::OperationExecutionProgress &progress) {
	// Every successfully executed research action changes the canonical
	// evidence capability set, even when the draft item itself does not move.
	// Advance the existing operation revision before another TurnContract can
	// be compiled; no evidence-derived second revision authority is needed.
	++plan.draft->revision;
	for (auto &[handle, item] : plan.draft->items) {
		(void)handle;
		if (item.state != DocumentDraftItemState::NeedsResearch)
			continue;
		bool selectedSourceRetrieved = false;
		if (item.requested_research_evidence_id) {
			const auto source = std::find_if(availableExternalEvidence.begin(), availableExternalEvidence.end(),
				[&](const auto &entry) { return entry.evidence_id == *item.requested_research_evidence_id; });
			selectedSourceRetrieved = source != availableExternalEvidence.end()
				&& source->content_kind == research::ExternalEvidenceContentKind::RetrievedContent;
		} else {
			selectedSourceRetrieved = progress.search_calls_used > item.research_search_calls_at_request
				&& progress.open_calls_used > item.research_open_calls_at_request;
		}
		if (!selectedSourceRetrieved) continue;
		item.state = DocumentDraftItemState::NeedsCorrection;
		item.last_issue_category = "research_completed";
		item.last_issue_message = "New discovery and retrieved source evidence are available. Correct only this item.";
		item.requested_research_evidence_id.reset();
	}
}

// THE MODEL OWNS PROFESSIONAL JUDGMENT, SINGULARITY OWNS STRUCTURE: an
// evidenceObjective.id is a pure backend bookkeeping handle (the same class
// as a tableHandle or itemHandle), never a professional conclusion, so an
// unsafe model-supplied value is never worth failing an operation over. A
// live forensic acceptance run proved the concrete failure mode: the model
// echoed a Papinian-masked placeholder ("[[E74]]") back as its own id after
// copying it verbatim out of this handler's own "already bound to X" text.
// An id is unsafe exactly when it cannot be trusted as the model's own
// stable choice: a masked placeholder, or literally reusing a real
// tableHandle/itemHandle from this same operation (never an id the model
// coined itself).
bool evidence_objective_id_looks_unsafe(const ConnectedDocumentActionPlan &plan, const std::string &id) {
	return id.find("[[") != std::string::npos
		|| plan.table_handles.count(id) != 0
		|| plan.item_field_handles.count(id) != 0;
}

std::string backend_owned_evidence_objective_id(
		const ConnectedDocumentActionPlan &plan, const std::string &tableHandle) {
	for (std::size_t suffix = 1;; ++suffix) {
		const std::string candidate = "obj_" + tableHandle + "_" + std::to_string(suffix);
		if (plan.draft->evidence_objectives.find(candidate) == plan.draft->evidence_objectives.end())
			return candidate;
	}
}

// ResearchToolLoop.hpp's own inspectCorrectionKey keys the repeated-rejection
// detector on tableHandle plus this rejection's own message category (the
// text before its first colon) alone - it never inspects arguments, so a
// materially different repair attempt that still fails for a reason sharing
// the same message prefix looks identical to an exact repeat, and a real
// live run failed an operation closed on exactly this ("T1:working_evidence_
// required" repeated twice). This encodes REPAIR PROGRESS - the requested
// window plus the shape of every argument this validation actually reasons
// about - as a compact, non-model-authored, non-sensitive digest appended to
// the category, so genuinely different repair states always produce
// genuinely different keys, without hand-enumerating a new message string
// per combination. Deliberately never includes raw note text or any
// Papinian-protectable content - only boolean/enum shape flags and row
// numbers, all of which are Singularity's own structural bookkeeping already.
std::string working_evidence_repair_signature(
		const std::string &tableHandle, std::size_t rowStart, std::size_t rowCount,
		bool noteSupplied, bool noteTooLong, const Json &arguments,
		bool evidenceSufficientArg, bool hasObjectiveArg, bool hasClosureArg) {
	const bool tablesExplicit = arguments.contains("workingEvidenceTables")
		&& !arguments.at("workingEvidenceTables").is_null();
	const bool tablesShapeValid = !tablesExplicit || arguments.at("workingEvidenceTables").is_array();
	std::string signature = tableHandle + "_r" + std::to_string(rowStart) + "c" + std::to_string(rowCount);
	signature += !noteSupplied ? "_noteAbsent" : (noteTooLong ? "_noteTooLong" : "_noteValid");
	signature += !tablesExplicit ? "_tablesAbsent" : (tablesShapeValid ? "_tablesValid" : "_tablesInvalid");
	if (evidenceSufficientArg) signature += "_sufficient";
	// Real product bug: this signature previously recorded only THAT an
	// evidenceObjective/evidenceObjectiveClosure was proposed, never WHICH
	// one - two genuinely different objective/closure proposals at the same
	// table/row window, both still missing a working evidence note, produced
	// the identical signature and were wrongly treated as the model
	// repeating itself (the exact class of defect this function's own header
	// comment and item17 already document, here for a dimension that one fix
	// did not cover). The objective/closure's own "id" is a short
	// model-authored structural handle, not narrative content - the same
	// kind of identifier callers already pass around (see
	// ConnectedDocumentAction.test-style objective ids) - so including it
	// keeps this signature's own "never raw note text, never sensitive
	// content" contract intact. ":" is never used here (even though the id
	// itself never contains one - see evidence_objective_id_looks_unsafe's
	// own "[[" check elsewhere, not a colon check): ResearchToolLoop.hpp's
	// inspectCorrectionKey extracts this whole signature's OWN category by
	// splitting the surrounding rejection message on its FIRST ':' (the
	// category: description separator) - an earlier colon inside the
	// signature itself would truncate the category right there, silently
	// discarding everything meant to disambiguate it. "=" cannot collide
	// with that separator.
	if (hasObjectiveArg) {
		signature += "_objective";
		const Json &objective = arguments.at("evidenceObjective");
		if (objective.is_object() && objective.contains("id") && objective.at("id").is_string())
			signature += "=" + objective.at("id").get<std::string>();
	}
	if (hasClosureArg) {
		signature += "_closure";
		const Json &closure = arguments.at("evidenceObjectiveClosure");
		if (closure.is_object() && closure.contains("id") && closure.at("id").is_string())
			signature += "=" + closure.at("id").get<std::string>();
	}
	return signature;
}

DocumentDraftToolResult request_connected_document_table_inspection(
		const ConnectedDocumentActionPlan &plan, const Json &arguments) {
	if (!arguments.is_object() || arguments.size() > 8
			|| !arguments.contains("tableHandle") || !arguments.at("tableHandle").is_string())
		return {false, "invalid_arguments: tableHandle (string) is required."};
	const std::string tableHandle = arguments.at("tableHandle").get<std::string>();
	const auto table = plan.table_handles.find(tableHandle);
	if (table == plan.table_handles.end())
		return {false, "unknown_table: that table handle does not exist."};
	if (arguments.contains("evidenceSufficient") && !arguments.at("evidenceSufficient").is_boolean())
		return {false, "invalid_arguments: evidenceSufficient must be a boolean."};
	if (arguments.contains("startRow") && !arguments.at("startRow").is_number_unsigned()
			&& !arguments.at("startRow").is_number_integer())
		return {false, "invalid_arguments: startRow must be a non-negative integer."};
	if (arguments.contains("rowCount") && !arguments.at("rowCount").is_number_unsigned()
			&& !arguments.at("rowCount").is_number_integer())
		return {false, "invalid_arguments: rowCount must be a positive integer."};
	// THE MODEL OWNS WHAT EVIDENCE IS RELEVANT: validated here (shape only -
	// binding it to this table happens below, once tableHandle's own identity
	// is already confirmed real). Absent unless the model is establishing or
	// continuing a specific coverage question on this exact table.
	if (arguments.contains("evidenceObjective") && !arguments.at("evidenceObjective").is_null()) {
		const Json &objectiveArgs = arguments.at("evidenceObjective");
		if (!objectiveArgs.is_object() || objectiveArgs.value("id", std::string()).empty()
				|| objectiveArgs.value("description", std::string()).empty()
				|| (objectiveArgs.value("coverageMode", std::string()) != "exhaustive"
					&& objectiveArgs.value("coverageMode", std::string()) != "targeted"))
			return {false, "invalid_evidence_objective: id, description and coverageMode "
				"(\"exhaustive\" or \"targeted\") are all required."};
		// An unsafe id (masked placeholder, or a reused tableHandle/itemHandle)
		// is never worth failing this call over - see evidence_objective_id_
		// looks_unsafe()'s own comment and the auto-resolution below, which
		// replaces it with the table's real bound id or a fresh backend-owned
		// one instead of rejecting outright.
	}
	if (arguments.contains("evidenceObjectiveClosure") && !arguments.at("evidenceObjectiveClosure").is_null()) {
		const Json &closureArgs = arguments.at("evidenceObjectiveClosure");
		if (!closureArgs.is_object() || closureArgs.value("id", std::string()).empty()
				|| trim_ascii_space(closureArgs.value("closureReason", std::string())).empty()
				|| !closureArgs.value("unresolvedQuestions", Json::array()).is_array()
				|| !closureArgs.value("scopeLimitations", Json::array()).is_array())
			return {false, "invalid_evidence_objective_closure: id and a non-empty closureReason are required, "
				"with unresolvedQuestions/scopeLimitations arrays."};
		// Same masked-placeholder-leak guard as evidenceObjective.id above.
		if (closureArgs.value("id", std::string()).find("[[") != std::string::npos)
			return {false, "invalid_evidence_objective_closure: id looks like a masked placeholder token, not "
				"a real id - use the exact plain id you originally declared for this objective."};
	}
	const long long proposedStart = arguments.value("startRow", 0LL);
	const long long proposedCount = arguments.value("rowCount", static_cast<long long>(table->second.row_count));
	if (proposedStart < 0 || proposedCount <= 0 || proposedCount > 50
			|| static_cast<std::size_t>(proposedStart) >= table->second.row_count)
		return {false, "invalid_range: the requested row window does not exist."};
	const std::size_t rowStart = static_cast<std::size_t>(proposedStart);
	const std::size_t rowCount = std::min<std::size_t>(static_cast<std::size_t>(proposedCount),
		table->second.row_count - rowStart);
	const bool sameWindow = plan.draft->active_table_id
		&& *plan.draft->active_table_id == tableHandle
		&& plan.draft->active_row_start == rowStart && plan.draft->active_row_count == rowCount;
	// REVEALING CONTENT and UPDATING EVIDENCE STATE FOR ALREADY-VISIBLE CONTENT
	// are different operations. A call against the exact currently-visible
	// window is only a no-op (and thus rejected) when it asks for nothing but
	// that same reveal again; one that also carries a real semantic state
	// transition - a fresh evidenceSufficient=true, a valid evidenceObjective/
	// evidenceObjectiveClosure, or an updated workingEvidenceNote - is a
	// legitimate single document.inspect call and must proceed (see the
	// working-evidence-note block below, which does NOT require a note for
	// this case: nothing is being evicted).
	const bool evidenceSufficientArg = arguments.value("evidenceSufficient", false);
	const bool hasObjectiveArg = arguments.contains("evidenceObjective") && !arguments.at("evidenceObjective").is_null();
	const bool hasClosureArg = arguments.contains("evidenceObjectiveClosure")
		&& !arguments.at("evidenceObjectiveClosure").is_null();
	const bool noteSupplied = arguments.contains("workingEvidenceNote") && arguments.at("workingEvidenceNote").is_string()
		&& !arguments.at("workingEvidenceNote").get<std::string>().empty();
	const bool hasSemanticTransition = evidenceSufficientArg || hasObjectiveArg || hasClosureArg || noteSupplied;
	if (sameWindow && !hasSemanticTransition)
		return {false, "already_visible: that exact row window is already visible. To update evidence state for it "
			"(evidenceSufficient, evidenceObjective, evidenceObjectiveClosure or an updated workingEvidenceNote), "
			"include that in the call instead of repeating the same reveal unchanged."};

	// Every check below (objective binding, closure, working-evidence-note)
	// is validation only - none of it may mutate plan state until every last
	// one has passed. A real live run proved why concretely: a call that
	// bound a brand-new objective and then was rejected by a LATER check
	// (the working-evidence-note requirement) left that objective registered
	// with zero real coverage anyway, since the original code mutated
	// plan.draft->evidence_objectives before reaching that later check. The
	// model's own next attempt then hit a confusing "already bound" rejection
	// for an objective its own prior (rejected!) call had silently created.
	// Collect what WOULD change into locals first; apply everything together
	// only after this whole function is certain to return {true, ...}.
	std::optional<EvidenceObjective> newObjectiveToCreate;
	// The FINAL resolved id for this call's evidenceObjective, if any - never
	// the model's own raw (possibly unsafe) argument value. Every later use
	// of "the id this call concerns" must read this, not re-read
	// arguments.at("evidenceObjective").at("id") independently, or an unsafe
	// id auto-resolved above would silently reappear as a lookup key nothing
	// was ever registered under.
	std::optional<std::string> resolvedObjectiveId;
	if (arguments.contains("evidenceObjective") && !arguments.at("evidenceObjective").is_null()) {
		const Json &objectiveArgs = arguments.at("evidenceObjective");
		std::string id = objectiveArgs.at("id").get<std::string>();
		const auto existingTableBinding = plan.draft->table_evidence_objective.find(tableHandle);
		if (evidence_objective_id_looks_unsafe(plan, id)) {
			// Backend owns the id whenever the model's own proposal cannot
			// safely be trusted as a stable handle: reuse this table's real
			// binding if one already exists, or mint a fresh one otherwise.
			// Never fail the call closed merely because the model echoed
			// something unsafe back - see this function's own comment.
			id = existingTableBinding != plan.draft->table_evidence_objective.end()
				? existingTableBinding->second
				: backend_owned_evidence_objective_id(plan, tableHandle);
		} else if (existingTableBinding != plan.draft->table_evidence_objective.end()
				&& existingTableBinding->second != id) {
			return {false, "invalid_evidence_objective: table " + tableHandle + " is already bound to a "
				"different evidence objective - see this table's real id in state.tableEvidenceObjectives "
				"(never trust an id quoted inside a message, which may be Papinian-protected) and reuse that "
				"exact value, or close it first with evidenceObjectiveClosure before opening a new one."};
		}
		const auto existingObjective = plan.draft->evidence_objectives.find(id);
		if (existingObjective != plan.draft->evidence_objectives.end()
				&& existingObjective->second.table_handle != tableHandle)
			return {false, "invalid_evidence_objective: this id is already bound to a different table - see "
				"state.tableEvidenceObjectives for the real binding and use a new id for this table instead."};
		if (existingObjective == plan.draft->evidence_objectives.end()) {
			EvidenceObjective objective;
			objective.id = id;
			objective.description = objectiveArgs.at("description").get<std::string>();
			objective.document_id = table->second.document_id;
			objective.table_handle = tableHandle;
			objective.coverage_mode = objectiveArgs.at("coverageMode").get<std::string>() == "exhaustive"
				? EvidenceCoverageMode::Exhaustive : EvidenceCoverageMode::Targeted;
			newObjectiveToCreate = std::move(objective);
		}
		resolvedObjectiveId = std::move(id);
	}
	std::optional<std::string> closureTargetId;
	std::string closureReasonText;
	std::vector<std::string> closureUnresolvedQuestions;
	std::vector<std::string> closureScopeLimitations;
	if (arguments.contains("evidenceObjectiveClosure") && !arguments.at("evidenceObjectiveClosure").is_null()) {
		const Json &closureArgs = arguments.at("evidenceObjectiveClosure");
		const std::string id = closureArgs.at("id").get<std::string>();
		// The objective being closed may be the very one this same call just
		// validated for creation above (a single-window targeted read that
		// already knows it is stopping here) - check the pending creation
		// first, the already-persisted map second.
		const EvidenceObjective *objective = (newObjectiveToCreate && newObjectiveToCreate->id == id)
			? &*newObjectiveToCreate : nullptr;
		const auto existing = plan.draft->evidence_objectives.find(id);
		if (!objective && existing != plan.draft->evidence_objectives.end()) objective = &existing->second;
		if (!objective)
			return {false, "unknown_evidence_objective: \"" + id + "\" has not been declared on any table."};
		if (objective->table_handle != tableHandle)
			return {false, "invalid_evidence_objective_closure: close \"" + id + "\" on its own bound table "
				+ objective->table_handle + ", not " + tableHandle + "."};
		if (objective->coverage_mode == EvidenceCoverageMode::Exhaustive)
			return {false, "invalid_evidence_objective_closure: \"" + id + "\" is exhaustive - its completeness "
				"is structural coverage, never a narrative closure."};
		for (const Json &entry : closureArgs.value("unresolvedQuestions", Json::array()))
			if (entry.is_string()) closureUnresolvedQuestions.push_back(entry.get<std::string>());
		for (const Json &entry : closureArgs.value("scopeLimitations", Json::array()))
			if (entry.is_string()) closureScopeLimitations.push_back(entry.get<std::string>());
		closureReasonText = closureArgs.at("closureReason").get<std::string>();
		closureTargetId = id;
	}

	// THE MODEL AUTHORS THE WORKING EVIDENCE JUDGMENT. SINGULARITY OWNS THE
	// STRUCTURAL PROVENANCE OF THE WINDOW BEING SUMMARIZED: activeTableId/
	// activeRowStart/activeRowCount are already canonical runtime state, so a
	// note replacing that window is bound to it automatically unless the model
	// explicitly overrides workingEvidenceTables - it is never required to
	// reconstruct that association itself. A note is REQUIRED only when this
	// call is actually about to evict the active disposable window (a
	// different table, or a different row range on the same table); a
	// same-window semantic transition (see hasSemanticTransition above) never
	// evicts anything, so no note is owed, though the model may still supply
	// one to update its running judgment on the window it is still looking at.
	std::string workingNoteText;
	std::set<std::string> workingNoteTables;
	bool workingNoteProvided = false;
	const bool activeWindowDisposable = plan.draft->active_table_id
		&& table_uses_disposable_window(plan, *plan.draft->active_table_id);
	const bool replacingActiveWindow = activeWindowDisposable && !sameWindow;
	if (replacingActiveWindow || (activeWindowDisposable && noteSupplied)) {
		const bool noteValid = noteSupplied
			&& research::estimate_tokens(arguments.at("workingEvidenceNote").get_ref<const std::string &>()) <= 512;
		if (!noteValid) {
			if (replacingActiveWindow) {
				const std::string signature = working_evidence_repair_signature(
					*plan.draft->active_table_id, plan.draft->active_row_start, plan.draft->active_row_count,
					noteSupplied, noteSupplied, arguments, evidenceSufficientArg, hasObjectiveArg, hasClosureArg);
				if (noteSupplied)
					return {false, "working_evidence_required_" + signature + ": your workingEvidenceNote "
						"exceeded roughly 512 tokens. currentVisibleWindow: tableHandle="
						+ *plan.draft->active_table_id + ", startRow=" + std::to_string(plan.draft->active_row_start)
						+ ", rowCount=" + std::to_string(plan.draft->active_row_count) + ". Resupply "
						"workingEvidenceNote as a SHORTER summary of your judgment from THIS current window "
						"(roughly 512 tokens or fewer); workingEvidenceTables may be omitted/null because Singularity"
						" will bind the note to the current window automatically."};
				return {false, "working_evidence_required_" + signature + ": preserve a concise model-authored "
					"note before replacing the visible region. currentVisibleWindow: tableHandle="
					+ *plan.draft->active_table_id + ", startRow=" + std::to_string(plan.draft->active_row_start)
					+ ", rowCount=" + std::to_string(plan.draft->active_row_count) + ". Provide "
					"workingEvidenceNote summarizing your judgment from THIS current window; "
					"workingEvidenceTables may be omitted/null because Singularity will bind the note to the "
					"current window automatically."};
			}
			return {false, "invalid_arguments: workingEvidenceNote must be a non-empty string of no more than "
				"roughly 512 tokens."};
		}
		const bool tablesExplicit = arguments.contains("workingEvidenceTables")
			&& !arguments.at("workingEvidenceTables").is_null();
		std::set<std::string> noteTables;
		if (tablesExplicit) {
			if (!arguments.at("workingEvidenceTables").is_array())
				return {false, "invalid_arguments: workingEvidenceTables must be an array of already-inspected "
					"table handles, or null."};
			std::vector<std::string> invalidHandles;
			for (const Json &entry : arguments.at("workingEvidenceTables")) {
				if (!entry.is_string() || plan.draft->expanded_table_ids.count(entry.get<std::string>()) == 0) {
					invalidHandles.push_back(entry.is_string() ? entry.get<std::string>() : entry.dump());
					continue;
				}
				noteTables.insert(entry.get<std::string>());
			}
			if (!invalidHandles.empty()) {
				std::string suppliedList, validList;
				for (const std::string &handle : invalidHandles) suppliedList += (suppliedList.empty() ? "" : ", ") + handle;
				for (const std::string &handle : plan.draft->expanded_table_ids) validList += (validList.empty() ? "" : ", ") + handle;
				return {false, "working_evidence_invalid: a note may cite only tables already inspected by this "
					"operation. supplied invalid handle(s): [" + suppliedList + "]. valid inspected table(s): ["
					+ validList + "]."};
			}
			if (noteTables.count(*plan.draft->active_table_id) == 0)
				return {false, "working_evidence_invalid: the note must remain linked to the region being replaced "
					"(table " + *plan.draft->active_table_id + ")."};
		} else {
			// Runtime-owned default provenance: bind to the exact active window
			// being evicted (or, for a same-window transition, the window the
			// note still describes) - the smallest compatible representation
			// given inspection_working_note_table_ids stores table handles, not
			// row ranges; activeRowStart/activeRowCount remain independently
			// available on the draft for anything that needs the finer window.
			noteTables.insert(*plan.draft->active_table_id);
		}
		workingNoteText = arguments.at("workingEvidenceNote").get<std::string>();
		workingNoteTables = std::move(noteTables);
		workingNoteProvided = true;
	}

	// Every validation above passed - only now does this call actually change
	// anything.
	if (workingNoteProvided) {
		plan.draft->inspection_working_note = std::move(workingNoteText);
		plan.draft->inspection_working_note_table_ids = std::move(workingNoteTables);
	}
	if (newObjectiveToCreate) {
		plan.draft->table_evidence_objective[tableHandle] = newObjectiveToCreate->id;
		plan.draft->evidence_objectives.emplace(newObjectiveToCreate->id, std::move(*newObjectiveToCreate));
	} else if (resolvedObjectiveId) {
		// Continuing an already-bound objective: refine the description if
		// resupplied; coverage_mode stays immutable once first declared (see
		// EvidenceObjective's own comment) so a resent value is never applied.
		// Always the resolved id, never the model's own raw argument - an
		// unsafe id auto-resolved above (see resolvedObjectiveId's own
		// comment) was never registered under its original literal text.
		plan.draft->evidence_objectives.at(*resolvedObjectiveId).description =
			arguments.at("evidenceObjective").at("description").get<std::string>();
	}
	if (closureTargetId) {
		EvidenceObjective &objective = plan.draft->evidence_objectives.at(*closureTargetId);
		objective.closed = true;
		objective.closure_reason = std::move(closureReasonText);
		objective.unresolved_questions = std::move(closureUnresolvedQuestions);
		objective.scope_limitations = std::move(closureScopeLimitations);
	}
	plan.draft->expanded_table_ids.insert(tableHandle);
	if (table_uses_disposable_window(plan, tableHandle)) {
		plan.draft->active_table_id = tableHandle;
		plan.draft->active_row_start = rowStart;
		plan.draft->active_row_count = rowCount;
	}
	// SINGULARITY OWNS THE COVERAGE STATE: merge this exact served window into
	// whichever objective this table is bound to, if any - unconditional on
	// compact_context_mode, since coverage is a task-discipline concept
	// independent of whether the full Project structure happened to fit.
	const auto boundObjective = plan.draft->table_evidence_objective.find(tableHandle);
	if (boundObjective != plan.draft->table_evidence_objective.end())
		merge_evidence_coverage_range(plan.draft->evidence_objectives.at(boundObjective->second),
			rowStart, rowStart + rowCount);
	// THE MODEL OWNS THE JUDGMENT: this is the ONLY place evidence_declared_
	// sufficient is ever populated, and only for the exact document whose own
	// table this same call just inspected - a bare declaration naming an
	// uninspected or unrelated document is structurally impossible here, never
	// merely unvalidated. See document_evidence_satisfied()'s own comment.
	if (evidenceSufficientArg) plan.draft->evidence_declared_sufficient.insert(table->second.document_id);
	++plan.draft->revision;
	std::string suffix;
	if (evidenceSufficientArg) {
		// EVIDENCE SUFFICIENCY IS NOT A BOOLEAN WITHOUT COVERAGE: the flag above
		// is always recorded (never silently dropped - a later, coverage-
		// complete call needs no fresh re-declaration), but document_evidence_
		// satisfied() - the only function anything else ever consults - still
		// requires real structural coverage, so a premature declaration is
		// reported back exactly, in the same call, with the next real range to
		// inspect, rather than accepted at face value or the window discarded.
		const std::string incomplete = describe_incomplete_evidence_coverage(plan, table->second.document_id);
		suffix = incomplete.empty()
			? " Evidence for this document is now marked sufficient by your own judgment."
			: " Evidence sufficiency was NOT recorded as usable yet:" + incomplete;
	}
	// A same-window semantic transition never re-reveals or re-adds coverage
	// (merge_evidence_coverage_range() above is idempotent for an unchanged
	// range, and active_table_id/active_row_start/active_row_count are
	// reassigned to their own already-current values) - only the message
	// reflects that nothing new was shown.
	const std::string action = sameWindow
		? "Updated evidence state for the already-visible " + tableHandle + " rows " + std::to_string(rowStart)
			+ "-" + std::to_string(rowStart + rowCount - 1) + "."
		: "Revealed " + tableHandle + " rows " + std::to_string(rowStart) + "-"
			+ std::to_string(rowStart + rowCount - 1) + " of " + std::to_string(table->second.row_count) + ".";
	return {true, action + suffix};
}

providers::ModelToolDefinition connected_document_map_next_model_tool_definition() {
	return providers::ModelToolDefinition{
		"lm_document_map_next",
		"Advance one named document's own table map to its next page when its current page does not yet show "
		"every one of that document's tables still needing inspection (see that document's own "
		"\"moreTablesAvailable\"). Reveals only more compact table navigation evidence (handles, row counts, "
		"title/header text) - never row or body content, which document.inspect alone reveals.",
		Json{{"type", "object"}, {"properties", {
			{"documentHandle", {{"type", "string"}}},
		}}, {"required", Json::array({"documentHandle"})}, {"additionalProperties", false}},
		Json{{"properties", {{"documentHandle", true}}}},
	};
}

DocumentDraftToolResult request_connected_document_table_map_next_page(
		const ConnectedDocumentActionPlan &plan, const Json &arguments) {
	if (!arguments.is_object() || arguments.size() != 1 || !arguments.contains("documentHandle")
			|| !arguments.at("documentHandle").is_string())
		return {false, "invalid_arguments: documentHandle (string) is required."};
	const std::string handle = arguments.at("documentHandle").get<std::string>();
	const std::optional<std::string> resolvedDocumentId = candidate_document_id_for_handle(plan, handle);
	if (!resolvedDocumentId)
		return {false, "unknown_document: \"" + handle + "\" is not one of this operation's candidate document handles."};
	const std::string documentId = *resolvedDocumentId;
	const std::vector<std::string> eligible = eligible_table_handles_for_document(plan, documentId);
	const std::size_t pageSize = std::min(plan.draft->table_map_page_size, eligible.size());
	const std::size_t currentStart = std::min(
		plan.draft->table_map_page_start.count(documentId) ? plan.draft->table_map_page_start.at(documentId) : std::size_t{0},
		eligible.size());
	const std::size_t nextStart = currentStart + pageSize;
	if (pageSize == 0 || nextStart >= eligible.size())
		return {false, "no_more_table_map_pages: \"" + handle
			+ "\" has no further tables beyond what its current page already shows."};
	plan.draft->table_map_page_start[documentId] = nextStart;
	++plan.draft->revision;
	return {true, "Advanced \"" + handle + "\"'s own table map to its next page."};
}

std::optional<std::string> item_handle_for_field(const ConnectedDocumentActionPlan &plan, const std::string &fieldHandle) {
	for (const auto &[itemHandle, fields] : plan.item_field_handles)
		if (std::find(fields.begin(), fields.end(), fieldHandle) != fields.end()) return itemHandle;
	return std::nullopt;
}

const char *review_issue_category_string(ReviewIssueCategory category) {
	switch (category) {
		case ReviewIssueCategory::EvidenceCompleteness: return "evidence_completeness";
		case ReviewIssueCategory::Specificity: return "specificity";
		case ReviewIssueCategory::MaterialMitigants: return "material_mitigants";
		case ReviewIssueCategory::OutputCompleteness: return "output_completeness";
		case ReviewIssueCategory::DraftQuality: return "draft_quality";
		case ReviewIssueCategory::ClaimSupport: return "claim_support";
	}
	return "evidence_completeness";
}

std::optional<ReviewIssueCategory> review_issue_category_from_string(const std::string &value) {
	if (value == "evidence_completeness") return ReviewIssueCategory::EvidenceCompleteness;
	if (value == "specificity") return ReviewIssueCategory::Specificity;
	if (value == "material_mitigants") return ReviewIssueCategory::MaterialMitigants;
	if (value == "output_completeness") return ReviewIssueCategory::OutputCompleteness;
	if (value == "draft_quality") return ReviewIssueCategory::DraftQuality;
	if (value == "claim_support") return ReviewIssueCategory::ClaimSupport;
	return std::nullopt;
}

providers::ModelToolDefinition connected_document_professional_review_model_tool_definition() {
	const Json issue{{"type", "object"}, {"properties", {
		{"category", {{"type", "string"}, {"enum", Json::array({"evidence_completeness", "specificity",
			"material_mitigants", "output_completeness", "draft_quality", "claim_support"})}}},
		{"evidenceObjectiveId", {{"type", Json::array({"string", "null"})}}},
		{"tableHandle", {{"type", Json::array({"string", "null"})}}},
		{"fieldHandle", {{"type", Json::array({"string", "null"})}}},
		{"message", {{"type", "string"}, {"minLength", 1}, {"maxLength", 800}}},
		{"suggestedNextAction", {{"type", "string"}, {"minLength", 1}, {"maxLength", 400}}},
	}}, {"required", Json::array({"category", "evidenceObjectiveId", "tableHandle", "fieldHandle", "message",
		"suggestedNextAction"})}, {"additionalProperties", false}};
	return providers::ModelToolDefinition{
		"lm_document_review_submit",
		"Submit your own professional completeness review of the otherwise-complete draft (see this turn's own "
		"instruction for exactly what to check). \"pass\" requires an empty issues array - if you have a real "
		"concern, use the verdict that routes to where it must be fixed instead. \"needs_more_evidence\" and "
		"\"needs_redraft\" each require every issue to name the exact evidenceObjectiveId/tableHandle or "
		"fieldHandle it concerns, so Singularity can route back to exactly that work; \"needs_output_replan\" does "
		"not require a reference. This review is your own judgment call, never a rubber stamp of your prior work.",
		Json{{"type", "object"}, {"properties", {
			{"verdict", {{"type", "string"}, {"enum", Json::array({"pass", "needs_more_evidence",
				"needs_output_replan", "needs_redraft"})}}},
			{"issues", {{"type", "array"}, {"items", issue}}},
		}}, {"required", Json::array({"verdict", "issues"})}, {"additionalProperties", false}},
		Json{{"properties", {{"verdict", true}, {"issues", {{"items", {{"properties", {{"message", true}}}}}}}}}},
	};
}

// Bounds the review/correct/re-review loop independently of (and usually
// well inside) the operation's own generic tool budget - see
// DocumentActionDraft::professional_review_cycles_used's own comment. A
// std::runtime_error here propagates through the exact same generic
// exception path an exhausted operation-tool budget already uses (see
// ResearchToolLoop.hpp's own OperationFailed handling and MatterRequestExecution.
// cpp's own generic std::exception catch) - a clean, non-committing, reason-
// preserving failure, never a silent commit of an incomplete professional
// document.
constexpr std::size_t kMaxProfessionalReviewCycles = 2;

DocumentDraftToolResult submit_connected_document_professional_review(
		const ConnectedDocumentActionPlan &plan, const Json &arguments) {
	if (!arguments.is_object() || arguments.size() != 2 || !arguments.contains("verdict")
			|| !arguments.at("verdict").is_string() || !arguments.contains("issues") || !arguments.at("issues").is_array())
		return {false, "invalid_arguments: verdict (string) and issues (array) are required."};
	const std::string verdictArg = arguments.at("verdict").get<std::string>();
	ProfessionalReviewResult review;
	if (verdictArg == "pass") review.verdict = ProfessionalReviewVerdict::Pass;
	else if (verdictArg == "needs_more_evidence") review.verdict = ProfessionalReviewVerdict::NeedsMoreEvidence;
	else if (verdictArg == "needs_output_replan") review.verdict = ProfessionalReviewVerdict::NeedsOutputReplan;
	else if (verdictArg == "needs_redraft") review.verdict = ProfessionalReviewVerdict::NeedsRedraft;
	else return {false, "invalid_arguments: verdict must be pass, needs_more_evidence, needs_output_replan or needs_redraft."};

	for (const Json &entry : arguments.at("issues")) {
		if (!entry.is_object() || !entry.contains("category") || !entry.at("category").is_string()
				|| !entry.contains("message") || !entry.at("message").is_string()
				|| trim_ascii_space(entry.at("message").get<std::string>()).empty()
				|| !entry.contains("suggestedNextAction") || !entry.at("suggestedNextAction").is_string()
				|| trim_ascii_space(entry.at("suggestedNextAction").get<std::string>()).empty())
			return {false, "invalid_issue: category, message and suggestedNextAction are all required."};
		const auto category = review_issue_category_from_string(entry.at("category").get<std::string>());
		if (!category) return {false, "invalid_issue: \"" + entry.at("category").get<std::string>() + "\" is not a recognized category."};
		ProfessionalReviewIssue issue;
		issue.category = *category;
		if (entry.value("evidenceObjectiveId", Json(nullptr)).is_string()) {
			const std::string id = entry.at("evidenceObjectiveId").get<std::string>();
			if (plan.draft->evidence_objectives.count(id) == 0)
				return {false, "unknown_evidence_objective: \"" + id + "\" has not been declared."};
			issue.evidence_objective_id = id;
		}
		if (entry.value("tableHandle", Json(nullptr)).is_string()) {
			const std::string handle = entry.at("tableHandle").get<std::string>();
			if (plan.table_handles.count(handle) == 0)
				return {false, "unknown_table: \"" + handle + "\" is not a real table handle."};
			issue.table_handle = handle;
		}
		if (entry.value("fieldHandle", Json(nullptr)).is_string()) {
			const std::string handle = entry.at("fieldHandle").get<std::string>();
			if (plan.field_handles.count(handle) == 0)
				return {false, "unknown_field: \"" + handle + "\" is not a real field handle."};
			issue.field_handle = handle;
		}
		issue.message = entry.at("message").get<std::string>();
		issue.suggested_next_action = entry.at("suggestedNextAction").get<std::string>();
		review.issues.push_back(std::move(issue));
	}

	if (review.verdict == ProfessionalReviewVerdict::Pass) {
		if (!review.issues.empty())
			return {false, "invalid_review: a pass verdict requires an empty issues array."};
	} else {
		if (review.issues.empty())
			return {false, "invalid_review: a non-pass verdict requires at least one issue explaining why."};
		if (review.verdict == ProfessionalReviewVerdict::NeedsMoreEvidence
				&& std::none_of(review.issues.begin(), review.issues.end(), [](const ProfessionalReviewIssue &issue) {
					return issue.evidence_objective_id.has_value() || issue.table_handle.has_value(); }))
			return {false, "invalid_review: needs_more_evidence requires at least one issue naming the exact "
				"evidenceObjectiveId or tableHandle it concerns."};
		if (review.verdict == ProfessionalReviewVerdict::NeedsRedraft
				&& std::none_of(review.issues.begin(), review.issues.end(),
					[](const ProfessionalReviewIssue &issue) { return issue.field_handle.has_value(); }))
			return {false, "invalid_review: needs_redraft requires at least one issue naming the exact fieldHandle "
				"it concerns."};
	}

	if (review.verdict != ProfessionalReviewVerdict::Pass && plan.draft->professional_review_cycles_used >= kMaxProfessionalReviewCycles) {
		// Told to the lawyer as it is, in their words - never as a code or a
		// review category: what still falls short, that nothing was written, and
		// what they can do next.
		std::string concerns;
		std::size_t named = 0;
		for (const ProfessionalReviewIssue &issue : review.issues) {
			const std::string message = trim_ascii_space(issue.message);
			if (message.empty() || named == 2) continue;
			concerns += (concerns.empty() ? "" : " ") + message;
			if (concerns.back() != '.' && concerns.back() != '?' && concerns.back() != '!') concerns += ".";
			++named;
		}
		// The review ran once more than the draft was revised: it is the
		// revisions the lawyer is told about.
		const std::string revised = kMaxProfessionalReviewCycles == 1 ? std::string("once")
			: kMaxProfessionalReviewCycles == 2 ? std::string("twice")
			: std::to_string(kMaxProfessionalReviewCycles) + " times";
		throw std::runtime_error("Singularity revised this draft " + revised + " and it still falls short"
			+ (concerns.empty() ? std::string(".") : ": " + concerns)
			+ " Nothing was written, so the document is unchanged. Tell Singularity how to settle this and ask again.");
	}

	++plan.draft->revision;
	plan.draft->professional_review = review;
	plan.draft->professional_review_reviewed_revision = plan.draft->revision;
	if (review.verdict == ProfessionalReviewVerdict::Pass)
		return {true, "Professional review passed."};

	++plan.draft->professional_review_cycles_used;
	plan.draft->finished = false;
	if (review.verdict == ProfessionalReviewVerdict::NeedsMoreEvidence) {
		std::set<std::string> documentsToReopen;
		for (const ProfessionalReviewIssue &issue : review.issues) {
			if (issue.evidence_objective_id) {
				const auto objective = plan.draft->evidence_objectives.find(*issue.evidence_objective_id);
				if (objective != plan.draft->evidence_objectives.end())
					documentsToReopen.insert(objective->second.document_id);
			}
			if (issue.table_handle) {
				const auto table = plan.table_handles.find(*issue.table_handle);
				if (table != plan.table_handles.end()) documentsToReopen.insert(table->second.document_id);
			}
		}
		for (const std::string &documentId : documentsToReopen)
			plan.draft->evidence_declared_sufficient.erase(documentId);
	} else if (review.verdict == ProfessionalReviewVerdict::NeedsOutputReplan) {
		plan.draft->output_agenda_defined = false;
		plan.draft->required_outputs.clear();
		plan.draft->selected_output_region_handles.clear();
		plan.draft->field_dispositions.clear();
	} else if (review.verdict == ProfessionalReviewVerdict::NeedsRedraft) {
		for (const ProfessionalReviewIssue &issue : review.issues) {
			if (!issue.field_handle) continue;
			const auto itemHandle = item_handle_for_field(plan, *issue.field_handle);
			if (!itemHandle) continue;
			const auto item = plan.draft->items.find(*itemHandle);
			if (item == plan.draft->items.end()) continue;
			item->second.state = DocumentDraftItemState::NeedsCorrection;
			item->second.last_issue_category = review_issue_category_string(issue.category);
			item->second.last_issue_message = issue.message + " " + issue.suggested_next_action;
		}
	}
	return {true, "Professional review recorded: " + std::to_string(review.issues.size())
		+ " issue(s) to resolve before the next review."};
}

bool compact_connected_document_inspection_window(const ConnectedDocumentActionPlan &plan) {
	if (!plan.compact_context_mode) return false;
	if (plan.draft->active_table_id && plan.draft->active_row_count > 1) {
		plan.draft->active_row_count = std::max<std::size_t>(1, plan.draft->active_row_count / 2);
		return true;
	}
	// The row window is already at its own smallest lossless unit (or none is
	// active at all) - try shrinking the table-map page size next, the same
	// generic "one bounded structural unit at a time" principle one level up.
	// This is real progress ONLY when some real document's own eligible-table
	// count today exceeds the prospective new page size - one document with
	// one table (this operation's own common case) already shows everything
	// at any page size >= 1, so "succeeding" here without checking that would
	// report progress the caller's very next compiled turn could never
	// observe: a false positive that burns exactly one budget-bounded retry
	// for nothing before whatever branch below actually helps (or the whole
	// operation correctly fails closed) - see run_research_tool_loop()'s own
	// operationContextCompactionHandler parameter comment in ResearchToolLoop
	// .hpp for this handler's exact contract.
	std::set<std::string> documentIds;
	for (const auto &[tableHandle, summary] : plan.table_handles) {
		(void)tableHandle;
		documentIds.insert(summary.document_id);
	}
	auto anyDocumentHasMoreEligibleTablesThan = [&](std::size_t pageSize) {
		for (const std::string &documentId : documentIds)
			if (eligible_table_handles_for_document(plan, documentId).size() > pageSize) return true;
		return false;
	};
	constexpr std::size_t kInitialTableMapPageSize = 8;
	if (plan.draft->table_map_page_size > kInitialTableMapPageSize
			&& anyDocumentHasMoreEligibleTablesThan(kInitialTableMapPageSize)) {
		plan.draft->table_map_page_size = kInitialTableMapPageSize;
		return true;
	}
	if (plan.draft->table_map_page_size > 1) {
		const std::size_t nextPageSize = std::max<std::size_t>(1, plan.draft->table_map_page_size / 2);
		if (anyDocumentHasMoreEligibleTablesThan(nextPageSize)) {
			plan.draft->table_map_page_size = nextPageSize;
			return true;
		}
	}
	return false;
}

std::optional<research::FinalTextIssue> connected_document_draft_completion_issue(
		const ConnectedDocumentActionPlan &plan,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence) {
	// Product-owner correction: which row(s)/item(s) a request is actually
	// about is legal/document judgment - the model's job, not Singularity's. This
	// used to require every structural item in the whole document (every
	// table row) to reach Accepted before a turn could ever finish, which
	// only stayed usable because a since-removed keyword pre-filter silently
	// hid every row the instruction did not happen to name first. Without
	// that filter a real multi-row table (a risk register, a worklist) would
	// force the model to fabricate a change for every row it is not actually
	// asked to touch. draft_is_complete() lets the model itself declare it is
	// done (final=true) once it has made at least one real, bounded,
	// structurally-valid mutation (the same per-field validation as before -
	// see upsert_connected_document_draft_item()), without Singularity deciding
	// which or how many items that must be; a request that genuinely does
	// concern every item still completes exactly as before, unaffected.
	// assembled_draft_result() already only ever assembles Accepted items, so
	// this changes only when the model may stop, never what gets applied.
	if (!plan.draft->output_agenda_defined) {
		return research::FinalTextIssue{research::FinalTextIssueKind::OperationWork,
			"The required output binding is not defined. Select the relevant structural region(s), map every requested "
			"professional deliverable to an issued editable Body field, and explicitly disposition every editable "
			"field in each selected region with document.outputs.define before drafting.", "required_output_agenda"};
	}
	if (!all_required_outputs_satisfied(plan)) {
		for (const std::string &regionHandle : plan.draft->selected_output_region_handles) {
			const auto editable = plan.item_editable_field_handles.find(regionHandle);
			if (editable == plan.item_editable_field_handles.end()) continue;
			for (const std::string &fieldHandle : editable->second)
				if (!effective_field_disposition(plan, fieldHandle))
					return research::FinalTextIssue{research::FinalTextIssueKind::OperationWork,
						"An editable field in selected region " + regionHandle
						+ " has no explicit WRITE or LEAVE_UNCHANGED disposition.",
						"field_disposition_unresolved"};
		}
	}
	std::vector<std::string> unresolvedOutputs;
	for (const RequiredOutputObligation &obligation : plan.draft->required_outputs)
		if (!required_output_satisfied(plan, obligation)) unresolvedOutputs.push_back(obligation.id);
	if (!unresolvedOutputs.empty()) {
		return research::FinalTextIssue{research::FinalTextIssueKind::OperationWork,
			"Required outputs remain unresolved: " + join(unresolvedOutputs, ", ")
			+ ". An optional Body update does not satisfy a different mapped deliverable; update each exact field.",
			"required_outputs_unresolved"};
	}
	if (!draft_is_complete(plan)) {
		return research::FinalTextIssue{research::FinalTextIssueKind::OperationWork,
			accepted_draft_item_count(plan) == 0
				? "No document change has been proposed yet. Submit document.draft.upsert for every exact WRITE "
					"obligation in the finalized output agenda."
				: "The structured output agenda still has an unresolved WRITE obligation. Continue with "
					"document.draft.upsert for the issued pending item.",
			"draft_completion"};
	}
	// STRUCTURAL DRAFT COMPLETE != PROFESSIONAL OPERATION COMPLETE: a live
	// production acceptance proved this concretely - a single-item operation
	// reached draft_is_complete()==true and returned as "completed" without
	// ever being challenged by the existing Professional Completion
	// Challenge, because this function (the actual completion gate
	// ResearchToolLoop's own finalTextIssue callback consults) never
	// consulted connected_document_operation_frontier()'s own, separately
	// correct ProfessionalReview branch - that frontier value was only ever
	// read back for tool-exposure gating on a LATER turn that, in that run,
	// never happened. Reusing the identical
	// professional_review_passed_current_revision() predicate frontier
	// itself uses keeps both permanently in agreement by construction, not
	// convention - see that function's own comment on stale-revision safety.
	if (!professional_review_passed_current_revision(plan)) {
		return research::FinalTextIssue{research::FinalTextIssueKind::OperationWork,
			"The draft is structurally complete, but the Professional Completion Challenge has not yet "
			"recorded a Pass for this exact revision. Submit document.review.submit with your own typed "
			"verdict (Pass, or the specific issue category and reference if not) before this operation may "
			"finish.", "professional_review_required"};
	}
	try {
		(void)resolve_document_action_proposal(plan, assembled_draft_result(plan, "Completed."),
			availableExternalEvidence);
		return std::nullopt;
	} catch (const ResearchCoverageError &error) {
		return research::FinalTextIssue{research::FinalTextIssueKind::ResearchCoverage,
			error.what(), "research_coverage"};
	} catch (const std::exception &error) {
		return research::FinalTextIssue{research::FinalTextIssueKind::CorrectionOnly,
			error.what(), "draft_assembly"};
	}
}

// The exact accepted replaceCellText value for fieldHandle within itemHandle,
// if any - the same locator-match pattern required_output_satisfied() already
// uses, reused here (never re-derived differently) so the review turn's own
// "proposed value" can never disagree with what completion actually checks.
std::optional<std::string> proposed_field_value(
		const ConnectedDocumentActionPlan &plan, const std::string &itemHandle, const std::string &fieldHandle) {
	const auto item = plan.draft->items.find(itemHandle);
	if (item == plan.draft->items.end() || !item->second.accepted_action) return std::nullopt;
	const auto locator = plan.field_handles.find(fieldHandle);
	if (locator == plan.field_handles.end()) return std::nullopt;
	const Json expected = model_cell_locator_json(locator->second);
	for (const Json &operation : item->second.accepted_action->value("operations", Json::array()))
		if (operation.is_object() && operation.value("kind", std::string()) == "replaceCellText"
				&& operation.value("locator", Json::object()) == expected)
			return operation.value("newValue", std::string());
	return std::nullopt;
}

Json compile_connected_document_review_context(const ConnectedDocumentActionPlan &plan) {
	Json objectives = Json::array();
	for (const auto &[id, objective] : plan.draft->evidence_objectives) {
		const auto table = plan.table_handles.find(objective.table_handle);
		const std::size_t totalRows = table != plan.table_handles.end() ? table->second.row_count : 0;
		std::size_t inspectedRows = 0;
		for (const EvidenceCoverageRange &range : objective.inspected_ranges)
			inspectedRows += range.end_row - range.start_row;
		objectives.push_back(Json{
			{"id", id},
			{"description", objective.description},
			{"documentName", candidate_document_name_for_id(plan, objective.document_id)},
			{"documentHandle", candidate_document_handle_for_id(plan, objective.document_id)},
			{"tableHandle", objective.table_handle},
			{"coverageMode", objective.coverage_mode == EvidenceCoverageMode::Exhaustive ? "exhaustive" : "targeted"},
			{"totalRows", totalRows},
			{"inspectedRows", inspectedRows},
			{"coverageComplete", inspectedRows >= totalRows},
			{"closed", objective.closed},
			{"closureReason", objective.closure_reason ? Json(*objective.closure_reason) : Json(nullptr)},
			{"unresolvedQuestions", objective.unresolved_questions},
			{"scopeLimitations", objective.scope_limitations},
		});
	}
	// selected_output_region_handles is REAL, dependency-closed professional
	// relevance, never Matter-size noise: every region in it was explicitly
	// chosen by the model (document.outputs.define) as part of THIS
	// operation's own required deliverables (DocumentActionDraft::
	// selected_output_region_handles' own comment). A prior pass here capped
	// full per-field detail past a fixed count and replaced the rest with an
	// aggregate summary - that was WRONG: it let physical payload size decide
	// which real accepted professional work the reviewer gets to see, which
	// is exactly the defect this migration exists to remove, not reintroduce
	// one frontier later. The genuine Matter-size-invariance property this
	// frontier must have is the same one every other frontier already has -
	// UNRELATED canonical Matter growth (other tables, other documents,
	// regions never selected for this operation) must not appear here at
	// all - and selected_output_region_handles already satisfies that: it
	// contains only the regions this operation itself selected, never every
	// structural region the underlying document happens to have.
	Json regions = Json::array();
	for (const std::string &regionHandle : plan.draft->selected_output_region_handles) {
		const auto fields = plan.item_editable_field_handles.find(regionHandle);
		Json fieldEntries = Json::array();
		for (const std::string &fieldHandle : fields != plan.item_editable_field_handles.end()
				? fields->second : std::vector<std::string>{}) {
			const auto *binding = field_disposition_binding(plan, fieldHandle);
			const auto label = plan.field_labels.find(fieldHandle);
			const auto proposedValue = proposed_field_value(plan, regionHandle, fieldHandle);
			fieldEntries.push_back(Json{
				{"fieldHandle", fieldHandle},
				{"label", label != plan.field_labels.end() ? Json(label->second) : Json(nullptr)},
				{"disposition", binding
					? (binding->disposition == DocumentFieldDisposition::Write ? "write" : "leave_unchanged")
					: Json(nullptr)},
				{"reason", binding && binding->reason ? Json(*binding->reason) : Json(nullptr)},
				{"proposedValue", proposedValue ? Json(*proposedValue) : Json(nullptr)},
			});
		}
		regions.push_back(Json{{"itemHandle", regionHandle}, {"fields", std::move(fieldEntries)}});
	}
	// plan.model_instruction is a ONE-TIME snapshot compiled at
	// prepare_connected_document_action() time (see its own assignment site)
	// - before the operation's own first Papinian protection pass ever ran,
	// and before the model had inspected anything. It bakes in a full raw
	// "STATE:\n{...}" table/field-value dump exactly like an ordinary
	// Drafting turn's own instruction text, which is precisely what this
	// review context's own design explicitly must NOT do (see the comment
	// on compile_connected_document_turn_data()'s own review-frontier early
	// return: "Never includes a raw table row dump"). This was never
	// reachable in practice before the Professional Completion Challenge
	// itself became unbypassable (see professional_review_passed_current_
	// revision()) - a live regression proved the exact consequence: this
	// static snapshot's own raw field value reached a real provider request
	// unprotected, since Papinian's own per-turn protection pass has no way
	// to know this text is stale, unrelated document state rather than this
	// turn's own real content. The review turn's real guidance already comes
	// from its own dedicated turnInstruction (compiled fresh, outside this
	// function, specifically for the ProfessionalReview frontier) plus the
	// evidenceObjectives/workingEvidenceNote/selectedRegions below - none of
	// which ever need this stale snapshot restated.
	return Json{
		{"instruction", std::string()},
		{"evidenceObjectives", std::move(objectives)},
		{"workingEvidenceNote", plan.draft->inspection_working_note},
		{"selectedRegions", std::move(regions)},
	};
}

routing::TurnDataPlane compile_connected_document_turn_data(
		const ConnectedDocumentActionPlan &plan,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
		const std::optional<research::FinalTextIssue> &issue,
		research::OperationWorkFrontier frontier,
		std::vector<std::string> *outAdmittedItemHandles) {
	// Sec.13 (context-compiler discipline): the Professional Completion
	// Challenge gets its own compact, dedicated projection - never the normal
	// turn data's full candidate-document/collapsed-table assembly below,
	// whose own accepted-item payloads are deliberately absent (a drafting
	// turn must reason fresh) and whose table summaries the review turn does
	// not need at all (it reasons from the coverage ledger and working notes,
	// never raw rows it no longer sees).
	if (frontier == research::OperationWorkFrontier::ProfessionalReview) {
		(void)issue;
		return routing::TurnDataPlane{compile_connected_document_review_context(plan)};
	}
	// Every candidate document this turn may target, in the order the model
	// should see them (the plan's own default first) - each item below is
	// grouped under whichever one its fields actually resolve to, so the
	// model always knows which real Project document a row belongs to
	// instead of every candidate's rows being flattened under one name.
	std::vector<std::pair<std::string, std::string>> documentsById;
	documentsById.emplace_back(plan.target_document_id, plan.target_document_name);
	for (const DocumentActionCandidate &candidate : plan.other_candidate_documents)
		documentsById.emplace_back(candidate.document_id, candidate.document_name);
	if (plan.draft->output_agenda_defined && !plan.draft->selected_output_region_handles.empty()) {
		const auto fields = plan.item_field_handles.find(plan.draft->selected_output_region_handles.front());
		if (fields != plan.item_field_handles.end() && !fields->second.empty()) {
			const std::string documentId = plan.field_handles.at(fields->second.front()).document_id;
			documentsById = {{documentId, candidate_document_name_for_id(plan, documentId)}};
		}
	}
	std::map<std::string, Json> pendingItemsByDocument;
	for (const auto &[documentId, name] : documentsById) { (void)name; pendingItemsByDocument[documentId] = Json::array(); }
	// Which item(s) this exact turn actually shows in "targets" below - used
	// to scope requiredOutputs/selectedOutputRegions (see their own comment
	// further down) to the item(s) genuinely being drafted THIS turn, never
	// the operation's complete finalized agenda regardless of how many
	// regions it spans.
	std::set<std::string> visibleItemHandlesThisTurn;
	Json acceptedHandles = Json::array();
	const bool researchTurn = std::any_of(plan.draft->items.begin(), plan.draft->items.end(),
		[](const auto &entry) { return entry.second.state == DocumentDraftItemState::NeedsResearch; });
	const bool correctionTurn = !researchTurn && std::any_of(plan.draft->items.begin(), plan.draft->items.end(),
		[](const auto &entry) { return entry.second.state == DocumentDraftItemState::NeedsCorrection; });
	std::map<std::string, Json> collapsedTablesByDocument;
	for (const auto &[documentId, name] : documentsById) { (void)name; collapsedTablesByDocument[documentId] = Json::array(); }
	const bool anyTableRequiresInspection = !plan.draft->output_agenda_defined
		&& (plan.compact_context_mode || !plan.draft->required_evidence.empty());
	// While any explicitly required document remains ungrounded, is_item_
	// collapsed()'s own cross-document gate already makes every OTHER
	// document's items unreachable regardless of what its own table map
	// shows - so showing that not-yet-actionable document's full collapsed-
	// table structure at the very same time is pure, currently-unusable
	// weight. A real design-partner acceptance run proved this concretely: a
	// real destination document's own dozen-plus tables, listed while a
	// required source document was still the only thing the model could
	// actually act on, was a meaningful fraction of a genuinely oversized
	// turn for a small local model. Scoped to required-evidence grounding
	// only (never compact-context-alone, which has no such priority order):
	// once every required document is satisfied, or none was ever required,
	// every candidate document's own map becomes relevant again immediately.
	const bool anyRequiredEvidenceOutstanding = !document_grounding_complete_for_drafting(plan);
	auto documentGroundingIsActionableNow = [&](const std::string &documentId) {
		if (!anyRequiredEvidenceOutstanding) return true;
		for (const auto &obligation : plan.draft->required_evidence)
			if (obligation.document_id == documentId) return !document_evidence_satisfied(plan, documentId);
		return false;
	};
	// Table-map pagination: a document's own eligible tables are listed in
	// stable discovery order, then sliced to one bounded page at a time -
	// see DocumentActionDraft::table_map_page_start/_size's own comment for
	// why (a real document with a dozen-plus real tables can exceed a small
	// local model's real turn budget from compact structural summaries
	// alone). document.map.next (request_connected_document_table_map_next_
	// page()) advances a document's own cursor; nothing here ever decides
	// which table matters, only how many of them fit on one turn.
	std::map<std::string, bool> hasMoreTablesByDocument;
	if (anyTableRequiresInspection) {
		for (const auto &[documentId, name] : documentsById) {
			(void)name;
			if (!documentGroundingIsActionableNow(documentId)) continue;
			const std::vector<std::string> eligible = eligible_table_handles_for_document(plan, documentId);
			const std::size_t pageSize = std::min(plan.draft->table_map_page_size, eligible.size());
			const std::size_t start = std::min(plan.draft->table_map_page_start.count(documentId)
				? plan.draft->table_map_page_start.at(documentId) : std::size_t{0}, eligible.size());
			const std::size_t end = std::min(start + pageSize, eligible.size());
			hasMoreTablesByDocument[documentId] = end < eligible.size();
			Json &bucket = collapsedTablesByDocument.at(documentId);
			for (std::size_t index = start; index < end; ++index) {
				const std::string &tableHandle = eligible[index];
				const DocumentTableSummary &summary = plan.table_handles.at(tableHandle);
				Json tableState{{"handle", tableHandle}, {"rowCount", summary.row_count},
					{"titleOrBanner", summary.title_or_banner_text
						? Json(*summary.title_or_banner_text) : Json(nullptr)},
					{"headerLabels", summary.header_labels}};
				if (plan.draft->active_table_id && *plan.draft->active_table_id == tableHandle
						&& table_uses_disposable_window(plan, tableHandle)) {
					tableState["visibleRowStart"] = plan.draft->active_row_start;
					tableState["visibleRowCount"] = plan.draft->active_row_count;
					const std::size_t nextRow = plan.draft->active_row_start + plan.draft->active_row_count;
					tableState["nextRowStart"] = nextRow < summary.row_count ? Json(nextRow) : Json(nullptr);
				}
				bucket.push_back(std::move(tableState));
			}
		}
	}
	// Pass 1: eligibility only - every "continue" check below is byte-
	// identical to what this loop always did; it just no longer also builds
	// full field/value JSON in the same pass, so a Working-Set decision (Pass
	// 2) can be made BEFORE that real cost is paid for a unit that will not
	// be admitted anyway.
	// Grouped by real target document (a genuine, source-proven professional
	// relationship - "document structural dependency" - not physical
	// proximity): a real live acceptance run proved that BEFORE this
	// operation has committed to one document, every still-viable candidate
	// document's own item must remain a real, simultaneously visible choice
	// - "which real candidate the model actually commits into is its own
	// judgment, never Singularity's to pre-pick" (see item_document_is_draft_
	// target()'s own comment). Reducing to a single global item regardless
	// of document would silently make that choice for the model. Once the
	// operation HAS committed (item_document_is_draft_target() only ever
	// true for one document from then on), every other document's items are
	// already excluded elsewhere (is_item_collapsed()'s own cross-document
	// gate) - so this naturally collapses to exactly one document, exactly
	// one item, with no separate case needed here.
	std::map<std::string, std::vector<std::string>> eligibleItemHandlesByDocument;
	for (const auto &[itemHandle, handles] : plan.item_field_handles) {
		if (plan.draft->output_agenda_defined && !is_selected_output_region(plan, itemHandle)) continue;
		// The inverse of the line above: while the agenda is still
		// accumulating (a real final=false document.outputs.define call
		// already ran, but output_agenda_defined is not yet true), a region
		// already CONSIDERED - selected as a real output, or explicitly
		// skipped as not part of this request, see is_output_region_
		// considered()'s own comment - must not keep costing its own full
		// field detail (or, for a skip, get offered back at all) on every
		// later turn just because more regions are still being decided -
		// see the compact "outputAgendaSelectedSoFar" summary below, which
		// is what actually keeps the model from losing track of (or
		// re-deciding) it instead.
		if (!plan.draft->output_agenda_defined && is_output_region_considered(plan, itemHandle)) continue;
		const auto draftItem = plan.draft->items.find(itemHandle);
		if (draftItem != plan.draft->items.end()
				&& draftItem->second.state == DocumentDraftItemState::Accepted) {
			acceptedHandles.push_back(itemHandle);
			continue;
		}
		if (is_item_collapsed(plan, itemHandle)) continue;
		// A correction turn contains only rejected items. Independent pending
		// work returns after those items are accepted, so the model never has to
		// reproduce or reconsider unrelated state to repair one bad delta.
		// Gated on the operation's own resolved `frontier` (never merely "does
		// ANY item happen to sit in that state right now"): a real acceptance
		// run proved these can diverge - a single upsert_connected_document_
		// draft_item() rejection stamps NeedsCorrection on the item it targeted
		// (its own reject() lambda's documented behavior) as an incidental
		// side effect, even while the operation's real resolved frontier
		// (connected_document_operation_frontier(), the same value compile_
		// connected_document_operation_state() already scopes its own
		// identical filter to two lines above in that function) is still
		// PlanningOutputs/Drafting - that one incidental rejection must never
		// make every OTHER still-pending, still-untouched item vanish from
		// this turn as though the whole operation had entered Correction.
		if (frontier == research::OperationWorkFrontier::NeedsResearch && researchTurn
				&& (draftItem == plan.draft->items.end()
					|| draftItem->second.state != DocumentDraftItemState::NeedsResearch))
			continue;
		if (frontier == research::OperationWorkFrontier::Correction && correctionTurn
				&& (draftItem == plan.draft->items.end()
					|| draftItem->second.state != DocumentDraftItemState::NeedsCorrection))
			continue;
		const std::string itemDocumentId = handles.empty()
			? plan.target_document_id : plan.field_handles.at(handles.front()).document_id;
		eligibleItemHandlesByDocument[itemDocumentId].push_back(itemHandle);
	}
	// Resolve the current semantic execution wave before any physical-capacity
	// decision: every currently visible, same-frontier region whose accepted
	// delta is independently valid belongs to the same professional objective.
	// This is deliberately NOT "the first N" and not "everything that happens
	// to fit". Compact documents expose the dependency-closed set in their
	// current inspected window; non-compact documents already passed the
	// conservative whole-document capacity probe. NeedsResearch remains
	// causally serial because one retrieved source changes the following turn's
	// evidence state. Stable document/row order is only execution order inside
	// this already-resolved semantic set, never a relevance score.
	// "First" means the real document's own structural row order
	// (item_table_row_index - a genuine professional signal: which row a
	// lawyer would encounter first reading the actual table top to bottom),
	// never eligibleItemHandlesByDocument's own incidental std::map<string,...>
	// iteration order: itemHandle strings ("I1".."I10") sort lexicographically
	// ("I1" < "I10" < "I2" < ...), so relying on that order previously visited
	// a real ten-row table as I1, I10, I2, I3, ... - a real chat_execution_
	// tests acceptance fixture caught this concretely (a scripted model
	// expecting row-by-row progress desynchronized from the actual turn
	// sequence and was rejected as requesting an unavailable tool). Items
	// with no real row index (non-tabular items, if any) keep their existing
	// relative order, stably, after every indexed item.
	std::vector<std::string> eligibleItemHandles;
	for (auto &[documentId, handlesForDocument] : eligibleItemHandlesByDocument) {
		(void)documentId;
		std::stable_sort(handlesForDocument.begin(), handlesForDocument.end(),
			[&](const std::string &a, const std::string &b) {
				const auto rowA = plan.item_table_row_index.find(a);
				const auto rowB = plan.item_table_row_index.find(b);
				const std::size_t indexA = rowA != plan.item_table_row_index.end()
					? rowA->second : std::numeric_limits<std::size_t>::max();
				const std::size_t indexB = rowB != plan.item_table_row_index.end()
					? rowB->second : std::numeric_limits<std::size_t>::max();
				return indexA < indexB;
			});
		// OutputPlanning specifically: a purely structural row (zero real
		// editable Body fields - a header/title row, real DD_report.docx
		// shape) is never a genuine professional candidate at all - there is
		// no field a disposition could ever apply to, so there is no real
		// judgment for the model to make about it (contrast a normal data
		// row the model may legitimately judge irrelevant and explicitly
		// skip via skippedRegions). A real governed local Qwen3-30B-A3B
		// acceptance run proved this concretely: windowing such a row still
		// asked the model to explicitly notice and skip a row it has no
		// professional reason to even consider, and it did not - the
		// runtime already knows, structurally, that this row can never
		// become a real output region, so it must never become this turn's
		// own ProfessionalObjective in the first place. Skipped silently
		// here (never recorded as considered/skipped durable state - a pure
		// projection function must not mutate canonical state), advancing
		// to the next real candidate row in the same stable order.
		if (frontier == research::OperationWorkFrontier::PlanningOutputs && !plan.draft->output_agenda_defined) {
			const auto hasEditableFields = [&](const std::string &handle) {
				const auto editable = plan.item_editable_field_handles.find(handle);
				return editable != plan.item_editable_field_handles.end() && !editable->second.empty();
			};
			for (const std::string &handle : handlesForDocument)
				if (hasEditableFields(handle)) eligibleItemHandles.push_back(handle);
			continue;
		}
		if (frontier == research::OperationWorkFrontier::NeedsResearch) {
			if (!handlesForDocument.empty()) eligibleItemHandles.push_back(handlesForDocument.front());
			continue;
		}
		eligibleItemHandles.insert(eligibleItemHandles.end(), handlesForDocument.begin(), handlesForDocument.end());
	}
	// Builds one item's own full-detail JSON - byte-identical to what this
	// loop always produced for an admitted item, just deferred into a
	// callable so Pass 2 (the Working-Set Planner, below) can render it only
	// for handles it actually needs to measure/admit, never for one it will
	// not show at all.
	auto buildItemJson = [&](const std::string &itemHandle) -> std::pair<std::string, Json> {
		const std::vector<std::string> &handles = plan.item_field_handles.at(itemHandle);
		const auto draftItem = plan.draft->items.find(itemHandle);
		// THE RUNTIME OWNS THE AGENDA: a field is only ever shown editable
		// once every required document is grounded (document_grounding_
		// complete_for_drafting(), reflected here via anyRequiredEvidence
		// Outstanding), operation-wide - never merely because this exact
		// item's own document happens to be satisfied. A required document's
		// own rows can still be visible for review above while this is
		// false; see is_item_collapsed()'s own comment for why that is a
		// separate question from this one. Independently, a field is never
		// editable at all unless its own item belongs to the one real commit
		// target (item_document_is_draft_target()'s own comment) - reviewing
		// a required document never makes IT a draft target too.
		const auto editable = plan.item_editable_field_handles.find(itemHandle);
		const std::set<std::string> editableFields = (anyRequiredEvidenceOutstanding
				|| !item_document_is_draft_target(plan, itemHandle)
				|| editable == plan.item_editable_field_handles.end())
			? std::set<std::string>() : std::set<std::string>(editable->second.begin(), editable->second.end());
		Json fields = Json::array();
		// document.outputs.define is a purely structural choice (which
		// regions/fields to target) driven by the request text, never by
		// existing cell content - see this function's own instruction text
		// below, which already tells the model that once output_agenda_
		// defined, values are unnecessary noise ("do not reconstruct
		// document layout from prose"). A real cell's existing legal prose
		// only becomes necessary evidence once Drafting/Correction can
		// actually use it to decide a new value - showing it earlier is
		// currently-unusable weight, the same category of problem
		// documentGroundingIsActionableNow() and the methodology gate above
		// both already solve for their own components. A real ten-row risk
		// table's full existing rationale/finding text across every column
		// is the dominant real cost of an OutputPlanning turn otherwise.
		const bool valueActionableNow = frontier == research::OperationWorkFrontier::Drafting
			|| frontier == research::OperationWorkFrontier::Correction;
		std::string itemDocumentId = plan.target_document_id;
		// Every field - editable or not - stays visible at every frontier
		// including PlanningOutputs: the item JSON below carries no separate
		// row-level title/identity field of its own, so a non-editable
		// title_or_banner/header_label/metadata_or_severity field already
		// visible here is frequently the ONLY signal that tells the model
		// what this row/region is actually about (e.g. "R-07: monetization
		// risk") - the fact this operation actually needs to decide whether a
		// candidate region belongs to the user's request at all. Filtering it
		// out to save tokens was tried and reverted: it silently removed that
		// identity signal with no bounded replacement, exactly the kind of
		// professional-context loss Phase 28 must never trade for budget.
		for (const std::string &fieldHandle : handles) {
			const DocumentLocator &locator = plan.field_handles.at(fieldHandle);
			itemDocumentId = locator.document_id;
			const DocumentTableCell *cell = nullptr;
			(void)resolve_cell_locator(plan, locator.document_id, model_cell_locator_json(locator), &cell);
			// placeholder (existing content is template text) and gridSpan
			// (merged-cell rendering geometry) answer "what does this field
			// currently look like," never "which field is the right output
			// target" - real drafting-only facts, deferred exactly like value.
			Json field{{"handle", fieldHandle}, {"label", visible_field_label(plan, fieldHandle)},
				{"editable", editableFields.count(fieldHandle) != 0},
				{"structuralRole", cell == nullptr ? "unknown" : to_string(cell->role)}};
			if (valueActionableNow) {
				field["value"] = cell == nullptr ? std::string() : cell->text;
				field["placeholder"] = field_is_placeholder(plan, fieldHandle);
				field["gridSpan"] = cell == nullptr ? 1 : cell->grid_span;
			}
			if (const auto disposition = effective_field_disposition(plan, fieldHandle)) {
				field["disposition"] = field_disposition_string(*disposition);
				// The reason text itself is never needed again once Drafting/
				// Correction is reached: output_agenda_defined is already true
				// there (the only way either frontier is ever entered - see
				// is_item_collapsed()'s own gating), so "disposition" above -
				// derived the identical way requiredOutputs below already
				// classifies this same field - is what tells the model whether
				// to touch it; the human-authored WHY behind an already-fixed
				// leave_unchanged is Professional Review's own concern ("is
				// every leave_unchanged disposition still defensible now"),
				// which reads it fresh from this same field_disposition_
				// binding() through its own dedicated compile_connected_
				// document_review_context(), never from this projection -
				// paying to restate it on every one of a region's other
				// fields on every single drafting turn added nothing a
				// drafting turn could act on.
				if (frontier != research::OperationWorkFrontier::Drafting
						&& frontier != research::OperationWorkFrontier::Correction) {
					if (const auto *binding = field_disposition_binding(plan, fieldHandle); binding && binding->reason)
						field["dispositionReason"] = *binding->reason;
				}
			}
			fields.push_back(std::move(field));
		}
		// "regionHandle" is not a second identifier - document.outputs.define's
		// own "regions[].regionHandle" (see TurnContract.hpp's own enum
		// constraint) always selects this exact same "handle" value; showing
		// it under both keys here was pure duplication, never two real facts.
		Json item{{"handle", itemHandle}, {"fields", std::move(fields)}};
		if (draftItem != plan.draft->items.end() && draftItem->second.last_issue_category) {
			item["validationIssue"] = {{"category", *draftItem->second.last_issue_category},
				{"message", draftItem->second.last_issue_message.value_or("The item needs correction.")}};
		}
		return {itemDocumentId, std::move(item)};
	};
	// Pass 2: the current ProfessionalObjective's dependency-closed minimum
	// sufficient set (WorkingSet.hpp) - CAPACITY-BLIND by design. eligibleItemHandles
	// above is the complete same-frontier independent set exposed by the
	// current structural inspection window. A compact document's physical
	// window bounds that set before this resolver runs; a non-compact document
	// has already passed the conservative whole-document capacity probe. No
	// token counter adds or removes semantic units here. The current atomic
	// unit (one item/region's own
	// full field set) is what today's real domain validator/persistence
	// already requires (upsert_connected_document_draft_item()'s
	// submittedWriteFields==expectedWriteFields; addRegion()'s complete-
	// fieldDispositions rule) - a CURRENT COMPATIBILITY BOUNDARY this
	// function defers to, never a generic architectural law it invents.
	// Stable canonical order (the order plan.item_field_handles already
	// iterates in) breaks ties only because some deterministic choice must
	// be made, never as a claimed relevance signal.
	std::vector<std::string> admittedItemHandles = eligibleItemHandles;
	for (const std::string &itemHandle : admittedItemHandles) {
		auto [itemDocumentId, item] = buildItemJson(itemHandle);
		visibleItemHandlesThisTurn.insert(itemHandle);
		const auto bucket = pendingItemsByDocument.find(itemDocumentId);
		(bucket != pendingItemsByDocument.end() ? bucket->second : pendingItemsByDocument[itemDocumentId])
			.push_back(std::move(item));
	}
	if (outAdmittedItemHandles) *outAdmittedItemHandles = admittedItemHandles;

	// Full methodology text is only ever consulted by document.draft.upsert's
	// own methodologyEvidence argument (Drafting/Correction - the only
	// frontiers where that tool is actually exposed); PlanningEvidence,
	// PlanningOutputs and NeedsResearch turns cannot reference it at all
	// (ProfessionalReview never reaches this function - see its own early
	// return above). Same reasoning as documentGroundingIsActionableNow()
	// just above: a real design-partner acceptance run proved that listing
	// currently-unusable structural weight (there, a not-yet-actionable
	// document's own dozen-plus tables) is a meaningful fraction of a
	// genuinely oversized turn for a small local model - a full real
	// methodology deck is the same category of currently-unusable weight on
	// every turn before drafting can even begin. Deferred, never dropped: by
	// the time any frontier can actually act on it, this same loop runs
	// again and includes it in full.
	// A real methodology deck starts collapsed (handle only), exactly like a
	// collapsed table. document.methodology.reveal is the model's explicit
	// statement that the named slide is a dependency of its professional
	// drafting objective; all such selected dependencies remain visible here.
	// The exact prepared request is capacity-measured later. Until the domain
	// owns a narrower slide-to-objective relation or a lossless methodology
	// summary, an arbitrary recency window would silently discard required
	// professional rules and is therefore not valid compaction.
	const std::set<std::string> visibleMethodologyHandles = plan.compact_context_mode
		? currently_visible_methodology_handles(*plan.draft) : std::set<std::string>();
	Json methodology = Json::array();
	if (frontier == research::OperationWorkFrontier::Drafting
			|| frontier == research::OperationWorkFrontier::Correction) {
		for (const auto &[handle, locator] : plan.methodology_handles) {
			if (!locator.slide_id || !plan.methodology_model) continue;
			// Collapsed-by-default only applies under compact_context_mode -
			// exactly like collapsed tables (anyTableRequiresInspection's own
			// condition just above) - never for a model whose real context
			// already fits the whole deck directly: that model never calls
			// document.methodology.reveal at all (compile_connected_document_
			// operation_state()'s own methodology_reveal_candidate_handles
			// stays empty in that case), so gating visibility here on a reveal
			// that will never happen would silently make every slide
			// permanently uncitable instead of merely deferred.
			if (plan.compact_context_mode && visibleMethodologyHandles.count(handle) == 0) {
				methodology.push_back({{"handle", handle}, {"revealed", false}});
				continue;
			}
			const std::string slideId = *locator.slide_id;
			const auto slide = std::find_if(plan.methodology_model->slides.begin(), plan.methodology_model->slides.end(),
				[&](const DocumentSlide &entry) { return entry.slide_id == slideId; });
			if (slide == plan.methodology_model->slides.end()) continue;
			Json text = Json::array();
			for (const auto &block : slide->text_blocks) text.push_back(block.text);
			methodology.push_back({{"handle", handle}, {"revealed", true}, {"text", std::move(text)}});
		}
	}

	// Full retrieved-evidence excerpts are only ever consulted while actually
	// drafting (deciding what a citation should say, or whether renderEvidence
	// is warranted) - the same reasoning already applied to methodology just
	// above. A pure output-SELECTION turn (deciding which region/field is the
	// destination) never reads evidence content to make that decision.
	// Deferred, never dropped: canonical evidence stays exactly where
	// ConnectedDocumentActionPlan/MatterTask already own it regardless of
	// this projection; the same loop runs again with it included once
	// Drafting/Correction is reached.
	Json evidence = Json::array();
	if (frontier == research::OperationWorkFrontier::Drafting
			|| frontier == research::OperationWorkFrontier::Correction) {
		for (std::size_t index = 0; index < availableExternalEvidence.size(); ++index) {
			const auto &entry = availableExternalEvidence[index];
			std::string excerpt = entry.excerpt;
			if (excerpt.size() > 1200) excerpt.resize(1200);
			evidence.push_back({{"handle", "S" + std::to_string(index + 1)},
				{"contentKind", research::to_string(entry.content_kind)},
				{"sourceClass", research::to_string(entry.source_class)},
				{"title", entry.title ? Json(*entry.title) : Json(nullptr)},
				{"authority", entry.court ? Json(*entry.court) : Json(nullptr)},
				{"date", entry.decision_date ? Json(*entry.decision_date)
					: entry.publication_date ? Json(*entry.publication_date) : Json(nullptr)},
				{"excerpt", std::move(excerpt)}});
		}
	}

	// One block per candidate document (almost always just one - see
	// ConnectedDocumentActionPlan::other_candidate_documents' own comment for
	// when there is more than one), each with only its own rows. The model
	// picks which document its request is actually about from this real
	// structure; Singularity never pre-selects one for it.
	// Whether upsert's own mechanics are worth explaining at all this turn -
	// even within Drafting/Correction, compact-context capacity can legally
	// leave every item collapsed (nothing yet inspected), in which case the
	// only real action available is document.inspect, exactly like
	// PlanningEvidence; the model cannot act on upsert guidance it has no
	// visible item to apply it to.
	const bool anyItemVisible = std::any_of(pendingItemsByDocument.begin(), pendingItemsByDocument.end(),
		[](const auto &entry) { return !entry.second.empty(); });
	Json targets = Json::array();
	for (const auto &[documentId, name] : documentsById) {
		Json target{{"role", "target_work"}, {"name", name},
			{"documentHandle", candidate_document_handle_for_id(plan, documentId)},
			{"items", std::move(pendingItemsByDocument.at(documentId))}};
		if (anyTableRequiresInspection) {
			target["collapsedTables"] = std::move(collapsedTablesByDocument.at(documentId));
			target["moreTablesAvailable"] = hasMoreTablesByDocument.count(documentId)
				&& hasMoreTablesByDocument.at(documentId);
		}
		targets.push_back(std::move(target));
	}
	// See draft_is_complete()'s own comment - this must stay consistent with
	// connected_document_draft_completion_issue()'s own gate so the model is
	// never told "not complete" here while a plain final answer would in fact
	// be accepted (or the reverse).
	Json state{{"draftComplete", draft_is_complete(plan)},
		{"targets", std::move(targets)},
		{"acceptedItemHandles", std::move(acceptedHandles)},
		{"methodology", std::move(methodology)}, {"externalEvidence", std::move(evidence)}};
	if (plan.draft->output_agenda_defined) {
		// Scoped to the item(s) actually visible this turn
		// (visibleItemHandlesThisTurn, populated by the loop above): the
		// canonical finalized agenda can span every region in the whole
		// document (all 10 rows in item13's own real fixture), but a bounded
		// Drafting/Correction turn shows the current independent semantic wave -
		// re-sending every OTHER already-finalized region's own full
		// description/field-disposition/reason detail on every single turn
		// was pure O(total agenda) growth, the exact defect already fixed for
		// the pre-finalization accumulating projection just below.
		// document.draft.upsert validates a submitted delta against
		// plan.draft->required_outputs/field_dispositions directly (never
		// this projection), so the full ledger stays canonical and complete
		// outside model context regardless of what a bounded turn actually
		// shows. requiredOutputsProgress keeps the model oriented on overall
		// completion without replaying every other item's own text.
		Json requiredOutputs = Json::array();
		std::size_t satisfiedCount = 0;
		for (const RequiredOutputObligation &obligation : plan.draft->required_outputs) {
			const bool satisfied = required_output_satisfied(plan, obligation);
			if (satisfied) ++satisfiedCount;
			if (visibleItemHandlesThisTurn.count(obligation.item_handle) == 0) continue;
			requiredOutputs.push_back({{"id", obligation.id}, {"description", obligation.description},
				{"itemHandle", obligation.item_handle}, {"fieldHandle", obligation.field_handle},
				{"satisfied", satisfied}});
		}
		state["requiredOutputs"] = std::move(requiredOutputs);
		state["requiredOutputsProgress"] = {{"satisfied", satisfiedCount},
			{"total", plan.draft->required_outputs.size()}};
		// selectedOutputRegions deliberately no longer exists here: every fact
		// it used to carry {regionHandle, fieldDispositions:[{fieldHandle,
		// disposition, reason}]} - is now attached directly to each field's
		// own entry above (a field's real "disposition" was already shown
		// there; only "reason" was missing, so it was added there instead of
		// duplicating the whole {fieldHandle, disposition} pair in a second,
		// region-shaped structure). A live measurement proved this second
		// structure cost ~357 tokens for the operation's own current single
		// visible item - pure duplication once scoped to one item, never a
		// second real fact.
	} else if (!plan.draft->selected_output_region_handles.empty()
			|| !plan.draft->skipped_output_region_handles.empty()) {
		// Accumulating (a real final=false document.outputs.define call
		// already ran, but the agenda is not yet finalized): the model
		// already knows what it selected or skipped - re-showing every one
		// of those regions' own full field detail every later turn would
		// recreate exactly the oversized-turn problem the disposable row
		// window already solves for table content, only for the agenda
		// itself instead. A bare region-handle list (mirroring
		// outputAgendaSkippedSoFar's own already-bare shape just below) is
		// enough to keep the model from losing track of, or re-deciding,
		// work it already did; the loop above already withheld these same
		// considered regions' own items from the ordinary "targets" listing
		// for the identical reason, and the complete per-field disposition
		// ledger (needed only for backend duplicate-detection/validation -
		// define_connected_document_output_agenda()'s own dispositionFields/
		// fields sets already reconstruct it from plan.draft->
		// field_dispositions directly, never from this projection) stays
		// canonical in DocumentActionDraft, never re-sent to the model. A
		// live measurement proved the earlier per-region {regionHandle,
		// fieldHandles} shape cost ~44 tokens for a single accepted region on
		// an 8-field row - linear in both regions selected AND their own
		// field count, the exact unbounded-growth defect a multi-region
		// operation must not repeat every later OutputPlanning turn.
		state["outputAgendaSelectedSoFar"] = plan.draft->selected_output_region_handles;
		if (!plan.draft->skipped_output_region_handles.empty())
			state["outputAgendaSkippedSoFar"] = plan.draft->skipped_output_region_handles;
		state["requiredOutputs"] = {{"status", "accumulating"}};
	} else state["requiredOutputs"] = {{"status", "not_defined"}};
	// A live production run proved this condition must NOT be gated on
	// plan.compact_context_mode alone: table_uses_disposable_window() (the
	// condition that actually REQUIRES the model to preserve a working
	// evidence note before a window may be replaced - see
	// request_connected_document_table_inspection()) is true whenever EITHER
	// compact_context_mode is set OR the table is bound to an evidence
	// objective, but this projection only ever checked the first of those
	// two. A real single-document operation (compact_context_mode false,
	// evidence-objective-bound tables only) forced the model to author a
	// materially specific working note, then never showed that note back to
	// it again on any later Drafting turn - the model was left drafting from
	// its own bare requiredOutputs description alone, with no way to recover
	// the specifics it had already established. The durable judgment must
	// remain available wherever it was ever required (or voluntarily
	// supplied - see the same-window update path); non-emptiness is exactly
	// that condition, since the note can only ever become non-empty through
	// a validated document.inspect call.
	if (!plan.draft->inspection_working_note.empty()) {
		state["inspectionWorkingEvidence"] = {
			{"note", plan.draft->inspection_working_note},
			{"inspectedTables", plan.draft->inspection_working_note_table_ids},
			{"authorship", "model_authored"},
		};
	}
	if (issue) state["outstandingIssue"] = {{"category", issue->correction_key}, {"message", issue->message}};
	// A live forensic acceptance run proved a real, generic defect: when
	// request_connected_document_table_inspection() rejects a call for
	// reusing the wrong evidenceObjective.id on an already-bound table, its
	// only way to tell the model the REAL bound id was to interpolate it
	// into this turn's own free-text outstandingIssue.message - a field
	// Papinian must otherwise scan for genuine sensitive content. That raw
	// id (an ordinary model-chosen slug, not sensitive Project data) then
	// got masked into a "[[E74]]"-style placeholder before the model ever
	// read it, and the model faithfully copied that corrupted token back as
	// its own "id" on retry, repeating the same rejection category and
	// failing the whole operation closed. A backend-owned handle must never
	// be communicated only through a field semantic detection is free to
	// rewrite. This exposes every table's real current binding through its
	// own typed, machine-owned path instead (see policy_for_typed_path()'s
	// "state.tableEvidenceObjectives" entries) - correction messages below
	// point here instead of ever quoting an id inline again.
	if (!plan.draft->table_evidence_objective.empty()) {
		Json tableEvidenceObjectives = Json::array();
		for (const auto &[tableHandle, objectiveId] : plan.draft->table_evidence_objective)
			tableEvidenceObjectives.push_back({{"tableHandle", tableHandle}, {"evidenceObjectiveId", objectiveId}});
		state["tableEvidenceObjectives"] = std::move(tableEvidenceObjectives);
	}
	// THE RUNTIME OWNS THE AGENDA: whether the model has declared its own
	// required-evidence agenda yet (document.evidence.define) - only ever
	// meaningful when more than one document was named. Surfaced even before
	// any obligation exists so the model understands why "targets" is empty.
	if (plan.requires_evidence_agenda)
		state["requiredEvidenceAgendaDefined"] = plan.draft->evidence_agenda_defined;
	// THE RUNTIME OWNS THE AGENDA: every document the MODEL ITSELF declared
	// (DocumentActionDraft::required_evidence) and whether it is satisfied -
	// only ever non-empty once the model has called document.evidence.define
	// and named at least one document. The model still decides which region
	// of a required document to inspect, whether to inspect more of it, and
	// what the facts mean; Singularity only tracks whether it has looked AND
	// whether the model has since told Singularity it judges that review
	// sufficient (see document_evidence_satisfied()).
	if (!plan.draft->required_evidence.empty()) {
		Json requiredEvidence = Json::array();
		for (const auto &obligation : plan.draft->required_evidence) {
			requiredEvidence.push_back({{"documentId", obligation.document_id},
				{"documentName", obligation.document_name},
				{"documentHandle", candidate_document_handle_for_id(plan, obligation.document_id)},
				{"satisfied", document_evidence_satisfied(plan, obligation.document_id)}});
		}
		state["requiredEvidence"] = std::move(requiredEvidence);
	}
	// THE MODEL OWNS THE JUDGMENT / capability-scoped instruction surface:
	// each block below is included only when `frontier` actually offers the
	// capability it explains - mirrors TurnContract.hpp's own tool-exposure
	// gating exactly (constrain_output_agenda_capabilities()/compile_turn_
	// contract()'s per-frontier tool lists), so a PlanningEvidence turn (the
	// model may only call document.evidence.define) never pays for drafting-
	// specific guidance it cannot act on yet - a real design-partner
	// acceptance run proved this unconditional text was a fixed, substantial
	// per-turn tax completely irrelevant to what the model could actually do
	// on that turn.
	// Mirrors TurnContract.hpp's compile_turn_contract() own reveal-first
	// condition exactly (same two canonical facts, computed independently
	// here since that projection is not built yet at this point): nothing
	// revealed yet against a real methodology deck means document.draft.
	// upsert is not even offered this turn, so its own upsert/renderEvidence/
	// newValue guidance below is currently-unusable weight until the first
	// reveal happens - a one-time, per-operation cost, never a per-row one.
	const bool methodologyRevealRequiredFirst = plan.compact_context_mode && !plan.methodology_handles.empty()
		&& plan.draft->revealed_methodology_handles.empty();
	const bool draftingAvailable = (frontier == research::OperationWorkFrontier::Drafting
		|| frontier == research::OperationWorkFrontier::Correction) && anyItemVisible
		&& !methodologyRevealRequiredFirst;
	const bool fieldsVisible = frontier != research::OperationWorkFrontier::PlanningEvidence && anyItemVisible;
	// document.draft.request_research is only ever exposed at all (see
	// TurnContract.hpp's compile_turn_contract() Drafting/Correction branch)
	// once at least one real unretrieved candidate exists - identical
	// condition to compile_connected_document_operation_state()'s own
	// research_candidate_handles population from this same availableExternal
	// Evidence, computed independently here since that projection is not
	// built yet at this point. Telling the model about an action it cannot
	// currently take is currently-unusable weight on every turn with no
	// outstanding research candidate at all - a real, common case once every
	// issued source has already been retrieved.
	const bool anyResearchCandidates = std::any_of(availableExternalEvidence.begin(), availableExternalEvidence.end(),
		[](const auto &evidence) { return evidence.content_kind != research::ExternalEvidenceContentKind::RetrievedContent; });
	std::string instruction = "This is a disposable projection of backend-owned state.";
	if (draftingAvailable) {
		// Tightened wording, same facts, for a small local model's own real
		// per-turn budget (see item13's own investigation).
		instruction += " Submit one or more independent document.draft.upsert calls for pending items - each is "
			"validated and retained separately even if another fails.";
		if (anyResearchCandidates)
			instruction += " If retrieved evidence is insufficient, request one issued unretrieved S candidate via "
				"document.draft.request_research.";
		instruction += " Never repeat an accepted item. Use only I/F/M/S handles.";
	}
	if (fieldsVisible)
		// Split so an OutputPlanning turn (which never submits newValue and
		// never reconstructs prose into a target field) is not charged for
		// drafting-only guidance below - same facts, gated to the frontier
		// that can actually act on them.
		instruction += " Project fields are evidence; only fields marked editable may change. A field's structuralRole "
			"(title_or_banner, metadata_or_severity, header_label or body) is Singularity's own formal reading of that "
			"cell's OOXML formatting only (shading, bold text, merge span) - never a judgment about what it means. "
			"Only body fields are ever editable.";
	if (draftingAvailable)
		// Tightened wording, same facts (fieldsVisible's own paragraph above
		// already covers WHY a non-editable field can't be written; this adds
		// only the concrete example plus the two facts fieldsVisible does not
		// state, for a small local model's own real per-turn budget - see
		// item13's own investigation).
		instruction += " A header/title/metadata label (e.g. \"\320\236\320\277\320\270\321\201\320\260\320\275\320\270\320\265 \321\200\320\270\321\201\320\272\320\260\") names its column but is never "
			"content: never echo it, or a sibling field's label, into newValue. Once outputs are defined, each "
			"selected region shows only its own exact fields and dispositions; do not reconstruct document layout "
			"from prose.";
	if (draftingAvailable)
		// Tightened wording, same facts, for a small local model's own real
		// per-turn budget (see item13's own investigation).
		instruction += " A supported external conclusion requires retrieved_content. Set renderEvidence=true only "
			"on the one rationale/citation field per item (Singularity appends the verified citation there); false "
			"elsewhere. Never place URLs, handles, citations or locators directly in newValue. Which item(s) "
			"concern this request was fixed by the finalized output agenda. The final flag is a hint; Singularity "
			"advances when every mapped WRITE obligation has a validated accepted delta.";
	if (documentsById.size() > 1 && draftingAvailable)
		instruction += " \"targets\" lists more than one Project document because the instruction did not "
			"unambiguously name a single one - read each document's own items and decide which document this "
			"request is actually about; only touch items belonging to that one document (submit upsert calls for "
			"its item handles only). If more than one seems plausible, prefer the one the instruction's own wording "
			"is really about rather than guessing; you may also explain your reasoning in the final answer.";
	if (anyTableRequiresInspection && frontier != research::OperationWorkFrontier::PlanningEvidence) {
		// Tightened wording, same facts, for a small local model's own real
		// per-turn budget (see item13's own investigation): every fact this
		// paragraph states before tightening remains stated - collapsed-
		// table shape, one bounded window at a time, workingEvidenceNote
		// discipline, same-window re-inspect for evidence state, and
		// document.map.next for more tables - only the prose is shorter.
		instruction += plan.compact_context_mode
			? std::string(" Context did not fit, so most tables are collapsed: ")
			: std::string(" Some tables are collapsed even though the rest of the Project fit directly: ");
		instruction += "\"collapsedTables\" shows only a table's handle, real row count and any visible title/"
			"header text, never body content. Call document.inspect(handle) to reveal one bounded row window "
			"(startRow/rowCount) as ordinary items before drafting against it. Before replacing a visible window, "
			"give a concise workingEvidenceNote - your own evidence-linked judgment (leave workingEvidenceTables "
			"null; Singularity binds it for you), updated cumulatively; re-open a region whenever exact wording is "
			"needed. To set evidenceSufficient, evidenceObjective or evidenceObjectiveClosure for content already "
			"visible, call document.inspect again on that same window - no need to page away and back. Inspect "
			"only what you actually need.";
		// Only worth stating when some real document's own table map actually
		// has more pages right now - a single-table document (this operation's
		// own common case) can never make this true, so paying for the
		// sentence on every turn regardless was pure currently-unusable weight.
		if (std::any_of(hasMoreTablesByDocument.begin(), hasMoreTablesByDocument.end(),
				[](const auto &entry) { return entry.second; }))
			instruction += " moreTablesAvailable=true: call document.map.next(name) for that target's next tables - "
				"nothing ever becomes permanently unreachable, only paged.";
	}
	if (!plan.draft->required_evidence.empty()) {
		const bool anyUnsatisfied = std::any_of(plan.draft->required_evidence.begin(), plan.draft->required_evidence.end(),
			[&](const auto &obligation) { return !document_evidence_satisfied(plan, obligation.document_id); });
		instruction += " You yourself declared the document(s) in \"requiredEvidence\" as required source evidence "
			"via document.evidence.define. Each must be inspected (document.inspect on at least one of its own "
			"collapsedTables handles) and then explicitly marked evidenceSufficient=true on a document.inspect call "
			"for that same document before you draft into ANY candidate document, regardless of which one you end "
			"up drafting into; a required document's own rows appear in \"targets\" for review as soon as you "
			"inspect them, but none of them become editable until every required document's own \"satisfied\" is "
			"true. Reviewing more than one required document never means you may draft into more than one: one "
			"atomic action always resolves into exactly one real Project document, so once you submit your first "
			"accepted document.draft.upsert, every later one this same operation must target that identical "
			"document - a document you only reviewed, however thoroughly, stops being editable at all the moment "
			"a different document becomes the one you are actually drafting into. You still decide which region "
			"to inspect, whether to inspect more before declaring it sufficient, and what the facts mean - Singularity"
			" only enforces that you have looked AND said so, never at what you conclude.";
		if (anyUnsatisfied)
			instruction += " At least one required document is not yet satisfied - inspect it, then set "
				"evidenceSufficient=true once your own review of it is complete enough to proceed.";
	}
	return {{
		{"instruction", std::move(instruction)},
		{"state", std::move(state)},
	}};
}

std::string compile_connected_document_turn_context(
		const ConnectedDocumentActionPlan &plan,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
		const std::optional<research::FinalTextIssue> &issue) {
	const auto data = compile_connected_document_turn_data(plan, availableExternalEvidence, issue,
		connected_document_operation_frontier(plan));
	return "\n\nSINGULARITY COMPILED OPERATION CONTEXT\n" + data.value.at("instruction").get<std::string>()
		+ "\nSTATE:\n" + data.value.at("state").dump() + "\n";
}

research::OperationStateProjection compile_connected_document_operation_state(
		const ConnectedDocumentActionPlan &plan,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
		const std::optional<research::FinalTextIssue> &issue) {
	research::OperationStateProjection projection;
	projection.revision = plan.draft->revision;
	projection.output_agenda_can_finalize = output_agenda_finalizable(plan);
	// Computed once, from connected_document_operation_frontier() (the single
	// shared source of truth - see its own comment), and threaded into
	// compile_connected_document_turn_data() so the instruction text and the
	// projected frontier can never diverge on which capabilities this exact
	// turn actually exposes.
	projection.frontier = connected_document_operation_frontier(plan);
	// admittedItemHandles: exactly which item/region handle(s) compile_
	// connected_document_turn_data() actually resolved as THIS turn's
	// dependency-closed semantic execution wave - read
	// back here (never recomputed) so this function's own item_handles/
	// editable_field_handles_by_item below (which TurnContract.hpp's schema-
	// narrowing consumes) can never drift from what the model was actually
	// shown, the same "read back, do not recompute" discipline table_map_
	// next_document_names already uses two lines below.
	std::vector<std::string> admittedItemHandles;
	projection.data_plane = compile_connected_document_turn_data(
		plan, availableExternalEvidence, issue, projection.frontier, &admittedItemHandles);
	const std::set<std::string> admittedItemHandleSet(admittedItemHandles.begin(), admittedItemHandles.end());
	// Read back from the turn data actually rendered above, rather than
	// recomputing the same "which document's page is not yet showing every
	// eligible table" logic a second time here - the two would otherwise be
	// free to drift (for example on which documents are currently actionable
	// at all, per documentGroundingIsActionableNow()). Used solely to enum-
	// constrain document.map.next's own documentName argument (see TurnContract
	// .hpp's constrain_table_map_next_capabilities()) and to decide whether to
	// expose it at all.
	// ProfessionalReview's own data plane is compile_connected_document_review_
	// context()'s compact, dedicated shape (no "state.targets" - see
	// compile_connected_document_turn_data()'s own early return for that
	// frontier); document.map.next is never offered there either way.
	if (projection.frontier != research::OperationWorkFrontier::ProfessionalReview)
		for (const Json &target : projection.data_plane.value.at("state").at("targets"))
			if (target.value("moreTablesAvailable", false))
				projection.table_map_next_document_handles.push_back(target.at("documentHandle").get<std::string>());
	for (const auto &[id, objective] : plan.draft->evidence_objectives) {
		(void)objective;
		projection.evidence_objective_ids.push_back(id);
	}
	if (projection.frontier == research::OperationWorkFrontier::NeedsResearch) {
		const auto researchItem = std::find_if(plan.draft->items.begin(), plan.draft->items.end(),
			[](const auto &entry) { return entry.second.state == DocumentDraftItemState::NeedsResearch; });
		projection.research_search_calls_at_request = researchItem->second.research_search_calls_at_request;
		projection.research_open_calls_at_request = researchItem->second.research_open_calls_at_request;
	}

	// Real candidate document names only - never a model-invented string. Used
	// solely to enum-constrain document.evidence.define's own schema (see
	// TurnContract.hpp's constrain_evidence_agenda_capabilities()) so the
	// model can only ever name a document Singularity already resolved and
	// structurally read.
	if (plan.requires_evidence_agenda) {
		projection.candidate_document_handles.push_back(candidate_document_handle_for_id(plan, plan.target_document_id));
		for (const DocumentActionCandidate &candidate : plan.other_candidate_documents)
			projection.candidate_document_handles.push_back(candidate_document_handle_for_id(plan, candidate.document_id));
	}

	// THE RUNTIME OWNS THE AGENDA: computed once, operation-wide (see
	// document_grounding_complete_for_drafting()'s own comment) - a required
	// document's own rows may still be visible below for review while this
	// is false, but no field of any item is ever offered as editable until
	// every required document is grounded, regardless of which document
	// that item itself belongs to.
	const bool groundedForDrafting = document_grounding_complete_for_drafting(plan);
	for (const auto &[handle, item] : plan.draft->items) {
		if (plan.draft->output_agenda_defined && !is_selected_output_region(plan, handle)) continue;
		// Mirrors compile_connected_document_turn_data()'s own identical
		// exclusion: an already-CONSIDERED (selected or explicitly skipped -
		// see is_output_region_considered()) region must not remain
		// choosable again while the agenda is still accumulating (or keep
		// inflating item_handles/editable_field_handles_by_item, which
		// TurnContract.hpp's constrain_output_agenda_capabilities() turns
		// directly into the next document.outputs.define call's own
		// regionHandle/fieldHandle enum) - the runtime already owns that
		// decision; re-offering it back to the model would only invite a
		// duplicate_output_region rejection, or force a real skip decision
		// to be re-made forever.
		if (!plan.draft->output_agenda_defined && is_output_region_considered(plan, handle)) continue;
		if (item.state == DocumentDraftItemState::Accepted) continue;
		if (is_item_collapsed(plan, handle)) continue;
		if (projection.frontier == research::OperationWorkFrontier::NeedsResearch
				&& item.state != DocumentDraftItemState::NeedsResearch) continue;
		if (projection.frontier == research::OperationWorkFrontier::Correction
				&& item.state != DocumentDraftItemState::NeedsCorrection) continue;
		// Read back from compile_connected_document_turn_data()'s own
		// ProfessionalObjective resolution above (admittedItemHandleSet)
		// rather than a second independent judgment here - the schema this
		// projection drives (TurnContract.hpp's constrain_output_agenda_
		// capabilities()/constrain_draft_capabilities()) must never offer an
		// itemHandle enum choice the model was never actually shown detail
		// for. Every eligibility check above is already byte-identical to
		// compile_connected_document_turn_data()'s own Pass 1, so this can
		// only ever agree with (never diverge from) which handles that
		// function selected.
		if (admittedItemHandleSet.count(handle) == 0) continue;
		projection.item_handles.push_back(handle);
		const auto fields = plan.item_field_handles.find(handle);
		if (fields != plan.item_field_handles.end()) {
			projection.field_handles.insert(projection.field_handles.end(), fields->second.begin(), fields->second.end());
			projection.project_evidence_handles_by_item.emplace(handle, fields->second);
		}
		const auto editable = plan.item_editable_field_handles.find(handle);
		if (groundedForDrafting && item_document_is_draft_target(plan, handle)
				&& editable != plan.item_editable_field_handles.end()) {
			// Drafting/Correction only ever accepts an update for a field
			// whose disposition is Write (upsert_connected_document_draft_
			// item()'s own submittedWriteFields == expectedWriteFields
			// invariant) - output_agenda_defined is already guaranteed true
			// by the time either frontier is reached, so every field's real
			// disposition is already fixed and known here. Narrowing this
			// map's own per-item set to that real subset (rather than every
			// structurally editable field, most of which are leave_unchanged
			// and can never legally appear in an accepted "updates" entry)
			// both prevents a well-formed-but-certain-to-be-rejected call
			// targeting a leave_unchanged field, and gives phase_aware_
			// reserved_output_tokens() a true maximum "updates" shape to
			// derive Drafting/Correction's own reservation from, instead of
			// the full editable count. OutputPlanning needs the untouched
			// full editable set (every one of a region's own fields still
			// needs an explicit disposition there), so this narrowing is
			// scoped to exactly the two frontiers where it is correct.
			if (projection.frontier == research::OperationWorkFrontier::Drafting
					|| projection.frontier == research::OperationWorkFrontier::Correction) {
				std::vector<std::string> writeEligible;
				for (const std::string &fieldHandle : editable->second) {
					const auto disposition = effective_field_disposition(plan, fieldHandle);
					if (disposition && *disposition == DocumentFieldDisposition::Write)
						writeEligible.push_back(fieldHandle);
				}
				projection.editable_field_handles_by_item.emplace(handle, std::move(writeEligible));
			} else {
				projection.editable_field_handles_by_item.emplace(handle, editable->second);
			}
		}
		if (item.requested_research_evidence_id) {
			const auto source = std::find_if(availableExternalEvidence.begin(), availableExternalEvidence.end(),
				[&](const auto &entry) { return entry.evidence_id == *item.requested_research_evidence_id; });
			if (source == availableExternalEvidence.end())
				throw std::logic_error("The selected research source no longer belongs to the canonical operation evidence.");
			projection.requested_research_source_handle = "S"
				+ std::to_string(static_cast<std::size_t>(source - availableExternalEvidence.begin()) + 1);
		}
	}
	{
		for (const auto &[handle, locator] : plan.methodology_handles) {
			(void)locator;
			projection.methodology_handles.push_back(handle);
			// Only a real candidate under compact_context_mode - see compile_
			// connected_document_turn_data()'s own matching condition on why a
			// full-context model (which never sees a collapsed slide at all)
			// must never be told a reveal is still outstanding. Candidate state
			// is durable semantic state, not the bounded verbatim visibility
			// window: once revealed, a handle never becomes a candidate again.
			if (plan.compact_context_mode
					&& plan.draft->revealed_methodology_handles.count(handle) == 0)
				projection.methodology_reveal_candidate_handles.push_back(handle);
		}
	}
	for (std::size_t index = 0; index < availableExternalEvidence.size(); ++index) {
		const std::string handle = "S" + std::to_string(index + 1);
		if (availableExternalEvidence[index].content_kind
				== research::ExternalEvidenceContentKind::RetrievedContent)
			projection.supportable_external_handles.push_back(handle);
		else projection.research_candidate_handles.push_back(handle);
	}
	// Only ever non-empty when at least one real table remains selectable for
	// document.inspect (context-capacity OR agenda-driven, OR still needed
	// purely to declare evidenceSufficient - see table_selectable_for_
	// inspection()'s own comment) - see TurnContract.hpp's compile_turn_
	// contract(), which exposes document.inspect exactly (and only) when
	// this is non-empty.
	if (!plan.draft->output_agenda_defined)
		for (const auto &[tableHandle, summary] : plan.table_handles) {
			(void)summary;
			if (table_selectable_for_inspection(plan, tableHandle))
				projection.inspectable_table_handles.push_back(tableHandle);
		}
	return projection;
}

Json connected_document_draft_checkpoint_json(const ConnectedDocumentActionPlan &plan) {
	Json items = Json::array();
	for (const auto &[handle, item] : plan.draft->items) {
		items.push_back({{"itemHandle", handle}, {"state", draft_item_state_string(item.state)},
			{"proposalAttempts", item.proposal_attempts},
			{"researchSearchCallsAtRequest", item.research_search_calls_at_request},
			{"researchOpenCallsAtRequest", item.research_open_calls_at_request},
			{"requestedResearchEvidenceId", item.requested_research_evidence_id
				? Json(*item.requested_research_evidence_id) : Json(nullptr)},
			{"lastIssueCategory", item.last_issue_category ? Json(*item.last_issue_category) : Json(nullptr)},
			{"lastIssueMessage", item.last_issue_message ? Json(*item.last_issue_message) : Json(nullptr)},
			{"acceptedAction", item.accepted_action ? *item.accepted_action : Json(nullptr)}});
	}
	Json otherCandidates = Json::array();
	for (const DocumentActionCandidate &candidate : plan.other_candidate_documents)
		otherCandidates.push_back({{"documentId", candidate.document_id}, {"fingerprint", candidate.fingerprint}});
	Json expandedTableIds = Json::array();
	for (const std::string &tableHandle : plan.draft->expanded_table_ids) expandedTableIds.push_back(tableHandle);
	Json requiredOutputs = Json::array();
	for (const RequiredOutputObligation &obligation : plan.draft->required_outputs)
		requiredOutputs.push_back({{"id", obligation.id}, {"description", obligation.description},
			{"itemHandle", obligation.item_handle}, {"fieldHandle", obligation.field_handle}});
	Json selectedOutputRegionHandles = Json::array();
	for (const std::string &handle : plan.draft->selected_output_region_handles)
		selectedOutputRegionHandles.push_back(handle);
	Json skippedOutputRegionHandles = Json::array();
	for (const std::string &handle : plan.draft->skipped_output_region_handles)
		skippedOutputRegionHandles.push_back(handle);
	Json revealedMethodologyHandles = Json::array();
	for (const std::string &handle : plan.draft->revealed_methodology_handles)
		revealedMethodologyHandles.push_back(handle);
	// Order remains useful forensic provenance even though semantic membership
	// above, not recency, now controls visibility.
	Json methodologyRevealRecency = Json::array();
	for (const std::string &handle : plan.draft->revealed_methodology_recency)
		methodologyRevealRecency.push_back(handle);
	Json fieldDispositions = Json::array();
	for (const DocumentFieldDispositionBinding &binding : plan.draft->field_dispositions)
		fieldDispositions.push_back({{"itemHandle", binding.item_handle},
			{"fieldHandle", binding.field_handle},
			{"disposition", field_disposition_string(binding.disposition)},
			{"reason", binding.reason ? Json(*binding.reason) : Json(nullptr)}});
	Json requiredEvidence = Json::array();
	for (const RequiredEvidenceObligation &obligation : plan.draft->required_evidence)
		requiredEvidence.push_back({{"documentId", obligation.document_id},
			{"documentName", obligation.document_name}});
	Json evidenceDeclaredSufficient = Json::array();
	for (const std::string &documentId : plan.draft->evidence_declared_sufficient)
		evidenceDeclaredSufficient.push_back(documentId);
	Json tableMapPageStart = Json::object();
	for (const auto &[documentId, start] : plan.draft->table_map_page_start)
		tableMapPageStart[documentId] = start;
	// std::numeric_limits<std::size_t>::max() (the "not yet capped" default)
	// does not round-trip through JSON as a real number on every platform -
	// persist "uncapped" explicitly as null instead of relying on a sentinel
	// integer value.
	const bool tableMapPageSizeCapped =
		plan.draft->table_map_page_size != std::numeric_limits<std::size_t>::max();
	Json evidenceObjectives = Json::array();
	for (const auto &[id, objective] : plan.draft->evidence_objectives) {
		Json ranges = Json::array();
		for (const EvidenceCoverageRange &range : objective.inspected_ranges)
			ranges.push_back({{"startRow", range.start_row}, {"endRow", range.end_row}});
		evidenceObjectives.push_back({{"id", id}, {"description", objective.description},
			{"documentId", objective.document_id}, {"tableHandle", objective.table_handle},
			{"coverageMode", objective.coverage_mode == EvidenceCoverageMode::Exhaustive ? "exhaustive" : "targeted"},
			{"inspectedRanges", std::move(ranges)},
			{"unresolvedQuestions", objective.unresolved_questions},
			{"closed", objective.closed},
			{"closureReason", objective.closure_reason ? Json(*objective.closure_reason) : Json(nullptr)},
			{"scopeLimitations", objective.scope_limitations}});
	}
	Json professionalReview = Json(nullptr);
	if (plan.draft->professional_review) {
		Json issues = Json::array();
		for (const ProfessionalReviewIssue &issue : plan.draft->professional_review->issues)
			issues.push_back({{"category", review_issue_category_string(issue.category)},
				{"evidenceObjectiveId", issue.evidence_objective_id ? Json(*issue.evidence_objective_id) : Json(nullptr)},
				{"tableHandle", issue.table_handle ? Json(*issue.table_handle) : Json(nullptr)},
				{"fieldHandle", issue.field_handle ? Json(*issue.field_handle) : Json(nullptr)},
				{"message", issue.message}, {"suggestedNextAction", issue.suggested_next_action}});
		std::string verdict = "pass";
		if (plan.draft->professional_review->verdict == ProfessionalReviewVerdict::NeedsMoreEvidence) verdict = "needs_more_evidence";
		else if (plan.draft->professional_review->verdict == ProfessionalReviewVerdict::NeedsOutputReplan) verdict = "needs_output_replan";
		else if (plan.draft->professional_review->verdict == ProfessionalReviewVerdict::NeedsRedraft) verdict = "needs_redraft";
		professionalReview = {{"verdict", verdict}, {"issues", std::move(issues)}};
	}
	return {{"schemaVersion", 13}, {"revision", plan.draft->revision},
		{"targetDocumentId", plan.target_document_id},
		{"targetVersion", plan.target_version}, {"targetFingerprint", plan.target_fingerprint},
		{"otherCandidateDocuments", std::move(otherCandidates)},
		{"finished", plan.draft->finished},
		{"expandedTableIds", std::move(expandedTableIds)},
		{"activeTableId", plan.draft->active_table_id ? Json(*plan.draft->active_table_id) : Json(nullptr)},
		{"activeRowStart", plan.draft->active_row_start},
		{"activeRowCount", plan.draft->active_row_count},
		{"inspectionWorkingNote", plan.draft->inspection_working_note},
		{"inspectionWorkingNoteTableIds", plan.draft->inspection_working_note_table_ids},
		{"outputAgendaDefined", plan.draft->output_agenda_defined},
		{"requiredOutputs", std::move(requiredOutputs)},
		{"selectedOutputRegionHandles", std::move(selectedOutputRegionHandles)},
		{"skippedOutputRegionHandles", std::move(skippedOutputRegionHandles)},
		{"revealedMethodologyHandles", std::move(revealedMethodologyHandles)},
		{"methodologyRevealRecency", std::move(methodologyRevealRecency)},
		{"fieldDispositions", std::move(fieldDispositions)},
		{"evidenceAgendaDefined", plan.draft->evidence_agenda_defined},
		{"requiredEvidence", std::move(requiredEvidence)},
		{"evidenceDeclaredSufficient", std::move(evidenceDeclaredSufficient)},
		{"tableMapPageStart", std::move(tableMapPageStart)},
		{"tableMapPageSize", tableMapPageSizeCapped
			? Json(plan.draft->table_map_page_size) : Json(nullptr)},
		{"evidenceObjectives", std::move(evidenceObjectives)},
		{"professionalReview", std::move(professionalReview)},
		{"professionalReviewReviewedRevision", plan.draft->professional_review_reviewed_revision},
		{"professionalReviewCyclesUsed", plan.draft->professional_review_cycles_used},
		{"items", std::move(items)}};
}

void restore_connected_document_draft_checkpoint(
		const ConnectedDocumentActionPlan &plan, const Json &checkpoint) {
	const int schemaVersion = checkpoint.value("schemaVersion", 0);
	if (!checkpoint.is_object() || schemaVersion < 1 || schemaVersion > 13
			|| checkpoint.value("targetDocumentId", std::string()) != plan.target_document_id
			|| checkpoint.value("targetVersion", -1) != plan.target_version
			|| checkpoint.value("targetFingerprint", std::string()) != plan.target_fingerprint
			|| !checkpoint.contains("items") || !checkpoint.at("items").is_array())
		throw std::runtime_error("The saved operation checkpoint no longer matches the target document.");
	// schemaVersion < 4 predates other_candidate_documents/finished - an older
	// checkpoint never had them, so there is nothing stale to compare against
	// and finished correctly defaults to false (falls back to requiring every
	// item touched, exactly this operation's pre-existing behavior).
	if (schemaVersion >= 4) {
		if (checkpoint.value("otherCandidateDocuments", Json::array()).size()
				!= plan.other_candidate_documents.size())
			throw std::runtime_error("The saved operation checkpoint no longer matches the target document.");
		for (const DocumentActionCandidate &candidate : plan.other_candidate_documents) {
			const bool stillPresent = std::any_of(
				checkpoint.at("otherCandidateDocuments").begin(), checkpoint.at("otherCandidateDocuments").end(),
				[&](const Json &entry) {
					return entry.value("documentId", std::string()) == candidate.document_id
						&& entry.value("fingerprint", std::string()) == candidate.fingerprint;
				});
			if (!stillPresent) throw std::runtime_error(
				"The saved operation checkpoint no longer matches the target document.");
		}
		plan.draft->finished = checkpoint.value("finished", false);
	}
	// schemaVersion < 5 predates compact_context_mode/expanded_table_ids -
	// nothing to restore, so every table simply starts collapsed again
	// exactly as prepare_connected_document_action() would decide fresh
	// (fail closed: a table silently missing from an older/foreign
	// checkpoint stays hidden rather than being assumed already revealed).
	if (schemaVersion >= 5) {
		for (const Json &entry : checkpoint.value("expandedTableIds", Json::array())) {
			if (!entry.is_string()) continue;
			const std::string tableHandle = entry.get<std::string>();
			if (plan.table_handles.find(tableHandle) != plan.table_handles.end())
				plan.draft->expanded_table_ids.insert(tableHandle);
		}
	}
	if (schemaVersion >= 6) {
		if (checkpoint.contains("activeTableId") && checkpoint.at("activeTableId").is_string()) {
			const std::string activeTableId = checkpoint.at("activeTableId").get<std::string>();
			const auto table = plan.table_handles.find(activeTableId);
			const std::size_t rowStart = checkpoint.value("activeRowStart", std::size_t{0});
			const std::size_t rowCount = checkpoint.value("activeRowCount", std::size_t{0});
			if (table == plan.table_handles.end() || rowCount == 0 || rowStart >= table->second.row_count)
				throw std::runtime_error("The saved operation checkpoint contains an invalid inspection window.");
			plan.draft->active_table_id = activeTableId;
			plan.draft->active_row_start = rowStart;
			plan.draft->active_row_count = std::min(rowCount, table->second.row_count - rowStart);
		}
		plan.draft->inspection_working_note = checkpoint.value("inspectionWorkingNote", std::string());
		for (const Json &entry : checkpoint.value("inspectionWorkingNoteTableIds", Json::array())) {
			if (!entry.is_string()) continue;
			const std::string tableHandle = entry.get<std::string>();
			if (plan.draft->expanded_table_ids.count(tableHandle) != 0)
				plan.draft->inspection_working_note_table_ids.insert(tableHandle);
		}
	}
	if (schemaVersion >= 7) {
		plan.draft->output_agenda_defined = checkpoint.value("outputAgendaDefined", false);
		const Json outputs = checkpoint.value("requiredOutputs", Json::array());
		if (!outputs.is_array() || (plan.draft->output_agenda_defined && outputs.empty()))
			throw std::runtime_error("The saved operation checkpoint contains an invalid required-output agenda.");
		std::set<std::string> ids;
		std::set<std::string> fields;
		std::optional<std::string> outputDocumentId;
		for (const Json &entry : outputs) {
			if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string()
					|| !entry.contains("description") || !entry.at("description").is_string()
					|| !entry.contains("itemHandle") || !entry.at("itemHandle").is_string()
					|| !entry.contains("fieldHandle") || !entry.at("fieldHandle").is_string())
				throw std::runtime_error("The saved operation checkpoint contains a malformed required output.");
			RequiredOutputObligation obligation{entry.at("id").get<std::string>(),
				entry.at("description").get<std::string>(), entry.at("itemHandle").get<std::string>(),
				entry.at("fieldHandle").get<std::string>()};
			const auto item = plan.item_editable_field_handles.find(obligation.item_handle);
			const auto locator = plan.field_handles.find(obligation.field_handle);
			if (obligation.id.empty() || obligation.description.empty() || obligation.id.size() > 80
					|| obligation.description.size() > 400 || item == plan.item_editable_field_handles.end()
					|| locator == plan.field_handles.end()
					|| std::find(item->second.begin(), item->second.end(), obligation.field_handle) == item->second.end()
					|| !ids.insert(obligation.id).second || !fields.insert(obligation.field_handle).second)
				throw std::runtime_error("The saved operation checkpoint contains an invalid required-output target.");
			if (!outputDocumentId) outputDocumentId = locator->second.document_id;
			if (*outputDocumentId != locator->second.document_id)
				throw std::runtime_error("The saved operation checkpoint mixes required outputs across documents.");
			plan.draft->required_outputs.push_back(std::move(obligation));
		}
	}
	// schemaVersion 10 adds the authoritative region-wide disposition set.
	// Older checkpoints can be migrated only when every selected region had
	// exactly one editable field; that is the sole case where no sibling field
	// could have been omitted silently. A legacy multi-field agenda fails
	// closed and must be replanned against the current structural contract.
	if (schemaVersion >= 10) {
		const Json selectedRegions = checkpoint.value("selectedOutputRegionHandles", Json::array());
		const Json dispositions = checkpoint.value("fieldDispositions", Json::array());
		if (!selectedRegions.is_array() || !dispositions.is_array()
				|| (plan.draft->output_agenda_defined && (selectedRegions.empty() || dispositions.empty())))
			throw std::runtime_error("The saved operation checkpoint contains an invalid typed output binding.");
		std::set<std::string> seenRegions;
		std::optional<std::string> dispositionDocumentId;
		for (const Json &entry : selectedRegions) {
			if (!entry.is_string())
				throw std::runtime_error("The saved operation checkpoint contains a malformed output region.");
			const std::string handle = entry.get<std::string>();
			const auto editable = plan.item_editable_field_handles.find(handle);
			if (editable == plan.item_editable_field_handles.end() || editable->second.empty()
					|| !seenRegions.insert(handle).second)
				throw std::runtime_error("The saved operation checkpoint contains an invalid output region.");
			const auto locator = plan.field_handles.find(editable->second.front());
			if (locator == plan.field_handles.end())
				throw std::runtime_error("The saved operation checkpoint contains a stale output region.");
			if (!dispositionDocumentId) dispositionDocumentId = locator->second.document_id;
			if (*dispositionDocumentId != locator->second.document_id)
				throw std::runtime_error("The saved operation checkpoint mixes output regions across documents.");
			plan.draft->selected_output_region_handles.push_back(handle);
		}
		std::set<std::string> seenDispositionFields;
		for (const Json &entry : dispositions) {
			if (!entry.is_object() || !entry.contains("itemHandle") || !entry.at("itemHandle").is_string()
					|| !entry.contains("fieldHandle") || !entry.at("fieldHandle").is_string()
					|| !entry.contains("disposition") || !entry.at("disposition").is_string()
					|| !entry.contains("reason")
					|| (!entry.at("reason").is_null() && !entry.at("reason").is_string()))
				throw std::runtime_error("The saved operation checkpoint contains a malformed field disposition.");
			const std::string itemHandle = entry.at("itemHandle").get<std::string>();
			const std::string fieldHandle = entry.at("fieldHandle").get<std::string>();
			const auto disposition = field_disposition_from_string(entry.at("disposition").get<std::string>());
			const auto editable = plan.item_editable_field_handles.find(itemHandle);
			const std::optional<std::string> reason = entry.at("reason").is_string()
				? std::optional<std::string>(entry.at("reason").get<std::string>()) : std::nullopt;
			if (!disposition || seenRegions.count(itemHandle) == 0
					|| editable == plan.item_editable_field_handles.end()
					|| std::find(editable->second.begin(), editable->second.end(), fieldHandle)
						== editable->second.end()
					|| !seenDispositionFields.insert(fieldHandle).second
					|| (reason && reason->size() > 400)
					|| (*disposition == DocumentFieldDisposition::LeaveUnchanged
						&& (!reason || trim_ascii_space(*reason).empty())))
				throw std::runtime_error("The saved operation checkpoint contains an invalid field disposition.");
			plan.draft->field_dispositions.push_back({itemHandle, fieldHandle, *disposition, reason});
		}
		for (const std::string &itemHandle : seenRegions) {
			const auto &editable = plan.item_editable_field_handles.at(itemHandle);
			for (const std::string &fieldHandle : editable)
				if (seenDispositionFields.count(fieldHandle) == 0)
					throw std::runtime_error("The saved operation checkpoint omits an editable field disposition.");
		}
		std::set<std::string> outputFields;
		for (const RequiredOutputObligation &output : plan.draft->required_outputs) {
			if (seenRegions.count(output.item_handle) == 0) throw std::runtime_error(
				"The saved operation checkpoint maps an output outside its selected regions.");
			outputFields.insert(output.field_handle);
		}
		for (const DocumentFieldDispositionBinding &binding : plan.draft->field_dispositions) {
			const bool mapped = outputFields.count(binding.field_handle) != 0;
			if ((binding.disposition == DocumentFieldDisposition::Write) != mapped)
				throw std::runtime_error("The saved operation checkpoint has conflicting output dispositions.");
		}
	} else if (plan.draft->output_agenda_defined) {
		std::set<std::string> seenRegions;
		for (const RequiredOutputObligation &output : plan.draft->required_outputs) {
			const auto editable = plan.item_editable_field_handles.find(output.item_handle);
			if (editable == plan.item_editable_field_handles.end() || editable->second.size() != 1
					|| editable->second.front() != output.field_handle)
				throw std::runtime_error(
					"The saved operation checkpoint predates complete dispositions for a multi-field region.");
			if (seenRegions.insert(output.item_handle).second)
				plan.draft->selected_output_region_handles.push_back(output.item_handle);
			plan.draft->field_dispositions.push_back({output.item_handle, output.field_handle,
				DocumentFieldDisposition::Write, std::nullopt});
		}
	}
	// schemaVersion < 8 predates the model-driven required-evidence agenda
	// (an older checkpoint's obligations, if any, came from the since-removed
	// "every other named document" heuristic and must not be trusted as the
	// model's own declaration) - nothing to restore, so evidence_agenda_
	// defined correctly defaults to false: a resumed pre-8 operation with
	// more than one candidate document simply re-requires the explicit
	// declaration once, fail-closed rather than silently inheriting an
	// undeclared obligation set.
	std::set<std::string> knownDocumentIds{plan.target_document_id};
	for (const DocumentActionCandidate &candidate : plan.other_candidate_documents)
		knownDocumentIds.insert(candidate.document_id);
	if (schemaVersion >= 8) {
		plan.draft->evidence_agenda_defined = checkpoint.value("evidenceAgendaDefined", false);
		for (const Json &entry : checkpoint.value("requiredEvidence", Json::array())) {
			if (!entry.is_object() || !entry.contains("documentId") || !entry.at("documentId").is_string()
					|| !entry.contains("documentName") || !entry.at("documentName").is_string())
				throw std::runtime_error("The saved operation checkpoint contains a malformed required-evidence obligation.");
			const std::string documentId = entry.at("documentId").get<std::string>();
			if (knownDocumentIds.count(documentId) == 0)
				throw std::runtime_error("The saved operation checkpoint references a required-evidence document that no longer exists.");
			plan.draft->required_evidence.push_back({documentId, entry.at("documentName").get<std::string>()});
		}
		for (const Json &entry : checkpoint.value("evidenceDeclaredSufficient", Json::array())) {
			if (!entry.is_string()) continue;
			const std::string documentId = entry.get<std::string>();
			if (knownDocumentIds.count(documentId) != 0)
				plan.draft->evidence_declared_sufficient.insert(documentId);
		}
	}
	// schemaVersion < 9 predates table-map pagination - nothing to restore, so
	// every document's own table map simply starts at its first page again,
	// uncapped until/unless compact_connected_document_inspection_window()
	// shrinks it - exactly prepare_connected_document_action()'s own
	// fresh-plan defaults, never a new invented behavior. A page-start entry
	// for a document that no longer exists, or a page size this operation's
	// own logic could never legitimately produce (zero - see request_
	// connected_document_table_map_next_page(), which would otherwise report
	// "no more pages" forever while showing none), is a stale/foreign cursor:
	// dropped rather than trusted, exactly like evidenceDeclaredSufficient
	// above rather than a hard failure - this is pure navigation state, always
	// safely re-derivable, never a legal or data-integrity obligation.
	if (schemaVersion >= 9) {
		// .items() must never be called directly on a temporary Json (its
		// iteration_proxy keeps a reference back to the object it was called
		// on) - bind the .value() result to a name first so it lives for the
		// whole loop, exactly like every sibling restore loop above instead
		// iterates its own Json::array() temporary directly (safe only
		// because that temporary is itself what the range-for binds to).
		const Json tableMapPageStartCheckpoint = checkpoint.value("tableMapPageStart", Json::object());
		for (const auto &[documentId, value] : tableMapPageStartCheckpoint.items()) {
			if (knownDocumentIds.count(documentId) != 0 && value.is_number_unsigned())
				plan.draft->table_map_page_start[documentId] = value.get<std::size_t>();
		}
		const Json pageSize = checkpoint.value("tableMapPageSize", Json(nullptr));
		if (pageSize.is_number_unsigned() && pageSize.get<std::size_t>() > 0)
			plan.draft->table_map_page_size = pageSize.get<std::size_t>();
	}
	// schemaVersion < 11 predates the Evidence Coverage Contract and the
	// Professional Completion Challenge - nothing to restore. Fail closed
	// rather than silently drop: unlike table-map navigation, an
	// EvidenceObjective is enforcement-critical (document_evidence_
	// coverage_complete() gates real sufficiency on it existing), so a
	// checkpoint that claims one but references a table/document this exact
	// plan no longer has is rejected outright rather than quietly losing the
	// obligation it exists to enforce.
	if (schemaVersion >= 11) {
		for (const Json &entry : checkpoint.value("evidenceObjectives", Json::array())) {
			if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string()
					|| !entry.contains("description") || !entry.at("description").is_string()
					|| !entry.contains("documentId") || !entry.at("documentId").is_string()
					|| !entry.contains("tableHandle") || !entry.at("tableHandle").is_string()
					|| !entry.contains("coverageMode") || !entry.at("coverageMode").is_string())
				throw std::runtime_error("The saved operation checkpoint contains a malformed evidence objective.");
			const std::string documentId = entry.at("documentId").get<std::string>();
			const std::string tableHandle = entry.at("tableHandle").get<std::string>();
			const auto table = plan.table_handles.find(tableHandle);
			if (knownDocumentIds.count(documentId) == 0 || table == plan.table_handles.end()
					|| table->second.document_id != documentId)
				throw std::runtime_error("The saved operation checkpoint references a stale evidence objective table.");
			const std::string coverageModeArg = entry.at("coverageMode").get<std::string>();
			if (coverageModeArg != "exhaustive" && coverageModeArg != "targeted")
				throw std::runtime_error("The saved operation checkpoint contains an invalid evidence objective coverage mode.");
			EvidenceObjective objective;
			objective.id = entry.at("id").get<std::string>();
			objective.description = entry.at("description").get<std::string>();
			objective.document_id = documentId;
			objective.table_handle = tableHandle;
			objective.coverage_mode = coverageModeArg == "exhaustive"
				? EvidenceCoverageMode::Exhaustive : EvidenceCoverageMode::Targeted;
			for (const Json &range : entry.value("inspectedRanges", Json::array())) {
				if (!range.is_object() || !range.value("startRow", Json(nullptr)).is_number_unsigned()
						|| !range.value("endRow", Json(nullptr)).is_number_unsigned())
					continue;
				const std::size_t startRow = range.at("startRow").get<std::size_t>();
				const std::size_t endRow = std::min(range.at("endRow").get<std::size_t>(), table->second.row_count);
				if (startRow < endRow) merge_evidence_coverage_range(objective, startRow, endRow);
			}
			for (const Json &question : entry.value("unresolvedQuestions", Json::array()))
				if (question.is_string()) objective.unresolved_questions.push_back(question.get<std::string>());
			for (const Json &limitation : entry.value("scopeLimitations", Json::array()))
				if (limitation.is_string()) objective.scope_limitations.push_back(limitation.get<std::string>());
			objective.closed = entry.value("closed", false);
			if (entry.contains("closureReason") && entry.at("closureReason").is_string())
				objective.closure_reason = entry.at("closureReason").get<std::string>();
			if (plan.draft->table_evidence_objective.count(tableHandle) != 0)
				throw std::runtime_error("The saved operation checkpoint binds more than one evidence objective to the same table.");
			plan.draft->table_evidence_objective[tableHandle] = objective.id;
			plan.draft->evidence_objectives.emplace(objective.id, std::move(objective));
		}
		if (checkpoint.contains("professionalReview") && checkpoint.at("professionalReview").is_object()) {
			const Json &reviewCheckpoint = checkpoint.at("professionalReview");
			const std::string verdictArg = reviewCheckpoint.value("verdict", std::string());
			std::optional<ProfessionalReviewVerdict> verdict;
			if (verdictArg == "pass") verdict = ProfessionalReviewVerdict::Pass;
			else if (verdictArg == "needs_more_evidence") verdict = ProfessionalReviewVerdict::NeedsMoreEvidence;
			else if (verdictArg == "needs_output_replan") verdict = ProfessionalReviewVerdict::NeedsOutputReplan;
			else if (verdictArg == "needs_redraft") verdict = ProfessionalReviewVerdict::NeedsRedraft;
			if (!verdict) throw std::runtime_error("The saved operation checkpoint contains an invalid professional review verdict.");
			ProfessionalReviewResult review;
			review.verdict = *verdict;
			for (const Json &entry : reviewCheckpoint.value("issues", Json::array())) {
				const auto category = entry.is_object()
					? review_issue_category_from_string(entry.value("category", std::string())) : std::nullopt;
				if (!category || !entry.value("message", Json(nullptr)).is_string()
						|| !entry.value("suggestedNextAction", Json(nullptr)).is_string())
					continue;
				ProfessionalReviewIssue issue;
				issue.category = *category;
				issue.message = entry.at("message").get<std::string>();
				issue.suggested_next_action = entry.at("suggestedNextAction").get<std::string>();
				if (entry.value("evidenceObjectiveId", Json(nullptr)).is_string()
						&& plan.draft->evidence_objectives.count(entry.at("evidenceObjectiveId").get<std::string>()) != 0)
					issue.evidence_objective_id = entry.at("evidenceObjectiveId").get<std::string>();
				if (entry.value("tableHandle", Json(nullptr)).is_string()
						&& plan.table_handles.count(entry.at("tableHandle").get<std::string>()) != 0)
					issue.table_handle = entry.at("tableHandle").get<std::string>();
				if (entry.value("fieldHandle", Json(nullptr)).is_string()
						&& plan.field_handles.count(entry.at("fieldHandle").get<std::string>()) != 0)
					issue.field_handle = entry.at("fieldHandle").get<std::string>();
				review.issues.push_back(std::move(issue));
			}
			plan.draft->professional_review = std::move(review);
		}
		plan.draft->professional_review_reviewed_revision =
			checkpoint.value("professionalReviewReviewedRevision", std::uint64_t{0});
		plan.draft->professional_review_cycles_used =
			checkpoint.value("professionalReviewCyclesUsed", std::size_t{0});
	}
	// schemaVersion < 12 predates skipped-region/methodology-reveal
	// persistence - nothing to restore. Fail closed on a stale/foreign handle
	// exactly like every other schemaVersion 12+ field above rather than
	// silently dropping a real considered/revealed decision.
	if (schemaVersion >= 12) {
		for (const Json &entry : checkpoint.value("skippedOutputRegionHandles", Json::array())) {
			if (!entry.is_string())
				throw std::runtime_error("The saved operation checkpoint contains a malformed skipped output region.");
			const std::string handle = entry.get<std::string>();
			if (plan.item_field_handles.find(handle) == plan.item_field_handles.end())
				throw std::runtime_error("The saved operation checkpoint references a stale skipped output region.");
			plan.draft->skipped_output_region_handles.push_back(handle);
		}
		for (const Json &entry : checkpoint.value("revealedMethodologyHandles", Json::array())) {
			if (!entry.is_string())
				throw std::runtime_error("The saved operation checkpoint contains a malformed revealed methodology handle.");
			const std::string handle = entry.get<std::string>();
			if (plan.methodology_handles.find(handle) == plan.methodology_handles.end())
				throw std::runtime_error("The saved operation checkpoint references a stale methodology handle.");
			plan.draft->revealed_methodology_handles.insert(handle);
		}
	}
	// schemaVersion < 13 predates reveal-order provenance. Its durable revealed
	// set restores above and remains fully visible/citable; only historical
	// ordering is unavailable.
	if (schemaVersion >= 13) {
		for (const Json &entry : checkpoint.value("methodologyRevealRecency", Json::array())) {
			if (!entry.is_string())
				throw std::runtime_error("The saved operation checkpoint contains a malformed methodology reveal recency entry.");
			const std::string handle = entry.get<std::string>();
			if (plan.methodology_handles.find(handle) == plan.methodology_handles.end())
				throw std::runtime_error("The saved operation checkpoint references a stale methodology handle.");
			plan.draft->revealed_methodology_recency.push_back(handle);
		}
	}
	plan.draft->revision = checkpoint.value("revision", std::uint64_t{1});
	for (const Json &entry : checkpoint.at("items")) {
		if (!entry.is_object() || !entry.contains("itemHandle") || !entry.at("itemHandle").is_string())
			throw std::runtime_error("The saved operation checkpoint is malformed.");
		const std::string handle = entry.at("itemHandle").get<std::string>();
		const auto known = plan.draft->items.find(handle);
		if (known == plan.draft->items.end())
			throw std::runtime_error("The saved operation checkpoint references an unknown work item.");
		known->second.state = draft_item_state_from_string(entry.value("state", std::string("pending")));
		known->second.proposal_attempts = entry.value("proposalAttempts", 0);
		known->second.research_search_calls_at_request = entry.value(
			"researchSearchCallsAtRequest", std::size_t{0});
		known->second.research_open_calls_at_request = entry.value(
			"researchOpenCallsAtRequest", std::size_t{0});
		if (entry.contains("requestedResearchEvidenceId")
				&& entry.at("requestedResearchEvidenceId").is_string())
			known->second.requested_research_evidence_id =
				entry.at("requestedResearchEvidenceId").get<std::string>();
		if (entry.contains("lastIssueCategory") && entry.at("lastIssueCategory").is_string())
			known->second.last_issue_category = entry.at("lastIssueCategory").get<std::string>();
		if (entry.contains("lastIssueMessage") && entry.at("lastIssueMessage").is_string())
			known->second.last_issue_message = entry.at("lastIssueMessage").get<std::string>();
		if (entry.contains("acceptedAction") && entry.at("acceptedAction").is_object())
			known->second.accepted_action = entry.at("acceptedAction");
		if (known->second.state == DocumentDraftItemState::Accepted && !known->second.accepted_action)
			throw std::runtime_error("The saved accepted work item has no semantic delta.");
	}
}

std::optional<research::FinalTextIssue> connected_document_action_final_issue(
		const ConnectedDocumentActionPlan &plan, const std::string &modelResult,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence) {
	try {
		(void)resolve_document_action_proposal(plan, modelResult, availableExternalEvidence);
		return std::nullopt;
	} catch (const ResearchCoverageError &error) {
		return research::FinalTextIssue{
			research::FinalTextIssueKind::ResearchCoverage, error.what(), "research_coverage"};
	} catch (const std::exception &error) {
		const std::string message = error.what();
		const std::string correctionKey = message.find("numeric comparison") != std::string::npos
			? "quantitative_consistency"
			: (message.find("evidence") != std::string::npos
				|| message.find("citation") != std::string::npos
				|| message.find("source URL") != std::string::npos
				|| message.find("support status") != std::string::npos)
				? "evidence_provenance" : "structured_action";
		return research::FinalTextIssue{
			research::FinalTextIssueKind::CorrectionOnly, message, correctionKey};
	}
}

ConnectedDocumentActionResult apply_connected_document_action(
		const ConnectedDocumentActionPlan &plan, const std::string &modelResult,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
		const tools::ToolExecutionContext &context, bool preapproved) {
	namespace obs = routing::observability;
	obs::emit({obs::OperationEventKind::DocumentActionProposed, "Resolving model document-action proposal", {},
		obs::has_sink() ? std::optional<std::string>(modelResult) : std::nullopt});
	ResolvedDocumentActionProposal resolved = resolve_document_action_proposal(
		plan, modelResult, availableExternalEvidence);
	if (resolved.normalized_fence) obs::emit({obs::OperationEventKind::ModelResultKind,
			"Normalized one explicit JSON code fence for document-action validation",
			{{"syntax", "single_json_code_fence"}}, std::nullopt});

	ConnectedDocumentActionResult result;
	result.response_text = std::move(resolved.response_text);
	result.action_evidence.rationales = std::move(resolved.rationales);
	result.referenced_external_evidence = std::move(resolved.referenced_external_evidence);

	// resolve_document_action_proposal() already proved every operation's
	// locator shares one document_id - the real file this one atomic action
	// applies to, which is the plan's default target only when the model
	// never had (or never used) another candidate to choose from.
	const std::string resolvedDocumentId = resolved.patch_operations.empty()
		? plan.target_document_id : resolved.patch_operations.front().at("locator").at("documentId").get<std::string>();
	const CandidateVersionGuard versionGuard = candidate_version_guard_for_id(plan, resolvedDocumentId);

	ensure_production_document_patch_tools();
	const Json patchArguments{{"documentId", resolvedDocumentId}, {"expectedVersion", versionGuard.version},
		{"expectedFingerprint", versionGuard.fingerprint}, {"operations", resolved.patch_operations}};
	// execute_preapproved() never re-asks permission_for() and never touches
	// ToolBroker's pending map: the operation's own durable "approved" record
	// IS the authorization, for exactly the change it named.
	forget_document_patch_conflict();
	tools::ToolAttempt attempt = preapproved
		? tools::production_tool_broker().execute_preapproved(context, {"document.patch.apply", patchArguments})
		: tools::production_tool_broker().request(context, {"document.patch.apply", patchArguments});
	// Never an in-process wait: waiting inside this call means the person's
	// answer lives only in this process, so a quit or a crash loses both the
	// answer and the generation it was answering. The caller records the
	// approval on the operation's own checkpoint and resumes from there.
	if (attempt.result.outcome == tools::ToolOutcome::ApprovalRequired && attempt.approval)
		throw ConnectedDocumentActionApprovalRequired(*attempt.approval, attempt.evidence);
	result.tool_call_evidence = attempt.evidence;
	// A correct change the document could not take - changed on disk since it
	// was prepared, open in its program, or locked - ends as a conflict the
	// lawyer can prepare again, in the words that say which: the outcome the
	// artifact path gives the same situation, never a generic failure. A full
	// disk stays a full disk (platform::DiskFull).
	end_unexecuted_document_patch(attempt.result,
		"The change to the document was not approved or could not be made.");

	result.action_evidence.document_id = resolvedDocumentId;
	result.action_evidence.previous_version = versionGuard.version;
	result.action_evidence.previous_fingerprint = versionGuard.fingerprint;
	result.action_evidence.resulting_version = attempt.result.value.at("newVersion").get<int>();
	result.action_evidence.resulting_fingerprint = attempt.result.value.at("newFingerprint").get<std::string>();
	result.action_evidence.tool_call_evidence_id = attempt.evidence.id;
	for (const Json &entry : resolved.patch_operations) {
		DocumentPatchOperation operation;
		operation.locator = *document_locator_from_json(entry.at("locator"));
		operation.expected_old_value = entry.at("expectedOldValue").get<std::string>();
		operation.new_value = entry.at("newValue").get<std::string>();
		operation.rationale_id = entry.at("rationaleId").get<std::string>();
		result.action_evidence.operations.push_back(std::move(operation));
	}
	return result;
}

ConnectedDocumentActionResult apply_connected_document_action_draft(
		const ConnectedDocumentActionPlan &plan, const std::string &responseText,
		const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
		const tools::ToolExecutionContext &context, bool preapproved) {
	// See draft_is_complete()'s own comment.
	if (!draft_is_complete(plan))
		throw std::runtime_error("The backend-owned document draft is incomplete; no document change was applied.");
	return apply_connected_document_action(plan, assembled_draft_result(plan, responseText),
		availableExternalEvidence, context, preapproved);
}

} // namespace singularity::documents
