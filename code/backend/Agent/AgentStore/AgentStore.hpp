#pragma once

#include "Agent/DataRoomScan/DataRoomScan.hpp"
#include "Agent/TextDiff/TextDiff.hpp"
#include "Agent/Types/Agent.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace singularity::agents {

// Agents live in this computer's application data, one folder each:
//
//   Agents/<agent id>/agent.json          the agent (rewritten whole, staged)
//   Agents/<agent id>/runs/<run id>.json  one file per run
//   Agents/<agent id>/assignments/<id>.json one file per piece of work given
//   Agents/<agent id>/inventory.json      the room as last compared
//   Agents/<agent id>/texts/<sha256>.json the words of each version read
//
// Every write is staged beside its file and committed by one rename, so a
// crash leaves the earlier file whole. Nothing here is shared with another
// computer or written into a Matter folder.

// An agent's record was written by a newer Singularity: it is shown, never
// changed, so nothing it holds is lost.
class AgentDataTooNew final : public std::runtime_error {
public:
	AgentDataTooNew()
		: std::runtime_error("This agent was saved by a newer version of Singularity. Update Singularity to change it.") {}
};

std::filesystem::path agents_directory();

// One lock for every agent record on this computer: a read-change-write of
// an agent is always under it, never across a model call or a folder scan.
std::mutex &agent_store_mutex();

// Every agent whose record can be read. A record that cannot be read is
// skipped and named in `unreadable` (its folder's id), never deleted.
std::vector<Agent> load_agents(std::vector<std::string> *unreadable = nullptr);
std::optional<Agent> load_agent(const std::string &agent_id);
void save_agent(const Agent &agent);

// Newest first; at most `most`.
std::vector<AgentRun> load_runs(const std::string &agent_id, std::size_t most);
std::optional<AgentRun> load_run(const std::string &agent_id, const std::string &run_id);
void save_run(const AgentRun &run);

// Every piece of work given to the agent, oldest first.
std::vector<AgentAssignment> load_assignments(const std::string &agent_id);
std::optional<AgentAssignment> load_assignment(const std::string &agent_id, const std::string &assignment_id);
void save_assignment(const AgentAssignment &assignment);

DataRoomInventory load_inventory(const std::string &agent_id);
void save_inventory(const std::string &agent_id, const DataRoomInventory &inventory);

std::optional<std::vector<TextBlock>> load_text(const std::string &agent_id, const std::string &sha256);
void save_text(const std::string &agent_id, const std::string &sha256, const std::vector<TextBlock> &blocks);
// Drops every kept reading whose digest is not in `keep`.
void prune_texts(const std::string &agent_id, const std::set<std::string> &keep);

// Where a colleague's records travel to the firm's other computers: the
// firm's own record, and each Matter's own folder. The application installs
// them (Bridge/NativeBridge); with none installed a colleague's records
// stay in its own folder on this computer, as before.
struct SharedPlaces {
	// The firm's record's folder here; none while it cannot be kept.
	std::function<std::optional<std::filesystem::path>()> firm;
	// The folder of the Matter `matter_id` here; none for one with no folder.
	std::function<std::optional<std::filesystem::path>(const std::string &matter_id)> matter;
	// Every Matter's folder here.
	std::function<std::vector<std::filesystem::path>()> matters;
	// Something was written into one of them: the exchange carries it at once.
	std::function<void()> written;
	// This computer, which writes what it holds of a colleague.
	std::function<std::string()> device_id;
};
void install_shared_places(SharedPlaces places);
const SharedPlaces &shared_places();

