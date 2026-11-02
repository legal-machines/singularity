#include "ExternalResearch/ResearchTools/ResearchTools.hpp"

#include "ExternalResearch/ExternalResearchAdapter/ExternalResearchAdapter.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/HttpClient/HttpsHost.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/RoutingLog/OutboundRecord.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace singularity::research {
namespace {

using Json = nlohmann::json;

std::mutex &registration_mutex() {
	static std::mutex value;
	return value;
}

bool &is_registered() {
	static bool value = false;
	return value;
}

// Which ExternalResearch backend actually runs a real web.search/web.open -
// selected once, from platform::LocalNodeSetup::research_backend (never a
// silent per-request failover between the two: Master Spec "no automatic
// retry", "record the actual research backend truthfully"). Both backends
// implement the identical IExternalResearchAdapter contract, so this is the
// ONLY place that decides which one runs; ResearchToolLoop/ToolBroker hold
// no knowledge of which backend a given operation used.
struct ResearchBackendSelection {
	bool openrouter = false;
	std::optional<std::string> credential;
	std::optional<std::string> unavailable_reason;
};

// The configured backend, its credential and, when it cannot run, the reason.
// The OpenRouter adapter always posts to openrouter.ai, so an OpenAI-compatible
// connection's credential is used for research only when that connection is
// OpenRouter itself - never a key issued for another endpoint.
ResearchBackendSelection select_research_backend(const tools::ToolExecutionContext &context) {
	const platform::AppSettings settings = platform::load_app_settings();
	ResearchBackendSelection selection;
	selection.openrouter = context.research_backend.value_or(settings.local_node_setup.research_backend) == "openrouter";
	const std::string providerId = selection.openrouter ? "openai_compatible" : "openai";
	const std::string providerName = selection.openrouter ? "OpenRouter" : "OpenAI";
	const auto provider = std::find_if(settings.local_node_setup.connected_providers.begin(),
		settings.local_node_setup.connected_providers.end(), [&](const platform::ConnectedProvider &entry) {
			return entry.id == providerId;
		});
	if (provider == settings.local_node_setup.connected_providers.end()) {
		selection.unavailable_reason = "Web research needs a connected " + providerName + " provider.";
		return selection;
	}
	if (selection.openrouter && platform::https_host(provider->base_url.value_or("")) != "openrouter.ai") {
		selection.unavailable_reason =
			"Web research through OpenRouter needs the OpenAI-compatible connection to use https://openrouter.ai.";
		return selection;
	}
	try {
		selection.credential = platform::get_provider_credential(providerId);
	} catch (const std::exception &) {
		// A credential that cannot be READ right now (macOS keeps it unreadable
		// while the Mac is locked, which is exactly the protection it is stored
		// with) makes web research unavailable - it must never take the whole
		// request down with it. A local, source-bound request needs no research
		// credential at all, and a lawyer working offline on their own documents
		// must not be stopped because a cloud key could not be opened.
		selection.unavailable_reason =
			std::string("Web research is unavailable right now: ") + platform::credential_unreadable_explanation(providerName);
		return selection;
	}
	if (selection.credential.has_value() == false || selection.credential->empty() == true)
		selection.unavailable_reason = "Reconnect " + providerName + " before using web research.";
	return selection;
}

// Which backend a research step goes through, from settings alone - never the
// credential, so describing a step for approval opens nothing.
bool is_openrouter_research(const tools::ToolExecutionContext &context) {
	return context.research_backend.value_or(platform::load_app_settings().local_node_setup.research_backend) == "openrouter";
}

// The page address behind every source a search returned, by its sourceId: a
// web.open step names only the id, and the lawyer is asked about the address.
// Addresses are public and already kept with the evidence; the oldest go first.
std::mutex &source_address_mutex() {
	static std::mutex value;
	return value;
}

std::unordered_map<std::string, std::string> &source_addresses() {
	static std::unordered_map<std::string, std::string> value;
	return value;
}

void remember_source_addresses(const std::vector<ExternalEvidenceReference> &evidence) {
	constexpr std::size_t kRememberedAddresses = 4096;
	const std::lock_guard<std::mutex> lock(source_address_mutex());
	if (source_addresses().size() > kRememberedAddresses) source_addresses().clear();
	for (const ExternalEvidenceReference &entry : evidence)
		if (entry.evidence_id.empty() == false && entry.url.empty() == false) source_addresses()[entry.evidence_id] = entry.url;
}

std::string source_address(const std::string &sourceId) {
	const std::lock_guard<std::mutex> lock(source_address_mutex());
	const auto found = source_addresses().find(sourceId);
	return found == source_addresses().end() ? std::string() : found->second;
}

// The outbound record of one research call (Routing/RoutingLog/OutboundRecord.hpp),
// under the operation it served. A query carries no protection markers: the
// Project's protected values are taken out of it, never replaced. The
// protection it records is the one this very call was held to (the research
// loop's ResearchCallProtection) - never the computer's current setting, which
// says nothing about a search a model on this computer wrote; "off" when
// nothing says it was protected.
void note_research_call(const tools::ToolExecutionContext &context, const std::string &kind,
		const std::string &providerId, const std::string &modelId, const std::string &url) {
	protected_api::papinian::OutboundCall call;
	call.kind = kind;
	call.provider_id = providerId;
	call.model_id = modelId;
	const std::shared_ptr<ResearchCallProtection> protection = current_research_call_protection();
	call.protection_mode = protected_api::to_string(protection ? protection->mode : protected_api::Mode::Off);
	call.url = url;
	// A step outside any operation is still the attempt's own: it runs on the
	// attempt's thread, and is written with the attempt's entry by that.
	static std::atomic<unsigned long long> unnamedSteps{0};
	const std::string key = context.operation_id.empty() == false ? context.operation_id
		: "research-step-" + std::to_string(++unnamedSteps);
	routing::outbound::record_call(key, std::move(call));
}

// The step's approval carries what will leave, under "outbound" in
// tools::ToolApprovalRequest's change_detail_json (research_search_outbound()).
void describe_search_target(const tools::ToolExecutionContext &context, const Json &arguments,
		tools::ToolApprovalRequest &approval) {
	approval.change_detail_json = Json{{"outbound", research_search_outbound(arguments, is_openrouter_research(context))}}.dump();
}

void describe_open_target(const tools::ToolExecutionContext &context, const Json &arguments,
		tools::ToolApprovalRequest &approval) {
	const std::string url = source_address(arguments.at("sourceId").get<std::string>());
	approval.change_detail_json = Json{{"outbound", research_open_outbound(url, is_openrouter_research(context))}}.dump();
}

std::shared_ptr<IExternalResearchAdapter> production_adapter(const tools::ToolExecutionContext &context) {
	const ResearchBackendSelection selection = select_research_backend(context);
	if (selection.unavailable_reason) throw std::runtime_error(*selection.unavailable_reason);
	if (selection.openrouter) return std::make_shared<OpenRouterExternalResearchAdapter>(*selection.credential);
	return std::make_shared<OpenAiHostedResearchAdapter>(*selection.credential);
}

// Papinian protection for a research query means something narrower than
// PapinianGuard::protect()'s general document/prompt-content masking: a
// query is deliberately public text whose entire value is naming real,
// public research subjects (a court, a statute, a regulator) - the only
// thing that must never reach it is a confidential Project value. Running
// the full semantic-NER guard (the same one used for Project document/chat
// content, where any organization/person/place name could genuinely be
// confidential) over a query masks exactly the terms a search needs to be
// useful - real live evidence: the identical public research question
// returns real, correctly cited results from OpenAI's hosted search when
// sent verbatim, and "Web research found no usable external evidence." once
// routed through protect(), because it silently rewrites terms like "UK
// Supreme Court" into an opaque [[LM_...]] placeholder before the query
// ever reaches the search adapter. minimize_research_query() is already the
// correctly-scoped protection for this boundary - it replaces only the
// Project's own known-sensitive values.
//
// By the time this handler runs, `arguments` has already been through
// canonicalize_search_arguments() once, identically for every provider and
// every model, in run_research_tool_loop() - before ToolBroker even
// validated the request (see ResearchTools.hpp/ResearchToolLoop.hpp). So the
// query below is already minimized; this step only re-verifies none of the
// Project's sensitive values survived that upstream pass, the same
// fail-closed guarantee protect()'s masking-completeness check gave, kept
// here as the last check before a real network call leaves the machine -
// never trusting upstream canonicalization blindly for a privacy boundary -
// without masking the query's own legitimate public subject matter.
Json external_call_json(const std::string &providerId, const std::string &modelId,
		const providers::UsageMetadata &usage) {
	const auto integer = [](const std::optional<long long> &value) { return value ? Json(*value) : Json(nullptr); };
	const auto text = [](const std::optional<std::string> &value) { return value ? Json(*value) : Json(nullptr); };
	return Json{{"providerId", providerId}, {"modelId", modelId}, {"usage", Json{
		{"inputTokens", integer(usage.input_tokens)}, {"outputTokens", integer(usage.output_tokens)},
		{"totalTokens", integer(usage.total_tokens)}, {"costMicros", integer(usage.cost_micros)},
		{"costCurrency", text(usage.cost_currency)}, {"costClassification", text(usage.cost_classification)},
		{"costMethodology", text(usage.cost_methodology)}}}};
}

Json execute_search(const tools::ToolExecutionContext &context, const Json &arguments) {
	const platform::AppSettings settings = platform::load_app_settings();
	const protected_api::Policy policy = settings.local_node_setup.protected_api_policy;
	routing::require_papinian_model_ready(policy);
	// The same normalized, boundary-aware matching the canonicalization used,
	// against the Project's sensitive values and this operation's protected
	// originals, over every string the search sends - anything it would still
	// change has not been minimized - and, for a search a model on this
	// computer wrote, Papinian Guard's own pass (prepare_outbound_research_request()).
	const std::shared_ptr<ResearchCallProtection> protection = current_research_call_protection();
	const ResearchRequest request = prepare_outbound_research_request(context, arguments, protection.get());
	ResearchResponse response;
	try {
		response = production_adapter(context)->search(request);
	} catch (const BilledResearchFailure &failure) {
		note_research_call(context, "web_search", failure.provider_id, failure.model_id, "");
		throw tools::BilledExternalCallFailure(failure.what(), failure.provider_id, failure.model_id, failure.usage);
	}
	note_research_call(context, "web_search", response.provider_id, response.model_id, "");
	remember_source_addresses(response.evidence);
	Json evidence = Json::array();
	for (const ExternalEvidenceReference &entry : response.evidence)
		evidence.push_back(external_evidence_to_json(entry));
	return Json{{"providerId", response.provider_id}, {"summary", response.summary}, {"evidence", evidence},
		{"externalCall", external_call_json(response.provider_id, response.model_id, response.usage)}};
}

Json execute_open(const tools::ToolExecutionContext &context, const Json &arguments) {
	std::optional<ResearchOpenResult> opened;
	const std::string sourceId = arguments.at("sourceId").get<std::string>();
	try {
		opened = production_adapter(context)->open(sourceId);
	} catch (const BilledResearchFailure &failure) {
		note_research_call(context, "web_open", failure.provider_id, failure.model_id, source_address(sourceId));
		throw tools::BilledExternalCallFailure(failure.what(), failure.provider_id, failure.model_id, failure.usage);
	}
	if (opened.has_value() == false)
		throw std::runtime_error("This external source was not returned by this research session.");
	// Only a page fetched through a provider is a call that left; one the search
	// already read is answered here.
	if (opened->usage.has_value()) note_research_call(context, "web_open", opened->provider_id, opened->model_id, opened->evidence.url);
	Json result{{"evidence", external_evidence_to_json(opened->evidence)}};
	if (opened->usage.has_value())
		result["externalCall"] = external_call_json(opened->provider_id, opened->model_id, *opened->usage);
	return result;
}

} // namespace

