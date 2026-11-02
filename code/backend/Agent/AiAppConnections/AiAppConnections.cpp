#include "Agent/AiAppConnections/AiAppConnections.hpp"
#include "Agent/AgentRoles/AgentRoles.hpp"

#include "Agent/AgentActions/AgentActions.hpp"
#include "Agent/AgentStore/AgentStore.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/stat.h>
#endif

namespace singularity::agents {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMostLabelCharacters = 60;
constexpr std::size_t kMostMatters = 50;
constexpr std::size_t kMostRequestsPerMinute = 30;
constexpr std::size_t kMostPendingDrafts = 3;
constexpr std::size_t kMostDraftsPerDay = 10;
constexpr std::size_t kMostActivityLines = 400;
// What every key an app is given begins with, before its connection's id.
constexpr std::string_view kKeyPrefix = "singularity_";

const char *const kUnknownKey = "This key is not one Singularity gave, or it was replaced. In Singularity, choose the firm's name, "
	"then AI apps, and give this app a new key.";
const char *const kDisconnected = "The lawyer disconnected this app in Singularity.";

AiAppDependencies &installed() {
	static AiAppDependencies dependencies;
	return dependencies;
}

std::mutex &store_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::filesystem::path connections_directory() {
	return ai_apps_directory() / "connections";
}

std::filesystem::path activity_directory() {
	return ai_apps_directory() / "activity";
}

bool is_connection_id(const std::string &value) {
	if (value.size() != 20 || value.rfind("aic_", 0) != 0) return false;
	for (std::size_t index = 4; index < value.size(); ++index) {
		const char character = value[index];
		if ((character < '0' || character > '9') && (character < 'a' || character > 'f')) return false;
	}
	return true;
}

bool is_matter_id(const std::string &value) {
	if (value.empty() == true || value.size() > 80) return false;
	for (const char character : value) {
		const bool isAllowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
			|| (character >= '0' && character <= '9') || character == '_' || character == '-';
		if (isAllowed == false) return false;
	}
	return true;
}

std::string trimmed(const std::string &text) {
	const std::size_t start = text.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) return "";
	const std::size_t end = text.find_last_not_of(" \t\r\n");
	return text.substr(start, end - start + 1);
}

std::size_t characters_in(const std::string &text) {
	std::size_t count = 0;
	for (const char byte : text)
		if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) ++count;
	return count;
}

// At most `most` characters, never cutting one in two.
std::string clipped(const std::string &text, std::size_t most) {
	std::size_t count = 0;
	for (std::size_t index = 0; index < text.size(); ++index) {
		if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) {
			if (count == most) return text.substr(0, index);
			++count;
		}
	}
	return text;
}

bool equal_in_constant_time(const std::string &left, const std::string &right) {
	if (left.size() != right.size()) return false;
	unsigned char difference = 0;
	for (std::size_t index = 0; index < left.size(); ++index)
		difference |= static_cast<unsigned char>(left[index] ^ right[index]);
	return difference == 0;
}

std::string text_of(const Json &value, const char *key) {
	const auto found = value.find(key);
	if (found == value.end() || found->is_string() == false) return "";
	return found->get<std::string>();
}

std::optional<std::string> optional_text_of(const Json &value, const char *key) {
	const auto found = value.find(key);
	if (found == value.end() || found->is_string() == false) return std::nullopt;
	return found->get<std::string>();
}

Json connection_to_json(const AiAppConnection &connection) {
	return Json{
		{"formatVersion", connection.format_version},
		{"id", connection.id},
		{"label", connection.label},
		{"matterIds", connection.matter_ids},
		{"mayPauseAgents", connection.may_pause_agents},
		{"keySha256", connection.key_sha256},
		{"createdAt", connection.created_at},
		{"createdByActorId", connection.created_by_actor_id},
		{"revokedAt", connection.revoked_at.has_value() == true ? Json(*connection.revoked_at) : Json(nullptr)},
		{"lastUsedAt", connection.last_used_at.has_value() == true ? Json(*connection.last_used_at) : Json(nullptr)},
		{"lastClientName", connection.last_client_name},
		{"boundTeamId", connection.bound_team_id},
		{"boundSigningId", connection.bound_signing_id},
		{"boundSigner", connection.bound_signer},
		{"draftsProposed", connection.drafts_proposed},
	};
}

