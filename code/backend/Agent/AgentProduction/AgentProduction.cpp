#include "Agent/AgentProduction/AgentProduction.hpp"
#include "Chat/MessageProvenance/MessageProvenance.hpp"

#include "Agent/AgentActions/AgentActions.hpp"
#include "Agent/AgentStore/AgentStore.hpp"
#include "Agent/ColleagueBrief/ColleagueBrief.hpp"
#include "Chat/ChatActions/ChatActions.hpp"
#include "Chat/ChatExecution/ChatExecution.hpp"
#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"
#include "Chat/ChatOutboundPreview/ChatOutboundPreview.hpp"
#include "Tool/DeviceTrust/DeviceTrust.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"
#include "Context/ProjectContextActions/ProjectContextActions.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Matter/SharedMatter/SharedMatter.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"
#include "WorkProduct/Playbook/Playbook.hpp"
#include "WorkProduct/PlaybookChanges/PlaybookChanges.hpp"
#include "WorkProduct/ReviewTable/ReviewTable.hpp"
#include "WorkProduct/WorkProductActions/WorkProductActions.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace singularity::agents {
namespace {

std::string clipped_name(const std::string &value) {
	return value.size() <= 120 ? value : value.substr(0, 120);
}

// --- The agent as a colleague: its brief, its first words, its reports ---

// "9:00 AM" for minutes after midnight.
std::string clock_words(int minute) {
	const int hours = minute / 60;
	const int minutes = minute % 60;
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "%d:%02d %s", hours % 12 == 0 ? 12 : hours % 12, minutes, hours < 12 ? "AM" : "PM");
	return buffer;
}

std::string weekday_words(int day) {
	static const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
	return day >= 0 && day <= 6 ? days[day] : "week";
}

std::string leader_name_of(const Agent &agent) {
	if (agent.follows_agent_id.empty() == true) return "";
	try {
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		const std::optional<Agent> leader = load_agent(agent.follows_agent_id);
		return leader.has_value() == true ? leader->name : "";
	} catch (const std::exception &) {
		return "";
	}
}

// Who the agent is and the work it stands for: its thread's standing
// instructions, carried by every request in the thread, whoever sends it - the
// firm's one way of professional communication and this colleague's role
// (Agent/ColleagueBrief, D-101).
// The colleague as a member of the firm (Agent::office): of the whole firm,
// never of one Matter (the owner, 2026-10-26: "she exists not within one
// project but within the whole firm"); whom it answers to is its office's
// word, not the lawyer whose computer runs it.
// Who the colleague works with in the firm (D-159's context): the member at
// this computer and the people of the firm's Matters known here, each in the
// office they state, and the firm's other AI colleagues in theirs - sorted, so
// its brief stays the same bytes, and read from its cache, until the team
// changes.
std::string firm_team_words(const std::string &agent_id) {
	std::vector<std::string> entries;
	std::set<std::string> named;
	const auto add = [&](const std::string &name, const std::string &office) {
		std::string clean = name;
		while (clean.empty() == false && std::isspace(static_cast<unsigned char>(clean.back())) != 0) clean.pop_back();
		while (clean.empty() == false && std::isspace(static_cast<unsigned char>(clean.front())) != 0) clean.erase(clean.begin());
		if (clean.empty() == true || named.insert(clean).second == false) return;
		entries.push_back(office.empty() == true ? clean : clean + " (" + office + ")");
	};
	try {
		const platform::ActorIdentity identity = platform::resolve_actor_identity();
		add(identity.display_name.value_or(std::string()), organisation::office_words(matter::own_office()));
		// The people of the firm's Matters known here, as their member records
		// state them.
		for (const matters::Matter &known : matter::list()) {
			if (known.directory_path.has_value() == false || known.directory_path->empty() == true) continue;
			for (const matter::SharedMember &member : matter::load_members(std::filesystem::u8path(*known.directory_path))) {
				if (member.actor_id == identity.actor_id) continue;
				add(member.name, organisation::office_words(organisation::MemberOffice{member.position, member.practice}));
			}
		}
	} catch (const std::exception &) {
	}
	try {
		for (const AgentView &view : list_agent_views(std::nullopt)) {
			if (view.agent.id == agent_id || view.agent.status == AgentStatus::Stopped || view.agent.status == AgentStatus::Draft) continue;
			add(view.agent.name, organisation::colleague_office_words(view.agent.office.title, view.agent.office.practice,
				view.agent.office.area));
		}
	} catch (const std::exception &) {
	}
	std::sort(entries.begin(), entries.end());
	if (entries.size() > 30) entries.resize(30);
	std::string words;
	for (const std::string &entry : entries) words += (words.empty() == true ? "" : "; ") + entry;
	return words;
}

ColleagueRole role_of(const Agent &agent) {
	ColleagueRole role;
	role.name = agent.name;
	role.office = agent.office.title;
	role.ranks_with = agent.office.ranks_with;
	role.practice = agent.office.practice;
	role.area = agent.office.area;
	// A colleague of the firm with no words of its own takes on the work its
	// members bring it, in any Matter (D-157); one of the zero state watches the
	// Matter it was made for.
	role.standing_work = agent.goal.empty() == false ? agent.goal : agent.matter_id.empty() == true
		? std::string("take on the work the firm's members bring you, in any of its Matters.")
		: std::string("tell the lawyer what changes in the Matter's documents and what it may mean.");
	role.manner = agent.manner;
	role.works_only_here = agent.model_policy == AgentModelPolicy::LocalOnly;
	try {
		role.firm = platform::resolve_actor_identity().organisation_name.value_or(std::string());
	} catch (const std::exception &) {
	}
	role.team = firm_team_words(agent.id);
	return role;
}

std::string agent_brief(const Agent &agent) {
	return colleague_brief(role_of(agent));
}

// Where the member who wrote to the colleague stands in the firm: their
// office as they state it (the person at this computer's own, or their record
// in the colleague's Matter), and where that stands to the colleague's rank.
void say_who_asks(const Agent &agent, TaskRunInput &input) {
	input.identity = colleague_identity(role_of(agent));
	if (input.given_by_actor_id.empty() == true) return;
	organisation::MemberOffice office;
	try {
		if (input.given_by_actor_id == platform::resolve_actor_identity().actor_id) {
			office = matter::own_office();
		} else {
			std::optional<matters::Matter> found;
			{
				const persistence::PersistenceLock lock(persistence::persistence_mutex());
				found = matters::load_matter(node::require_active_workspace_path(),
					input.matter_id.empty() == false ? input.matter_id : agent.matter_id);
			}
			if (found.has_value() == true && found->directory_path.has_value() == true) {
				for (const matter::SharedMember &member : matter::load_members(std::filesystem::u8path(*found->directory_path))) {
					if (member.actor_id == input.given_by_actor_id) office = organisation::MemberOffice{member.position, member.practice};
				}
			}
		}
	} catch (const std::exception &) {
	}
	input.asker_office = organisation::office_words(office);
	input.asker_standing = organisation::standing_words(agent.office.ranks_with, office.position);
}

