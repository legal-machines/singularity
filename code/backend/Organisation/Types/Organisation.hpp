#pragma once

#include <string>
#include <vector>

// The organisation as the window reads it: its members, people and AI
// colleagues in one list, the areas they own, the work under way, the
// decisions waiting on a member, and what each member's record already says. A
// read model over today's data: it holds nothing of its own and decides
// nothing; each fact comes from the domain that owns it
// (Organisation/OrganisationProduction).
namespace singularity::organisation {

// Who is looking: the person at this computer, and what today's code lets that
// member do. Until roles carry rights, the person at this computer holds every
// decision made here.
struct Viewer {
	std::string member_id;
	std::string name;
	std::string organisation_id;
	std::string organisation_name;
	std::string seat_id;
	std::string device_id;
	// Their place in the firm, as they state it (Organisation/Types/FirmOffice):
	// their position and their practice, each empty where they stated none.
	std::string position;
	std::string practice;
	// The photo they show the firm, a data URL; empty without one.
	std::string photo;
	bool decides_changes = true;
	bool answers_questions = true;
	bool gives_work = true;
	bool manages_colleagues = true;
	// Connects the firm's model accounts and sets a colleague's model and
	// provider: the administrative account's alone.
	bool connects_models = true;
	// Makes, re-passwords and removes the firm's seats: the
	// administrative account's alone.
	bool manages_seats = true;
	bool resolves_conflicts = true;
};

// What today's data can say of a member's work. Recent runs are a window of an
// AI colleague's latest runs, not its whole history.
struct MemberRecord {
	int recent_runs = 0;
	int recent_runs_completed = 0;
	int recent_runs_failed = 0;
	int work_given = 0;
	int work_done = 0;
	int changes_waiting = 0;
	int decisions_waiting = 0;
	double spend = 0.0;
	std::string spend_currency;
	std::string spend_month;
	std::string last_active_at;
};

struct Member {
	// A person's actor id; an AI colleague's agent id.
	std::string id;
	// "person" or "ai_colleague": a plain fact, shown as such.
	std::string kind;
	std::string name;
	bool is_viewer = false;
	// The role as today's code knows it: an AI colleague's role id and name
	// (Agent/AgentRoles); people carry none yet (R3).
	std::string role;
	std::string role_name;
	// An AI colleague's state (Agent/AgentActions agent_state); "member" for a
	// person.
	std::string state;
	// The member it answers to today: an AI colleague's responsible lawyer.
	std::string answers_to;
	std::string character;
	// What an AI colleague's character acts out now: greet, think, work,
	// wait, explain, listen or rest. Empty for a person.
	std::string presence;
	// Its computer runs it here, or a colleague's computer does.
	bool runs_here = true;
	std::vector<std::string> area_ids;
	std::vector<std::string> matter_ids;
	MemberRecord record;
};

// What an AI colleague owns today: one scope of one Matter. Colleagues will
// own areas across Matters (R3).
struct Area {
	std::string id;
	std::string member_id;
	std::string matter_id;
	std::string matter_name;
	// Its folder, relative to the Matter's; empty for the whole Matter.
	std::string folder;
	std::string role;
	std::string mode;
	std::string trigger;
	std::string playbook_id;
	std::string playbook_name;
	std::string instruction;
};

// Work under way: work given to a colleague, a colleague's own run, or a
// request running in a Matter.
struct Assignment {
	std::string id;
	// "work_given", "run" or "request".
	std::string kind;
	std::string member_id;
	std::string given_by;
	std::string matter_id;
	std::string chat_id;
	std::string operation_id;
	std::string title;
	// "waiting", "working", "waiting_for_decision" or "interrupted".
	std::string state;
	std::string started_at;
	std::string updated_at;
};

// Something waiting on a member, wherever it is.
struct Decision {
	std::string id;
	// "approve_change", "answer_question", "overwritten_change", "conflict",
	// "approve_colleague", "colleague_paused" or "colleague_needs_folder".
	std::string kind;
	std::string matter_id;
	std::string matter_name;
	std::string chat_id;
	std::string operation_id;
	// The object it is about: a change, a conflict, a colleague.
	std::string object_id;
	std::string title;
	// The member whose work raised it, and the member who holds it.
	std::string asked_by;
	std::string holder;
	std::string waiting_since;
	// It can be decided on this computer; otherwise why not, in words.
	bool decidable_here = true;
	std::string elsewhere_reason;
};

struct OrganisationView {
	Viewer viewer;
	std::vector<Member> members;
	std::vector<Area> areas;
	std::vector<Assignment> assignments;
	std::vector<Decision> decisions;
	std::string read_at;
};

} // namespace singularity::organisation
