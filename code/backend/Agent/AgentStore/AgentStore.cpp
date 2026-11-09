#include "Agent/AgentStore/AgentStore.hpp"

#include "Platform/Events/Events.hpp"

#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>
#include <map>
#include <sstream>
#include <system_error>

namespace singularity::agents {
namespace {

using Json = nlohmann::json;

bool is_safe_identifier(const std::string &value) {
	if (value.empty() == true || value.size() > 80) return false;
	for (const char character : value) {
		const bool isAllowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
		if (isAllowed == false) return false;
	}
	return true;
}

bool is_hex_digest(const std::string &value) {
	if (value.size() != 64) return false;
	for (const char character : value)
		if ((character < '0' || character > '9') && (character < 'a' || character > 'f')) return false;
	return true;
}

std::filesystem::path agent_folder(const std::string &agent_id) {
	// An id names a folder: anything but the id alphabet would let a request
	// reach outside the agents' own folder.
	if (is_safe_identifier(agent_id) == false) throw AgentRefused("This agent does not exist.");
	return agents_directory() / agent_id;
}

std::optional<Json> read_json(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	if (input.is_open() == false) return std::nullopt;
	const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	Json value = Json::parse(bytes, nullptr, false);
	if (value.is_discarded() == true) return std::nullopt;
	return value;
}

void write_json(const std::filesystem::path &path, const Json &value) {
	platform::ensure_record_directory(path.parent_path());
	const std::string bytes = value.dump(1, '\t');
	const std::filesystem::path staged = platform::staged_sibling_path(path);
	try {
		platform::write_file_durably(staged, bytes);
		platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
	} catch (...) {
		platform::discard_staged_file(staged);
		throw;
	}
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

template <typename Number>
Number number_of(const Json &value, const char *key, Number fallback) {
	const auto found = value.find(key);
	if (found == value.end() || found->is_number() == false) return fallback;
	return found->get<Number>();
}

bool flag_of(const Json &value, const char *key, bool fallback) {
	const auto found = value.find(key);
	if (found == value.end() || found->is_boolean() == false) return fallback;
	return found->get<bool>();
}

std::vector<std::string> texts_of(const Json &value, const char *key) {
	std::vector<std::string> result;
	const auto found = value.find(key);
	if (found == value.end() || found->is_array() == false) return result;
	for (const Json &item : *found)
		if (item.is_string() == true) result.push_back(item.get<std::string>());
	return result;
}

void put_optional(Json &value, const char *key, const std::optional<std::string> &text) {
	if (text.has_value() == true) value[key] = *text;
	else value[key] = nullptr;
}

Json quotes_to_json(const std::vector<EvidenceQuote> &quotes) {
	Json result = Json::array();
	for (const EvidenceQuote &quote : quotes) result.push_back({{"text", quote.text}, {"location", quote.location}});
	return result;
}

std::vector<EvidenceQuote> quotes_from_json(const Json &value, const char *key) {
	std::vector<EvidenceQuote> result;
	const auto found = value.find(key);
	if (found == value.end() || found->is_array() == false) return result;
	for (const Json &item : *found) {
		if (item.is_object() == false) continue;
		result.push_back({text_of(item, "text"), text_of(item, "location")});
	}
	return result;
}

Json requester_to_json(const AgentRequester &requester) {
	return {
		{"connectionId", requester.connection_id},
		{"connectionLabel", requester.connection_label},
		{"clientName", requester.client_name},
		{"signer", requester.signer},
		{"isSignerVerified", requester.is_signer_verified},
		{"requestId", requester.request_id},
		{"requestedAt", requester.requested_at},
		{"requestedName", requester.requested_name},
		{"requestedGoal", requester.requested_goal},
		{"requestedFolder", requester.requested_folder},
		{"requestedTrigger", requester.requested_trigger},
	};
}

AgentRequester requester_from_json(const Json &value) {
	AgentRequester requester;
	requester.connection_id = text_of(value, "connectionId");
	requester.connection_label = text_of(value, "connectionLabel");
	requester.client_name = text_of(value, "clientName");
	requester.signer = text_of(value, "signer");
	requester.is_signer_verified = flag_of(value, "isSignerVerified", false);
	requester.request_id = text_of(value, "requestId");
	requester.requested_at = text_of(value, "requestedAt");
	requester.requested_name = text_of(value, "requestedName");
	requester.requested_goal = text_of(value, "requestedGoal");
	requester.requested_folder = text_of(value, "requestedFolder");
	requester.requested_trigger = text_of(value, "requestedTrigger");
	return requester;
}

} // namespace

const char *to_string(AgentMode mode) {
	return mode == AgentMode::Task ? "task" : "changes";
}

std::optional<AgentMode> agent_mode_from_string(const std::string &value) {
	if (value == "changes") return AgentMode::Changes;
	if (value == "task") return AgentMode::Task;
	return std::nullopt;
}

const char *to_string(AgentStatus status) {
	switch (status) {
		case AgentStatus::Draft: return "draft";
		case AgentStatus::Active: return "active";
		case AgentStatus::Paused: return "paused";
		case AgentStatus::Stopped: return "stopped";
	}
	return "active";
}

const char *to_string(AgentTrigger trigger) {
	switch (trigger) {
		case AgentTrigger::Manual: return "manual";
		case AgentTrigger::OnChange: return "on_change";
		case AgentTrigger::Daily: return "daily";
		case AgentTrigger::Weekly: return "weekly";
		case AgentTrigger::AfterAgent: return "after_agent";
	}
	return "on_change";
}

const char *to_string(AgentModelPolicy policy) {
	switch (policy) {
		case AgentModelPolicy::MatterSetup: return "matter_setup";
		case AgentModelPolicy::LocalOnly: return "local_only";
	}
	return "matter_setup";
}

std::optional<AgentStatus> agent_status_from_string(const std::string &value) {
	if (value == "draft") return AgentStatus::Draft;
	if (value == "active") return AgentStatus::Active;
	if (value == "paused") return AgentStatus::Paused;
	if (value == "stopped") return AgentStatus::Stopped;
	return std::nullopt;
}

std::optional<AgentTrigger> agent_trigger_from_string(const std::string &value) {
	if (value == "manual") return AgentTrigger::Manual;
	if (value == "on_change") return AgentTrigger::OnChange;
	if (value == "daily") return AgentTrigger::Daily;
	if (value == "weekly") return AgentTrigger::Weekly;
	if (value == "after_agent") return AgentTrigger::AfterAgent;
	return std::nullopt;
}

std::optional<AgentModelPolicy> agent_model_policy_from_string(const std::string &value) {
	if (value == "matter_setup") return AgentModelPolicy::MatterSetup;
	if (value == "local_only") return AgentModelPolicy::LocalOnly;
	return std::nullopt;
}

std::filesystem::path agents_directory() {
	return platform::application_data_directory() / "Agents";
}

std::mutex &agent_store_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::string new_agent_id() {
	return "agent_" + secure_random_hex(12);
}

std::string new_memory_note_id() {
	return "mem_" + secure_random_hex(12);
}

namespace {

SharedPlaces &installed_places() {
	static SharedPlaces places;
	return places;
}

// A computer's own file of notes: named by its device id where that is a plain
// name, by "device" otherwise.
std::string memory_file_name(const std::string &device_id) {
	return (is_safe_identifier(device_id) == true ? device_id : std::string("device")) + ".json";
}

std::filesystem::path local_memory_folder(const std::string &agent_id) {
	return agent_folder(agent_id) / "memory";
}

std::filesystem::path shared_memory_folder(const std::filesystem::path &root, const std::string &agent_id) {
	return root / ".lm" / "Agents" / agent_id / "Memory";
}

// Every folder a colleague's notes may be in here: its own, the firm's
// record's, and each Matter's.
std::vector<std::filesystem::path> memory_folders(const std::string &agent_id) {
	std::vector<std::filesystem::path> folders{local_memory_folder(agent_id)};
	const SharedPlaces &places = installed_places();
	try {
		if (places.firm) {
			if (const std::optional<std::filesystem::path> firm = places.firm()) folders.push_back(shared_memory_folder(*firm, agent_id));
		}
		if (places.matters) {
			for (const std::filesystem::path &matter : places.matters()) folders.push_back(shared_memory_folder(matter, agent_id));
		}
	} catch (const std::exception &) {
		// What cannot be reached now is read when it can be.
	}
	return folders;
}

// Where a note travels: a Matter's note in that Matter's own folder, so only
// its computers hold it; the firm's and a member's in the firm's record; any
// other, and any whose place cannot be reached, here.
std::filesystem::path memory_folder_for(const std::string &agent_id, const MemoryNote &note) {
	const SharedPlaces &places = installed_places();
	try {
		if (note.scope == "matter") {
			if (note.matter_id.empty() == false && places.matter) {
				if (const std::optional<std::filesystem::path> folder = places.matter(note.matter_id)) return shared_memory_folder(*folder, agent_id);
			}
			return local_memory_folder(agent_id);
		}
		if (places.firm) {
			if (const std::optional<std::filesystem::path> firm = places.firm()) return shared_memory_folder(*firm, agent_id);
		}
	} catch (const std::exception &) {
	}
	return local_memory_folder(agent_id);
}

Json memory_note_json(const MemoryNote &note) {
	return {
		{"id", note.id},
		{"kind", note.kind},
		{"scope", note.scope},
		{"matterId", note.matter_id},
		{"matterName", note.matter_name},
		{"personActorId", note.person_actor_id},
		{"personName", note.person_name},
		{"text", note.text},
		{"projectId", note.project_id},
		{"chatId", note.chat_id},
		{"messageId", note.message_id},
		{"saidByActorId", note.said_by_actor_id},
		{"saidByName", note.said_by_name},
		{"at", note.at},
		{"deviceId", note.device_id},
		{"retires", note.retires},
	};
}

MemoryNote memory_note_of(const Json &value) {
	MemoryNote note;
	note.id = text_of(value, "id");
	note.kind = text_of(value, "kind");
	note.scope = text_of(value, "scope");
	note.matter_id = text_of(value, "matterId");
	note.matter_name = text_of(value, "matterName");
	note.person_actor_id = text_of(value, "personActorId");
	note.person_name = text_of(value, "personName");
	note.text = text_of(value, "text");
	note.project_id = text_of(value, "projectId");
	note.chat_id = text_of(value, "chatId");
	note.message_id = text_of(value, "messageId");
	note.said_by_actor_id = text_of(value, "saidByActorId");
	note.said_by_name = text_of(value, "saidByName");
	note.at = text_of(value, "at");
	note.device_id = text_of(value, "deviceId");
	note.retires = text_of(value, "retires");
	return note;
}

std::vector<MemoryNote> notes_in(const std::filesystem::path &path) {
	std::vector<MemoryNote> notes;
	const std::optional<Json> value = read_json(path);
	if (value.has_value() == false || value->is_object() == false) return notes;
	const auto listed = value->find("notes");
	if (listed == value->end() || listed->is_array() == false) return notes;
	for (const Json &item : *listed) {
		if (item.is_object() == false) continue;
		MemoryNote note = memory_note_of(item);
		if (note.id.empty() == false) notes.push_back(std::move(note));
	}
	return notes;
}

} // namespace

void install_shared_places(SharedPlaces places) {
	installed_places() = std::move(places);
}

namespace {

std::optional<std::filesystem::path> firm_record() {
	const SharedPlaces &places = installed_places();
	if (!places.firm) return std::nullopt;
	try {
		return places.firm();
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

Json colleague_definition(const Agent &agent) {
	return {
		{"formatVersion", 1},
		{"agentId", agent.id},
		{"name", agent.name},
		{"character", agent.character},
		{"goal", agent.goal},
		{"manner", agent.manner},
		{"office", {{"title", agent.office.title}, {"ranksWith", agent.office.ranks_with}, {"practice", agent.office.practice},
			{"area", agent.office.area}, {"answersTo", agent.office.answers_to}}},
		{"modelProviderId", agent.model_provider_id},
		{"modelId", agent.model_id},
		{"memoryProviderId", agent.memory_provider_id},
		{"memoryModelId", agent.memory_model_id},
		{"memoryOff", agent.is_memory_off},
		{"status", to_string(agent.status)},
		{"responsibleActorId", agent.responsible_actor_id},
		{"responsibleName", agent.responsible_name},
		{"createdAt", agent.created_at},
	};
}

// The firm's colleague, as its computer holds it, written into the firm's
// record when what every computer should hold of it changed - never for its
// runs or its work in hand, so the exchange carries nothing then.
void publish_colleague(const Agent &agent) {
	// A copy speaks for no one: the computer that runs the colleague writes it.
	if (agent.matter_id.empty() == false || agent.status == AgentStatus::Draft || agent.is_firm_copy == true) return;
	const std::optional<std::filesystem::path> firm = firm_record();
	if (firm.has_value() == false) return;
	const std::filesystem::path path = *firm / ".lm" / "Agents" / agent.id / "Colleague.json";
	Json definition = colleague_definition(agent);
	if (const std::optional<Json> kept = read_json(path)) {
		Json was = *kept;
		was.erase("definedAt");
		was.erase("writtenBy");
		if (was == definition) return;
	}
	const SharedPlaces &places = installed_places();
	definition["definedAt"] = agent_timestamp_now();
	definition["writtenBy"] = places.device_id ? places.device_id() : std::string();
	write_json(path, definition);
	if (places.written) places.written();
}

} // namespace

void forget_agent(const std::string &agent_id, bool tell_firm) {
	const std::filesystem::path folder = agent_folder(agent_id);
	if (tell_firm == true) {
		try {
			const std::optional<std::filesystem::path> firm = firm_record();
			const std::optional<Agent> agent = load_agent(agent_id);
			if (firm.has_value() == true && agent.has_value() == true && agent->matter_id.empty() == true) {
				Json gone = colleague_definition(*agent);
				gone["status"] = "deleted";
				const SharedPlaces &places = installed_places();
				gone["definedAt"] = agent_timestamp_now();
				gone["writtenBy"] = places.device_id ? places.device_id() : std::string();
				write_json(*firm / ".lm" / "Agents" / agent_id / "Colleague.json", gone);
				if (places.written) places.written();
			}
		} catch (const std::exception &) {
			// Without the firm's record here, its copies stay until the record comes.
		}
	}
	std::error_code error;
	std::filesystem::remove_all(folder, error);
}

void publish_firm_colleagues(const std::vector<Agent> &agents) {
	for (const Agent &agent : agents) {
		try {
			publish_colleague(agent);
		} catch (const std::exception &) {
		}
	}
}

std::vector<FirmColleague> firm_colleagues() {
	std::vector<FirmColleague> found;
	const std::optional<std::filesystem::path> firm = firm_record();
	if (firm.has_value() == false) return found;
	std::error_code error;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(*firm / ".lm" / "Agents", error)) {
		if (entry.is_directory(error) == false) continue;
		const std::optional<Json> value = read_json(entry.path() / "Colleague.json");
		if (value.has_value() == false || value->is_object() == false) continue;
		FirmColleague colleague;
		colleague.agent_id = text_of(*value, "agentId");
		// A record speaks only for the colleague its folder names.
		if (colleague.agent_id.empty() == true || colleague.agent_id != entry.path().filename().string()) continue;
		colleague.name = text_of(*value, "name");
		if (colleague.name.empty() == true) continue;
		colleague.character = text_of(*value, "character");
		colleague.goal = text_of(*value, "goal");
		colleague.manner = text_of(*value, "manner");
		const auto office = value->find("office");
		if (office != value->end() && office->is_object() == true) {
			colleague.office.title = text_of(*office, "title");
			colleague.office.ranks_with = text_of(*office, "ranksWith");
			colleague.office.practice = text_of(*office, "practice");
			colleague.office.area = text_of(*office, "area");
			// No member it answers to is read: one kept before is none now.
		}
		colleague.model_provider_id = text_of(*value, "modelProviderId");
		colleague.model_id = text_of(*value, "modelId");
		colleague.memory_provider_id = text_of(*value, "memoryProviderId");
		colleague.memory_model_id = text_of(*value, "memoryModelId");
		colleague.is_memory_off = value->value("memoryOff", false) == true;
		colleague.status = text_of(*value, "status");
		colleague.responsible_actor_id = text_of(*value, "responsibleActorId");
		colleague.responsible_name = text_of(*value, "responsibleName");
		colleague.created_at = text_of(*value, "createdAt");
		colleague.defined_at = text_of(*value, "definedAt");
		colleague.written_by = text_of(*value, "writtenBy");
		found.push_back(std::move(colleague));
	}
	return found;
}

namespace {

Json colleague_edit_json(const ColleagueEdit &edit) {
	return Json{
		{"agentId", edit.agent_id},
		{"name", edit.name},
		{"character", edit.character},
		{"goal", edit.goal},
		{"manner", edit.manner},
		{"office", {{"title", edit.office.title}, {"ranksWith", edit.office.ranks_with}, {"practice", edit.office.practice},
			{"area", edit.office.area}, {"answersTo", edit.office.answers_to}}},
		{"seatId", edit.seat_id},
		{"at", edit.at},
		{"writtenBy", edit.written_by},
	};
}

// A device id as a file's name: letters, digits, '-' and '_' only.
bool is_plain_name(const std::string &name) {
	return name.empty() == false && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
	});
}

} // namespace

std::string colleague_edit_text(const ColleagueEdit &edit) {
	// An object's keys are in one order (nlohmann::json keeps them sorted), so
	// every computer writes the same words for the same edit.
	return "singularity-colleague-edit/1\n" + colleague_edit_json(edit).dump();
}

void write_colleague_edit(const ColleagueEdit &edit) {
	const std::optional<std::filesystem::path> firm = firm_record();
	if (firm.has_value() == false) throw std::runtime_error("This computer holds no firm record yet.");
	if (is_plain_name(edit.written_by) == false || edit.agent_id.rfind("agent_", 0) != 0 || is_plain_name(edit.agent_id) == false)
		throw std::runtime_error("This change can't be written.");
	Json value = colleague_edit_json(edit);
	value["signature"] = edit.signature;
	write_json(*firm / ".lm" / "Agents" / edit.agent_id / "Edits" / (edit.written_by + ".json"), value);
	const SharedPlaces &places = installed_places();
	if (places.written) places.written();
}

std::vector<ColleagueEdit> colleague_edits(const std::string &agent_id) {
	std::vector<ColleagueEdit> found;
	const std::optional<std::filesystem::path> firm = firm_record();
	if (firm.has_value() == false || is_plain_name(agent_id) == false) return found;
	std::error_code error;
	for (const std::filesystem::directory_entry &entry :
			std::filesystem::directory_iterator(*firm / ".lm" / "Agents" / agent_id / "Edits", error)) {
		if (entry.is_regular_file(error) == false || entry.path().extension() != ".json") continue;
		const std::optional<Json> value = read_json(entry.path());
		if (value.has_value() == false || value->is_object() == false) continue;
		ColleagueEdit edit;
		edit.agent_id = text_of(*value, "agentId");
		edit.name = text_of(*value, "name");
		edit.character = text_of(*value, "character");
		edit.goal = text_of(*value, "goal");
		edit.manner = text_of(*value, "manner");
		const auto office = value->find("office");
		if (office != value->end() && office->is_object() == true) {
			edit.office.title = text_of(*office, "title");
			edit.office.ranks_with = text_of(*office, "ranksWith");
			edit.office.practice = text_of(*office, "practice");
			edit.office.area = text_of(*office, "area");
		}
		edit.seat_id = text_of(*value, "seatId");
		edit.at = text_of(*value, "at");
		edit.written_by = text_of(*value, "writtenBy");
		edit.signature = text_of(*value, "signature");
		// A file speaks only for the computer it is named for, and the colleague
		// its folder names.
		if (edit.agent_id != agent_id || edit.written_by != entry.path().stem().string() || edit.at.empty() == true) continue;
		found.push_back(std::move(edit));
	}
	return found;
}

std::map<std::string, std::string> taken_colleague_edits(const std::string &agent_id) {
	std::map<std::string, std::string> taken;
	const std::optional<Json> value = read_json(agent_folder(agent_id) / "edits-taken.json");
	if (value.has_value() == false || value->is_object() == false) return taken;
	for (const auto &[device, at] : value->items())
		if (at.is_string() == true) taken[device] = at.get<std::string>();
	return taken;
}

void save_taken_colleague_edits(const std::string &agent_id, const std::map<std::string, std::string> &taken) {
	write_json(agent_folder(agent_id) / "edits-taken.json", Json(taken));
}

const SharedPlaces &shared_places() {
	return installed_places();
}

std::vector<MemoryNote> load_memory_notes(const std::string &agent_id) {
	std::vector<MemoryNote> notes;
	std::set<std::string> seen;
	for (const std::filesystem::path &folder : memory_folders(agent_id)) {
		std::error_code error;
		if (std::filesystem::is_directory(folder, error) == false) continue;
		for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(folder, error)) {
			if (entry.path().extension() != ".json") continue;
			for (MemoryNote &note : notes_in(entry.path())) {
				if (seen.insert(note.id).second == true) notes.push_back(std::move(note));
			}
		}
	}
	std::stable_sort(notes.begin(), notes.end(), [](const MemoryNote &first, const MemoryNote &second) {
		return first.at < second.at;
	});
	return notes;
}

