#pragma once

#include "ExternalResearch/Types/ExternalResearchTypes.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"

#include <nlohmann/json.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace singularity::research {

void ensure_production_research_tools();

// The production research backend's reason it cannot run for `context` (see
// routing::research_unavailable_reason()), or std::nullopt.
std::optional<std::string> production_research_unavailable_reason(const tools::ToolExecutionContext &context);

inline nlohmann::json external_evidence_to_json(const ExternalEvidenceReference &evidence) {
	using Json = nlohmann::json;
	return Json{
		{"evidenceId", evidence.evidence_id}, {"url", evidence.url},
		{"title", evidence.title ? Json(*evidence.title) : Json(nullptr)},
		{"domain", evidence.domain ? Json(*evidence.domain) : Json(nullptr)},
		{"retrievedAt", evidence.retrieved_at}, {"sourceClass", to_string(evidence.source_class)},
		{"jurisdiction", evidence.jurisdiction ? Json(*evidence.jurisdiction) : Json(nullptr)},
		{"publicationDate", evidence.publication_date ? Json(*evidence.publication_date) : Json(nullptr)},
		{"decisionDate", evidence.decision_date ? Json(*evidence.decision_date) : Json(nullptr)},
		{"caseNumber", evidence.case_number ? Json(*evidence.case_number) : Json(nullptr)},
		{"court", evidence.court ? Json(*evidence.court) : Json(nullptr)},
		{"excerpt", evidence.excerpt}, {"contentHash", evidence.content_hash},
		{"providerSourceId", evidence.provider_source_id ? Json(*evidence.provider_source_id) : Json(nullptr)},
		{"researchProviderId", evidence.research_provider_id ? Json(*evidence.research_provider_id) : Json(nullptr)},
		{"researchEngine", evidence.research_engine ? Json(*evidence.research_engine) : Json(nullptr)},
		{"contentKind", to_string(evidence.content_kind)},
	};
}

inline std::optional<ExternalEvidenceReference> external_evidence_from_json(
		const nlohmann::json &value) {
	if (value.is_object() == false) return std::nullopt;
	const std::optional<ExternalSourceClass> sourceClass = external_source_class_from_string(
		value.value("sourceClass", std::string()));
	if (sourceClass.has_value() == false) return std::nullopt;
	ExternalEvidenceReference evidence;
	evidence.evidence_id = value.value("evidenceId", std::string());
	evidence.url = value.value("url", std::string());
	evidence.retrieved_at = value.value("retrievedAt", std::string());
	evidence.source_class = *sourceClass;
	evidence.excerpt = value.value("excerpt", std::string());
	evidence.content_hash = value.value("contentHash", std::string());
	if (evidence.evidence_id.empty() || evidence.url.empty() || evidence.content_hash.empty()) return std::nullopt;
	auto readOptional = [&](const char *key) -> std::optional<std::string> {
		if (value.contains(key) && value.at(key).is_string() && value.at(key).get<std::string>().empty() == false)
			return value.at(key).get<std::string>();
		return std::nullopt;
	};
	evidence.title = readOptional("title");
	evidence.domain = readOptional("domain");
	evidence.jurisdiction = readOptional("jurisdiction");
	evidence.publication_date = readOptional("publicationDate");
	evidence.decision_date = readOptional("decisionDate");
	evidence.case_number = readOptional("caseNumber");
	evidence.court = readOptional("court");
	evidence.provider_source_id = readOptional("providerSourceId");
	evidence.research_provider_id = readOptional("researchProviderId");
	evidence.research_engine = readOptional("researchEngine");
	if (value.contains("contentKind")) {
		if (!value.at("contentKind").is_string()) return std::nullopt;
		const auto kind = external_evidence_content_kind_from_string(value.at("contentKind").get<std::string>());
		if (!kind) return std::nullopt;
		evidence.content_kind = *kind;
	}
	return evidence;
}

// Unicode mechanics (valid-UTF-8 iteration, canonical normalization, full
// case folding, character-category classification) are infrastructure, not
// Legal Machines proprietary logic - this boundary owns the *privacy
// matching policy* (which values are sensitive, what counts as a boundary,
// how a match gets masked) and delegates every Unicode table/algorithm to
// utf8proc (https://github.com/JuliaStrings/utf8proc, MIT + Unicode data
// license, pinned in deploy/backend/cmake/FetchDeps.cmake).
//
// Normalization form: canonical composition (NFC, i.e. UTF8PROC_COMPOSE)
// plus full Unicode case folding (UTF8PROC_CASEFOLD), deliberately without
// UTF8PROC_COMPAT. Compatibility normalization (NFKC/NFKD) would fold in
// equivalences this boundary does not want - e.g. collapsing typographic
// ligatures, full-width forms, or superscript digits into plain ASCII -
// which broadens what counts as "the same identity" beyond canonical
// equivalence and was never asked for. Canonical equivalence (a precomposed
// "é" and its decomposed "e" + combining acute accent are the same
// character) plus case folding (which - unlike simple 1:1 lower-casing -
// correctly expands e.g. German "ß" to "ss" for caseless comparison) is
// exactly the equivalence a known Project entity's ordinary Unicode
// rendering needs, no broader.
inline utf8proc_category_t category_for_research_query(std::uint32_t code_point) {
	return utf8proc_category(static_cast<utf8proc_int32_t>(code_point));
}