// What every computer of the firm holds of each of its colleagues, in the
// firm's record (`.lm/Agents/<agent id>/Colleague.json`): who it is, its
// office, how it works and what it does, its model, its status and who
// answers for it - never its runs or the work in its hands. The computer that
// runs a colleague writes it as the colleague's settings change; every other
// computer lists the colleague from it.
struct FirmColleague {
	std::string agent_id;
	std::string name;
	std::string character;
	std::string goal;
	std::string manner;
	Agent::Office office;
	std::string model_provider_id;
	std::string model_id;
	std::string memory_provider_id;
	std::string memory_model_id;
	bool is_memory_off = false;
	std::string status;
	std::string responsible_actor_id;
	std::string responsible_name;
	std::string created_at;
	std::string defined_at;
	std::string written_by;
};
std::vector<FirmColleague> firm_colleagues();
// A colleague the firm let go: its own record here goes, with its runs and its
// work; with `tell_firm`, the firm's record keeps that it is gone (status
// "deleted"), so the copies on the firm's other computers go too. Its words
// stay in the threads where it wrote them.
void forget_agent(const std::string &agent_id, bool tell_firm);
// Writes each of `agents` that is the firm's into the firm's record where what
// it holds of it changed; one unchanged is not written again. Never throws.
void publish_firm_colleagues(const std::vector<Agent> &agents);

// An administrator's change of a colleague made on a computer that does not
// run it, in the firm's record (`.lm/Agents/<agent id>/Edits/
// <device>.json`), each computer writing only its own file. It is signed with
// that computer's device key, since a file in the record does not prove which
// computer wrote it; the computer that runs the colleague takes it only from
// one of the firm's computers it checked, of a seat Legal Machines lists as
// the administrator's.
struct ColleagueEdit {
	std::string agent_id;
	std::string name;
	std::string character;
	std::string goal;
	std::string manner;
	Agent::Office office;
	std::string seat_id;
	std::string at;
	std::string written_by;
	std::string signature;
};
// The words its signature covers: every field but the signature, in one
// fixed order.
std::string colleague_edit_text(const ColleagueEdit &edit);
// Throws std::runtime_error while this computer holds no firm record.
void write_colleague_edit(const ColleagueEdit &edit);
// Each computer's edit of `agent_id` the firm's record holds, as written; none
// without a firm record.
std::vector<ColleagueEdit> colleague_edits(const std::string &agent_id);
// The computer that runs a colleague keeps, for itself only, the time of the
// newest edit it took from each computer, so none is taken twice.
std::map<std::string, std::string> taken_colleague_edits(const std::string &agent_id);
void save_taken_colleague_edits(const std::string &agent_id, const std::map<std::string, std::string> &taken);

// A colleague's notes as every computer wrote them, oldest first, each
// computer writing only its own file, so nothing one computer wrote is ever
// rewritten by another. Where each travels: a Matter's note in that Matter's
// folder (`.lm/Agents/<agent id>/Memory/<device>.json`), so only that Matter's
// computers hold it; the firm's and a member's in the firm's record, at the
// same place; a note no shared place can hold, in Agents/<agent
// id>/memory/<device>.json here.
std::vector<MemoryNote> load_memory_notes(const std::string &agent_id);
std::vector<MemoryNote> load_own_memory_notes(const std::string &agent_id, const std::string &device_id);
void save_own_memory_notes(const std::string &agent_id, const std::string &device_id, const std::vector<MemoryNote> &notes);
std::string new_memory_note_id();

std::string new_agent_id();
std::string new_run_id();
std::string new_assignment_id();
// Hex of `bytes` bytes from the operating system's secure random source: the
// one every identifier of Singularity draws from (Platform/SecureRandom).
using platform::secure_random_hex;
// Now, in UTC, as "2026-10-10T21:04:00Z".
std::string agent_timestamp_now();

nlohmann::json agent_to_json(const Agent &agent);
Agent agent_from_json(const nlohmann::json &value);
nlohmann::json run_to_json(const AgentRun &run);
AgentRun run_from_json(const nlohmann::json &value);
nlohmann::json assignment_to_json(const AgentAssignment &assignment);
AgentAssignment assignment_from_json(const nlohmann::json &value);

} // namespace singularity::agents
