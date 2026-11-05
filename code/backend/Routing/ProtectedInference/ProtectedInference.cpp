#include "Platform/Trace/Trace.hpp"
#include "Routing/ProtectedInference/ProtectedInference.hpp"
#include "Routing/ProtectedInference/ComputeDisclosure.hpp"
#include "Routing/ProtectedInference/SourceMaterialSpans.hpp"
#include "Routing/LiveAnswer/LiveAnswer.hpp"
#include "Provider/Types/GenerationStream.hpp"
#include "Routing/ProtectedInference/TextualAnswer.hpp"
#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "Context/ControlText/ControlText.hpp"
#include "Context/PromptAssembler/PromptAssembler.hpp"
#include "Context/BusinessCalendar/BusinessCalendar.hpp"
#include "PapinianGuard/ProtectedMarkers/ProtectedMarkers.hpp"
#include "PapinianGuard/ProtectedRelations/ProtectedRelations.hpp"
#include "PapinianGuard/ProtectedOperations/ProtectedOperations.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeBudget.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeProtocol.hpp"
#include "PapinianGuard/Types/FactAnchor.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeBinding.hpp"
#include "PapinianGuard/RestorationGrammar/SpelledFigures.hpp"
#include "PapinianGuard/SemanticRoles/SemanticRoles.hpp"

#include "Provider/Types/TaskError.hpp"
#include "Provider/Types/InputPictures.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Node/WorkLog/WorkLog.hpp"
#include "Routing/MemoryDiagnostics/MemoryDiagnostics.hpp"
#include "Routing/Observability/OperationEvent.hpp"
#include "Routing/RoutingLog/OutboundRecord.hpp"

#include <nlohmann/json.hpp>
#include <regex>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <cctype>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <set>
#include <string_view>
#include <vector>

// The memory diagnostic prints a digest of a value the final check found; it
// belongs to a test runner, never to the application.
#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC) && defined(SINGULARITY_NO_FORENSIC_OBSERVABILITY)
#error "SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC must never be compiled into the application"
#endif