std::vector<MemoryNote> load_own_memory_notes(const std::string &agent_id, const std::string &device_id) {
	std::vector<MemoryNote> notes;
	std::set<std::string> seen;
	for (const std::filesystem::path &folder : memory_folders(agent_id)) {
		for (MemoryNote &note : notes_in(folder / memory_file_name(device_id))) {
			if (seen.insert(note.id).second == true) notes.push_back(std::move(note));
		}
	}
	std::stable_sort(notes.begin(), notes.end(), [](const MemoryNote &first, const MemoryNote &second) {
		return first.at < second.at;
	});
	return notes;
}

void save_own_memory_notes(const std::string &agent_id, const std::string &device_id, const std::vector<MemoryNote> &notes) {
	const std::string name = memory_file_name(device_id);
	// This computer's file in every place it wrote one before is written again,
	// so a note that left it, or moved, leaves it there too.
	std::map<std::filesystem::path, Json> files;
	for (const std::filesystem::path &folder : memory_folders(agent_id)) {
		std::error_code error;
		if (std::filesystem::is_regular_file(folder / name, error) == true) files[folder] = Json::array();
	}
	for (const MemoryNote &note : notes) {
		Json &listed = files[memory_folder_for(agent_id, note)];
		if (listed.is_array() == false) listed = Json::array();
		listed.push_back(memory_note_json(note));
	}
	bool isShared = false;
	for (const auto &[folder, listed] : files) {
		write_json(folder / name, {{"formatVersion", 1}, {"deviceId", device_id}, {"notes", listed}});
		isShared = isShared || folder != local_memory_folder(agent_id);
	}
	if (isShared == true && installed_places().written) installed_places().written();
	platform::events::publish("agent.changed", nlohmann::json{{"agentId", agent_id}});
}

