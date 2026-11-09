#include "ExternalResearch/ExternalResearchAdapter/ExternalResearchAdapter.hpp"
#include "ExternalResearch/ResearchTools/ResearchTools.hpp"
#include "Routing/Observability/OperationEvent.hpp"

#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <stdexcept>

namespace singularity::research {
namespace {

using Json = nlohmann::json;

// A real web_search call with tool_choice required and up to
// max_tool_calls searches can legitimately run past a connectivity-check-
// scale timeout while the provider performs several real searches server
// side before returning anything - a 60s cap was observed failing a
// genuine multi-search legal research turn.
constexpr int kResearchTimeoutSeconds = 300;

// The usage a hosted research response reported, in either wire dialect: the
// Responses API's input/output token counts, or a chat completion's prompt/
// completion counts with OpenRouter's bare cost (always USD, provider-reported
// - the same reading ProviderRegistry.cpp applies to reasoning calls). A count
// the response omits stays unset; a hosted search tool's per-call fee that the
// response does not report is never guessed.
providers::UsageMetadata reported_research_usage(const Json &payload) {
	providers::UsageMetadata usage;
	if (payload.is_object() == false || payload.contains("usage") == false || payload.at("usage").is_object() == false)
		return usage;
	const Json &reported = payload.at("usage");
	const auto count = [&](const char *key) -> std::optional<long long> {
		if (reported.contains(key) == false || reported.at(key).is_number_integer() == false) return std::nullopt;
		const long long value = reported.at(key).get<long long>();
		return value >= 0 ? std::optional<long long>(value) : std::nullopt;
	};
	usage.input_tokens = count("input_tokens");
	if (usage.input_tokens.has_value() == false) usage.input_tokens = count("prompt_tokens");
	usage.output_tokens = count("output_tokens");
	if (usage.output_tokens.has_value() == false) usage.output_tokens = count("completion_tokens");
	usage.total_tokens = count("total_tokens");
	if (reported.contains("cost") && reported.at("cost").is_number()) {
		const double amount = reported.at("cost").get<double>();
		if (std::isfinite(amount) && amount >= 0.0 && amount <= 9.0e12) {
			usage.cost_micros = std::llround(amount * 1'000'000.0);
			usage.cost_currency = "USD";
			usage.cost_classification = "actual";
			usage.cost_methodology = "provider_reported";
		}
	}
	return usage;
}

std::mutex &cache_mutex() {
	static std::mutex value;
	return value;
}

std::map<std::string, ExternalEvidenceReference> &source_cache() {
	static std::map<std::string, ExternalEvidenceReference> value;
	return value;
}

std::optional<std::string> host_from_url(const std::string &url) {
	const std::size_t schemeEnd = url.find("://");
	if (schemeEnd == std::string::npos) return std::nullopt;
	const std::size_t hostStart = schemeEnd + 3;
	const std::size_t hostEnd = url.find_first_of("/:?#", hostStart);
	std::string host = url.substr(hostStart, hostEnd - hostStart);
	std::transform(host.begin(), host.end(), host.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	if (host.empty() == true) return std::nullopt;
	return host;
}

bool safe_public_https_url(const std::string &url) noexcept {
	if (url.rfind("https://", 0) != 0) return false;
	const std::optional<std::string> host = host_from_url(url);
	if (host.has_value() == false) return false;
	if (*host == "localhost" || host->size() > 255 || host->find('@') != std::string::npos
		|| host->find('%') != std::string::npos
		|| host->find('[') != std::string::npos || host->find(']') != std::string::npos) return false;
	if (std::all_of(host->begin(), host->end(), [](unsigned char value) { return std::isdigit(value) != 0; }))
		return false;
	for (const std::string &suffix : {".localhost", ".local", ".internal", ".home", ".lan"})
		if (host->size() >= suffix.size()
			&& host->compare(host->size() - suffix.size(), suffix.size(), suffix) == 0) return false;
	unsigned int octets[4] = {};
	char trailing = '\0';
	if (std::sscanf(host->c_str(), "%u.%u.%u.%u%c", &octets[0], &octets[1], &octets[2], &octets[3],
			&trailing) == 4) {
		for (unsigned int octet : octets) if (octet > 255) return false;
		const unsigned int first = octets[0];
		const unsigned int second = octets[1];
		if (first == 0 || first == 10 || first == 127 || first >= 224
			|| (first == 100 && second >= 64 && second <= 127)
			|| (first == 169 && second == 254) || (first == 172 && second >= 16 && second <= 31)
			|| (first == 192 && (second == 0 || second == 168))
			|| (first == 198 && (second == 18 || second == 19))) return false;
	}
	return true;
}

ExternalSourceClass classify_source(const std::string &domain) {
	if (domain == "pravo.gov.ru" || domain == "publication.pravo.gov.ru")
		return ExternalSourceClass::OfficialLegislation;
	if (domain == "vsrf.ru" || domain.find("sudrf.ru") != std::string::npos)
		return ExternalSourceClass::CourtDecision;
	if (domain.size() >= 4 && domain.substr(domain.size() - 4) == ".gov")
		return ExternalSourceClass::Government;
	if (domain.size() >= 7 && domain.substr(domain.size() - 7) == ".gov.ru")
		return ExternalSourceClass::Government;
	return ExternalSourceClass::GeneralWeb;
}

// A UTF-8 continuation byte always has the top bits 10xxxxxx. Used as a
// last-resort defensive floor below - genuinely malformed input (utf8proc
// cannot decode further) must still never slice mid-character.
std::size_t clamp_to_utf8_lead(const std::string &text, std::size_t index) {
	while (index > 0 && index < text.size()
			&& (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
		--index;
	}
	return index;
}

// UNVERIFIED provider contract - do not treat as proven. The Responses API
// documents start_index/end_index as character offsets into the output text
// but does not publish a code-unit convention, no captured or live payload
// has been available to determine one (this codebase never scrapes/re-fetches
// a page or logs a raw provider body - see the "no scraping" web policy - and
// no prior live run recorded the raw response bytes anywhere), and no further
// paid call may be spent to resolve it. Code-point offsets are used below as
// the best-reasoned placeholder (a Python-authored API's native string
// indexing never distinguishes a surrogate-pair-width character from any
// other), NOT as an established fact - ExternalResearch.test.cpp's own
// non-BMP case is implementation coverage pinning this exact choice against
// the UTF-16 and UTF-8-byte alternatives, not provider-contract evidence.
// Walked forward through real decoded code points (utf8proc, never a
// hand-rolled decode table), one offset step per code point.
std::size_t codepoint_offset_to_utf8_byte_offset(const std::string &text, std::size_t codepointOffset) {
	std::size_t byteOffset = 0;
	std::size_t codepoints = 0;
	while (byteOffset < text.size() && codepoints < codepointOffset) {
		utf8proc_int32_t codepoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + byteOffset,
			static_cast<utf8proc_ssize_t>(text.size() - byteOffset), &codepoint);
		if (consumed <= 0) return clamp_to_utf8_lead(text, byteOffset);
		++codepoints;
		byteOffset += static_cast<std::size_t>(consumed);
	}
	return byteOffset;
}

// UNVERIFIED provider contract, same caveat as above. Treated as INCLUSIVE
// (end_index names the last cited character, per the vendor SDK's own
// generated description - "the index of the last character" - rather than
// assumed to be a Python-slice-style exclusive bound). See
// ExternalResearch.test.cpp's dedicated single-character boundary case,
// which fails under the exclusive interpretation and would catch a silent
// regression to it.
std::string bounded_excerpt(const std::string &text, std::size_t codepointStart, std::size_t codepointEndInclusive) {
	const std::size_t start = codepoint_offset_to_utf8_byte_offset(text, codepointStart);
	if (start >= text.size()) return "";
	std::size_t end = codepoint_offset_to_utf8_byte_offset(text, codepointEndInclusive + 1);
	const std::size_t maximum = 700;
	if (end - start > maximum) end = start + maximum;
	end = clamp_to_utf8_lead(text, std::min(end, text.size()));
	if (end <= start) return "";
	return text.substr(start, end - start);
}

} // namespace

bool is_safe_public_research_url(const std::string &url) noexcept {
	return safe_public_https_url(url);
}

OpenAiHostedResearchAdapter::OpenAiHostedResearchAdapter(std::string apiKey, std::string modelId,
		HostedResearchPost post)
	: api_key_(std::move(apiKey))
	, model_id_(std::move(modelId))
	, post_(std::move(post))
{
	if (post_ == nullptr) post_ = platform::https_post;
}

ResearchResponse OpenAiHostedResearchAdapter::search(const ResearchRequest &request) {
	if (request.policy.cancellation != nullptr && request.policy.cancellation->load() == true)
		throw std::runtime_error("Research was stopped.");
	Json tool{{"type", "web_search"}};
	// Only a hostname is ever sent as a domain (is_research_hostname()): what
	// reached here as anything else - a sentence, a path - stays here, and the
	// search is only less narrowed.
	std::vector<std::string> allowedDomains;
	for (const std::string &domain : request.policy.allowed_domains)
		if (is_research_hostname(domain)) allowedDomains.push_back(domain);
	if (allowedDomains.empty() == false) {
		tool["filters"] = Json{{"allowed_domains", allowedDomains}};
	}
	const Json body{
		{"model", model_id_},
		{"input", request.query},
		{"tools", Json::array({tool})},
		{"tool_choice", "required"},
		{"max_tool_calls", std::min<std::size_t>(request.policy.maximum_tool_calls, 4)},
		{"include", Json::array({"web_search_call.action.sources"})},
		{"store", false},
	};
	routing::observability::emit({routing::observability::OperationEventKind::ProviderSendStart,
		"Sending approved research adapter request", {{"bodyBytes", std::to_string(body.dump().size())}},
		routing::observability::has_sink() ? std::optional<std::string>(body.dump()) : std::nullopt});
	const platform::HttpResponse response = post_(
		"https://api.openai.com/v1/responses",
		{{"Authorization", "Bearer " + api_key_}, {"Content-Type", "application/json"}}, body.dump(),
		kResearchTimeoutSeconds);
	if (!api_key_.empty() && response.body.find(api_key_) != std::string::npos)
		throw std::runtime_error("The research provider returned an unsafe credential echo.");
	routing::observability::emit({routing::observability::OperationEventKind::ProviderResponseReceived,
		"Research adapter HTTP response received", {{"status", std::to_string(response.status)}},
		routing::observability::has_sink() ? std::optional<std::string>(response.body) : std::nullopt});
	if (response.status == 429) throw std::runtime_error("Research is rate-limited. Wait and try again.");
	if (response.status < 200 || response.status >= 300)
		throw std::runtime_error("The configured research provider could not complete web search (HTTP "
			+ std::to_string(response.status) + ").");
	const Json payload = Json::parse(response.body, nullptr, false);
	if (payload.is_discarded())
		throw std::runtime_error("The research provider returned an unexpected response.");
	if (payload.contains("output") == false || payload.at("output").is_array() == false)
		throw BilledResearchFailure("The research provider returned an unexpected response.", "openai", model_id_,
			reported_research_usage(payload));

	ResearchResponse result;
	result.provider_id = "openai";
	result.model_id = model_id_;
	result.usage = reported_research_usage(payload);
	std::vector<std::pair<std::string, std::optional<std::string>>> reportedSources;
	for (const Json &item : payload.at("output")) {
		if (item.value("type", std::string()) == "web_search_call" && item.contains("action")
			&& item.at("action").is_object() && item.at("action").contains("sources")
			&& item.at("action").at("sources").is_array()) {
			for (const Json &source : item.at("action").at("sources")) {
				if (source.is_object() == false || source.value("url", std::string()).empty()) continue;
				const std::string title = source.value("title", std::string());
				reportedSources.emplace_back(source.at("url").get<std::string>(),
					title.empty() ? std::nullopt : std::optional<std::string>(title));
			}
		}
		if (item.value("type", std::string()) != "message" || item.contains("content") == false) continue;
		for (const Json &content : item.at("content")) {
			if (content.value("type", std::string()) != "output_text") continue;
			const std::string text = content.value("text", std::string());
			result.summary += text;
			if (content.contains("annotations") == false || content.at("annotations").is_array() == false) continue;
			for (const Json &annotation : content.at("annotations")) {
				if (annotation.value("type", std::string()) != "url_citation") continue;
				const Json &citation = annotation.contains("url_citation")
					&& annotation.at("url_citation").is_object() ? annotation.at("url_citation") : annotation;
				const std::string url = citation.value("url", std::string());
				if (safe_public_https_url(url) == false) continue;
				const std::optional<std::string> domain = host_from_url(url);
				if (domain.has_value() == false) continue;
				if (std::find(request.policy.blocked_domains.begin(), request.policy.blocked_domains.end(),
					*domain) != request.policy.blocked_domains.end()) continue;
				ExternalEvidenceReference evidence;
				evidence.url = url;
				const std::string title = citation.value("title", std::string());
				if (title.empty() == false) evidence.title = title;
				evidence.domain = domain;
				evidence.retrieved_at = workspaces::current_timestamp();
				evidence.source_class = classify_source(*domain);
				evidence.jurisdiction = request.policy.jurisdiction;
				evidence.excerpt = bounded_excerpt(text, citation.value("start_index", 0U),
					citation.value("end_index", 0U));
				evidence.content_hash = platform::sha256_hex_digest(evidence.excerpt);
				evidence.evidence_id = "ext_" + platform::sha256_hex_digest(url).substr(0, 20);
				const auto duplicate = std::find_if(result.evidence.begin(), result.evidence.end(),
					[&](const ExternalEvidenceReference &entry) { return entry.url == url; });
				if (duplicate == result.evidence.end()) result.evidence.push_back(std::move(evidence));
			}
		}
	}
	for (const auto &reported : reportedSources) {
		if (is_safe_public_research_url(reported.first) == false) continue;
		const std::optional<std::string> domain = host_from_url(reported.first);
		if (domain.has_value() == false
			|| std::find(request.policy.blocked_domains.begin(), request.policy.blocked_domains.end(),
				*domain) != request.policy.blocked_domains.end()) continue;
		if (std::find_if(result.evidence.begin(), result.evidence.end(), [&](const auto &known) {
			return known.url == reported.first;
		}) != result.evidence.end()) continue;
		ExternalEvidenceReference evidence;
		evidence.url = reported.first;
		evidence.title = reported.second;
		evidence.domain = domain;
		evidence.retrieved_at = workspaces::current_timestamp();
		evidence.source_class = classify_source(*domain);
		evidence.jurisdiction = request.policy.jurisdiction;
		evidence.excerpt = result.summary.substr(0,
			clamp_to_utf8_lead(result.summary, std::min<std::size_t>(result.summary.size(), 700)));
		evidence.content_hash = platform::sha256_hex_digest(evidence.excerpt);
		evidence.evidence_id = "ext_" + platform::sha256_hex_digest(evidence.url).substr(0, 20);
		result.evidence.push_back(std::move(evidence));
	}
	for (auto &entry : result.evidence) entry.research_provider_id = "openai";
	if (result.evidence.empty() == true)
		throw BilledResearchFailure("Web research found no usable external evidence.", result.provider_id,
			result.model_id, result.usage);
	{
		const std::lock_guard<std::mutex> lock(cache_mutex());
		for (const ExternalEvidenceReference &evidence : result.evidence)
			source_cache()[evidence.evidence_id] = evidence;
		while (source_cache().size() > 128) source_cache().erase(source_cache().begin());
	}
	return result;
}

std::optional<ResearchOpenResult> OpenAiHostedResearchAdapter::open(const std::string &sourceId) {
	const std::lock_guard<std::mutex> lock(cache_mutex());
	const auto found = source_cache().find(sourceId);
	if (found == source_cache().end()) return std::nullopt;
	ResearchOpenResult result;
	result.evidence = found->second;
	return result;
}

namespace {

// A separate cache from OpenAiHostedResearchAdapter's own source_cache()
// above - each adapter's opaque sourceId space is that adapter's own state,
// never cross-resolved just because both happen to produce "ext_..." ids.
std::mutex &openrouter_cache_mutex() {
	static std::mutex value;
	return value;
}

std::map<std::string, ExternalEvidenceReference> &openrouter_source_cache() {
	static std::map<std::string, ExternalEvidenceReference> value;
	return value;
}

// Bounded, contained entirely within this adapter (Master Spec: "no complex
// research router", "no unbounded agentic loop") - never a model-controlled
// argument, never exposed as Node/Project configuration surface in Phase
// 24.1D.
constexpr int kOpenRouterMaxSearchResults = 5;
constexpr int kOpenRouterMaxFetchContentTokens = 2000;
constexpr std::size_t kOpenRouterMaxExcerptBytes = 700;

// Truncates to at most kOpenRouterMaxExcerptBytes without ever slicing a
// multi-byte UTF-8 character in half - the exact live Phase 24.1D failure
// class ContextBudget.hpp's own utf8_safe_truncation_length() fixed for
// evidence compaction; reproduced here (not shared - this file has no
// dependency on ExternalResearch/ContextBudget.hpp, and the two truncation
// sites serve different callers) using the same well-known algorithm this
// file's own clamp_to_utf8_lead() already implements for a byte offset that
// starts mid-string, applied here to a truncation length starting at zero.
std::string bounded_excerpt_text(const std::string &text) {
	if (text.size() <= kOpenRouterMaxExcerptBytes) return text;
	return text.substr(0, clamp_to_utf8_lead(text, kOpenRouterMaxExcerptBytes));
}

// OpenRouter's server-tool citation shape is not independently documented
// with the same rigor as OpenAI's Responses API (see this file's own
// "UNVERIFIED provider contract" comment above for that one) - OpenRouter
// keeps its chat-completions response OpenAI-compatible, and its web search
// mechanisms have historically surfaced citations as
// choices[0].message.annotations[].url_citation (nested) - defensively
// accepting a flat annotation (url/title/content directly on the entry, no
// nested url_citation) as a fallback, exactly the same dual-shape tolerance
// this file's own direct-OpenAI annotation parsing above already uses. This
// is the one production judgment call the narrow live OpenRouter research
// probe exists specifically to prove or correct before any paid golden
// acceptance run.
struct OpenRouterCitation {
	std::string url;
	std::optional<std::string> title;
	std::optional<std::string> content;
};

std::vector<OpenRouterCitation> parse_openrouter_citations(const Json &message) {
	std::vector<OpenRouterCitation> citations;
	if (message.contains("annotations") == false || message.at("annotations").is_array() == false)
		return citations;
	for (const Json &annotation : message.at("annotations")) {
		if (annotation.is_object() == false) continue;
		if (annotation.contains("type") && annotation.at("type").is_string()
			&& annotation.at("type").get<std::string>() != "url_citation") continue;
		const Json &fields = annotation.contains("url_citation") && annotation.at("url_citation").is_object()
			? annotation.at("url_citation") : annotation;
		const std::string url = fields.value("url", std::string());
		if (url.empty()) continue;
		OpenRouterCitation citation;
		citation.url = url;
		const std::string title = fields.value("title", std::string());
		if (title.empty() == false) citation.title = title;
		const std::string content = fields.contains("content") && fields.at("content").is_string()
			? fields.at("content").get<std::string>()
			: fields.value("snippet", std::string());
		if (content.empty() == false) citation.content = content;
		citations.push_back(std::move(citation));
	}
	return citations;
}

} // namespace

OpenRouterExternalResearchAdapter::OpenRouterExternalResearchAdapter(std::string apiKey,
		std::string workerModelId, HostedResearchPost post)
	: api_key_(std::move(apiKey))
	, worker_model_id_(std::move(workerModelId))
	, post_(std::move(post))
{
	if (post_ == nullptr) post_ = platform::https_post;
}

ResearchResponse OpenRouterExternalResearchAdapter::search(const ResearchRequest &request) {
	if (request.policy.cancellation != nullptr && request.policy.cancellation->load() == true)
		throw std::runtime_error("Research was stopped.");
	// The prompt is built ONLY from the already-canonicalized/minimized
	// ResearchRequest fields ResearchTools.cpp's execute_search() hands this
	// adapter - never raw Project content, which this adapter (like
	// OpenAiHostedResearchAdapter above) never receives in the first place.
	std::string prompt = "Search the public web for real, current sources for this legal research "
		"query and use the search tool to find them. Query: " + request.query;
	if (request.policy.jurisdiction) prompt += "\nJurisdiction: " + *request.policy.jurisdiction;
	if (request.policy.freshness_required) prompt += "\nCurrent/recent information is required.";

	const Json tool{
		{"type", "openrouter:web_search"},
		{"parameters", Json{{"engine", "exa"}, {"max_results", kOpenRouterMaxSearchResults}}},
	};
	const Json body{
		{"model", worker_model_id_},
		{"messages", Json::array({Json{{"role", "user"}, {"content", prompt}}})},
		{"tools", Json::array({tool})},
		// The narrowest broadly-supported tool_choice that still guarantees a
		// real search executes: exactly one tool is offered, so "required"
		// (the same OpenAI-compatible value OpenAiHostedResearchAdapter's own
		// tool_choice above already uses) unambiguously forces it rather than
		// letting the worker model answer from memory.
		{"tool_choice", "required"},
	};
	routing::observability::emit({routing::observability::OperationEventKind::ProviderSendStart,
		"Sending approved research adapter request", {{"bodyBytes", std::to_string(body.dump().size())}},
		routing::observability::has_sink() ? std::optional<std::string>(body.dump()) : std::nullopt});
	const platform::HttpResponse response = post_(
		"https://openrouter.ai/api/v1/chat/completions",
		{{"Authorization", "Bearer " + api_key_}, {"Content-Type", "application/json"}}, body.dump(),
		kResearchTimeoutSeconds);
	if (!api_key_.empty() && response.body.find(api_key_) != std::string::npos)
		throw std::runtime_error("The research provider returned an unsafe credential echo.");
	routing::observability::emit({routing::observability::OperationEventKind::ProviderResponseReceived,
		"Research adapter HTTP response received", {{"status", std::to_string(response.status)}},
		routing::observability::has_sink() ? std::optional<std::string>(response.body) : std::nullopt});
	if (response.status == 429) throw std::runtime_error("Research is rate-limited. Wait and try again.");
	if (response.status < 200 || response.status >= 300)
		throw std::runtime_error("The configured research provider could not complete web search (HTTP "
			+ std::to_string(response.status) + ").");
	const Json payload = Json::parse(response.body, nullptr, false);
	if (payload.is_discarded())
		throw std::runtime_error("The research provider returned an unexpected response.");
	if (payload.contains("choices") == false || payload.at("choices").is_array() == false || payload.at("choices").empty())
		throw BilledResearchFailure("The research provider returned an unexpected response.", "openrouter",
			worker_model_id_, reported_research_usage(payload));
	const Json &message = payload.at("choices").at(0).contains("message")
		? payload.at("choices").at(0).at("message") : Json::object();

	ResearchResponse result;
	result.provider_id = "openrouter";
	result.model_id = worker_model_id_;
	result.usage = reported_research_usage(payload);
	result.summary = message.value("content", std::string());
	for (const OpenRouterCitation &citation : parse_openrouter_citations(message)) {
		if (safe_public_https_url(citation.url) == false) continue;
		const std::optional<std::string> domain = host_from_url(citation.url);
		if (domain.has_value() == false) continue;
		if (std::find(request.policy.blocked_domains.begin(), request.policy.blocked_domains.end(), *domain)
			!= request.policy.blocked_domains.end()) continue;
		if (std::find_if(result.evidence.begin(), result.evidence.end(), [&](const auto &known) {
			return known.url == citation.url;
		}) != result.evidence.end()) continue;
		ExternalEvidenceReference evidence;
		evidence.url = citation.url;
		evidence.title = citation.title;
		evidence.domain = domain;
		evidence.retrieved_at = workspaces::current_timestamp();
		evidence.source_class = classify_source(*domain);
		evidence.jurisdiction = request.policy.jurisdiction;
		// Truthfully a search-result snippet Exa/OpenRouter reported, never
		// independently fetched page content - see ExternalEvidenceReference's
		// own excerpt doc comment, which already makes this same distinction
		// for OpenAiHostedResearchAdapter above.
		evidence.excerpt = citation.content ? bounded_excerpt_text(*citation.content) : std::string();
		evidence.content_hash = platform::sha256_hex_digest(evidence.excerpt);
		evidence.evidence_id = "ext_" + platform::sha256_hex_digest(citation.url).substr(0, 20);
		evidence.provider_source_id = evidence.evidence_id;
		result.evidence.push_back(std::move(evidence));
	}
	for (auto &entry : result.evidence) {
		entry.research_provider_id = "openrouter";
		entry.research_engine = "exa";
	}
	if (result.evidence.empty() == true)
		throw BilledResearchFailure("Web research found no usable external evidence.", result.provider_id,
			result.model_id, result.usage);
	{
		const std::lock_guard<std::mutex> lock(openrouter_cache_mutex());
		for (const ExternalEvidenceReference &evidence : result.evidence)
			openrouter_source_cache()[evidence.evidence_id] = evidence;
		while (openrouter_source_cache().size() > 128) openrouter_source_cache().erase(openrouter_source_cache().begin());
	}
	return result;
}

std::optional<ResearchOpenResult> OpenRouterExternalResearchAdapter::open(const std::string &sourceId) {
	// The existing opaque sourceId / SSRF boundary, unchanged: this adapter
	// never accepts a model-supplied URL - only a sourceId already returned
	// by THIS adapter's own search() above, resolved to its already-verified
	// URL entirely locally before any web_fetch request is ever built.
	ExternalEvidenceReference cached;
	{
		const std::lock_guard<std::mutex> lock(openrouter_cache_mutex());
		const auto found = openrouter_source_cache().find(sourceId);
		if (found == openrouter_source_cache().end()) return std::nullopt;
		cached = found->second;
	}
	if (safe_public_https_url(cached.url) == false) return std::nullopt;

	const Json tool{
		{"type", "openrouter:web_fetch"},
		{"parameters", Json{{"engine", "exa"}, {"max_uses", 1}, {"max_content_tokens", kOpenRouterMaxFetchContentTokens}}},
	};
	const Json body{
		{"model", worker_model_id_},
		{"messages", Json::array({Json{{"role", "user"},
			{"content", "Fetch this exact URL using the fetch tool and report its content: " + cached.url}}})},
		{"tools", Json::array({tool})},
		{"tool_choice", "required"},
	};
	routing::observability::emit({routing::observability::OperationEventKind::ProviderSendStart,
		"Sending approved research adapter request", {{"bodyBytes", std::to_string(body.dump().size())}},
		routing::observability::has_sink() ? std::optional<std::string>(body.dump()) : std::nullopt});
	const platform::HttpResponse response = post_(
		"https://openrouter.ai/api/v1/chat/completions",
		{{"Authorization", "Bearer " + api_key_}, {"Content-Type", "application/json"}}, body.dump(),
		kResearchTimeoutSeconds);
	if (!api_key_.empty() && response.body.find(api_key_) != std::string::npos)
		throw std::runtime_error("The research provider returned an unsafe credential echo.");
	routing::observability::emit({routing::observability::OperationEventKind::ProviderResponseReceived,
		"Research adapter HTTP response received", {{"status", std::to_string(response.status)}},
		routing::observability::has_sink() ? std::optional<std::string>(response.body) : std::nullopt});
	if (response.status == 429) throw std::runtime_error("Research is rate-limited. Wait and try again.");
	if (response.status < 200 || response.status >= 300)
		throw std::runtime_error("The configured research provider could not complete web fetch (HTTP "
			+ std::to_string(response.status) + ").");
	const Json payload = Json::parse(response.body, nullptr, false);
	if (payload.is_discarded())
		throw std::runtime_error("The research provider returned an unexpected response.");
	const providers::UsageMetadata fetchUsage = reported_research_usage(payload);
	if (payload.contains("choices") == false || payload.at("choices").is_array() == false || payload.at("choices").empty())
		throw BilledResearchFailure("The research provider returned an unexpected response.", "openrouter",
			worker_model_id_, fetchUsage);
	const Json &message = payload.at("choices").at(0).contains("message")
		? payload.at("choices").at(0).at("message") : Json::object();
	const std::string fetchedContent = message.value("content", std::string());
	if (fetchedContent.empty())
		throw BilledResearchFailure("The research provider's web fetch returned no content.", "openrouter",
			worker_model_id_, fetchUsage);

	ExternalEvidenceReference fetched = cached;
	fetched.excerpt = bounded_excerpt_text(fetchedContent);
	fetched.content_hash = platform::sha256_hex_digest(fetched.excerpt);
	fetched.retrieved_at = workspaces::current_timestamp();
	fetched.content_kind = ExternalEvidenceContentKind::RetrievedContent;
	{
		const std::lock_guard<std::mutex> lock(openrouter_cache_mutex());
		openrouter_source_cache()[sourceId] = fetched;
	}
	ResearchOpenResult result;
	result.evidence = std::move(fetched);
	result.provider_id = "openrouter";
	result.model_id = worker_model_id_;
	result.usage = fetchUsage;
	return result;
}

} // namespace singularity::research