inline bool is_combining_mark_category_for_research_query(utf8proc_category_t category) {
	return category == UTF8PROC_CATEGORY_MN || category == UTF8PROC_CATEGORY_MC
		|| category == UTF8PROC_CATEGORY_ME;
}

// A code point counts as part of a Project entity's identity (never a
// separator) only if it is a letter or a number, in any script - this is
// the Unicode-general replacement for what used to be a hand-maintained
// ASCII+Cyrillic range table. Everything else - space, hyphen, underscore,
// ordinary punctuation, symbols - normalizes as a separator/boundary, per
// requirement 3. Combining marks (category Mn/Mc/Me, e.g. a standalone
// combining accent) are handled separately below: they attach to the
// preceding base character's cluster rather than being classified here.
inline bool is_identity_category_for_research_query(utf8proc_category_t category) {
	switch (category) {
		case UTF8PROC_CATEGORY_LU: case UTF8PROC_CATEGORY_LL: case UTF8PROC_CATEGORY_LT:
		case UTF8PROC_CATEGORY_LM: case UTF8PROC_CATEGORY_LO:
		case UTF8PROC_CATEGORY_ND: case UTF8PROC_CATEGORY_NL: case UTF8PROC_CATEGORY_NO:
			return true;
		default:
			return false;
	}
}

// A cluster is one base code point plus any immediately following combining
// marks (e.g. a base "e" plus a combining acute accent) - the grain at
// which canonical composition/case folding is meaningful, and the smallest
// unit this boundary ever treats as identity-or-separator/masks as a whole.
// Grouping this way (rather than one code point at a time) is what lets a
// precomposed "é" and a combining-mark-decomposed "e"+U+0301 fold to the
// same result below: both become a single cluster whose bytes utf8proc
// normalizes identically. Malformed UTF-8 becomes its own non-identity
// cluster - a safe default, since it can only ever end up a separator, never
// a false match.
struct ResearchQueryCluster {
	std::size_t start = 0;
	std::size_t end = 0; // exclusive, in original bytes
	bool is_identity = false;
};

inline std::vector<ResearchQueryCluster> cluster_research_query_text(const std::string &text) {
	std::vector<ResearchQueryCluster> clusters;
	std::size_t offset = 0;
	while (offset < text.size()) {
		utf8proc_int32_t code_point = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + offset,
			static_cast<utf8proc_ssize_t>(text.size() - offset), &code_point);
		if (consumed <= 0) {
			clusters.push_back(ResearchQueryCluster{offset, offset + 1, false});
			++offset;
			continue;
		}
		const auto length = static_cast<std::size_t>(consumed);
		const bool combining = is_combining_mark_category_for_research_query(
			category_for_research_query(static_cast<std::uint32_t>(code_point)));
		if (combining && clusters.empty() == false) {
			clusters.back().end = offset + length;
		} else {
			const bool identity = combining
				? false
				: is_identity_category_for_research_query(
					category_for_research_query(static_cast<std::uint32_t>(code_point)));
			clusters.push_back(ResearchQueryCluster{offset, offset + length, identity});
		}
		offset += length;
	}
	return clusters;
}

struct ExpandedResearchQueryLetter {
	std::uint32_t value = 0;
	std::size_t cluster_index = 0;
};