std::string new_run_id() {
	return "run_" + secure_random_hex(12);
}

std::string new_assignment_id() {
	return "work_" + secure_random_hex(12);
}

std::string agent_timestamp_now() {
	const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buffer;
}

Json agent_to_json(const Agent &agent) {
	Json value = {
		{"formatVersion", agent.format_version},
		{"id", agent.id},
		{"idNonce", agent.id_nonce},
		{"matterId", agent.matter_id},
		{"kind", agent.kind},
		{"mode", to_string(agent.mode)},
		{"name", agent.name},
		{"character", agent.character},
		{"goal", agent.goal},
		{"manner", agent.manner},
		{"sourceRoot", agent.source_root},
		{"trigger", to_string(agent.trigger)},
		{"dailyMinute", agent.daily_minute},
		{"weeklyDay", agent.weekly_day},
		{"modelPolicy", to_string(agent.model_policy)},
		{"modelProviderId", agent.model_provider_id},
		{"modelId", agent.model_id},
		{"memoryProviderId", agent.memory_provider_id},
		{"memoryModelId", agent.memory_model_id},
		{"memoryOff", agent.is_memory_off},
		{"status", to_string(agent.status)},
		{"firmCopy", agent.is_firm_copy},
		{"revision", agent.revision},
		{"createdAt", agent.created_at},
		{"updatedAt", agent.updated_at},
		{"approvedByActorId", agent.approved_by_actor_id},
		{"approvedAt", agent.approved_at},
		{"responsibleActorId", agent.responsible_actor_id},
		{"responsibleName", agent.responsible_name},
		{"followsAgentId", agent.follows_agent_id},
		{"playbookId", agent.playbook_id},
		{"reviewTableId", agent.review_table_id},
		{"waitingWork", agent.waiting_work},
		{"workGiven", agent.work_given},
		{"mostFiles", agent.most_files},
		{"mostEvidenceCharacters", agent.most_evidence_characters},
		{"failedInARow", agent.failed_in_a_row},
		{"pausedReason", agent.paused_reason},
		{"automaticRunsDay", agent.automatic_runs_day},
		{"automaticRuns", agent.automatic_runs},
		{"spendMonth", agent.spend_month},
		{"spend", agent.spend},
		{"spendCurrency", agent.spend_currency},
	};
	if (agent.requested_by.has_value() == true) value["requestedBy"] = requester_to_json(*agent.requested_by);
	else value["requestedBy"] = nullptr;
	value["office"] = Json{{"title", agent.office.title}, {"ranksWith", agent.office.ranks_with},
		{"practice", agent.office.practice}, {"area", agent.office.area}, {"answersTo", agent.office.answers_to}};
	put_optional(value, "chatId", agent.chat_id);
	put_optional(value, "lastRunId", agent.last_run_id);
	put_optional(value, "lastCheckAt", agent.last_check_at);
	put_optional(value, "lastAttemptAt", agent.last_attempt_at);
	put_optional(value, "nextCheckAt", agent.next_check_at);
	put_optional(value, "changeNoticedAt", agent.change_noticed_at);
	return value;
}

