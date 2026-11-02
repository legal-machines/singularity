#include "DocumentReview/ColleagueWork/ColleagueWork.hpp"
#include "Agent/AgentStore/AgentStore.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"
#include "DocumentReview/DocumentReviewActions/DocumentReviewActions.hpp"
#include "DocumentIntelligence/SlideWriter/SlideWriter.hpp"
#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"
#include "Context/ProjectContextActions/ProjectContextActions.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/ProfessionalArtifactAction/ProfessionalArtifactAction.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"
#include "Tool/OperationAuthority/OperationAuthority.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <regex>
#include <thread>

namespace singularity::document_review {
namespace {

std::string lowered(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

long long millisecondsSince(const std::chrono::steady_clock::time_point& start) {
	return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count());
}

// Cut at a whole character, never inside one, with the reading's end said.
std::string clippedWords(const std::string& text, std::size_t most, bool& isComplete) {
	isComplete = text.size() <= most;
	if (isComplete == true) return text;
	std::size_t end = most;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
	return text.substr(0, end) + "\n(The document goes on; this is as far as it was read.)";
}

} // namespace

routing::ModelTarget modelFor(const std::string& matterId, bool isLocalOnly, const std::string& agentId) {

	// The model chosen on the colleague's page, when one is (D-185): its own
	// for every piece of its work. A colleague kept to this computer takes only
	// a model on it.
	if (agentId.empty() == false) {
		std::optional<agents::Agent> agent;
		try {
			const std::lock_guard<std::mutex> lock(agents::agent_store_mutex());
			agent = agents::load_agent(agentId);
		} catch (const std::exception&) {
		}
		if (agent.has_value() == true && agent->model_id.empty() == false
				&& (isLocalOnly == false || agent->model_provider_id == "local"))
			return routing::ModelTarget{agent->model_provider_id, agent->model_id};
	}
	chat::OperationSetupRequest setup;
	setup.project_id = matterId;
	std::optional<routing::ModelTarget> model = chat::resolve_operation_setup(setup).primary_model;
	if (isLocalOnly == true) {
		if (model.has_value() == true && model->provider_id == "local") return *model;
		const std::optional<std::string> local = routing::preferred_local_generative_model();
		if (local.has_value() == false) {
			throw ReviewRefused("This colleague works only on " + computerWords() + ", and no model is installed there. "
				"Install one in Models there, then try again.");
		}
		return routing::ModelTarget{"local", *local};
	}
	// Said where it is chosen and by whom (D-184, D-185): a colleague's words
	// are read on every computer of its Matter (2026-10-31: Unicorn told Dana,
	// on another computer, to "choose one in AI Setup", a place the window does
	// not have).
	if (model.has_value() == false) throw ReviewRefused("No AI model is set for me yet. " + modelChooser(true) + " can choose one on my page.");
	return *model;

}

namespace {

// The member at this computer, as their own Settings name them; "" for no one.
std::string personHere() {
	try {
		const platform::AppSettings settings = platform::load_app_settings();
		std::string name = settings.local_display_name.value_or(std::string());
		const auto isSpace = [](unsigned char letter) { return std::isspace(letter) != 0; };
		while (name.empty() == false && isSpace(static_cast<unsigned char>(name.back())) == true) name.pop_back();
		while (name.empty() == false && isSpace(static_cast<unsigned char>(name.front())) == true) name.erase(name.begin());
		return name;
	} catch (const std::exception&) {
		// Unread settings name no one.
	}
	return std::string();
}

} // namespace

std::string computerWords() {
	const std::string name = personHere();
	return name.empty() == true ? std::string("this computer") : name + "'s computer";
}

std::string modelChooser(bool isSentenceStart) {
	const std::string name = personHere();
	if (name.empty() == false) return name;
	return isSentenceStart == true ? "The firm's administrator" : "the firm's administrator";
}

// A model as the window names it: "openai/gpt-6.1-sol" is GPT 6.1 Sol
// (components/Review/Review.jsx modelWords), never its id in a message.
std::string modelWordsOf(const std::string& id) {
	std::string name = id.substr(id.find_last_of('/') == std::string::npos ? 0 : id.find_last_of('/') + 1);
	std::string words;
	bool isWordStart = true;
	for (const char character : name) {
		if (character == '-') {
			words += ' ';
			isWordStart = true;
			continue;
		}
		words += isWordStart == true && character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A') : character;
		isWordStart = false;
	}
	if (words.rfind("Gpt", 0) == 0) words.replace(0, 3, "GPT");
	return words.empty() == true ? std::string("The Matter's model") : words;
}

std::string reasonFor(providers::TaskErrorCode code, const std::string& modelId, const std::string& message) {
	const std::string model = modelWordsOf(modelId);
	switch (code) {
		case providers::TaskErrorCode::ProviderAuthentication:
			return "The model's provider refused the key kept on " + computerWords() + ". It can be connected again there, in AI "
				"models; then ask again.";
		case providers::TaskErrorCode::ProviderRateLimit:
			return "The model's provider is busy and asked to wait. Try again in a minute.";
		case providers::TaskErrorCode::NetworkError:
			return "The model's provider could not be reached. Check the connection, then try again.";
		case providers::TaskErrorCode::PrivacyProtectionUnavailable:
			return "Papinian Guard could not protect this request, so nothing was sent: its protection model is not "
				"ready on " + computerWords() + ". Once it is, try again.";
		case providers::TaskErrorCode::ProtectedPayloadLeakDetected:
		case providers::TaskErrorCode::ProtectedResponseInvalid:
			return "Papinian Guard stopped this request to keep the document's protected facts safe, so nothing was kept. "
				"Try again.";
		case providers::TaskErrorCode::ContextCapacityExceeded:
			return model + " cannot read a document this long at once. " + modelChooser(true) + " can choose a model that "
				"reads longer documents for me, on my page; then ask again.";
		case providers::TaskErrorCode::UnsupportedModel:
			return model + " cannot take this request. " + modelChooser(true) + " can choose another model for me on my page; "
				"then ask again.";
		// One request turned down is not a reason to change the Matter's model: a
		// DPA's last turn was, and the thread told the lawyer to choose another
		// model by its id (2026-10-31).
		case providers::TaskErrorCode::ProviderRejectedRequest:
			// An account with no credit left is no refusal of the model's, and no
			// other model on that account takes the request either: said as the
			// account and what to do (2026-10-31: Griffin's reply twice read "GPT
			// 6.1 Sol turned the request down" for an OpenRouter account out of credit).
			if (const std::optional<std::string> provider = providers::out_of_credit_provider(message); provider.has_value() == true) {
				const bool isNamed = provider->empty() == false && *provider != "This service" && *provider != "This endpoint";
				return (isNamed == true ? "The " + *provider + " account" : std::string("The model's account"))
					+ " has no credit left for this request. Add credit to it, then ask again.";
			}
			// Said so it reads the same in a reply ("I can't answer right now: ...")
			// as in a closing about work that stopped.
			return model + " turned the request down. If it happens again, " + modelChooser(false) + " can choose another model "
				"for me on my page.";
		case providers::TaskErrorCode::ResponseIncomplete:
			return "The model stopped before its answer was complete, so none of it was kept. Try again.";
		default:
			return "The work could not be completed. Nothing was changed; try again.";
	}
}

CallDecision decisionOf(const routing::ModelTarget& model, const routing::InferenceRequest& request,
	const std::optional<providers::TaskErrorCode>& error, const std::string& message) {
	CallDecision made;
	made.mode = routing::protection_mode_of(model.provider_id, request);
	if (model.provider_id == "local") {
		made.decision = error.has_value() == true ? "failed" : "local";
		return made;
	}
	if (error.has_value() == false) {
		made.decision = "sent";
		return made;
	}
	const bool isBlocked = *error == providers::TaskErrorCode::ProtectedPayloadLeakDetected
		|| *error == providers::TaskErrorCode::PrivacyProtectionUnavailable;
	made.decision = isBlocked == true ? "blocked" : "failed";
	// The Guard's own refusals are kept in its words: they name kinds and places
	// by design. A provider's are kept value-free (providerWords): its text may
	// echo the request, so what it quotes and every key-like token goes.
	if (isBlocked == true) made.reason = message;
	else if (*error == providers::TaskErrorCode::ProviderRejectedRequest || *error == providers::TaskErrorCode::UnsupportedModel
			|| *error == providers::TaskErrorCode::NetworkError || *error == providers::TaskErrorCode::ProviderRateLimit)
		made.reason = providerWords(message);
	return made;
}

std::string providerWords(const std::string& message) {
	std::string words;
	char quote = 0;
	for (const char character : message) {
		if (words.size() >= 320) break;
		if (quote != 0) {
			if (character == quote) {
				words += "...";
				words += character;
				quote = 0;
			}
			continue;
		}
		if (character == '"' || character == '`') quote = character;
		words += character;
	}
	if (quote != 0) words += "...";
	static const std::regex keyLike("(sk-[A-Za-z0-9_\\-]+|[Bb]earer\\s+\\S+|[A-Za-z0-9_\\-]{24,})");
	return std::regex_replace(words, keyLike, "[redacted]");
}

void recordCall(const std::string& matterId, const std::string& taskId, const routing::ModelTarget& model,
	const routing::InferenceResult* result, const std::optional<providers::TaskErrorCode>& error, long long latency,
	const providers::UsageMetadata* spent, const std::optional<std::string>& spentFinish, const std::string& presetId,
	std::size_t documentCount, const CallDecision& decided) {

	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		std::optional<matters::Matter> matter = matters::load_matter(workspace, matterId);
		if (matter.has_value() == false) return;
		for (matters::MatterTask& task : matter->tasks) {
			if (task.id != taskId || result == nullptr) continue;
			task.usage = providers::accumulate_usage(task.usage, result->usage);
			task.protected_api_applied = task.protected_api_applied || result->protected_api_applied;
			task.privacy_metadata = result->privacy_metadata;
		}
		if (result == nullptr) {
			for (matters::MatterTask& task : matter->tasks) {
				if (task.id != taskId) continue;
				task.status = matters::TaskStatus::Error;
				if (error.has_value() == true) task.error_code = error;
				if (spent != nullptr) task.usage = providers::accumulate_usage(task.usage, *spent);
			}
		}
		matters::save_matter(workspace, *matter);
		routing::RoutingLogEntry entry;
		entry.matter_id = matterId;
		entry.operation_id = taskId;
		entry.task_id = taskId;
		entry.time = workspaces::current_timestamp();
		entry.provider_id = model.provider_id;
		entry.model_id = model.model_id;
		entry.execution_kind = model.provider_id == "local" ? "local" : "external";
		entry.legal_task_preset_id = presetId;
		entry.document_count = documentCount;
		entry.success = result != nullptr;
		entry.latency_ms = latency;
		if (result != nullptr) {
			entry.privacy_metadata = result->privacy_metadata;
			entry.usage = result->usage;
			entry.finish_reason = result->finish_reason;
		} else if (spent != nullptr) {
			entry.usage = *spent;
			entry.finish_reason = spentFinish;
		}
		// A call that failed carries no protection result: its record still says
		// the mode it ran under and what became of it.
		if (result == nullptr && decided.mode.empty() == false) entry.privacy_metadata.mode = decided.mode;
		if (decided.decision.empty() == false) {
			entry.privacy_metadata.decision = decided.decision;
			entry.privacy_metadata.decision_reason = decided.reason;
		}
		if (error.has_value() == true) entry.error_code = error;
		routing::append_routing_log_entry(workspace, entry);
	} catch (...) {
	}

}

