#pragma once

#include "Agent/Types/Agent.hpp"

#include <string>
#include <vector>

namespace singularity::agents {

// The roles an AI colleague can be begun in: what it does, the mode it works
// in, its first name and instruction, and when it works by default. This is
// the one catalogue (Beta 1 #272). Every domain that names or checks a role
// reads it here, and the window reads it through agent.roles, so no copy of it
// lives in React.
struct AgentRole {
	// The role's id: an agent's `kind`.
	std::string id;
	AgentMode mode = AgentMode::Changes;
	// What a person chooses ("Watch a data room"), and what it does, in one
	// sentence.
	std::string label;
	std::string description;
	// What it does for its team, as a lawyer would say it of a colleague
	// ("watches the data room").
	std::string duty;
	// The name and the instruction a new colleague in this role starts with:
	// the person can change both. Empty when the person writes their own.
	std::string name;
	std::string goal;
	AgentTrigger trigger = AgentTrigger::OnChange;
	int weekly_day = 1;
	int daily_minute = 9 * 60;
	// It checks against a firm Playbook, which the person chooses.
	bool uses_playbook = false;
};

// Every role, in the order a person is offered them.
const std::vector<AgentRole> &agent_roles();
// The role with this id, or nullptr for an id no role has.
const AgentRole *agent_role(const std::string &id);
// What a colleague in this role does for its team; the standing task's words
// for an id no role has.
const std::string &agent_role_duty(const std::string &id);

} // namespace singularity::agents