std::optional<std::string> production_research_unavailable_reason(const tools::ToolExecutionContext &context) {
	const ResearchBackendSelection selection = select_research_backend(context);
	if (selection.unavailable_reason) return selection.unavailable_reason;
	// A research query is minimized by Papinian under the Node's protection
	// policy. While Papinian is still loading the search waits for it; unloaded
	// or failed, research cannot run.
	const protected_api::Policy policy = platform::load_app_settings().local_node_setup.protected_api_policy;
	if (policy.mode != protected_api::Mode::Off) {
		const auto status = routing::papinian_warmup_tracker().status();
		if (status.state == routing::ModelLifecycleState::Unloaded)
			return std::string("Papinian NER is unloaded, and web research queries need it. Load it in Models.");
		if (status.state == routing::ModelLifecycleState::Error)
			return status.message.empty() ? std::string("Papinian Guard is not ready, and web research queries need it.")
				: status.message;
	}
	return std::nullopt;
}

void ensure_production_research_tools() {
	const std::lock_guard<std::mutex> lock(registration_mutex());
	if (is_registered() == true) return;
	tools::ToolDefinition search;
	search.id = "web.search";
	search.capability = "web.search";
	search.effect = tools::ToolEffectClass::ExternalNetwork;
	search.approval_summary = "Search the public web for this legal issue";
	search.describe_approval_target = describe_search_target;
	search.validate = validate_search;
	search.execute = execute_search;
	tools::production_tool_broker().register_tool(search);
	tools::ToolDefinition open;
	open.id = "web.open";
	open.capability = "web.open";
	open.effect = tools::ToolEffectClass::ExternalNetwork;
	open.approval_summary = "Open a source returned by web research";
	open.describe_approval_target = describe_open_target;
	open.validate = validate_open;
	open.execute = execute_open;
	tools::production_tool_broker().register_tool(open);
	is_registered() = true;
}

} // namespace singularity::research
