#include "Agent/AgentRoles/AgentRoles.hpp"

#include <algorithm>

namespace singularity::agents {

const std::vector<AgentRole> &agent_roles() {
	static const std::vector<AgentRole> roles = [] {
		std::vector<AgentRole> list;
		AgentRole watch;
		watch.id = kAgentKindDataRoomWatch;
		watch.mode = AgentMode::Changes;
		watch.label = "Watch a data room";
		watch.description = "Reports new and changed documents, quoting what changed.";
		watch.duty = "watches the data room";
		watch.name = "Data room watch";
		watch.goal = "Report new documents, changed clauses and missing consents, and anything a lawyer should read.";
		watch.trigger = AgentTrigger::OnChange;
		list.push_back(watch);

		AgentRole deadlines;
		deadlines.id = "deadlines";
		deadlines.mode = AgentMode::Changes;
		deadlines.label = "Track deadlines";
		deadlines.description = "Finds dates, notice periods and deadlines in new and changed documents.";
		deadlines.duty = "tracks deadlines";
		deadlines.name = "Deadlines";
		deadlines.goal = "Find every deadline, notice period and date by which someone must act in the new or changed "
			"documents. Quote the clause and say who must act.";
		deadlines.trigger = AgentTrigger::OnChange;
		list.push_back(deadlines);

		AgentRole review;
		review.id = "new_documents";
		review.mode = AgentMode::Changes;
		review.label = "Review new documents";
		review.description = "Says what each new document is and what it asks of each party.";
		review.duty = "reviews new documents";
		review.name = "New documents review";
		review.goal = "For each new or changed document, say what it is, who the parties are, what it obliges each of "
			"them to do, and anything unusual a lawyer should read.";
		review.trigger = AgentTrigger::OnChange;
		list.push_back(review);

		AgentRole summary;
		summary.id = "matter_summary";
		summary.mode = AgentMode::Task;
		summary.label = "Weekly Matter summary";
		summary.description = "Writes where the Matter stands, from its documents, once a week.";
		summary.duty = "keeps the weekly Matter summary";
		summary.name = "Weekly Matter summary";
		summary.goal = "Write a short summary of where this Matter stands: what changed this week, what is still open, "
			"and what needs a decision.";
		summary.trigger = AgentTrigger::Weekly;
		summary.weekly_day = 1;
		summary.daily_minute = 9 * 60;
		list.push_back(summary);

		AgentRole status;
		status.id = "status_update";
		status.mode = AgentMode::Task;
		status.label = "Client status note";
		status.description = "Drafts a status note for the client; it never sends anything.";
		status.duty = "writes the client status note";
		status.name = "Client status note";
		status.goal = "Draft a short status note for the client on this Matter's progress this week, in plain English. "
			"Do not send it.";
		status.trigger = AgentTrigger::Weekly;
		status.weekly_day = 5;
		status.daily_minute = 16 * 60;
		list.push_back(status);

		AgentRole playbook;
		playbook.id = "playbook_review";
		playbook.mode = AgentMode::Task;
		playbook.label = "Check against a Playbook";
		playbook.description = "Checks contracts against your firm's Playbook and drafts the changes.";
		playbook.duty = "checks contracts against the firm's Playbook";
		playbook.name = "Playbook review";
		playbook.trigger = AgentTrigger::OnChange;
		playbook.uses_playbook = true;
		list.push_back(playbook);

		AgentRole custom;
		custom.id = "custom";
		custom.mode = AgentMode::Task;
		custom.label = "Your own task";
		custom.description = "Any task you would give a chat, done on its own schedule.";
		custom.duty = "carries out a standing task";
		custom.trigger = AgentTrigger::Manual;
		list.push_back(custom);
		return list;
	}();
	return roles;
}

const AgentRole *agent_role(const std::string &id) {
	const std::vector<AgentRole> &roles = agent_roles();
	const auto found = std::find_if(roles.begin(), roles.end(), [&](const AgentRole &role) { return role.id == id; });
	return found == roles.end() ? nullptr : &*found;
}

const std::string &agent_role_duty(const std::string &id) {
	const AgentRole *role = agent_role(id);
	return role != nullptr ? role->duty : agent_role("custom")->duty;
}

} // namespace singularity::agents