// The agent's first words in its thread (D-101): who it is, the work it is here
// for, and when it takes it up. Of the firm, never of one Matter (D-130). Where
// it runs is not the lawyer's concern, and what needs the lawyer's word is said
// where it arises, not promised in advance.
std::string introduction(const Agent &agent) {
	std::string when;
	switch (agent.trigger) {
		case AgentTrigger::Manual: when = std::string(); break;
		case AgentTrigger::OnChange: when = "whenever a document there changes"; break;
		case AgentTrigger::Daily: when = "every day at " + clock_words(agent.daily_minute); break;
		case AgentTrigger::Weekly: when = "every " + weekday_words(agent.weekly_day) + " at " + clock_words(agent.daily_minute); break;
		case AgentTrigger::AfterAgent: {
			const std::string leader = leader_name_of(agent);
			when = "whenever " + (leader.empty() == true ? std::string("the colleague I follow") : leader) + " reports something";
			break;
		}
	}
	std::string words = "Hi, I'm " + agent.name + ". ";
	if (agent.mode == AgentMode::Changes) {
		const std::string folder = std::filesystem::path(agent.source_root).filename().string();
		words += "I'll keep an eye on " + (folder.empty() == true ? std::string("its folder") : "\u201C" + folder + "\u201D")
			+ " and tell you here what changes and what it may mean" + (when.empty() == true ? std::string() : ", " + when) + ".";
		if (agent.goal.empty() == false) words += " What to look out for: \u201C" + agent.goal + "\u201D";
		words += " Mention me here if you need anything else.";
	} else {
		if (agent.goal.empty() == false) words += "My brief: \u201C" + agent.goal + "\u201D ";
		words += when.empty() == true ? std::string("Mention me here whenever you need me.")
			: "I'll take it up " + when + " and tell you here; mention me if you need anything in between.";
	}
	return words;
}

std::string count_words(std::size_t count, const char *one, const char *many) {
	return std::to_string(count) + " " + (count == 1 ? one : many);
}

// What a check of the folder found, in the agent's own words: nothing when
// nothing changed, so the thread holds only what is worth reading.
std::string report_words(const Agent &agent, const AgentRun &run) {
	if (run.status == "completed" && run.is_baseline == true)
		return "I've read the folder: " + count_words(run.counts.files, "document", "documents")
			+ ". From now on I'll tell you here when something in it changes.";
	if (run.status == "failed" || run.status == "access_needed") {
		// Said once, not at every attempt; and once more when it pauses itself.
		if (agent.failed_in_a_row > 1 && agent.status != AgentStatus::Paused) return "";
		// Work it was given is no check of a folder: it says so as a colleague
		// would.
		std::string words = (run.trigger == "assignment" ? std::string("I couldn't do that just now. ")
			: std::string("I couldn't finish my check. ")) + run.error_message;
		if (agent.status == AgentStatus::Paused && agent.paused_reason.empty() == false)
			words += " I've paused myself until you resume me.";
		return words;
	}
	if (run.status != "completed" || run.documents.empty() == true) return "";
	std::vector<std::string> parts;
	if (run.counts.changed > 0) parts.push_back(count_words(run.counts.changed, "document changed", "documents changed"));
	if (run.counts.added > 0) parts.push_back(count_words(run.counts.added, "new document", "new documents"));
	if (run.counts.removed > 0) parts.push_back(count_words(run.counts.removed, "document removed", "documents removed"));
	if (run.counts.moved > 0) parts.push_back(count_words(run.counts.moved, "document moved", "documents moved"));
	if (run.counts.unreadable > 0) parts.push_back(count_words(run.counts.unreadable, "document I could not read", "documents I could not read"));
	std::string words = "I looked again: ";
	for (std::size_t index = 0; index < parts.size(); ++index)
		words += (index == 0 ? "" : (index + 1 == parts.size() ? " and " : ", ")) + parts[index];
	words += ".";
	if (run.findings.empty() == true) {
		if (run.analysis.status == "done") words += " Nothing in them needs your attention.";
		else if (run.analysis.reason.empty() == false) words += " " + run.analysis.reason;
	} else {
		words += run.findings.size() == 1 ? " One thing needs your eye:" : " " + std::to_string(run.findings.size()) + " things need your eye:";
		for (const RunFinding &finding : run.findings) words += "\n\n**" + finding.title + ".** " + finding.observation;
	}
	if (run.work_product_id.has_value() == true) words += "\n\nThe full report, with the changed words quoted, is in Work product.";
	return words;
}

std::string open_thread_of(const Agent &agent);

// What a stopped analysis, a missing model or a refused protection means for
// the lawyer, said in the run's own words: what happened, what is safe, what
// to do next.
std::string reason_for(providers::TaskErrorCode code) {
	switch (code) {
		case providers::TaskErrorCode::PrivacyProtectionUnavailable:
			return "Protection is not ready on this computer, so nothing was sent. The changes are listed; run the check again "
				"once protection is ready.";
		case providers::TaskErrorCode::ProtectedPayloadLeakDetected:
			return "Protection could not hide everything it should, so nothing was sent. The changes are listed for you to read.";
		case providers::TaskErrorCode::ProviderAuthentication:
			return "The model's provider refused the saved key. Check the connection in Settings, then run the check again.";
		case providers::TaskErrorCode::ProviderRateLimit:
			return "The model's provider is busy. The changes are listed; run the check again in a few minutes.";
		case providers::TaskErrorCode::NetworkError:
			return "The model could not be reached. The changes are listed; run the check again when the connection is back.";
		case providers::TaskErrorCode::ContextCapacityExceeded:
			return "The changes were more than the model can read at once, so nothing was sent. They are listed for you to read.";
		case providers::TaskErrorCode::UnsupportedModel:
		case providers::TaskErrorCode::ProviderRejectedRequest:
			return "The model refused this analysis. The firm's administrator can choose another model for me on my page; then "
				"run the check again.";
		default:
			return "The analysis could not be made. The changes are listed; run the check again to analyse them.";
	}
}

bool is_withheld(providers::TaskErrorCode code) {
	return code == providers::TaskErrorCode::PrivacyProtectionUnavailable
		|| code == providers::TaskErrorCode::ProtectedPayloadLeakDetected
		|| code == providers::TaskErrorCode::ContextCapacityExceeded;
}

// Whether the next check analyses the same changes again, as reason_for tells
// the lawyer: not when the same words would meet the same refusal.
bool analyses_again(providers::TaskErrorCode code) {
	return code != providers::TaskErrorCode::ProtectedPayloadLeakDetected
		&& code != providers::TaskErrorCode::ContextCapacityExceeded;
}

std::optional<MatterFolder> matter_folder_of(const std::string &matter_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	const std::optional<matters::Matter> matter = matters::load_matter(workspace, matter_id);
	if (matter.has_value() == false || matter->directory_path.has_value() == false || matter->directory_path->empty() == true)
		return std::nullopt;
	MatterFolder folder;
	folder.name = matter->name;
	folder.folder = std::filesystem::path(*matter->directory_path);
	for (const matters::MatterDocument &document : matter->documents)
		if (document.relative_path.empty() == false) folder.document_ids[document.relative_path] = document.id;
	// What Singularity wrote for an agent's own change once the lawyer
	// approved it: the version, and whose operation it was.
	std::map<std::string, std::string> agentOfOperation;
	for (const matters::MatterTask &task : matter->tasks)
		if (task.agent_id.empty() == false && task.agent_operation_id.has_value() == true)
			agentOfOperation[*task.agent_operation_id] = task.agent_id;
	for (const documents::DocumentEvent &event : matter->document_events) {
		if (event.kind != documents::DocumentEventKind::Changed || event.actor != "singularity") continue;
		const auto agent = agentOfOperation.find(event.operation_id);
		if (event.operation_id.empty() == true || agent == agentOfOperation.end()) continue;
		folder.written_by_agents.push_back(AgentWrittenVersion{event.relative_path, event.fingerprint, agent->second});
	}
	return folder;
}