namespace singularity::routing {
namespace {

// Shared by the normal send path and the preview-capture path below, so the
// two can never compute privacy metadata differently for the same
// protection result (Phase 08 WP8-B's "one source of truth" requirement).
protected_api::papinian::PrivacyMetadata build_privacy_metadata(
    const protected_api::Policy &policy,
    const protected_api::papinian::ProtectionResult &protected_request) {
  protected_api::papinian::PrivacyMetadata metadata;
  metadata.mode = protected_api::to_string(policy.mode);
  metadata.protected_count = protected_request.entities.size();
  for (const auto &entity : protected_request.entities) {
    const std::string category = entity.type
                                     ? protected_api::papinian::to_string(*entity.type)
                                     : (entity.source == protected_api::papinian::DetectionSource::CustomTerm
                                            ? "CUSTOM_TERM"
                                            : "URL");
    ++metadata.category_counts[category];
  }
  return metadata;
}

// Restores every string leaf of a parsed tool-call arguments JSON value
// through the exact same Papinian Guard `text` above uses - a model may echo
// a request-scoped placeholder token into a tool call's own string arguments
// (e.g. a "query" value) exactly as it can into ordinary prose, and that
// placeholder must be restored back to the real value here for the SAME
// reason (integrity: an un-restored placeholder is not usable downstream
// content) - not because the restored value is then safe to send onward.
// ResearchTools.hpp's execute_search() re-minimizes the real query against
// the Project's own sensitive values immediately before any network call,
// specifically because this restoration step puts the real value back.
// Recurses through objects/arrays only - a tool argument is never anything
// but object/array/string/number/bool/null. integrity_ok is the logical AND
// of every string leaf's own restoration.
struct JsonRestoreOutcome {
  nlohmann::json value;
  bool integrity_ok = true;
	std::string malformed_candidate;
	std::string json_locator;
	std::vector<std::string> normalized_candidates;
	std::string normalized_locator;
	// Each amount or date restoration wrote, with the fact it is, in the order
	// the leaves stand: where the finished text's anchors are found again.
	std::vector<std::pair<std::string, std::string>> anchored;
	// Each string restoration wrote with its anchors, as the lawyer reads it:
	// where this operation finds its own answer again when it is sent back
	// (record_restored_facts).
	std::vector<std::pair<std::string, std::vector<protected_api::papinian::FactAnchor>>> restored_texts;
};

std::string json_pointer_part(std::string value) {
	std::size_t position = 0;
	while ((position = value.find('~', position)) != std::string::npos) {
		value.replace(position, 1, "~0");
		position += 2;
	}
	position = 0;
	while ((position = value.find('/', position)) != std::string::npos) {
		value.replace(position, 1, "~1");
		position += 2;
	}
	return value;
}

// Where a string field of a response stands once restored: a workbook cell's
// value ("/…/cells/0/newValue"), an exact quotation of the document
// ("matchText", an "expectedOldValue"), or prose. A value that is the whole
// field is written the way its place takes it (PapinianGuard::restore).
protected_api::papinian::RestorationField restoration_field(const std::string &jsonLocator) {
	const std::size_t slash = jsonLocator.rfind('/');
	const std::string key = slash == std::string::npos ? jsonLocator : jsonLocator.substr(slash + 1);
	const bool cell = jsonLocator.find("/cells/") != std::string::npos;
	if (cell && (key == "newValue" || key == "expectedOldValue")) return protected_api::papinian::RestorationField::Cell;
	if (key == "matchText" || key == "expectedOldValue") return protected_api::papinian::RestorationField::Quote;
	// A review's change is written into the agreement: a clause as it should
	// read keeps the agreement's own writing of every value in it, so the words
	// it keeps are word for word the agreement's (DocumentIntelligence/ReviewResult).
	if (jsonLocator.find("/changes/") != std::string::npos && (key == "text" || key == "currentText"))
		return protected_api::papinian::RestorationField::Quote;
	return protected_api::papinian::RestorationField::Prose;
}

JsonRestoreOutcome restore_json_strings(const protected_api::papinian::PapinianGuard &guard,
    const nlohmann::json &value, const protected_api::papinian::ProtectionResult &protected_request,
	const std::string &jsonLocator = "") {
  // What the model asks Singularity to work out (localFacts) stays in the symbol
  // plane: the operations executor reads "[[Amount3]]" as the fact it is and
  // answers with symbols (DESIGN3 9.6.1). Restored here, its operands would
  // come back as values no symbol stands for.
  const std::size_t facts = jsonLocator.find("/localFacts");
  if (facts != std::string::npos && (facts + 11 == jsonLocator.size() || jsonLocator[facts + 11] == '/')) {
    JsonRestoreOutcome outcome;
    outcome.value = value;
    return outcome;
  }
  // So does a textual-protocol call of lm_compute (DESIGN4 §1.2, carrier N),
  // whole: {"type":"tool_call","tool":"papinian.compute","arguments":{"ops":
  // [...]}} names symbols for Singularity to work over, never values.
  if (value.is_object() && value.contains("tool") && value.at("tool").is_string()
      && protected_api::papinian::compute::is_compute_tool(value.at("tool").get<std::string>())) {
    JsonRestoreOutcome outcome;
    outcome.value = value;
    return outcome;
  }
  if (value.is_string()) {
    // A textual-protocol answer carries its structured answer as one string:
    // restored field by field like the structured path, its localFacts kept.
    if (const std::optional<nlohmann::json> embedded = textual_answer::embedded_answer(value.get<std::string>())) {
      JsonRestoreOutcome inner = restore_json_strings(guard, *embedded, protected_request, jsonLocator);
      inner.value = inner.value.dump();
      if (inner.json_locator.empty()) inner.json_locator = jsonLocator.empty() ? "/" : jsonLocator;
      return inner;
    }
    // A protected value inside arithmetic the model asked for comes back as one
    // operand ("⟦$2.8 million⟧"), so Singularity can work it out where the lawyer
    // reads it (Context/ComputedStatements).
    const auto restored = guard.restore(
        context::computed_statements::mark_response_operands(value.get<std::string>(), jsonLocator), protected_request,
        restoration_field(jsonLocator));
		JsonRestoreOutcome outcome;
		outcome.value = restored.text;
		outcome.integrity_ok = restored.integrity_ok;
		for (const protected_api::papinian::FactAnchor &anchor : restored.fact_anchors)
			outcome.anchored.emplace_back(restored.text.substr(anchor.start, anchor.length), anchor.key);
		if (!restored.fact_anchors.empty()) outcome.restored_texts.emplace_back(restored.text, restored.fact_anchors);
		outcome.json_locator = jsonLocator.empty() ? "/" : jsonLocator;
		outcome.normalized_candidates = restored.normalized_placeholders;
		if (!outcome.normalized_candidates.empty()) outcome.normalized_locator = outcome.json_locator;
		if (!restored.unknown_or_malformed_placeholders.empty())
			outcome.malformed_candidate = restored.unknown_or_malformed_placeholders.front();
		return outcome;
  }
  if (value.is_object()) {
    nlohmann::json result = nlohmann::json::object();
    bool ok = true;
		JsonRestoreOutcome outcome;
    for (auto entry = value.begin(); entry != value.end(); ++entry) {
		const std::string childLocator = jsonLocator + "/" + json_pointer_part(entry.key());
		const JsonRestoreOutcome child = restore_json_strings(guard, entry.value(), protected_request, childLocator);
      result[entry.key()] = child.value;
      ok = ok && child.integrity_ok;
		outcome.anchored.insert(outcome.anchored.end(), child.anchored.begin(), child.anchored.end());
		outcome.restored_texts.insert(outcome.restored_texts.end(), child.restored_texts.begin(), child.restored_texts.end());
		if (outcome.malformed_candidate.empty() && !child.malformed_candidate.empty()) {
			outcome.malformed_candidate = child.malformed_candidate;
			outcome.json_locator = child.json_locator;
		}
		outcome.normalized_candidates.insert(outcome.normalized_candidates.end(),
			child.normalized_candidates.begin(), child.normalized_candidates.end());
		if (outcome.normalized_locator.empty() && !child.normalized_locator.empty())
			outcome.normalized_locator = child.normalized_locator;
    }
		outcome.value = std::move(result);
		outcome.integrity_ok = ok;
		return outcome;
  }
  if (value.is_array()) {
    nlohmann::json result = nlohmann::json::array();
    bool ok = true;
		JsonRestoreOutcome outcome;
		std::size_t index = 0;
		for (const nlohmann::json &entry : value) {
			const JsonRestoreOutcome child = restore_json_strings(
				guard, entry, protected_request, jsonLocator + "/" + std::to_string(index++));
      result.push_back(child.value);
      ok = ok && child.integrity_ok;
			outcome.anchored.insert(outcome.anchored.end(), child.anchored.begin(), child.anchored.end());
			outcome.restored_texts.insert(outcome.restored_texts.end(), child.restored_texts.begin(), child.restored_texts.end());
			if (outcome.malformed_candidate.empty() && !child.malformed_candidate.empty()) {
				outcome.malformed_candidate = child.malformed_candidate;
				outcome.json_locator = child.json_locator;
			}
			outcome.normalized_candidates.insert(outcome.normalized_candidates.end(),
				child.normalized_candidates.begin(), child.normalized_candidates.end());
			if (outcome.normalized_locator.empty() && !child.normalized_locator.empty())
				outcome.normalized_locator = child.normalized_locator;
    }
		outcome.value = std::move(result);
		outcome.integrity_ok = ok;
		return outcome;
  }
	JsonRestoreOutcome outcome;
	outcome.value = value;
	return outcome;
}

// A literal Singularity's own protocol issues: a handle ("I1", "T12", "D2") or a
// lower-case identifier ("leave_unchanged", "create_content"), dotted ones
// among them - a colleague's tool, "work.give", "document.read" (2026-11-02:
// every piece of a colleague's work stopped because "work" was a protected
// spelling in that request; D-237). It is asked only of the enum and const
// strings of a request's own schemas (collect_schema_literals), which
// Singularity builds from fixed lists of its own words; text a person or a
// document supplied never reaches it, so a dotted value of theirs
// ("anna.ivanova") is still read by every other check. It cannot carry
// Project text, and a protected term that happens to equal one (a custom term
// "I1") is handled by response restoration, not treated as a schema leak.
bool is_protocol_literal(const std::string &literal) {
	if (literal.empty()) return false;
	std::size_t letters = 0;
	while (letters < literal.size() && literal[letters] >= 'A' && literal[letters] <= 'Z') ++letters;
	if (letters >= 1 && letters <= 2 && letters < literal.size()
			&& std::all_of(literal.begin() + static_cast<std::ptrdiff_t>(letters), literal.end(),
				[](char character) { return character >= '0' && character <= '9'; }))
		return true;
	return literal.front() >= 'a' && literal.front() <= 'z' && literal.back() != '.'
		&& std::all_of(literal.begin(), literal.end(), [](char character) {
			return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_'
				|| character == '.';
		});
}

// Every enum and const string anywhere in a provider body: the only places a
// schema (tool parameters, a response format) carries values rather than
// structure. Schemas are not transformed by the Papinian boundary.
void collect_schema_literals(const nlohmann::json &value, std::vector<std::string> &literals) {
	if (value.is_object()) {
		for (auto entry = value.begin(); entry != value.end(); ++entry) {
			if (entry.key() == "enum" && entry.value().is_array()) {
				for (const auto &literal : entry.value())
					if (literal.is_string()) literals.push_back(literal.get<std::string>());
			} else if (entry.key() == "const" && entry.value().is_string()) {
				literals.push_back(entry.value().get<std::string>());
			}
			collect_schema_literals(entry.value(), literals);
		}
	} else if (value.is_array()) {
		for (const auto &entry : value) collect_schema_literals(entry, literals);
	}
}

// Every picture or file a provider body carries, as the base64 it carries it
// in (D-256): a data URL of bytes anywhere (an OpenAI-style image_url) and an
// Anthropic block's base64 source. Everything else a body holds is words.
void collect_body_pictures(const nlohmann::json &value, std::vector<std::string> &pictures) {
	if (value.is_object()) {
		const auto type = value.find("type");
		const auto data = value.find("data");
		if (type != value.end() && type->is_string() && type->get_ref<const std::string &>() == "base64"
				&& data != value.end() && data->is_string())
			pictures.push_back(data->get<std::string>());
		for (auto entry = value.begin(); entry != value.end(); ++entry) collect_body_pictures(entry.value(), pictures);
	} else if (value.is_array()) {
		for (const auto &entry : value) collect_body_pictures(entry, pictures);
	} else if (value.is_string()) {
		const std::string &text = value.get_ref<const std::string &>();
		const std::size_t marker = text.rfind("data:", 0) == 0 ? text.find(";base64,") : std::string::npos;
		if (marker != std::string::npos) pictures.push_back(text.substr(marker + 8));
	}
}

void merge_restore_diagnostics(JsonRestoreOutcome &target, const JsonRestoreOutcome &child) {
	target.integrity_ok = target.integrity_ok && child.integrity_ok;
	target.anchored.insert(target.anchored.end(), child.anchored.begin(), child.anchored.end());
	target.restored_texts.insert(target.restored_texts.end(), child.restored_texts.begin(), child.restored_texts.end());
	if (target.malformed_candidate.empty() && !child.malformed_candidate.empty()) {
		target.malformed_candidate = child.malformed_candidate;
		target.json_locator = child.json_locator;
	}
	target.normalized_candidates.insert(target.normalized_candidates.end(),
		child.normalized_candidates.begin(), child.normalized_candidates.end());
	if (target.normalized_locator.empty() && !child.normalized_locator.empty())
		target.normalized_locator = child.normalized_locator;
}

JsonRestoreOutcome restore_declared_data_leaves(
		const protected_api::papinian::PapinianGuard &guard, const nlohmann::json &value,
		const nlohmann::json &shape,
		const protected_api::papinian::ProtectionResult &protected_request,
		const std::string &jsonLocator = "") {
	if (shape.is_boolean() && shape.get<bool>())
		return restore_json_strings(guard, value, protected_request, jsonLocator);
	JsonRestoreOutcome outcome;
	outcome.value = value;
	if (value.is_object() && shape.is_object() && shape.contains("properties")
			&& shape.at("properties").is_object()) {
		for (auto property = shape.at("properties").begin(); property != shape.at("properties").end(); ++property) {
			if (!value.contains(property.key())) continue;
			const auto child = restore_declared_data_leaves(guard, value.at(property.key()), property.value(),
				protected_request, jsonLocator + "/" + json_pointer_part(property.key()));
			outcome.value[property.key()] = child.value;
			merge_restore_diagnostics(outcome, child);
		}
	} else if (value.is_array() && shape.is_object() && shape.contains("items")) {
		for (std::size_t index = 0; index < value.size(); ++index) {
			const auto child = restore_declared_data_leaves(guard, value.at(index), shape.at("items"),
				protected_request, jsonLocator + "/" + std::to_string(index));
			outcome.value[index] = child.value;
			merge_restore_diagnostics(outcome, child);
		}
	}
	return outcome;
}

bool json_type_matches(const nlohmann::json &value, const std::string &type) {
	if (type == "object") return value.is_object();
	if (type == "array") return value.is_array();
	if (type == "string") return value.is_string();
	if (type == "boolean") return value.is_boolean();
	if (type == "integer") return value.is_number_integer();
	if (type == "number") return value.is_number();
	if (type == "null") return value.is_null();
	return false;
}

bool response_matches_schema(const nlohmann::json &value, const nlohmann::json &schema) {
	if (!schema.is_object()) return false;
	if (schema.contains("anyOf") && schema.at("anyOf").is_array()) {
		std::size_t matches = 0;
		for (const auto &alternative : schema.at("anyOf"))
			if (response_matches_schema(value, alternative)) ++matches;
		return matches == 1;
	}
	if (schema.contains("enum") && schema.at("enum").is_array()
			&& std::find(schema.at("enum").begin(), schema.at("enum").end(), value) == schema.at("enum").end())
		return false;
	if (schema.contains("type")) {
		bool matches = false;
		const auto &type = schema.at("type");
		if (type.is_string()) matches = json_type_matches(value, type.get<std::string>());
		else if (type.is_array())
			for (const auto &candidate : type)
				if (candidate.is_string() && json_type_matches(value, candidate.get<std::string>())) matches = true;
		if (!matches) return false;
	}
	if (value.is_object()) {
		if (schema.contains("required") && schema.at("required").is_array())
			for (const auto &required : schema.at("required"))
				if (!required.is_string() || !value.contains(required.get<std::string>())) return false;
		if (schema.value("additionalProperties", true) == false && schema.contains("properties"))
			for (auto entry = value.begin(); entry != value.end(); ++entry)
				if (!schema.at("properties").contains(entry.key())) return false;
		if (schema.contains("properties") && schema.at("properties").is_object())
			for (auto property = schema.at("properties").begin(); property != schema.at("properties").end(); ++property)
				if (value.contains(property.key()) && !response_matches_schema(value.at(property.key()), property.value()))
					return false;
	}
	if (value.is_array()) {
		if (schema.contains("minItems") && value.size() < schema.at("minItems").get<std::size_t>()) return false;
		if (schema.contains("maxItems") && value.size() > schema.at("maxItems").get<std::size_t>()) return false;
		if (schema.contains("items"))
			for (const auto &entry : value)
				if (!response_matches_schema(entry, schema.at("items"))) return false;
	}
	return true;
}

std::string protected_symbol_failure_message(const JsonRestoreOutcome &outcome) {
	return "Papinian Guard could not verify the provider's protected symbol"
		+ (outcome.malformed_candidate.empty() ? std::string() : " candidate=" + outcome.malformed_candidate)
		+ (outcome.json_locator.empty() ? std::string() : " locator=" + outcome.json_locator)
		+ ". The result was not restored.";
}

std::string canonical_marker_for_candidate(const std::string &candidate,
		const protected_api::papinian::ProtectionResult &request) {
	// "[[ e5 ]]" is [[E5]], "[[ org1 ]]" is [[Org1]] - the identity as the table
	// issued it, matched case-blind; a name part keeps its word: "[[E5.First]]"
	// is [[E5.first]]. An identity no row has is written the way its letters
	// suggest ([[E9]], [[Amount9]]).
	std::string identity;
	std::string part;
	bool inPart = false;
	for (const unsigned char character : candidate) {
		if (character == '.' && !identity.empty()) inPart = true;
		else if (std::isalnum(character) != 0)
			(inPart ? part : identity) += static_cast<char>(inPart ? std::tolower(character) : character);
	}
	if (identity.empty()) return std::string();
	std::string issued;
	for (const auto &entry : request.mapping) {
		std::string base = entry.placeholder.size() > 4 ? entry.placeholder.substr(2, entry.placeholder.size() - 4) : std::string();
		std::string folded = base;
		std::string wanted = identity;
		for (char &character : folded) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		for (char &character : wanted) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		if (folded == wanted) {
			issued = base;
			break;
		}
	}
	if (issued.empty()) {
		std::size_t letters = 0;
		while (letters < identity.size() && std::isalpha(static_cast<unsigned char>(identity[letters])) != 0) ++letters;
		issued = identity;
		for (std::size_t index = 0; index < letters; ++index)
			issued[index] = static_cast<char>(letters == 1 || index == 0 ? std::toupper(static_cast<unsigned char>(issued[index]))
				: std::tolower(static_cast<unsigned char>(issued[index])));
	}
	return "[[" + issued + (part.empty() ? std::string() : "." + part) + "]]";
}

// A symbol the provider wrote that no row of this request, no result Singularity
// worked out and no table of the operation ever was ("[[Date50]]" where the
// request's dates end at [[Date49]]). It stands for no value of the lawyer's,
// so nothing is lost or disclosed by it; where it once made a whole answer
// unusable (live R2 T4 and RU1 T5, 27 Sep 2026: the task failed), what is
// restored shows a neutral gap in its place, in the answer's own language,
// and the answer contract - reading the reply as the model wrote it - names
// the symbol to the model, which may write it again properly.
bool issued_symbol(const std::string &marker, const protected_api::papinian::ProtectionResult &request,
		const protected_api::papinian::OperationProtectedSymbolTable *symbols) {
	const std::string whole = canonical_marker_for_candidate(marker, request);
	const std::size_t dot = whole.find('.');
	const std::string identity = dot == std::string::npos ? whole : whole.substr(0, dot) + "]]";
	for (const auto &entry : request.mapping)
		if (entry.placeholder == identity) return true;
	if (symbols == nullptr) return false;
	if (symbols->derived_facts.count(identity) != 0) return true;
	for (const auto &entry : symbols->mapping)
		if (entry.placeholder == identity) return true;
	for (const auto &table : symbols->tables)
		if ("[[" + table.handle + "]]" == identity) return true;
	// The protocol's own handles, read from the legend: never a fact.
	return identity.rfind("[[Schedule", 0) == 0 || identity.rfind("[[Calendar", 0) == 0;
}

std::string gap_unissued_symbols(const std::string &text, const protected_api::papinian::ProtectionResult &request,
		const protected_api::papinian::OperationProtectedSymbolTable *symbols, std::size_t &gapped) {
	static const std::regex symbol(R"(\[\[([A-Za-z]+)[0-9]+(\.[a-z]+)?\]\])");
	std::string out;
	std::size_t last = 0;
	bool russian = false;
	bool decided = false;
	for (auto match = std::sregex_iterator(text.begin(), text.end(), symbol); match != std::sregex_iterator(); ++match) {
		const std::string marker = match->str();
		if (issued_symbol(marker, request, symbols)) continue;
		if (!decided) {
			russian = protected_api::papinian::semantic::language_of(text) == protected_api::papinian::semantic::Language::Russian;
			decided = true;
		}
		namespace control = context::control_text;
		const std::string kind = (*match)[1].str();
		const control::Template gap = kind == "Date" ? (russian ? control::Template::FigureGapDateRu : control::Template::FigureGapDateEn)
			: kind == "Rate" ? (russian ? control::Template::FigureGapRateRu : control::Template::FigureGapRateEn)
			: kind == "Amount" || kind == "Number" || kind == "Count"
				? (russian ? control::Template::FigureGapAmountRu : control::Template::FigureGapAmountEn)
			: (russian ? control::Template::FigureGapOtherRu : control::Template::FigureGapOtherEn);
		out += text.substr(last, static_cast<std::size_t>(match->position()) - last) + control::render(gap);
		last = static_cast<std::size_t>(match->position() + match->length());
		++gapped;
	}
	if (last == 0) return text;
	return out + text.substr(last);
}

nlohmann::json gap_unissued_symbols(const nlohmann::json &value, const protected_api::papinian::ProtectionResult &request,
		const protected_api::papinian::OperationProtectedSymbolTable *symbols, std::size_t &gapped) {
	if (value.is_string()) return gap_unissued_symbols(value.get<std::string>(), request, symbols, gapped);
	if (!value.is_object() && !value.is_array()) return value;
	nlohmann::json out = value;
	for (auto entry = out.begin(); entry != out.end(); ++entry) *entry = gap_unissued_symbols(*entry, request, symbols, gapped);
	return out;
}

// The reply's never-issued symbols gapped where restoration reads the reply,
// and nowhere else: a typed turn's payload only - never its protectionMetadata,
// which restoration leaves as sent - any other JSON's strings, or the text.
// Unchanged, byte for byte, when there is nothing to gap.
std::string gap_unissued_in_reply(const std::string &text, const protected_api::papinian::ProtectionResult &request,
		const protected_api::papinian::OperationProtectedSymbolTable *symbols, std::size_t &gapped) {
	const auto structured = nlohmann::json::parse(text, nullptr, false);
	if (!structured.is_object() && !structured.is_array()) return gap_unissued_symbols(text, request, symbols, gapped);
	const std::size_t before = gapped;
	nlohmann::json out = structured;
	const bool typedTurn = structured.is_object()
		&& structured.contains("controlPlane") && structured.at("controlPlane").is_object()
		&& structured.contains("dataPlane") && structured.at("dataPlane").is_object()
		&& structured.at("dataPlane").contains("payload") && structured.at("dataPlane").at("payload").is_object();
	if (typedTurn) out["dataPlane"]["payload"] = gap_unissued_symbols(structured.at("dataPlane").at("payload"), request, symbols, gapped);
	else out = gap_unissued_symbols(structured, request, symbols, gapped);
	return gapped == before ? text : out.dump();
}

providers::SafeResponseDiagnostics safe_response_diagnostics(
		const std::string &candidate, const std::string &jsonPointer,
		const protected_api::papinian::ProtectionResult &request, bool acceptedNormalization) {
	providers::SafeResponseDiagnostics diagnostics;
	diagnostics.issue_code = acceptedNormalization
		? "protected_symbol_normalized" : "protected_symbol_integrity_rejected";
	diagnostics.marker_candidate = candidate;
	diagnostics.canonical_marker = canonical_marker_for_candidate(candidate, request);
	diagnostics.json_pointer = jsonPointer.empty() ? "/" : jsonPointer;
	diagnostics.normalization_outcome = acceptedNormalization ? "normalized_wrapper"
		: std::any_of(request.mapping.begin(), request.mapping.end(), [&](const auto &entry) {
			return entry.placeholder == diagnostics.canonical_marker;
		}) ? "malformed_wrapper" : "unknown_identity";
	const auto mapped = std::find_if(request.mapping.begin(), request.mapping.end(),
		[&](const auto &entry) { return entry.placeholder == diagnostics.canonical_marker; });
	diagnostics.marker_type = mapped != request.mapping.end() && mapped->type
		? protected_api::papinian::to_string(*mapped->type) : "unknown";
	return diagnostics;
}

void copy_safe_diagnostics(protected_api::papinian::PrivacyMetadata &metadata,
		const providers::SafeResponseDiagnostics &diagnostics) {
	metadata.response_issue_code = diagnostics.issue_code;
	metadata.response_marker_candidate = diagnostics.marker_candidate;
	metadata.response_canonical_marker = diagnostics.canonical_marker;
	metadata.response_marker_type = diagnostics.marker_type;
	metadata.response_json_pointer = diagnostics.json_pointer;
	metadata.response_normalization_outcome = diagnostics.normalization_outcome;
	metadata.response_repair_class = diagnostics.repair_class;
	metadata.response_repair_attempts = diagnostics.repair_attempts;
}

enum class ProtectionPolicy { MachineOwnedPassThrough, ExactMappingOnly, SemanticDetection };

struct PlannedDataLeaf {
	std::string identity;
	std::string value;
	ProtectionPolicy policy = ProtectionPolicy::SemanticDetection;
	// Singularity's own instruction carried as a typed request's leaf
	// (request.capabilityInstruction): what a conversational prompt's
	// CAPABILITY section is. Its examples of the symbols Singularity issues
	// ("[[Org1]]", "[[Amount3]]") are Singularity's words (exact_leaf_spans).
	bool lm_instruction = false;
};

struct ProtectionPlan {
	std::vector<PlannedDataLeaf> leaves;
	std::size_t machine_pass_through = 0;
	std::size_t exact_mapping = 0;
	std::size_t semantic_detection = 0;
};

bool path_is(const std::vector<std::string> &path,
		std::initializer_list<const char *> expected) {
	if (path.size() != expected.size()) return false;
	std::size_t index = 0;
	for (const char *part : expected)
		if (path[index++] != part) return false;
	return true;
}

ProtectionPolicy policy_for_typed_path(const std::vector<std::string> &path) {
	// These exact paths are emitted solely by the production compilers. Their
	// complete value spaces are LM-owned instructions, enums, or opaque handles.
	if (path_is(path, {"instruction"}) || path_is(path, {"turnInstruction"})
			|| path_is(path, {"request", "system", "profileId"})
			|| path_is(path, {"request", "system", "instruction"})
			|| path_is(path, {"request", "task", "presetId"})
			|| path_is(path, {"request", "task", "name"})
			|| path_is(path, {"request", "task", "instruction"})
			|| path_is(path, {"request", "task", "expectedOutputStyle"})
			|| path_is(path, {"request", "projectContext", "*", "handle"})
			|| path_is(path, {"request", "projectContext", "*", "contentClass"})
			|| path_is(path, {"request", "sourceMaterial", "*", "handle"})
			|| path_is(path, {"request", "sourceMaterial", "*", "contentClass"})
			|| path_is(path, {"request", "history", "*", "handle"})
			|| path_is(path, {"request", "history", "*", "contentClass"})
			|| path_is(path, {"state", "target", "role"})
			|| path_is(path, {"state", "target", "items", "*", "handle"})
			|| path_is(path, {"state", "target", "items", "*", "fields", "*", "handle"})
			|| path_is(path, {"state", "acceptedItemHandles", "*"})
			|| path_is(path, {"state", "methodology", "*", "handle"})
			|| path_is(path, {"state", "externalEvidence", "*", "handle"})
			|| path_is(path, {"state", "externalEvidence", "*", "contentKind"})
			|| path_is(path, {"state", "externalEvidence", "*", "sourceClass"})
			|| path_is(path, {"state", "outstandingIssue", "category"})
			|| path_is(path, {"state", "target", "items", "*", "validationIssue", "category"})
			// A live forensic acceptance run proved a masked-id echo defect
			// concretely (see compile_connected_document_turn_data()'s own
			// comment on "state.tableEvidenceObjectives"): an already-bound
			// evidence-objective id is an ordinary backend-tracked handle,
			// the same class as activeTableId/tableHandle itself, never
			// sensitive Project content - it must stay byte-stable so the
			// model can safely copy it back on the next call.
			|| path_is(path, {"state", "tableEvidenceObjectives", "*", "tableHandle"})
			|| path_is(path, {"state", "tableEvidenceObjectives", "*", "evidenceObjectiveId"})
			// A real live acceptance run proved this second, plural "targets"
			// shape (ConnectedDocumentAction.cpp's own compile_connected_
			// document_turn_data()/compile_connected_document_review_context())
			// never received its own typed-path entries here at all, so every
			// one of its own opaque structural handles - table/item/field
			// handles, region handles, model-chosen-but-canonical objective and
			// output ids, and every enum - fell through to SemanticDetection
			// unconditionally, alongside the free text/document values that
			// genuinely need it. document/table NAMES are deliberately absent
			// below (never LM-owned handles - may themselves be a client or
			// company name) and so are description/reason/note/message/value/
			// label fields (genuinely free text or real document content).
			|| path_is(path, {"state", "targets", "*", "role"})
			|| path_is(path, {"state", "targets", "*", "items", "*", "handle"})
			|| path_is(path, {"state", "targets", "*", "items", "*", "regionHandle"})
			|| path_is(path, {"state", "targets", "*", "items", "*", "fields", "*", "handle"})
			|| path_is(path, {"state", "targets", "*", "items", "*", "fields", "*", "disposition"})
			|| path_is(path, {"state", "targets", "*", "items", "*", "fields", "*", "structuralRole"})
			|| path_is(path, {"state", "requiredOutputs", "*", "id"})
			|| path_is(path, {"state", "requiredOutputs", "*", "itemHandle"})
			|| path_is(path, {"state", "requiredOutputs", "*", "fieldHandle"})
			|| path_is(path, {"state", "selectedOutputRegions", "*", "regionHandle"})
			|| path_is(path, {"state", "selectedOutputRegions", "*", "fieldDispositions", "*", "fieldHandle"})
			|| path_is(path, {"state", "selectedOutputRegions", "*", "fieldDispositions", "*", "disposition"})
			|| path_is(path, {"state", "requiredEvidence", "*", "documentId"})
			|| path_is(path, {"state", "inspectionWorkingEvidence", "inspectedTables", "*"})
			|| path_is(path, {"state", "inspectionWorkingEvidence", "authorship"})
			// compile_connected_document_review_context()'s own dedicated
			// projection - a real live acceptance run proved this exact gap
			// concretely: its own evidenceObjectives[].id, an ordinary model-
			// chosen-but-canonical handle once first declared (see
			// EvidenceObjective::id's own comment), overlapped a real protected
			// value's own mention and was masked into a "[[E119]]"-style
			// placeholder the model then could not distinguish from a real
			// entity reference, corrupting the very "reuse this id" correction
			// it was quoted inside. evidenceObjectives[].description/
			// documentName/closureReason/unresolvedQuestions/scopeLimitations
			// and selectedRegions[].fields[].label/reason/proposedValue are
			// deliberately absent below - genuine free text or real document
			// content that must keep receiving semantic protection.
			|| path_is(path, {"evidenceObjectives", "*", "id"})
			|| path_is(path, {"evidenceObjectives", "*", "tableHandle"})
			|| path_is(path, {"evidenceObjectives", "*", "coverageMode"})
			|| path_is(path, {"selectedRegions", "*", "itemHandle"})
			|| path_is(path, {"selectedRegions", "*", "fields", "*", "fieldHandle"})
			|| path_is(path, {"selectedRegions", "*", "fields", "*", "disposition"}))
		return ProtectionPolicy::MachineOwnedPassThrough;
	// LM-authored instruction text that carries no Project data by construction
	// (a bound artifact's preview and structured-record candidates travel in
	// request.candidateContext): never semantically scored - ordinary
	// instruction vocabulary must never be replaced by a symbol - but still
	// covered by the frozen mapping and the outbound leak validation.
	if (path_is(path, {"request", "capabilityInstruction"}) || path_is(path, {"request", "sourceCoverage"})
			|| path_is(path, {"request", "requestCoverage"}))
		return ProtectionPolicy::ExactMappingOnly;
	// Keep the typed policy explicit: new/unknown string paths fail safe to semantic.
	return ProtectionPolicy::SemanticDetection;
}

// One contiguous span of a semantic leaf. Only the conversational turn's prompt
// leaf ("/prompt") can be a PromptAssembler rendering, and it is treated as
// one only while its exact structure holds: the SYSTEM section starts at
// offset 0 and every further section follows the previous one directly, in
// PromptAssembler's canonical order, as "<<<LM_SECTION:NAME>>>\n" + content +
// "\n<<<LM_END_SECTION:NAME>>>\n\n", with no unescaped "<<<LM_" inside the
// content (PromptAssembler renders every such occurrence in data as
// "<<<LM_LITERAL_"). Within that structure the section marker lines, the
// instruction sections (SYSTEM, TASK, CAPABILITY, SOURCE_COVERAGE, TEMPORAL_CONTEXT, REQUEST_COVERAGE - no
// Project data by construction) and PromptAssembler's constant framing (a
// data section's preamble, or a placeholder standing for its whole content)
// are machine-owned: never scored, never rewritten by the operation's mapping
// and never validated as if they could hold a protected value, so a phrase
// detection finds elsewhere (e.g. "Singularity" in a research wrapper) cannot
// rewrite an instruction. Every other span is scored, mapped and validated.
// At the first deviation recognition ends and everything from there on is one
// scored span - including anything appended after CURRENT_REQUEST
// (continuations, corrections, research wrappers), which is not escaped.
// Every other leaf (typed operation data, Automatic Context prompts, direct
// model prompts) is one scored span, so boundary syntax inside Project text
// can never hide a value.
struct LeafSpan {
	std::size_t from = 0;
	std::size_t to = 0;
	bool machine_owned = false;
	// The person's own words in this request - what they typed now, their
	// standing instructions, the Project's instructions - as opposed to a
	// document's text or Singularity's own framing. Only used to decide which
	// spelling of a name their answer comes back in.
	bool user_authored = false;
	// An earlier turn of this chat carried forward (HISTORY): mostly what a
	// model wrote. Only used to record where a writing was met.
	bool model_authored = false;
};

std::vector<LeafSpan> semantic_leaf_spans(const std::string &identity, const std::string &value) {
	static constexpr std::array<std::string_view, 14> kCanonicalSections{"SYSTEM", "USER_DEFAULT_INSTRUCTIONS",
		"MATTER_INSTRUCTIONS", "TASK", "CAPABILITY", "CANDIDATE_CONTEXT", "PROJECT_CONTEXT", "SOURCE_MATERIAL",
		"SOURCE_COVERAGE", "TEMPORAL_CONTEXT", "CONVERSATION_STATE", "HISTORY", "REQUEST_COVERAGE", "CURRENT_REQUEST"};
	// TEMPORAL_CONTEXT is Singularity's own statement of relations between document
	// handles (Context/TemporalContext) - it carries no date and no Project text.
	static constexpr std::array<std::string_view, 6> kInstructionSections{"SYSTEM", "TASK", "CAPABILITY",
		"SOURCE_COVERAGE", "TEMPORAL_CONTEXT", "REQUEST_COVERAGE"};
	static constexpr std::array<std::string_view, 3> kPersonsOwnSections{"USER_DEFAULT_INSTRUCTIONS",
		"MATTER_INSTRUCTIONS", "CURRENT_REQUEST"};
	std::vector<LeafSpan> spans;
	bool personsOwnSection = false;
	bool historySection = false;
	const auto addSpan = [&](std::size_t from, std::size_t to, bool machineOwned) {
		if (to > from)
			spans.push_back({from, to, machineOwned, machineOwned ? false : personsOwnSection,
				machineOwned ? false : historySection});
	};
	std::size_t cursor = 0;
	if (identity == "/prompt") {
		std::size_t order = 0;
		while (order < kCanonicalSections.size()) {
			std::string name;
			std::size_t found = order;
			for (; found < kCanonicalSections.size(); ++found) {
				const std::string opening = "<<<LM_SECTION:" + std::string(kCanonicalSections[found]) + ">>>\n";
				if (value.compare(cursor, opening.size(), opening) != 0) continue;
				name = std::string(kCanonicalSections[found]);
				break;
			}
			if (name.empty() || (cursor == 0 && name != "SYSTEM")) break;
			personsOwnSection = std::find(kPersonsOwnSections.begin(), kPersonsOwnSections.end(), name)
				!= kPersonsOwnSections.end();
			historySection = name == "HISTORY";
			const std::size_t contentStart = cursor + name.size() + 18;
			const std::string closing = "\n<<<LM_END_SECTION:" + name + ">>>\n\n";
			const std::size_t close = value.find(closing, contentStart);
			if (close == std::string::npos) break;
			bool escaped = true;
			for (std::size_t marker = value.find("<<<LM_", contentStart); marker != std::string::npos && marker < close;
					marker = value.find("<<<LM_", marker + 6)) {
				if (value.compare(marker, 14, "<<<LM_LITERAL_") == 0) continue;
				escaped = false;
				break;
			}
			if (!escaped) break;
			addSpan(cursor, contentStart, true);
			const bool instruction = std::find(kInstructionSections.begin(), kInstructionSections.end(), name)
				!= kInstructionSections.end();
			if (instruction) {
				addSpan(contentStart, close, true);
			} else {
				const std::string_view preamble = name == "PROJECT_CONTEXT" ? context::kProjectContextPreamble
					: name == "SOURCE_MATERIAL" ? context::kSourceMaterialPreamble
					: name == "CONVERSATION_STATE" ? context::kConversationStatePreamble
					: name == "HISTORY" ? context::kHistoryPreamble : std::string_view();
				std::size_t dataStart = contentStart;
				if (!preamble.empty() && close - contentStart >= preamble.size()
						&& value.compare(contentStart, preamble.size(), preamble) == 0) {
					addSpan(contentStart, contentStart + preamble.size(), true);
					dataStart += preamble.size();
				}
				const std::string_view data(value.data() + dataStart, close - dataStart);
				const bool placeholder =
					data == context::kNoneSuppliedPlaceholder || data == context::kExcludedByBudgetPlaceholder;
				if (name == "SOURCE_MATERIAL" && !placeholder) {
					for (const auto &part : source_material::passage_spans(value, dataStart, close))
						addSpan(part.from, part.to, part.machine_owned);
				} else if (!placeholder && (name == "CONVERSATION_STATE" || name == "HISTORY")) {
					// Both grow by appending: CONVERSATION_STATE one record per line,
					// HISTORY one earlier turn per block. Scored record by record, a
					// turn costs what it added, not what the conversation has become.
					for (const auto &part : source_material::record_spans(value, dataStart, close,
							name == "CONVERSATION_STATE" ? "\n" : "\n\n"))
						addSpan(part.from, part.to, part.machine_owned);
				} else {
					addSpan(dataStart, close, placeholder);
				}
			}
			addSpan(close, close + closing.size(), true);
			cursor = close + closing.size();
			order = found + 1;
			if (name == "CURRENT_REQUEST") break;
		}
	}
	// Singularity's own continuation lines and field labels (Context/ControlText),
	// wherever they stand in what is left, are control text: never scored,
	// rewritten or validated. Everything between them is data.
	// So is a verbatim span (DESIGN4 §8.7): the model's own bytes, sent back
	// exactly as the provider wrote them - never scored, mapped or validated.
	// Only a span whose bytes the operation registered reaches this point
	// (execute_protected_generation takes the marks off every other first).
	std::size_t at = cursor;
	const std::string_view rest(value.data() + cursor, value.size() - cursor);
	std::vector<std::pair<std::size_t, std::size_t>> owned;
	for (const context::control_text::ControlSpan &control : context::control_text::recognise(rest))
		owned.emplace_back(cursor + control.from, cursor + control.to);
	for (const protected_api::papinian::compute::VerbatimSpan &span : protected_api::papinian::compute::verbatim_spans(rest))
		owned.emplace_back(cursor + span.from, cursor + span.to);
	std::sort(owned.begin(), owned.end());
	for (const auto &[from, to] : owned) {
		if (from < at) continue;
		addSpan(at, from, false);
		addSpan(from, to, true);
		at = to;
	}
	addSpan(at, value.size(), false);
	return spans;
}

// The scored parts of one semantic leaf, for detection.
struct DetectionPart {
	std::string text;
	bool user_authored = false;
	bool model_authored = false;
	// The document a SOURCE_MATERIAL passage belongs to in this prompt, by its
	// marker's document component ("DOC_2"); empty for any other part.
	std::string document;
};

std::vector<DetectionPart> semantic_detection_parts(const std::string &identity, const std::string &value) {
	std::vector<DetectionPart> parts;
	std::string document;
	for (const LeafSpan &span : semantic_leaf_spans(identity, value)) {
		if (span.machine_owned) {
			// "[SOURCE:DOC_2:CHUNK_5]\n" opens a passage of DOC_2.
			document.clear();
			if (source_material::opening_marker_length(value, span.from, span.to) == span.to - span.from) {
				const std::size_t from = span.from + 8;
				document = value.substr(from, value.find(':', from) - from);
			}
			continue;
		}
		std::string part = value.substr(span.from, span.to - span.from);
		if (part.find_first_not_of(" \t\r\n") != std::string::npos)
			parts.push_back({std::move(part), span.user_authored, span.model_authored, document});
		document.clear();
	}
	return parts;
}

std::string pointer_part(std::string value) {
	std::size_t position = 0;
	while ((position = value.find('~', position)) != std::string::npos) {
		value.replace(position, 1, "~0"); position += 2;
	}
	position = 0;
	while ((position = value.find('/', position)) != std::string::npos) {
		value.replace(position, 1, "~1"); position += 2;
	}
	return value;
}

void enumerate_data_leaves(const nlohmann::json &value,
		std::vector<std::string> &typedPath, const std::string &pointer,
		ProtectionPlan &plan) {
	if (value.is_string()) {
		const auto policy = policy_for_typed_path(typedPath);
		plan.leaves.push_back({pointer.empty() ? "/" : pointer,
			value.get<std::string>(), policy, path_is(typedPath, {"request", "capabilityInstruction"})});
		if (policy == ProtectionPolicy::MachineOwnedPassThrough) ++plan.machine_pass_through;
		else if (policy == ProtectionPolicy::ExactMappingOnly) ++plan.exact_mapping;
		else ++plan.semantic_detection;
		return;
	}
	if (value.is_object()) {
		for (auto entry = value.begin(); entry != value.end(); ++entry) {
			typedPath.push_back(entry.key());
			enumerate_data_leaves(entry.value(), typedPath,
				pointer + "/" + pointer_part(entry.key()), plan);
			typedPath.pop_back();
		}
	} else if (value.is_array()) {
		for (std::size_t index = 0; index < value.size(); ++index) {
			typedPath.push_back("*");
			enumerate_data_leaves(value.at(index), typedPath,
				pointer + "/" + std::to_string(index), plan);
			typedPath.pop_back();
		}
	}
}

ProtectionPlan compile_protection_plan(const nlohmann::json &data) {
	ProtectionPlan plan;
	std::vector<std::string> path;
	enumerate_data_leaves(data, path, "", plan);
	return plan;
}

// The spans of an exactly mapped leaf. An ordinary one is one span, covered by
// the mapping and the outbound validation whole. Singularity's own instruction
// (PlannedDataLeaf::lm_instruction) writes examples of the symbols Singularity
// issues: each is machine-owned - never read as a symbol of the request's
// table, never mapped, never validated - exactly as the same words are inside
// a conversational prompt's CAPABILITY section; the words between them are
// covered as before.
std::vector<LeafSpan> exact_leaf_spans(const PlannedDataLeaf &leaf, const std::string &value) {
	std::vector<LeafSpan> spans;
	std::size_t at = 0;
	if (leaf.lm_instruction)
		for (const auto &parsed : protected_api::papinian::markers::find_all(value)) {
			if (parsed.start < at) continue;
			if (parsed.start > at) spans.push_back({at, parsed.start, false});
			spans.push_back({parsed.start, parsed.start + parsed.length, true});
			at = parsed.start + parsed.length;
		}
	if (at < value.size() || spans.empty()) spans.push_back({at, value.size(), false});
	return spans;
}

void transform_planned_leaves(nlohmann::json &data, const ProtectionPlan &plan,
		const protected_api::papinian::PapinianGuard &guard,
		const protected_api::papinian::OperationProtectedSymbolTable &frozenSymbols) {
	// Every span is masked with the same frozen table: its index of rows is
	// built once for the payload (PapinianGuard::FrozenMapper), not per span.
	const protected_api::papinian::PapinianGuard::FrozenMapper mapper = guard.frozen_mapper(frozenSymbols);
	for (const auto &leaf : plan.leaves) {
		if (leaf.policy == ProtectionPolicy::MachineOwnedPassThrough) continue;
		// A refusal names the field it happened in: fails closed, and says where.
		const auto named = [&](const std::exception &error) {
			return std::runtime_error(std::string(error.what()) + " in " + leaf.identity);
		};
		if (leaf.policy != ProtectionPolicy::SemanticDetection) {
			std::string mapped;
			try {
				for (const LeafSpan &span : exact_leaf_spans(leaf, leaf.value)) {
					const std::string text = leaf.value.substr(span.from, span.to - span.from);
					mapped += span.machine_owned ? text : mapper.apply(text);
				}
			} catch (const std::exception &error) {
				throw named(error);
			}
			data[nlohmann::json::json_pointer(leaf.identity)] = std::move(mapped);
			continue;
		}
		std::string transformed;
		transformed.reserve(leaf.value.size());
		for (const LeafSpan &span : semantic_leaf_spans(leaf.identity, leaf.value)) {
			const std::string text = leaf.value.substr(span.from, span.to - span.from);
			try {
				transformed += span.machine_owned ? text : mapper.apply(text);
			} catch (const std::exception &error) {
				throw named(error);
			}
		}
		data[nlohmann::json::json_pointer(leaf.identity)] = std::move(transformed);
	}
}

// The text of `delivered` that outbound validation must check for one planned
// leaf. A semantic leaf's machine-owned spans are skipped only when the
// delivered leaf still carries exactly the planned leaf's machine-owned spans,
// in order and byte for byte; any other shape is validated whole.
// Whether the delivered leaf still carries exactly the planned leaf's
// machine-owned spans, in order and byte for byte.
bool machine_owned_spans_hold(const PlannedDataLeaf &leaf, const std::string &delivered,
		const std::vector<LeafSpan> &outbound) {
	const std::vector<LeafSpan> planned = semantic_leaf_spans(leaf.identity, leaf.value);
	const auto machineOwnedTexts = [](const std::string &value, const std::vector<LeafSpan> &spans) {
		std::vector<std::string_view> texts;
		for (const LeafSpan &span : spans)
			if (span.machine_owned) texts.emplace_back(value.data() + span.from, span.to - span.from);
		return texts;
	};
	return machineOwnedTexts(leaf.value, planned) == machineOwnedTexts(delivered, outbound);
}

std::string validated_leaf_text(const PlannedDataLeaf &leaf, const std::string &delivered) {
	if (leaf.policy != ProtectionPolicy::SemanticDetection) {
		if (!leaf.lm_instruction) return delivered;
		std::string scored;
		for (const LeafSpan &span : exact_leaf_spans(leaf, delivered)) {
			if (span.machine_owned) continue;
			scored.append(delivered, span.from, span.to - span.from);
			scored += '\n';
		}
		return scored;
	}
	const std::vector<LeafSpan> outbound = semantic_leaf_spans(leaf.identity, delivered);
	if (!machine_owned_spans_hold(leaf, delivered, outbound)) return delivered;
	std::string scored;
	for (const LeafSpan &span : outbound) {
		if (span.machine_owned) continue;
		scored.append(delivered, span.from, span.to - span.from);
		scored += '\n';
	}
	return scored;
}

protected_api::papinian::GuardValidation validate_planned_leaves(
		const nlohmann::json &data, const ProtectionPlan &plan,
		const protected_api::papinian::PapinianGuard &guard,
		const protected_api::papinian::ProtectionResult &request) {
	protected_api::papinian::GuardValidation result;
	for (const auto &leaf : plan.leaves) {
		if (leaf.policy == ProtectionPolicy::MachineOwnedPassThrough) continue;
		const auto child = guard.validate_outbound_payload(
			validated_leaf_text(leaf, data.at(nlohmann::json::json_pointer(leaf.identity)).get<std::string>()), request);
		if (!child.masking_complete) {
			result.masking_complete = false;
			result.leaked_values.insert(result.leaked_values.end(),
				child.leaked_values.begin(), child.leaked_values.end());
			// Their rows name what kind of value each is. Their positions are
			// each its own leaf's, so a merged list is read for kinds only.
			result.occurrences.insert(result.occurrences.end(), child.occurrences.begin(), child.occurrences.end());
		}
	}
	return result;
}

// Repair before refusing. A writing of a value the frozen table holds can
// still stand in a transformed leaf where the transform did not reach it: a
// name glued to a number the transform masked ("№ 1ДС" in a history heading
// became "№ [[E11]]ДС"), a value beside a symbol it never stood beside in the
// source. Each such place is masked with its row's own symbol, span by span,
// exactly where validation reads it; the table is never changed. What is
// left is refused by the validation that follows.
struct LeafRepair {
	std::size_t masked = 0;
	// Where a writing still stands after repair, and what kind of value it is:
	// the lawyer is told in words, never by the value.
	std::set<std::string> where;
	std::set<std::string> what;
};

std::string value_noun(const std::optional<protected_api::papinian::EntityType> &type) {
	using protected_api::papinian::EntityType;
	if (!type) return "detail";
	switch (*type) {
		case EntityType::Person:
		case EntityType::Judge:
		case EntityType::Organisation:
		case EntityType::LegalEntity:
		case EntityType::Court:
			return "name";
		case EntityType::MoneyAmount:
			return "amount";
		case EntityType::Date:
			return "date";
		default:
			return "detail";
	}
}

// Which part of a request a span of a leaf is in, in the lawyer's words.
std::string span_place(const std::string &identity, const std::string &value, std::size_t from) {
	if (identity != "/prompt") return "the request";
	std::string section;
	for (std::size_t marker = value.find("<<<LM_SECTION:"); marker != std::string::npos && marker <= from;
			marker = value.find("<<<LM_SECTION:", marker + 1)) {
		const std::size_t close = value.find(">>>", marker);
		if (close != std::string::npos) section = value.substr(marker + 14, close - marker - 14);
	}
	const std::size_t requestEnd = value.find("<<<LM_END_SECTION:CURRENT_REQUEST>>>");
	if (requestEnd != std::string::npos && from > requestEnd) return "Singularity's follow-up to the model";
	if (section == "HISTORY" || section == "CONVERSATION_STATE") return "an earlier answer of this chat";
	if (section == "SOURCE_MATERIAL" || section == "PROJECT_CONTEXT") return "the Project's documents";
	return "the request";
}

// A place an amount or a date still stands is attributed to the fact it is
// (PapinianGuard::mask_leak_occurrences with the table): the fact its role
// names, the one fact of its value, or a fact of its own met there - which the
// operation's table then holds, and the request's mapping with it.
std::string repaired_text(const std::string &text, const protected_api::papinian::PapinianGuard &guard,
		protected_api::papinian::ProtectionResult &request, protected_api::papinian::OperationProtectedSymbolTable &symbols,
		LeafRepair &repair, const std::string &place) {
	const auto validation = guard.validate_outbound_payload(text, request);
	if (validation.masking_complete) return text;
	const std::string repaired = guard.mask_leak_occurrences(text, validation, symbols);
	if (symbols.mapping.size() != request.mapping.size()) request.mapping = symbols.mapping;
	const auto after = guard.validate_outbound_payload(repaired, request);
	for (const auto &occurrence : validation.occurrences)
		if (occurrence.row < request.mapping.size()) ++repair.masked;
	if (!after.masking_complete) {
		repair.where.insert(place);
		for (const auto &occurrence : after.occurrences)
			repair.what.insert(occurrence.row < request.mapping.size()
				? value_noun(request.mapping[occurrence.row].type) : std::string("detail"));
	}
	return repaired;
}

LeafRepair repair_planned_leaves(nlohmann::json &data, const ProtectionPlan &plan,
		const protected_api::papinian::PapinianGuard &guard,
		protected_api::papinian::ProtectionResult &request, protected_api::papinian::OperationProtectedSymbolTable &symbols) {
	LeafRepair repair;
	for (const auto &leaf : plan.leaves) {
		if (leaf.policy == ProtectionPolicy::MachineOwnedPassThrough) continue;
		const nlohmann::json::json_pointer pointer(leaf.identity);
		const std::string delivered = data.at(pointer).get<std::string>();
		std::string repaired;
		// Spans exactly as validation reads them (validated_leaf_text): the
		// machine-owned ones are left alone only while they are the planned ones.
		const std::vector<LeafSpan> outbound = leaf.policy == ProtectionPolicy::SemanticDetection
			? semantic_leaf_spans(leaf.identity, delivered) : std::vector<LeafSpan>{};
		const bool bySpan = leaf.policy == ProtectionPolicy::SemanticDetection
			&& machine_owned_spans_hold(leaf, delivered, outbound);
		if (leaf.lm_instruction) {
			for (const LeafSpan &span : exact_leaf_spans(leaf, delivered)) {
				const std::string text = delivered.substr(span.from, span.to - span.from);
				repaired += span.machine_owned ? text
					: repaired_text(text, guard, request, symbols, repair, span_place(leaf.identity, delivered, span.from));
			}
		} else if (!bySpan) {
			repaired = repaired_text(delivered, guard, request, symbols, repair, span_place(leaf.identity, delivered, 0));
		} else {
			for (const LeafSpan &span : outbound) {
				const std::string text = delivered.substr(span.from, span.to - span.from);
				repaired += span.machine_owned ? text
					: repaired_text(text, guard, request, symbols, repair, span_place(leaf.identity, delivered, span.from));
			}
		}
		if (repaired != delivered) data[pointer] = repaired;
	}
	return repair;
}

namespace semantic = protected_api::papinian::semantic;

// What a value is counted in, as the legend says it: a currency, what a rate
// is per, the unit of a period. Never the value.
std::string legend_unit(const semantic::Value &value) {
	const auto per = [](semantic::RateBasis basis) -> std::string {
		switch (basis) {
			case semantic::RateBasis::PerDay: return " per day";
			case semantic::RateBasis::PerMonth: return " per month";
			case semantic::RateBasis::PerYear: return " per year";
			case semantic::RateBasis::None: return "";
		}
		return "";
	};
	switch (value.kind) {
		case semantic::Kind::Money: return value.currency.empty() ? std::string() : value.currency + per(value.basis);
		case semantic::Kind::Percentage: return "percent" + per(value.basis);
		case semantic::Kind::Duration:
			switch (value.unit) {
				case semantic::DurationUnit::Day: return "days";
				case semantic::DurationUnit::Week: return "weeks";
				case semantic::DurationUnit::Month: return "months";
				case semantic::DurationUnit::Year: return "years";
				case semantic::DurationUnit::BusinessDay: return "business days";
				case semantic::DurationUnit::Quarter: return "quarters";
				case semantic::DurationUnit::Hour: return "hours";
				case semantic::DurationUnit::Minute: return "minutes";
			}
			return "";
		default: return "";
	}
}

const char *precision_word(context::temporal::Precision precision) {
	switch (precision) {
		case context::temporal::Precision::Day: return "day";
		case context::temporal::Precision::Month: return "month";
		case context::temporal::Precision::Quarter: return "quarter";
		case context::temporal::Precision::Year: return "year";
	}
	return "day";
}

// How a request's protected dates stand to one another, as intervals of days
// (SemanticAlgebra's relations): each date against the next one it wholly
// precedes, each date inside the smallest period that holds it, and dates
// beside each other that are the same day or overlap. Short - a chain, not
// every pair - and true: "July 2026" is neither before nor after "July 25,
// 2026", it contains it. A relation between symbols; a date never leaves.
// Each line with the two symbols it relates and how: "before" (the first
// wholly before the second), "same" (the same day, or the same period),
// "contains", "overlaps", "day_of_year" - so it is charged like any relation.
struct TimelineLine {
	std::string text;
	std::string first;
	std::string second;
	std::string relation;
};

std::vector<TimelineLine> date_timeline(const std::vector<std::pair<std::string, context::temporal::TemporalValue>> &unsorted) {
	std::vector<std::pair<std::string, context::temporal::TemporalValue>> dates = unsorted;
	std::sort(dates.begin(), dates.end(), [](const auto &left, const auto &right) {
		if (left.second.first_day != right.second.first_day) return left.second.first_day < right.second.first_day;
		if (left.second.last_day != right.second.last_day) return left.second.last_day > right.second.last_day;
		return left.first < right.first;
	});
	std::vector<TimelineLine> timeline;
	std::set<std::string> said;
	const auto say = [&](const std::string &line, const std::string &first, const std::string &second, const char *relation) {
		if (said.insert(line).second) timeline.push_back({line, first, second, relation});
	};
	for (std::size_t index = 0; index < dates.size(); ++index) {
		const auto &[symbol, date] = dates[index];
		// The smallest period that holds this date.
		std::optional<std::size_t> container;
		for (std::size_t other = 0; other < dates.size(); ++other) {
			if (other == index || semantic::relate_dates(dates[other].second, date) != semantic::DateRelation::Contains) continue;
			const long long span = dates[other].second.last_day - dates[other].second.first_day;
			if (!container || span < dates[*container].second.last_day - dates[*container].second.first_day) container = other;
		}
		if (container)
			say(dates[*container].first + " (a " + precision_word(dates[*container].second.precision) + ") contains " + symbol,
				dates[*container].first, symbol, "contains");
		for (std::size_t next = index + 1; next < dates.size(); ++next) {
			if (semantic::relate_dates(date, dates[next].second) != semantic::DateRelation::Before) continue;
			say(symbol + " before " + dates[next].first, symbol, dates[next].first, "before");
			break;
		}
		if (index + 1 < dates.size()) {
			const std::string &next = dates[index + 1].first;
			const auto relation = semantic::relate_dates(date, dates[index + 1].second);
			if (relation == semantic::DateRelation::SameDay) say(symbol + " same day as " + next, symbol, next, "same");
			else if (relation == semantic::DateRelation::SamePeriod) say(symbol + " same period as " + next, symbol, next, "same");
			else if (relation == semantic::DateRelation::Overlaps) say(symbol + " overlaps " + next, symbol, next, "overlaps");
		}
	}
	return timeline;
}

// What the relations hook (ProtectedRelations' planner) gives the legend and the
// outbound record of one call: the lines sent, the date symbols the legend's
// timeline may order (nullopt: every date, as when nothing was planned), and
// the local provenance of every relation sent or held back. Records never
// leave this device.
struct PlannedRelations {
	std::vector<std::string> lines;
	std::optional<std::set<std::string>> timeline_markers;
	std::vector<protected_api::papinian::OutboundRelation> records;
	std::vector<protected_api::papinian::OutboundRelation> withheld;
	// Whether a timeline line may be sent: charged to the observation ledger
	// as a preload (the caller's; none sends every line, as a test's legend).
	std::function<bool(const TimelineLine &)> admit_timeline;
};

// Whether the legend writes consecutive symbols of one kind, unit and
// precision as one entry ("[[Amount3]]–[[Amount17]]"): a ledger of equal rents
// is many facts, and each written out costs tokens without telling the model
// more. Kept switchable for the token measurement (DESIGN3 9.3.6).
inline constexpr bool kCompactLegend = true;

// Whether protectionMetadata.symbols is written as the compact typed strings
// of DESIGN4 §5.4 ("[[Amount3]] MONEY(USD/month) PRICE \"Base Rent\" · DOC_1
// §4.1") in place of one JSON object per symbol. Off until the integration
// step turns the typed legend on (DESIGN4 §13.2 step 4); until then the roles
// ride beside the objects, in protectionMetadata.roles.
inline constexpr bool kTypedLegendStrings = false;

// What the legend says about each symbol beyond its type (DESIGN4 §5, §6.4):
// its role in the material's own words, the tables the evidence touched and
// the value-free cross-source pairs - each string already masked through the
// operation's frozen mapping and kept only when the words rule holds.
struct RoleLegend {
	std::map<std::string, protected_api::papinian::roles::RoleLabel> roles;
	// A label's words as they may be sent, or "" when they may not.
	std::function<std::string(const std::string &)> label;
	// Whether a role line that ties a fact to a party ("Payment to [[Org4]]") may
	// leave under the relation policy (DESIGN4 §5.5); refused, the line keeps
	// its class alone.
	std::function<bool(const protected_api::papinian::RequestMappingEntry &, const std::string &)> permitted;
	// A role's location as it may be sent ("DOC_2 B12", "DOC_1 §[[Section3]]"),
	// or "" when it may not.
	std::function<std::string(const std::string &)> location;
	// Whether a whole legend string is free of every protected value's writing:
	// the last gate before any roles, tables or crossChecks line leaves.
	std::function<bool(const std::string &)> clean;
	// Symbols the payload writes as a figure and its words: "[[Amount1]]
	// ([[Amount1]])" - an English amount's words are a writing of the same fact.
	std::set<std::string> written_in_words;
	std::vector<std::string> tables;
	// The business-day calendars the material states (DESIGN4 §7.3), closed
	// words only.
	std::vector<std::string> calendars;
	// What the task already worked out (DESIGN4 §4.5): each result line as the
	// model was answered, most recent first - symbols, types, names and
	// derivations, no value.
	std::vector<std::string> derived;
	std::vector<std::string> cross_checks;
	protected_api::papinian::semantic::Language language = protected_api::papinian::semantic::Language::English;
};

// `isConversation`: a colleague's talk (ModelVisibleTurn::is_conversation,
// D-130). Its instruction says what a symbol is in a conversation's few words,
// and adds each of the full instruction's sentences for what its symbols hold
// - units and dates, ranges, days of the year, relations, figures in words,
// roles - but never the push to write a name wherever an answer can: in a
// conversation that made the colleague greet the person by name at every
// message (0 of 6 with this instruction and the talk's framing, 6 of 6 with the
// full one, 2026-10-26). Every other call's instruction is the full one,
// unchanged.
nlohmann::json protection_legend(
		const protected_api::papinian::ProtectionResult &protection,
		const PlannedRelations &planned = {}, const RoleLegend *roleLegend = nullptr, bool isConversation = false) {
	namespace markers = protected_api::papinian::markers;
	const std::vector<std::string> &relationLines = planned.lines;
	struct Entry {
		nlohmann::json body;
		std::string letters;
		std::size_t number = 0;
		std::string group;
	};
	std::vector<Entry> built;
	std::set<std::string> includedSymbols;
	std::vector<std::pair<std::string, context::temporal::TemporalValue>> dates;
	std::vector<std::pair<std::string, context::temporal::DayOfYear>> daysOfYear;
	bool described = false;
	// What a conversation's instruction needs to say: a value counted or dated,
	// a figure written in words, a name in parts, a range of symbols.
	bool hasValues = false;
	bool hasWords = false;
	bool hasParts = false;
	bool hasRange = false;
	// How this table writes its symbols: the examples the instruction gives are
	// written the same way.
	bool typed = false;
	for (const auto &entry : protection.mapping) {
		if (!includedSymbols.insert(entry.placeholder).second) continue;
		const std::string category = entry.type ? protected_api::papinian::to_string(*entry.type)
			: (entry.source == protected_api::papinian::DetectionSource::CustomTerm
				? "CUSTOM_TERM" : "PROTECTED_VALUE");
		nlohmann::json symbol{{"symbol", entry.placeholder}, {"category", category}};
		if (entry.value) {
			const std::string unit = legend_unit(*entry.value);
			if (!unit.empty()) symbol["unit"] = unit;
			if (entry.value->kind == semantic::Kind::Date) {
				symbol["precision"] = precision_word(entry.value->date.precision);
				dates.emplace_back(entry.placeholder, entry.value->date);
			} else if (entry.value->kind == semantic::Kind::DayOfYear) {
				// "July 1" of "each Lease Year beginning on July 1": a day that comes
				// every year, which no year places in time.
				symbol["precision"] = "day of the year (no year)";
				daysOfYear.emplace_back(entry.placeholder, entry.value->day_of_year);
			}
			described = described || !unit.empty() || entry.value->kind == semantic::Kind::Date
				|| entry.value->kind == semantic::Kind::DayOfYear;
			hasValues = hasValues || !unit.empty() || entry.value->kind == semantic::Kind::Date
				|| entry.value->kind == semantic::Kind::DayOfYear;
			// How the material writes this figure when it writes it with its words
			// ("828 000 (восемьсот двадцать восемь тысяч) рублей"): a figure put in
			// its place is asked for in the same form ([[Amount9.digitsandwords]]).
			const std::size_t open = entry.original.find(" (");
			if ((open != std::string::npos
					&& protected_api::papinian::restoration_grammar::spelling_after(entry.original, open, *entry.value))
					|| (roleLegend != nullptr && roleLegend->written_in_words.count(entry.placeholder) > 0)) {
				symbol["written"] = "digits and words";
				hasWords = true;
			}
		}
		bool parted = false;
		if (const auto parts = protected_api::papinian::person_name_parts(entry)) {
			const std::string stem = entry.placeholder.substr(0, entry.placeholder.size() - 2);
			nlohmann::json named = nlohmann::json::array();
			if (!parts->first.empty()) named.push_back(stem + ".first]]");
			if (!parts->last.empty()) named.push_back(stem + ".last]]");
			if (!parts->patronymic.empty()) named.push_back(stem + ".patronymic]]");
			if (!named.empty()) {
				symbol["parts"] = std::move(named);
				described = true;
				parted = true;
				hasParts = true;
			}
		} else if (protected_api::papinian::organisation_short_name(entry)) {
			// "Meadowbrook" of Meadowbrook Industrial Partners LLC, written alone.
			const std::string stem = entry.placeholder.substr(0, entry.placeholder.size() - 2);
			symbol["parts"] = nlohmann::json::array({stem + ".short]]"});
			described = true;
			parted = true;
			hasParts = true;
		}
		Entry item;
		const std::vector<markers::Parsed> parsed = markers::find_all(entry.placeholder);
		if (parsed.size() == 1 && parsed.front().canonical) {
			for (const char character : parsed.front().base) {
				if (markers::is_ascii_letter(character)) item.letters += character;
				else item.number = item.number * 10 + static_cast<std::size_t>(character - '0');
			}
		}
		typed = typed || item.letters.size() > 1;
		if (!parted && item.letters.size() > 1)
			item.group = item.letters + "|" + category + "|" + symbol.value("unit", "") + "|" + symbol.value("precision", "") + "|"
				+ symbol.value("written", "");
		item.body = std::move(symbol);
		built.push_back(std::move(item));
	}
	// Consecutive symbols of one kind, unit and precision: one entry.
	nlohmann::json entries = nlohmann::json::array();
	if (kCompactLegend && typed) {
		std::stable_sort(built.begin(), built.end(), [](const Entry &left, const Entry &right) {
			if (left.letters != right.letters) return left.letters < right.letters;
			return left.number < right.number;
		});
		for (std::size_t index = 0; index < built.size();) {
			std::size_t last = index;
			while (!built[index].group.empty() && last + 1 < built.size() && built[last + 1].group == built[index].group
					&& built[last + 1].number == built[last].number + 1)
				++last;
			if (last == index) {
				entries.push_back(std::move(built[index].body));
			} else {
				hasRange = true;
				nlohmann::json run = built[index].body;
				run.erase("symbol");
				nlohmann::json grouped{{"symbols", built[index].body.at("symbol").get<std::string>() + "\xE2\x80\x93"
					+ built[last].body.at("symbol").get<std::string>()}};
				for (auto field = run.begin(); field != run.end(); ++field) grouped[field.key()] = field.value();
				entries.push_back(std::move(grouped));
			}
			index = last + 1;
		}
	} else {
		for (Entry &item : built) entries.push_back(std::move(item.body));
	}
	const std::string person = typed ? "[[Person1" : "[[E5";
	const std::string organisation = typed ? "[[Org1" : "[[E3";
	// A model that reads a symbol as a gap refuses work it could do: asked to
	// change "[[E14]]" to "[[E15]]" in a clause, one live run answered that the
	// values it needed were masked. The symbol IS the value, as far as the
	// model is concerned, and it is told so. A model that reads a symbol as a
	// secret leaves the value out instead: a live Golden run (25 Sep 2026) set
	// out "Earlier draft / Signed agreement / Board position" with no date,
	// though [[E15]] would have come back March 3, 2026.
	// Each sentence the instruction may say, said the same way wherever it is.
	const std::string rangeSentence = " An entry for a range of symbols (\"[[Amount3]]\xE2\x80\x93[[Amount6]]\") describes "
		"each symbol of that range.";
	const std::string valueSentence = " A symbol's unit is what it is counted in (a currency, or what a rate is per) and a "
		"DATE's precision whether it names a day, a month, a quarter or a year; the timeline states how this request's "
		"dates stand to one another, and a date inside a longer period is neither before nor after it.";
	const std::string partsSentence = " A PERSON's parts write one part of that name alone (" + person + ".first]], " + person
		+ ".last]]), and Singularity writes each in the right form; an organisation's " + organisation + ".short]] is the "
		"same organisation written by its short name, as the documents write it alone.";
	const std::string dayOfYearSentence = " A DATE whose precision is \"day of the year (no year)\" comes every year and is "
		"before or after nothing by itself; the timeline says which full dates fall on it, and Singularity works out "
		"{{= same_day_of_year(a, b)}} and {{= next_occurrence(day, date)}} (the first such day on or after a date).";
	// What Singularity worked out from the real values before the request left
	// (PapinianGuard/ProtectedRelations): a model that has to ask for each of
	// these in a call of its own asked for none of them 18 times in 38 and
	// hedged instead (ab4/ab2, 25 Sep 2026).
	const std::string relationsSentence = " relations states what Singularity worked out on the lawyer's computer from the "
		"real values: totals and balances of tables, stepped amounts, days and business days between dates, deadlines a "
		"rule sets, and whose contact details are whose. Each value it worked out is a symbol of its own, restored like "
		"any other: rely on these lines rather than guessing, write their symbols in calculations, chronologies and "
		"letters, and never call a figure unknown when a relation gives it. A symbol is the whole value with its unit - a "
		"duration already says days or business days, an amount its currency - so never write a unit, currency or scale "
		"word right after one.";
	// Amounts and dates in words (owner, 26 Sep 2026): the model never sees a
	// value, so it asks for the form and Singularity writes the words from the value.
	const std::string amount = typed ? "[[Amount3" : "[[E7";
	const std::string date = typed ? "[[Date4" : "[[E8";
	const std::string wordsSentence = " Where the answer or a document writes a figure or a date in words as well as in "
		"figures (a symbol whose entry says written: digits and words is so written in the material), write " + amount
		+ ".digitsandwords]] - the figure, then its words in brackets, with the currency's words - or " + amount
		+ ".words]] for the words alone (" + date + ".words]] for a date written in words); Singularity writes the words from "
		"the real value, in the answer's language and grammar. Never write a figure's words yourself.";
	const std::string rolesSentence = " roles says what a symbol is in the lawyer's material: its type, its role class "
		"(LIMIT, PRICE, BALANCE, DEADLINE ...) and the material's own words for it, with where they stand; tables "
		"describes the spreadsheet tables the evidence touches, and a cell is addressed by reference ([[Table1]].B16).";
	const bool hasRoles = roleLegend != nullptr && !roleLegend->roles.empty();
	std::string instruction;
	if (isConversation == false) {
		instruction = "Preserve every protected symbol exactly as written in response data. Each symbol "
			"stands for a real value in the lawyer's material, which Singularity puts back into your answer before the lawyer "
			"reads it: to quote, compare or change text that contains one, write the symbol itself. A symbol is never "
			"missing information. Write one wherever the answer states its value - each event's date in a chronology, a "
			"party's name, an amount, a section or document reference - as you would write the value itself; never leave a "
			"value out because it is protected, and never write one the material does not give.";
		// Owner correction (26 Sep 2026): an amount's or a date's symbol names a
		// fact, not a value - four $5m facts are four symbols.
		instruction += " A symbol stands for one fact of the material: where it is stated and what it is, with its value. "
			"Two facts of equal value are two symbols. Singularity tells you when values are equal if your task needs it "
			"(relations), or when you ask (compare).";
		if (kCompactLegend && typed) instruction += rangeSentence;
		if (described) instruction += valueSentence + partsSentence;
		if (!daysOfYear.empty()) instruction += dayOfYearSentence;
		if (!relationLines.empty()) instruction += relationsSentence;
		if (described) instruction += wordsSentence;
		if (hasRoles) instruction += rolesSentence;
	} else {
		// A colleague's talk: what a symbol is, in few words, and each sentence its
		// symbols need - never the push to name a party wherever it can.
		instruction = "Each [[symbol]] stands for a real name or value that Singularity puts back before your colleague "
			"reads your answer: write a symbol exactly as given wherever you mean that name or value, never a guess at it.";
		if (hasRange) instruction += rangeSentence;
		if (hasValues) instruction += valueSentence;
		if (hasParts)
			instruction += " A PERSON's " + person + ".first]] or " + person + ".last]] writes one part of the name, and an "
				"organisation's " + organisation + ".short]] its short name.";
		if (!daysOfYear.empty()) instruction += dayOfYearSentence;
		if (!relationLines.empty()) instruction += relationsSentence;
		if (hasValues || hasWords) instruction += wordsSentence;
		if (hasRoles) instruction += rolesSentence;
	}
	nlohmann::json legend{{"instruction", std::move(instruction)}, {"symbols", std::move(entries)}};
	if (!relationLines.empty()) legend["relations"] = relationLines;
	// Roles (DESIGN4 §5.4): one line per symbol this payload carries, in the
	// material's own words where the words rule lets them go, else the class
	// alone; tables and cross-source pairs after them.
	if (roleLegend != nullptr) {
		namespace roles = protected_api::papinian::roles;
		nlohmann::json roleLines = nlohmann::json::array();
		std::set<std::string> listed;
		for (const auto &entry : protection.mapping) {
			if (!listed.insert(entry.placeholder).second) continue;
			const auto role = entry.fact_key.empty() ? roleLegend->roles.end() : roleLegend->roles.find(entry.fact_key);
			if (role == roleLegend->roles.end()) {
				if (kTypedLegendStrings) roleLines.push_back(entry.placeholder + " " + roles::type_of_row(entry));
				continue;
			}
			std::string words = role->second.words.empty() || !roleLegend->label ? std::string()
				: roleLegend->label(role->second.words);
			if (!words.empty() && roleLegend->permitted && !roleLegend->permitted(entry, words)) words.clear();
			const bool request = role->second.source == roles::RoleLabel::Source::Request;
			if (words.empty() && role->second.role_class.empty() && !request) {
				if (kTypedLegendStrings) roleLines.push_back(entry.placeholder + " " + roles::type_of_row(entry));
				continue;
			}
			const std::string place = words.empty() || role->second.location.empty() || !roleLegend->location ? std::string()
				: roleLegend->location(role->second.location);
			const std::string type = roles::type_of_row(entry, role->second);
			std::string line = roles::symbol_line(entry.placeholder, type, role->second.role_class, words, place, request,
				roleLegend->language);
			// Never sent unchecked: a line that fails falls back to its class alone.
			if (roleLegend->clean && !roleLegend->clean(line)) {
				line = roles::symbol_line(entry.placeholder, type, role->second.role_class, std::string(), std::string(), request,
					roleLegend->language);
				if (!roleLegend->clean(line)) continue;
			}
			roleLines.push_back(std::move(line));
		}
		if (!roleLines.empty()) {
			if (kTypedLegendStrings) legend["symbols"] = roleLines;
			else legend["roles"] = std::move(roleLines);
		}
		const auto checked = [&](const std::vector<std::string> &lines) {
			nlohmann::json kept = nlohmann::json::array();
			for (const std::string &line : lines)
				if (!roleLegend->clean || roleLegend->clean(line)) kept.push_back(line);
			return kept;
		};
		if (!roleLegend->tables.empty()) {
			nlohmann::json tables = checked(roleLegend->tables);
			if (!tables.empty()) legend["tables"] = std::move(tables);
		}
		if (!roleLegend->calendars.empty()) {
			nlohmann::json calendars = checked(roleLegend->calendars);
			if (!calendars.empty()) legend["calendars"] = std::move(calendars);
		}
		// Beside the symbols: what Singularity already worked out for this task, so a
		// review reads each figure with how it was worked out.
		if (!roleLegend->derived.empty()) {
			nlohmann::json derived = checked(roleLegend->derived);
			if (!derived.empty()) {
				if (legend.contains("symbols") && legend.at("symbols").is_array())
					for (const nlohmann::json &line : derived) legend["symbols"].push_back(line);
				else
					legend["workedOut"] = std::move(derived);
			}
		}
		if (!roleLegend->cross_checks.empty()) {
			nlohmann::json crossChecks = checked(roleLegend->cross_checks);
			if (crossChecks.size() > 1) legend["crossChecks"] = std::move(crossChecks);
		}
	}
	// The timeline is a relation family the planner governs: only the dates it
	// names are ordered when it has planned the request's relations.
	if (planned.timeline_markers) {
		dates.erase(std::remove_if(dates.begin(), dates.end(), [&](const auto &date) {
			return planned.timeline_markers->count(date.first) == 0;
		}), dates.end());
	}
	std::vector<TimelineLine> lines = date_timeline(dates);
	// Every full date that falls on a day of the year: the anniversary a Lease
	// Year begins on is the day the Term commenced, and the model is told so -
	// the relation, never the day.
	for (const auto &[daySymbol, day] : daysOfYear)
		for (const auto &[dateSymbol, date] : dates) {
			if (date.precision != context::temporal::Precision::Day) continue;
			const semantic::Value full = semantic::make_date(date);
			const semantic::Value recurring = semantic::make_day_of_year(day.month, day.day);
			if (semantic::same_day_of_year(recurring, full).truth == semantic::Truth::True)
				lines.push_back({daySymbol + " is the day of the year on which " + dateSymbol + " falls", daySymbol, dateSymbol,
					"day_of_year"});
		}
	// Each line a relation of two protected dates: sent only as the ledger
	// admits it (a preload, after what the task's operations may need).
	nlohmann::json timeline = nlohmann::json::array();
	for (const TimelineLine &line : lines)
		if (!planned.admit_timeline || planned.admit_timeline(line)) timeline.push_back(line.text);
	if (!timeline.empty()) legend["timeline"] = std::move(timeline);
	return legend;
}

// The text between a PromptAssembler section's own lines, or nothing.
std::string prompt_section(const std::string &prompt, std::string_view name);

// The roles, tables and cross-source pairs one protected call's legend carries
// (DESIGN4 §5, §6.4, §5.7; lane R). Roles are read from the RAW passages the
// facts were met in (never from a value) and kept only for symbols the
// payload carries; every material string is masked through the operation's
// frozen mapping and sent only when each of its words already stands in the
// call's protected source-material spans (the words rule) and the leak check
// finds nothing in it. Anything else is sent as the role class alone, or not.
RoleLegend role_legend(const nlohmann::json &protectedData, const std::vector<protected_api::papinian::SemanticLeaf> &leaves,
		const std::vector<std::pair<std::string, std::string>> &passagesOfDocuments,
		const protected_api::papinian::PapinianGuard &guard,
		const protected_api::papinian::OperationProtectedSymbolTable &symbols,
		const protected_api::papinian::ProtectionResult &request, const std::string &outboundPrompt,
		const protected_api::Policy &policy) {
	namespace roles = protected_api::papinian::roles;
	RoleLegend legend;
	legend.permitted = [&symbols, policy](const protected_api::papinian::RequestMappingEntry &fact, const std::string &words) {
		const auto category = [](const protected_api::papinian::RequestMappingEntry &row) {
			return row.type ? std::string(protected_api::papinian::to_string(*row.type)) : std::string("PROTECTED_VALUE");
		};
		protected_api::RelationFacts facts;
		facts.kind = "role";
		facts.operand_categories.push_back(category(fact));
		facts.operand_parties.push_back(std::string());
		bool party = false;
		for (const protected_api::papinian::markers::Parsed &parsed : protected_api::papinian::markers::find_all(words)) {
			const std::string symbol = "[[" + parsed.base + "]]";
			for (const auto &row : symbols.mapping) {
				if (row.placeholder != symbol || row.value) continue;
				facts.operand_categories.push_back(category(row));
				facts.operand_parties.push_back(symbol);
				party = true;
			}
		}
		return !party || protected_api::permit_relation(facts, policy);
	};
	std::map<std::string, std::string> documentOfSha;
	for (const auto &[identity, sha] : passagesOfDocuments) {
		const std::size_t bar = identity.rfind('|');
		if (bar != std::string::npos) documentOfSha.emplace(sha, identity.substr(bar + 1));
	}
	std::vector<context::business_calendar::RulePassage> passages;
	std::string material;
	for (const auto &leaf : leaves) {
		if (leaf.model_authored) continue;
		context::business_calendar::RulePassage passage;
		passage.text = leaf.text;
		passage.sha256 = platform::sha256_hex_digest(leaf.text);
		const auto document = documentOfSha.find(passage.sha256);
		passage.document = document == documentOfSha.end() ? std::string(leaf.user_authored ? "request" : "material")
			: document->second;
		material += leaf.text;
		// Each SOURCE block is a passage of its own document: the digest a fact
		// met inside one is filed under (the guard's place of a fact).
		const std::string &text = leaf.text;
		for (std::size_t marker = text.find("[SOURCE:DOC_"); marker != std::string::npos;
				marker = text.find("[SOURCE:DOC_", marker + 1)) {
			if (marker > 0 && text[marker - 1] != '\n') continue;
			const std::size_t lineEnd = text.find('\n', marker);
			if (lineEnd == std::string::npos || text[lineEnd - 1] != ']') continue;
			const std::size_t closing = text.find("\n[/SOURCE]", lineEnd);
			if (closing == std::string::npos) continue;
			const std::size_t colon = text.find(':', marker + 8);
			context::business_calendar::RulePassage block;
			block.text = text.substr(lineEnd + 1, closing - lineEnd - 1);
			block.sha256 = platform::sha256_hex_digest(block.text);
			block.document = colon == std::string::npos || colon > lineEnd ? std::string("material")
				: text.substr(marker + 8, colon - marker - 8);
			passages.push_back(std::move(block));
		}
		passages.push_back(std::move(passage));
	}
	legend.language = semantic::language_of(material);
	// Only the facts this payload carries.
	std::vector<protected_api::papinian::RequestMappingEntry> carried;
	for (const auto &entry : request.mapping)
		if (outboundPrompt.find(entry.placeholder) != std::string::npos) {
			carried.push_back(entry);
			if (entry.value && outboundPrompt.find(entry.placeholder + " (" + entry.placeholder + ")") != std::string::npos)
				legend.written_in_words.insert(entry.placeholder);
		}
	legend.roles = roles::assign_roles(passages, carried, symbols.tables, legend.language);
	// The words rule reads the protected source material, the request and the
	// Project's records - never Singularity's own blocks, never a model's text.
	std::vector<std::string> spans;
	const auto prompt = protectedData.find("prompt");
	if (prompt != protectedData.end() && prompt->is_string())
		for (const char *section : {"SOURCE_MATERIAL", "CURRENT_REQUEST", "PROJECT_CONTEXT"}) {
			std::string text = prompt_section(prompt->get_ref<const std::string &>(), section);
			if (!text.empty()) spans.push_back(std::move(text));
		}
	auto cache = std::make_shared<std::map<std::string, std::string>>();
	// Every label and location is masked with the same frozen table: its index
	// of rows is built once for them all (PapinianGuard::FrozenMapper), not for
	// each of hundreds of short texts, and again only if rows were added since.
	struct LegendMapper {
		std::optional<protected_api::papinian::PapinianGuard::FrozenMapper> mapper;
		std::size_t rows = 0;
	};
	const auto legendMapper = std::make_shared<LegendMapper>();
	const auto masked = [&guard, &symbols, legendMapper](const std::string &text) {
		if (!legendMapper->mapper || legendMapper->rows != symbols.mapping.size()) {
			legendMapper->mapper.emplace(guard.frozen_mapper(symbols));
			legendMapper->rows = symbols.mapping.size();
		}
		return legendMapper->mapper->apply(text);
	};
	// The spans' words, folded once for every label the legend asks about.
	const auto spanWords = std::make_shared<std::optional<std::unordered_set<std::string>>>();
	legend.label = [&guard, &request, spans, spanWords, cache, masked](const std::string &words) -> std::string {
		const auto known = cache->find(words);
		if (known != cache->end()) return known->second;
		std::string sent;
		try {
			const std::string masked_words = masked(words);
			if (!*spanWords) *spanWords = roles::spans_words(spans);
			if (roles::sendable_in_words(masked_words, **spanWords) && guard.validate_outbound_payload(masked_words, request).masking_complete)
				sent = masked_words;
		} catch (const std::exception &) {
			sent.clear();
		}
		cache->emplace(words, sent);
		return sent;
	};
	legend.clean = [&guard, &request](const std::string &line) {
		try {
			return guard.validate_outbound_payload(line, request).masking_complete;
		} catch (const std::exception &) {
			return false;
		}
	};
	// A location is Singularity's own document handle and a bare reference; a
	// clause number goes through the frozen mapping (a section reference is a
	// protected value of its own). Anything else is not sent.
	legend.location = [masked, clean = legend.clean](const std::string &location) -> std::string {
		const std::size_t space = location.find(' ');
		const std::string document = location.substr(0, space);
		if (document.size() <= 4 || document.rfind("DOC_", 0) != 0
				|| document.find_first_not_of("0123456789", 4) != std::string::npos)
			return {};
		if (space == std::string::npos) return document;
		std::string within = location.substr(space + 1);
		const std::string section = "\xC2\xA7";
		if (within.rfind(section, 0) == 0) {
			try {
				within = section + masked(within.substr(section.size()));
			} catch (const std::exception &) {
				return document;
			}
		} else if (within.empty() || within.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:") != std::string::npos) {
			return document;
		}
		const std::string sent = document + " " + within;
		return clean(sent) ? sent : document;
	};
	legend.tables = roles::table_block(symbols.tables, legend.label);
	// Band schedules from prose (DESIGN4 §6.6), listed with the tables: each
	// threshold and band value as its symbol, or as the figure the material
	// writes openly; a schedule that would write an amount or a date is not
	// listed at all.
	for (const protected_api::papinian::tables::ProtectedSchedule &schedule : symbols.schedules) {
		namespace semantic = protected_api::papinian::semantic;
		bool listable = true;
		const auto written = [&](const std::string &symbol, const std::optional<semantic::Value> &value) -> std::string {
			if (symbol.rfind("[[", 0) == 0) return symbol;
			if (!value || value->kind == semantic::Kind::Money || value->kind == semantic::Kind::Date
					|| value->kind == semantic::Kind::DayOfYear) {
				listable = false;
				return std::string();
			}
			// A figure protection masks anywhere in this operation is never
			// written out: it is the one fact of that value, by its symbol, or
			// the schedule is not listed.
			// By amount, whatever kind the schedule read it as ("1,000,000" of
			// "$1,000,000" is that amount's figure).
			std::set<std::string> placeholders;
			for (const protected_api::papinian::RequestMappingEntry &row : symbols.mapping)
				if (!row.derived && row.value && row.value->amount == value->amount && row.value->kind != semantic::Kind::Boolean
						&& row.value->kind != semantic::Kind::Date && row.value->kind != semantic::Kind::DayOfYear
						&& (row.value->kind == semantic::Kind::Percentage) == (value->kind == semantic::Kind::Percentage))
					placeholders.insert(row.placeholder);
			if (placeholders.size() == 1) return *placeholders.begin();
			if (!placeholders.empty()) {
				listable = false;
				return std::string();
			}
			try {
				return semantic::display(*value, {}, legend.language);
			} catch (const std::exception &) {
				listable = false;
				return std::string();
			}
		};
		std::string line = "[[" + schedule.handle + "]]";
		const std::string title = schedule.title.empty() ? std::string() : legend.label(schedule.title);
		if (!title.empty()) line += " \"" + title + "\"";
		line += ":";
		for (std::size_t index = 0; index < schedule.bands.size(); ++index) {
			const protected_api::papinian::tables::ScheduleBand &band = schedule.bands[index];
			std::string text;
			if (!band.lower.empty())
				text += std::string(band.lower_inclusive ? "\xE2\x89\xA5 " : "> ") + written(band.lower, band.lower_value);
			if (!band.upper.empty())
				text += std::string(text.empty() ? "" : ", ") + (band.upper_inclusive ? "\xE2\x89\xA4 " : "< ")
					+ written(band.upper, band.upper_value);
			text += " \xE2\x86\x92 " + written(band.value, band.band_value);
			line += std::string(index == 0 ? " " : "; ") + text;
		}
		if (listable && !schedule.bands.empty()) legend.tables.push_back(line);
	}
	for (const protected_api::papinian::compute::CalendarHandle &calendar : symbols.calendars)
		if (!calendar.line.empty()) legend.calendars.push_back(calendar.line);
	// The task's derived facts, for a later call of the attempt (a review, a
	// redraft): how each figure it reads was worked out.
	for (auto record = symbols.op_log.rbegin(); record != symbols.op_log.rend()
			&& legend.derived.size() < protected_api::papinian::compute::kDerivedFactsListed; ++record) {
		if ((record->status != "ok" && record->status != "bounded") || record->results.empty() || record->result_line.empty())
			continue;
		std::string line = record->result_line;
		while (!line.empty() && (line.back() == '\n' || line.back() == ' ')) line.pop_back();
		// "q3 ok [[Amount5]] ..." - the op's place in its reply says nothing here.
		const std::size_t space = line.find(' ');
		const std::size_t second = space == std::string::npos ? std::string::npos : line.find(' ', space + 1);
		if (second != std::string::npos) line = line.substr(second + 1);
		for (std::size_t at = line.find("\n  "); at != std::string::npos; at = line.find("\n  ", at)) line.replace(at, 3, "; ");
		legend.derived.push_back(std::move(line));
	}
	std::map<std::string, roles::RoleLabel> sendableRoles;
	for (const auto &[key, label] : legend.roles)
		if (!label.words.empty() && !legend.label(label.words).empty()) sendableRoles.emplace(key, label);
	for (const roles::CrossCheck &check : roles::cross_checks(sendableRoles, symbols.tables, symbols)) {
		bool words = true;
		for (const std::string &label : check.labels) words = words && !legend.label(label).empty();
		if (!words) continue;
		std::string line;
		try {
			line = masked(check.line);
		} catch (const std::exception &) {
			continue;
		}
		if (!guard.validate_outbound_payload(line, request).masking_complete) continue;
		if (legend.cross_checks.empty())
			legend.cross_checks.push_back(context::control_text::render(context::control_text::Template::RoleCrossCheck));
		legend.cross_checks.push_back(line);
	}
	return legend;
}

// The text between a PromptAssembler section's own lines, or nothing.
std::string prompt_section(const std::string &prompt, std::string_view name) {
	const std::string opening = "<<<LM_SECTION:" + std::string(name) + ">>>\n";
	const std::size_t start = prompt.find(opening);
	if (start == std::string::npos) return {};
	const std::size_t from = start + opening.size();
	const std::size_t close = prompt.find("\n<<<LM_END_SECTION:" + std::string(name) + ">>>", from);
	return close == std::string::npos ? std::string() : prompt.substr(from, close - from);
}

struct RelationsReport {
	std::size_t lines = 0;
	std::size_t dropped_by_cap = 0;
	// Outcome lines the observation budget no longer covered (DESIGN4 §8.3).
	std::size_t dropped_by_budget = 0;
	std::size_t refused = 0;
	double ms = 0.0;
};

// The relations a protected conversational turn carries (Papinian round 3,
// DESIGN3 5.3): read from the PROTECTED prompt - symbols in place - and the
// operation's table, so every line is a relation between symbols. The values
// they work out are issued into the operation's table as symbols of their own.
// A line that would carry any writing of a protected value is not sent.
// The relations a protected conversational turn carries (DESIGN3 9.5): a
// query planner, not a relation dump. Every relation Singularity can work out is
// derived locally from the PROTECTED prompt (symbols in place) and the
// operation's table; only those the current task justifies are sent - they
// serve an intent of the lawyer's RAW question, relate a fact the question
// names or a passage it targets, and the relation policy permits them - each
// with its local record. The business-day rule is read from the RAW passages
// (the operative document's own definition first). A value a sent relation
// derives is issued into the table as a fact of its own. A line that would
// carry any writing of a protected value is not sent, and neither is its record.
// Nothing when the planner did not run (a prompt with no material, Project
// context or history to relate): the legend then keeps its timeline of every date.
// The planner is a preload, never a correctness dependency (DESIGN4 D5,
// §4.3, §8.3). Each value one of its lines works out is registered as a
// derived fact of the operation - the model may name it in an operation like
// any result - and each line that tells how values stand to one another (an
// order, an equality, a rate against a cell) is an observation, charged to
// the observation ledger's counts over its root facts BEFORE it is sent: a
// line the budget of any root no longer covers is dropped, never sent. Asked
// again with the same canonical key, it is free; so is an equality that
// equalities already told imply (A = B and B = C tell A = C: owner, 27 Sep
// 2026, an implied relation is free). Counts and fact keys only.
std::size_t register_planner_lines(protected_api::papinian::relations::Plan &plan,
		protected_api::papinian::OperationProtectedSymbolTable &symbols) {
	namespace compute = protected_api::papinian::compute;
	namespace roles = protected_api::papinian::roles;
	// A line states an outcome - an order, an equality, a sign - when its kind
	// always does, or when the words it carries compare (a sheet total checked
	// against its total row, a period set against a threshold). A derivation
	// alone ("sum of B rows 4-18: [[Amount9]]") is a derived fact, not an
	// observation. A calendar observation also spends its date roots' calendar
	// budget.
	static const std::set<std::string> observations{"escalated_vs_ledger", "date_order", "rule_vs_cell", "value_equality",
		"row_balance", "step_on_due_date", "next_business_day"};
	const auto statesOutcome = [](const protected_api::papinian::OutboundRelation &record, const std::string &line) {
		if (observations.count(record.kind) > 0) return true;
		if (record.kind == "column_total")
			return line.find("; equals ") != std::string::npos || line.find("; differs from ") != std::string::npos;
		if (record.kind == "days_between")
			for (const char *said : {"fewer than ", "more than ", "exactly ", "at least ", "is not determined"})
				if (line.find(said) != std::string::npos) return true;
		return false;
	};
	std::map<std::string, std::size_t> rowOf;
	for (std::size_t row = 0; row < symbols.mapping.size(); ++row) rowOf.emplace(symbols.mapping[row].placeholder, row);
	const auto rootsOf = [&](const std::string &marker) {
		std::set<std::string> roots;
		const auto derived = symbols.derived_facts.find(marker);
		if (derived != symbols.derived_facts.end() && !derived->second.roots.empty()) return derived->second.roots;
		const auto row = rowOf.find(marker);
		if (row != rowOf.end() && !symbols.mapping[row->second].fact_key.empty()) roots.insert(symbols.mapping[row->second].fact_key);
		return roots;
	};
	compute::ObservationLedger &ledger = symbols.observations;
	protected_api::papinian::relations::Plan kept = plan;
	kept.lines.clear();
	kept.records.clear();
	std::size_t dropped = 0;
	for (std::size_t index = 0; index < plan.lines.size(); ++index) {
		const protected_api::papinian::OutboundRelation *record = index < plan.records.size() ? &plan.records[index] : nullptr;
		std::set<std::string> roots;
		std::set<std::string> calendarRoots;
		std::vector<std::string> operandKeys;
		const bool calendar = record != nullptr && (record->kind == "next_business_day" || record->kind == "step_on_due_date"
			|| !record->business_day_rule.empty());
		if (record != nullptr)
			for (const std::string &operand : record->operands) {
				const std::set<std::string> each = rootsOf(operand);
				roots.insert(each.begin(), each.end());
				const auto row = rowOf.find(operand);
				if (row != rowOf.end()) {
					const protected_api::papinian::RequestMappingEntry &entry = symbols.mapping[row->second];
					operandKeys.push_back(entry.fact_key);
					if (calendar && entry.value && entry.value->kind == semantic::Kind::Date)
						calendarRoots.insert(each.begin(), each.end());
				}
			}
		if (record != nullptr && statesOutcome(*record, plan.lines[index]) && !roots.empty()) {
			// Facts told equal, as the planner chains them: what earlier
			// equalities - or any relation told - already settle is free.
			const bool equality = record->kind == "value_equality" && operandKeys.size() >= 2
				&& std::none_of(operandKeys.begin(), operandKeys.end(), [](const std::string &fact) { return fact.empty(); });
			std::vector<std::string> equalEdges;
			// (ledger.implied: one fact against itself is known.)
			bool implied = equality;
			if (equality)
				for (std::size_t at = 1; at < operandKeys.size(); ++at) {
					implied = implied && ledger.implied(operandKeys[at - 1], operandKeys[at], "equal");
					equalEdges.push_back("le|" + operandKeys[at - 1] + "|" + operandKeys[at]);
					equalEdges.push_back("le|" + operandKeys[at] + "|" + operandKeys[at - 1]);
				}
			std::vector<std::string> sortedKeys = operandKeys;
			std::sort(sortedKeys.begin(), sortedKeys.end());
			compute::ObservationCharge charge;
			charge.key = "planner|" + record->kind;
			for (const std::string &operand : sortedKeys) charge.key += "|" + operand;
			charge.roots.assign(roots.begin(), roots.end());
			charge.calendar_roots.assign(calendarRoots.begin(), calendarRoots.end());
			if (implied) ledger.matter.answered.insert(charge.key);
			const bool first = ledger.diagnostics.preload_seen.insert(charge.key).second;
			if (first) ++ledger.diagnostics.preload_proposed;
			if (ledger.answered(charge.key)) {
				if (first) ++ledger.diagnostics.preload_free;
			} else if (ledger.admit(charge, compute::DisclosurePriority::Preload) != compute::Code::None) {
				// Kept for the operations the lawyer's task may need.
				if (first) ++ledger.diagnostics.preload_skipped_reserve;
				++dropped;
				continue;
			} else {
				ledger.charge(charge, compute::DisclosurePriority::Preload);
				++ledger.diagnostics.preload_sent;
			}
			ledger.learn(equalEdges);
		}
		if (record != nullptr)
			for (const std::string &result : record->results) {
				const auto row = rowOf.find(result);
				if (row == rowOf.end() || symbols.derived_facts.count(result) > 0) continue;
				const protected_api::papinian::RequestMappingEntry &entry = symbols.mapping[row->second];
				if (!entry.value) continue;
				compute::DerivedFact fact;
				fact.placeholder = entry.placeholder;
				fact.fact_key = entry.fact_key;
				fact.op_id = "planner:" + record->id;
				fact.op = record->kind;
				fact.ast = entry.provenance.derivation;
				fact.operand_symbols = record->operands;
				fact.operand_fact_keys = operandKeys;
				fact.type = roles::type_of_row(entry);
				fact.value = *entry.value;
				fact.label = record->kind;
				fact.roots = roots;
				fact.root_sources = record->sources;
				fact.value_sensitivity = record->value_sensitivity;
				fact.relation_sensitivity = record->relation_sensitivity;
				fact.depth = 1;
				fact.disclosure = compute::DerivedFact::Disclosure::Derivation;
				symbols.derived_facts.emplace(fact.placeholder, std::move(fact));
				symbols.sent_symbols.insert(entry.placeholder);
			}
		kept.lines.push_back(plan.lines[index]);
		if (record != nullptr) kept.records.push_back(*record);
	}
	kept.dropped_by_cap = plan.dropped_by_cap;
	plan = std::move(kept);
	return dropped;
}

// The capacity a preload leaves of every root (ObservationLedger::preload_keep):
// a bound ranked among the rows of the attempt's longest table is the heaviest
// single charge an operation makes of one root, ceil(log2(N+1)) (list_rows).
void keep_for_tables(protected_api::papinian::OperationProtectedSymbolTable &symbols) {
	namespace compute = protected_api::papinian::compute;
	std::size_t rows = 0;
	for (const protected_api::papinian::tables::ProtectedTable &table : symbols.tables)
		rows = std::max<std::size_t>(rows, static_cast<std::size_t>(std::count_if(table.rows.begin(), table.rows.end(),
			[](const protected_api::papinian::tables::TableRow &row) {
				return row.kind == protected_api::papinian::tables::RowKind::Body;
			})));
	std::size_t bits = 0;
	while ((std::size_t{1} << bits) < rows + 1) ++bits;
	symbols.observations.preload_keep = std::max(compute::kObservationsPerRoot - compute::kPreloadObservationsPerRoot,
		std::min(compute::kObservationsPerRoot, bits));
}

// Whether a timeline line may leave, charged to the observation ledger as a
// preload over the two facts it relates (owner, 28 Sep 2026: a date's order is
// a disclosure like any other; the timeline is no second budget). What was
// told already implies is free, and what it tells is kept as order edges.
bool admit_timeline_line(protected_api::papinian::OperationProtectedSymbolTable &symbols, const TimelineLine &line) {
	namespace compute = protected_api::papinian::compute;
	const auto factOf = [&](const std::string &symbol) {
		std::pair<std::string, std::set<std::string>> fact;
		const auto derived = symbols.derived_facts.find(symbol);
		if (derived != symbols.derived_facts.end()) {
			fact.first = derived->second.fact_key;
			fact.second = derived->second.roots;
		} else {
			for (const protected_api::papinian::RequestMappingEntry &entry : symbols.mapping)
				if (entry.placeholder == symbol) {
					fact.first = entry.fact_key;
					break;
				}
		}
		if (fact.first.empty()) fact.first = "sym:" + symbol;
		if (fact.second.empty()) fact.second.insert(fact.first);
		return fact;
	};
	const auto [a, rootsA] = factOf(line.first);
	const auto [b, rootsB] = factOf(line.second);
	// One fact written twice tells nothing of it.
	if (a == b) return true;
	const bool ordered = line.relation != "same" && line.relation != "overlaps";
	compute::ObservationCharge charge;
	charge.key = "timeline|" + line.relation + "|" + (ordered || a < b ? a + "|" + b : b + "|" + a);
	charge.roots.assign(rootsA.begin(), rootsA.end());
	charge.roots.insert(charge.roots.end(), rootsB.begin(), rootsB.end());
	std::sort(charge.roots.begin(), charge.roots.end());
	charge.roots.erase(std::unique(charge.roots.begin(), charge.roots.end()), charge.roots.end());
	std::vector<std::string> edges;
	if (line.relation == "before") edges = {"lt|" + a + "|" + b};
	if (line.relation == "same") edges = {"le|" + a + "|" + b, "le|" + b + "|" + a};
	compute::ObservationLedger &ledger = symbols.observations;
	if ((line.relation == "before" && ledger.implied(a, b, "before")) || (line.relation == "same" && ledger.implied(a, b, "equal")))
		ledger.matter.answered.insert(charge.key);
	const bool first = ledger.diagnostics.preload_seen.insert(charge.key).second;
	if (first) ++ledger.diagnostics.preload_proposed;
	if (ledger.answered(charge.key)) {
		if (first) ++ledger.diagnostics.preload_free;
	} else if (ledger.admit(charge, compute::DisclosurePriority::Preload) != compute::Code::None) {
		if (first) ++ledger.diagnostics.preload_skipped_reserve;
		return false;
	} else {
		ledger.charge(charge, compute::DisclosurePriority::Preload);
		++ledger.diagnostics.preload_sent;
	}
	ledger.learn(edges);
	return true;
}

std::optional<protected_api::papinian::relations::Plan> plan_relations(const nlohmann::json &data, const nlohmann::json &raw_data,
		const protected_api::papinian::PapinianGuard &guard,
		protected_api::papinian::OperationProtectedSymbolTable &symbols,
		const protected_api::Policy &policy, RelationsReport &report) {
	namespace relations = protected_api::papinian::relations;
	namespace operations = protected_api::papinian::operations;
	const auto started = std::chrono::steady_clock::now();
	relations::Plan planned;
	const auto prompt = data.find("prompt");
	if (prompt == data.end() || !prompt->is_string()) return std::nullopt;
	const std::string &text = prompt->get_ref<const std::string &>();
	const std::string sources = prompt_section(text, "SOURCE_MATERIAL");
	const std::string project = prompt_section(text, "PROJECT_CONTEXT");
	const std::string history = prompt_section(text, "HISTORY");
	if (sources.empty() && project.empty() && history.empty()) return std::nullopt;
	const std::vector<protected_api::papinian::RequestMappingEntry> frozen = symbols.mapping;
	relations::Input input;
	input.mapping = &frozen;
	// The task is the lawyer's request, as the attempt's first call read it.
	if (symbols.planner_question.empty()) symbols.planner_question = prompt_section(text, "CURRENT_REQUEST");
	input.question = symbols.planner_question;
	// Each source passage on its own, in the order the prompt reads them: up to
	// its closing marker, never the next document's title after it.
	for (std::size_t at = sources.find("[SOURCE:"); at != std::string::npos;) {
		const std::size_t next = sources.find("[SOURCE:", at + 8);
		std::size_t end = next == std::string::npos ? sources.size() : next;
		const std::size_t closing = sources.find("\n[/SOURCE]\n", at);
		if (closing != std::string::npos && closing < end) end = closing + 11;
		input.passages.push_back(sources.substr(at, end - at));
		at = next;
	}
	if (!project.empty() && project != context::kNoneSuppliedPlaceholder) input.passages.push_back(project);
	if (!history.empty() && history != context::kNoneSuppliedPlaceholder) {
		input.passages.push_back(history);
		const std::size_t response = history.rfind("\nRESPONSE:\n");
		if (response != std::string::npos) input.last_answer = history.substr(response);
	}
	std::string whole = input.question;
	for (const std::string &passage : input.passages) whole += passage;
	input.language = semantic::language_of(whole);
	// Local only, never rendered: the lawyer's own question for its intents, the
	// raw passages for the business-day rule that governs, and a rule the lawyer
	// gave in this request.
	const auto rawPrompt = raw_data.find("prompt");
	if (rawPrompt != raw_data.end() && rawPrompt->is_string()) {
		const std::string &raw = rawPrompt->get_ref<const std::string &>();
		if (symbols.planner_raw_question.empty()) symbols.planner_raw_question = prompt_section(raw, "CURRENT_REQUEST");
		input.raw_question = symbols.planner_raw_question;
		input.raw_passages = operations::rule_passages(raw);
		input.matter_rule = operations::lawyer_rule_in(raw);
	}
	const relations::Candidates candidates = relations::derive_candidates(input);
	const relations::TaskFocus focus = relations::focus_of(input);
	planned = relations::plan(input, candidates, focus, relations::plan_context(policy),
		[&](const relations::Derived &derived) {
			return guard.issue_derived_value(symbols, derived.value, derived.type, derived.derivation);
		});
	// The last gate: a line that would carry a protected value is not sent.
	protected_api::papinian::ProtectionResult check;
	check.mapping = symbols.mapping;
	relations::Plan kept = planned;
	kept.lines.clear();
	kept.records.clear();
	for (std::size_t index = 0; index < planned.lines.size(); ++index) {
		if (!guard.validate_outbound_payload(planned.lines[index], check).masking_complete) {
			++report.refused;
			continue;
		}
		kept.lines.push_back(planned.lines[index]);
		if (index < planned.records.size()) kept.records.push_back(planned.records[index]);
	}
	// What a preload must leave of each root: the heaviest single charge an
	// operation over the attempt's tables could make of one (a bound ranked
	// among a table's rows), so no preload makes that operation impossible.
	keep_for_tables(symbols);
	// Registered and charged before they leave (DESIGN4 D5).
	report.dropped_by_budget = register_planner_lines(kept, symbols);
	symbols.observations.diagnostics.preload_skipped_irrelevant =
		std::max(symbols.observations.diagnostics.preload_skipped_irrelevant, planned.skipped_irrelevant);
	report.lines = kept.lines.size();
	report.dropped_by_cap = kept.dropped_by_cap;
	report.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	return kept;
}

void require_json_structure_unchanged(const nlohmann::json &before,
		const nlohmann::json &after) {
	if (before.type() != after.type())
		throw std::runtime_error("Papinian restoration cannot change response protocol structure.");
	if (before.is_object()) {
		if (before.size() != after.size())
			throw std::runtime_error("Papinian restoration cannot change response protocol keys.");
		for (auto entry = before.begin(); entry != before.end(); ++entry) {
			if (!after.contains(entry.key()))
				throw std::runtime_error("Papinian restoration cannot change response protocol keys.");
			require_json_structure_unchanged(entry.value(), after.at(entry.key()));
		}
	} else if (before.is_array()) {
		if (before.size() != after.size())
			throw std::runtime_error("Papinian restoration cannot change response protocol array shape.");
		for (std::size_t index = 0; index < before.size(); ++index)
			require_json_structure_unchanged(before.at(index), after.at(index));
	}
}

void require_schema_protocol_values_unchanged(const nlohmann::json &before,
		const nlohmann::json &after, const nlohmann::json &schema) {
	if (!schema.is_object()) return;
	if (schema.contains("enum") && before != after)
		throw std::runtime_error("Papinian restoration cannot change an enum-constrained protocol value.");
	if (before.is_object() && after.is_object() && schema.contains("properties")
			&& schema.at("properties").is_object()) {
		for (auto property = schema.at("properties").begin(); property != schema.at("properties").end(); ++property)
			if (before.contains(property.key()) && after.contains(property.key()))
				require_schema_protocol_values_unchanged(
					before.at(property.key()), after.at(property.key()), property.value());
	}
	if (before.is_array() && after.is_array() && schema.contains("items"))
		for (std::size_t index = 0; index < std::min(before.size(), after.size()); ++index)
			require_schema_protocol_values_unchanged(before.at(index), after.at(index), schema.at("items"));
	if (schema.contains("anyOf") && schema.at("anyOf").is_array())
		for (const auto &alternative : schema.at("anyOf"))
			require_schema_protocol_values_unchanged(before, after, alternative);
}

// Whether what is sent carries a protected value Singularity can work a fact out
// with: an amount, a date, a rate or a quantity.
bool carries_computable_symbol(const std::string &prompt,
		const std::vector<protected_api::papinian::RequestMappingEntry> &mapping) {
	using protected_api::papinian::EntityType;
	for (const protected_api::papinian::RequestMappingEntry &entry : mapping) {
		if (!entry.type) continue;
		const EntityType type = *entry.type;
		if (type != EntityType::Date && type != EntityType::MoneyAmount && type != EntityType::Percentage
				&& type != EntityType::Number)
			continue;
		if (prompt.find(entry.placeholder) != std::string::npos) return true;
	}
	return false;
}


} // namespace

// The pictures of the member's own that the exact body carries (D-256), or a
// refusal: a picture in a body that is not one the member attached to this
// request and sent is never sent, whatever the protection.
std::vector<protected_api::papinian::OutboundPicture> hold_pictures_to_the_members(
		const providers::FinalPayloadView &payload) {
	const nlohmann::json body = nlohmann::json::parse(payload.serialized_body, nullptr, false);
	std::vector<std::string> carried;
	if (body.is_discarded() == false) collect_body_pictures(body, carried);
	const std::shared_ptr<const std::vector<providers::InputPicture>> given = providers::current_input_pictures();
	std::vector<protected_api::papinian::OutboundPicture> sent;
	bool isHeld = carried.size() == payload.picture_sha256.size();
	for (std::size_t index = 0; index < carried.size() && isHeld; ++index) {
		const providers::InputPicture *found = nullptr;
		if (given) {
			for (const providers::InputPicture &picture : *given)
				if (picture.base64 == carried[index] && picture.sha256 == payload.picture_sha256[index]) found = &picture;
		}
		if (found == nullptr) {
			isHeld = false;
			break;
		}
		sent.push_back(protected_api::papinian::OutboundPicture{found->name, found->bytes, found->sha256});
	}
	if (isHeld == false)
		throw providers::TaskError(providers::TaskErrorCode::ProtectedPayloadLeakDetected,
			"Singularity blocked the request because it carried a picture no member attached to it, so nothing was sent.");
	return sent;
}

ProtectedGenerationResult execute_protected_generation(
    const std::string &provider_id, const std::string &api_key,
    const std::string &model_id, const ModelVisibleTurn &turn,
    const std::optional<std::string> &base_url,
    const protected_api::Policy &policy,
    const protected_api::papinian::PapinianGuard &guard,
    const std::string &request_namespace,
    providers::IProviderTransport *transport,
	const std::vector<ProjectEntityProtectionHint> &project_entity_hints,
	std::optional<std::uint64_t> maximum_output_tokens,
	const std::optional<providers::GenerationOutputContract> &output_contract,
	const std::optional<providers::GenerationToolControl> &tool_control,
	protected_api::papinian::OperationProtectedSymbolTable *operation_symbols) {
  platform::trace::Span span("generation", "execute_protected_generation",
      {{"provider", provider_id}, {"model", model_id}, {"namespace", request_namespace}}, true);
  using protected_api::Mode;

  const bool protection_active = policy.mode != Mode::Off;
  protected_api::papinian::ProtectionResult protected_request;
	ProtectionPlan protectionPlan;
	// The relations this call carries and their local records (the hook below
	// fills it; the legend and the outbound record read it).
	PlannedRelations plannedRelations;
	protected_api::papinian::OperationProtectedSymbolTable localOperationSymbols;
	auto *activeOperationSymbols = operation_symbols == nullptr ? &localOperationSymbols : operation_symbols;
	const nlohmann::json canonicalControl = turn.control.value;
	const std::string canonicalControlDigest = platform::sha256_hex_digest(canonicalControl.dump());
	ModelVisibleTurn outboundTurn = turn;
	// Verbatim spans (DESIGN4 §8.7): a protected send keeps only those whose
	// bytes this operation registered from the provider's own reply; any other
	// text that looks like one - and every span of an unprotected send - loses
	// its marks and is ordinary text.
	protected_api::papinian::compute::neutralize_verbatim(outboundTurn.data.value,
		protection_active ? activeOperationSymbols : nullptr);
	// An earlier answer's fact anchors (⟪writing|fact:key⟫) are local marks for
	// protection to read; a send nothing protects writes the writing alone.
	if (!protection_active) {
		const ProtectionPlan unprotected = compile_protection_plan(outboundTurn.data.value);
		for (const auto &leaf : unprotected.leaves)
			if (leaf.value.find(protected_api::papinian::fact_anchor_marks::kKey) != std::string::npos)
				outboundTurn.data.value[nlohmann::json::json_pointer(leaf.identity)]
					= protected_api::papinian::strip_fact_anchor_marks(leaf.value);
	}
	std::string outbound_prompt = outboundTurn.serialize();
  if (protection_active) {
	const auto protectionStarted = std::chrono::steady_clock::now();
	for (const ProjectEntityProtectionHint &hint : project_entity_hints) {
		activeOperationSymbols->project_entity_aliases[hint.entity_id] = hint.local_values;
		activeOperationSymbols->project_entity_types[hint.entity_id] = hint.category;
		if (!hint.symbol.empty()) activeOperationSymbols->project_entity_symbols[hint.entity_id] = hint.symbol;
		if (!hint.curated_values.empty())
			activeOperationSymbols->project_entity_curated_aliases[hint.entity_id] =
				std::set<std::string>(hint.curated_values.begin(), hint.curated_values.end());
	}
#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC)
	const auto chunksBefore = lm_gliner2_semantic_chunks_executed_total();
	const auto encoderForwardsBefore = lm_gliner2_encoder_forward_calls_total();
	const auto encoderBatchesBefore = lm_gliner2_encoder_batches_total();
	const auto downstreamForwardsBefore = lm_gliner2_downstream_forward_calls_total();
#endif
	memory_diagnostics::checkpoint("protected_inference_before_guard",
		"unprotected_prompt_bytes=" + std::to_string(outbound_prompt.size()));
	memory_diagnostics::progress("Papinian: protecting data plane locally (" + std::to_string(outbound_prompt.size())
		+ " bytes) - local NER, no network...");
	observability::emit(observability::OperationEvent{observability::OperationEventKind::PapinianProtectStart,
		"Papinian: protecting data plane locally - local NER, no network",
		{{"promptBytes", std::to_string(outbound_prompt.size())}},
		observability::has_sink() ? std::optional<std::string>(outbound_prompt) : std::nullopt});
	protected_request.request_namespace = request_namespace;
	protectionPlan = compile_protection_plan(outboundTurn.data.value);
	// A fresh table writes typed symbols ([[Org1]], [[Amount3]]) unless the
	// request's own data already writes such a shape: then compact ones, so no
	// word of the material can be read back as a symbol. Only what protection
	// reads counts: Singularity's own instruction sections (CAPABILITY names
	// [[Amount3]] as an example of the symbols it issues) are machine-owned -
	// never mapped, never read back - exactly as the transform treats them.
	if (activeOperationSymbols->mapping.empty()
			&& activeOperationSymbols->marker_style == protected_api::papinian::markers::MarkerStyle::Typed) {
		const auto collides = [&](const PlannedDataLeaf &leaf) {
			if (leaf.policy == ProtectionPolicy::MachineOwnedPassThrough) return false;
			if (leaf.policy != ProtectionPolicy::SemanticDetection) {
				for (const LeafSpan &span : exact_leaf_spans(leaf, leaf.value))
					if (!span.machine_owned && protected_api::papinian::markers::collides(
							std::string_view(leaf.value).substr(span.from, span.to - span.from)))
						return true;
				return false;
			}
			for (const LeafSpan &span : semantic_leaf_spans(leaf.identity, leaf.value))
				if (!span.machine_owned && protected_api::papinian::markers::collides(
						std::string_view(leaf.value).substr(span.from, span.to - span.from)))
					return true;
			return false;
		};
		if (std::any_of(protectionPlan.leaves.begin(), protectionPlan.leaves.end(), collides))
			activeOperationSymbols->marker_style = protected_api::papinian::markers::MarkerStyle::Compact;
	}
	std::vector<protected_api::papinian::SemanticLeaf> semanticLeaves;
	semanticLeaves.reserve(protectionPlan.semantic_detection);
	// Each passage under its document, by the digest detection files it under:
	// what a defined term, a table's role or a sentence read twice can say "the
	// same fact" within (OperationProtectedSymbolTable::document_of_passage).
	std::vector<std::pair<std::string, std::string>> passagesOfDocuments;
	for (const auto &leaf : protectionPlan.leaves) {
		if (leaf.policy != ProtectionPolicy::SemanticDetection) continue;
		const std::vector<DetectionPart> parts = semantic_detection_parts(leaf.identity, leaf.value);
		for (std::size_t index = 0; index < parts.size(); ++index) {
			semanticLeaves.push_back({parts.size() == 1 ? leaf.identity : leaf.identity + "#" + std::to_string(index),
				parts[index].text, parts[index].user_authored, parts[index].model_authored});
			if (!parts[index].document.empty())
				passagesOfDocuments.emplace_back(leaf.identity + "|" + parts[index].document,
					platform::sha256_hex_digest(parts[index].text));
		}
	}
	protected_api::papinian::file_passages_under_documents(*activeOperationSymbols, passagesOfDocuments);
	// Text Singularity wrote itself is never semantically scored, but it may still
	// name something this Project protects (an instruction bound to a document,
	// a coverage statement). Those known values are issued before the freeze,
	// so they are masked like any other - and a request can no longer fail
	// closed because the table had never seen one.
	for (const auto &leaf : protectionPlan.leaves) {
		if (leaf.policy != ProtectionPolicy::ExactMappingOnly) continue;
		for (const LeafSpan &span : exact_leaf_spans(leaf, leaf.value)) {
			if (span.machine_owned) continue;
			const auto known = guard.collect_known_values(leaf.value.substr(span.from, span.to - span.from), policy,
				request_namespace, *activeOperationSymbols);
			protected_request.entities.insert(protected_request.entities.end(), known.begin(), known.end());
		}
	}
	protected_api::papinian::ProtectionBatchMetrics batchMetrics;
	const auto detectedLeaves = guard.collect_sensitive_values_batch(
		semanticLeaves, policy, request_namespace, *activeOperationSymbols, &batchMetrics);
	for (const auto &leaf : detectedLeaves)
		protected_request.entities.insert(protected_request.entities.end(),
			leaf.entities.begin(), leaf.entities.end());
	// The table grows as the request is read, so a passage read early was
	// weighed against fewer known spellings than the finished table holds. The
	// transform runs every passage against that finished table, and where a
	// spelling learned later overlaps one accepted earlier, the two are taken
	// as one range - a value the frozen table has no row for, and the request
	// fails closed on work it had already protected. Reading the same passages
	// once more against the settled table issues those rows before the freeze.
	// The same text, in the same units, as detection and the transform use.
	for (int settling = 0; settling < 2; ++settling) {
		const std::size_t before = activeOperationSymbols->mapping.size();
		for (const auto &leaf : semanticLeaves) {
			const auto known = guard.collect_known_values(leaf.text, policy, request_namespace,
				*activeOperationSymbols, leaf.user_authored ? protected_api::papinian::FactProvenance::Origin::Request
					: leaf.model_authored ? protected_api::papinian::FactProvenance::Origin::History
					: protected_api::papinian::FactProvenance::Origin::Source);
			protected_request.entities.insert(protected_request.entities.end(), known.begin(), known.end());
		}
		if (activeOperationSymbols->mapping.size() == before) break;
	}
	// A resumed run's operations (DESIGN4 §1.10), rebuilt once its material has
	// been read and protected again - before the freeze, so the derived facts
	// the prompt's anchors name are rows of this table for this very call.
	protected_api::papinian::compute::restore_pending(guard, *activeOperationSymbols, policy, turn.serialize());
	const protected_api::papinian::OperationProtectedSymbolTable frozenSymbols = *activeOperationSymbols;
	const std::size_t frozenMappingSize = frozenSymbols.mapping.size();
	const auto transformStarted = std::chrono::steady_clock::now();
	transform_planned_leaves(outboundTurn.data.value, protectionPlan, guard, frozenSymbols);
	const double transformMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - transformStarted).count();
	if (activeOperationSymbols->mapping.size() != frozenMappingSize)
		throw std::runtime_error("Papinian mapping changed after the typed data-plane freeze");
	protected_request.mapping = frozenSymbols.mapping;
	// The relations the model needs about the protected amounts and dates it
	// reads, worked out here (Papinian round 3, lane P2). A value they derive is
	// a row of its own; the data plane is then transformed once more against the
	// table that holds it, so a figure of that value anywhere is its symbol too,
	// and validated like any other.
	RelationsReport relationsReport;
	if (protected_api::papinian::relations::kProtectedRelations && !frozenSymbols.mapping.empty()) {
		const std::optional<protected_api::papinian::relations::Plan> plan = plan_relations(outboundTurn.data.value,
			turn.data.value, guard, *activeOperationSymbols, policy, relationsReport);
		if (plan) {
			plannedRelations.lines = plan->lines;
			plannedRelations.records = plan->records;
			activeOperationSymbols->surfaced_relations.clear();
			for (const auto &record : plan->records)
				if (!record.results.empty()) activeOperationSymbols->surfaced_relations.push_back(record);
			plannedRelations.withheld = plan->withheld;
			// The planner governs the timeline too: it orders only the dates the
			// task's relations and question need.
			plannedRelations.timeline_markers = plan->timeline_markers;
		}
		if (activeOperationSymbols->mapping.size() != frozenMappingSize) {
			const protected_api::papinian::OperationProtectedSymbolTable withDerived = *activeOperationSymbols;
			transform_planned_leaves(outboundTurn.data.value, protectionPlan, guard, withDerived);
			if (activeOperationSymbols->mapping.size() != withDerived.mapping.size())
				throw std::runtime_error("Papinian mapping changed after the typed data-plane freeze");
			protected_request.mapping = withDerived.mapping;
		}
	}
	outbound_prompt = outboundTurn.serialize();
	protected_request.protected_text = outbound_prompt;
	const auto validationStarted = std::chrono::steady_clock::now();
	protected_request.validation = validate_planned_leaves(
		outboundTurn.data.value, protectionPlan, guard, protected_request);
	// Repair before refusing (repair_planned_leaves): a writing the transform did
	// not reach is masked with its own row's symbol and the whole plan checked
	// again. Twice at most; what still stands fails closed below.
	LeafRepair leakRepair;
	std::size_t repairedPlaces = 0;
	for (int round = 0; round < 2 && !protected_request.validation.masking_complete; ++round) {
		leakRepair = repair_planned_leaves(outboundTurn.data.value, protectionPlan, guard, protected_request,
			*activeOperationSymbols);
		repairedPlaces += leakRepair.masked;
		outbound_prompt = outboundTurn.serialize();
		protected_request.protected_text = outbound_prompt;
		protected_request.validation = validate_planned_leaves(
			outboundTurn.data.value, protectionPlan, guard, protected_request);
	}
	// The legend is written last, after any repair, so a fact a repair had to
	// give a symbol of its own is described like every other.
	if (!protected_request.mapping.empty()) {
		// Where the protocol is offered (its tables were read), the calendars and
		// band schedules the material states are listed beside them.
		if (activeOperationSymbols->tables_built)
			protected_api::papinian::compute::prepare_calendars_and_schedules(*activeOperationSymbols, turn.serialize());
		const RoleLegend roleLegend = role_legend(outboundTurn.data.value, semanticLeaves, passagesOfDocuments, guard,
			*activeOperationSymbols, protected_request, outbound_prompt, policy);
		keep_for_tables(*activeOperationSymbols);
		plannedRelations.admit_timeline = [activeOperationSymbols](const TimelineLine &line) {
			return admit_timeline_line(*activeOperationSymbols, line);
		};
		outboundTurn.data.protection_metadata = protection_legend(protected_request, plannedRelations, &roleLegend,
			turn.is_conversation);
		outbound_prompt = outboundTurn.serialize();
		protected_request.protected_text = outbound_prompt;
	}
	// What this call gives the model of the lawyer's material grounds the
	// figures the attempt's answers write openly, in this call and later ones.
	protected_api::papinian::answer_contract::remember_sent_material(*activeOperationSymbols,
		compute_disclosure::contract_text(outboundTurn));
	const double validationMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - validationStarted).count();
	const double totalProtectionMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - protectionStarted).count();
	memory_diagnostics::progress("Papinian: protection complete - " + std::to_string(protected_request.entities.size())
		+ " entit(y/ies) masked");
	{
		// PAPIAN ENTITY DETECTION / MAPPING: this is a genuinely forensic
		// event - CanonicalEntity/RequestMappingEntry carry real Project
		// plaintext (original values). Only ever attached when a sink is
		// registered, i.e. only when a caller (Singularity CLI's own /trace
		// forensic) has explicitly opted in; never persisted here, never
		// entered into normal logs - see OperationEvent.hpp's own comment.
		std::optional<std::string> forensicMapping;
		if (observability::has_sink()) {
			nlohmann::json entities = nlohmann::json::array();
			for (const auto &entity : protected_request.entities) {
				entities.push_back(nlohmann::json{
					{"type", entity.type ? protected_api::papinian::to_string(*entity.type) : "unknown"},
					{"start", entity.start}, {"length", entity.length}, {"confidence", entity.confidence}});
			}
			nlohmann::json mapping = nlohmann::json::array();
			for (const auto &entry : protected_request.mapping) {
				mapping.push_back(nlohmann::json{{"placeholder", entry.placeholder}, {"original", entry.original}});
			}
			forensicMapping = nlohmann::json{{"entities", entities}, {"mapping", mapping},
				{"unmaskedValues", protected_request.validation.leaked_values}}.dump();
		}
		observability::OperationEvent protectEnd{observability::OperationEventKind::PapinianProtectEnd,
			"Papinian: protection complete",
			{{"entityCount", std::to_string(protected_request.entities.size())},
			 {"mappingSize", std::to_string(protected_request.mapping.size())},
			 {"detectionLeafCount", std::to_string(protectionPlan.semantic_detection)},
			 {"totalLeafCount", std::to_string(protectionPlan.leaves.size())},
			 {"machinePassThroughLeafCount", std::to_string(protectionPlan.machine_pass_through)},
			 {"exactMappingLeafCount", std::to_string(protectionPlan.exact_mapping)},
			 {"semanticDetectionLeafCount", std::to_string(protectionPlan.semantic_detection)},
			 {"semanticSamples", std::to_string(semanticLeaves.size())},
			 {"semanticDetectionMs", std::to_string(batchMetrics.semantic_detection_ms)},
			 // Where this send's detections came from: what was prepared when the
			 // documents became ready, what this operation had already scored, and
			 // what the model had to run now.
			 {"detectionsReused", std::to_string(batchMetrics.detections_reused)},
			 {"detectionsFromOperation", std::to_string(batchMetrics.detections_from_operation)},
			 {"detectionsComputed", std::to_string(batchMetrics.detections_computed)},
			 {"mappingBuildMs", std::to_string(batchMetrics.mapping_build_ms)},
			 {"deterministicMs", std::to_string(batchMetrics.deterministic_ms)},
			 {"projectMatchMs", std::to_string(batchMetrics.project_match_ms)},
			 {"reconcileMs", std::to_string(batchMetrics.reconcile_ms)},
			 {"transformMs", std::to_string(transformMs)},
			 {"validationMs", std::to_string(validationMs)},
			 {"totalProtectionMs", std::to_string(totalProtectionMs)},
			 {"mappingFrozenBeforeTransform", "true"},
			 // The relations this send carries: counts and time only.
			 {"relationCount", std::to_string(relationsReport.lines)},
			 {"relationsDropped", std::to_string(relationsReport.dropped_by_cap)},
			 {"relationsDroppedByBudget", std::to_string(relationsReport.dropped_by_budget)},
			 {"relationsRefused", std::to_string(relationsReport.refused)},
			 {"relationsMs", std::to_string(relationsReport.ms)},
			 {"repairedLeakCount", std::to_string(repairedPlaces)},
			 // Facts a repair had to issue for a place no fact could be proven to
			 // be, and the relations sent and held back: counts only.
			 {"occurrenceFactsIssued", std::to_string(activeOperationSymbols->occurrence_facts_issued)},
			 {"relationRecords", std::to_string(plannedRelations.records.size())},
			 {"relationsWithheld", std::to_string(plannedRelations.withheld.size())},
			 {"markerStyle", activeOperationSymbols->marker_style == protected_api::papinian::markers::MarkerStyle::Typed
				 ? "typed" : "compact"},
			 {"unmaskedValueCount", std::to_string(protected_request.validation.leaked_values.size())}}, forensicMapping};
		// What this task disclosed and held back so far (preload and operations).
		for (auto &detail : protected_api::papinian::compute::disclosure_details(activeOperationSymbols->observations))
			protectEnd.details.push_back(std::move(detail));
		observability::emit(protectEnd);
	}
	memory_diagnostics::checkpoint("protected_inference_after_guard",
		"unprotected_prompt_bytes=" + std::to_string(turn.serialize().size())
		+ " protected_prompt_bytes=" + std::to_string(protected_request.protected_text.size())
		+ " semantic_chunks_this_boundary=" + std::to_string(
#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC)
			lm_gliner2_semantic_chunks_executed_total() - chunksBefore
#else
			0
#endif
		)
		+ " encoder_forward_calls_this_boundary=" + std::to_string(
#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC)
			lm_gliner2_encoder_forward_calls_total() - encoderForwardsBefore
#else
			0
#endif
		)
		+ " encoder_batches_this_boundary=" + std::to_string(
#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC)
			lm_gliner2_encoder_batches_total() - encoderBatchesBefore
#else
			0
#endif
		)
		+ " downstream_forward_calls_this_boundary=" + std::to_string(
#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC)
			lm_gliner2_downstream_forward_calls_total() - downstreamForwardsBefore
#else
			0
#endif
		));
	for (auto &mapping : protected_request.mapping) {
		std::set<std::string> matches;
		for (const ProjectEntityProtectionHint &hint : project_entity_hints) {
			if (std::find(hint.local_values.begin(), hint.local_values.end(), mapping.original) != hint.local_values.end())
				matches.insert(hint.entity_id);
		}
		if (matches.size() == 1) mapping.project_entity_id = *matches.begin();
	}
    if (!protected_request.validation.masking_complete) {
      // Why it stopped, for whoever reads this computer's log: which check, the
      // kind of each value and where it stood - never a value (AG-SEC-3).
      {
        std::string kinds;
        for (const auto &occurrence : protected_request.validation.occurrences)
          kinds += std::to_string(static_cast<int>(occurrence.kind)) + "/"
              + (occurrence.row < protected_request.mapping.size() ? value_noun(protected_request.mapping[occurrence.row].type) : std::string("detail")) + " ";
        std::string where;
        for (const std::string &place : leakRepair.where) where += place + "; ";
        std::cerr << "Papinian Guard stop (before sending): " << protected_request.validation.leaked_values.size()
                  << " value(s) left after repair; kinds " << kinds << "; places " << where << std::endl;
      }
      // Said by kind and by place, never by the value: "one protected name in
      // an earlier answer of this chat".
      const auto join = [](const std::set<std::string> &words, const std::string &otherwise) {
        std::string joined;
        for (const std::string &word : words) joined += (joined.empty() ? "" : " and ") + word;
        return joined.empty() ? otherwise : joined;
      };
      throw providers::TaskError(
          providers::TaskErrorCode::ProtectedPayloadLeakDetected,
          "Papinian Guard could not hide one protected " + join(leakRepair.what, "detail") + " in "
          + join(leakRepair.where, "the request") + ", so nothing was sent.");
    }
		outbound_prompt = protected_request.protected_text;
  }
	// A turn that only asks which facts about protected amounts, dates, rates
	// and quantities an answer rests on has nothing to ask where protection
	// left none of them in what would be sent: no call is made.
	if (turn.needs_computable_values
			&& (!protection_active || !carries_computable_symbol(outbound_prompt, protected_request.mapping))) {
		ProtectedGenerationResult skipped;
		skipped.protected_api_applied = protection_active;
		skipped.provider_call_skipped = true;
		return skipped;
	}
	bool final_payload_validated = false;
	std::string final_payload_sha256;
	// The member's pictures the validated body carried as they are (D-256).
	std::vector<protected_api::papinian::OutboundPicture> sent_pictures;
	// A design partner's computer keeps this call in its chat's work log
	// (Node/WorkLog, D-257): the body exactly as it leaves, once the final
	// check below has let it through - never a body it refused, never the text
	// before protection, never the mapping (AG-SEC-3).
	std::optional<std::string> workLogBody;

  const providers::FinalPayloadValidator validate_final_payload =
      [&](const providers::FinalPayloadView &payload) {
		auto keepForWorkLog = [&]() {
			if (node::work_log::wanted() == true) workLogBody = payload.serialized_body;
		};
		final_payload_sha256 = platform::sha256_hex_digest(payload.serialized_body);
		// EXACT PROVIDER BODY: the real, final serialized body, exactly as it
		// will be transported - emitted only once this same leak boundary
		// has already validated it below (never before, never a
		// reconstruction made later). Never includes the Authorization
		// header - FinalPayloadView deliberately never carries headers at
		// all (see ProviderRegistry.hpp's own comment).
		auto emitValidatedBody = [&]() {
			observability::emit(observability::OperationEvent{observability::OperationEventKind::ProviderSendStart,
				"provider: final request body validated, sending",
				{{"bodyBytes", std::to_string(payload.serialized_body.size())}},
				observability::has_sink() ? std::optional<std::string>(payload.serialized_body) : std::nullopt});
		};
		for (const auto &content : payload.content_segments) {
			const ModelVisibleTurn delivered = ModelVisibleTurn::parse(content);
			const std::string deliveredDigest = platform::sha256_hex_digest(delivered.control.value.dump());
			if (delivered.control.value != canonicalControl || deliveredDigest != canonicalControlDigest)
				throw providers::TaskError(providers::TaskErrorCode::ProtectedPayloadLeakDetected,
					"Singularity blocked the request because its execution control plane changed before provider delivery.");
		}
		// No picture but the member's own, protection on or off (D-256).
		sent_pictures = hold_pictures_to_the_members(payload);
		if (!protection_active) { emitValidatedBody(); final_payload_validated = true; keepForWorkLog(); return; }
        protected_api::papinian::GuardValidation validation;
		// What still stands and where, in words - never the value - for the
		// refusal and its routing record.
		std::set<std::string> blockedKinds;
		std::set<std::string> blockedPlaces;
        for (const auto &content : payload.content_segments) {
		  const ModelVisibleTurn delivered = ModelVisibleTurn::parse(content);
		  const auto segment_validation = validate_planned_leaves(
			  delivered.data.value, protectionPlan, guard, protected_request);
          if (!segment_validation.masking_complete) {
            std::cerr << "Papinian Guard stop (final check): a content segment still holds "
                      << segment_validation.leaked_values.size() << " value(s)" << std::endl;
            validation.masking_complete = false;
            validation.leaked_values.insert(validation.leaked_values.end(),
                                             segment_validation.leaked_values.begin(),
                                             segment_validation.leaked_values.end());
			blockedPlaces.insert("the request");
			for (const auto &occurrence : segment_validation.occurrences)
				blockedKinds.insert(occurrence.row < protected_request.mapping.size()
					? value_noun(protected_request.mapping[occurrence.row].type) : std::string("detail"));
          }
        }
		// The request's schemas are outside the transformed data plane, so a
		// protected value must never appear in one: check every enum and const
		// string of the exact body against this operation's protected values.
		{
			const nlohmann::json body = nlohmann::json::parse(payload.serialized_body, nullptr, false);
			std::vector<std::string> schemaLiterals;
			if (!body.is_discarded()) collect_schema_literals(body, schemaLiterals);
			for (const std::string &literal : schemaLiterals)
				for (const auto &entry : protected_request.mapping)
					if (!is_protocol_literal(literal))
					for (const std::string &form : entry.every_form())
					if (form.size() >= 2 && literal.find(form) != std::string::npos) {
						std::cerr << "Papinian Guard stop (final check): a schema literal of " << literal.size()
							<< " characters holds a " << form.size() << "-character writing of a " << value_noun(entry.type) << std::endl;
						validation.masking_complete = false;
						validation.leaked_values.push_back(form);
						blockedPlaces.insert("the request's tool definitions");
						blockedKinds.insert(value_noun(entry.type));
					}
		}
        if (!validation.masking_complete) {
#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC)
			nlohmann::json framing = nlohmann::json::parse(payload.serialized_body, nullptr, false);
			if (framing.is_object() && framing.contains("messages") && framing.at("messages").is_array()
					&& !framing.at("messages").empty()) framing["messages"][0]["content"] = "";
			const std::string framingOnly = framing.is_discarded() ? std::string{} : framing.dump();
			for (const std::string &leaked : validation.leaked_values) {
				std::printf("PRODMEMDIAG final_validation_leak length=%zu sha256=%s framing_match=%s protected_prompt_match=%s\n",
					leaked.size(), platform::sha256_hex_digest(leaked).c_str(),
					framingOnly.find(leaked) == std::string::npos ? "false" : "true",
					outbound_prompt.find(leaked) == std::string::npos ? "false" : "true");
			}
			std::fflush(stdout);
#endif
          const auto joined = [](const std::set<std::string> &words, const std::string &otherwise) {
            std::string joinedWords;
            for (const std::string &word : words) joinedWords += (joinedWords.empty() ? "" : " and ") + word;
            return joinedWords.empty() ? otherwise : joinedWords;
          };
          throw providers::TaskError(
              providers::TaskErrorCode::ProtectedPayloadLeakDetected,
              "Papinian Guard blocked this request at the final check before sending: one protected "
              + joined(blockedKinds, "detail") + " stood in " + joined(blockedPlaces, "the request") + ".");
        }
		emitValidatedBody();
		final_payload_validated = true;
		keepForWorkLog();
      };

	// The outbound record of this call (Routing/RoutingLog/OutboundRecord.hpp):
	// where it went, under which protection, and exactly the markers that stand
	// in what it sent - read from the prompt as it left, so a row of the
	// operation's table that this request never carried is not claimed.
	const std::string outboundOperationKey = activeOperationSymbols->operation_id.empty()
		? request_namespace : activeOperationSymbols->operation_id;
	// A key that is this request's own namespace names no operation: a Context
	// or Memory call, or a single call with no table of its own.
	const bool isOutboundOperation = outboundOperationKey != request_namespace;
	bool isOutboundCallNoted = false;
	// `answered` is what came back, or none when the call failed before it.
	const auto noteOutboundCall = [&](const providers::GenerationResult *answered) {
		isOutboundCallNoted = true;
		protected_api::papinian::OutboundCall call;
		call.kind = "model";
		call.provider_id = provider_id;
		call.model_id = model_id;
		call.protection_mode = protected_api::to_string(policy.mode);
		call.payload_sha256 = final_payload_sha256;
		// Each picture that left as it is, by its name, size and SHA-256 (D-256).
		call.pictures = sent_pictures;
		// The evidence a bill's line rests on (D-252): the request as the
		// provider's records name it, the answer's fingerprint, and this call's
		// own tokens and cost as the provider reported them.
		if (answered != nullptr) {
			call.provider_request_id = answered->response_id;
			call.response_sha256 = answered->response_sha256;
			call.input_tokens = answered->usage.input_tokens;
			call.cached_input_tokens = answered->usage.cached_input_tokens;
			call.output_tokens = answered->usage.output_tokens;
			call.reasoning_tokens = answered->usage.reasoning_tokens;
			call.cost_micros = answered->usage.cost_micros;
			call.cost_currency = answered->usage.cost_currency.value_or("");
			call.cost_classification = answered->usage.cost_classification.value_or("");
			call.cost_methodology = answered->usage.cost_methodology.value_or("");
			call.served_by = answered->usage.served_by.value_or("");
		}
		if (protection_active) {
			// Markers are numbered per table: the call names the table it read them from.
			if (activeOperationSymbols->outbound_scope.empty()) activeOperationSymbols->outbound_scope = outbound::new_symbol_scope();
			call.symbol_scope = activeOperationSymbols->outbound_scope;
			for (const std::string &marker : outbound::markers_in(outbound_prompt)) {
				const auto row = std::find_if(protected_request.mapping.begin(), protected_request.mapping.end(),
					[&](const protected_api::papinian::RequestMappingEntry &entry) { return entry.placeholder == marker; });
				if (row == protected_request.mapping.end()) continue;
				protected_api::papinian::OutboundMarker sent;
				sent.marker = marker;
				sent.category = row->type ? protected_api::papinian::to_string(*row->type)
					: (row->source == protected_api::papinian::DetectionSource::CustomTerm ? "CUSTOM_TERM" : "URL");
				sent.project_entity_id = row->project_entity_id.value_or("");
				++call.category_counts[sent.category];
				call.markers.push_back(std::move(sent));
			}
			// Each passage and the instruction exactly as they left, cut from the
			// prompt that was sent; what they were built from is only digested.
			try {
				call.passages = outbound::sent_passages(turn.data.value, ModelVisibleTurn::parse(outbound_prompt).data.value,
					[](const std::string &text) { return platform::sha256_hex_digest(text); });
			} catch (...) {
				call.passages.clear();
			}
			// The relations this call carried, with their local provenance - the
			// planner's, and each an operation answered for the model whose line
			// stands in what left - and those held back. Never sent; the lawyer
			// reads them in "What left this device".
			call.relations = plannedRelations.records;
			call.withheld_relations = plannedRelations.withheld;
			// A record is local provenance and keeps no writing of a protected
			// value (PrivacyMetadata.hpp). A line that wrote one (a model's own
			// figure an answer echoed) is kept as the protected prompt wrote it -
			// the value's symbol - and found there; a derivation that still writes
			// one is not kept (the lawyer reads "the values were not kept").
			const auto symbolic = [&](std::string text) {
				if (text.empty() || !protection_active) return text;
				const protected_api::papinian::GuardValidation found = guard.validate_outbound_payload(text, protected_request);
				return found.masking_complete ? text : guard.mask_leak_occurrences(text, found, protected_request.mapping);
			};
			const auto keepSymbolic = [&](protected_api::papinian::OutboundRelation &relation) {
				if (!relation.derivation.empty()
						&& !guard.validate_outbound_payload(relation.derivation, protected_request).masking_complete)
					relation.derivation.clear();
			};
			for (auto &relation : call.relations) keepSymbolic(relation);
			for (auto &relation : call.withheld_relations) keepSymbolic(relation);
			auto &pending = activeOperationSymbols->pending_relations;
			for (auto relation = pending.begin(); relation != pending.end();) {
				if (relation->decision == "withheld") {
					keepSymbolic(*relation);
					call.withheld_relations.push_back(std::move(*relation));
					relation = pending.erase(relation);
					continue;
				}
				std::string sent = relation->sent;
				if (!sent.empty() && outbound_prompt.find(sent) == std::string::npos) sent = symbolic(sent);
				if (!sent.empty() && outbound_prompt.find(sent) != std::string::npos) {
					keepSymbolic(*relation);
					relation->sent = sent;
					call.relations.push_back(std::move(*relation));
					relation = pending.erase(relation);
				} else {
					++relation;
				}
			}
			// The protected computation protocol (DESIGN4 §11.2), read from exactly
			// what this call sent: every symbol it carried - all an operation or a
			// final answer may name - the role lines and table blocks of its
			// legend, and each operation result it returned to the model, with the
			// operation as the previous call's response asked for it.
			const std::string sentText = compute_disclosure::sent_text(outboundTurn);
			for (const std::string &symbol : compute_disclosure::symbols_sent(outboundTurn))
				activeOperationSymbols->sent_symbols.insert(symbol);
			call.roles = compute_disclosure::roles_sent(outboundTurn.data.protection_metadata, sentText,
				*activeOperationSymbols, policy.relations);
			call.tables = compute_disclosure::tables_sent(outboundTurn.data.protection_metadata, sentText,
				*activeOperationSymbols);
			// Every operation the table holds that no call lists yet was asked in
			// the response of this table's previous call - refused ones too, and
			// ones whose result never reached a call - and each is listed there
			// once. Then every result line this body carries, on this call, each
			// with the call that asked it. What the model wrote into a request or
			// its why is kept as symbols (`symbolic`), never as a value.
			std::map<std::string, std::string> maskedOnce;
			const compute_disclosure::Symbolic operationText = [&](const std::string &text) {
				const auto known = maskedOnce.find(text);
				if (known != maskedOnce.end()) return known->second;
				return maskedOnce.emplace(text, symbolic(text)).first->second;
			};
			outbound::note_operations_requested(outboundOperationKey, call.symbol_scope,
				compute_disclosure::operations_held(*activeOperationSymbols, operationText,
					outbound::requested_ops(outboundOperationKey)));
			call.operations_returned = compute_disclosure::returned_in(*activeOperationSymbols, sentText, final_payload_sha256,
				operationText);
			for (auto &operation : call.operations_returned)
				operation.asked_in_reply_to = outbound::asked_in(outboundOperationKey, operation.op_id);
		}
		outbound::record_call(outboundOperationKey, std::move(call), isOutboundOperation);
	};

  // The answer is shown while it is written (Routing/LiveAnswer), with this
  // call's symbols put back for the lawyer's own screen: the provider is sent
  // and answers in symbols, and nothing of the shown text is kept.
  std::optional<providers::GenerationStreamScope> liveScope;
  if (const std::shared_ptr<providers::GenerationStream> shown = providers::current_generation_stream();
		shown != nullptr && shown->text && !protected_request.mapping.empty()) {
	std::map<std::string, std::string> values;
	for (const auto &entry : protected_request.mapping) values.emplace(entry.placeholder, entry.original);
	auto restored = std::make_shared<providers::GenerationStream>();
	restored->begin = shown->begin;
	restored->text = live_answer::restoring(shown->text, std::move(values));
	liveScope.emplace(std::move(restored));
  }
  providers::GenerationResult generation;
	// The call as its chat's work log keeps it, on a design partner's computer
	// (D-257): what went, the answer as read before anything was restored, or
	// what failed.
	const auto workLogStarted = std::chrono::steady_clock::now();
	const std::string workLogAt = node::work_log::wanted() == true ? node::work_log::now_text() : std::string();
	const auto keepCall = [&](const std::string &answer, const std::string &failure) {
		if (workLogBody.has_value() == false) return;
		node::work_log::Call call;
		call.at = workLogAt;
		call.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - workLogStarted).count();
		call.provider_id = provider_id;
		call.model_id = model_id;
		call.purpose = request_namespace;
		call.is_protected = protection_active;
		call.request = *workLogBody;
		call.answer = answer;
		call.failure = failure;
		call.input_tokens = generation.usage.input_tokens.value_or(-1);
		call.output_tokens = generation.usage.output_tokens.value_or(-1);
		node::work_log::record(call);
		workLogBody.reset();
	};
  try {
	memory_diagnostics::checkpoint("protected_inference_before_provider_serialization",
		"protected_prompt_bytes=" + std::to_string(outbound_prompt.size()));
	memory_diagnostics::progress("provider: sending real generation request to " + provider_id + "/" + model_id
		+ " (" + std::to_string(outbound_prompt.size()) + " prompt bytes) - real network call...");
    generation = transport
                    ? providers::generate_with_transport(
                          provider_id, api_key, model_id, outbound_prompt,
                          validate_final_payload, *transport, base_url, maximum_output_tokens,
                          output_contract, tool_control, turn.reasoning_effort)
                    : providers::generate(provider_id, api_key, model_id, outbound_prompt,
                                          validate_final_payload, base_url, maximum_output_tokens,
                                          output_contract, tool_control, turn.reasoning_effort);
	noteOutboundCall(&generation);
	if (generation.tool_calls.empty())
		if (std::optional<std::string> last = textual_answer::last_of_whole_objects(generation.text)) {
			observability::emit({observability::OperationEventKind::ModelResultKind,
				"provider: the reply wrote its answer more than once; the last is read",
				{{"reason", "repeated_final_object"}, {"textBytes", std::to_string(generation.text.size())},
				 {"readBytes", std::to_string(last->size())}}});
			generation.text = std::move(*last);
		}
	{
		// PARSED PROVIDER RESULT: the parsed provider tool call(s) - call id,
		// function name, raw arguments - exactly as ProviderRegistry.cpp's
		// own choices[].message.tool_calls parsing produced them, plus any
		// accompanying text content. Never the raw HTTP body (not threaded
		// through this layer's own return type) - the normalized shape Singularity
		// itself already trusts is enough to debug tool-call
		// normalization without adding a second body-capture path.
		std::optional<std::string> forensicResponse;
		if (observability::has_sink()) {
			nlohmann::json toolCalls = nlohmann::json::array();
			for (const auto &call : generation.tool_calls) {
				toolCalls.push_back(nlohmann::json{{"id", call.call_id}, {"name", call.wire_name},
					{"arguments", call.arguments.is_discarded() ? nlohmann::json(nullptr) : call.arguments}});
			}
			forensicResponse = nlohmann::json{{"text", generation.text}, {"toolCalls", toolCalls},
				{"finishReason", generation.finish_reason.value_or("")}}.dump();
		}
		observability::emit(observability::OperationEvent{observability::OperationEventKind::ProviderResultParsed,
			"provider: parsed generation result", {{"textBytes", std::to_string(generation.text.size())},
			{"toolCalls", std::to_string(generation.tool_calls.size())},
			{"outputLimitReached", generation.finish_reason == "length" || generation.finish_reason == "max_tokens" ? "true" : "false"}}, forensicResponse});
	}
	if (workLogBody.has_value() == true) {
		nlohmann::json toolCalls = nlohmann::json::array();
		for (const auto &call : generation.tool_calls)
			toolCalls.push_back(nlohmann::json{{"id", call.call_id}, {"name", call.wire_name},
				{"arguments", call.arguments.is_discarded() ? nlohmann::json(nullptr) : call.arguments}});
		keepCall(nlohmann::json{{"text", generation.text}, {"toolCalls", toolCalls},
			{"finishReason", generation.finish_reason.value_or("")}, {"responseId", generation.response_id}}
				.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
			std::string());
	}
	memory_diagnostics::progress("provider: response received - " + std::to_string(generation.text.size())
		+ " text bytes, " + std::to_string(generation.tool_calls.size()) + " tool call(s)");
  } catch (const providers::OutboundPayloadCaptured &captured) {
    // Only reachable when `transport` is an OutboundCapturingTransport
    // (Phase 08 WP8-B preview) - the ordinary send path passes no transport
    // override / a real HttpsProviderTransport, neither of which ever
    // throws this. The final outbound leak guard (validate_final_payload)
    // has already run and passed by this point, exactly as it would for a
    // real send - see generate_openai_style()/generate_anthropic().
    OutboundPreviewCaptured preview_captured;
    preview_captured.outbound_text = outbound_prompt;
    preview_captured.outbound_request_body = captured.body;
    preview_captured.protected_api_applied = protection_active;
    preview_captured.privacy_metadata = build_privacy_metadata(policy, protected_request);
	for (const auto &mapping : protected_request.mapping)
		if (mapping.project_entity_id) preview_captured.privacy_metadata.protected_project_entity_ids.push_back(*mapping.project_entity_id);
	std::sort(preview_captured.privacy_metadata.protected_project_entity_ids.begin(), preview_captured.privacy_metadata.protected_project_entity_ids.end());
	preview_captured.privacy_metadata.protected_project_entity_ids.erase(
		std::unique(preview_captured.privacy_metadata.protected_project_entity_ids.begin(), preview_captured.privacy_metadata.protected_project_entity_ids.end()),
		preview_captured.privacy_metadata.protected_project_entity_ids.end());
	preview_captured.privacy_metadata.final_payload_validated = final_payload_validated;
	preview_captured.privacy_metadata.final_payload_sha256 = final_payload_sha256;
	// What the preview shows is this request's own protection, never a second
	// pass over the prompt: one such pass scored Singularity's own instruction
	// sections, which this one never does, and showed "the PERSON 1's current
	// objective" where "the user's current objective" was sent (24 Sep 2026).
	// The text is the prompt as it leaves; the mapping stays in native memory.
	preview_captured.protection = protected_request;
	const auto sentPrompt = outboundTurn.data.value.find("prompt");
	if (sentPrompt != outboundTurn.data.value.end() && sentPrompt->is_string())
		preview_captured.protection.protected_text = sentPrompt->get<std::string>();
    throw preview_captured;
  } catch (...) {
	// A request the final check let through and the provider then failed on
	// (refused it, timed out, broke off) may still have reached the provider:
	// it is recorded as sent, never assumed to have stayed here.
	if (final_payload_validated == true && isOutboundCallNoted == false) noteOutboundCall(nullptr);
	// And kept, with what failed, in a design partner's work log (D-257).
	if (workLogBody.has_value() == true) {
		std::string failure = "The call failed.";
		try {
			throw;
		} catch (const std::exception &error) {
			failure = error.what();
		} catch (...) {
		}
		keepCall(std::string(), failure);
	}
	throw;
  }

  ProtectedGenerationResult result;
  result.protected_api_applied = protection_active;
  result.protected_count = protected_request.entities.size();
  result.privacy_metadata = build_privacy_metadata(policy, protected_request);
	for (const auto &mapping : protected_request.mapping)
		if (mapping.project_entity_id) result.privacy_metadata.protected_project_entity_ids.push_back(*mapping.project_entity_id);
	std::sort(result.privacy_metadata.protected_project_entity_ids.begin(), result.privacy_metadata.protected_project_entity_ids.end());
	result.privacy_metadata.protected_project_entity_ids.erase(
		std::unique(result.privacy_metadata.protected_project_entity_ids.begin(), result.privacy_metadata.protected_project_entity_ids.end()),
		result.privacy_metadata.protected_project_entity_ids.end());
	result.privacy_metadata.final_payload_validated = final_payload_validated;
	result.privacy_metadata.final_payload_sha256 = final_payload_sha256;
	// Every call the operation has made so far travels with this one's result,
	// so the operation's record is whole whichever call's result it keeps.
	result.privacy_metadata.outbound_calls = outbound::calls_for(outboundOperationKey);
	result.privacy_metadata.outbound_operation_key = outboundOperationKey;
  result.usage = generation.usage;
  result.finish_reason = generation.finish_reason;
  result.tool_calls = generation.tool_calls;
  // finish_reason=="length"/"max_tokens" is no longer converted into an
  // UnusableGeneration/ResponseIncomplete failure here FOR A TURN THAT
  // ACTUALLY REQUESTED SEMANTIC COMPLETION - see InferenceExecutor.cpp's own
  // local-provider call site for the full rationale (the same invariant
  // applies identically to a cloud provider: a physical output-limit event is
  // a runtime/resource fact, never itself a semantic-completion verdict, for
  // a TurnContract-compiled operation turn). The text/tool-calls still flow
  // through the normal restoration path below exactly as they would for
  // finish_reason=="stop"; only whether this specific truncation reflects a
  // genuine physical boundary is recorded, via `turn` (this call's own
  // ModelVisibleTurn) rather than inferred. A caller that never requested
  // semantic completion (turn.request_semantic_completion == false - every
  // direct/legacy execute_protected_generation() call, which never reaches
  // ResearchToolLoop's own classification layer at all) keeps today's
  // UNCHANGED behavior below: its own configured max_output_tokens ceiling
  // being reached is still a genuine ResponseIncomplete failure.
  const bool outputLimitReached =
      generation.finish_reason == "length" || generation.finish_reason == "max_tokens";
  result.physical_context_exhausted = turn.request_semantic_completion && outputLimitReached;
  if (!turn.request_semantic_completion && outputLimitReached)
    throw providers::UnusableGeneration(providers::TaskErrorCode::ResponseIncomplete,
        "The model response reached its output limit before it was complete.", result.usage, result.finish_reason);
  // What the model wrote, as the provider sent it (DESIGN4 D13): the research
  // loop echoes the model's own requests and admits its queries from these
  // bytes, never from what restoration made of them. The latest reply only;
  // protected text, symbols and all - no value is here that the provider did
  // not write itself.
  if (activeOperationSymbols != nullptr) {
    nlohmann::json writtenCalls = nlohmann::json::array();
    for (const providers::ModelToolCall &call : generation.tool_calls)
      writtenCalls.push_back({{"wireName", call.wire_name},
        {"arguments", call.arguments.is_discarded() ? nlohmann::json() : call.arguments}});
    activeOperationSymbols->model_authored["reply"] =
      nlohmann::json{{"text", generation.text}, {"toolCalls", std::move(writtenCalls)}}.dump();
  }
  if (!protection_active || protected_request.mapping.empty()) {
    result.text = generation.text;
    return result;
  }

  // Tool-call arguments are restored/integrity-gated whenever present,
  // regardless of what happens to generation.text below - ResearchToolLoop.hpp
  // consumes them (never .text) whenever a turn has tool_calls.
  for (providers::ModelToolCall &call : result.tool_calls) {
    // A tool call whose arguments never parsed as JSON in the first place
    // (ProviderRegistry.cpp's own ModelToolCall::arguments left discarded)
    // has no string leaves to restore - ResearchToolLoop.hpp fails this
    // call closed on shape alone, the same as it would with nothing masked.
    if (call.arguments.is_discarded()) continue;
    // lm_compute (DESIGN4 §1.2, carrier N) is a symbol-plane tool: its ops
    // reach Singularity's executor exactly as the model wrote them - never
    // restored, never read as restored facts, never figure-checked.
    if (protected_api::papinian::compute::is_compute_tool(call.wire_name)) continue;
    const nlohmann::json originalArguments = call.arguments;
		const providers::ModelToolDefinition *definition = nullptr;
		if (tool_control) {
			const auto found = std::find_if(tool_control->tools.begin(), tool_control->tools.end(),
				[&](const auto &entry) { return entry.wire_name == call.wire_name; });
			if (found != tool_control->tools.end()) definition = &*found;
		}
		JsonRestoreOutcome restoredArguments;
		// A symbol Singularity never issued becomes a gap in what is restored; the
		// contract below reads the arguments as the model wrote them.
		std::size_t unissued = 0;
		call.arguments = gap_unissued_symbols(call.arguments, protected_request, activeOperationSymbols, unissued);
		if (definition != nullptr && !definition->response_data_plane_shape.empty())
			restoredArguments = restore_declared_data_leaves(guard, call.arguments,
				definition->response_data_plane_shape, protected_request);
		else if (turn.response_restoration.arbitrary_structured_value_is_data)
			restoredArguments = restore_json_strings(guard, call.arguments, protected_request);
		else
			restoredArguments.value = call.arguments;
    if (!restoredArguments.integrity_ok) {
	  const auto diagnostics = safe_response_diagnostics(restoredArguments.malformed_candidate,
		  restoredArguments.json_locator, protected_request, false);
      throw providers::UnusableGeneration(
          providers::TaskErrorCode::ProtectedResponseInvalid,
			protected_symbol_failure_message(restoredArguments),
          generation.usage, generation.finish_reason, diagnostics);
    }
	if (!restoredArguments.normalized_candidates.empty()) {
		const std::string normalization = restoredArguments.normalized_candidates.front();
		const std::string candidate = normalization.substr(0, normalization.find(" -> "));
		copy_safe_diagnostics(result.privacy_metadata, safe_response_diagnostics(candidate,
			restoredArguments.normalized_locator, protected_request, true));
	}
	try {
		require_json_structure_unchanged(originalArguments, restoredArguments.value);
		if (definition != nullptr)
			require_schema_protocol_values_unchanged(
				originalArguments, restoredArguments.value, definition->parameters_schema);
	} catch (const std::exception &error) {
		throw providers::UnusableGeneration(providers::TaskErrorCode::ProtectedResponseInvalid,
			error.what(), generation.usage, generation.finish_reason);
	}
    call.arguments = std::move(restoredArguments.value);
		// What this operation wrote back for which fact: sent again later in it,
		// the same sentence of it is the same fact (restored_writings).
		for (const auto &[text, anchors] : restoredArguments.restored_texts)
			protected_api::papinian::record_restored_facts(*activeOperationSymbols, text, anchors);
		// The answer contract (DESIGN4 §9) over what a tool call writes into the
		// lawyer's work - a draft's text, a cell - in the model's own protected
		// plane, before anything of it is restored. No final text is corrected
		// for it, so a figure that cannot be verified is never written: the
		// field keeps its sentences and shows a neutral gap in its own language
		// where the figure stood, and the lawyer is told beside the answer. A
		// search, a page opened, an operation asked of Singularity (lm_compute) or a
		// question is not work product.
		static const std::set<std::string> notWorkProduct{"lm_web_search", "lm_web_open", "web_search", "web_open",
			"web_fetch", "lm_document_draft_request_research", "lm_document_inspect", "lm_compute", "papinian.compute",
			"papinian_compute"};
		if (notWorkProduct.count(call.wire_name) == 0) {
			namespace contract = protected_api::papinian::answer_contract;
			const std::size_t callIndex = static_cast<std::size_t>(&call - result.tool_calls.data());
			const std::string contractSent = compute_disclosure::contract_text(outboundTurn);
			std::map<std::string, std::string> withheldFields;
			compute_disclosure::each_field(originalArguments, std::string(),
					[&](const std::string &pointer, const std::string &field) {
				const semantic::Language language = semantic::language_of(field);
				const contract::ContractResult checked = contract::check(field, pointer, *activeOperationSymbols,
					contractSent, language);
				std::size_t failures = 0;
				for (const contract::ContractViolation &violation : checked.violations) {
					if (violation.soft) continue;
					++failures;
					result.untraced_figures.locators.push_back("tool@" + std::to_string(callIndex) + ":" + violation.code
						+ "@" + std::to_string(violation.offset));
				}
				if (failures == 0) return;
				result.untraced_figures.count += failures;
				const contract::Withheld withheld = contract::withhold(field, checked.violations, contract::Surface::Artifact,
					language);
				result.answer_contract.tool_call_withheld += withheld.figures;
				withheldFields.emplace(pointer, withheld.text);
			});
			// Each field with a gap is restored as its siblings were, in its place.
			for (const auto &[pointer, text] : withheldFields) {
				const JsonRestoreOutcome field = restore_json_strings(guard, nlohmann::json(text), protected_request, pointer);
				bool placed = field.integrity_ok;
				try {
					if (placed) call.arguments.at(nlohmann::json::json_pointer(pointer)) = field.value;
				} catch (const std::exception &) {
					placed = false;
				}
				// Never the field with its unverifiable figure instead.
				if (!placed)
					throw providers::UnusableGeneration(providers::TaskErrorCode::ProtectedResponseInvalid,
						"Papinian Guard could not verify a figure the model wrote. The result was not restored.",
						generation.usage, generation.finish_reason);
			}
		}
  }

  // generation.text is never consumed downstream when tool_calls is
  // non-empty (see ResearchToolLoop.hpp's normalize_native_turn(): a turn
  // with tool_calls never reads .text). A real live Phase 24.1D acceptance
  // run proved this is not theoretical: a real model, while correctly
  // proposing a forced native tool call, also emitted accompanying prose
  // whose own placeholder-token fragment failed strict restoration
  // integrity - which used to fail the ENTIRE generation even though that
  // prose was always going to be discarded by the caller. Native tool
  // calling structurally allows "tool call plus commentary" in a way the
  // old strict-JSON-only textual protocol never could (a
  // response_format-constrained turn was always exactly one shape, never
  // both) - restoring, and integrity-gating on, text nobody will ever read
  // is pure risk with no benefit. The tool call's own arguments above are
  // still fully restored and integrity-gated regardless.
  if (generation.tool_calls.empty() == false) {
    result.text = generation.text;
    result.privacy_metadata.response_integrity = "passed";
    return result;
  }

  observability::emit(observability::OperationEvent{observability::OperationEventKind::PapinianRestoreStart,
    "Papinian: restoring provider response text", {{"providerTextBytes", std::to_string(generation.text.size())}},
    observability::has_sink() ? std::optional<std::string>(generation.text) : std::nullopt});
	// The final answer contract (DESIGN4 §9), on the model's own protected text
	// before anything of it is restored: every quantitative symbol exists, was
	// given to this operation, is sourced or worked out here with provenance to
	// the material, fits the words around it and is written as the bound it
	// is; every claimed equation is one Singularity recorded (structure only, no
	// value read); no figure is the model's own. Each field of a structured
	// answer is checked on its own. What fails is recorded by code, symbol,
	// field and offset - never a value - and the caller repairs it; the answer
	// the lawyer may read once no repair is left carries none of it: a chat
	// sentence with such a figure is left out, a document keeps its sentence
	// and shows a neutral gap.
	namespace contract = protected_api::papinian::answer_contract;
	// Singularity's own marks around the model's words sent back to it (the prompt
	// and options of its question to the lawyer, ⟪verbatim⟫…⟪/verbatim⟫) are
	// never part of an answer: a model that copies the lawyer's chosen option
	// with them writes the option alone (a fake-provider run copied them).
	for (const std::string_view mark : {protected_api::papinian::compute::kVerbatimOpen,
			protected_api::papinian::compute::kVerbatimClose})
		for (std::size_t found = generation.text.find(mark); found != std::string::npos; found = generation.text.find(mark, found))
			generation.text.erase(found, mark.size());
	// Nor an earlier answer's fact anchor (⟪writing|fact:key⟫): Singularity writes
	// those into its own history only; one in a reply would pass the model's own
	// figure off as a verified one.
	generation.text = protected_api::papinian::strip_fact_anchor_marks(generation.text);
	const std::string contractSent = compute_disclosure::contract_text(outboundTurn);
	const nlohmann::json finalStructure = nlohmann::json::parse(generation.text, nullptr, false);
	const bool isStructuredFinal = finalStructure.is_object() || finalStructure.is_array();
	std::map<std::string, std::string> withheldFields;
	// The answer's prose - every chat field that reads as a sentence - and what
	// of it withholding leaves: a field left with nothing is dropped from the
	// answer; the answer is emptied only when no prose of it is left at all
	// (DESIGN4 §9.2: a task fails only when every answer sentence would go).
	const auto isProse = [](const std::string &text) {
		bool letter = false;
		for (const char character : text)
			if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
					|| static_cast<unsigned char>(character) >= 0xC0)
				letter = true;
		return letter && text.find(' ') != std::string::npos;
	};
	std::size_t proseFields = 0;
	std::size_t proseFieldsEmptied = 0;
	// A document's fields are in the language the model declared for it
	// ("language": "ru"), never a guess from a field that mixes scripts: a gap
	// in a Russian letter reads «[сумма уточняется]».
	std::optional<semantic::Language> declaredLanguage;
	if (finalStructure.is_object() && finalStructure.contains("artifactAction") && finalStructure.at("artifactAction").is_object()
			&& finalStructure.at("artifactAction").contains("language")
			&& finalStructure.at("artifactAction").at("language").is_string()) {
		const std::string code = finalStructure.at("artifactAction").at("language").get<std::string>();
		if (code == "ru") declaredLanguage = semantic::Language::Russian;
		else if (code == "en") declaredLanguage = semantic::Language::English;
	}
	const auto checkField = [&](const std::string &pointer, const std::string &field) {
		const semantic::Language language = declaredLanguage && compute_disclosure::artifact_field(pointer)
			? *declaredLanguage : semantic::language_of(field);
		const contract::ContractResult checked = contract::check(field, pointer, *activeOperationSymbols, contractSent,
			language);
		result.answer_contract.quantitative_symbols += checked.quantitative_symbols;
		std::size_t failures = 0;
		for (const contract::ContractViolation &violation : checked.violations) {
			// The symbol as the repair message names it: its local anchor, which
			// protection sends as the symbol again.
			const protected_api::papinian::RequestMappingEntry *row = violation.symbol.empty() ? nullptr
				: protected_api::papinian::row_of_marker(*activeOperationSymbols, violation.symbol);
			result.answer_contract.violations.push_back(violation);
			// A figure outside any symbol is named as the model wrote it.
			result.answer_contract.local_symbols.push_back(row == nullptr
				? (violation.written.empty() ? std::string() : "\u201C" + violation.written + "\u201D")
				: protected_api::papinian::operations::local_writing(*row));
			if (violation.soft) continue;
			++failures;
			result.untraced_figures.locators.push_back(violation.code + "@" + (pointer.empty() ? std::string("/") : pointer)
				+ ":" + std::to_string(violation.offset));
		}
		const bool isArtifact = compute_disclosure::artifact_field(pointer);
		const bool isChatProse = !isArtifact && isProse(field);
		if (isChatProse) ++proseFields;
		if (failures == 0) return;
		result.answer_contract.failures += failures;
		const contract::Withheld withheld = contract::withhold(field, checked.violations,
			isArtifact ? contract::Surface::Artifact : contract::Surface::Chat, language);
		if (isChatProse && !isProse(withheld.text)) ++proseFieldsEmptied;
		result.answer_contract.withheld_figures += withheld.figures;
		withheldFields[pointer] = withheld.text;
	};
	if (isStructuredFinal) compute_disclosure::each_field(finalStructure, std::string(), checkField);
	else checkField(std::string(), generation.text);
	result.untraced_figures.count = result.answer_contract.failures;
	const bool isAnswerEmptied = proseFields > 0 && proseFieldsEmptied == proseFields;
	result.answer_contract.answer_emptied = isAnswerEmptied;

	// Structured final results contain escaped JSON strings. Restore their
	// decoded leaves and serialize once, so quotes/backslashes in local values
	// cannot corrupt a valid model action graph during restoration.
	struct FinalRestore {
		protected_api::papinian::RestorationResult restored;
		// A structured answer's strings as the lawyer reads them, with their anchors.
		std::vector<std::pair<std::string, std::vector<protected_api::papinian::FactAnchor>>> pieces;
		std::optional<providers::SafeResponseDiagnostics> diagnostics;
		std::string normalizedLocator = "/";
	};
	const auto restoreFinal = [&](const std::string &providerText) {
		FinalRestore restoring;
		protected_api::papinian::RestorationResult &restored = restoring.restored;
		const auto structured = nlohmann::json::parse(providerText, nullptr, false);
		if (structured.is_object() || structured.is_array()) {
			const bool isTypedTurn = structured.is_object()
				&& structured.contains("controlPlane") && structured.at("controlPlane").is_object()
				&& structured.contains("dataPlane") && structured.at("dataPlane").is_object()
				&& structured.at("dataPlane").contains("payload")
				&& structured.at("dataPlane").at("payload").is_object();
			nlohmann::json restoredStructured = structured;
			JsonRestoreOutcome outcome;
			if (isTypedTurn) {
				outcome = restore_json_strings(guard, structured.at("dataPlane").at("payload"),
					protected_request, "/dataPlane/payload");
				restoredStructured["dataPlane"]["payload"] = outcome.value;
			} else if (turn.response_restoration.arbitrary_structured_value_is_data) {
				outcome = restore_json_strings(guard, structured, protected_request);
				restoredStructured = outcome.value;
			} else {
				const StructuredResponseVariant *matched = nullptr;
				for (const auto &variant : turn.response_restoration.structured_variants) {
					if (!response_matches_schema(structured, variant.schema)) continue;
					if (matched != nullptr)
						throw providers::UnusableGeneration(providers::TaskErrorCode::ProtectedResponseInvalid,
							"The provider response matched more than one typed response variant.",
							generation.usage, generation.finish_reason);
					matched = &variant;
				}
				if (matched == nullptr)
					throw providers::UnusableGeneration(providers::TaskErrorCode::ProtectedResponseInvalid,
						"The provider response did not match the current turn contract before restoration.",
						generation.usage, generation.finish_reason);
				outcome = restore_declared_data_leaves(
					guard, structured, matched->data_plane_shape, protected_request);
				restoredStructured = outcome.value;
			}
			try {
				require_json_structure_unchanged(structured, restoredStructured);
				if (output_contract)
					require_schema_protocol_values_unchanged(structured, restoredStructured, output_contract->json_schema);
			} catch (const std::exception &error) {
				throw providers::UnusableGeneration(providers::TaskErrorCode::ProtectedResponseInvalid,
					error.what(), generation.usage, generation.finish_reason);
			}
			restored.text = restoredStructured.dump();
			restored.integrity_ok = outcome.integrity_ok;
			restored.fact_anchors = protected_api::papinian::anchors_in_order(restored.text, outcome.anchored);
			restoring.pieces = outcome.restored_texts;
			restored.normalized_placeholders = outcome.normalized_candidates;
			restoring.normalizedLocator = outcome.normalized_locator.empty() ? "/" : outcome.normalized_locator;
			if (!outcome.malformed_candidate.empty()) {
				restored.unknown_or_malformed_placeholders.push_back(outcome.malformed_candidate);
				restoring.diagnostics = safe_response_diagnostics(outcome.malformed_candidate,
					outcome.json_locator, protected_request, false);
			}
		} else {
			if (!turn.response_restoration.unstructured_text_is_data)
				throw providers::UnusableGeneration(providers::TaskErrorCode::ProtectedResponseInvalid,
					"The current turn contract does not classify unstructured provider text as response data.",
					generation.usage, generation.finish_reason);
			restored = guard.restore(context::computed_statements::mark_response_operands(providerText, ""), protected_request);
		}
		return restoring;
	};
	std::size_t unissuedInAnswer = 0;
	FinalRestore answerRestore = restoreFinal(
		gap_unissued_in_reply(generation.text, protected_request, activeOperationSymbols, unissuedInAnswer));
	const protected_api::papinian::RestorationResult &restored = answerRestore.restored;
  result.response_integrity_ok = restored.integrity_ok;
  if (!restored.integrity_ok) {
    observability::emit({observability::OperationEventKind::ModelResultKind,
        "Provider response failed restoration integrity; content withheld", {}, std::nullopt});
	const auto diagnostics = answerRestore.diagnostics.value_or(safe_response_diagnostics(
		restored.unknown_or_malformed_placeholders.empty() ? std::string()
			: restored.unknown_or_malformed_placeholders.front(), "/", protected_request, false));
    throw providers::UnusableGeneration(
        providers::TaskErrorCode::ProtectedResponseInvalid,
		"Papinian Guard could not verify the provider's protected symbol"
			+ (restored.unknown_or_malformed_placeholders.empty() ? std::string()
				: " candidate=" + restored.unknown_or_malformed_placeholders.front())
			+ ". The result was not restored.",
        generation.usage, generation.finish_reason, diagnostics);
  }
	if (!restored.normalized_placeholders.empty()) {
		const std::string normalization = restored.normalized_placeholders.front();
		const std::string candidate = normalization.substr(0, normalization.find(" -> "));
		copy_safe_diagnostics(result.privacy_metadata,
			safe_response_diagnostics(candidate, answerRestore.normalizedLocator, protected_request, true));
	}
  result.privacy_metadata.response_integrity = "passed";
	// The answer as the lawyer may read it once no repair is left: every figure
	// the contract could not verify left out (a chat sentence) or shown as a gap
	// (a document field), restored like the rest. Leaving out that would leave
	// no answer at all is not done here: the caller fails the task in plain
	// words instead.
	// When the answer as withheld cannot itself be restored, nothing is: never
	// the answer with its unverifiable figure instead.
	const FinalRestore *chosen = &answerRestore;
	std::optional<FinalRestore> withheldFinal;
	if (!withheldFields.empty() && !isAnswerEmptied) {
		const std::string withheldText = isStructuredFinal
			? compute_disclosure::with_fields(finalStructure, std::string(), withheldFields).dump()
			: withheldFields.begin()->second;
		try {
			withheldFinal = restoreFinal(withheldText);
		} catch (const std::exception &) {
			withheldFinal.reset();
		}
		if (!withheldFinal || !withheldFinal->restored.integrity_ok)
			throw providers::UnusableGeneration(providers::TaskErrorCode::ProtectedResponseInvalid,
				"Papinian Guard could not verify a figure the model wrote. The result was not restored.",
				generation.usage, generation.finish_reason);
		chosen = &*withheldFinal;
		result.answer_contract.text_withheld = true;
		result.answer_contract.withheld_protected_text = withheldText;
	}
  result.text = chosen->restored.text;
	// Where each restored amount and date stands, with the fact it is: what a
	// stored answer keeps so the next turn sends it as the same fact again - and
	// what this operation itself sends again later (restored_writings).
	result.fact_anchors = chosen->restored.fact_anchors;
	if (chosen->pieces.empty())
		protected_api::papinian::record_restored_facts(*activeOperationSymbols, chosen->restored.text, chosen->restored.fact_anchors);
	for (const auto &[text, anchors] : chosen->pieces)
		protected_api::papinian::record_restored_facts(*activeOperationSymbols, text, anchors);
	// The contract's outcome, release-safe: counts and closed codes only.
	std::set<std::string> contractCodes;
	for (const contract::ContractViolation &violation : result.answer_contract.violations) contractCodes.insert(violation.code);
	std::string contractCodeList;
	for (const std::string &code : contractCodes) contractCodeList += (contractCodeList.empty() ? "" : ",") + code;
	// Which symbols failed and where their facts were first met - placeholder
	// names and closed origins only, never a value - so a repair loop can be
	// told apart from a model's mistake.
	std::string contractSymbolList;
	for (const contract::ContractViolation &violation : result.answer_contract.violations) {
		if (violation.soft || violation.symbol.empty()) continue;
		const protected_api::papinian::RequestMappingEntry *row =
			protected_api::papinian::row_of_marker(*activeOperationSymbols, violation.symbol);
		std::string origin = "none";
		if (row != nullptr) {
			origin = protected_api::papinian::to_string(row->provenance.origin);
			if (row->derived) origin += "+derived";
			if (!row->provenance.passage_sha256.empty()) origin += "+passage";
		}
		contractSymbolList += (contractSymbolList.empty() ? "" : ",") + violation.code + ":" + violation.symbol + ":" + origin;
	}
  observability::emit(observability::OperationEvent{observability::OperationEventKind::PapinianRestoreEnd,
    "Papinian: restoration complete", {{"restoredTextBytes", std::to_string(result.text.size())},
		{"answerContractFailures", std::to_string(result.answer_contract.failures)},
		{"answerContractCodes", contractCodeList},
		{"answerContractSymbols", contractSymbolList},
		{"answerContractWithheld", result.answer_contract.text_withheld ? "true" : "false"}},
    observability::has_sink() ? std::optional<std::string>(result.text) : std::nullopt});
  return result;
}


std::vector<std::pair<std::string, bool>>
semantic_detection_parts_for_testing(const std::string &identity, const std::string &value) {
	std::vector<std::pair<std::string, bool>> parts;
	for (const DetectionPart &part : semantic_detection_parts(identity, value))
		parts.emplace_back(part.text, part.user_authored);
	return parts;
}

} // namespace singularity::routing