// Canonically composes + case-folds one identity cluster's own bytes (never
// more, never less - composition/folding never needs context beyond a
// cluster) and appends the resulting code point(s) to `out`, each tagged
// with the source cluster so a later match can be mapped back to the
// original byte span it came from. A cluster utf8proc cannot map (malformed
// input) contributes no letters - the same safe "can only ever become a
// separator" default as above.
inline void append_expanded_research_query_letters(const std::string &text, std::size_t start, std::size_t end,
		std::size_t cluster_index, std::vector<ExpandedResearchQueryLetter> &out) {
	utf8proc_uint8_t *mapped = nullptr;
	const utf8proc_ssize_t mapped_length = utf8proc_map(
		reinterpret_cast<const utf8proc_uint8_t *>(text.data() + start),
		static_cast<utf8proc_ssize_t>(end - start), &mapped,
		static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
	if (mapped_length < 0 || mapped == nullptr) {
		if (mapped != nullptr) std::free(mapped);
		return;
	}
	std::size_t offset = 0;
	const auto length = static_cast<std::size_t>(mapped_length);
	while (offset < length) {
		utf8proc_int32_t code_point = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(mapped + offset,
			static_cast<utf8proc_ssize_t>(length - offset), &code_point);
		if (consumed <= 0) break;
		out.push_back(ExpandedResearchQueryLetter{static_cast<std::uint32_t>(code_point), cluster_index});
		offset += static_cast<std::size_t>(consumed);
	}
	std::free(mapped);
}

// `letters` is the canonically-composed, case-folded expansion of every
// identity cluster in `clusters`, each entry tagged with the cluster it came
// from - one cluster can expand to more than one letter (German "ß" folds
// to "s","s"). `clusters` holds every cluster, identity and separator alike,
// so boundary checks can see "the cluster immediately before/after a match"
// without ever landing mid-sequence.
struct NormalizedResearchText {
	std::vector<ExpandedResearchQueryLetter> letters;
	std::vector<ResearchQueryCluster> clusters;
};

inline NormalizedResearchText normalize_for_research_query(const std::string &source) {
	NormalizedResearchText result;
	result.clusters = cluster_research_query_text(source);
	result.letters.reserve(result.clusters.size());
	for (std::size_t index = 0; index < result.clusters.size(); ++index) {
		if (result.clusters[index].is_identity == false) continue;
		append_expanded_research_query_letters(source, result.clusters[index].start, result.clusters[index].end,
			index, result.letters);
	}
	return result;
}

// A candidate match only counts if it is not glued to more identity
// clusters on either side - e.g. a sensitive "Orion Holdings" must not mask
// the unrelated longer token "OrionHoldingsGroup". The start/end of the
// text is always a valid boundary.
inline bool is_normalization_boundary_before(const NormalizedResearchText &text, std::size_t clusterIndex) {
	return clusterIndex == 0 || text.clusters[clusterIndex - 1].is_identity == false;
}

inline bool is_normalization_boundary_after(const NormalizedResearchText &text, std::size_t clusterIndex) {
	return clusterIndex + 1 >= text.clusters.size() || text.clusters[clusterIndex + 1].is_identity == false;
}

inline std::string minimize_research_query(const std::string &query,
		const std::vector<std::string> &sensitiveValues) {
	static const std::string kReplacement = "protected project entity";
	static const auto letters_equal = [](const ExpandedResearchQueryLetter &a, const ExpandedResearchQueryLetter &b) {
		return a.value == b.value;
	};
	std::string result = query;
	for (const std::string &sensitive : sensitiveValues) {
		const NormalizedResearchText needle = normalize_for_research_query(sensitive);
		if (needle.letters.size() < 3) continue;
		std::size_t searchFromOriginal = 0;
		while (true) {
			const NormalizedResearchText haystack = normalize_for_research_query(result);
			std::size_t normSearchFrom = 0;
			while (normSearchFrom < haystack.letters.size()
					&& haystack.clusters[haystack.letters[normSearchFrom].cluster_index].start < searchFromOriginal)
				++normSearchFrom;
			if (normSearchFrom + needle.letters.size() > haystack.letters.size()) break;
			const auto matchIt = std::search(haystack.letters.begin() + static_cast<std::ptrdiff_t>(normSearchFrom),
				haystack.letters.end(), needle.letters.begin(), needle.letters.end(), letters_equal);
			if (matchIt == haystack.letters.end()) break;
			const std::size_t normFound = static_cast<std::size_t>(matchIt - haystack.letters.begin());
			const std::size_t firstCluster = haystack.letters[normFound].cluster_index;
			const std::size_t lastCluster = haystack.letters[normFound + needle.letters.size() - 1].cluster_index;
			const std::size_t originalStart = haystack.clusters[firstCluster].start;
			const std::size_t originalEnd = haystack.clusters[lastCluster].end;
			if (is_normalization_boundary_before(haystack, firstCluster)
					&& is_normalization_boundary_after(haystack, lastCluster)) {
				result.replace(originalStart, originalEnd - originalStart, kReplacement);
				searchFromOriginal = originalStart + kReplacement.size();
			} else {
				searchFromOriginal = originalStart + 1;
			}
		}
	}
	return result;
}

// Pure argument-schema validators for web.search/web.open - deliberately
// dependency-light (no adapter, credential or inference-executor includes)
// so acceptance/regression tests can exercise the exact production schema
// ToolBroker::request() enforces without linking the network-execution path
// those tools' execute() handlers pull in. ensure_production_research_tools()
// wires these same functions as each ToolDefinition's validate. Defined
// here, after minimize_research_query() above, so canonicalize_search_arguments()
// just below can call it without a forward declaration.
//
// validate_search() is contract validation only - is this a well-formed
// research-tool request - never a content-size policy. A full forensic
// audit (two live Phase 24.1D acceptance runs) traced the query.size() > 1000
// check this function used to have to an unexplained defensive constant
// introduced verbatim in the original Phase 24.1B commit (46e540e): no
// product/spec rationale, and no corresponding constraint anywhere in
// OpenAiHostedResearchAdapter - the query is sent as the entire `input` of
// an OpenAI Responses API call, not as a bounded search-engine query field.
// The one real downstream capacity boundary is the selected research
// model's context window, which is provider/model-specific and only
// knowable at the ResearchToolLoop layer that actually selects a model -
// not here, and not as a fixed byte count that silently penalizes non-Latin
// scripts (Cyrillic costs ~1.9x the UTF-8 bytes of English for the same
// content) or that privacy minimization can itself push over (a short
// sensitive value expands to the fixed "protected project entity"
// placeholder). See ResearchToolLoop.hpp's admissible_research_input_tokens()
// / capacity check, which now owns that real boundary instead.
//
// A domain a search is kept to (allowedDomains, which the OpenAI adapter
// sends) or kept away from (blockedDomains) is a hostname and nothing else
// (is_research_hostname() below): a sentence, a path or an address written
// there would leave as a "domain" no minimization ever read.
inline bool is_research_hostname(const std::string &value) {
	if (value.empty() || value.size() > 253) return false;
	std::size_t labels = 0;
	std::string last;
	std::size_t start = 0;
	for (;;) {
		const std::size_t dot = value.find('.', start);
		const std::string label = value.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
		if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
		for (const char character : label)
			if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
					|| (character >= '0' && character <= '9') || character == '-'))
				return false;
		++labels;
		last = label;
		if (dot == std::string::npos) break;
		start = dot + 1;
	}
	// A name, never an address: two labels at least, and a top-level label
	// that is not a number ("127.0.0.1" is not a domain).
	return labels >= 2 && std::any_of(last.begin(), last.end(), [](char character) {
		return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
	});
}