std::optional<AiAppConnection> connection_from_json(const Json &value) {
	if (value.is_object() == false) return std::nullopt;
	AiAppConnection connection;
	const auto format = value.find("formatVersion");
	if (format != value.end() && format->is_number_integer() == true) connection.format_version = format->get<int>();
	connection.id = text_of(value, "id");
	if (is_connection_id(connection.id) == false) return std::nullopt;
	connection.label = text_of(value, "label");
	const auto matters = value.find("matterIds");
	if (matters != value.end() && matters->is_array() == true)
		for (const Json &matter : *matters)
			if (matter.is_string() == true && is_matter_id(matter.get<std::string>()) == true) connection.matter_ids.push_back(matter.get<std::string>());
	const auto pause = value.find("mayPauseAgents");
	connection.may_pause_agents = pause != value.end() && pause->is_boolean() == true && pause->get<bool>() == true;
	connection.key_sha256 = text_of(value, "keySha256");
	connection.created_at = text_of(value, "createdAt");
	connection.created_by_actor_id = text_of(value, "createdByActorId");
	connection.revoked_at = optional_text_of(value, "revokedAt");
	connection.last_used_at = optional_text_of(value, "lastUsedAt");
	connection.last_client_name = text_of(value, "lastClientName");
	connection.bound_team_id = text_of(value, "boundTeamId");
	connection.bound_signing_id = text_of(value, "boundSigningId");
	connection.bound_signer = text_of(value, "boundSigner");
	const auto drafts = value.find("draftsProposed");
	if (drafts != value.end() && drafts->is_number_integer() == true) connection.drafts_proposed = drafts->get<int>();
	return connection;
}

void write_private(const std::filesystem::path &path, const std::string &bytes) {
	platform::ensure_record_directory(path.parent_path());
	platform::restrict_to_owner(ai_apps_directory());
	platform::restrict_to_owner(path.parent_path());
	const std::filesystem::path staged = platform::staged_sibling_path(path);
	try {
		platform::write_file_durably(staged, bytes);
		platform::restrict_to_owner(staged);
		platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
	} catch (...) {
		platform::discard_staged_file(staged);
		throw;
	}
}

std::optional<AiAppConnection> load_connection(const std::string &connection_id) {
	if (is_connection_id(connection_id) == false) return std::nullopt;
	std::ifstream input(connections_directory() / (connection_id + ".json"), std::ios::binary);
	if (input.is_open() == false) return std::nullopt;
	const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	const Json value = Json::parse(bytes, nullptr, false);
	if (value.is_discarded() == true) return std::nullopt;
	std::optional<AiAppConnection> connection = connection_from_json(value);
	if (connection.has_value() == true && connection->id != connection_id) return std::nullopt;
	return connection;
}

void save_connection(const AiAppConnection &connection) {
	write_private(connections_directory() / (connection.id + ".json"), connection_to_json(connection).dump(1, '\t'));
}

std::vector<AiAppConnection> load_connections() {
	std::vector<AiAppConnection> connections;
	std::error_code error;
	std::filesystem::directory_iterator entries(connections_directory(), error);
	if (error) return connections;
	for (const auto &entry : entries) {
		if (entry.path().extension() != ".json") continue;
		std::optional<AiAppConnection> connection = load_connection(entry.path().stem().string());
		if (connection.has_value() == true) connections.push_back(*connection);
	}
	std::sort(connections.begin(), connections.end(),
		[](const AiAppConnection &left, const AiAppConnection &right) { return left.created_at > right.created_at; });
	return connections;
}

void record_activity(const std::string &connection_id, const std::string &tool, bool is_refused, const std::string &detail) {
	if (is_connection_id(connection_id) == false) return;
	const std::filesystem::path path = activity_directory() / (connection_id + ".jsonl");
	try {
		platform::ensure_record_directory(path.parent_path());
		platform::restrict_to_owner(path.parent_path());
		const Json line{{"at", agent_timestamp_now()}, {"tool", tool}, {"isRefused", is_refused}, {"detail", clipped(detail, 300)}};
		{
			std::ofstream output(path, std::ios::binary | std::ios::app);
			output << line.dump() << "\n";
		}
		platform::restrict_to_owner(path);
		// The record stays short: its oldest lines go once it is long.
		std::ifstream input(path, std::ios::binary);
		std::vector<std::string> lines;
		for (std::string text; std::getline(input, text);)
			if (text.empty() == false) lines.push_back(text);
		if (lines.size() > kMostActivityLines) {
			std::string kept;
			for (std::size_t index = lines.size() - kMostActivityLines / 2; index < lines.size(); ++index) kept += lines[index] + "\n";
			write_private(path, kept);
		}
	} catch (const std::exception &) {
	}
}

AiAppSetup with_new_key(AiAppConnection connection) {
	const std::string key = std::string(kKeyPrefix) + connection.id + "_" + secure_random_hex(32);
	connection.key_sha256 = platform::sha256_hex_digest(key);
	// A new key may be used by another program: it is bound again at first use.
	connection.bound_team_id.clear();
	connection.bound_signing_id.clear();
	connection.bound_signer.clear();
	save_connection(connection);
	return AiAppSetup{connection, key};
}