Agent agent_from_json(const Json &value) {
	if (value.is_object() == false) throw std::runtime_error("An agent record is not an object.");
	Agent agent;
	agent.format_version = number_of<int>(value, "formatVersion", 1);
	if (agent.format_version > kAgentFormatVersion) throw AgentDataTooNew();
	agent.id = text_of(value, "id");
	if (is_safe_identifier(agent.id) == false) throw std::runtime_error("An agent record has no valid id.");
	agent.id_nonce = text_of(value, "idNonce");
	agent.matter_id = text_of(value, "matterId");
	agent.kind = text_of(value, "kind");
	if (agent.kind.empty() == true) agent.kind = kAgentKindDataRoomWatch;
	// A mode this build does not know is never run as something else: the
	// agent reads as paused (below) and keeps its record.
	const std::optional<AgentMode> mode = agent_mode_from_string(text_of(value, "mode").empty() == true ? "changes" : text_of(value, "mode"));
	agent.mode = mode.value_or(AgentMode::Changes);
	agent.name = text_of(value, "name");
	agent.character = text_of(value, "character");
	agent.goal = text_of(value, "goal");
	agent.manner = text_of(value, "manner");
	// Its office; an agent kept before offices has none.
	if (value.contains("office") == true && value.at("office").is_object() == true) {
		const Json &office = value.at("office");
		agent.office.title = text_of(office, "title");
		agent.office.ranks_with = text_of(office, "ranksWith");
		agent.office.practice = text_of(office, "practice");
		agent.office.area = text_of(office, "area");
	}
	agent.source_root = text_of(value, "sourceRoot");
	agent.trigger = agent_trigger_from_string(text_of(value, "trigger")).value_or(AgentTrigger::Manual);
	agent.daily_minute = std::clamp(number_of<int>(value, "dailyMinute", 9 * 60), 0, 24 * 60 - 1);
	agent.weekly_day = std::clamp(number_of<int>(value, "weeklyDay", 1), 0, 6);
	agent.chat_id = optional_text_of(value, "chatId");
	agent.model_policy = agent_model_policy_from_string(text_of(value, "modelPolicy")).value_or(AgentModelPolicy::MatterSetup);
	agent.model_provider_id = text_of(value, "modelProviderId");
	agent.model_id = text_of(value, "modelId");
	// A model is a provider's and its id, or the firm's default: never half.
	if (agent.model_provider_id.empty() == true || agent.model_id.empty() == true) {
		agent.model_provider_id.clear();
		agent.model_id.clear();
	}
	agent.memory_provider_id = text_of(value, "memoryProviderId");
	agent.memory_model_id = text_of(value, "memoryModelId");
	if (agent.memory_provider_id.empty() == true || agent.memory_model_id.empty() == true) {
		agent.memory_provider_id.clear();
		agent.memory_model_id.clear();
	}
	agent.is_memory_off = value.contains("memoryOff") == true && value.at("memoryOff").is_boolean() == true
		&& value.at("memoryOff").get<bool>() == true;
	// A status this build does not know is never run: it reads as paused.
	agent.status = agent_status_from_string(text_of(value, "status")).value_or(AgentStatus::Paused);
	if (mode.has_value() == false && agent.status == AgentStatus::Active) agent.status = AgentStatus::Paused;
	agent.is_firm_copy = value.contains("firmCopy") == true && value.at("firmCopy").is_boolean() == true
		&& value.at("firmCopy").get<bool>() == true;
	agent.revision = number_of<long long>(value, "revision", 1);
	agent.created_at = text_of(value, "createdAt");
	agent.updated_at = text_of(value, "updatedAt");
	agent.approved_by_actor_id = text_of(value, "approvedByActorId");
	agent.approved_at = text_of(value, "approvedAt");
	agent.responsible_actor_id = text_of(value, "responsibleActorId");
	if (agent.responsible_actor_id.empty() == true) agent.responsible_actor_id = agent.approved_by_actor_id;
	agent.responsible_name = text_of(value, "responsibleName");
	agent.follows_agent_id = text_of(value, "followsAgentId");
	agent.playbook_id = text_of(value, "playbookId");
	agent.review_table_id = text_of(value, "reviewTableId");
	for (const std::string &id : texts_of(value, "waitingWork"))
		if (is_safe_identifier(id) == true) agent.waiting_work.push_back(id);
	agent.work_given = number_of<int>(value, "workGiven", 0);
	agent.most_files = number_of<std::size_t>(value, "mostFiles", 5000);
	agent.most_evidence_characters = number_of<std::size_t>(value, "mostEvidenceCharacters", 48000);
	agent.failed_in_a_row = number_of<int>(value, "failedInARow", 0);
	agent.paused_reason = text_of(value, "pausedReason");
	agent.automatic_runs_day = text_of(value, "automaticRunsDay");
	agent.automatic_runs = number_of<int>(value, "automaticRuns", 0);
	agent.spend_month = text_of(value, "spendMonth");
	agent.spend = number_of<double>(value, "spend", 0.0);
	agent.spend_currency = text_of(value, "spendCurrency");
	const auto requested = value.find("requestedBy");
	if (requested != value.end() && requested->is_object() == true) agent.requested_by = requester_from_json(*requested);
	agent.last_run_id = optional_text_of(value, "lastRunId");
	agent.last_check_at = optional_text_of(value, "lastCheckAt");
	agent.last_attempt_at = optional_text_of(value, "lastAttemptAt");
	agent.next_check_at = optional_text_of(value, "nextCheckAt");
	agent.change_noticed_at = optional_text_of(value, "changeNoticedAt");
	return agent;
}

