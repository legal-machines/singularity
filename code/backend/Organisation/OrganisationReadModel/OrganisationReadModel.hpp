#pragma once

#include "Organisation/Types/Organisation.hpp"

#include <string>
#include <vector>

// The organisation read, built from plain facts (Beta 1 #273). The facts come
// from the domains that own them (Organisation/OrganisationProduction); this
// part reads no domain and no file, so every rule below is tested on facts
// alone.
namespace singularity::organisation {

struct PersonFacts {
	std::string actor_id;
	std::string name;
	std::string organisation;
	bool is_you = false;
	std::vector<std::string> matter_ids;
	std::string last_seen_at;
};

struct WorkGivenFacts {
	std::string id;
	// waiting | working | done | failed | stopped | withdrawn
	std::string state;
	std::string instruction;
	std::string given_by_actor_id;
	std::string given_at;
	std::string started_at;
	std::string finished_at;
	std::string chat_id;
};

struct RunFacts {
	// running | completed | failed | cancelled | interrupted | access_needed
	std::string status;
	std::string started_at;
	std::string finished_at;
};

struct ColleagueFacts {
	std::string agent_id;
	std::string name;
	std::string role;
	std::string role_name;
	std::string mode;
	std::string character;
	std::string matter_id;
	// The agent's state (agent_state) and status (active, paused, stopped,
	// draft).
	std::string state;
	std::string status;
	bool is_running = false;
	// What its character acts out now (Agent/AgentActions agent_presence).
	std::string presence;
	bool runs_here = true;
	std::string responsible_actor_id;
	std::string folder;
	std::string trigger;
	std::string playbook_id;
	std::string playbook_name;
	std::string instruction;
	// Why it paused itself; empty when a person paused it, or it works.
	std::string paused_reason;
	int work_given = 0;
	std::vector<WorkGivenFacts> work;
	std::vector<RunFacts> runs;
	double spend = 0.0;
	std::string spend_currency;
	std::string spend_month;
	std::string last_run_at;
	std::string updated_at;
};

struct OperationFacts {
	std::string operation_id;
	std::string chat_id;
	// The operation's state, as Chat/ChatExecution writes it: working,
	// preparing, stopping, waiting_for_approval, waiting_for_answer,
	// interrupted (ended states are not read).
	std::string state;
	std::string activity;
	std::string prompt;
	std::string question_title;
	std::string updated_at;
	bool runs_elsewhere = false;
	// Whose work it is: the colleague that runs it, or the person who asked.
	std::string agent_id;
	std::string author_actor_id;
};

struct OverwrittenFacts {
	std::string id;
	std::string document_name;
	std::string found_at;
};

struct ConflictFacts {
	std::string id;
	std::string label;
	std::string collection;
	std::string timestamp;
};

struct MatterFacts {
	std::string id;
	std::string name;
	std::vector<OperationFacts> operations;
	std::vector<OverwrittenFacts> overwritten;
	std::vector<ConflictFacts> conflicts;
};

struct OrganisationFacts {
	Viewer viewer;
	std::vector<PersonFacts> people;
	std::vector<ColleagueFacts> colleagues;
	std::vector<MatterFacts> matters;
	std::string now;
};

// The read the window shows. Members come viewer first, then everyone else by
// name, people and AI colleagues together; decisions oldest first.
OrganisationView build(const OrganisationFacts &facts);

} // namespace singularity::organisation