std::vector<std::string> checked_matters(const std::vector<std::string> &matter_ids) {
	std::vector<std::string> checked;
	std::set<std::string> seen;
	for (const std::string &matterId : matter_ids) {
		if (is_matter_id(matterId) == false) throw AgentRefused("One of the chosen Matters is not a Matter on this computer.");
		if (seen.insert(matterId).second == true) checked.push_back(matterId);
	}
	if (checked.empty() == true) throw AgentRefused("Choose at least one Matter the app may see.");
	if (checked.size() > kMostMatters) throw AgentRefused("Choose at most 50 Matters for one app.");
	if (installed().list_matters) {
		std::set<std::string> known;
		for (const AiAppMatter &matter : installed().list_matters()) known.insert(matter.id);
		for (const std::string &matterId : checked)
			if (known.count(matterId) == 0) throw AgentRefused("One of the chosen Matters is not a Matter on this computer.");
	}
	return checked;
}

// --- Deciding a request --------------------------------------------------

std::mutex &pace_mutex() {
	static std::mutex mutex;
	return mutex;
}

// Whether the connection asked fewer than its limit in the last minute;
// counts this request when it did.
bool is_within_pace(const std::string &connection_id) {
	static std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> recent;
	const std::lock_guard<std::mutex> lock(pace_mutex());
	const auto now = std::chrono::steady_clock::now();
	std::deque<std::chrono::steady_clock::time_point> &times = recent[connection_id];
	while (times.empty() == false && now - times.front() > std::chrono::minutes(1)) times.pop_front();
	if (times.size() >= kMostRequestsPerMinute) return false;
	times.push_back(now);
	return true;
}

AiAppAnswer refused(const AiAppRequest &request, const std::string &text) {
	AiAppAnswer answer;
	answer.request_id = request.id;
	answer.is_refused = true;
	answer.text = text;
	return answer;
}

AiAppAnswer answered(const AiAppRequest &request, const std::string &text, Json data) {
	AiAppAnswer answer;
	answer.request_id = request.id;
	answer.text = text;
	answer.data = std::move(data);
	return answer;
}

const char *state_words(const std::string &state) {
	if (state == "waiting_for_approval") return "waits for the lawyer's approval";
	if (state == "running") return "is running now";
	if (state == "needs_review") return "has a result the lawyer has not reviewed yet";
	if (state == "access_needed") return "cannot read its folder";
	if (state == "failed") return "could not finish its last run";
	if (state == "paused") return "is paused";
	if (state == "stopped") return "is stopped";
	return "is at work";
}

// What the colleague does, in the one catalogue's words (Agent/AgentRoles).
const char *kind_words(const std::string &kind) {
	return agent_role_duty(kind).c_str();
}

Json agent_entry(const AgentView &view, const std::string &connection_id) {
	const Agent &agent = view.agent;
	Json entry{
		{"agentId", agent.id},
		{"matterId", agent.matter_id},
		{"name", agent.name},
		{"kind", agent.kind},
		{"state", view.state},
		{"proposedByThisApp", agent.requested_by.has_value() == true && agent.requested_by->connection_id == connection_id},
	};
	entry["lastRunAt"] = view.latest_run.has_value() == true && view.latest_run->finished_at.empty() == false
		? Json(view.latest_run->finished_at) : Json(nullptr);
	entry["nextRunAt"] = agent.next_check_at.has_value() == true ? Json(*agent.next_check_at) : Json(nullptr);
	return entry;
}

std::map<std::string, std::string> granted_matters(const AiAppConnection &connection) {
	std::map<std::string, std::string> names;
	const std::set<std::string> granted(connection.matter_ids.begin(), connection.matter_ids.end());
	if (installed().list_matters) {
		for (const AiAppMatter &matter : installed().list_matters())
			if (granted.count(matter.id) != 0) names[matter.id] = matter.name;
	}
	return names;
}

std::optional<std::string> argument_text(const Json &arguments, const char *key) {
	const auto found = arguments.find(key);
	if (found == arguments.end() || found->is_null() == true) return std::nullopt;
	if (found->is_string() == false) throw AgentRefused(std::string("\"") + key + "\" must be text.");
	return found->get<std::string>();
}

// HH:MM in the day, as minutes.
int minutes_of(const std::string &time) {
	if (time.size() != 5 || time[2] != ':') throw AgentRefused("\"time\" must be written as HH:MM, such as 09:00.");
	for (const std::size_t index : {0, 1, 3, 4})
		if (time[index] < '0' || time[index] > '9') throw AgentRefused("\"time\" must be written as HH:MM, such as 09:00.");
	const int hours = (time[0] - '0') * 10 + (time[1] - '0');
	const int minutes = (time[3] - '0') * 10 + (time[4] - '0');
	if (hours > 23 || minutes > 59) throw AgentRefused("\"time\" must be a time of day, such as 09:00.");
	return hours * 60 + minutes;
}

