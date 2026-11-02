#include "Organisation/OrganisationReadModel/OrganisationReadModel.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace singularity::organisation {
namespace {

std::string lowered(const std::string &text) {
	std::string out = text;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

// The first words of a long text, cut at a word, for a title.
std::string first_words(const std::string &text, std::size_t most = 160) {
	if (text.size() <= most) return text;
	std::size_t cut = text.rfind(' ', most);
	if (cut == std::string::npos || cut < most / 2) cut = most;
	return text.substr(0, cut) + "...";
}

bool is_open_operation(const std::string &state) {
	return state == "working" || state == "preparing" || state == "stopping" || state == "waiting_for_approval"
		|| state == "waiting_for_answer" || state == "interrupted";
}

std::string assignment_state_of_operation(const std::string &state) {
	if (state == "waiting_for_approval" || state == "waiting_for_answer") return "waiting_for_decision";
	if (state == "interrupted") return "interrupted";
	return "working";
}

std::string area_id_of(const std::string &agent_id) {
	return "area:" + agent_id;
}

} // namespace

OrganisationView build(const OrganisationFacts &facts) {
	OrganisationView view;
	view.viewer = facts.viewer;
	view.read_at = facts.now;

	std::map<std::string, std::string> matterNames;
	for (const MatterFacts &matter : facts.matters) matterNames[matter.id] = matter.name;
	const auto matterName = [&](const std::string &id) {
		const auto found = matterNames.find(id);
		return found == matterNames.end() ? std::string() : found->second;
	};
	std::set<std::string> colleagueIds;
	for (const ColleagueFacts &colleague : facts.colleagues) colleagueIds.insert(colleague.agent_id);
	const std::string &holder = facts.viewer.member_id;

	// --- Decisions: everything waiting on a member, wherever it is --------
	for (const MatterFacts &matter : facts.matters) {
		for (const OperationFacts &operation : matter.operations) {
			if (operation.state != "waiting_for_approval" && operation.state != "waiting_for_answer") continue;
			Decision decision;
			const bool isQuestion = operation.state == "waiting_for_answer";
			decision.kind = isQuestion ? "answer_question" : "approve_change";
			decision.id = (isQuestion ? "question:" : "approval:") + matter.id + ":" + operation.operation_id;
			decision.matter_id = matter.id;
			decision.matter_name = matter.name;
			decision.chat_id = operation.chat_id;
			decision.operation_id = operation.operation_id;
			decision.object_id = operation.operation_id;
			if (isQuestion)
				decision.title = operation.question_title.empty() == false ? operation.question_title
					: std::string("A question waits for your answer.");
			else
				decision.title = operation.activity.empty() == false ? operation.activity
					: std::string("A change waits for your approval.");
			decision.asked_by = operation.agent_id.empty() == false ? operation.agent_id
				: operation.author_actor_id.empty() == false ? operation.author_actor_id : holder;
			decision.holder = holder;
			decision.waiting_since = operation.updated_at;
			if (operation.runs_elsewhere) {
				decision.decidable_here = false;
				decision.elsewhere_reason = "Another computer's Singularity runs this request. It is decided there.";
			}
			view.decisions.push_back(decision);
		}
		for (const OverwrittenFacts &overwritten : matter.overwritten) {
			Decision decision;
			decision.kind = "overwritten_change";
			decision.id = "overwritten:" + matter.id + ":" + overwritten.id;
			decision.matter_id = matter.id;
			decision.matter_name = matter.name;
			decision.object_id = overwritten.id;
			decision.title = "A later save replaced a change Singularity wrote in " + overwritten.document_name + ".";
			decision.holder = holder;
			decision.waiting_since = overwritten.found_at;
			view.decisions.push_back(decision);
		}
		for (const ConflictFacts &conflict : matter.conflicts) {
			Decision decision;
			decision.kind = "conflict";
			decision.id = "conflict:" + matter.id + ":" + conflict.id;
			decision.matter_id = matter.id;
			decision.matter_name = matter.name;
			decision.object_id = conflict.id;
			decision.title = "Two changes to " + (conflict.label.empty() ? conflict.collection : conflict.label)
				+ " met, and one must be kept.";
			decision.holder = holder;
			decision.waiting_since = conflict.timestamp;
			view.decisions.push_back(decision);
		}
	}
	for (const ColleagueFacts &colleague : facts.colleagues) {
		if (colleague.runs_here == false) continue;
		Decision decision;
		decision.matter_id = colleague.matter_id;
		decision.matter_name = matterName(colleague.matter_id);
		decision.object_id = colleague.agent_id;
		decision.asked_by = colleague.agent_id;
		decision.holder = holder;
		decision.waiting_since = colleague.updated_at;
		if (colleague.status == "draft") {
			decision.kind = "approve_colleague";
			decision.title = "An AI app proposes " + colleague.name + " as a colleague.";
		} else if (colleague.paused_reason.empty() == false) {
			decision.kind = "colleague_paused";
			decision.title = colleague.paused_reason;
		} else if (colleague.state == "access_needed") {
			decision.kind = "colleague_needs_folder";
			decision.title = colleague.name + " cannot read its folder.";
		} else {
			continue;
		}
		decision.id = decision.kind + ":" + colleague.agent_id;
		view.decisions.push_back(decision);
	}
	std::stable_sort(view.decisions.begin(), view.decisions.end(), [](const Decision &left, const Decision &right) {
		if (left.waiting_since.empty() != right.waiting_since.empty()) return right.waiting_since.empty();
		return left.waiting_since < right.waiting_since;
	});

	// --- Assignments: the work under way -----------------------------------
	for (const ColleagueFacts &colleague : facts.colleagues) {
		for (const WorkGivenFacts &work : colleague.work) {
			if (work.state != "waiting" && work.state != "working") continue;
			Assignment assignment;
			assignment.id = "work:" + work.id;
			assignment.kind = "work_given";
			assignment.member_id = colleague.agent_id;
			assignment.given_by = work.given_by_actor_id;
			assignment.matter_id = colleague.matter_id;
			assignment.chat_id = work.chat_id;
			assignment.title = first_words(work.instruction);
			assignment.state = work.state;
			assignment.started_at = work.started_at.empty() ? work.given_at : work.started_at;
			assignment.updated_at = work.started_at.empty() ? work.given_at : work.started_at;
			view.assignments.push_back(assignment);
		}
		if (colleague.is_running) {
			Assignment assignment;
			assignment.id = "run:" + colleague.agent_id;
			assignment.kind = "run";
			assignment.member_id = colleague.agent_id;
			assignment.matter_id = colleague.matter_id;
			assignment.title = colleague.name + " " + (colleague.role_name.empty() ? std::string("is at work")
				: "at work: " + colleague.role_name);
			assignment.state = "working";
			for (const RunFacts &run : colleague.runs)
				if (run.status == "running") assignment.started_at = run.started_at;
			assignment.updated_at = assignment.started_at;
			view.assignments.push_back(assignment);
		}
	}
	for (const MatterFacts &matter : facts.matters) {
		for (const OperationFacts &operation : matter.operations) {
			if (is_open_operation(operation.state) == false) continue;
			Assignment assignment;
			assignment.id = "request:" + operation.operation_id;
			assignment.kind = "request";
			assignment.member_id = operation.agent_id.empty() == false ? operation.agent_id
				: operation.author_actor_id.empty() == false ? operation.author_actor_id : holder;
			assignment.matter_id = matter.id;
			assignment.chat_id = operation.chat_id;
			assignment.operation_id = operation.operation_id;
			assignment.title = first_words(operation.prompt);
			assignment.state = assignment_state_of_operation(operation.state);
			assignment.updated_at = operation.updated_at;
			view.assignments.push_back(assignment);
		}
	}

	// --- Areas: what each AI colleague owns --------------------------------
	for (const ColleagueFacts &colleague : facts.colleagues) {
		Area area;
		area.id = area_id_of(colleague.agent_id);
		area.member_id = colleague.agent_id;
		area.matter_id = colleague.matter_id;
		area.matter_name = matterName(colleague.matter_id);
		area.folder = colleague.folder;
		area.role = colleague.role;
		area.mode = colleague.mode;
		area.trigger = colleague.trigger;
		area.playbook_id = colleague.playbook_id;
		area.playbook_name = colleague.playbook_name;
		area.instruction = colleague.instruction;
		view.areas.push_back(area);
	}

	// --- Members: people and AI colleagues, one list -----------------------
	std::map<std::string, int> waitingByAsker;
	int heldByViewer = 0;
	for (const Decision &decision : view.decisions) {
		if (decision.kind == "approve_change") ++waitingByAsker[decision.asked_by];
		if (decision.holder == holder && decision.decidable_here) ++heldByViewer;
	}
	bool viewerListed = false;
	for (const PersonFacts &person : facts.people) {
		if (person.actor_id.empty() || colleagueIds.count(person.actor_id) > 0) continue;
		Member member;
		member.id = person.actor_id;
		member.kind = "person";
		member.name = person.name;
		member.is_viewer = person.actor_id == holder || person.is_you;
		member.state = "member";
		member.matter_ids = person.matter_ids;
		member.record.last_active_at = person.last_seen_at;
		if (member.is_viewer) {
			viewerListed = true;
			member.record.decisions_waiting = heldByViewer;
		}
		view.members.push_back(member);
	}
	if (viewerListed == false && holder.empty() == false) {
		Member member;
		member.id = holder;
		member.kind = "person";
		member.name = facts.viewer.name;
		member.is_viewer = true;
		member.state = "member";
		member.record.decisions_waiting = heldByViewer;
		view.members.push_back(member);
	}
	for (const ColleagueFacts &colleague : facts.colleagues) {
		Member member;
		member.id = colleague.agent_id;
		member.kind = "ai_colleague";
		member.name = colleague.name;
		member.role = colleague.role;
		member.role_name = colleague.role_name;
		member.state = colleague.state;
		member.answers_to = colleague.responsible_actor_id;
		member.character = colleague.character;
		member.presence = colleague.presence;
		member.runs_here = colleague.runs_here;
		member.area_ids = {area_id_of(colleague.agent_id)};
		// The Matter it was made for, in the zero state; none for one of the firm.
		if (colleague.matter_id.empty() == false) member.matter_ids = {colleague.matter_id};
		MemberRecord &record = member.record;
		record.recent_runs = static_cast<int>(colleague.runs.size());
		for (const RunFacts &run : colleague.runs) {
			if (run.status == "completed") ++record.recent_runs_completed;
			if (run.status == "failed") ++record.recent_runs_failed;
		}
		record.work_given = colleague.work_given;
		for (const WorkGivenFacts &work : colleague.work)
			if (work.state == "done") ++record.work_done;
		const auto waiting = waitingByAsker.find(colleague.agent_id);
		record.changes_waiting = waiting == waitingByAsker.end() ? 0 : waiting->second;
		record.spend = colleague.spend;
		record.spend_currency = colleague.spend_currency;
		record.spend_month = colleague.spend_month;
		record.last_active_at = colleague.last_run_at;
		view.members.push_back(member);
	}
	std::stable_sort(view.members.begin(), view.members.end(), [](const Member &left, const Member &right) {
		if (left.is_viewer != right.is_viewer) return left.is_viewer;
		const std::string a = lowered(left.name);
		const std::string b = lowered(right.name);
		if (a != b) return a < b;
		return left.id < right.id;
	});
	return view;
}

} // namespace singularity::organisation