inline std::optional<std::string> validate_search(
		const tools::ToolExecutionContext &, const nlohmann::json &arguments) {
	if (arguments.is_object() == false || arguments.size() > 8) return "invalid_arguments";
	static const std::unordered_set<std::string> allowed{
		"query", "jurisdiction", "freshnessRequired", "maximumToolCalls",
		"sourcePreferences", "allowedDomains", "blockedDomains",
	};
	for (const auto &entry : arguments.items())
		if (allowed.count(entry.key()) == 0) return "invalid_arguments";
	// Three distinct predicates, three distinct categories - a live Phase
	// 24.1D acceptance run proved a single combined category is not enough
	// to diagnose a real rejection after the fact, and raw arguments are
	// deliberately never persisted anywhere outside an isolated acceptance
	// artifact. This changes nothing about which requests pass or fail, only
	// which reason a rejection reports.
	if (arguments.contains("query") == false) return "invalid_query_missing";
	if (arguments.at("query").is_string() == false) return "invalid_query_type";
	const std::string &query = arguments.at("query").get_ref<const std::string &>();
	if (query.empty() == true) return "invalid_query_empty";
	if (arguments.contains("jurisdiction") && arguments.at("jurisdiction").is_null() == false
		&& arguments.at("jurisdiction").is_string() == false) return "invalid_arguments";
	if (arguments.contains("freshnessRequired") && arguments.at("freshnessRequired").is_null() == false
		&& arguments.at("freshnessRequired").is_boolean() == false) return "invalid_arguments";
	if (arguments.contains("maximumToolCalls")
		&& (arguments.at("maximumToolCalls").is_number_unsigned() == false
			|| arguments.at("maximumToolCalls").get<std::size_t>() < 1)) return "invalid_arguments";
	for (const char *key : {"sourcePreferences", "allowedDomains", "blockedDomains"}) {
		if (arguments.contains(key) == false) continue;
		if (arguments.at(key).is_array() == false || arguments.at(key).size() > 20) return "invalid_arguments";
		for (const nlohmann::json &entry : arguments.at(key))
			if (entry.is_string() == false || entry.get<std::string>().size() > 253) return "invalid_arguments";
	}
	for (const char *key : {"allowedDomains", "blockedDomains"}) {
		if (arguments.contains(key) == false) continue;
		for (const nlohmann::json &entry : arguments.at(key))
			if (is_research_hostname(entry.get<std::string>()) == false) return "invalid_domain";
	}
	return std::nullopt;
}

// Every free-text string a web search can send - its query, its jurisdiction,
// its source preferences - and every domain it names, as the arguments hold
// them: what minimization, the last check before sending and a local model's
// Papinian check each read (never a field another reader forgets).
inline std::vector<std::string> research_outbound_texts(const nlohmann::json &arguments) {
	std::vector<std::string> texts;
	if (arguments.is_object() == false) return texts;
	for (const char *key : {"query", "jurisdiction"})
		if (arguments.contains(key) && arguments.at(key).is_string()) texts.push_back(arguments.at(key).get<std::string>());
	if (arguments.contains("sourcePreferences") && arguments.at("sourcePreferences").is_array())
		for (const nlohmann::json &entry : arguments.at("sourcePreferences"))
			if (entry.is_string()) texts.push_back(entry.get<std::string>());
	return texts;
}