AiAppAnswer list_matters_tool(const AiAppRequest &request, const AiAppConnection &connection) {
	const std::map<std::string, std::string> matters = granted_matters(connection);
	Json list = Json::array();
	std::ostringstream text;
	text << "Matters this app may see in Singularity:";
	for (const auto &[matterId, name] : matters) {
		list.push_back(Json{{"matterId", matterId}, {"name", name}});
		text << "\n- " << name << " (" << matterId << ")";
	}
	if (matters.empty() == true) text.str("This app may see no Matter in Singularity now.");
	return answered(request, text.str(), Json{{"matters", list}});
}

// Whether the app may see a colleague: one of a Matter it may see, or one of
// the whole firm (D-157), which belongs to no Matter - its name, office and
// state only, to an app the lawyer let see any Matter.
bool isSeen(const AgentView &view, const std::map<std::string, std::string> &matters) {
	return matters.count(view.agent.matter_id) > 0 || (view.agent.matter_id.empty() == true && matters.empty() == false);
}

// Where a colleague the app may see works: its Matter, or the whole firm.
std::string whereOf(const AgentView &view, const std::map<std::string, std::string> &matters) {
	const auto found = matters.find(view.agent.matter_id);
	return found != matters.end() ? found->second : std::string("the firm");
}

AiAppAnswer list_agents_tool(const AiAppRequest &request, const AiAppConnection &connection) {
	const std::map<std::string, std::string> matters = granted_matters(connection);
	const std::optional<std::string> matterId = argument_text(request.arguments, "matterId");
	if (matterId.has_value() == true && matters.count(*matterId) == 0)
		throw AgentRefused("This app may not see a Matter with that id. Use list_matters to see the ones it may.");
	Json list = Json::array();
	std::ostringstream text;
	for (const AgentView &view : list_agent_views(matterId)) {
		if (isSeen(view, matters) == false) continue;
		list.push_back(agent_entry(view, connection.id));
		text << "- " << view.agent.name << " (" << view.agent.id << ") in " << whereOf(view, matters) << ": "
			<< kind_words(view.agent.kind) << ", " << state_words(view.state) << ".\n";
	}
	const std::string words = list.empty() == true ? "There is no agent in the Matters this app may see." : text.str();
	return answered(request, words, Json{{"agents", list}});
}

AiAppAnswer get_agent_tool(const AiAppRequest &request, const AiAppConnection &connection) {
	const std::optional<std::string> agentId = argument_text(request.arguments, "agentId");
	if (agentId.has_value() == false || agentId->empty() == true) throw AgentRefused("Give the agent's id (\"agentId\").");
	const std::map<std::string, std::string> matters = granted_matters(connection);
	std::optional<AgentView> found;
	try {
		found = agent_view(*agentId);
	} catch (const std::exception &) {
	}
	// An agent outside the app's Matters is answered as one that does not exist.
	if (found.has_value() == false || isSeen(*found, matters) == false)
		throw AgentRefused("There is no agent with that id in the Matters this app may see.");
	std::ostringstream text;
	text << found->agent.name << " in " << whereOf(*found, matters) << " " << kind_words(found->agent.kind) << " and "
		<< state_words(found->state) << ".";
	return answered(request, text.str(), agent_entry(*found, connection.id));
}

// An agent of the app's Matters, or a refusal that says nothing of agents
// outside them.
AgentView granted_agent(const AiAppRequest &request, const std::map<std::string, std::string> &matters) {
	const std::optional<std::string> agentId = argument_text(request.arguments, "agentId");
	if (agentId.has_value() == false || agentId->empty() == true) throw AgentRefused("Give the agent's id (\"agentId\").");
	std::optional<AgentView> found;
	try {
		found = agent_view(*agentId);
	} catch (const std::exception &) {
	}
	if (found.has_value() == false || matters.count(found->agent.matter_id) == 0)
		throw AgentRefused("There is no agent with that id in the Matters this app may see.");
	return *found;
}