Json run_to_json(const AgentRun &run) {
	Json documents = Json::array();
	for (const RunDocument &document : run.documents) {
		Json item = {
			{"relativePath", document.relative_path},
			{"kind", document.kind},
			{"previousPath", document.previous_path},
			{"previousSha256", document.previous_sha256},
			{"currentSha256", document.current_sha256},
			{"review", document.review},
			{"note", document.note},
			{"evidenceIds", document.evidence_ids},
		};
		put_optional(item, "documentId", document.document_id);
		documents.push_back(item);
	}
	Json evidence = Json::array();
	for (const RunEvidence &item : run.evidence) {
		evidence.push_back({
			{"id", item.id},
			{"relativePath", item.relative_path},
			{"kind", item.kind},
			{"context", quotes_to_json(item.context)},
			{"before", quotes_to_json(item.before)},
			{"after", quotes_to_json(item.after)},
			{"omittedBefore", item.omitted_before},
			{"omittedAfter", item.omitted_after},
		});
	}
	Json findings = Json::array();
	for (const RunFinding &finding : run.findings) {
		findings.push_back({
			{"id", finding.id},
			{"title", finding.title},
			{"observation", finding.observation},
			{"analysis", finding.analysis},
			{"uncertainty", finding.uncertainty},
			{"needsReview", finding.needs_review},
			{"evidenceIds", finding.evidence_ids},
		});
	}
	Json analysis = {
		{"status", run.analysis.status},
		{"reason", run.analysis.reason},
		{"providerId", run.analysis.provider_id},
		{"modelId", run.analysis.model_id},
		{"modelLabel", run.analysis.model_label},
		{"destination", run.analysis.destination},
		{"protectionMode", run.analysis.protection_mode},
		{"evidenceSent", run.analysis.evidence_sent},
		{"findingsDropped", run.analysis.findings_dropped},
		{"inputTokens", run.analysis.input_tokens},
		{"outputTokens", run.analysis.output_tokens},
		{"currency", run.analysis.currency},
	};
	put_optional(analysis, "operationId", run.analysis.operation_id);
	if (run.analysis.cost.has_value() == true) analysis["cost"] = *run.analysis.cost;
	else analysis["cost"] = nullptr;
	Json value = {
		{"id", run.id},
		{"agentId", run.agent_id},
		{"matterId", run.matter_id},
		{"trigger", run.trigger},
		{"assignmentId", run.assignment_id},
		{"status", run.status},
		{"stage", run.stage},
		{"answeringChatId", run.answering_chat_id},
		{"progressDone", run.progress_done},
		{"progressTotal", run.progress_total},
		{"startedAt", run.started_at},
		{"finishedAt", run.finished_at},
		{"isBaseline", run.is_baseline},
		{"counts", {
			{"files", run.counts.files},
			{"added", run.counts.added},
			{"changed", run.counts.changed},
			{"removed", run.counts.removed},
			{"moved", run.counts.moved},
			{"unreadable", run.counts.unreadable},
			{"unreviewed", run.counts.unreviewed},
		}},
		{"documents", documents},
		{"evidence", evidence},
		{"findings", findings},
		{"analysis", analysis},
		{"errorCode", run.error_code},
		{"errorMessage", run.error_message},
	};
	if (run.task.has_value() == true) {
		value["task"] = {
			{"chatId", run.task->chat_id},
			{"taskId", run.task->task_id},
			{"operationId", run.task->operation_id},
			{"state", run.task->state},
			{"answerExcerpt", run.task->answer_excerpt},
			{"message", run.task->message},
			{"cost", run.task->cost.has_value() == true ? Json(*run.task->cost) : Json(nullptr)},
			{"currency", run.task->currency},
		};
	} else {
		value["task"] = nullptr;
	}
	put_optional(value, "workProductId", run.work_product_id);
	put_optional(value, "reviewedAt", run.reviewed_at);
	put_optional(value, "reviewedByActorId", run.reviewed_by_actor_id);
	return value;
}