DocumentWords wordsOfDocument(const std::filesystem::path& path, std::size_t mostCharacters) {

	DocumentWords words;
	const std::string extension = lowered(path.extension().u8string());
	try {
		if (extension == ".xlsx" || extension == ".xlsm") {
			words.text = documents::workbook_cells_for_reading(path, mostCharacters);
			words.isComplete = words.text.find("this is as far as it was read") == std::string::npos;
			if (words.text.empty() == true) words.problem = "The workbook holds nothing Singularity could read.";
			return words;
		}
		if (extension == ".pptx") {
			std::string text;
			for (const document_intelligence::SlideParagraphs& slide : document_intelligence::readSlideParagraphs(path)) {
				text += "Slide " + std::to_string(slide.slideNumber) + ":\n";
				for (const std::string& paragraph : slide.paragraphs) text += "- " + paragraph + "\n";
			}
			if (text.empty() == true) {
				words.problem = "The presentation's slides could not be read.";
				return words;
			}
			words.text = clippedWords(text, mostCharacters, words.isComplete);
			return words;
		}
		documents::DocumentExtractionRequest request;
		request.path = path;
		const documents::DocumentExtractionResult result = documents::LocalDocumentExtractor().extract(request);
		switch (result.status) {
			case documents::ExtractionStatus::ready:
				words.text = clippedWords(result.text, mostCharacters, words.isComplete);
				return words;
			case documents::ExtractionStatus::empty:
				words.problem = "The document has no words to read.";
				return words;
			case documents::ExtractionStatus::ocr_required:
				words.problem = "A scanned document whose words this computer cannot read.";
				return words;
			case documents::ExtractionStatus::unsupported:
				words.problem = "Singularity does not read this kind of file.";
				return words;
			case documents::ExtractionStatus::unreadable:
				words.problem = "Singularity could not open this file: it may be damaged or protected.";
				return words;
		}
	} catch (const std::exception&) {
		words.problem = "Singularity could not read this file.";
		return words;
	}
	words.problem = "Singularity could not read this file.";
	return words;

}