AiAppAnswer get_agent_run_tool(const AiAppRequest &request, const AiAppConnection &connection) {
	const std::map<std::string, std::string> matters = granted_matters(connection);
	const AgentView view = granted_agent(request, matters);
	const std::optional<std::string> runId = argument_text(request.arguments, "runId");
	std::optional<AgentRun> run = view.latest_run;
	if (runId.has_value() == true && runId->empty() == false) {
		try {
			run = load_run(view.agent.id, *runId);
		} catch (const std::exception &) {
			run.reset();
		}
		if (run.has_value() == false) throw AgentRefused("This agent has no run with that id.");
	}
	if (run.has_value() == false) return answered(request, view.agent.name + " has not run yet.", Json{{"agentId", view.agent.id}, {"run", nullptr}});
	// How the run went, never what it read: no document's name or words.
	const Json counts{{"files", run->counts.files}, {"added", run->counts.added}, {"changed", run->counts.changed},
		{"removed", run->counts.removed}, {"moved", run->counts.moved}, {"unreadable", run->counts.unreadable}};
	Json data{
		{"agentId", view.agent.id}, {"runId", run->id}, {"status", run->status}, {"trigger", run->trigger},
		{"startedAt", run->started_at}, {"finishedAt", run->finished_at.empty() == true ? Json(nullptr) : Json(run->finished_at)},
		{"isBaseline", run->is_baseline}, {"counts", counts}, {"findings", run->findings.size()},
		{"needsReview", run_needs_review(*run) == true && run->reviewed_at.has_value() == false},
	};
	std::ostringstream text;
	text << view.agent.name << ": run " << run->status;
	if (run->status == "completed" && run->is_baseline == false)
		text << " - " << run->counts.changed << " changed, " << run->counts.added << " new, " << run->counts.removed << " removed, "
			<< run->findings.size() << " points for the lawyer";
	text << ".";
	return answered(request, text.str(), data);
}

AiAppAnswer pause_agent_tool(const AiAppRequest &request, const AiAppConnection &connection) {
	if (connection.may_pause_agents == false)
		throw AgentRefused("The lawyer did not allow this app to pause agents. They can connect it again in Singularity (the "
			"firm's name, then AI apps) and allow it there.");
	const std::map<std::string, std::string> matters = granted_matters(connection);
	const AgentView view = granted_agent(request, matters);
	const AgentView paused = pause_agent(view.agent.id);
	return answered(request, paused.agent.name + " is paused. The lawyer resumes it in Singularity.",
		Json{{"agentId", paused.agent.id}, {"state", paused.state}});
}

AiAppAnswer draft_agent_tool(const AiAppRequest &request, AiAppConnection &connection) {
	const std::map<std::string, std::string> matters = granted_matters(connection);
	const std::optional<std::string> matterId = argument_text(request.arguments, "matterId");
	if (matterId.has_value() == false || matters.count(*matterId) == 0)
		throw AgentRefused("This app may not propose an agent for that Matter. Use list_matters to see the ones it may.");

	// The app's own drafts the lawyer has not decided, and those of the last day.
	std::size_t pending = 0;
	std::size_t lastDay = 0;
	const std::string dayAgo = [] {
		const std::time_t then = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() - std::chrono::hours(24));
		std::tm utc {};
#if defined(_WIN32)
		gmtime_s(&utc, &then);
#else
		gmtime_r(&then, &utc);
#endif
		char buffer[32];
		std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
		return std::string(buffer);
	}();
	for (const AgentView &view : list_agent_views(std::nullopt)) {
		if (view.agent.requested_by.has_value() == false || view.agent.requested_by->connection_id != connection.id) continue;
		if (view.agent.status == AgentStatus::Draft) ++pending;
		if (view.agent.requested_by->requested_at >= dayAgo) ++lastDay;
	}
	if (pending >= kMostPendingDrafts)
		throw AgentRefused("This app already has 3 proposals waiting for the lawyer. Wait until they decide them.");
	if (lastDay >= kMostDraftsPerDay) throw AgentRefused("This app has proposed 10 agents in the last day, its limit. Try again tomorrow.");

	AgentSettings settings;
	settings.matter_id = *matterId;
	settings.name = argument_text(request.arguments, "name").value_or("");
	settings.goal = argument_text(request.arguments, "instruction").value_or("");
	settings.kind = argument_text(request.arguments, "kind").value_or("custom");
	const std::set<std::string> watching = {"data_room_watch", "deadlines", "new_documents"};
	settings.mode = watching.count(settings.kind) != 0 ? AgentMode::Changes : AgentMode::Task;
	const std::string schedule = argument_text(request.arguments, "schedule").value_or(settings.mode == AgentMode::Changes ? "on_change" : "manual");
	if (schedule == "manual") settings.trigger = AgentTrigger::Manual;
	else if (schedule == "on_change") settings.trigger = AgentTrigger::OnChange;
	else if (schedule == "daily") settings.trigger = AgentTrigger::Daily;
	else if (schedule == "weekly") settings.trigger = AgentTrigger::Weekly;
	else throw AgentRefused("\"schedule\" must be manual, on_change, daily or weekly.");
	const std::optional<std::string> time = argument_text(request.arguments, "time");
	if (time.has_value() == true) settings.daily_minute = minutes_of(*time);
	const auto weekday = request.arguments.find("weekday");
	if (weekday != request.arguments.end() && weekday->is_null() == false) {
		if (weekday->is_number_integer() == false || weekday->get<int>() < 0 || weekday->get<int>() > 6)
			throw AgentRefused("\"weekday\" must be a number from 0 (Sunday) to 6 (Saturday).");
		settings.weekly_day = weekday->get<int>();
	}

	AgentRequester requester;
	requester.connection_id = connection.id;
	requester.connection_label = connection.label;
	requester.client_name = clipped(trimmed(request.client_name), 80);
	// What the helper reported of the program that ran it. Singularity cannot
	// verify it: the request reaches it as a file any program of this user
	// could have written, so it is shown as reported, never as verified.
	requester.signer = request.caller.is_signer_verified == true ? signer_organisation(request.caller.signer) : "";
	requester.is_signer_verified = false;
	requester.request_id = request.id;
	// Saved only while this request's key is still the connection's and the
	// app is still connected, both asked under the store's lock: a
	// disconnection declines the drafts it finds once the app is marked
	// disconnected (revoke_ai_app), so a draft saved after that would wait
	// for the lawyer from an app that is gone.
	const AgentView draft = [&] {
		const std::lock_guard<std::mutex> lock(store_mutex());
		const std::optional<AiAppConnection> current = load_connection(connection.id);
		if (current.has_value() == false || current->revoked_at.has_value() == true) throw AgentRefused(kDisconnected);
		if (current->key_sha256 != connection.key_sha256) throw AgentRefused(kUnknownKey);
		return draft_agent_for_app(settings, requester);
	}();
	connection.drafts_proposed += 1;
	std::ostringstream text;
	text << "Proposed \"" << draft.agent.name << "\" for " << matters.at(*matterId) << ". It does nothing until the lawyer approves it in "
		"Singularity, where they choose the folder it may read, when it runs and which model it uses - or decline it.";
	return answered(request, text.str(), Json{{"agentId", draft.agent.id}, {"matterId", *matterId}, {"state", draft.state}});
}