inline std::vector<std::string> research_outbound_domains(const nlohmann::json &arguments) {
	std::vector<std::string> domains;
	if (arguments.is_object() == false) return domains;
	for (const char *key : {"allowedDomains", "blockedDomains"})
		if (arguments.contains(key) && arguments.at(key).is_array())
			for (const nlohmann::json &entry : arguments.at(key))
				if (entry.is_string()) domains.push_back(entry.get<std::string>());
	return domains;
}

inline std::optional<std::string> optional_string(const nlohmann::json &value, const char *key) {
	if (value.contains(key) && value.at(key).is_string() && value.at(key).get<std::string>().empty() == false)
		return value.at(key).get<std::string>();
	return std::nullopt;
}

// The real ResearchPolicy an accepted web.search call's arguments map to -
// kept here, inline and header-only like every other function on this page,
// rather than in ResearchTools.cpp where its only production caller
// (execute_search()) lives, precisely so a focused unit test can prove this
// pure argument mapping is null-safe without linking production_adapter()'s
// real connected-provider/credential path (see this file's own top-of-file
// comment on validate_search()). nlohmann::json's value() only substitutes a
// default when a key is MISSING, not when it is present and null, so a naive
// `arguments.value("freshnessRequired", false)` throws a json::type_error on
// the exact null web_search_model_tool_parameters_schema() above declares
// legal for a required-but-nullable field - a live Phase 24.1D
// OpenRouter/GPT-5.1 acceptance run proved a strict-tool-calling model sends
// exactly that null once validate_search() itself stops rejecting it.
inline ResearchPolicy policy_from_arguments(const nlohmann::json &arguments) {
	ResearchPolicy policy;
	policy.jurisdiction = optional_string(arguments, "jurisdiction");
	policy.freshness_required = arguments.contains("freshnessRequired")
			&& arguments.at("freshnessRequired").is_boolean()
		? arguments.at("freshnessRequired").get<bool>() : false;
	policy.maximum_tool_calls = std::min<std::size_t>(arguments.value("maximumToolCalls", 4U), 4U);
	for (const std::pair<const char *, std::vector<std::string> ResearchPolicy::*> &field : {
		std::make_pair("sourcePreferences", &ResearchPolicy::source_preferences),
		std::make_pair("allowedDomains", &ResearchPolicy::allowed_domains),
		std::make_pair("blockedDomains", &ResearchPolicy::blocked_domains),
	}) {
		if (arguments.contains(field.first) == false || arguments.at(field.first).is_array() == false) continue;
		for (const nlohmann::json &entry : arguments.at(field.first))
			if (entry.is_string()) (policy.*field.second).push_back(entry.get<std::string>());
	}
	return policy;
}

// What will leave this device if the lawyer allows a research step, word for
// word - the approval card prints it as it is. A search sends its query and,
// through OpenRouter, its jurisdiction; through OpenAI, the sites it is kept
// to. A page is asked for by its address through OpenRouter; through OpenAI it
// was already read with the search, and opening it sends nothing.
inline nlohmann::json research_search_outbound(const nlohmann::json &arguments, bool isOpenRouter) {
	nlohmann::json outbound{
		{"kind", "web.search"},
		{"destination", isOpenRouter == true ? "OpenRouter" : "OpenAI"},
		{"leaves", true},
		{"query", arguments.at("query").get<std::string>()},
	};
	const ResearchPolicy policy = policy_from_arguments(arguments);
	if (isOpenRouter == true && policy.jurisdiction.has_value()) outbound["jurisdiction"] = *policy.jurisdiction;
	if (isOpenRouter == false && policy.allowed_domains.empty() == false) outbound["allowedDomains"] = policy.allowed_domains;
	return outbound;
}

inline nlohmann::json research_open_outbound(const std::string &url, bool isOpenRouter) {
	return nlohmann::json{
		{"kind", "web.open"},
		{"destination", isOpenRouter == true ? "OpenRouter" : "OpenAI"},
		{"leaves", isOpenRouter},
		{"url", url},
	};
}

inline std::optional<std::string> validate_open(
		const tools::ToolExecutionContext &, const nlohmann::json &arguments) {
	if (arguments.is_object() == false || arguments.size() != 1
		|| arguments.contains("sourceId") == false || arguments.at("sourceId").is_string() == false)
		return "invalid_source_id";
	const std::string value = arguments.at("sourceId").get<std::string>();
	if (value.rfind("ext_", 0) != 0 || value.size() != 24) return "invalid_source_id";
	return std::nullopt;
}