AgentRun run_from_json(const Json &value) {
	if (value.is_object() == false) throw std::runtime_error("A run record is not an object.");
	AgentRun run;
	run.id = text_of(value, "id");
	run.agent_id = text_of(value, "agentId");
	run.matter_id = text_of(value, "matterId");
	run.trigger = text_of(value, "trigger");
	run.assignment_id = text_of(value, "assignmentId");
	run.status = text_of(value, "status");
	run.stage = text_of(value, "stage");
	run.answering_chat_id = text_of(value, "answeringChatId");
	run.progress_done = number_of<std::size_t>(value, "progressDone", 0);
	run.progress_total = number_of<std::size_t>(value, "progressTotal", 0);
	run.started_at = text_of(value, "startedAt");
	run.finished_at = text_of(value, "finishedAt");
	run.is_baseline = flag_of(value, "isBaseline", false);
	const auto counts = value.find("counts");
	if (counts != value.end() && counts->is_object() == true) {
		run.counts.files = number_of<std::size_t>(*counts, "files", 0);
		run.counts.added = number_of<std::size_t>(*counts, "added", 0);
		run.counts.changed = number_of<std::size_t>(*counts, "changed", 0);
		run.counts.removed = number_of<std::size_t>(*counts, "removed", 0);
		run.counts.moved = number_of<std::size_t>(*counts, "moved", 0);
		run.counts.unreadable = number_of<std::size_t>(*counts, "unreadable", 0);
		run.counts.unreviewed = number_of<std::size_t>(*counts, "unreviewed", 0);
	}
	const auto documents = value.find("documents");
	if (documents != value.end() && documents->is_array() == true) {
		for (const Json &item : *documents) {
			if (item.is_object() == false) continue;
			RunDocument document;
			document.relative_path = text_of(item, "relativePath");
			document.kind = text_of(item, "kind");
			document.previous_path = text_of(item, "previousPath");
			document.previous_sha256 = text_of(item, "previousSha256");
			document.current_sha256 = text_of(item, "currentSha256");
			document.document_id = optional_text_of(item, "documentId");
			document.review = text_of(item, "review");
			document.note = text_of(item, "note");
			document.evidence_ids = texts_of(item, "evidenceIds");
			run.documents.push_back(document);
		}
	}
	const auto evidence = value.find("evidence");
	if (evidence != value.end() && evidence->is_array() == true) {
		for (const Json &item : *evidence) {
			if (item.is_object() == false) continue;
			RunEvidence entry;
			entry.id = text_of(item, "id");
			entry.relative_path = text_of(item, "relativePath");
			entry.kind = text_of(item, "kind");
			entry.context = quotes_from_json(item, "context");
			entry.before = quotes_from_json(item, "before");
			entry.after = quotes_from_json(item, "after");
			entry.omitted_before = number_of<std::size_t>(item, "omittedBefore", 0);
			entry.omitted_after = number_of<std::size_t>(item, "omittedAfter", 0);
			run.evidence.push_back(entry);
		}
	}
	const auto findings = value.find("findings");
	if (findings != value.end() && findings->is_array() == true) {
		for (const Json &item : *findings) {
			if (item.is_object() == false) continue;
			RunFinding finding;
			finding.id = text_of(item, "id");
			finding.title = text_of(item, "title");
			finding.observation = text_of(item, "observation");
			finding.analysis = text_of(item, "analysis");
			finding.uncertainty = text_of(item, "uncertainty");
			finding.needs_review = flag_of(item, "needsReview", true);
			finding.evidence_ids = texts_of(item, "evidenceIds");
			run.findings.push_back(finding);
		}
	}
	const auto analysis = value.find("analysis");
	if (analysis != value.end() && analysis->is_object() == true) {
		run.analysis.status = text_of(*analysis, "status");
		run.analysis.reason = text_of(*analysis, "reason");
		run.analysis.provider_id = text_of(*analysis, "providerId");
		run.analysis.model_id = text_of(*analysis, "modelId");
		run.analysis.model_label = text_of(*analysis, "modelLabel");
		run.analysis.destination = text_of(*analysis, "destination");
		run.analysis.protection_mode = text_of(*analysis, "protectionMode");
		run.analysis.evidence_sent = number_of<std::size_t>(*analysis, "evidenceSent", 0);
		run.analysis.findings_dropped = number_of<std::size_t>(*analysis, "findingsDropped", 0);
		run.analysis.operation_id = optional_text_of(*analysis, "operationId");
		run.analysis.input_tokens = number_of<long long>(*analysis, "inputTokens", 0);
		run.analysis.output_tokens = number_of<long long>(*analysis, "outputTokens", 0);
		run.analysis.currency = text_of(*analysis, "currency");
		const auto cost = analysis->find("cost");
		if (cost != analysis->end() && cost->is_number() == true) run.analysis.cost = cost->get<double>();
	}
	const auto task = value.find("task");
	if (task != value.end() && task->is_object() == true) {
		RunTask entry;
		entry.chat_id = text_of(*task, "chatId");
		entry.task_id = text_of(*task, "taskId");
		entry.operation_id = text_of(*task, "operationId");
		entry.state = text_of(*task, "state");
		entry.answer_excerpt = text_of(*task, "answerExcerpt");
		entry.message = text_of(*task, "message");
		const auto cost = task->find("cost");
		if (cost != task->end() && cost->is_number() == true) entry.cost = cost->get<double>();
		entry.currency = text_of(*task, "currency");
		run.task = entry;
	}
	run.work_product_id = optional_text_of(value, "workProductId");
	run.reviewed_at = optional_text_of(value, "reviewedAt");
	run.reviewed_by_actor_id = optional_text_of(value, "reviewedByActorId");
	run.error_code = text_of(value, "errorCode");
	run.error_message = text_of(value, "errorMessage");
	return run;
}