ExtractedText extract_text(const std::filesystem::path &path) {
	ExtractedText text;
	documents::DocumentExtractionRequest request;
	request.path = path;
	const documents::DocumentExtractionResult result = documents::LocalDocumentExtractor().extract(request);
	switch (result.status) {
		case documents::ExtractionStatus::ready: {
			std::vector<std::pair<std::size_t, std::size_t>> pages;
			for (const documents::DocumentPageBoundary &page : result.structure.page_boundaries)
				pages.push_back({page.page_number, page.text_offset});
			text.is_readable = true;
			text.blocks = blocks_from_text(result.text, pages);
			return text;
		}
		case documents::ExtractionStatus::empty:
			text.reason = "The document has no words to read.";
			return text;
		case documents::ExtractionStatus::ocr_required:
			text.reason = "A scanned document: its words are an image Singularity cannot read yet. Read it yourself.";
			return text;
		case documents::ExtractionStatus::unsupported:
			text.reason = "Singularity does not read this kind of file. Open it to see what changed.";
			return text;
		case documents::ExtractionStatus::unreadable:
			text.reason = "Singularity could not open this file: it may be damaged or protected.";
			return text;
	}
	text.reason = "Singularity could not read this file.";
	return text;
}

// The operation the analysis is in the Matter: Usage counts it, the Guard page
// and "What left this device" find its calls by its id. Never throws.
void record_operation(const AnalysisInput &input, const std::string &task_id, const routing::InferenceResult *result,
	const std::optional<providers::TaskErrorCode> &error, const std::string &provider_id, const std::string &model_id,
	const std::string &execution_kind, long long latency_ms) {
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		std::optional<matters::Matter> matter = matters::load_matter(workspace, input.matter_id);
		if (matter.has_value() == false) return;
		matters::MatterTask task;
		task.id = task_id;
		task.created_at = workspaces::current_timestamp();
		task.operation_type = "agent_run";
		task.operation_role = "reasoning";
		task.prompt = clipped_name("Data room watch: " + input.agent_name);
		task.requested_provider_id = provider_id;
		task.requested_model_id = model_id;
		task.provider_id = provider_id;
		task.model_id = model_id;
		task.execution_kind = execution_kind;
		if (execution_kind == "local") task.runtime_id = "local";
		if (result != nullptr) {
			task.response_text = result->text;
			task.protected_api_applied = result->protected_api_applied;
			task.privacy_metadata = result->privacy_metadata;
			task.protected_api_mode = result->privacy_metadata.mode;
			task.usage = result->usage;
			task.status = matters::TaskStatus::Success;
		} else {
			task.status = matters::TaskStatus::Error;
			task.error_code = error;
		}
		matter->tasks.push_back(task);
		matters::save_matter(workspace, *matter);

		routing::RoutingLogEntry entry;
		entry.matter_id = input.matter_id;
		entry.operation_id = task_id;
		entry.task_id = task_id;
		entry.time = task.created_at;
		entry.provider_id = provider_id;
		entry.model_id = model_id;
		entry.execution_kind = execution_kind;
		entry.legal_task_preset_id = "data-room-watch";
		entry.success = result != nullptr;
		entry.latency_ms = latency_ms;
		if (result != nullptr) {
			entry.privacy_metadata = result->privacy_metadata;
			entry.usage = result->usage;
			entry.finish_reason = result->finish_reason;
		}
		if (error.has_value() == true) entry.error_code = error;
		// A failed external call carries no protection result: its record says
		// the mode this computer's policy held it to, never an empty record's "off".
		if (result == nullptr && execution_kind == "external")
			entry.privacy_metadata.mode = protected_api::to_string(
				platform::load_app_settings().local_node_setup.protected_api_policy.mode);
		routing::append_routing_log_entry(workspace, entry);
	} catch (...) {
	}
}

AnalysisOutput analyse(const AnalysisInput &input) {
	AnalysisOutput output;
	// The model: only a local one, or what the Matter's AI Setup gives its
	// reasoning - the model its chats would use.
	std::optional<routing::ModelTarget> model;
	if (input.policy == AgentModelPolicy::LocalOnly) {
		const std::optional<std::string> local = routing::preferred_local_generative_model();
		if (local.has_value() == true) model = routing::ModelTarget{"local", *local};
		if (model.has_value() == false) {
			output.status = "withheld";
			output.reason = "This agent analyses only on this computer, and no local model is installed. The changes are "
				"listed; install a model in Models to analyse them.";
			return output;
		}
	} else {
		// The model chosen on its page, when one is (D-185).
		try {
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			const std::optional<Agent> agent = load_agent(input.agent_id);
			if (agent.has_value() == true && agent->model_id.empty() == false)
				model = routing::ModelTarget{agent->model_provider_id, agent->model_id};
		} catch (const std::exception &) {
		}
		chat::OperationSetupRequest setupRequest;
		setupRequest.project_id = input.matter_id;
		if (model.has_value() == false) model = chat::resolve_operation_setup(setupRequest).primary_model;
		if (model.has_value() == false) {
			output.status = "withheld";
			output.reason = "No AI model is set for me yet, so nothing was analysed. The firm's administrator can choose one on "
				"my page; then run the check again.";
			return output;
		}
	}
	const bool isLocal = model->provider_id == "local";
	output.provider_id = model->provider_id;
	output.model_id = model->model_id;
	output.model_label = model->model_id;
	output.destination = isLocal == true ? "local" : "protected";

	const std::string taskId = matters::generate_task_id();
	output.operation_id = taskId;
	routing::InferenceRequest request;
	request.call_purpose = routing::observability::CallPurpose::ContextAutomation;
	request.model_target = model;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		request.workspace_id = platform::load_app_settings().local_node_setup.active_workspace_id;
	}
	if (isLocal == true) {
		const auto window = routing::estimate_model_context_window_tokens(*model);
		request.maximum_output_tokens = window ? std::max<std::uint64_t>(1024, *window / 4) : 2048;
	} else {
		request.maximum_output_tokens = 8192;
		request.project_entity_hints = context_actions::memory_protection_hints(input.matter_id);
	}
	request.prompt = input.prompt;
	request.research_mode = research::ResearchMode::Off;
	request.research_final_output_contract = providers::GenerationOutputContract{"data_room_findings", input.contract};
	request.operation_symbols = std::make_shared<protected_api::papinian::OperationProtectedSymbolTable>();
	request.operation_symbols->operation_id = taskId;

	// A stop the lawyer asks for aborts the call in flight.
	const std::shared_ptr<platform::HttpCancelToken> token = std::make_shared<platform::HttpCancelToken>();
	std::atomic<bool> isDone(false);
	std::thread watcher;
	if (input.cancelled != nullptr) {
		watcher = std::thread([&]() {
			while (isDone.load() == false) {
				if (input.cancelled->load() == true) {
					token->cancel();
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			}
		});
	}
	const auto finishWatcher = [&]() {
		isDone.store(true);
		if (watcher.joinable() == true) watcher.join();
	};

	const auto started = std::chrono::steady_clock::now();
	const auto elapsed = [&]() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	};
	try {
		routing::require_execution_ready(request);
		routing::InferenceResult result;
		{
			const platform::HttpCancelScope scope(token);
			result = routing::execute(request);
		}
		finishWatcher();
		record_operation(input, taskId, &result, std::nullopt, model->provider_id, model->model_id,
			isLocal == true ? "local" : "external", elapsed());
		output.status = "done";
		output.text = result.text;
		output.protection_mode = result.privacy_metadata.mode;
		if (isLocal == false && result.protected_api_applied == false) output.destination = "unprotected";
		output.input_tokens = result.usage.input_tokens.value_or(0);
		output.output_tokens = result.usage.output_tokens.value_or(0);
		if (result.usage.cost_micros.has_value() == true) {
			output.cost = static_cast<double>(*result.usage.cost_micros) / 1000000.0;
			output.currency = result.usage.cost_currency.value_or("USD");
		}
		return output;
	} catch (const providers::TaskError &error) {
		finishWatcher();
		if (token->is_cancelled() == true) {
			output.status = "cancelled";
			output.reason = "The check was stopped.";
			return output;
		}
		record_operation(input, taskId, nullptr, error.code(), model->provider_id, model->model_id,
			isLocal == true ? "local" : "external", elapsed());
		output.status = is_withheld(error.code()) == true ? "withheld" : "failed";
		output.reason = reason_for(error.code());
		output.analyses_again = analyses_again(error.code());
		return output;
	} catch (const std::exception &) {
		finishWatcher();
		if (token->is_cancelled() == true) {
			output.status = "cancelled";
			output.reason = "The check was stopped.";
			return output;
		}
		record_operation(input, taskId, nullptr, providers::TaskErrorCode::Unknown, model->provider_id, model->model_id,
			isLocal == true ? "local" : "external", elapsed());
		output.status = "failed";
		output.reason = reason_for(providers::TaskErrorCode::Unknown);
		return output;
	}
}

