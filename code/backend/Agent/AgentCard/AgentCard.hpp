#pragma once

#include "Agent/Types/Agent.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace singularity::agents {

// An agent as its Matter's team sees it on every computer that opens the
// Matter. An agent runs on one computer - the one whose Singularity it was
// approved in - and its record stays there (Agent/AgentStore); what the rest
// of the team needs to know of it is written by that computer into the
// Matter's own store, one file per agent:
//
//   .lm/Agents/<agent id>.json
//
// its name and role, the lawyer answerable for it, its schedule and state,
// and how its last run went. Its reports are the Matter's Work Product and
// its task runs are the Matter's chats, so the team reads those as it reads
// any other. Only the computer that runs an agent writes its card; a draft
// another app proposed is not published until a lawyer approves it.
struct AgentCard {
	int format_version = 1;
	std::string agent_id;
	std::string matter_id;
	std::string name;
	std::string kind;
	std::string mode;
	// The character it is drawn as, the same on every computer.
	std::string character;
	// Its office in the firm, as the computer that runs it holds it: the title
	// ("M&A Associate") and the practice, so every member's window names it so.
	std::string office_title;
	std::string office_practice;
	// The lawyer's instruction, as the team should read it.
	std::string goal;
	std::string trigger;
	int daily_minute = 0;
	int weekly_day = 0;
	std::string follows_agent_id;
	// active, paused or stopped; and the one state that sums it with its last
	// run (AgentActions agent_state).
	std::string status;
	std::string state;
	std::string responsible_actor_id;
	std::string responsible_name;
	// The computer that runs it (Platform/ActorIdentity device id), its device
	// key, the nonce the agent's id was bound with to that key, the card's
	// counter, and that computer's signature over agent_card_signed_text. An
	// older card has no signature and never counts as the agent's host.
	std::string host_device_id;
	std::string host_device_key;
	std::string id_nonce;
	std::int64_t counter = 0;
	std::string signature;
	std::string last_run_at;
	// How its last run went, in a few words, never a document's contents.
	std::string last_run_words;
	bool needs_review = false;
	std::string updated_at;
};

// The card of `agent` as it is now, with its state and latest run.
AgentCard card_of(const Agent &agent, const std::optional<AgentRun> &latest_run, const std::string &state,
	const std::string &host_device_id, const std::string &now);

// Writes the card into the Matter's `.lm` (only when the Matter has one).
// Never throws: a read-only or unreachable share is left as it is.
void publish_agent_card(const std::filesystem::path &matter_folder, const AgentCard &card);
// Every card in the Matter's `.lm`; one that cannot be read is skipped.
std::vector<AgentCard> load_agent_cards(const std::filesystem::path &matter_folder);

} // namespace singularity::agents