Json assignment_to_json(const AgentAssignment &assignment) {
	return {
		{"formatVersion", assignment.format_version},
		{"id", assignment.id},
		{"agentId", assignment.agent_id},
		{"matterId", assignment.matter_id},
		{"number", assignment.number},
		{"chatId", assignment.chat_id},
		{"replyToMessageId", assignment.reply_to_message_id},
		{"instruction", assignment.instruction},
		{"givenByActorId", assignment.given_by_actor_id},
		{"givenByName", assignment.given_by_name},
		{"givenAt", assignment.given_at},
		{"fromSharedFolder", assignment.from_shared_folder},
		{"state", assignment.state},
		{"runId", assignment.run_id},
		{"startedAt", assignment.started_at},
		{"finishedAt", assignment.finished_at},
		{"attempts", assignment.attempts},
		{"message", assignment.message},
		{"foldedMessageIds", assignment.folded_message_ids},
	};
}

AgentAssignment assignment_from_json(const Json &value) {
	if (value.is_object() == false) throw std::runtime_error("A record of work given is not an object.");
	AgentAssignment assignment;
	assignment.format_version = number_of<int>(value, "formatVersion", 1);
	assignment.id = text_of(value, "id");
	if (is_safe_identifier(assignment.id) == false) throw std::runtime_error("A record of work given has no valid id.");
	assignment.agent_id = text_of(value, "agentId");
	assignment.matter_id = text_of(value, "matterId");
	assignment.number = number_of<int>(value, "number", 0);
	assignment.chat_id = text_of(value, "chatId");
	assignment.reply_to_message_id = text_of(value, "replyToMessageId");
	if (value.contains("foldedMessageIds") && value.at("foldedMessageIds").is_array())
		for (const Json &id : value.at("foldedMessageIds"))
			if (id.is_string()) assignment.folded_message_ids.push_back(id.get<std::string>());
	assignment.instruction = text_of(value, "instruction");
	assignment.given_by_actor_id = text_of(value, "givenByActorId");
	assignment.given_by_name = text_of(value, "givenByName");
	assignment.given_at = text_of(value, "givenAt");
	assignment.from_shared_folder = value.contains("fromSharedFolder") && value.at("fromSharedFolder").is_boolean()
		&& value.at("fromSharedFolder").get<bool>();
	assignment.state = text_of(value, "state");
	// A state this build does not know is never carried out again.
	static const std::set<std::string> states = {"waiting", "working", "done", "failed", "stopped", "withdrawn"};
	if (states.count(assignment.state) == 0) assignment.state = "failed";
	assignment.run_id = text_of(value, "runId");
	assignment.started_at = text_of(value, "startedAt");
	assignment.finished_at = text_of(value, "finishedAt");
	assignment.attempts = number_of<int>(value, "attempts", 0);
	assignment.message = text_of(value, "message");
	return assignment;
}

std::vector<Agent> load_agents(std::vector<std::string> *unreadable) {
	std::vector<Agent> agents;
	std::error_code error;
	const std::filesystem::path directory = agents_directory();
	if (std::filesystem::is_directory(directory, error) == false) return agents;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(directory, error)) {
		const std::string id = entry.path().filename().string();
		if (is_safe_identifier(id) == false || entry.is_directory(error) == false) continue;
		const std::optional<Json> value = read_json(entry.path() / "agent.json");
		if (value.has_value() == false) {
			if (unreadable != nullptr) unreadable->push_back(id);
			continue;
		}
		try {
			Agent agent = agent_from_json(*value);
			if (agent.id != id) throw std::runtime_error("An agent record names another agent.");
			agents.push_back(agent);
		} catch (const std::exception &) {
			if (unreadable != nullptr) unreadable->push_back(id);
		}
	}
	std::sort(agents.begin(), agents.end(), [](const Agent &left, const Agent &right) {
		if (left.created_at != right.created_at) return left.created_at < right.created_at;
		return left.id < right.id;
	});
	return agents;
}

std::optional<Agent> load_agent(const std::string &agent_id) {
	if (is_safe_identifier(agent_id) == false) return std::nullopt;
	const std::optional<Json> value = read_json(agent_folder(agent_id) / "agent.json");
	if (value.has_value() == false) return std::nullopt;
	Agent agent = agent_from_json(*value);
	if (agent.id != agent_id) return std::nullopt;
	return agent;
}

void save_agent(const Agent &agent) {
	if (agent.format_version > kAgentFormatVersion) throw AgentDataTooNew();
	write_json(agent_folder(agent.id) / "agent.json", agent_to_json(agent));
	// What every computer of the firm holds of it; a record that cannot be
	// written now is written with its next change.
	try {
		publish_colleague(agent);
	} catch (const std::exception &) {
	}
	// Whoever shows the colleague reads it again.
	platform::events::publish("agent.changed", nlohmann::json{{"agentId", agent.id}, {"matterId", agent.matter_id}});
}