// The one source of truth for what a model is offered as web.search/web.open
// tool inputs - a provider-facing wire name plus an exact JSON Schema of
// MODEL-OWNED SEMANTIC INPUTS ONLY. Deliberately narrower than what
// validate_search() above accepts: `maximumToolCalls`, `sourcePreferences`,
// `allowedDomains`, `blockedDomains` are Singularity policy/budget fields
// validate_search() tolerates from any caller (including the legacy textual
// fallback protocol), never model intent to be solicited here - a model may
// never propose its own tool-call budget. `query`/`jurisdiction`/
// `freshnessRequired` are exactly the fields ResearchTools.cpp's
// policy_from_arguments() and execute_search() already read from a
// web.search call; nothing here invents a field validate_search() would
// reject, and nothing validate_search() accepts is exposed merely because it
// is tolerated. `additionalProperties: false` plus every property listed in
// `required` (with `jurisdiction`/`freshnessRequired` typed nullable rather
// than omitted) matches the portable "strict" JSON Schema style several
// providers require for schema-enforced tool arguments.
inline const nlohmann::json &web_search_model_tool_parameters_schema() {
	static const nlohmann::json schema{
		{"type", "object"},
		{"properties", nlohmann::json{
			{"query", nlohmann::json{
				{"type", "string"},
				{"minLength", 1},
				{"description", "A privacy-minimized issue-level public legal research query."},
			}},
			{"jurisdiction", nlohmann::json{
				{"type", nlohmann::json::array({"string", "null"})},
				{"description", "Jurisdiction when relevant, otherwise null."},
			}},
			{"freshnessRequired", nlohmann::json{
				{"type", nlohmann::json::array({"boolean", "null"})},
				{"description", "Whether current information is required, otherwise null."},
			}},
		}},
		{"required", nlohmann::json::array({"query", "jurisdiction", "freshnessRequired"})},
		{"additionalProperties", false},
	};
	return schema;
}

// The existing opaque sourceId / SSRF boundary (see validate_open() above)
// remains the only thing a model may ever address with web.open - never an
// arbitrary URL. `sourceId` is the only field this schema offers.
inline const nlohmann::json &web_open_model_tool_parameters_schema() {
	static const nlohmann::json schema{
		{"type", "object"},
		{"properties", nlohmann::json{
			{"sourceId", nlohmann::json{
				{"type", "string"},
				{"description", "Opaque Singularity external evidence source ID returned by web search."},
			}},
		}},
		{"required", nlohmann::json::array({"sourceId"})},
		{"additionalProperties", false},
	};
	return schema;
}

inline providers::ModelToolDefinition web_search_model_tool_definition() {
	return providers::ModelToolDefinition{
		"lm_web_search",
		"Search the public web for a legal-research issue. Returns opaque evidence source IDs, "
		"never full page content.",
		web_search_model_tool_parameters_schema(),
		nlohmann::json{{"properties", {{"query", true}, {"jurisdiction", true}}}},
	};
}

inline providers::ModelToolDefinition web_open_model_tool_definition() {
	return providers::ModelToolDefinition{
		"lm_web_open",
		"Open exactly one opaque external evidence source previously returned by lm_web_search.",
		web_open_model_tool_parameters_schema(),
		nlohmann::json::object(),
	};
}

// The one bidirectional mapping between Singularity's canonical ToolBroker
// capability ids ("web.search"/"web.open" - unchanged, never renamed) and
// the provider-facing wire names a model actually sees. Wire names are
// transport syntax, never the canonical id itself - see
// web_search_model_tool_definition() above, which is the only place a
// canonical id is translated to a wire name for a provider request. An
// unrecognized wire name returned by a provider must fail closed (see
// ResearchToolLoop.hpp's native-tool-call normalization) rather than being
// invented a mapping on the spot.
inline const std::vector<std::pair<std::string, std::string>> &research_model_tool_wire_names() {
	static const std::vector<std::pair<std::string, std::string>> mapping{
		{"web.search", "lm_web_search"},
		{"web.open", "lm_web_open"},
	};
	return mapping;
}

inline std::optional<std::string> wire_name_for_canonical_research_tool(const std::string &canonicalToolId) {
	for (const auto &entry : research_model_tool_wire_names())
		if (entry.first == canonicalToolId) return entry.second;
	return std::nullopt;
}

inline std::optional<std::string> canonical_research_tool_for_wire_name(const std::string &wireName) {
	for (const auto &entry : research_model_tool_wire_names())
		if (entry.second == wireName) return entry.first;
	return std::nullopt;
}

// Return value of canonicalize_search_arguments() below: the canonical
// arguments ToolBroker should actually validate/execute, plus privacy-safe
// diagnostics (raw UTF-8 byte sizes and a changed flag - never argument
// content) for durable ToolCallEvidence. Byte counts only - this makes no
// claim about what unit production validation should use; see
// validate_search()'s own comment on the still-unresolved 1000-byte bound.
// All three diagnostic fields stay nullopt when there was no valid query to
// canonicalize in the first place (missing/non-string/empty), matching
// validate_search()'s own untouched pass-through.
struct CanonicalSearchArguments {
	nlohmann::json arguments;
	std::optional<std::size_t> raw_query_utf8_bytes;
	std::optional<std::size_t> canonical_query_utf8_bytes;
	std::optional<bool> intent_canonicalized;
};