ColleagueCallResult callColleagueModel(const ColleagueCall& call) {

	ColleagueCallResult outcome;
	try {
		outcome.model = call.model.has_value() == true && (call.isLocalOnly == false || call.model->provider_id == "local")
			? *call.model : modelFor(call.matterId, call.isLocalOnly, call.agentId);
	} catch (const std::exception& error) {
		outcome.failure = error.what();
		return outcome;
	}
	const routing::ModelTarget& model = outcome.model;
	const bool isLocal = model.provider_id == "local";
	const routing::ModelCapability capability = routing::model_capability(model);
	std::optional<std::uint64_t> allowance = capability.max_output_tokens;
	if (call.maximumOutputTokens.has_value() == true) {
		allowance = allowance.has_value() == true ? std::min(*allowance, *call.maximumOutputTokens) : call.maximumOutputTokens;
	}
	// A limit this computer chose on its spend is kept (D-118); with none
	// chosen, nothing here holds a call back (D-132).
	const ReviewBudget budget = isLocal == true ? ReviewBudget() : loadReviewBudget();
	if (isLocal == false && budget.limitUsd.has_value() == true) {
		if (allowance.has_value() == false) {
			outcome.failure = "Singularity cannot tell what this work could cost: the provider reports no limit on how much this "
				"model writes, so nothing was sent. " + modelChooser(true) + " can choose a model on my page whose provider reports "
				"its limits.";
			return outcome;
		}
		// What the budget's remainder pays for, where the whole allowance does
		// not fit (D-118).
		const std::uint64_t affordable = affordableOutputTokens(call.prompt.size(), *allowance,
			capability.input_price_per_token, capability.output_price_per_token, budget, kLeastStepOutputTokens);
		if (affordable > 0) allowance = affordable;
		if (affordable == 0) {
			char spent[32];
			char most[32];
			std::snprintf(spent, sizeof(spent), "%.2f", static_cast<double>(budget.spentMicros) / 1000000.0);
			std::snprintf(most, sizeof(most), "%.2f", *budget.limitUsd);
			outcome.failure = std::string("This computer's budget for colleagues' work by external models is $") + most + ", and $"
				+ spent + " of it is spent; this step could cost more than what is left, so nothing was sent.";
			return outcome;
		}
	}

	// The operation the call is in the Matter, as the colleague's own.
	const std::string operationId = "colleague-op-" + platform::secure_random_hex(10);
	outcome.taskId = matters::generate_task_id();
	tools::OperationAuthority authority;
	authority.origin = tools::OperationOrigin::Agent;
	authority.agent_id = call.agentId;
	authority.actor_id = platform::resolve_actor_identity().actor_id;
	// What the Matter's own mode lets any member do (D-158).
	authority.local_only = call.isLocalOnly;
	tools::begin_operation_authority(call.matterId, operationId, authority);
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		std::optional<matters::Matter> current = matters::load_matter(workspace, call.matterId);
		if (current.has_value() == true) {
			matters::MatterTask operation;
			operation.id = outcome.taskId;
			operation.created_at = workspaces::current_timestamp();
			operation.operation_type = "document_review";
			operation.operation_role = call.operationRole;
			operation.agent_operation_id = operationId;
			operation.agent_id = call.agentId;
			operation.author_actor_id = call.givenByActorId;
			operation.author_name = call.givenByName;
			operation.prompt = call.label;
			operation.requested_provider_id = model.provider_id;
			operation.requested_model_id = model.model_id;
			operation.provider_id = model.provider_id;
			operation.model_id = model.model_id;
			operation.execution_kind = isLocal == true ? "local" : "external";
			if (isLocal == true) operation.runtime_id = "local";
			operation.local_only = call.isLocalOnly;
			operation.operation_device_id = platform::current_device_id();
			operation.document_review_id = call.reviewId;
			operation.status = matters::TaskStatus::Success;
			current->tasks.push_back(std::move(operation));
			matters::save_matter(workspace, *current);
		}
	} catch (const std::exception& error) {
		outcome.failure = std::string("The work could not be recorded in the Matter: ") + error.what();
		return outcome;
	}

	routing::InferenceRequest request;
	request.call_purpose = call.isCorrection == true ? routing::observability::CallPurpose::Correction
		: routing::observability::CallPurpose::SubstantiveGeneration;
	request.model_target = model;
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		request.workspace_id = platform::load_app_settings().local_node_setup.active_workspace_id;
	} catch (const std::exception&) {
	}
	request.maximum_output_tokens = allowance;
	if (isLocal == false) request.project_entity_hints = context_actions::memory_protection_hints(call.matterId);
	request.prompt = call.prompt;
	request.prompt_parts = call.promptParts;
	request.prompt_instruction = call.promptInstruction;
	request.reasoning_effort = call.reasoningEffort;
	request.is_conversation = call.isConversation;
	request.research_mode = research::ResearchMode::Off;
	// A call that answers in words (a colleague's talk) asks for no JSON shape.
	if (call.schema.is_null() == false) {
		request.research_final_output_contract = providers::GenerationOutputContract{call.schemaName, call.schema};
	}
	request.operation_symbols = call.symbols != nullptr ? call.symbols
		: std::make_shared<protected_api::papinian::OperationProtectedSymbolTable>();
	if (request.operation_symbols->operation_id.empty() == true) request.operation_symbols->operation_id = operationId;

	// A Stop on the colleague, or Singularity closing, abandons the call.
	const std::shared_ptr<platform::HttpCancelToken> token = std::make_shared<platform::HttpCancelToken>();
	const platform::lifecycle::CancelOnClosing closingStops(token);
	std::atomic<bool> isDone(false);
	std::thread watcher;
	if (call.cancelled != nullptr) {
		watcher = std::thread([&]() {
			while (isDone.load() == false) {
				if (call.cancelled->load() == true) {
					token->cancel();
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			}
		});
	}
	const auto endWatch = [&]() {
		isDone.store(true);
		if (watcher.joinable() == true) watcher.join();
	};
	const auto count = [&](const providers::UsageMetadata& usage) {
		const long long in = usage.input_tokens.value_or(0);
		const long long out = usage.output_tokens.value_or(0);
		outcome.inputTokens += in;
		outcome.outputTokens += out;
		outcome.reasoningTokens += usage.reasoning_tokens.value_or(0);
		outcome.cachedInputTokens += usage.cached_input_tokens.value_or(0);
		outcome.cacheWriteTokens += usage.cache_write_tokens.value_or(0);
		std::optional<long long> cost;
		bool isEstimated = false;
		if (usage.cost_micros.has_value() == true) {
			cost = usage.cost_micros;
		} else if (isLocal == false && (in > 0 || out > 0)) {
			cost = reviewCostBoundMicros(call.prompt.size(), static_cast<std::uint64_t>(std::max<long long>(out, 0)),
				capability.input_price_per_token, capability.output_price_per_token, loadReviewBudget());
			isEstimated = true;
		}
		if (cost.has_value() == false) return;
		outcome.costSource = isEstimated == true ? "estimated" : "provider_reported";
		outcome.costMicros = outcome.costMicros.value_or(0) + *cost;
		if (isLocal == false) recordReviewSpend(*cost, isEstimated);
	};
	const auto started = std::chrono::steady_clock::now();
	routing::InferenceResult result;
	try {
		routing::require_execution_ready(request);
		const platform::HttpCancelScope scope(token);
		// What the provider may keep of the conversation's prompt between calls.
		const providers::PromptCacheScope cached(call.promptCache);
		result = routing::execute(request);
	} catch (const providers::TaskError& error) {
		endWatch();
		outcome.latencyMilliseconds = millisecondsSince(started);
		const auto* unusable = dynamic_cast<const providers::UnusableGeneration*>(&error);
		recordCall(call.matterId, outcome.taskId, model, nullptr, error.code(), outcome.latencyMilliseconds,
			unusable != nullptr ? &unusable->usage : nullptr,
			unusable != nullptr ? unusable->finish_reason : std::optional<std::string>(), call.presetId, call.documentCount,
			decisionOf(model, request, error.code(), error.what()));
		if (unusable != nullptr) count(unusable->usage);
		if (platform::lifecycle::is_closing() == true) {
			outcome.isInterrupted = true;
			outcome.failure = "Singularity closed during the work; it begins again when Singularity opens.";
			return outcome;
		}
		if (token->is_cancelled() == true || (call.cancelled != nullptr && call.cancelled->load() == true)) {
			outcome.isCancelled = true;
			outcome.failure = "The work was stopped before the model answered.";
			return outcome;
		}
		outcome.failure = reasonFor(error.code(), model.model_id, error.what());
		outcome.errorCode = error.code();
		// Unreachable for now (D-169): asked again at once a few times, as a
		// network or a busy provider usually comes back in seconds; still so, the
		// work waits for it, never fails.
		if (error.code() == providers::TaskErrorCode::NetworkError || error.code() == providers::TaskErrorCode::ProviderRateLimit)
			outcome.isUnavailable = true;
		return outcome;
	} catch (const platform::CredentialUnavailable& error) {
		// The Mac is locked: its saved credential is unreadable until it is
		// unlocked, and the work carries on then (D-169).
		endWatch();
		outcome.latencyMilliseconds = millisecondsSince(started);
		outcome.isInterrupted = true;
		outcome.isUnavailable = true;
		outcome.failure = std::string("The model cannot be asked yet: ") + error.what();
		return outcome;
	} catch (const std::exception& error) {
		endWatch();
		outcome.latencyMilliseconds = millisecondsSince(started);
		recordCall(call.matterId, outcome.taskId, model, nullptr, providers::TaskErrorCode::Unknown,
			outcome.latencyMilliseconds, nullptr, std::nullopt, call.presetId, call.documentCount,
			decisionOf(model, request, providers::TaskErrorCode::Unknown, std::string()));
		if (platform::lifecycle::is_closing() == true) {
			outcome.isInterrupted = true;
			outcome.failure = "Singularity closed during the work; it begins again when Singularity opens.";
			return outcome;
		}
		outcome.failure = std::string("The model could not be asked: ") + error.what();
		return outcome;
	}
	endWatch();
	outcome.latencyMilliseconds = millisecondsSince(started);
	recordCall(call.matterId, outcome.taskId, model, &result, std::nullopt, outcome.latencyMilliseconds, nullptr,
		std::nullopt, call.presetId, call.documentCount, decisionOf(model, request, std::nullopt, std::string()));
	count(result.usage);
	outcome.isAnswered = true;
	outcome.servedBy = result.usage.served_by.value_or(std::string());
	outcome.text = result.text;
	outcome.finishReason = result.finish_reason.value_or(std::string());
	outcome.isProtectionApplied = result.protected_api_applied;
	outcome.destination = isLocal == true ? "local" : result.protected_api_applied == true ? "protected" : "unprotected";
	return outcome;

}