std::optional<std::string> write_report(const Agent &agent, const AgentRun &run) {
	work_product::CreateWorkProductCommand command;
	command.project_id = agent.matter_id;
	command.type_id = matters::kWorkProductTypeAnalysis;
	command.title = report_title(run);
	command.content = report_markdown(agent, run);
	std::map<std::string, std::string> documentByEvidence;
	std::map<std::string, std::string> nameByDocument;
	for (const RunDocument &document : run.documents) {
		if (document.document_id.has_value() == false) continue;
		const std::size_t slash = document.relative_path.find_last_of('/');
		const std::string name = slash == std::string::npos ? document.relative_path : document.relative_path.substr(slash + 1);
		nameByDocument[*document.document_id] = name;
		for (const std::string &id : document.evidence_ids) documentByEvidence[id] = *document.document_id;
	}
	for (const auto &[documentId, name] : nameByDocument) {
		context::ResolvedSourceReference reference;
		reference.document_id = documentId;
		reference.document_name = name;
		command.source_refs.push_back(reference);
	}
	for (const RunFinding &finding : run.findings) {
		matters::Finding row;
		row.id = matters::generate_finding_id();
		row.matter_id = agent.matter_id;
		row.title = finding.title;
		row.description = finding.observation;
		if (finding.analysis.empty() == false) row.recommendation = finding.analysis;
		for (const std::string &id : finding.evidence_ids) {
			const auto found = documentByEvidence.find(id);
			if (found == documentByEvidence.end()) continue;
			const bool isListed = std::any_of(row.source_refs.begin(), row.source_refs.end(),
				[&](const context::ResolvedSourceReference &reference) { return reference.document_id == found->second; });
			if (isListed == true) continue;
			context::ResolvedSourceReference reference;
			reference.document_id = found->second;
			reference.document_name = nameByDocument[found->second];
			row.source_refs.push_back(reference);
		}
		command.findings.push_back(row);
	}
	command.origin.kind = matters::WorkProductOriginKind::Agent;
	if (run.analysis.status == "done" && run.analysis.operation_id.has_value() == true)
		command.origin.reference_id = run.analysis.operation_id;
	return work_product::create(command).id;
}

// The request a task run sends in the agent's chat: the lawyer's own
// instruction, said as theirs, with which documents changed since the last
// run, by name. The documents' words come as the request's sources, read and
// protected as every request's are.
std::string task_prompt(const TaskRunInput &input) {
	// Work the lawyer gave it once: the work is the request, the agent's
	// standing instruction the role it does it in.
	// Work the lawyer gave it is exactly the lawyer's words; who the agent is
	// travels as its thread's brief (agent_brief).
	// A member's message in a thread is said as theirs: who wrote to the agent,
	// then their words, so a greeting reads as one and a task as a task.
	if (input.is_assignment == true) {
		if (input.reply_to_message_id.empty() == false && input.given_by.empty() == false)
			return input.given_by + " wrote to you in the thread:\n\n" + input.instruction;
		return input.instruction;
	}
	// A run the agent began itself: its standing work, and why it runs now.
	std::string prompt = input.instruction;
	prompt += "\n\n(";
	if (input.is_first_run == true) prompt += "Your first run. ";
	if (input.woken_by_agent.empty() == false)
		prompt += "You run now because " + input.woken_by_agent + ", an agent of this Matter, reported something for review; its report is in the Matter's Work Product. ";
	if (input.change_note.empty() == false) prompt += "Since your last run, in your folder:\n" + input.change_note + ")";
	else if (input.is_first_run == true) {
		if (prompt.back() == ' ') prompt.pop_back();
		prompt += ")";
	}
	else prompt += "No document in your folder changed since your last run.)";
	return prompt;
}

// The opening words of an answer, without its source handles.
std::string excerpt_of(const std::string &answer) {
	std::string text;
	for (std::size_t index = 0; index < answer.size(); ++index) {
		if (answer.compare(index, 8, "[SOURCE:") == 0) {
			const std::size_t end = answer.find(']', index);
			if (end != std::string::npos) {
				index = end;
				continue;
			}
		}
		const char character = answer[index];
		if (character == '#' || character == '*') continue;
		if (character == '\n' || character == '\r' || character == '\t') {
			if (text.empty() == false && text.back() != ' ') text += ' ';
			continue;
		}
		text += character;
		if (text.size() >= 360) break;
	}
	while (text.empty() == false && text.back() == ' ') text.pop_back();
	std::size_t end = text.size();
	while (end > 0 && (static_cast<unsigned char>(text[end - 1]) & 0xC0) == 0x80) --end;
	if (end < text.size() && end > 0) text = text.substr(0, end - 1);
	return text.size() >= 359 ? text + "…" : text;
}

bool is_live_chat(const std::string &project_id, const std::string &chat_id) {
	try {
		for (const matters::MatterChat &chat : chat::list(project_id).chats)
			if (chat.id == chat_id && chat.deleted_at.empty() == true) return true;
	} catch (const std::exception &) {
	}
	return false;
}

