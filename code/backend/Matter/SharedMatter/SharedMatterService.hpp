#pragma once

#include "Agent/AgentCard/AgentCard.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/SharedMatter/SharedMatter.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace singularity::matter {

// The application's side of a Matter shared between computers
// (Matter/SharedMatter): who works on it, what changed and who changed it,
// and the changes a later save overwrote - read for the window, and kept
// current by the Matter's folder watch.

// What the folder watch does for a shared Matter: when a Matter is opened
// here, this person's member record; after each change it reconciled, a look
// for a change Singularity wrote that a save outside it overwrote.
SharedMatterHooks production_shared_matter_hooks();

struct SharedActivity {
	// More than one person's Singularity has opened this Matter, or a change
	// was made through another person's.
	bool is_shared = false;
	std::string my_actor_id;
	// What this computer may do with the Matter's folder, as the drive allows
	// it: "read_write", "read_only" (Singularity writes nothing there), or
	// "unavailable" (the drive cannot be reached now; nothing is lost).
	// Who may read or change a shared folder is the drive's to decide; Singularity
	// shows it and keeps to it.
	std::string access = "read_write";
	std::vector<SharedMember> members;
	// Every agent of the Matter, as the computer that runs it published it.
	std::vector<agents::AgentCard> agents;
	std::string this_device_id;
	std::vector<AttributedChange> changes;
	std::vector<OverwrittenChange> overwritten;
};

// The firm as this computer knows it: everyone whose Singularity opened one
// of the Matters known here, and every agent working for one of them.
struct FirmPerson {
	SharedMember member;
	bool is_you = false;
	// Archived by the firm: their seat closed, their history kept; or removed
	// from it, named only where they wrote.
	bool is_archived = false;
	bool is_removed = false;
	std::vector<std::pair<std::string, std::string>> matters;
};

struct FirmAgent {
	agents::AgentCard card;
	std::string matter_name;
};

struct FirmTeam {
	std::string my_actor_id;
	std::string this_device_id;
	std::vector<FirmPerson> people;
	std::vector<FirmAgent> agents;
};

FirmTeam firm_team();

SharedActivity shared_activity(const std::string &project_id, std::size_t most_changes);

// The person here confirms - or revokes - a colleague's computer as theirs
// (Tool/DeviceTrust), after comparing its code with them: the key that
// computer stated in this Matter's member records, never a key given from
// outside. A confirmed computer's signed messages count as the colleague's in
// every Matter; a revoked one's never again. The activity as it then reads.
SharedActivity decide_member_computer(const std::string &project_id, const std::string &actor_id,
	const std::string &device_id, bool confirm);

// The same for a computer that asks to join the Matter through the firm's
// coordination: it holds no Matter yet, so no member record states its key. It
// is decided by the key its request was signed with - the one whose code the
// person here compared - and so, once confirmed, what it writes when the
// Matter reaches it counts as the colleague's. A revoked one's request is not
// shown again.
SharedActivity decide_asking_computer(const std::string &project_id, const std::string &actor_id,
	const std::string &device_key, bool confirm);

// A lawyer's decision on an overwritten change: "keep_current" leaves the
// document as it is; "restore_written" puts the version with the change back
// (Document/ProfessionalArtifactAction restore_document_version), keeping the
// one it replaces, so nothing is lost either way.
SharedActivity resolve_overwritten(const std::string &project_id, const std::string &change_id, const std::string &choice);

// A document's paragraphs, normalised: what an overwrite is told by.
std::optional<std::vector<std::string>> read_document_paragraphs(const std::filesystem::path &file);

} // namespace singularity::matter
