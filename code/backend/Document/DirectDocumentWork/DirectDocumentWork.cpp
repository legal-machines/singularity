#include "Document/DirectDocumentWork/DirectDocumentWork.hpp"

#include "Document/DocumentConversationIntent/DocumentConversationIntent.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Document/DocxExtractor/DocxExtractor.hpp"
#include "ExternalResearch/ContextBudget/ContextBudget.hpp"
#include "ExternalResearch/ResearchIntent/ResearchIntent.hpp"

#include <algorithm>
#include <cctype>

namespace singularity::documents {
namespace {

using Json = nlohmann::json;

std::string to_lower_ascii(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

bool contains_ascii_insensitive(const std::string &haystack, const std::string &needle) {
	if (needle.empty()) return false;
	return to_lower_ascii(haystack).find(to_lower_ascii(needle)) != std::string::npos;
}

bool row_has_role(const DocumentTableRow &row, DocumentCellRole role) {
	for (const auto &cell : row.cells)
		if (cell.role == role) return true;
	return false;
}

// A "header row": every real cell is HeaderLabel-role, and none is Body -
// the same formal distinction Office Structure Law already draws elsewhere
// in this codebase (see DocumentCellRole's own comment), applied here only
// to GROUP rows into blocks, never to decide what any cell's text means.
bool is_header_row(const DocumentTableRow &row) {
	if (row.cells.empty()) return false;
	bool sawHeaderLabel = false;
	for (const auto &cell : row.cells) {
		if (cell.role == DocumentCellRole::Body) return false;
		if (cell.role == DocumentCellRole::HeaderLabel) sawHeaderLabel = true;
	}
	return sawHeaderLabel;
}

bool is_body_row(const DocumentTableRow &row) {
	if (row.cells.empty()) return false;
	bool sawBody = false;
	for (const auto &cell : row.cells) {
		if (cell.role == DocumentCellRole::HeaderLabel || cell.role == DocumentCellRole::TitleOrBanner) return false;
		if (cell.role == DocumentCellRole::Body) sawBody = true;
	}
	return sawBody;
}

} // namespace

std::vector<DocumentBlockSurface> discover_document_block_surfaces(const DocumentTable &table) {
	std::vector<DocumentBlockSurface> blocks;
	std::size_t index = 0;
	while (index < table.rows.size()) {
		const DocumentTableRow &bannerRow = table.rows[index];
		if (!row_has_role(bannerRow, DocumentCellRole::TitleOrBanner)) { ++index; continue; }

		DocumentBlockSurface block;
		block.table_id = table.table_id;
		block.banner_row_key = bannerRow.row_key;
		block.banner_row_index = bannerRow.row_index;
		for (const auto &cell : bannerRow.cells) {
			if (cell.role == DocumentCellRole::TitleOrBanner && block.banner_title_column_key.empty()) {
				block.banner_title_text = cell.text;
				block.banner_title_column_key = cell.column_key;
			} else if (cell.role == DocumentCellRole::MetadataOrSeverity && block.banner_metadata_column_key.empty()) {
				block.banner_metadata_text = cell.text;
				block.banner_metadata_column_key = cell.column_key;
				block.banner_metadata_shading_fill_hex = cell.shading_fill_hex;
				block.banner_metadata_shading_theme_fill = cell.shading_theme_fill;
				block.banner_metadata_shading_theme_fill_shade = cell.shading_theme_fill_shade;
			}
		}
		++index;

		// Consume every consecutive (header row, body row) pair immediately
		// following this banner - the real design-partner register's shape has exactly two such
		// pairs (Описание риска/Рекомендации, then Дополнительная информация),
		// but this makes no assumption about the count.
		while (index + 1 < table.rows.size()
				&& is_header_row(table.rows[index]) && is_body_row(table.rows[index + 1])) {
			const DocumentTableRow &headerRow = table.rows[index];
			const DocumentTableRow &bodyRow = table.rows[index + 1];
			for (const auto &headerCell : headerRow.cells) {
				if (headerCell.role != DocumentCellRole::HeaderLabel) continue;
				const DocumentTableCell *bodyCell = bodyRow.cell_for_column(headerCell.column_key);
				if (bodyCell == nullptr || bodyCell->role != DocumentCellRole::Body) continue;
				DocumentBlockSurface::Field field;
				field.header_label = headerCell.text;
				field.row_key = bodyRow.row_key;
				field.column_key = bodyCell->column_key;
				field.current_text = bodyCell->text;
				field.is_placeholder = is_template_placeholder(bodyCell->text);
				block.fields.push_back(std::move(field));
			}
			index += 2;
		}
		if (!block.fields.empty()) blocks.push_back(std::move(block));
	}
	return blocks;
}

EditSurfaceDecision resolve_edit_surface(const std::vector<DocumentBlockSurface> &candidates,
		const std::string &updateIntentBlockKey) {
	EditSurfaceDecision decision;
	if (!updateIntentBlockKey.empty()) {
		for (const auto &candidate : candidates) {
			if (candidate.banner_row_key != updateIntentBlockKey) continue;
			decision.mode = EditSurfaceMode::UpdateExisting;
			decision.target = candidate;
			decision.reason = "An explicit update intent named this exact existing block by its own real identity.";
			return decision;
		}
		// The named block was not found among real candidates - this must
		// never silently fall back to picking a different, unrelated
		// populated block. Continue to FILL_TEMPLATE/INSERT_NEW below exactly
		// as if no update intent had been given.
	}

	// Occupancy-only: the first genuinely available block (see
	// DocumentBlockSurface::all_fields_are_placeholder()'s own contract) is
	// selected, full stop - never filtered or re-ranked by any field's own
	// current default value. Which specific available block gets used never
	// changes what content, including severity, the model may professionally
	// conclude for it.
	for (const auto &candidate : candidates) {
		if (!candidate.all_fields_are_placeholder()) continue;
		decision.mode = EditSurfaceMode::FillTemplate;
		decision.target = candidate;
		decision.reason = "A genuinely empty (every mapped field is a recognized placeholder) template block "
			"exists and takes precedence over every other option.";
		return decision;
	}

	decision.mode = EditSurfaceMode::InsertNew;
	for (const auto &candidate : candidates) {
		if (candidate.all_fields_are_placeholder()) continue;
		decision.clone_source = candidate;
		break;
	}
	decision.reason = candidates.empty()
		? "No compatible block of any kind was discovered in this document."
		: "No genuinely empty template block exists and no explicit update intent named an existing block.";
	return decision;
}

std::optional<SeverityPresentation> document_native_severity_presentation(
		const std::vector<DocumentBlockSurface> &candidates, const std::string &canonicalSeverity) {
	// The professional_document_result_output_contract() schema fixes
	// severity to a stable English enum (high/medium/low/informational)
	// regardless of the document's own language, so a real severity-graded
	// document almost never uses that same English wording in its own
	// severity cell text (the real design-partner register uses "Высокий"/
	// "Средний"/"Низкий"/"Для информации"). A small, explicit EN<->RU
	// synonym table - never a general translation attempt - lets the same
	// English enum value still resolve to the right real word in that real,
	// proven document convention. Byte-exact substrings (never case-folded -
	// Cyrillic case folding needs a real Unicode library, not a hand-rolled
	// table): both the title-case form a banner's own real word actually
	// uses and a lower-case form are listed explicitly.
	static const std::vector<std::pair<std::string, std::vector<std::string>>> kSeveritySynonyms{
		{"high", {"Высок", "высок"}}, {"medium", {"Средн", "средн"}},
		{"low", {"Низк", "низк"}}, {"informational", {"нформац"}},
	};
	const auto matchesSeverity = [&](const std::string &metadataText) {
		if (contains_ascii_insensitive(metadataText, canonicalSeverity)) return true;
		for (const auto &synonym : kSeveritySynonyms) {
			if (!contains_ascii_insensitive(canonicalSeverity, synonym.first)) continue;
			for (const auto &russianForm : synonym.second)
				if (metadataText.find(russianForm) != std::string::npos) return true;
		}
		return false;
	};
	for (const auto &candidate : candidates) {
		if (candidate.banner_metadata_column_key.empty()) continue;
		if (candidate.banner_metadata_shading_fill_hex.empty()
				&& candidate.banner_metadata_shading_theme_fill.empty()) continue;
		if (!matchesSeverity(candidate.banner_metadata_text)) continue;
		SeverityPresentation presentation;
		presentation.fill_hex = candidate.banner_metadata_shading_fill_hex;
		presentation.theme_fill = candidate.banner_metadata_shading_theme_fill;
		presentation.theme_fill_shade = candidate.banner_metadata_shading_theme_fill_shade;
		return presentation;
	}
	return std::nullopt;
}

std::optional<DocumentBlockSurface> document_native_record_exemplar(
		const std::vector<DocumentBlockSurface> &candidates,
		const std::string &excludeBannerRowKey) {
	// "Populated" is decided by the same rule the rest of this file already
	// uses - all_fields_are_placeholder() - so a template whose default
	// title/severity scaffolding happens to be non-empty is never mistaken
	// for a real worked example.
	const DocumentBlockSurface *best = nullptr;
	std::size_t bestFilled = 0;
	for (const auto &candidate : candidates) {
		if (candidate.all_fields_are_placeholder()) continue;
		if (!excludeBannerRowKey.empty() && candidate.banner_row_key == excludeBannerRowKey) continue;
		// The most completely written block is the most informative example of
		// the document's own register; ties keep the first discovered block so
		// the choice stays deterministic for the same document.
		std::size_t filled = 0;
		for (const auto &field : candidate.fields)
			if (!field.is_placeholder && !field.current_text.empty()) filled += field.current_text.size();
		if (filled == 0) continue;
		if (filled > bestFilled) { bestFilled = filled; best = &candidate; }
	}
	if (best == nullptr) return std::nullopt;
	return *best;
}

// No per-string minLength/maxLength anywhere in this schema - see this
// session's own real diagnostic evidence: llama.cpp's real GBNF grammar
// PARSER (llama_grammar_parser::parse(), src/llama-grammar.cpp - a distinct,
// later stage than json_schema_to_grammar()'s own JSON-schema-to-GBNF-TEXT
// conversion, which handles these same bounds without error) rejects the
// bounded-repetition rules a large maxLength produces with its own real,
// documented safety ceiling: "number of
// rules that are going to be repeated multiplied by the new repetition
// exceeds sane defaults, please reduce the number of repetitions or rule
// complexity" - confirmed directly this session by feeding this exact
// schema's own produced grammar through that exact parser entry point, and
// confirmed fixed by removing exactly these bounds (all other structure -
// enum, minItems/maxItems on arrays, additionalProperties:false, required -
// parses and is admitted cleanly). This is a real local-grammar-engine
// capacity limit, never a cloud-only convenience - cloud providers' own
// structured-output enforcement tolerates these bounds, but they were never
// a professional-content requirement in the first place (the professional
// contract is the FIELDS and their meaning, not a numeric JSON-Schema
// character ceiling on the wire). The same effective size discipline is
// still enforced, identically for cloud and local, by
// parse_professional_document_result() below - Singularity remains the length-
// policy owner either way, exactly as it already owns targeting and
// mutation.
nlohmann::json professional_document_result_schema() {
	using Json = nlohmann::json;
	const Json stringArray{{"type", "array"}, {"minItems", 1}, {"maxItems", 6},
		{"items", {{"type", "string"}}}};
	return Json{
		{"type", "object"},
		{"additionalProperties", false},
		{"properties", {
			{"title", {{"type", "string"}}},
			{"severity", {{"type", "string"}, {"enum", Json::array({"high", "medium", "low", "informational"})}}},
			{"description", stringArray},
			{"recommendations", stringArray},
			{"additionalInformation", {{"type", Json::array({"string", "null"})}}},
			{"sourceSupport", {{"type", "array"}, {"maxItems", 10},
				{"items", {{"type", "string"}}}}},
			// Follow-up continuity (see ProfessionalDocumentResult::
			// updates_previous_object's own comment): a plain boolean, required
			// on every call (never omitted) so a model that never even
			// considers the question cannot silently default into either
			// meaning - the caller-side default (false) only ever applies to a
			// response that failed to parse at all, never a genuinely
			// considered answer.
			{"updatesPreviousObject", {{"type", "boolean"}}},
		}},
		{"required", Json::array({"title", "severity", "description", "recommendations",
			"additionalInformation", "sourceSupport", "updatesPreviousObject"})},
	};
}

providers::GenerationOutputContract professional_document_result_output_contract() {
	return providers::GenerationOutputContract{
		"professional_document_result", professional_document_result_schema()};
}

namespace {

// The exact size discipline professional_document_result_output_contract()'s
// own comment describes: enforced here, once, identically for every backend
// (cloud or local), in real Unicode characters (utf8_character_count() -
// DocumentSemantics.hpp's own canonical definition), never wire-schema
// minLength/maxLength. Mirrors the previous schema's own real bounds exactly
// - this is a relocation of the size policy, not a relaxation of it.
constexpr std::size_t kMaxTitleLength = 200;
constexpr std::size_t kMaxParagraphLength = 1500;
constexpr std::size_t kMaxAdditionalInformationLength = 1500;
constexpr std::size_t kMaxSourceSupportLength = 300;
constexpr std::size_t kMaxDescriptionOrRecommendationItems = 6;
constexpr std::size_t kMaxSourceSupportItems = 10;

} // namespace

std::optional<ProfessionalDocumentResult> parse_professional_document_result(const std::string &modelResponseText) {
	using Json = nlohmann::json;
	const Json parsed = Json::parse(modelResponseText, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
	for (const char *required : {"title", "severity", "description", "recommendations",
			"additionalInformation", "sourceSupport"})
		if (!parsed.contains(required)) return std::nullopt;
	if (!parsed.at("title").is_string()
			|| !parsed.at("severity").is_string() || !parsed.at("description").is_array()
			|| !parsed.at("recommendations").is_array() || !parsed.at("sourceSupport").is_array())
		return std::nullopt;

	ProfessionalDocumentResult result;
	result.title = parsed.at("title").get<std::string>();
	result.severity = parsed.at("severity").get<std::string>();
	if (utf8_character_count(result.title) < 1 || utf8_character_count(result.title) > kMaxTitleLength)
		return std::nullopt;
	if (parsed.at("description").size() > kMaxDescriptionOrRecommendationItems
			|| parsed.at("recommendations").size() > kMaxDescriptionOrRecommendationItems)
		return std::nullopt;
	for (const auto &entry : parsed.at("description")) {
		if (!entry.is_string()) return std::nullopt;
		const std::string text = entry.get<std::string>();
		if (utf8_character_count(text) < 1 || utf8_character_count(text) > kMaxParagraphLength) return std::nullopt;
		result.description.push_back(text);
	}
	for (const auto &entry : parsed.at("recommendations")) {
		if (!entry.is_string()) return std::nullopt;
		const std::string text = entry.get<std::string>();
		if (utf8_character_count(text) < 1 || utf8_character_count(text) > kMaxParagraphLength) return std::nullopt;
		result.recommendations.push_back(text);
	}
	if (result.description.empty() || result.recommendations.empty()) return std::nullopt;
	if (!parsed.at("additionalInformation").is_null()) {
		if (!parsed.at("additionalInformation").is_string()) return std::nullopt;
		const std::string text = parsed.at("additionalInformation").get<std::string>();
		if (utf8_character_count(text) > kMaxAdditionalInformationLength) return std::nullopt;
		result.additional_information = text;
	}
	if (parsed.at("sourceSupport").size() > kMaxSourceSupportItems) return std::nullopt;
	for (const auto &entry : parsed.at("sourceSupport")) {
		if (!entry.is_string()) return std::nullopt;
		const std::string text = entry.get<std::string>();
		if (utf8_character_count(text) > kMaxSourceSupportLength) return std::nullopt;
		result.source_support.push_back(text);
	}
	// Read leniently (absent -> false, the safe direction - see this field's
	// own comment on ProfessionalDocumentResult): the JSON SCHEMA requires it
	// from every real model call, but a hand-built fixture (existing focused
	// tests predating this field) omitting it is still a well-formed,
	// parseable professional result, never a schema-validation rejection.
	if (parsed.contains("updatesPreviousObject") && parsed.at("updatesPreviousObject").is_boolean())
		result.updates_previous_object = parsed.at("updatesPreviousObject").get<bool>();
	return result;
}

namespace {

std::string join_paragraphs(const std::vector<std::string> &paragraphs) {
	std::string joined;
	for (std::size_t index = 0; index < paragraphs.size(); ++index) {
		if (index != 0) joined += "\n";
		joined += paragraphs[index];
	}
	return joined;
}

// A small, documented EN/RU keyword table - not a hardcoded document
// filename or a client-specific literal. Falls back to position (first
// HeaderLabel column = description, second = recommendations, third =
// additional info) for a plain two/three-column shape when no keyword
// matches, which is the dominant real convention this session's own two
// independent golden fixtures (the synthetic DD_report.docx and the real
// design-partner risk register) both already use.
const DocumentBlockSurface::Field *find_field_by_keyword(
		const DocumentBlockSurface &target, std::initializer_list<const char *> keywords) {
	for (const auto &field : target.fields)
		for (const char *keyword : keywords)
			if (contains_ascii_insensitive(field.header_label, keyword)) return &field;
	return nullptr;
}

} // namespace

// The document's own real severity word for a canonical English enum value.
// It is learned from the document itself: the exact word another block of the
// same register already uses for this severity, or - when no block uses it -
// the vocabulary matching the register's own severity words. A register with
// no recognizable severity word of its own keeps the canonical English word,
// so a plain-English document is never given another language's wording.
std::string document_native_severity_label(const std::vector<DocumentBlockSurface> &candidates,
		const std::string &currentMetadataText, const std::string &canonicalSeverity) {
	struct SeverityVocabulary {
		const char *high;
		const char *medium;
		const char *low;
		const char *informational;
	};
	static const std::vector<SeverityVocabulary> kVocabularies{
		{"High", "Medium", "Low", "Informational"},
		{"Высокий", "Средний", "Низкий", "Для информации"},
	};
	static const std::vector<std::pair<std::string, std::vector<std::string>>> kSeverityWords{
		{"high", {"high", "Высок", "высок"}}, {"medium", {"medium", "Средн", "средн"}},
		{"low", {"low", "Низк", "низк"}}, {"informational", {"informational", "нформац"}},
	};
	const auto canonicalOf = [&](const std::string &text) {
		for (const auto &entry : kSeverityWords)
			for (const std::string &word : entry.second)
				if (text.find(word) != std::string::npos
						|| contains_ascii_insensitive(text, word)) return entry.first;
		return std::string();
	};
	const auto wordFor = [&](const SeverityVocabulary &vocabulary) {
		if (contains_ascii_insensitive(canonicalSeverity, "high")) return std::string(vocabulary.high);
		if (contains_ascii_insensitive(canonicalSeverity, "medium")) return std::string(vocabulary.medium);
		if (contains_ascii_insensitive(canonicalSeverity, "low")) return std::string(vocabulary.low);
		if (contains_ascii_insensitive(canonicalSeverity, "informational")) return std::string(vocabulary.informational);
		return std::string();
	};
	// 1. Another block of this register already writes this severity: use its
	// exact word, whatever the register's language or convention.
	for (const DocumentBlockSurface &candidate : candidates) {
		const std::string &text = candidate.banner_metadata_text;
		if (text.empty() || is_template_placeholder(text)) continue;
		if (canonicalOf(text) == canonicalOf(canonicalSeverity) && !canonicalOf(text).empty()) return text;
	}
	// 2. Otherwise the register's own severity vocabulary decides the wording.
	std::string registerSample = currentMetadataText;
	if (canonicalOf(registerSample).empty())
		for (const DocumentBlockSurface &candidate : candidates)
			if (!canonicalOf(candidate.banner_metadata_text).empty()) {
				registerSample = candidate.banner_metadata_text;
				break;
			}
	for (const SeverityVocabulary &vocabulary : kVocabularies) {
		const std::string high(vocabulary.high);
		const bool cyrillic = high.size() > 1 && static_cast<unsigned char>(high[0]) >= 0x80;
		const bool sampleIsCyrillic = std::any_of(registerSample.begin(), registerSample.end(),
			[](unsigned char character) { return character >= 0x80; });
		if (cyrillic != sampleIsCyrillic) continue;
		const std::string word = wordFor(vocabulary);
		if (!word.empty()) return word;
	}
	return canonicalSeverity; // an unrecognized enum value - display it verbatim rather than guess.
}

std::optional<Json> build_fill_template_operations(const std::string &documentId,
		const DocumentBlockSurface &target, const ProfessionalDocumentResult &content,
		const std::string &rationaleId, const std::optional<SeverityPresentation> &severityPresentation,
		const std::vector<DocumentBlockSurface> &registerBlocks) {
	const DocumentBlockSurface::Field *descriptionField =
		find_field_by_keyword(target, {"описан", "description"});
	const DocumentBlockSurface::Field *recommendationsField =
		find_field_by_keyword(target, {"рекомендац", "recommendation"});
	const DocumentBlockSurface::Field *additionalField =
		find_field_by_keyword(target, {"дополнительн", "additional"});
	if ((descriptionField == nullptr || recommendationsField == nullptr) && target.fields.size() >= 2) {
		if (descriptionField == nullptr) descriptionField = &target.fields[0];
		if (recommendationsField == nullptr) recommendationsField = &target.fields[1];
		if (additionalField == nullptr && target.fields.size() >= 3) additionalField = &target.fields[2];
	}
	if (descriptionField == nullptr || recommendationsField == nullptr) return std::nullopt;

	Json operations = Json::array();
	const auto addOperation = [&](const std::string &rowKey, const std::string &columnKey,
			const std::string &expectedOldValue, const std::string &newValue,
			const SeverityPresentation *shading = nullptr) {
		Json operation{
			{"kind", "replaceCellText"},
			{"locator", Json{{"documentId", documentId}, {"tableId", target.table_id},
				{"rowKey", rowKey}, {"columnKey", columnKey}}},
			{"expectedOldValue", expectedOldValue},
			{"newValue", newValue},
			{"rationaleId", rationaleId},
		};
		if (shading != nullptr) {
			if (!shading->fill_hex.empty()) operation["newShadingFillHex"] = shading->fill_hex;
			if (!shading->theme_fill.empty()) {
				operation["newShadingThemeFill"] = shading->theme_fill;
				if (!shading->theme_fill_shade.empty()) operation["newShadingThemeFillShade"] = shading->theme_fill_shade;
			}
		}
		operations.push_back(std::move(operation));
	};
	addOperation(descriptionField->row_key, descriptionField->column_key,
		descriptionField->current_text, join_paragraphs(content.description));
	addOperation(recommendationsField->row_key, recommendationsField->column_key,
		recommendationsField->current_text, join_paragraphs(content.recommendations));
	if (additionalField != nullptr && content.additional_information.has_value())
		addOperation(additionalField->row_key, additionalField->column_key,
			additionalField->current_text, *content.additional_information);
	// Title and severity are real, current text (a template's own default,
	// e.g. "[Наименование риска]"/"Средний") - but `target` was only ever
	// proposed once resolve_edit_surface() already confirmed this whole
	// block's real body fields are ALL placeholders, so this is replaceable
	// scaffolding, never an existing professional conclusion (see
	// DocumentBlockSurface's own comment).
	if (!target.banner_title_column_key.empty())
		addOperation(target.banner_row_key, target.banner_title_column_key,
			target.banner_title_text, content.title);
	if (!target.banner_metadata_column_key.empty())
		addOperation(target.banner_row_key, target.banner_metadata_column_key, target.banner_metadata_text,
			document_native_severity_label(registerBlocks, target.banner_metadata_text, content.severity),
			severityPresentation.has_value() ? &*severityPresentation : nullptr);
	return operations;
}

namespace {

// A safely-resolved real filesystem path for one Referenced, available
// Project document - the same real-file requirement ConnectedDocumentAction.cpp's
// own document_path() enforces for the old path, reproduced here rather than
// exported from that file's anonymous namespace (kept private there
// deliberately; this is a small, self-contained equivalent, not a shared
// dependency worth introducing between the two features).
std::optional<std::filesystem::path> safe_document_path(
		const matters::Matter &project, const matters::MatterDocument &document) {
	if (document.source_mode != matters::DocumentSourceMode::Referenced || !document.available
			|| !project.directory_path
			|| !registry::is_relative_path_safe(*project.directory_path, document.relative_path))
		return std::nullopt;
	return std::filesystem::path(*project.directory_path) / document.relative_path;
}

} // namespace

std::optional<DirectDocumentWorkTaskPlan> resolve_direct_document_work_task_plan(
		const std::string &instruction, const matters::Matter &project,
		const std::optional<std::string> &chatId, const std::vector<matters::MatterTask> &priorTasksInChat,
		const std::vector<std::string> &selectedDocumentIds, const std::optional<std::string> &targetDocumentId) {
	// DirectDocumentWork is source-bound only - an instruction that explicitly
	// requires current external research (see ResearchIntent.hpp's own
	// keyword/@web detection) is never this path's job, regardless of what
	// document surfaces exist.
	if (research::resolve_research_mode(instruction) == research::ResearchMode::Required) return std::nullopt;
	// A Project having documents must never mean every message is about
	// them - see instruction_bypasses_document_engagement()'s own header comment
	// for why a bare greeting must never engage this path just because a
	// free template block happens to exist.
	if (instruction_bypasses_document_engagement(instruction)) return std::nullopt;
	if (!project.directory_path) return std::nullopt;

	// Step 1 (structural, read-only): discover every FILL_TEMPLATE-eligible
	// block across every real, readable DOCX Project document - never just
	// one hardcoded document. Non-DOCX documents cannot host a block surface
	// at all (see document_format_capability()); every document's content
	// reaches the model only through the request's own source selection.
	// The source selection bounds which documents may offer a candidate (see
	// the declaration's comment); this Chat's own prior record stays reachable.
	std::optional<std::string> continuityDocumentId;
	for (auto taskIt = priorTasksInChat.rbegin(); taskIt != priorTasksInChat.rend(); ++taskIt) {
		if (taskIt->status != matters::TaskStatus::Success || !taskIt->direct_document_work_document_id) continue;
		continuityDocumentId = taskIt->direct_document_work_document_id;
		break;
	}
	// Selected documents are read scope, never write candidates: with an
	// explicit selection only the explicitly bound target and this Chat's own
	// prior record may host structured-record work.
	const auto mayOfferCandidate = [&](const matters::MatterDocument &document) {
		if (targetDocumentId && *targetDocumentId == document.id) return true;
		if (continuityDocumentId && *continuityDocumentId == document.id) return true;
		return selectedDocumentIds.empty();
	};
	// The bound target first, then the continuity document, then the rest in
	// Project order: a free block in the explicitly bound target always wins.
	std::vector<const matters::MatterDocument *> orderedDocuments;
	for (const matters::MatterDocument &document : project.documents)
		if (targetDocumentId && *targetDocumentId == document.id) orderedDocuments.push_back(&document);
	for (const matters::MatterDocument &document : project.documents)
		if (continuityDocumentId && *continuityDocumentId == document.id
				&& !(targetDocumentId && *targetDocumentId == document.id))
			orderedDocuments.push_back(&document);
	for (const matters::MatterDocument &document : project.documents)
		if (!(targetDocumentId && *targetDocumentId == document.id)
				&& !(continuityDocumentId && *continuityDocumentId == document.id))
			orderedDocuments.push_back(&document);
	std::vector<DocumentBlockSurface> candidates;
	for (const matters::MatterDocument *documentPointer : orderedDocuments) {
		const matters::MatterDocument &document = *documentPointer;
		if (document.file_type != DocumentFormat::docx) continue;
		if (!mayOfferCandidate(document)) continue;
		const auto path = safe_document_path(project, document);
		if (!path) continue;
		DocumentExtractionRequest tablesRequest;
		tablesRequest.path = *path;
		std::vector<DocumentTable> tables;
		try {
			tables = extract_docx_tables(tablesRequest);
		} catch (const std::exception &) {
			// A real but structurally unreadable DOCX simply contributes no
			// candidates - never a hard failure for the whole routing decision
			// (another Project document may still offer a genuine surface).
			continue;
		}
		for (const DocumentTable &table : tables) {
			auto blocks = discover_document_block_surfaces(table);
			for (auto &block : blocks) {
				block.document_id = document.id;
				candidates.push_back(std::move(block));
			}
		}
	}
	if (candidates.empty()) return std::nullopt;

	// Occupancy-only selection (see resolve_edit_surface()'s own comment) -
	// no update-intent block key here, since a real Chat message never names
	// an internal block/row handle: the ONLY thing that ever auto-routes a
	// real Chat turn into a NEW-work FILL_TEMPLATE target is a genuinely free
	// template. Arbitrary UPDATE_EXISTING/INSERT_NEW targeting stays the old
	// connected-document-action path's job (Beta product decision - see this
	// function's own header comment) - the ONLY update target this function
	// itself ever offers is this exact Chat's own prior DirectDocumentWork
	// object, resolved below.
	const EditSurfaceDecision decision = resolve_edit_surface(candidates);
	const bool hasNewWorkTarget = decision.mode == EditSurfaceMode::FillTemplate && decision.target.has_value();

	// Follow-up continuity: find THIS Chat's own most recent completed
	// DirectDocumentWork task and re-resolve its real, current block live -
	// never a stale cached copy, and never a block any OTHER task produced.
	std::optional<DocumentBlockSurface> updateTarget;
	std::optional<std::string> updateDocumentId;
	for (auto taskIt = priorTasksInChat.rbegin(); taskIt != priorTasksInChat.rend(); ++taskIt) {
		if (taskIt->status != matters::TaskStatus::Success) continue;
		if (!taskIt->direct_document_work_document_id || !taskIt->direct_document_work_table_id
				|| !taskIt->direct_document_work_banner_row_index) continue;
		for (const auto &candidate : candidates) {
			if (candidate.document_id == *taskIt->direct_document_work_document_id
					&& candidate.table_id == *taskIt->direct_document_work_table_id
					&& candidate.banner_row_index == *taskIt->direct_document_work_banner_row_index) {
				updateTarget = candidate;
				updateDocumentId = *taskIt->direct_document_work_document_id;
				break;
			}
		}
		break; // Only the most recent DirectDocumentWork task in this Chat is ever eligible.
	}
	if (!hasNewWorkTarget && !updateTarget.has_value()) return std::nullopt;

	DirectDocumentWorkTaskPlan plan;
	if (hasNewWorkTarget) {
		plan.new_work_document_id = decision.target->document_id;
		plan.new_work_target = *decision.target;
	}
	plan.update_document_id = updateDocumentId;
	plan.update_target = updateTarget;
	plan.candidates = candidates;
	plan.update_fallback_allowed = selectedDocumentIds.empty();
	(void)chatId;

	// Structure only (see DirectDocumentWorkTaskPlan::candidate_context): the
	// evidence is the request's own SOURCE_MATERIAL.
	std::string context;
	if (plan.update_target.has_value()) {
		context += "PREVIOUS STRUCTURED RECORD IN THIS CHAT - the immediately preceding professional record this "
			"Chat itself already produced currently reads:\n";
		if (!plan.update_target->banner_title_text.empty())
			context += "Title: \"" + plan.update_target->banner_title_text + "\"\n";
		if (!plan.update_target->banner_metadata_text.empty())
			context += "Severity: \"" + plan.update_target->banner_metadata_text + "\"\n";
		for (const auto &field : plan.update_target->fields)
			context += field.header_label + ": \"" + field.current_text + "\"\n";
		context += "If, and only if, the request is asking to modify THIS SAME record (e.g. \"shorten the "
			"recommendations\", \"keep the severity\", \"change only the title\") rather than describing a "
			"genuinely different, additional issue, set updatesPreviousObject to true and return the COMPLETE "
			"revised content for every field - copy any field the user did not ask to change verbatim from its "
			"current value above, never leave it blank. If you are not confident this is the same record, set "
			"updatesPreviousObject to false.\n";
	}
	if (plan.new_work_target.has_value()) {
		context += "FREE STRUCTURED RECORD TEMPLATE - when updatesPreviousObject is false, structured-record work "
			"drafts one new, currently free professional record block with these real field labels: ";
		for (const auto &field : plan.new_work_target->fields) context += "\"" + field.header_label + "\" ";
		context += "\n";
	}
	if (!plan.new_work_target.has_value())
		context += "No free structured record template is available in this request's scope: structured-record work "
			"can only revise the previous record above (updatesPreviousObject true), never add a new one. A new record "
			"cannot be written over the previous one.\n";
	// The document's OWN worked example, when it has one. Without this the
	// instruction below ("write it in the register of ... existing entries")
	// asked the model to match entries it was never shown - the free
	// template contributes field LABELS only. Bounded and explicitly marked
	// when truncated, following bounded_structural_preview()'s own rule that
	// a model-facing preview is never silently cut; candidate_context is
	// REQUIRED prompt content (PromptAssembler.cpp accounts it in
	// budget.included), so an unbounded example could turn a workable request
	// into a fail-closed one on a small-capacity model.
	const auto exemplar = document_native_record_exemplar(
		candidates, plan.update_target.has_value() ? plan.update_target->banner_row_key : std::string());
	if (exemplar.has_value()) {
		constexpr std::size_t kMaxExemplarChars = 2000;
		std::string example;
		if (!exemplar->banner_title_text.empty())
			example += "Title: \"" + exemplar->banner_title_text + "\"\n";
		if (!exemplar->banner_metadata_text.empty())
			example += "Severity: \"" + exemplar->banner_metadata_text + "\"\n";
		for (const auto &field : exemplar->fields) {
			if (field.is_placeholder || field.current_text.empty()) continue;
			example += field.header_label + ": \"" + field.current_text + "\"\n";
		}
		if (example.size() > kMaxExemplarChars) {
			// Never cut mid-character: this example is real document text, and
			// the real design-partner document is Cyrillic, so a byte-boundary
			// resize splits a two-byte character and produces invalid UTF-8
			// that fails JSON serialization downstream (caught by
			// chat_execution's own item14 against that exact real document).
			example.resize(research::utf8_safe_truncation_length(example, kMaxExemplarChars));
			example += "\n[Singularity shortened this example to " + std::to_string(kMaxExemplarChars)
				+ " characters - it is an illustration of this document's own style, not evidence.]\n";
		}
		if (!example.empty()) {
			context += "HOW THIS DOCUMENT ALREADY WRITES THIS KIND OF RECORD - a real, existing entry from the "
				"SAME document, supplied so you can match how it is written. Follow its language, terminology, "
				"defined terms, level of detail and professional register. Never copy its FACTS: it concerns a "
				"different matter, and the facts for this record come only from SOURCE_MATERIAL.\n";
			context += example;
		}
	}
	context += "A structuredRecord provides a description, recommendations, a short title and a severity judgment "
		"(high/medium/low/informational) supported by SOURCE_MATERIAL, without referencing internal document "
		"mechanics. Write it in the language and professional register of the record's own field labels and "
		"existing entries.";
	plan.candidate_context = std::move(context);
	return plan;
}

std::optional<SelectedDirectDocumentWorkTarget> select_direct_document_work_target(
		const DirectDocumentWorkTaskPlan &plan, bool updatesPreviousObject) {
	if (!plan.new_work_target.has_value() && !plan.update_target.has_value()) return std::nullopt;
	// A record the model declares as new is never written over the previous
	// one: with no free template the request is refused truthfully instead, so
	// an unrelated new risk can never replace a professional record already in
	// the register. The model is told this before it answers (see
	// candidate_context above).
	const bool useUpdateTarget = plan.update_target.has_value() && updatesPreviousObject;
	if (!useUpdateTarget && !plan.new_work_target.has_value()) return std::nullopt;
	if (useUpdateTarget)
		return SelectedDirectDocumentWorkTarget{*plan.update_document_id, *plan.update_target, true};
	return SelectedDirectDocumentWorkTarget{*plan.new_work_document_id, *plan.new_work_target, false};
}

std::string direct_document_work_memo(const ProfessionalDocumentResult &content, const std::string &severityLabel) {
	bool cyrillic = false;
	for (std::size_t index = 0; index + 1 < content.title.size(); ++index) {
		const auto lead = static_cast<unsigned char>(content.title[index]);
		if (lead == 0xD0 || lead == 0xD1) { cyrillic = true; break; }
	}
	const std::string severityWord = cyrillic ? "\xD0\xA3\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB5\xD0\xBD\xD1\x8C "
		"\xD1\x80\xD0\xB8\xD1\x81\xD0\xBA\xD0\xB0" : "Severity";
	const std::string recommendationsWord = cyrillic
		? "\xD0\xA0\xD0\xB5\xD0\xBA\xD0\xBE\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB4\xD0\xB0\xD1\x86\xD0\xB8\xD0\xB8"
		: "Recommendations";
	const std::string additionalWord = cyrillic
		? "\xD0\x94\xD0\xBE\xD0\xBF\xD0\xBE\xD0\xBB\xD0\xBD\xD0\xB8\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F "
		  "\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC\xD0\xB0\xD1\x86\xD0\xB8\xD1\x8F"
		: "Additional information";
	std::string memo = "## " + content.title + "\n\n**" + severityWord + ":** "
		+ (severityLabel.empty() ? content.severity : severityLabel);
	for (const std::string &paragraph : content.description) {
		if (paragraph.find_first_not_of(" \t\r\n") == std::string::npos) continue;
		memo += "\n\n" + paragraph;
	}
	std::size_t number = 0;
	for (const std::string &recommendation : content.recommendations) {
		if (recommendation.find_first_not_of(" \t\r\n") == std::string::npos) continue;
		if (number == 0) memo += "\n\n### " + recommendationsWord + "\n";
		memo += "\n" + std::to_string(++number) + ". " + recommendation;
	}
	if (content.additional_information
			&& content.additional_information->find_first_not_of(" \t\r\n") != std::string::npos) {
		memo += "\n\n### " + additionalWord + "\n\n" + *content.additional_information;
	}
	return memo;
}

std::string describe_direct_document_work_result(
		const ProfessionalDocumentResult &content, bool usedUpdateTarget, const std::string &severityLabel) {
	std::string text = (usedUpdateTarget ? "Updated: " : "Added: ") + content.title;
	// The reader sees the word the register itself uses, not the schema's code.
	text += "\n\nSeverity: " + (severityLabel.empty() ? content.severity : severityLabel);
	if (!content.description.empty()) text += "\n\n" + join_paragraphs(content.description);
	return text;
}

} // namespace singularity::documents