// An agent that checks the Matter against the firm's Playbook: its task is a
// Review Table of the contracts in its folder against the Playbook's
// positions, kept and brought up to date run after run, and the tracked
// changes that bring each departing contract to the firm's position - every one
// waiting for the lawyer. What it found, it tells its thread.
TaskRunOutput run_playbook_review(const Agent &agent, const TaskRunInput &input, const std::string &chat_id,
		TaskRunOutput output) {
	const auto fail = [&](const std::string &reason) {
		output.status = "failed";
		output.reason = reason;
		output.task.state = "failed";
		output.task.message = reason;
		return output;
	};
	// Singularity closed during the check (Beta 1 #16): no failure, no Stop and
	// no note; the scheduler begins it again at the next launch.
	const auto closed_on = [&]() {
		output.status = "interrupted";
		output.reason = "Singularity closed while the agent checked the contracts. What it answered is kept in its table.";
		output.task.state = "interrupted";
		output.task.message = output.reason;
		return output;
	};
	const std::optional<playbooks::Playbook> playbook = playbooks::get(agent.playbook_id);
	if (playbook.has_value() == false)
		return fail("The Playbook this agent checks against is no longer on this computer. Choose one in its settings.");
	// A contract that has just arrived is read in this run, whether or not
	// anyone opened the Matter's documents in this session (R0): the files the
	// run found new or changed are brought into the Matter first, as its folder
	// watch would bring them.
	if (input.changed_files.empty() == false) {
		try {
			std::vector<std::string> paths;
			for (const std::filesystem::path &file : input.changed_files) paths.push_back(file.u8string());
			(void)matter::detect_affected_paths(input.matter_id, paths);
			(void)matter::process_project_documents(input.matter_id);
		} catch (const std::exception &) {
		}
	}
	// The contracts it reads: the Matter's documents in its folder.
	std::vector<std::string> documentIds;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::optional<matters::Matter> matter = matters::load_matter(node::require_active_workspace_path(), input.matter_id);
		if (matter.has_value() == false) return fail("This agent's Matter is not open in Singularity on this computer.");
		const std::filesystem::path folder = std::filesystem::u8path(matter->directory_path.value_or(std::string()));
		const std::filesystem::path root = std::filesystem::u8path(agent.source_root).lexically_normal();
		for (const matters::MatterDocument &document : matter->documents) {
			if (document.available == false) continue;
			std::string extension = std::filesystem::u8path(document.relative_path).extension().u8string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			if (extension != ".docx" && extension != ".doc" && extension != ".pdf" && extension != ".rtf"
					&& extension != ".txt" && extension != ".md")
				continue;
			// Inside its folder, folder by folder: "Contracts Archive" is not
			// inside "Contracts".
			const std::filesystem::path path = (folder / std::filesystem::u8path(document.relative_path)).lexically_normal();
			if (agent.source_root.empty() == false && is_inside_folder(path, root) == false) continue;
			documentIds.push_back(document.id);
		}
	}
	if (documentIds.empty() == true) return fail("There is no contract in this agent's folder to check against the Playbook.");
	const bool keptHere = input.policy == AgentModelPolicy::LocalOnly;

	// Its table: the one it keeps, made again for a Playbook changed since.
	std::string tableId = agent.review_table_id;
	try {
		if (tableId.empty() == false) {
			const review_table::ReviewTableView kept = review_table::get(input.matter_id, tableId);
			if (kept.table.playbook_id != playbook->id || kept.table.playbook_version != playbook->version) tableId.clear();
		}
	} catch (const std::exception &) {
		tableId.clear();
	}
	try {
		if (tableId.empty() == true) {
			tableId = playbooks::review_table(input.matter_id, playbook->id, documentIds, agent.name + ": " + playbook->name,
				keptHere).id;
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			std::optional<Agent> current = load_agent(agent.id);
			if (current.has_value() == true) {
				current->review_table_id = tableId;
				save_agent(*current);
			}
		} else {
			review_table::UpdateReviewTable update;
			update.project_id = input.matter_id;
			update.table_id = tableId;
			update.document_ids = documentIds;
			if (keptHere == true) update.local_only = true;
			(void)review_table::update(update);
		}
	} catch (const std::exception &error) {
		return fail(error.what());
	}

	// A stop the lawyer asks for stops the table where it is.
	std::atomic<bool> isDone(false);
	std::thread watcher;
	if (input.cancelled != nullptr) {
		watcher = std::thread([&]() {
			while (isDone.load() == false) {
				if (input.cancelled->load() == true) {
					try {
						review_table::request_stop(input.matter_id, tableId);
					} catch (const std::exception &) {
					}
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			}
		});
	}
	const auto finishWatcher = [&]() {
		isDone.store(true);
		if (watcher.joinable() == true) watcher.join();
	};
	review_table::ReviewTableView view;
	try {
		const routing::ScopedLocalOnlyWork localOnly(keptHere);
		view = review_table::run(input.matter_id, tableId);
	} catch (const std::exception &error) {
		finishWatcher();
		return fail(error.what());
	}
	finishWatcher();
	// Singularity is closing: what the table answered is kept, nothing more is
	// prepared or said, and the check is begun again at the next launch.
	if (platform::lifecycle::is_closing()) return closed_on();
	if (input.cancelled != nullptr && input.cancelled->load() == true) {
		output.status = "cancelled";
		output.reason = "The check was stopped. The contracts it answered are kept in its table.";
		output.task.state = "stopped";
		output.task.message = output.reason;
		return output;
	}

	// The changes, for the contracts this run answered that depart from the
	// firm's position. A stop the lawyer asks for now reaches the change being
	// prepared, and no other is begun (playbook_changes::prepare).
	std::vector<std::string> answeredNow;
	for (const matters::ReviewRow &row : view.rows)
		if (row.run_id == view.table.run_id && row.state == "answered") answeredNow.push_back(row.document_id);
	std::vector<playbook_changes::PreparedChange> changes;
	if (answeredNow.empty() == false) {
		try {
			const routing::ScopedLocalOnlyWork localOnly(keptHere);
			changes = playbook_changes::prepare(input.matter_id, tableId, answeredNow,
				playbook_changes::PreparedBy{agent.id, agent.name}, input.cancelled);
		} catch (const std::exception &error) {
			output.task.message = std::string("The changes could not be prepared: ") + error.what();
		}
	}
	if (platform::lifecycle::is_closing()) return closed_on();
	const bool isStopped = input.cancelled != nullptr && input.cancelled->load() == true;

	// What it found, in its own words. Only a contract whose answers are
	// current is said to meet or depart from the firm's position: one that could
	// not be answered is said to be unchecked, never to conform.
	std::size_t departing = 0;
	std::size_t points = 0;
	std::vector<std::string> meets;
	std::vector<std::string> unchecked;
	// A contract whose approved change is in it now: its departures are what
	// that change was made for, so it is neither departing nor said to meet.
	std::vector<std::string> changed;
	for (const matters::ReviewDocument &document : view.table.documents) {
		const auto status = std::find_if(view.statuses.begin(), view.statuses.end(),
			[&](const review_table::RowStatus &entry) { return entry.document_id == document.document_id; });
		if (status == view.statuses.end() || status->status != "current") {
			unchecked.push_back(document.name);
			continue;
		}
		if (status->change_written == true) {
			changed.push_back(document.name);
			continue;
		}
		std::size_t here = 0;
		for (const matters::ReviewRow &row : view.rows) {
			if (row.document_id != document.document_id) continue;
			for (const matters::ReviewCell &cell : row.cells)
				if (cell.assessment == "deviates" || cell.assessment == "missing") ++here;
		}
		if (here > 0) ++departing;
		else meets.push_back(document.name);
		points += here;
	}
	const std::size_t total = view.table.documents.size();
	const std::size_t checked = total - unchecked.size();
	const auto count = [](std::size_t number, const char *one, const char *many) {
		return std::to_string(number) + " " + (number == 1 ? one : many);
	};
	const auto listed = [](const std::vector<std::string> &names) {
		std::string words;
		for (std::size_t index = 0; index < names.size(); ++index) {
			if (index > 0) words += index + 1 == names.size() ? " and " : ", ";
			words += names[index];
		}
		return words;
	};
	const std::string against = " against " + playbook->name + " (version " + std::to_string(playbook->version) + ").";
	std::string said;
	if (checked == 0) {
		said = "I could not check the " + count(total, "contract", "contracts") + against;
	} else {
		said = "I checked " + (checked == total ? count(total, "contract", "contracts")
			: std::to_string(checked) + " of the " + count(total, "contract", "contracts")) + against + " ";
		if (points > 0)
			said += count(points, "point departs", "points depart") + " from the firm's position, in "
				+ count(departing, "contract", "contracts") + ".";
		else if (changed.empty() == true)
			said += checked == 1 ? "It meets the firm's position." : "Every one I checked meets the firm's position.";
		if (meets.empty() == false && (points > 0 || changed.empty() == false))
			said += std::string(points > 0 ? " " : "") + (meets.size() == 1 ? meets.front() + " meets it throughout."
				: count(meets.size(), "contract meets", "contracts meet") + " it throughout.");
		if (changed.empty() == false)
			said += std::string(points > 0 || meets.empty() == false ? " " : "") + "The changes you approved are in "
				+ listed(changed) + ".";
		if (unchecked.empty() == false)
			said += "\n\nNot checked, so not known to meet it: " + listed(unchecked) + ".";
	}
	std::size_t waiting = 0;
	std::size_t asking = 0;
	for (const playbook_changes::PreparedChange &change : changes) {
		if (change.state == "waiting_for_approval") ++waiting;
		if (change.state == "waiting_for_answer") ++asking;
	}
	if (waiting > 0)
		said += "\n\nI prepared tracked changes for " + count(waiting, "contract", "contracts")
			+ ". Each waits for your approval in its own chat:";
	for (const playbook_changes::PreparedChange &change : changes)
		if (change.state == "waiting_for_approval") said += "\n- Playbook changes: " + change.document_name;
	for (const playbook_changes::PreparedChange &change : changes) {
		if (change.state == "waiting_for_approval") continue;
		said += "\n\n" + change.document_name + ": ";
		if (change.state == "waiting_for_answer")
			said += "before I change it I need your decision" + (change.question.empty() ? std::string(".")
				: ": " + change.question + (change.question.back() == '?' || change.question.back() == '.' ? "" : "."))
				+ " The question waits for your answer in its chat, Playbook changes: " + change.document_name
				+ ". Nothing in the contract changes until you answer, and the change I then prepare waits for your "
				"approval.";
		else if (change.state == "completed")
			said += "no change was proposed. Why is in its chat, Playbook changes: " + change.document_name + ".";
		else
			said += change.message.empty() ? std::string("its change could not be prepared.") : change.message;
	}
	if (isStopped == true) said += "\n\nI was stopped while I prepared the changes, and began no other.";
	if (view.table.run_message.empty() == false) said += "\n\n" + view.table.run_message;
	said += "\n\nThe table, with the words each answer rests on, is in Review tables.";
	try {
		(void)chat::post_agent_message(input.matter_id, chat_id, agent.id, agent.name, said, keptHere);
	} catch (const std::exception &) {
	}
	if (isStopped == true) {
		output.status = "cancelled";
		output.reason = "The check was stopped. What it answered is kept in its table, and each change it prepared "
			"waits for your approval.";
		output.task.state = "stopped";
		output.task.message = output.reason;
	} else if (checked == 0) {
		// Nothing was checked: a run that failed, and counts toward the agent
		// pausing itself, never one that reports a result.
		output.status = "failed";
		output.reason = view.table.run_message.empty() == false ? view.table.run_message
			: std::string("None of the contracts could be checked. Run it again, or the firm's administrator can choose another "
				"model for me on my page.");
		output.task.state = "failed";
		output.task.message = output.reason;
	} else {
		output.status = "completed";
		output.task.state = asking > 0 ? "waiting_for_answer" : waiting > 0 ? "waiting_for_approval" : "completed";
	}
	output.task.answer_excerpt = said.substr(0, said.find('\n'));
	if (view.cost_micros.has_value() == true) {
		output.task.cost = static_cast<double>(*view.cost_micros) / 1000000.0;
		output.task.currency = view.cost_currency.empty() ? std::string("USD") : view.cost_currency;
	}
	if (view.table.work_product_id.empty() == false) output.work_product_id = view.table.work_product_id;
	return output;
}

