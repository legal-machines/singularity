// Singularity CLI - a narrow, generic terminal client for real Singularity
// operations. Enters through the exact same production boundary the app's
// own UI does (NativeBridge::invoke_worker -> chat.send -> ChatExecution ->
// MatterRequestExecution): there is no second ResearchToolLoop/Papinian/
// ToolBroker/document-mutation implementation here, and nothing in this file
// is specific to any one Project, document, or methodology. It operates on
// the real active workspace, real Settings, real credential store and real
// providers exactly like the desktop app - there is no synthetic/sandboxed
// mode. What the app does by itself when a Project opens - reading its
// documents, then its privacy preprocessing, which builds the entity registry
// protected requests read - the CLI does on /preprocess, waiting for both (see
// print_help() for every command).
//
// Trace levels (see Routing/Observability/OperationEvent.hpp for the
// contract this renders):
//   off      - no sink registered, only the final chat.send outcome prints.
//   safe     - every OperationEvent's kind/message/details print live as the
//              operation runs (live events; invoke_worker runs
//              chat.send synchronously on this same thread) - never a
//              forensic_payload.
//   forensic - safe, plus each event's forensic_payload verbatim when
//              present (exact outbound provider body, raw provider
//              response, real Papinian entity/mapping detail, canonicalized
//              research query, ...). This CLI prints forensic content to the
//              terminal; it writes it to a file only when SINGULARITY_CLI_CAPTURE
//              names one (an acceptance run's record of each provider-bound
//              payload), and never to a MatterTask or any other durable log.

#include "Agent/AgentScheduler/AgentScheduler.hpp"
#include "Bridge/NativeBridge/NativeBridge.hpp"
#include "Bridge/NodeBridge/NodeBridge.hpp"
#include "Chat/ChatExecution/ChatExecution.hpp"
#include "Cli/TerminalTrace/TerminalTrace.hpp"
#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AppWindow/AppWindow.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Privacy/PrivacyActions/PrivacyActions.hpp"
#include "Privacy/ProjectPreprocessing/ProjectPreprocessing.hpp"
#include "Organisation/FirmRole/FirmRole.hpp"
#include "Provider/AccountGate/AccountGate.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"
#include "Provider/ProviderConnections/ProviderConnections.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/Observability/OperationEvent.hpp"
#include "Tool/Types/ToolTypes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <system_error>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace observability = singularity::routing::observability;

using singularity::cli::TraceLevel;

std::string trim(std::string value) {
	std::size_t begin = 0;
	while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
	std::size_t end = value.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
	return value.substr(begin, end - begin);
}

std::optional<TraceLevel> trace_level_from_string(const std::string &value) {
	if (value == "off") return TraceLevel::Off;
	if (value == "safe") return TraceLevel::Safe;
	if (value == "forensic") return TraceLevel::Forensic;
	return std::nullopt;
}

const char *to_string(TraceLevel level) {
	switch (level) {
		case TraceLevel::Off: return "off";
		case TraceLevel::Safe: return "safe";
		case TraceLevel::Forensic: return "forensic";
	}
	return "off";
}

class Session {
public:
	Session() : window_(false), bridge_(window_, false) {}
	// A send still running finishes before the session ends; agents the
	// session started stop with it.
	~Session() {
		if (background_.joinable()) background_.join();
		if (agents_on_) {
			singularity::agents::AgentScheduler::shared().stop();
		}
	}

	// What the window does when it opens: agents check their rooms on their
	// own thread while this process lives (Agent/AgentScheduler). Outside AI
	// apps are not answered (D-217).
	void agents_on() {
		if (agents_on_) return;
		singularity::agents::AgentScheduler::shared().start();
		agents_on_ = true;
		std::cout << "AGENTS_ON scheduler running in this process\n";
	}

	// Begins an agent for the open Project through agent.create, as the
	// window's New agent does: a data room watch of the Project's folder kept
	// for changes, or a task run by hand or after another agent reports.
	void agent_create(const std::string &kind, const std::string &instruction, const std::string &follows) {
		require_project();
		std::string folder;
		const Json listed = call("matter.list", Json::object());
		for (const Json &project : listed.at("matters"))
			if (project.value("id", std::string()) == project_id_ && project.contains("directoryPath") && project.at("directoryPath").is_string())
				folder = project.at("directoryPath").get<std::string>();
		const bool isWatch = kind == "data_room_watch";
		Json params{{"projectId", project_id_}, {"kind", kind}, {"mode", isWatch ? "changes" : "task"},
			{"name", isWatch ? "Data room watch" : (follows.empty() ? "Standing task" : "Follow-up task")},
			{"goal", instruction}, {"sourceRoot", folder}, {"modelPolicy", "matter_setup"},
			{"trigger", isWatch ? "on_change" : (follows.empty() ? "manual" : "after_agent")}};
		if (follows.empty() == false) params["followsAgentId"] = follows;
		const Json agent = call("agent.create", params);
		last_agent_id_ = agent.value("id", std::string());
		std::cout << "AGENT_CREATED " << agent.value("id", std::string()) << " state=" << agent.value("state", std::string())
				<< " responsible=" << agent.value("responsibleName", std::string()) << "\n";
	}

	void agents_list() {
		const Json listed = call("agent.list", project_id_.empty() ? Json::object() : Json{{"projectId", project_id_}});
		for (const Json &agent : listed.at("agents")) {
			std::cout << "AGENT " << agent.value("id", std::string()) << " \"" << agent.value("name", std::string()) << "\" state="
					<< agent.value("state", std::string()) << " trigger=" << agent.value("trigger", std::string());
			if (agent.contains("latestRun") && agent.at("latestRun").is_object())
				std::cout << " lastRun=" << agent.at("latestRun").value("id", std::string()) << "/" << agent.at("latestRun").value("status", std::string());
			std::cout << "\n";
		}
	}