// Takes one waiting request and leaves its answer. Nothing a request file
// holds can end Singularity: whatever goes wrong with one is that request's
// alone, and the inbox goes on.
void answer_waiting(const std::string &request_id) {
	try {
		const std::optional<AiAppRequest> request = take_request(request_id);
		if (request.has_value() == false) return;
		const AiAppAnswer answer = is_request_stale(*request) == true
			? refused(*request, "This request waited too long, so Singularity did not act on it. Ask again.")
			: answer_request(*request);
		leave_answer(answer);
	} catch (...) {
	}
}

} // namespace

void install_ai_app_dependencies(AiAppDependencies dependencies) {
	installed() = std::move(dependencies);
}

std::optional<std::string> connection_id_of_key(const std::string &key) {
	// "singularity_" + "aic_" + 16 hex + "_" + 64 hex
	const std::size_t start = kKeyPrefix.size();
	if (key.size() != start + 20 + 1 + 64 || key.rfind(kKeyPrefix, 0) != 0 || key[start + 20] != '_') return std::nullopt;
	const std::string connectionId = key.substr(start, 20);
	if (is_connection_id(connectionId) == false) return std::nullopt;
	for (std::size_t index = start + 21; index < key.size(); ++index)
		if ((key[index] < '0' || key[index] > '9') && (key[index] < 'a' || key[index] > 'f')) return std::nullopt;
	return connectionId;
}

std::string signer_organisation(const std::string &signer) {
	std::string organisation = signer;
	for (const char *prefix : {"Developer ID Application: ", "Apple Development: ", "Apple Distribution: "})
		if (organisation.rfind(prefix, 0) == 0) organisation = organisation.substr(std::string(prefix).size());
	if (organisation == "Software Signing") return "Apple";
	// Apple's certificates end in the team's ten-character id, " (Q6L2SF6YDW)".
	// Words a Windows signer's own name holds in brackets stay.
	const std::size_t team = organisation.rfind(" (");
	const auto is_team_id = [&] {
		for (std::size_t index = team + 2; index < team + 12; ++index) {
			const char character = organisation[index];
			if ((character < 'A' || character > 'Z') && (character < '0' || character > '9')) return false;
		}
		return true;
	};
	if (team != std::string::npos && organisation.size() == team + 13 && organisation.back() == ')' && is_team_id())
		organisation = organisation.substr(0, team);
	return organisation;
}