// The document work the application installed (install_thread_document_work):
// none until it does.
std::mutex &thread_document_work_mutex() {
	static std::mutex mutex;
	return mutex;
}

ThreadDocumentWork &installed_thread_document_work() {
	static ThreadDocumentWork work;
	return work;
}

TaskRunOutput run_task(const TaskRunInput &given) {
	TaskRunInput input = given;
	TaskRunOutput output;
	std::optional<Agent> agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = load_agent(input.agent_id);
	}
	if (agent.has_value() == false) {
		output.reason = "This agent no longer exists.";
		output.task.state = "failed";
		output.task.message = output.reason;
		return output;
	}
	// Where it answers: the thread where someone named it, carrying its brief
	// with the request; otherwise its own thread, whose brief is the chat's - in
	// its home: the Matter it was made for, or the member's desk (D-157).
	std::string chatId;
	bool isElsewhere = false;
	if (input.reply_chat_id.empty() == false && is_live_chat(input.matter_id, input.reply_chat_id) == true) {
		chatId = input.reply_chat_id;
		isElsewhere = chatId != agent->chat_id.value_or("");
	} else {
		chatId = open_thread_of(*agent);
		input.matter_id = agent->matter_id.empty() == false ? agent->matter_id : matter::desk_project_id();
	}
	if (chatId.empty() == true) {
		output.reason = "This agent's Matter is not open in Singularity on this computer.";
		output.task.state = "failed";
		output.task.message = output.reason;
		return output;
	}
	if (isElsewhere == false && agent->chat_id != chatId) output.made_chat_id = chatId;
	output.task.chat_id = chatId;
	// An agent that checks against the firm's Playbook does its task as a
	// Review Table and the changes it calls for.
	if (agent->playbook_id.empty() == false && input.is_assignment == false)
		return run_playbook_review(*agent, input, chatId, std::move(output));
	// Work a lawyer gives any colleague is understood by the colleague first:
	// an answer in words, a question, or work on the Matter's documents - one
	// or several, of any kind (D-115). Only an answer in words is left to the
	// colleague's ordinary answer below.
	// What the colleague did in this thread, as its record keeps it: its
	// ordinary answer has it before the question, so a question about a draft
	// is answered from what the draft holds (D-120).
	std::string workRecord;
	// A run the colleague begins itself is its talk as well (D-129): one light
	// call, the Matter's data only as it asks for it.
	// Who it is in the firm, and where whoever wrote to it stands.
	TaskRunInput told = input;
	say_who_asks(*agent, told);
	if (input.is_assignment == false) {
		ThreadDocumentWork work;
		{
			const std::lock_guard<std::mutex> lock(thread_document_work_mutex());
			work = installed_thread_document_work();
		}
		if (work.run_own) {
			std::optional<TaskRunOutput> done = work.run_own(*agent, told, chatId, agent_brief(*agent), task_prompt(input), output);
			if (done.has_value() == true) return std::move(*done);
		}
	}
	if (input.is_assignment == true) {
		ThreadDocumentWork work;
		{
			const std::lock_guard<std::mutex> lock(thread_document_work_mutex());
			work = installed_thread_document_work();
		}
		if (work.is_requested && work.run
				&& work.is_requested(input.matter_id, chatId, agent->id, input.instruction) == true) {
			std::optional<TaskRunOutput> done = work.run(*agent, told, chatId, agent_brief(*agent), output);
			if (done.has_value() == true) return std::move(*done);
		}
		if (work.record_of) {
			try {
				workRecord = work.record_of(input.matter_id, chatId, agent->id);
			} catch (const std::exception &) {
			}
		}
	}

	chat::ChatRequestComposition composition;
	composition.project_id = input.matter_id;
	composition.chat_id = chatId;
	composition.prompt = workRecord.empty() == true ? task_prompt(input)
		: "What you did in this thread, as Singularity recorded it from the saved drafts - more exact than your earlier "
			"messages; answer from it, and say only what the question needs:\n<<<YOUR WORK>>>\n" + workRecord
			+ "<<<END YOUR WORK>>>\n\n" + task_prompt(input);
	composition.can_ask_lawyer = false;
	// Nobody watches an agent's run: a web search it needs keeps its place and
	// the run returns at once, so one undecided search never holds every other
	// agent's work (Beta 1 #14).
	composition.unattended = true;
	composition.agent_initiated = input.is_assignment == false;
	// The operation is the agent's, whatever path later carries it on - a
	// resume, a retry, Send again: every change it would make waits for the
	// lawyer (Tool/OperationAuthority, recorded when it begins).
	composition.agent_run = true;
	composition.agent_name = agent->name;
	composition.answers_message_id = input.reply_to_message_id;
	// A colleague's work is theirs, whichever computer runs the agent.
	if (input.given_by_actor_id.empty() == false) {
		composition.author_actor_id = input.given_by_actor_id;
		composition.author_name = input.given_by;
	}
	if (isElsewhere == true) {
		composition.agent_id = agent->id;
		composition.agent_brief = agent_brief(*agent);
	}
	if (input.policy == AgentModelPolicy::LocalOnly) {
		const std::optional<std::string> local = routing::preferred_local_generative_model();
		if (local.has_value() == false) {
			output.reason = "This agent works only on this computer, and no local model is installed. Install one in Models.";
			output.task.state = "failed";
			output.task.message = output.reason;
			return output;
		}
		composition.one_time_primary_model_override = routing::ModelTarget{"local", *local};
		// Recorded with the operation when it begins, so every path that carries
		// it on - Resume, Retry, Send again, a restart - keeps it here too.
		composition.local_only = true;
	}
	const std::string operationId = "agent-op-" + secure_random_hex(10);
	output.task.operation_id = operationId;

	// A stop the lawyer asks for stops the request as Stop in a chat does.
	std::atomic<bool> isDone(false);
	std::thread watcher;
	if (input.cancelled != nullptr) {
		watcher = std::thread([&]() {
			while (isDone.load() == false) {
				if (input.cancelled->load() == true) {
					try {
						(void)chat::request_stop(input.matter_id, operationId);
					} catch (const std::exception &) {
					}
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			}
		});
	}
	chat::ChatExecutionOutcome outcome;
	try {
		// It may do what the Matter's own mode lets any member do (D-158).
		// An agent that works only on this computer sends nothing out: no web
		// research is offered to it, and any network tool is denied.
		const routing::ScopedLocalOnlyWork localOnly(input.policy == AgentModelPolicy::LocalOnly);
		outcome = chat::run_chat_request(operationId, composition);
	} catch (const std::exception &error) {
		isDone.store(true);
		if (watcher.joinable() == true) watcher.join();
		output.reason = error.what();
		output.task.state = "failed";
		output.task.message = output.reason;
		return output;
	}
	isDone.store(true);
	if (watcher.joinable() == true) watcher.join();

	output.task.task_id = outcome.task_id;
	output.task.message = outcome.message;
	std::string answer;
	if (outcome.task_id.empty() == false) {
		try {
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			const std::optional<matters::Matter> matter = matters::load_matter(node::require_active_workspace_path(), input.matter_id);
			if (matter.has_value() == true)
				for (const matters::MatterTask &task : matter->tasks) {
					if (task.id != outcome.task_id) continue;
					if (task.response_text.has_value() == true) answer = *task.response_text;
					// What the request cost, as the provider billed it: the
					// agent's month counts it.
					if (task.usage.cost_micros.has_value() == true) {
						output.task.cost = static_cast<double>(*task.usage.cost_micros) / 1000000.0;
						output.task.currency = task.usage.cost_currency.value_or("USD");
					}
				}
		} catch (const std::exception &) {
		}
	}
	output.task.answer_excerpt = excerpt_of(answer);
	switch (outcome.state) {
		case chat::ChatOperationState::Completed:
			output.status = "completed";
			output.task.state = "completed";
			break;
		case chat::ChatOperationState::WaitingForApproval: {
			output.status = "completed";
			// What waits: a web search it needs, or a change it proposes.
			bool isSearch = false;
			try {
				const chat::ChatOperationStatus waiting = chat::operation_status(input.matter_id, operationId);
				isSearch = waiting.approval.has_value() && waiting.approval->effect == tools::ToolEffectClass::ExternalNetwork;
			} catch (const std::exception &) {
			}
			output.task.state = isSearch ? "waiting_for_search" : "waiting_for_approval";
			output.task.message = isSearch
				? "A web search it needs waits for your approval in its chat. It goes on once you decide."
				: "A change it proposes waits for your approval in its chat.";
			break;
		}
		case chat::ChatOperationState::WaitingForAnswer:
			output.status = "completed";
			output.task.state = "waiting_for_answer";
			output.task.message = "It asked you a question in its chat. It goes on once you answer.";
			break;
		case chat::ChatOperationState::Interrupted:
			// Singularity closed while the agent worked on this: neither a failure
			// nor a Stop. The scheduler begins it again at the next launch.
			output.status = "interrupted";
			output.task.state = "interrupted";
			output.reason = "Singularity closed while the agent worked on this.";
			break;
		case chat::ChatOperationState::Stopped:
		case chat::ChatOperationState::Stopping:
			// A Stop - on the agent, or in its chat - is never a failure that counts
			// toward the agent pausing itself.
			output.status = "cancelled";
			output.task.state = "stopped";
			output.reason = "The run was stopped before it finished.";
			break;
		default:
			output.status = "failed";
			output.task.state = "failed";
			output.reason = outcome.message.empty() == true ? "The task could not be carried out. Nothing was changed; run it again."
				: outcome.message;
			break;
	}
	// A finished answer is kept as Work Product, as a lawyer keeps a chat's.
	if (output.task.state == "completed" && answer.empty() == false) {
		try {
			const std::string date = workspaces::current_timestamp().substr(0, 10);
			output.work_product_id = work_product::retain_task_result(input.matter_id, outcome.task_id,
				matters::kWorkProductTypeAnalysis, clipped_name(input.agent_name + ", " + date)).id;
		} catch (const std::exception &) {
		}
	}
	return output;
}