// LM-owned canonicalization for a model's raw web.search intent - run once,
// identically, for every provider and every model (local or external),
// before ToolBroker ever validates or executes the request, and before
// ResearchToolLoop.hpp's own capacity check (see
// admissible_research_input_tokens()) measures what actually gets sent.
// Only ever narrows/normalizes what is already semantically unambiguous - a
// missing/non-string/empty query is returned untouched, exactly as the model
// sent it, for validate_search() to fail closed on precisely as before. This
// never truncates at an arbitrary character boundary (which could change
// legal meaning); the only transform applied is the existing, already-audited
// privacy substitution - this function only changes what text downstream
// checks see (and reports how much it changed it), never what they accept.
// Every value a research request must not disclose: the Project's own
// sensitive values plus this operation's protected originals (see
// ToolExecutionContext::operation_protected_values).
inline std::vector<std::string> research_sensitive_values(const tools::ToolExecutionContext &context) {
	std::vector<std::string> values = context.project_sensitive_values;
	if (context.operation_protected_values) {
		const std::vector<std::string> operationValues = context.operation_protected_values();
		values.insert(values.end(), operationValues.begin(), operationValues.end());
	}
	return values;
}

// A Papinian placeholder ("[[E1]]") left in a model-authored research field
// names protected data by construction; it never leaves as a query term.
inline std::string without_protected_placeholders(const std::string &text) {
	static const std::regex placeholder(R"(\[\[[^\[\]\r\n]{1,64}\]\])");
	return std::regex_replace(text, placeholder, "protected project entity");
}

inline CanonicalSearchArguments canonicalize_search_arguments(
		const nlohmann::json &rawArguments, const std::vector<std::string> &projectSensitiveValues) {
	if (rawArguments.is_object() == false || rawArguments.contains("query") == false
			|| rawArguments.at("query").is_string() == false
			|| rawArguments.at("query").get<std::string>().empty() == true)
		return CanonicalSearchArguments{rawArguments, std::nullopt, std::nullopt, std::nullopt};
	const std::string rawQuery = rawArguments.at("query").get<std::string>();
	const std::string minimizedQuery =
		minimize_research_query(without_protected_placeholders(rawQuery), projectSensitiveValues);
	CanonicalSearchArguments result;
	result.arguments = rawArguments;
	result.arguments["query"] = minimizedQuery;
	// Every model-authored free-text field that leaves the machine is
	// minimized the same way - jurisdiction is sent verbatim to the research
	// adapter's own worker prompt, so it is exactly as exposed as the query.
	if (result.arguments.contains("jurisdiction") && result.arguments.at("jurisdiction").is_string())
		result.arguments["jurisdiction"] = minimize_research_query(
			without_protected_placeholders(result.arguments.at("jurisdiction").get<std::string>()), projectSensitiveValues);
	// Source preferences are free text like the jurisdiction; a domain keeps
	// only its own shape - a hostname that names nothing the Project protects
	// stays, anything else is dropped (the search is then only less narrowed).
	if (result.arguments.contains("sourcePreferences") && result.arguments.at("sourcePreferences").is_array())
		for (nlohmann::json &entry : result.arguments["sourcePreferences"])
			if (entry.is_string())
				entry = minimize_research_query(without_protected_placeholders(entry.get<std::string>()), projectSensitiveValues);
	for (const char *key : {"allowedDomains", "blockedDomains"}) {
		if (result.arguments.contains(key) == false || result.arguments.at(key).is_array() == false) continue;
		nlohmann::json kept = nlohmann::json::array();
		for (const nlohmann::json &entry : result.arguments.at(key))
			if (entry.is_string() && is_research_hostname(entry.get<std::string>())
					&& minimize_research_query(entry.get<std::string>(), projectSensitiveValues) == entry.get<std::string>())
				kept.push_back(entry);
		if (kept.empty()) result.arguments.erase(key);
		else result.arguments[key] = std::move(kept);
	}
	result.raw_query_utf8_bytes = rawQuery.size();
	result.canonical_query_utf8_bytes = minimizedQuery.size();
	result.intent_canonicalized = minimizedQuery != rawQuery;
	return result;
}

// How the web research running on this thread was written - set by
// routing::execute() for the run of its research loop (ScopedResearchCall-
// Protection), read by the research tools the moment a call is about to
// leave, on the same thread (ToolBroker runs a tool, and a tool approved
// while the loop waits, on the loop's own thread). `mode` is the protection
// the call is held to, and what its outbound record says. A model that read
// the Matter's own values - a model on this computer, whose turns Papinian
// never transforms - wrote it when `written_from_protected_text` is false:
// under a mode that protects, such a search then leaves only as Papinian
// Guard passed it, by its exact arguments (`passed`, filled by the loop's
// check). Process memory only; one thread.
struct ResearchCallProtection {
	protected_api::Mode mode = protected_api::Mode::Off;
	bool written_from_protected_text = false;
	std::set<std::string> passed;
};