AiAppSetup connect_ai_app(const std::string &label, const std::vector<std::string> &matter_ids, const std::string &actor_id,
		bool may_pause_agents) {
	const std::string name = trimmed(label);
	if (name.empty() == true) throw AgentRefused("Name the app, so you know it later: Claude Code, for instance.");
	if (characters_in(name) > kMostLabelCharacters) throw AgentRefused("The app's name is longer than 60 characters.");
	AiAppConnection connection;
	connection.id = "aic_" + secure_random_hex(8);
	connection.label = name;
	connection.matter_ids = checked_matters(matter_ids);
	connection.may_pause_agents = may_pause_agents;
	connection.created_at = agent_timestamp_now();
	connection.created_by_actor_id = actor_id;
	AiAppSetup setup;
	{
		const std::lock_guard<std::mutex> lock(store_mutex());
		setup = with_new_key(connection);
	}
	record_activity(connection.id, "connect", false, "Connected by the lawyer.");
	AiAppInbox::shared().connections_changed();
	return setup;
}

AiAppSetup renew_ai_app_key(const std::string &connection_id) {
	AiAppSetup setup;
	{
		const std::lock_guard<std::mutex> lock(store_mutex());
		std::optional<AiAppConnection> connection = load_connection(connection_id);
		if (connection.has_value() == false) throw AgentRefused("This app is no longer connected.");
		if (connection->revoked_at.has_value() == true) throw AgentRefused("This app was disconnected. Connect it again.");
		setup = with_new_key(*connection);
	}
	record_activity(connection_id, "renew_key", false, "A new key was made; the old one stopped working.");
	return setup;
}

AiAppConnection revoke_ai_app(const std::string &connection_id) {
	AiAppConnection connection;
	{
		const std::lock_guard<std::mutex> lock(store_mutex());
		std::optional<AiAppConnection> found = load_connection(connection_id);
		if (found.has_value() == false) throw AgentRefused("This app is no longer connected.");
		connection = *found;
		if (connection.revoked_at.has_value() == false) {
			connection.revoked_at = agent_timestamp_now();
			save_connection(connection);
		}
	}
	// What it proposed and the lawyer has not decided goes with it.
	for (const AgentView &view : list_agent_views(std::nullopt)) {
		if (view.agent.status != AgentStatus::Draft || view.agent.requested_by.has_value() == false) continue;
		if (view.agent.requested_by->connection_id != connection_id) continue;
		try {
			(void)decline_draft(view.agent.id);
		} catch (const std::exception &) {
		}
	}
	record_activity(connection_id, "disconnect", false, "Disconnected by the lawyer.");
	AiAppInbox::shared().connections_changed();
	return connection;
}

std::vector<AiAppConnection> list_ai_apps() {
	const std::lock_guard<std::mutex> lock(store_mutex());
	return load_connections();
}

std::vector<AiAppActivity> ai_app_activity(const std::string &connection_id, std::size_t most) {
	std::vector<AiAppActivity> activity;
	if (is_connection_id(connection_id) == false) return activity;
	std::ifstream input(activity_directory() / (connection_id + ".jsonl"), std::ios::binary);
	for (std::string line; std::getline(input, line);) {
		const Json value = Json::parse(line, nullptr, false);
		if (value.is_discarded() == true || value.is_object() == false) continue;
		AiAppActivity entry;
		entry.at = text_of(value, "at");
		entry.tool = text_of(value, "tool");
		const auto refusal = value.find("isRefused");
		entry.is_refused = refusal != value.end() && refusal->is_boolean() == true && refusal->get<bool>() == true;
		entry.detail = text_of(value, "detail");
		activity.push_back(entry);
	}
	std::reverse(activity.begin(), activity.end());
	if (activity.size() > most) activity.resize(most);
	return activity;
}