std::string open_thread_of(const Agent &agent) {
	if (agent.status == AgentStatus::Draft) return "";
	// The Matter it was made for, in the zero state, or the member's desk for a
	// colleague of the firm (D-157).
	const std::string home = agent.matter_id.empty() == false ? agent.matter_id : matter::desk_project_id();
	if (home.empty() == true) return "";
	const std::optional<MatterFolder> matter = matter_folder_of(home);
	if (matter.has_value() == false) return "";
	const std::string brief = agent_brief(agent);
	std::string chatId = agent.chat_id.value_or("");
	if (chatId.empty() == false && is_live_chat(home, chatId) == false) chatId.clear();
	if (chatId.empty() == false) {
		(void)chat::bind_agent_thread(home, chatId, agent.id, brief);
		return chatId;
	}
	matters::MatterChat made = chat::create(home);
	made = chat::rename(home, made.id, clipped_name(agent.name), made.revision, matters::ChatTitleAuthority::User);
	(void)chat::bind_agent_thread(home, made.id, agent.id, brief);
	// The agent keeps its thread from the moment it is made, so a run cut
	// short never leaves the next one to make a second.
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		std::optional<Agent> current = load_agent(agent.id);
		if (current.has_value() == true) {
			current->chat_id = made.id;
			save_agent(*current);
		}
	}
	// A colleague with an office in the firm is known by it: its personal chat
	// with a member opens as a messenger's does, with no words of its own
	// (D-130). One that watches a folder says what it watches.
	if (agent.office.title.empty() == true || agent.mode == AgentMode::Changes) {
		(void)chat::post_agent_message(home, made.id, agent.id, agent.name, introduction(agent),
			agent.model_policy == AgentModelPolicy::LocalOnly);
	}
	return made.id;
}