bool isWorthAskingAgain(const ColleagueCallResult& result) {
	if (result.isAnswered == true || result.isCancelled == true || result.isInterrupted == true) return false;
	if (result.errorCode.has_value() == false) return false;
	switch (*result.errorCode) {
		case providers::TaskErrorCode::ProviderRateLimit:
		case providers::TaskErrorCode::NetworkError:
		case providers::TaskErrorCode::ResponseIncomplete:
		case providers::TaskErrorCode::ProtectedResponseInvalid:
			return true;
		default:
			return false;
	}
}

namespace {

std::atomic<long long>& firstPause() {
	static std::atomic<long long> milliseconds(3000);
	return milliseconds;
}

std::atomic<long long>& laterPause() {
	static std::atomic<long long> milliseconds(9000);
	return milliseconds;
}

} // namespace

void setPausesBeforeAskingAgainForTesting(long long first, long long later) {
	firstPause().store(first);
	laterPause().store(later);
}

void pauseBeforeAskingAgain(int attempt, const std::atomic<bool>* cancelled) {
	const long long most = attempt <= 1 ? firstPause().load() : laterPause().load();
	for (long long waited = 0; waited < most; waited += 200) {
		if (platform::lifecycle::is_closing() == true || (cancelled != nullptr && cancelled->load() == true)) return;
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
}

} // namespace singularity::document_review