AiAppAnswer answer_request(const AiAppRequest &request) {
	const std::optional<std::string> connectionId = connection_id_of_key(request.key);
	if (connectionId.has_value() == false) return refused(request, kUnknownKey);

	AiAppConnection connection;
	{
		const std::lock_guard<std::mutex> lock(store_mutex());
		std::optional<AiAppConnection> found = load_connection(*connectionId);
		if (found.has_value() == false || equal_in_constant_time(found->key_sha256, platform::sha256_hex_digest(request.key)) == false)
			return refused(request, kUnknownKey);
		connection = *found;
	}
	const auto refuse = [&](const std::string &text) {
		record_activity(connection.id, request.tool, true, text);
		return refused(request, text);
	};
	if (connection.revoked_at.has_value() == true) return refuse(kDisconnected);
	if (is_within_pace(connection.id) == false) return refuse("This app asked too often. Wait a minute, then ask again.");

	// The program using the key, as the helper reports it: bound at first use,
	// held to it after. A consistency check against a key pasted into the
	// wrong app - not a defence against a program that writes the request file
	// itself, which can report anything (the key is the access credential).
	if (connection.bound_team_id.empty() == false) {
		const bool isSame = request.caller.is_signer_verified == true && request.caller.team_id == connection.bound_team_id
			&& request.caller.signing_id == connection.bound_signing_id;
		if (isSame == false)
			return refuse("This key belongs to " + signer_organisation(connection.bound_signer) + "'s program, and another program used it. "
				"In Singularity, make a new key for the app that should use it.");
	}

	AiAppAnswer answer;
	try {
		if (request.tool == "list_matters") answer = list_matters_tool(request, connection);
		else if (request.tool == "list_agents") answer = list_agents_tool(request, connection);
		else if (request.tool == "get_agent") answer = get_agent_tool(request, connection);
		else if (request.tool == "draft_agent") answer = draft_agent_tool(request, connection);
		else if (request.tool == "get_agent_run") answer = get_agent_run_tool(request, connection);
		else if (request.tool == "pause_agent") answer = pause_agent_tool(request, connection);
		else return refuse("Singularity has no tool named \"" + clipped(request.tool, 40) + "\".");
	} catch (const AgentRefused &refusal) {
		return refuse(refusal.what());
	} catch (const std::exception &) {
		return refuse("Singularity could not answer this request. Nothing was changed; ask again.");
	}

	{
		const std::lock_guard<std::mutex> lock(store_mutex());
		std::optional<AiAppConnection> current = load_connection(connection.id);
		if (current.has_value() == true) {
			current->drafts_proposed = connection.drafts_proposed;
			// Used and bound only as the connection still is: a key renewed
			// meanwhile is bound by its own first request, never by this
			// one's program, and a disconnected app is not said to be used.
			if (current->key_sha256 == connection.key_sha256 && current->revoked_at.has_value() == false) {
				current->last_used_at = agent_timestamp_now();
				current->last_client_name = clipped(trimmed(request.client_name), 80);
				if (current->bound_team_id.empty() == true && request.caller.is_signer_verified == true && request.caller.team_id.empty() == false) {
					current->bound_team_id = request.caller.team_id;
					current->bound_signing_id = request.caller.signing_id;
					current->bound_signer = request.caller.signer;
				}
			}
			try {
				save_connection(*current);
			} catch (const std::exception &) {
			}
		}
	}
	record_activity(connection.id, request.tool, false,
		request.tool == "draft_agent" || request.tool == "pause_agent" ? answer.text : "Answered.");
	return answer;
}

// --- The inbox -------------------------------------------------------------

AiAppInbox &AiAppInbox::shared() {
	static AiAppInbox inbox;
	return inbox;
}

AiAppInbox::~AiAppInbox() {
	stop();
}

void AiAppInbox::start() {
	const std::lock_guard<std::mutex> lock(mutex_);
	if (is_started_ == true) return;
	is_started_ = true;
	is_stopping_ = false;
	thread_ = std::thread([this] { loop(); });
}

void AiAppInbox::stop() {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		if (is_started_ == false) return;
		is_stopping_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable() == true) thread_.join();
	const std::lock_guard<std::mutex> lock(mutex_);
	is_started_ = false;
}

void AiAppInbox::connections_changed() {
	bool hasConnections = false;
	for (const AiAppConnection &connection : list_ai_apps())
		if (connection.revoked_at.has_value() == false) hasConnections = true;
	has_connections_ = hasConnections;
	wake_.notify_all();
}

void AiAppInbox::loop() {
	try {
		connections_changed();
	} catch (...) {
	}
	auto saidOpen = std::chrono::steady_clock::time_point{};
	auto cleaned = std::chrono::steady_clock::time_point{};
	while (true) {
		const auto now = std::chrono::steady_clock::now();
		if (now - saidOpen >= std::chrono::seconds(10)) {
			try {
				say_workspace_open();
			} catch (const std::exception &) {
			}
			saidOpen = now;
		}
		// Before any request, at launch too: one left long ago is removed
		// unread, never acted on.
		if (now - cleaned >= std::chrono::minutes(1)) {
			try {
				discard_stale_answers(std::chrono::minutes(10));
			} catch (...) {
			}
			cleaned = now;
		}
		std::vector<std::string> waiting;
		try {
			waiting = waiting_request_ids();
		} catch (...) {
		}
		for (const std::string &requestId : waiting) answer_waiting(requestId);
		std::unique_lock<std::mutex> lock(mutex_);
		// Often while an app is connected, so its requests are answered at
		// once; seldom otherwise.
		wake_.wait_for(lock, has_connections_ == true ? std::chrono::milliseconds(250) : std::chrono::milliseconds(2000),
			[this] { return is_stopping_; });
		if (is_stopping_ == true) break;
	}
	say_workspace_closed();
}

} // namespace singularity::agents