std::vector<AgentRun> load_runs(const std::string &agent_id, std::size_t most) {
	std::vector<AgentRun> runs;
	std::error_code error;
	const std::filesystem::path directory = agent_folder(agent_id) / "runs";
	if (std::filesystem::is_directory(directory, error) == false) return runs;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(directory, error)) {
		if (entry.path().extension() != ".json") continue;
		const std::optional<Json> value = read_json(entry.path());
		if (value.has_value() == false) continue;
		try {
			runs.push_back(run_from_json(*value));
		} catch (const std::exception &) {
		}
	}
	std::sort(runs.begin(), runs.end(), [](const AgentRun &left, const AgentRun &right) {
		if (left.started_at != right.started_at) return left.started_at > right.started_at;
		return left.id > right.id;
	});
	if (runs.size() > most) runs.resize(most);
	return runs;
}

std::optional<AgentRun> load_run(const std::string &agent_id, const std::string &run_id) {
	if (is_safe_identifier(run_id) == false) return std::nullopt;
	const std::optional<Json> value = read_json(agent_folder(agent_id) / "runs" / (run_id + ".json"));
	if (value.has_value() == false) return std::nullopt;
	AgentRun run = run_from_json(*value);
	if (run.id != run_id || run.agent_id != agent_id) return std::nullopt;
	return run;
}

void save_run(const AgentRun &run) {
	if (is_safe_identifier(run.id) == false) throw AgentRefused("A run needs a valid id.");
	write_json(agent_folder(run.agent_id) / "runs" / (run.id + ".json"), run_to_json(run));
	platform::events::publish("agent.changed", nlohmann::json{{"agentId", run.agent_id}, {"matterId", run.matter_id}});
}

std::vector<AgentAssignment> load_assignments(const std::string &agent_id) {
	std::vector<AgentAssignment> assignments;
	std::error_code error;
	const std::filesystem::path directory = agent_folder(agent_id) / "assignments";
	if (std::filesystem::is_directory(directory, error) == false) return assignments;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(directory, error)) {
		if (entry.path().extension() != ".json") continue;
		const std::optional<Json> value = read_json(entry.path());
		if (value.has_value() == false) continue;
		try {
			AgentAssignment assignment = assignment_from_json(*value);
			if (assignment.agent_id == agent_id && entry.path().stem().string() == assignment.id) assignments.push_back(assignment);
		} catch (const std::exception &) {
		}
	}
	std::sort(assignments.begin(), assignments.end(), [](const AgentAssignment &left, const AgentAssignment &right) {
		if (left.number != right.number) return left.number < right.number;
		if (left.given_at != right.given_at) return left.given_at < right.given_at;
		return left.id < right.id;
	});
	return assignments;
}

std::optional<AgentAssignment> load_assignment(const std::string &agent_id, const std::string &assignment_id) {
	if (is_safe_identifier(assignment_id) == false) return std::nullopt;
	const std::optional<Json> value = read_json(agent_folder(agent_id) / "assignments" / (assignment_id + ".json"));
	if (value.has_value() == false) return std::nullopt;
	AgentAssignment assignment = assignment_from_json(*value);
	if (assignment.id != assignment_id || assignment.agent_id != agent_id) return std::nullopt;
	return assignment;
}

void save_assignment(const AgentAssignment &assignment) {
	if (is_safe_identifier(assignment.id) == false) throw AgentRefused("Work given to an agent needs a valid id.");
	write_json(agent_folder(assignment.agent_id) / "assignments" / (assignment.id + ".json"), assignment_to_json(assignment));
	platform::events::publish("agent.changed", nlohmann::json{{"agentId", assignment.agent_id}, {"matterId", assignment.matter_id}});
}

DataRoomInventory load_inventory(const std::string &agent_id) {
	DataRoomInventory inventory;
	const std::optional<Json> value = read_json(agent_folder(agent_id) / "inventory.json");
	if (value.has_value() == false || value->is_object() == false) return inventory;
	const auto files = value->find("files");
	if (files == value->end() || files->is_array() == false) return inventory;
	for (const Json &item : *files) {
		if (item.is_object() == false) continue;
		FileFingerprint fingerprint;
		fingerprint.relative_path = text_of(item, "path");
		fingerprint.size = number_of<std::uintmax_t>(item, "size", 0);
		fingerprint.modified_ns = number_of<long long>(item, "modifiedNs", 0);
		fingerprint.file_id = number_of<unsigned long long>(item, "fileId", 0);
		fingerprint.sha256 = text_of(item, "sha256");
		fingerprint.read_error = text_of(item, "readError");
		if (fingerprint.relative_path.empty() == false) inventory.files.push_back(fingerprint);
	}
	return inventory;
}

void save_inventory(const std::string &agent_id, const DataRoomInventory &inventory) {
	Json files = Json::array();
	for (const FileFingerprint &fingerprint : inventory.files) {
		files.push_back({
			{"path", fingerprint.relative_path},
			{"size", fingerprint.size},
			{"modifiedNs", fingerprint.modified_ns},
			{"fileId", fingerprint.file_id},
			{"sha256", fingerprint.sha256},
			{"readError", fingerprint.read_error},
		});
	}
	write_json(agent_folder(agent_id) / "inventory.json", {{"formatVersion", 1}, {"files", files}});
}

std::optional<std::vector<TextBlock>> load_text(const std::string &agent_id, const std::string &sha256) {
	if (is_hex_digest(sha256) == false) return std::nullopt;
	const std::optional<Json> value = read_json(agent_folder(agent_id) / "texts" / (sha256 + ".json"));
	if (value.has_value() == false || value->is_object() == false) return std::nullopt;
	const auto blocks = value->find("blocks");
	if (blocks == value->end() || blocks->is_array() == false) return std::nullopt;
	std::vector<TextBlock> result;
	for (const Json &item : *blocks) {
		if (item.is_object() == false) continue;
		result.push_back({text_of(item, "text"), text_of(item, "location")});
	}
	return result;
}

void save_text(const std::string &agent_id, const std::string &sha256, const std::vector<TextBlock> &blocks) {
	if (is_hex_digest(sha256) == false) throw AgentRefused("A kept reading needs a content digest.");
	Json items = Json::array();
	for (const TextBlock &block : blocks) items.push_back({{"text", block.text}, {"location", block.location}});
	write_json(agent_folder(agent_id) / "texts" / (sha256 + ".json"), {{"formatVersion", 1}, {"blocks", items}});
}

void prune_texts(const std::string &agent_id, const std::set<std::string> &keep) {
	std::error_code error;
	const std::filesystem::path directory = agent_folder(agent_id) / "texts";
	if (std::filesystem::is_directory(directory, error) == false) return;
	std::vector<std::filesystem::path> unused;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(directory, error)) {
		const std::string digest = entry.path().stem().string();
		if (entry.path().extension() == ".json" && is_hex_digest(digest) == true && keep.count(digest) == 0)
			unused.push_back(entry.path());
	}
	for (const std::filesystem::path &path : unused) std::filesystem::remove(path, error);
}

} // namespace singularity::agents