inline std::shared_ptr<ResearchCallProtection> &research_call_protection_slot() {
	thread_local std::shared_ptr<ResearchCallProtection> slot;
	return slot;
}

inline std::shared_ptr<ResearchCallProtection> current_research_call_protection() {
	return research_call_protection_slot();
}

class ScopedResearchCallProtection {
public:
	explicit ScopedResearchCallProtection(std::shared_ptr<ResearchCallProtection> protection)
		: previous_(research_call_protection_slot()) {
		research_call_protection_slot() = std::move(protection);
	}
	~ScopedResearchCallProtection() { research_call_protection_slot() = previous_; }
	ScopedResearchCallProtection(const ScopedResearchCallProtection &) = delete;
	ScopedResearchCallProtection &operator=(const ScopedResearchCallProtection &) = delete;

private:
	std::shared_ptr<ResearchCallProtection> previous_;
};

// The request one web search sends, prepared at the last moment before the
// research adapter posts it (ResearchTools.cpp's execute_search()), never
// trusting what came before it for a privacy boundary: every free-text string
// it sends carries none of this Project's sensitive values or this
// operation's protected originals (canonicalize_search_arguments() already
// took them out - anything it would still change was never minimized); every
// domain is a hostname that names none of them; and a search a model wrote
// from the Matter's own values leaves only as Papinian Guard passed it.
// Throws, in words the lawyer reads, when any of it does not hold: nothing is
// sent.
inline ResearchRequest prepare_outbound_research_request(const tools::ToolExecutionContext &context,
		const nlohmann::json &arguments, const ResearchCallProtection *protection) {
	if (protection == nullptr)
		throw std::runtime_error("The web search was not run: Singularity could not tell how it was written, so "
			"nothing was sent.");
	if (protection->written_from_protected_text == false && protection->mode != protected_api::Mode::Off
			&& protection->passed.count(arguments.dump()) == 0)
		throw std::runtime_error("The web search was not run: Papinian Guard had not checked it, so nothing was sent.");
	const std::vector<std::string> sensitiveValues = research_sensitive_values(context);
	for (const std::string &text : research_outbound_texts(arguments))
		if (minimize_research_query(without_protected_placeholders(text), sensitiveValues) != text)
			throw std::runtime_error("Papinian Guard could not safely prepare the research query.");
	for (const std::string &domain : research_outbound_domains(arguments))
		if (is_research_hostname(domain) == false || minimize_research_query(domain, sensitiveValues) != domain)
			throw std::runtime_error("Papinian Guard could not safely prepare the research query.");
	ResearchRequest request;
	request.query = arguments.at("query").get<std::string>();
	request.policy = policy_from_arguments(arguments);
	return request;
}

struct ExternalCitationProjection {
	std::string plain_text;
	std::vector<ExternalEvidenceReference> citations;
	std::size_t unresolved_count = 0;
};

inline std::vector<ExternalEvidenceReference> cited_external_evidence(const std::string &response,
		const std::vector<ExternalEvidenceReference> &supplied) {
	std::vector<ExternalEvidenceReference> result;
	for (const ExternalEvidenceReference &evidence : supplied) {
		if (response.find(evidence.marker()) == std::string::npos) continue;
		if (std::find_if(result.begin(), result.end(), [&](const ExternalEvidenceReference &entry) {
			return entry.evidence_id == evidence.evidence_id;
		}) == result.end()) result.push_back(evidence);
	}
	return result;
}

inline ExternalCitationProjection project_external_citations(const std::string &response,
		const std::vector<ExternalEvidenceReference> &cited) {
	ExternalCitationProjection result;
	std::unordered_map<std::string, const ExternalEvidenceReference *> byMarker;
	for (const ExternalEvidenceReference &evidence : cited) byMarker[evidence.marker()] = &evidence;
	std::unordered_map<std::string, std::size_t> projectedIndex;
	std::size_t position = 0;
	while (position < response.size()) {
		const std::size_t start = response.find("[EXTERNAL:", position);
		if (start == std::string::npos) {
			result.plain_text += response.substr(position);
			break;
		}
		result.plain_text += response.substr(position, start - position);
		const std::size_t end = response.find(']', start);
		if (end == std::string::npos || end - start > 64) {
			++result.unresolved_count;
			position = end == std::string::npos ? response.size() : end + 1;
			continue;
		}
		const std::string marker = response.substr(start, end - start + 1);
		const auto found = byMarker.find(marker);
		if (found == byMarker.end()) {
			++result.unresolved_count;
		} else {
			auto projected = projectedIndex.find(marker);
			if (projected == projectedIndex.end()) {
				const std::size_t index = result.citations.size();
				result.citations.push_back(*found->second);
				projected = projectedIndex.emplace(marker, index).first;
			}
			result.plain_text += "\x1e" + std::to_string(projected->second) + "\x1f";
		}
		position = end + 1;
	}
	return result;
}

} // namespace singularity::research