	// Waits until the agent's newest run is not the one given and has ended,
	// then prints what it found, where its analysis ran and what it cost.
	void agent_wait(const std::string &givenAgentId, const std::string &givenAfterRunId, int seconds) {
		// "last" is the agent this session made last; "next" is a run after the
		// agent's newest run as it is now.
		const std::string agentId = givenAgentId == "last" ? last_agent_id_ : givenAgentId;
		std::string afterRunId = givenAfterRunId;
		if (afterRunId == "next") {
			const Json now = call("agent.get", Json{{"agentId", agentId}}).at("agent");
			afterRunId = now.contains("latestRun") && now.at("latestRun").is_object() ? now.at("latestRun").value("id", std::string()) : "";
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
		Json detail;
		while (std::chrono::steady_clock::now() < deadline) {
			detail = call("agent.get", Json{{"agentId", agentId}});
			const Json &agent = detail.at("agent");
			const Json latest = agent.contains("latestRun") ? agent.at("latestRun") : Json(nullptr);
			if (latest.is_object() && latest.value("id", std::string()) != afterRunId && latest.value("status", std::string()) != "running"
					&& agent.value("isRunning", false) == false) {
				const Json run = call("agent.getRun", Json{{"agentId", agentId}, {"runId", latest.at("id")}});
				print_agent_run(run);
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		std::cout << "AGENT_WAIT_TIMED_OUT " << agentId << "\n";
	}

	// Gives an agent work, as its page does, and remembers the work.
	void agent_assign(const std::string &givenAgentId, const std::string &instruction) {
		const std::string agentId = givenAgentId == "last" ? last_agent_id_ : givenAgentId;
		const Json work = call("agent.assign", Json{{"agentId", agentId}, {"instruction", instruction}});
		last_work_agent_id_ = agentId;
		last_work_id_ = work.value("id", std::string());
		std::cout << "AGENT_WORK_GIVEN " << last_work_id_ << " state=" << work.value("state", std::string()) << " position="
				<< work.at("position").dump() << " givenBy=\"" << work.value("givenByName", std::string()) << "\"\n";
	}

	// Waits until the work given last is neither waiting nor being done, then
	// prints what became of it and the run that did it.
	void work_wait(int seconds) {
		if (last_work_id_.empty()) throw std::runtime_error("/work-wait needs work given in this session (/agent-assign).");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
		while (std::chrono::steady_clock::now() < deadline) {
			const Json detail = call("agent.get", Json{{"agentId", last_work_agent_id_}});
			for (const Json &work : detail.at("assignments")) {
				if (work.value("id", std::string()) != last_work_id_) continue;
				const std::string state = work.value("state", std::string());
				if (state == "waiting" || state == "working") break;
				std::cout << "AGENT_WORK " << last_work_id_ << " state=" << state << " message=\"" << work.value("message", std::string()) << "\"\n";
				if (work.at("result").is_object())
					print_agent_run(call("agent.getRun", Json{{"agentId", last_work_agent_id_}, {"runId", work.at("result").at("runId")}}));
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		std::cout << "AGENT_WORK_WAIT_TIMED_OUT " << last_work_id_ << "\n";
	}

	void print_agent_run(const Json &run) {
		std::cout << "AGENT_RUN " << run.value("id", std::string()) << " status=" << run.value("status", std::string())
				<< " trigger=" << run.value("trigger", std::string()) << " baseline=" << run.value("isBaseline", false)
				<< " started=" << run.value("startedAt", std::string()) << " finished=" << run.value("finishedAt", std::string()) << "\n";
		if (run.contains("counts")) std::cout << "  counts " << run.at("counts").dump() << "\n";
		for (const Json &document : run.value("documents", Json::array()))
			std::cout << "  document " << document.value("kind", std::string()) << " \"" << document.value("relativePath", std::string())
					<< "\" review=" << document.value("review", std::string()) << "\n";
		if (run.contains("analysis")) {
			const Json &analysis = run.at("analysis");
			std::cout << "  analysis status=" << analysis.value("status", std::string()) << " destination="
					<< analysis.value("destination", std::string()) << " model=" << analysis.value("modelLabel", std::string())
					<< " cost=" << (analysis.contains("cost") ? analysis.at("cost").dump() : "null") << " " << analysis.value("currency", std::string())
					<< " reason=\"" << analysis.value("reason", std::string()) << "\"\n";
		}
		for (const Json &finding : run.value("findings", Json::array())) {
			std::cout << "  finding \"" << finding.value("title", std::string()) << "\" evidence=" << finding.value("evidenceIds", Json::array()).dump()
					<< " needsReview=" << finding.value("needsReview", false) << "\n    " << finding.value("observation", std::string()) << "\n";
			if (finding.value("analysis", std::string()).empty() == false) std::cout << "    analysis: " << finding.value("analysis", std::string()) << "\n";
		}
		if (run.contains("task") && run.at("task").is_object())
			std::cout << "  task " << run.at("task").dump() << "\n";
		std::cout << "  workProduct " << (run.contains("workProductId") ? run.at("workProductId").dump() : "null")
				<< " error=\"" << run.value("errorMessage", std::string()) << "\"\n";
	}

	Json call(const std::string &method, const Json &params) {
		return Json::parse(bridge_.invoke_worker(method, params.dump()));
	}

	void set_trace(TraceLevel level) {
		trace_ = level;
		sink_.reset();
		if (level == TraceLevel::Off) return;
		sink_ = std::make_unique<singularity::cli::TerminalTrace>(level, std::cout);
	}

	TraceLevel trace() const { return trace_; }

	void open_project(const std::string &pathOrId) {
		const Json existing = call("matter.list", Json::object());
		for (const auto &project : existing.at("matters")) {
			if (project.at("id").get<std::string>() != pathOrId) continue;
			project_id_ = pathOrId;
			project_label_ = project.value("name", pathOrId);
			create_or_reuse_chat();
			std::cout << "PROJECT_OPENED " << project_id_ << " (" << project_label_ << ")\n";
			return;
		}

		std::error_code error;
		const auto path = std::filesystem::absolute(pathOrId, error);
		if (error || !std::filesystem::is_directory(path)) {
			throw std::runtime_error(
					"\"" + pathOrId + "\" is neither an existing Project id nor a real directory.");
		}
		// The app's own New Project / Open existing Project: a folder holding a
		// Project's `.lm` - one a colleague's computer began, in a shared folder -
		// opens under that Project's identity.
		const auto project = singularity::matter::create_or_open_from_folder(path.u8string());
		const auto withDocuments = singularity::matter::list_documents(project.id);
		singularity::matter::stop_watching_documents(project.id);
		project_id_ = project.id;
		project_label_ = project.name;
		create_or_reuse_chat();
		std::cout << "PROJECT_CREATED " << project_id_ << " (" << project_label_ << ") - "
				<< withDocuments.documents.size() << " document(s) discovered\n";
	}

	void require_project() const {
		if (project_id_.empty()) throw std::runtime_error("No Project is open. Use /project <path-or-id> first.");
	}

	void set_model_override(const std::string &providerId, const std::string &modelId) {
		model_override_ = Json{{"providerId", providerId}, {"modelId", modelId}};
		std::cout << "MODEL_OVERRIDE_SET " << providerId << "/" << modelId << "\n";
	}

	void clear_model_override() {
		model_override_.reset();
		std::cout << "MODEL_OVERRIDE_CLEARED\n";
	}

	void set_research_backend(const std::string &backend) {
		if (backend != "openai_hosted" && backend != "openrouter")
			throw std::runtime_error("Unknown research backend \"" + backend + "\" (openai_hosted|openrouter).");
		auto settings = singularity::platform::load_app_settings();
		settings.local_node_setup.research_backend = backend;
		singularity::platform::save_app_settings(settings);
		std::cout << "RESEARCH_BACKEND_SET global " << backend << "\n";
	}

	void save_permission(const std::string &mode) {
		require_project();
		const auto parsed = singularity::tools::project_action_mode_from_string(mode);
		if (!parsed.has_value())
			throw std::runtime_error(
					"Unknown permission mode \"" + mode + "\" (ask_for_approval|approve_safe_actions|full_project_access).");
		const Json result = call("chat.saveActionPermission", {{"projectId", project_id_}, {"requestedMode", mode}});
		std::cout << "PERMISSION_SET " << result.dump() << "\n";
	}


	// The Chat's working document, set through the same bridge method the
	// composer uses: a document id, "new" for a new document, "clear" for none.
	void set_working_document(const std::string &value) {
		require_project();
		if (chat_id_.empty()) throw std::runtime_error("No Chat is open.");
		const std::string documentId = value == "new" ? std::string("new-document")
			: value == "clear" ? std::string() : value;
		const Json chat = call("chat.setWorkingDocument",
			{{"projectId", project_id_}, {"chatId", chat_id_}, {"documentId", documentId}});
		std::cout << "WORKING_DOCUMENT_SET " << chat.value("workingDocumentId", std::string()) << "\n";
	}

	// What the app does when a Project opens, run here and waited for: first
	// its documents - every file in the folder found and read, the way the
	// Documents view reads them - then the Project's privacy preprocessing over
	// them, which builds the entity registry every protected request of this
	// Project reads its people and companies from. The app's own background
	// pass may hold the Project for a while; this waits for it, bounded. A
	// live run printed "1/6 documents ready" because it did not (25 Sep 2026).
	void preprocess() {
		require_project();
		const auto started = std::chrono::steady_clock::now();
		const auto deadline = started + std::chrono::minutes(15);
		const auto waitingFor = [](const singularity::matters::Matter &project) {
			std::size_t pending = 0;
			for (const auto &document : project.documents) {
				const auto state = singularity::matters::document_lifecycle_state(document);
				if (state == singularity::matters::DocumentLifecycleState::Processing
						|| state == singularity::matters::DocumentLifecycleState::Changed) ++pending;
			}
			return pending;
		};
		singularity::matters::Matter project = singularity::matter::list_documents(project_id_);
		singularity::matter::stop_watching_documents(project_id_);
		while (waitingFor(project) > 0 && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			project = singularity::matter::process_project_documents(project_id_);
		}
		std::size_t ready = 0;
		std::size_t total = 0;
		for (const auto &document : project.documents) {
			const auto state = singularity::matters::document_lifecycle_state(document);
			if (state == singularity::matters::DocumentLifecycleState::Unavailable) continue;
			++total;
			if (state == singularity::matters::DocumentLifecycleState::Ready) ++ready;
		}
		std::cout << "DOCUMENTS " << ready << "/" << total << " ready" << (waitingFor(project) > 0
				? " (" + std::to_string(waitingFor(project)) + " still being read)" : std::string()) << "\n";

		singularity::routing::papinian_warmup_tracker().run_once();
		const auto status = singularity::routing::papinian_warmup_tracker().status();
		if (status.state != singularity::routing::ModelLifecycleState::Ready) {
			std::cout << "PREPROCESS_UNAVAILABLE the Papinian model is not ready"
					<< (status.message.empty() ? std::string() : " (" + status.message + ")") << "\n";
			return;
		}
		// Waited for only while another pass - the app's own, queued at launch -
		// holds the Project. A pass of this command's own that leaves a document
		// unprotected is not run again and again: that document cannot be
		// protected now, and is said so below.
		const auto unprotected = [](const singularity::matters::Matter &current) {
			std::vector<const singularity::matters::MatterDocument *> documents;
			for (const auto &document : current.documents)
				if (singularity::privacy::needs_protection_pass(current, document)) documents.push_back(&document);
			return documents;
		};
		for (;;) {
			const bool heldElsewhere = singularity::privacy_actions::preprocessing_running(project_id_);
			project = singularity::privacy_actions::run_preprocessing(project_id_);
			if (unprotected(project).empty() || !heldElsewhere || std::chrono::steady_clock::now() >= deadline) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		for (const singularity::matters::MatterDocument *document : unprotected(project)) {
			const auto status = singularity::privacy::find_document_protection(project, document->id);
			std::cout << "NOT_PROTECTED " << document->name << ": "
					<< (status && !status->detail.empty() ? status->detail
						: std::string("its text could not be read for protection"))
					<< "\n";
		}
		const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();
		const auto overview = singularity::privacy_actions::get_overview(project_id_);
		std::cout << "PREPROCESSED " << overview.entity_count << " entities, " << overview.coverage.ready << "/"
				<< overview.coverage.total << " documents protected"
				<< (overview.pending_ambiguity_count > 0
					? ", " + std::to_string(overview.pending_ambiguity_count) + " identities awaiting review" : std::string())
				<< ", " << ready << "/" << total << " documents ready  (" << elapsedMs << " ms)\n";
	}

	void list_providers() { std::cout << "PROVIDERS " << call("providers.list", Json::object()).dump() << "\n"; }

	void list_workspaces() { std::cout << "WORKSPACES " << call("workspace.list", Json::object()).dump() << "\n"; }

	void set_workspace(const std::string &id) {
		std::cout << "WORKSPACE_SET " << call("workspace.setActive", {{"id", id}}).dump() << "\n";
		project_id_.clear();
		project_label_.clear();
		chat_id_.clear();
		last_task_ = nullptr;
	}

	void show_status() const {
		std::cout << "trace: " << to_string(trace_) << "\n";
		std::cout << "project: " << (project_id_.empty() ? "(none)" : project_id_ + " (" + project_label_ + ")")
				<< "\n";
		std::cout << "chat: " << (chat_id_.empty() ? "(none)" : chat_id_) << "\n";
		std::cout << "model override: " << (model_override_.has_value() ? model_override_->dump() : "(none - uses Project default)")
				<< "\n";
		std::cout << "research mode: derived from the task; @web requires research\n";
		std::cout << "lawyer questions: " << (can_ask_lawyer_ ? "on (/send, /wait, /answer)" : "off") << "\n";
		std::cout << "research backend (global): " << singularity::platform::load_app_settings().local_node_setup.research_backend
				<< "\n";
	}

	void show_task() const {
		if (last_task_.is_null()) { std::cout << "No task has completed yet.\n"; return; }
		std::cout << last_task_.dump(2) << "\n";
	}

	void show_evidence() const {
		if (last_task_.is_null() || !last_task_.contains("externalEvidence")) {
			std::cout << "No task with external evidence has completed yet.\n";
			return;
		}
		for (const auto &evidence : last_task_.at("externalEvidence")) {
			std::cout << evidence.value("sourceClass", "unknown") << "  " << evidence.value("url", "unknown") << "\n";
		}
	}

	// The one real production entrypoint this whole CLI exists to drive:
	// NativeBridge chat.send. Runs synchronously on this thread, so a
	// registered trace sink observes every OperationEvent live, in order, as
	// the real operation actually executes - not replayed afterward.
	int send_prompt(const std::string &prompt) {
		require_project();
		Json params{
				{"projectId", project_id_}, {"chatId", chat_id_},
				{"operationId", "singularity-cli-" + singularity::routing::generate_request_namespace()},
				{"prompt", prompt}, {"sourcePolicyMode", "relevant"}, {"selectedDocumentIds", Json::array()},
		};
		// The Legal task is what decides an objective's own output contract (a
		// Risk Review produces findings, an Extract Obligations produces
		// obligations - see Professional/StructuredResults). Drives the real
		// production field; unset means General, exactly as in the app.
		if (!legal_task_preset_id_.empty()) {
			params["legalTaskPresetId"] = legal_task_preset_id_;
			params["legalTaskPresetVersion"] = legal_task_preset_version_;
		}
		if (model_override_.has_value()) params["oneTimePrimaryModelOverride"] = *model_override_;
		if (can_ask_lawyer_) params["canAskLawyer"] = true;

		// Deliberately does NOT prepare Papinian here. The backend's own
		// readiness gate joins the canonical warm-up (see
		// require_papinian_model_ready), so every protected entry point -
		// chat.send, matter.runConsilium, matter.synthesizeConsilium - behaves
		// identically on a cold start. Warming up only here is what used to
		// make Chat look fine while Consilium failed.
		const auto started = std::chrono::steady_clock::now();
		const Json outcome = call("chat.send", params);
		const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();

		std::cout << "OUTCOME " << outcome.dump() << "  (" << elapsedMs << " ms)\n";
		if (outcome.value("state", std::string()) != "completed") {
			std::cerr << "SEND_FAILED " << outcome.value("message", std::string("unknown failure")) << "\n";
			return 1;
		}
		const std::string taskId = outcome.at("taskId").get<std::string>();
		const Json projects = call("matter.list", Json::object());
		for (const auto &project : projects.at("matters")) {
			if (project.at("id").get<std::string>() != project_id_) continue;
			for (const auto &task : project.at("tasks")) {
				if (task.at("id").get<std::string>() != taskId) continue;
				last_task_ = task;
				// As the lawyer reads it: arithmetic the model asked for, worked out.
				std::cout << "RESULT " << singularity::context::computed_statements::compute(
						task.value("responseText", std::string())) << "\n";
			}
		}
		return 0;
	}

	// A send on its own thread, so what the operation asks the lawyer can be
	// answered here while it waits: /send, then /wait until it finishes or
	// pauses; /approve or /reject a proposed change, /answer a question - the
	// same ChatExecution calls the app's approval and question cards make.
	void send_in_background(const std::string &prompt) {
		require_project();
		if (background_.joinable()) throw std::runtime_error("An operation is still running: /wait for it first.");
		Json params{
				{"projectId", project_id_}, {"chatId", chat_id_},
				{"operationId", "singularity-cli-" + singularity::routing::generate_request_namespace()},
				{"prompt", prompt}, {"sourcePolicyMode", "relevant"}, {"selectedDocumentIds", Json::array()},
		};
		if (!legal_task_preset_id_.empty()) {
			params["legalTaskPresetId"] = legal_task_preset_id_;
			params["legalTaskPresetVersion"] = legal_task_preset_version_;
		}
		if (model_override_.has_value()) params["oneTimePrimaryModelOverride"] = *model_override_;
		if (can_ask_lawyer_) params["canAskLawyer"] = true;
		operation_id_ = params.at("operationId").get<std::string>();
		sent_at_ = std::chrono::steady_clock::now();
		live_shown_ = 0;
		run_in_background("chat.send", params);
		std::cout << "SENT " << operation_id_ << "\n";
	}

	void wait_for_operation() {
		if (!background_.joinable()) {
			std::cout << "NOTHING_RUNNING\n";
			return;
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
		while (std::chrono::steady_clock::now() < deadline) {
			bool done = false;
			{
				const std::lock_guard<std::mutex> guard(background_mutex_);
				done = background_done_;
			}
			if (done) {
				background_.join();
				print_outcome(background_outcome_, background_elapsed_ms_);
				return;
			}
			const singularity::chat::ChatOperationStatus status = singularity::chat::operation_status(project_id_, operation_id_);
			// The answer as the model writes it: when its first words arrived, and
			// how far it has come.
			if (status.live_answer.size() > live_shown_) {
				const long long since = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - sent_at_).count();
				std::cout << "LIVE " << since << "ms " << status.live_answer.size() << " bytes";
				if (live_shown_ == 0) std::cout << " " << Json(status.live_answer.substr(0, 120)).dump();
				std::cout << "\n";
				live_shown_ = status.live_answer.size();
			}
			if (status.state == singularity::chat::ChatOperationState::WaitingForAnswer && status.asking) {
				asking_ = *status.asking;
				std::cout << "QUESTION " << asking_.dump() << "\n";
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
		}
		std::cout << "WAIT_TIMEOUT " << operation_id_ << "\n";
	}

	void decide(bool allowed) {
		require_project();
		if (approval_id_.empty()) throw std::runtime_error("No change is waiting for a decision.");
		const singularity::chat::ChatOperationStatus status =
			singularity::chat::record_approval_response(project_id_, operation_id_, approval_id_, allowed);
		std::cout << "DECIDED " << (allowed ? "approve " : "reject ") << singularity::chat::to_string(status.state) << "\n";
		approval_id_.clear();
		// The decision carries its operation on, as the application's own
		// bridge does after it (Beta 1 #262): an approved change is written, a
		// rejected one has already ended.
		if (singularity::chat::paused_decision_awaits_resume(project_id_, operation_id_) == false) return;
		run_in_background("chat.resumeOperation", Json{{"projectId", project_id_}, {"operationId", operation_id_}});
	}

	// Every file of the open Project's folder with its SHA-256, so a transcript
	// shows what a decision did and did not change on disk.
	void print_hashes() {
		require_project();
		const Json projects = call("matter.list", Json::object());
		for (const auto &project : projects.at("matters")) {
			if (project.at("id").get<std::string>() != project_id_) continue;
			const std::string folder = project.value("directoryPath", std::string());
			if (folder.empty()) throw std::runtime_error("This Project has no folder.");
			std::vector<std::pair<std::string, std::string>> files;
			for (const auto &entry : std::filesystem::recursive_directory_iterator(folder)) {
				if (!entry.is_regular_file()) continue;
				const std::string relative = std::filesystem::relative(entry.path(), folder).generic_u8string();
				if (relative.rfind(".lm", 0) == 0) continue;
				files.push_back({relative, singularity::platform::sha256_hex_digest_of_file(entry.path())});
			}
			std::sort(files.begin(), files.end());
			for (const auto &[relative, hash] : files) std::cout << "FILE " << hash.substr(0, 16) << "  " << relative << "\n";
			return;
		}
	}

	// "/answer 2" chooses the question's second option; "/answer text: ..." gives
	// the lawyer's own words; "/answer decide" leaves it to Singularity.
	void answer(const std::string &given) {
		require_project();
		if (asking_.is_null()) throw std::runtime_error("No question is waiting for an answer.");
		const std::string questionId = asking_.value("id", std::string());
		std::optional<Json> answers;
		if (given != "decide") {
			Json perQuestion = Json::object();
			for (const Json &question : asking_.value("questions", Json::array())) {
				const std::string id = question.value("id", std::string());
				Json chosen = Json::array();
				std::string text;
				if (given.rfind("text:", 0) == 0) {
					text = trim(given.substr(5));
				} else {
					const std::size_t index = static_cast<std::size_t>(std::stoul(given));
					const Json options = question.value("options", Json::array());
					if (index == 0 || index > options.size()) throw std::runtime_error("That option does not exist.");
					chosen.push_back(options[index - 1].value("id", std::string()));
				}
				perQuestion[id] = Json{{"optionIds", chosen}, {"text", text}};
			}
			answers = perQuestion;
		}
		const singularity::chat::ChatOperationStatus status =
			singularity::chat::record_question_answer(project_id_, operation_id_, questionId, answers);
		std::cout << "ANSWERED " << singularity::chat::to_string(status.state) << "\n";
		asking_ = nullptr;
	}

private:
	void run_in_background(const std::string &method, const Json &params) {
		{
			const std::lock_guard<std::mutex> guard(background_mutex_);
			background_done_ = false;
			background_outcome_ = nullptr;
		}
		background_ = std::thread([this, method, params] {
			// An acceptance run's record of exactly what each provider call of this
			// operation carried: every event's forensic payload - the prompt before
			// protection, the provider-bound body, the provider's reply - one JSON
			// line each, to the file SINGULARITY_CLI_CAPTURE names. The sink is
			// thread-local, so it is set up on the thread the operation runs on.
			// Terminal tooling only: never on for the app, never a credential.
			std::unique_ptr<singularity::routing::observability::ScopedEventSink> capture;
			// Compiled out of every release build: the capture holds the
			// protection's original-to-symbol mapping, and a release binary can
			// never be made to write it, whatever its environment says.
#if !defined(NDEBUG) && !defined(SINGULARITY_NO_FORENSIC_OBSERVABILITY)
			if (const char *path = std::getenv("SINGULARITY_CLI_CAPTURE"); path != nullptr && *path != '\0') {
				auto file = std::make_shared<std::ofstream>(path, std::ios::app);
				const std::string operation = params.value("operationId", std::string());
				capture = std::make_unique<singularity::routing::observability::ScopedEventSink>(
					[file, operation, method](const singularity::routing::observability::OperationEvent &event) {
						// The task's disclosure diagnostics ride on their events' details
						// (counts, fact-key digests): kept beside the payloads.
						Json disclosure = Json::object();
						for (const auto &[key, value] : event.details)
							if (key.rfind("disclosure", 0) == 0) disclosure[key] = value;
						if (!event.forensic_payload && disclosure.empty()) return;
						Json row{{"method", method}, {"operationId", operation},
							{"kind", singularity::routing::observability::to_string(event.kind)}, {"message", event.message}};
						if (event.forensic_payload) row["payload"] = *event.forensic_payload;
						if (!disclosure.empty()) row["disclosure"] = std::move(disclosure);
						*file << row.dump(-1, ' ', false, Json::error_handler_t::replace) << "\n";
						file->flush();
					}, false, true);
			}
#endif
			const auto started = std::chrono::steady_clock::now();
			Json outcome;
			try {
				outcome = call(method, params);
			} catch (const std::exception &error) {
				outcome = Json{{"state", "failed"}, {"message", error.what()}};
			}
			const std::lock_guard<std::mutex> guard(background_mutex_);
			background_outcome_ = outcome;
			background_elapsed_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();
			background_done_ = true;
		});
	}

	// Singularity's own review of a proposed change, round by round, as the
	// operation recorded it.
	void print_review_rounds() {
		if (operation_id_.empty()) return;
		try {
			const singularity::chat::ChatOperationStatus status = singularity::chat::operation_status(project_id_, operation_id_);
			for (const singularity::matters::AgentOperationEvent &event : status.events)
				if (event.type == "review.round")
					std::cout << "REVIEW " << event.created_at << " " << event.outcome.value_or(std::string()) << " "
						<< event.message << "\n";
		} catch (const std::exception &) {
		}
	}

	void print_outcome(const Json &outcome, long long elapsedMs) {
		std::cout << "OUTCOME " << outcome.dump() << "  (" << elapsedMs << " ms)\n";
		print_review_rounds();
		const std::string state = outcome.value("state", std::string());
		if (state == "waiting_for_approval") {
			// What waits for the lawyer, as the approval card reads it: the
			// operation's own status (the send's result does not carry it).
			const singularity::chat::ChatOperationStatus status = singularity::chat::operation_status(project_id_, operation_id_);
			if (status.approval) {
				approval_id_ = status.approval->approval_id;
				Json approval{{"approvalId", status.approval->approval_id}, {"toolId", status.approval->tool_id},
					{"summary", status.approval->summary},
					{"targetDocumentId", status.approval->target_document_id ? Json(*status.approval->target_document_id) : Json(nullptr)}};
				if (!status.approval->change_detail_json.empty())
					approval["change"] = Json::parse(status.approval->change_detail_json, nullptr, false);
				std::cout << "APPROVAL " << approval.dump() << "\n";
			}
			return;
		}
		if (state != "completed" || !outcome.contains("taskId") || !outcome.at("taskId").is_string()) return;
		const std::string taskId = outcome.at("taskId").get<std::string>();
		const Json projects = call("matter.list", Json::object());
		for (const auto &project : projects.at("matters")) {
			if (project.at("id").get<std::string>() != project_id_) continue;
			for (const auto &task : project.at("tasks")) {
				if (task.at("id").get<std::string>() != taskId) continue;
				last_task_ = task;
				std::cout << "RESULT " << singularity::context::computed_statements::compute(
						task.value("responseText", std::string())) << "\n";
			}
		}
	}

	std::thread background_;
	std::mutex background_mutex_;
	bool background_done_ = false;
	// When the request was sent, and how much of its answer the wait has shown.
	std::chrono::steady_clock::time_point sent_at_;
	std::size_t live_shown_ = 0;
	Json background_outcome_ = nullptr;
	long long background_elapsed_ms_ = 0;
	std::string operation_id_;
	std::string approval_id_;
	Json asking_ = nullptr;

	void create_or_reuse_chat() {
		chat_id_.clear();
		last_task_ = nullptr;
		const Json chat = call("chat.create", {{"projectId", project_id_}});
		chat_id_ = chat.at("id").get<std::string>();
		std::cout << "CHAT " << chat_id_ << "\n";
	}

public:
	// Goes on in a Chat the open Project already has - after Singularity was
	// closed and opened again, the way a lawyer returns to yesterday's Chat.
	void continue_chat(const std::string &chatId) {
		require_project();
		const Json chats = call("chat.list", {{"projectId", project_id_}});
		const Json &list = chats.is_array() ? chats : chats.value("chats", Json::array());
		for (const Json &chat : list) {
			if (chat.value("id", std::string()) != chatId) continue;
			chat_id_ = chatId;
			last_task_ = nullptr;
			std::cout << "CHAT_CONTINUED " << chat_id_ << "\n";
			return;
		}
		throw std::runtime_error("The open Project has no Chat " + chatId + ".");
	}
private:

	singularity::app::AppWindow window_;
	singularity::bridge::NativeBridge bridge_;
	bool agents_on_ = false;
	std::string last_agent_id_;
	std::string last_work_agent_id_;
	std::string last_work_id_;
	TraceLevel trace_ = TraceLevel::Off;
	std::unique_ptr<singularity::cli::TerminalTrace> sink_;
	std::string project_id_;
	std::string project_label_;
	std::string chat_id_;
	std::optional<Json> model_override_;
	std::string legal_task_preset_id_;
	int legal_task_preset_version_ = 1;
	// Whether the operation may put a question to the lawyer (the app's
	// question card; /wait shows it as QUESTION, /answer replies) - off unless
	// asked for, so an unattended script never waits on one.
	bool can_ask_lawyer_ = false;
public:
	Json call_public(const std::string &method, const Json &params) { return call(method, params); }

	void set_can_ask_lawyer(const std::string &value) {
		if (value != "on" && value != "off") throw std::runtime_error("/lawyer takes on or off.");
		can_ask_lawyer_ = value == "on";
		std::cout << "LAWYER_QUESTIONS " << value << "\n";
	}

	// Consilium and Work Product through the exact bridge methods the frontend
	// calls - never a CLI-only shortcut, so what prints here is what a UI would
	// actually receive and have to render.
	Json run_consilium(const Json &targets, const std::string &objective) {
		require_project();
		Json params{{"matterId", project_id_}, {"prompt", objective}, {"targets", targets}};
		if (!chat_id_.empty()) params["chatId"] = chat_id_;
		return call("matter.runConsilium", params);
	}

	Json synthesize_consilium(const std::string &groupId, const std::string &providerId,
			const std::string &modelId) {
		require_project();
		return call("matter.synthesizeConsilium", {{"matterId", project_id_}, {"groupId", groupId},
			{"providerId", providerId}, {"modelId", modelId}});
	}

	void require_project_public() const { require_project(); }
	const std::string &project_id() const { return project_id_; }

	Json run_consilium_configuration(const std::string &configurationId) {
		require_project();
		Json params{{"matterId", project_id_}, {"configurationId", configurationId}};
		if (!chat_id_.empty()) params["chatId"] = chat_id_;
		return call("matter.runConsilium", params);
	}

	Json list_consilium() {
		require_project();
		return call("consilium.list", {{"projectId", project_id_}});
	}

	Json list_work_products() {
		require_project();
		return call("workProduct.list", {{"projectId", project_id_}});
	}
	Json compact_open_chat() {
		require_project();
		return call("chat.compact", Json{{"projectId", project_id_}, {"chatId", chat_id_}});
	}
	void set_legal_task(const std::string &presetId, int version) {
		legal_task_preset_id_ = presetId;
		legal_task_preset_version_ = version;
		std::cout << "legal task: " << presetId << " v" << version << "\n";
	}
	void clear_legal_task() {
		legal_task_preset_id_.clear();
		legal_task_preset_version_ = 1;
		std::cout << "legal task: (none - General)\n";
	}
private:
	Json last_task_ = nullptr;
};

void print_help() {
	std::cout <<
			"/project <path-or-id>            open a Project (creates one from a folder, or reopens by id)\n"
			"/preprocess                      read the open Project's documents and run its privacy preprocessing, waiting for both\n"
			"/providers                       list configured providers\n"
			"/model <providerId> <modelId>    override the primary model for this session's sends\n"
			"/model clear                     go back to the Project's own default model\n"
			"/research-backend <openai_hosted|openrouter>   save the global research backend\n"
			"/permission <ask_for_approval|approve_safe_actions|full_project_access>  set the open Project's action permission\n"
			"/lawyer <on|off>  let an operation ask the lawyer a question (answer it with /answer after /wait)\n"
			"/revision-author [name]          show or set who Singularity's tracked changes in Word are recorded as\n"
			"/working-document <id|new|clear> choose what this Chat works on (new: write a new document)\n"
			"/workspaces                      list workspaces\n"
			"/workspace <id>                  activate a workspace\n"
			"/compact                         carry this Chat forward in a shorter form (chat.compact)\n"
			"/legal-task <id> [version]       choose the Legal task for this session's sends (e.g. risk-review 2)\n"
			"/legal-task clear                go back to General\n"
			"/trace <off|safe|forensic>       choose how much live detail streams during a send\n"
			"/status                          show the current session state\n"
			"/chat <chatId>                   go on in a Chat the open Project already has\n"
			"/send <request>                  send on a thread of its own, so what it asks can be answered\n"
			"/wait                            wait until the operation finishes, or asks, or waits for approval\n"
			"/approve, /reject                decide the change the operation waits on\n"
			"/answer <n | text: ... | decide> answer the question the operation waits on\n"
			"/hashes                          every file of the Project folder with its SHA-256\n"
			"/agents-on                       run agents and answer AI apps in this process, as the window does\n"
			"/workspace-folder <folder>       add and use a workspace folder, as the window's folder panel does\n"
			"/call <method> [json]            call one Native API method with its parameters and print its answer\n"
			"/agent-watch <what to look for>  a data room watch of the open Project's folder, kept for changes\n"
			"/agent-task <task>               a task agent run by hand; /agent-follow <agentId> <task> one run after that agent reports\n"
			"/agents                          the agents (of the open Project) and their latest runs\n"
			"/agent-run <agentId>             run an agent now\n"
			"/agent-wait <agentId> <runId|-> [seconds]  wait for a run after that one, then print what it found\n"
			"/agent-assign <agentId|last> <work>  give an agent work once; /work-wait [seconds] waits until it is done\n"
			"/task                            print the last completed task's full canonical JSON\n"
			"/evidence                        print the last completed task's external evidence\n"
			"/help                            show this text\n"
			"/quit                            exit\n"
			"anything else                    sent as a prompt to the open Project's chat\n";
}

// One command dispatch shared by both the REPL and one-shot flag handling.
// `line` is a full command including its leading '/' (e.g. "/trace safe"),
// or, with no leading '/', literal prompt text to send.
int dispatch(Session &session, const std::string &line) {
	const std::string trimmed = trim(line);
	if (trimmed.empty()) return 0;
	if (trimmed[0] != '/') return session.send_prompt(trimmed);

	std::istringstream stream(trimmed);
	std::string command;
	stream >> command;
	std::string rest;
	std::getline(stream, rest);
	rest = trim(rest);

	if (command == "/help") { print_help(); return 0; }
	if (command == "/quit" || command == "/exit") return -1;
	if (command == "/status") { session.show_status(); return 0; }
	if (command == "/send") {
		if (rest.empty()) throw std::runtime_error("/send needs the request to send.");
		session.send_in_background(rest);
		return 0;
	}
	if (command == "/wait") { session.wait_for_operation(); return 0; }
	if (command == "/chat") {
		if (rest.empty()) throw std::runtime_error("/chat needs the id of a Chat of the open Project.");
		session.continue_chat(rest);
		return 0;
	}
	if (command == "/hashes") { session.print_hashes(); return 0; }
	if (command == "/agents-on") { session.agents_on(); return 0; }
	if (command == "/workspace-folder") {
		// The folder the window's workspace.choose would take from its panel.
		if (rest.empty()) throw std::runtime_error("/workspace-folder needs a folder.");
		std::cout << "WORKSPACES " << singularity::bridge::node_bridge::choose_workspace(rest).dump() << "\n";
		return 0;
	}
	if (command == "/call") {
		std::istringstream words(rest);
		std::string method;
		words >> method;
		std::string body;
		std::getline(words, body);
		body = trim(body);
		const Json result = session.call(method, body.empty() ? Json::object() : Json::parse(body));
		std::cout << "CALL_RESULT " << method << " " << result.dump() << "\n";
		return 0;
	}
	if (command == "/agents") { session.agents_list(); return 0; }
	if (command == "/agent-watch") { session.agent_create("data_room_watch", rest, ""); return 0; }
	if (command == "/agent-task") { session.agent_create("custom", rest, ""); return 0; }
	if (command == "/agent-follow") {
		std::istringstream words(rest);
		std::string leader;
		words >> leader;
		std::string instruction;
		std::getline(words, instruction);
		session.agent_create("custom", trim(instruction), leader);
		return 0;
	}
	if (command == "/agent-assign") {
		std::istringstream words(rest);
		std::string agentId;
		words >> agentId;
		std::string instruction;
		std::getline(words, instruction);
		session.agent_assign(agentId, trim(instruction));
		return 0;
	}
	if (command == "/work-wait") {
		session.work_wait(rest.empty() ? 900 : std::stoi(rest));
		return 0;
	}
	if (command == "/agent-run") { (void)session.call("agent.run", Json{{"agentId", rest}}); std::cout << "AGENT_RUN_REQUESTED " << rest << "\n"; return 0; }
	if (command == "/agent-wait") {
		std::istringstream words(rest);
		std::string agentId;
		std::string afterRun;
		int seconds = 300;
		words >> agentId >> afterRun >> seconds;
		if (afterRun == "-") afterRun.clear();
		session.agent_wait(agentId, afterRun, seconds);
		return 0;
	}
	if (command == "/approve") { session.decide(true); return 0; }
	if (command == "/reject") { session.decide(false); return 0; }
	if (command == "/answer") {
		if (rest.empty()) throw std::runtime_error("/answer needs an option number, \"text: ...\" or \"decide\".");
		session.answer(rest);
		return 0;
	}
	if (command == "/task") { session.show_task(); return 0; }
	if (command == "/evidence") { session.show_evidence(); return 0; }
	if (command == "/preprocess") { session.preprocess(); return 0; }
	if (command == "/providers") { session.list_providers(); return 0; }
	if (command == "/workspaces") { session.list_workspaces(); return 0; }
	if (command == "/project") {
		if (rest.empty()) throw std::runtime_error("/project needs a folder path or an existing Project id.");
		session.open_project(rest);
		return 0;
	}
	if (command == "/workspace") {
		if (rest.empty()) throw std::runtime_error("/workspace needs an id (see /workspaces).");
		session.set_workspace(rest);
		return 0;
	}
	if (command == "/research-backend") {
		if (rest.empty()) throw std::runtime_error("/research-backend needs a backend (openai_hosted|openrouter).");
		session.set_research_backend(rest);
		return 0;
	}
	if (command == "/lawyer") {
		session.set_can_ask_lawyer(rest);
		return 0;
	}
	if (command == "/permission") {
		if (rest.empty())
			throw std::runtime_error("/permission needs a mode (ask_for_approval|approve_safe_actions|full_project_access).");
		session.save_permission(rest);
		return 0;
	}
	if (command == "/working-document") {
		if (rest.empty()) throw std::runtime_error("/working-document needs a document id, new or clear.");
		session.set_working_document(rest);
		return 0;
	}
	if (command == "/revision-author") {
		// Who Singularity's tracked changes in Word are recorded as. Saved in
		// Settings; an empty name keeps the one already set.
		auto settings = singularity::platform::load_app_settings();
		if (!rest.empty()) {
			settings.revision_author = rest;
			singularity::platform::save_app_settings(settings);
		}
		std::cout << "REVISION_AUTHOR " << singularity::platform::load_app_settings().revision_author << "\n";
		return 0;
	}
	if (command == "/trace") {
		const auto level = trace_level_from_string(rest);
		if (!level.has_value()) throw std::runtime_error("/trace needs off, safe, or forensic.");
		session.set_trace(*level);
		std::cout << "TRACE_SET " << to_string(*level) << "\n";
		return 0;
	}
	if (command == "/model") {
		if (rest == "clear") { session.clear_model_override(); return 0; }
		std::istringstream args(rest);
		std::string providerId, modelId;
		args >> providerId >> modelId;
		if (providerId.empty() || modelId.empty())
			throw std::runtime_error("/model needs a providerId and a modelId (or /model clear).");
		session.set_model_override(providerId, modelId);
		return 0;
	}
	if (command == "/compact") {
		// The real product path: chat.compact on the open Chat. Nothing here
		// summarises anything itself - what is carried forward is built by the
		// backend from the Chat's own canonical records.
		const Json compacted = session.compact_open_chat();
		std::cout << "COMPACTED " << compacted.dump() << "\n";
		return 0;
	}
	if (command == "/consilium") {
		// The real product path: matter.runConsilium, the exact method the
		// frontend calls. Each member is an independent real provider call on
		// the same objective - never one model asked to role-play several.
		// "<providerId>/<modelId>,<providerId>/<modelId>,... <objective>"
		const auto split = rest.find(' ');
		if (split == std::string::npos)
			throw std::runtime_error("/consilium <provider/model,provider/model,...> <objective>");
		const std::string targetList = rest.substr(0, split);
		const std::string objective = rest.substr(split + 1);
		Json targets = Json::array();
		std::size_t start = 0;
		while (start <= targetList.size()) {
			const auto comma = targetList.find(',', start);
			const std::string entry = targetList.substr(start,
				comma == std::string::npos ? std::string::npos : comma - start);
			const auto slash = entry.find('/');
			if (slash == std::string::npos)
				throw std::runtime_error("each Consilium member is <providerId>/<modelId>");
			targets.push_back(Json{{"providerId", entry.substr(0, slash)},
				{"modelId", entry.substr(slash + 1)}});
			if (comma == std::string::npos) break;
			start = comma + 1;
		}
		const Json result = session.run_consilium(targets, objective);
		std::cout << "CONSILIUM " << result.dump() << "\n";
		return 0;
	}
	if (command == "/call") {
		// Any Native API method, with its params as JSON - the same registry and
		// the same handlers the frontend calls. Exists so an acceptance run can
		// exercise a real contract end to end without a CLI-only shortcut being
		// invented for each one. "{projectId}" is substituted with the open
		// Project's id so a caller never has to paste it.
		const auto split = rest.find(' ');
		const std::string method = rest.substr(0, split);
		std::string body = split == std::string::npos ? std::string("{}") : rest.substr(split + 1);
		for (std::string::size_type at = body.find("{projectId}"); at != std::string::npos;
				at = body.find("{projectId}")) {
			session.require_project_public();
			body.replace(at, std::string("{projectId}").size(), session.project_id());
		}
		const Json result = session.call_public(method, Json::parse(body));
		std::cout << "CALL " << result.dump() << "\n";
		return 0;
	}
	if (command == "/consilium-config") {
		// Runs a SAVED Consilium Configuration - the authoritative execution
		// plan. Proves that participants, roles, stage order/mode, objective
		// and source policy actually govern a real run.
		const Json result = session.run_consilium_configuration(rest);
		std::cout << "CONSILIUM " << result.dump() << "\n";
		return 0;
	}
	if (command == "/consilium-list") {
		const Json result = session.list_consilium();
		std::cout << "CONSILIUM_LIST " << result.dump() << "\n";
		return 0;
	}
	if (command == "/synthesize") {
		// "Synthesize with...": one real call over every member's verbatim
		// answer. "<groupId> <providerId>/<modelId>"
		const auto split = rest.find(' ');
		if (split == std::string::npos)
			throw std::runtime_error("/synthesize <groupId> <providerId>/<modelId>");
		const std::string groupId = rest.substr(0, split);
		const std::string target = rest.substr(split + 1);
		const auto slash = target.find('/');
		if (slash == std::string::npos)
			throw std::runtime_error("/synthesize <groupId> <providerId>/<modelId>");
		const Json result = session.synthesize_consilium(groupId, target.substr(0, slash),
			target.substr(slash + 1));
		std::cout << "SYNTHESIS " << result.dump() << "\n";
		return 0;
	}
	if (command == "/work-products") {
		// workProduct.list exactly as the frontend calls it - the durable
		// deliverables a lawyer keeps, not a rendering of them.
		const Json result = session.list_work_products();
		std::cout << "WORK_PRODUCTS " << result.dump() << "\n";
		return 0;
	}
	if (command == "/local-models") {
		// The same production registry every real send resolves through - never a
		// second catalogue. Shows exactly what this computer can run right now.
		const Json models = session.call_public("localModels.list", Json::object());
		std::cout << "LOCAL MODELS " << models.dump() << "\n";
		return 0;
	}
	if (command == "/legal-task") {
		if (rest == "clear") { session.clear_legal_task(); return 0; }
		std::istringstream args(rest);
		std::string presetId;
		int version = 1;
		args >> presetId;
		if (!(args >> version)) version = 1;
		if (presetId.empty())
			throw std::runtime_error("/legal-task needs a preset id (or /legal-task clear).");
		session.set_legal_task(presetId, version);
		return 0;
	}
	throw std::runtime_error("Unknown command \"" + command + "\" - try /help.");
}

struct Flags {
	std::optional<std::string> project;
	std::optional<std::string> provider;
	std::optional<std::string> model;
	std::optional<std::string> research;
	std::optional<std::string> permission;
	std::optional<std::string> trace;
	std::optional<std::string> prompt;
	// One-time local setup only: writes straight to the real credential store
	// (Provider/CredentialStore/CredentialStore.hpp) this CLI already reads
	// from for every real send, then exits - never combined with --prompt in
	// the same invocation, and never echoed anywhere (see main() below).
	std::optional<std::pair<std::string, std::string>> set_credential;
	// One-time local setup only, same discipline as set_credential above, but
	// through the real production connection path (Provider/
	// ProviderConnections/ProviderConnections.hpp's configure_provider_
	// connection) instead of a raw credential write - required for a provider
	// like "openai_compatible" whose usable model list only exists once a
	// live test_connection() against its own baseUrl has actually run. The
	// secret itself is read from stdin at use, never taken as an argv value
	// (see main() below), so it never appears in a process listing.
	std::optional<std::pair<std::string, std::string>> configure_provider;
	// A custom connection (e.g. "openrouter") needs its display name, exactly
	// as the Settings connect form sends it; built-in presets ignore it.
	std::optional<std::string> display_name;
};

Flags parse_flags(int argc, char **argv) {
	Flags flags;
	for (int i = 1; i < argc; ++i) {
		const std::string flag = argv[i];
		const auto next = [&]() -> std::string {
			if (i + 1 >= argc) throw std::runtime_error(flag + " needs a value.");
			return argv[++i];
		};
		if (flag == "--project") flags.project = next();
		else if (flag == "--provider") flags.provider = next();
		else if (flag == "--model") flags.model = next();
		else if (flag == "--research-backend") flags.research = next();
		else if (flag == "--permission") flags.permission = next();
		else if (flag == "--trace") flags.trace = next();
		else if (flag == "--prompt") flags.prompt = next();
		else if (flag == "--set-credential") {
			const std::string providerId = next();
			const std::string credential = next();
			flags.set_credential = std::make_pair(providerId, credential);
		}
		else if (flag == "--display-name") flags.display_name = next();
		else if (flag == "--configure-provider") {
			const std::string providerId = next();
			const std::string baseUrl = next();
			flags.configure_provider = std::make_pair(providerId, baseUrl);
		}
		else throw std::runtime_error("Unknown argument \"" + flag + "\".");
	}
	return flags;
}

} // namespace

// When this binary was built, from the binary itself.
//
// The CLI that reaches real providers is EXCLUDE_FROM_ALL, so an ordinary
// `cmake --build` leaves the previous one in place. That cost one acceptance
// run whose transcript was read as evidence for code the binary did not
// contain. Every run now says which build it is, first line, before anything
// else happens - so a transcript can never be attributed to the wrong code.
std::string executable_build_time() {
	std::error_code error;
	const std::filesystem::path self = std::filesystem::canonical("/proc/self/exe", error);
	std::filesystem::path path = self;
#if defined(__APPLE__)
	std::vector<char> buffer(1024);
	auto size = static_cast<std::uint32_t>(buffer.size());
	if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
		buffer.resize(size + 1);
		if (_NSGetExecutablePath(buffer.data(), &size) != 0) return "unknown";
	}
	path = std::filesystem::path(buffer.data());
#else
	if (error) return "unknown";
#endif
	const std::filesystem::path resolved = std::filesystem::canonical(path, error);
	if (error) return "unknown";
	const auto written = std::filesystem::last_write_time(resolved, error);
	if (error) return "unknown";
	const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
		written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
	const std::time_t seconds = std::chrono::system_clock::to_time_t(systemTime);
	std::tm parts{};
#if defined(_WIN32)
	gmtime_s(&parts, &seconds);
#else
	gmtime_r(&seconds, &parts);
#endif
	char stamp[32] = {0};
	std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &parts);
	return stamp;
}

int main(int argc, char **argv) {
	try {
		std::cout << "SINGULARITY_CLI_BUILD " << NATIVE_APP_VERSION << " " << executable_build_time() << "\n";
		const Flags flags = parse_flags(argc, argv);

		// Only the firm's administrator connects or removes the firm's model
		// accounts (D-184), from the command line as from the window.
		singularity::providers::set_may_connect_accounts([] { return singularity::organisation::is_administrator(); });
		if (flags.set_credential.has_value()) {
			singularity::providers::require_may_connect_accounts();
			// Deliberately the very first thing this process does, before any
			// Session/trace sink exists, so the credential value is never at
			// risk of being echoed into a live trace stream. Exits immediately
			// afterward - never combined with a real send in the same process.
			singularity::platform::set_provider_credential(
					flags.set_credential->first, flags.set_credential->second);
			std::cout << "CREDENTIAL_SET " << flags.set_credential->first << " (" << flags.set_credential->second.size()
					<< " bytes, value not echoed)\n";
			return 0;
		}

		if (flags.configure_provider.has_value()) {
			// Same discipline as --set-credential above: first thing this process
			// does, secret read from stdin (never argv, so it never appears in a
			// process listing), never echoed, exits immediately without ever
			// opening a Session.
			std::string secret;
			if (!std::getline(std::cin, secret) || secret.empty())
				throw std::runtime_error("--configure-provider needs the secret on stdin.");
			const auto connections = singularity::providers::configure_provider_connection(
					flags.configure_provider->first, secret, flags.configure_provider->second, flags.display_name);
			secret.clear();
			const auto configured = std::find_if(connections.begin(), connections.end(),
					[&](const auto &connection) { return connection.provider.id == flags.configure_provider->first; });
			const bool connected = configured != connections.end()
					&& configured->state == singularity::providers::ProviderConnectionState::Connected;
			const std::size_t modelCount = configured != connections.end() ? configured->provider.models.size() : 0;
			std::cout << "PROVIDER_CONFIGURED " << flags.configure_provider->first
					<< " status=" << (connected ? "connected" : "needs_attention")
					<< " models=" << modelCount << "\n";
			return connected ? 0 : 1;
		}

		TraceLevel initialTrace = TraceLevel::Safe;
		if (flags.trace.has_value()) {
			const auto parsed = trace_level_from_string(*flags.trace);
			if (!parsed.has_value()) throw std::runtime_error("--trace needs off, safe, or forensic.");
			initialTrace = *parsed;
		}
		Session session;
		session.set_trace(initialTrace);
		if (flags.project.has_value()) session.open_project(*flags.project);
		if (flags.model.has_value()) {
			if (!flags.provider.has_value())
				throw std::runtime_error("--model requires --provider as well.");
			session.set_model_override(*flags.provider, *flags.model);
		}
		if (flags.research.has_value()) session.set_research_backend(*flags.research);
		if (flags.permission.has_value()) session.save_permission(*flags.permission);

		if (flags.prompt.has_value()) return session.send_prompt(*flags.prompt);

		std::cout << "Singularity CLI - type /help for commands, /quit to exit.\n";
		session.show_status();
		std::string line;
		while (true) {
			std::cout << "singularity> " << std::flush;
			if (!std::getline(std::cin, line)) break;
			try {
				if (dispatch(session, line) < 0) break;
			} catch (const std::exception &error) {
				std::cerr << "ERROR " << error.what() << "\n";
			}
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "SINGULARITY_CLI_FAILED " << error.what() << "\n";
		return 1;
	}
}