// Messages naming this computer's agents, from any computer of the team:
// each Matter's shared operation log is looked at only when it changed, or
// when the person here has confirmed a colleague's computer since - work that
// computer gave before it was confirmed is then taken up.
void take_up_synced_mentions() {
	struct Seen {
		std::filesystem::path folder;
		std::filesystem::file_time_type operations_time {};
		std::chrono::steady_clock::time_point folder_found_at {};
	};
	static std::map<std::string, Seen> *seen = new std::map<std::string, Seen>();
	static std::uint64_t *trustSeen = new std::uint64_t(0);
	const std::uint64_t trust = tools::device_trust_decisions();
	const bool isTrustChanged = trust != *trustSeen;
	*trustSeen = trust;
	std::map<std::string, std::string> agentsSince;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		for (const Agent &agent : load_agents()) {
			if (agent.status != AgentStatus::Active && agent.status != AgentStatus::Paused) continue;
			// A copy of the firm's colleague answers only the member here (D-213).
			if (agent.is_firm_copy == true) continue;
			agentsSince[agent.id] = agent.created_at;
		}
	}
	if (agentsSince.empty() == true) return;
	// A colleague is the firm's: it is named in any Matter of the firm known
	// here (D-157) - never the member's desk, whose messages are posted here.
	std::set<std::string> matterIds;
	try {
		for (const matters::Matter &known : matter::list()) matterIds.insert(known.id);
	} catch (const std::exception &) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	for (const std::string &matterId : matterIds) {
		Seen &entry = (*seen)[matterId];
		if (entry.folder.empty() == true || now - entry.folder_found_at > std::chrono::minutes(5)) {
			const std::optional<MatterFolder> matter = matter_folder_of(matterId);
			if (matter.has_value() == false) continue;
			entry.folder = matter->folder;
			entry.folder_found_at = now;
		}
		std::error_code error;
		const auto changed = std::filesystem::last_write_time(entry.folder / ".lm" / "Operations", error);
		if (error || (changed == entry.operations_time && isTrustChanged == false)) continue;
		entry.operations_time = changed;
		std::optional<matters::Matter> matter;
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			matter = matters::load_matter(node::require_active_workspace_path(), matterId);
		}
		if (matter.has_value() == false) continue;
		// Only a member of the Matter's team gives its agents work: someone
		// whose Singularity opened the Matter, or the person at this computer.
		std::set<std::string> members;
		for (const matter::SharedMember &member : matter::load_members(entry.folder)) members.insert(member.actor_id);
		members.insert(platform::resolve_actor_identity().actor_id);
		for (const matters::MatterTask &task : matter->tasks) {
			if (task.operation_type != "member_message" || task.chat_id.has_value() == false) continue;
			if (task.mentions.empty() == true && task.quote_message_id.empty() == true) continue;
			if (members.count(task.author_actor_id) == 0) continue;
			// Work given in the shared folder is taken up only as its author's: a
			// message this computer wrote, or one signed by a computer the person
			// here confirmed as the author's. Anyone with the folder can write a
			// message under a colleague's name; such a one is shown, never acted on.
			if (chat::counts_as_authors(matterId, task) == false) continue;
			// Those it names, and the colleague whose message it replies to.
			const std::vector<std::string> addressed = chat::addressed_colleagues(*matter, task);
			bool namesOurs = false;
			for (const std::string &mention : addressed) {
				const auto agent = agentsSince.find(mention);
				// Only what was written after the agent began to work.
				if (agent != agentsSince.end() && task.created_at >= agent->second) namesOurs = true;
			}
			if (namesOurs == false) continue;
			MentionMessage message;
			message.matter_id = matterId;
			message.chat_id = *task.chat_id;
			message.message_id = task.id;
			message.text = chat::words_for_colleagues(*matter, task);
			message.author_actor_id = task.author_actor_id;
			message.author_name = task.author_name;
			message.mentions = addressed;
			(void)take_up_mentions(message);
		}
	}
}

// A colleague's word that work just given waits behind the work it is doing,
// in the thread the work was given in, as a reply to the request.
std::string say_waiting_of(const Agent &agent, const AgentAssignment &work, const std::string &words) {
	if (work.matter_id.empty() == true || work.chat_id.empty() == true) return std::string();
	chat::MessageExtras extras;
	extras.quote_message_id = work.reply_to_message_id;
	return chat::post_agent_message(work.matter_id, work.chat_id, agent.id, agent.name, words,
		agent.model_policy == AgentModelPolicy::LocalOnly, extras);
}

void report_run_of(const Agent &agent, const AgentRun &run) {
	// A task's answer, or its failure, is a turn of the thread already.
	if (run.task.has_value() == true && run.task->task_id.empty() == false) return;
	const std::string words = report_words(agent, run);
	if (words.empty() == true) return;
	const std::string chatId = open_thread_of(agent);
	if (chatId.empty() == true) return;
	const std::string home = agent.matter_id.empty() == false ? agent.matter_id : matter::desk_project_id();
	(void)chat::post_agent_message(home, chatId, agent.id, agent.name, words,
		agent.model_policy == AgentModelPolicy::LocalOnly);
}

} // namespace

void install_thread_document_work(ThreadDocumentWork work) {
	const std::lock_guard<std::mutex> lock(thread_document_work_mutex());
	installed_thread_document_work() = std::move(work);
}

bool agent_works_only_here(const std::string &agent_id) {
	try {
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		const std::optional<Agent> agent = load_agent(agent_id);
		return agent.has_value() == true && agent->model_policy == AgentModelPolicy::LocalOnly;
	} catch (const std::exception &) {
		return false;
	}
}

RunnerDependencies production_runner_dependencies() {
	RunnerDependencies dependencies;
	dependencies.matter_folder = matter_folder_of;
	dependencies.extract = extract_text;
	dependencies.analyse = analyse;
	dependencies.run_task = run_task;
	dependencies.write_report = write_report;
	dependencies.actor_id = []() { return platform::resolve_actor_identity().actor_id; };
	dependencies.person_name = []() { return platform::resolve_actor_identity().display_name.value_or(""); };
	dependencies.device_id = []() { return platform::current_device_id(); };
	dependencies.device_key = []() { return platform::device_public_key(); };
	dependencies.sign = [](const std::string &message) { return platform::sign_with_device_key(message); };
	dependencies.verify = [](const std::string &key, const std::string &message, const std::string &signature) {
		return platform::verify_device_signature(key, message, signature);
	};
	dependencies.seat_id = []() {
		const std::string actor = platform::resolve_actor_identity().actor_id;
		return actor.rfind("seat:", 0) == 0 ? actor.substr(5) : std::string();
	};
	dependencies.open_thread = open_thread_of;
	dependencies.report_run = report_run_of;
	dependencies.say_waiting = say_waiting_of;
	dependencies.take_up_mentions = take_up_synced_mentions;
	dependencies.desk_project = []() { return matter::desk_project_id(); };
	return dependencies;
}

AiAppDependencies production_ai_app_dependencies() {
	AiAppDependencies dependencies;
	dependencies.list_matters = []() {
		std::vector<AiAppMatter> matters;
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		for (const matters::Matter &matter : matters::load_matters(node::require_active_workspace_path()))
			if (matter::is_desk(matter) == false) matters.push_back(AiAppMatter{matter.id, matter.name});
		return matters;
	};
	return dependencies;
}

} // namespace singularity::agents
