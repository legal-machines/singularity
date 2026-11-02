#pragma once

#include "Document/Types/DocumentEvent.hpp"
#include "Matter/Types/Matter.hpp"
#include "Organisation/Types/FirmOffice.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::matter {

// A MATTER SHARED BETWEEN COMPUTERS
//
// Several lawyers may open the same Matter folder from a shared or synced
// drive. Nothing coordinates them but the folder itself - no Legal Machines
// server - so what they share is written as records no two computers write
// at once, in the Matter's own `.lm`:
//
//   .lm/Members/<member>.json          one per person whose Singularity opened
//                                      the Matter, written only by that person's
//                                      Singularity: their name as their own
//                                      Settings or account give it, and when
//                                      they were last here
//   .lm/Conflicts/<id>.json            a change Singularity wrote that a later
//                                      save outside it overwrote, found by
//                                      whichever computer saw it first; the id
//                                      is the same on every computer, so it is
//                                      kept once
//   .lm/Conflicts/<id>.resolution.json what a lawyer decided about it
//
// A change is named by who made it only when that is known: a change Singularity
// wrote carries its person (DocumentEvent::actor_id); a change
// found on disk that is exactly what a member's Singularity wrote is theirs;
// anything else is an external change whose author is unknown. A name is
// never guessed from the file system.
//
// Nothing here says who is online now: Singularity has no way to know.

// A computer of a member, by the key it signs with (Platform/DeviceKey): stated
// under that key's own signature, so the statement is the key holder's. Whose
// computer it is, is the person's decision on each other computer
// (Tool/DeviceTrust).
struct SharedDeviceKey {
	std::string device_id;
	std::string public_key;
	// Asks to join the Matter through the firm's coordination (D-123): its
	// lawyer is to compare its code and confirm it.
	bool is_asking = false;
};

struct SharedMember {
	std::string actor_id;
	std::string name;
	std::string organisation;
	// Their place in the firm, as they state it (organisation::MemberOffice):
	// their position on the firm's scale and their practice; empty where they
	// stated none.
	std::string position;
	std::string practice;
	// The person's name comes from a Legal Machines account, not only from
	// their own Settings.
	bool is_account_name = false;
	std::vector<std::string> device_ids;
	// Only statements whose signature verifies.
	std::vector<SharedDeviceKey> device_keys;
	std::string first_seen_at;
	std::string last_seen_at;
	// The photo they show the firm (D-163): a data URL of a small JPEG or PNG,
	// empty without one or where the record's is not a photo
	// (is_member_photo).
	std::string photo;
	// Their own words for what they do, from the firm's record of who sits in
	// their seat (D-209); never in a Matter's record.
	std::string title;
	// What they would have the firm know of them, from the same record.
	std::string about;
	// A lawyer this computer gave the Matter to by choosing them (D-205), with
	// no record of their own in it yet: invited, until their computer has it
	// and opens it.
	bool is_invited = false;
};

// Writes this person's own member record into the Matter's `.lm` (only when
// the Matter has one), at most once a day: their name, their computer, when.
// Never throws; a read-only or unreachable share is left as it is.
void record_member(const std::filesystem::path &project_folder, const std::string &actor_id, const std::string &name,
	const std::string &organisation, bool is_account_name, const std::string &device_id, const std::string &now);
// The same, with their place in the firm: a changed one is written at once,
// not the next day.
void record_member(const std::filesystem::path &project_folder, const std::string &actor_id, const std::string &name,
	const std::string &organisation, const ::singularity::organisation::MemberOffice &office, bool is_account_name,
	const std::string &device_id, const std::string &now);
std::vector<SharedMember> load_members(const std::filesystem::path &project_folder);

// The place in the firm the person at this computer states (their position
// and practice), from where the application keeps it
// (Organisation/OrganisationProduction): the application installs the source
// when it starts; without one, none is stated.
void set_own_office_source(std::function<organisation::MemberOffice()> source);
organisation::MemberOffice own_office();
// The photo the person at this computer shows the firm (D-163), from where the
// application keeps it (Organisation/OrganisationProduction): it travels in
// their member record as their office does. Without a source, none.
void set_own_photo_source(std::function<std::string()> source);
std::string own_photo();
// The firm's seal on a Matter (D-167): `.lm/Seal.json`, a statement that a
// computer of the firm made or keeps this Matter for its organisation, signed
// with that computer's own key (Platform/DeviceKey). It travels with the
// Matter between the firm's computers, and only a Matter whose seal is the
// firm's is offered or opened here as the firm's: a `.lm` copied in from
// outside, or written by hand, is not.
enum class MatterSealState {
	// Sealed for this organisation, for this Matter, unchanged, by this
	// computer or by a computer confirmed here (set_seal_trust).
	Ours,
	// The same, by a computer no one here has confirmed yet: its person is
	// confirmed from what they wrote in the Matter (D-142).
	Unconfirmed,
	// Sealed, and the signature holds, but for another organisation.
	OtherOrganisation,
	// No seal at all.
	Unsealed,
	// A seal that does not hold: changed after it was signed, made for another
	// Matter, or signed by a computer the person here refused.
	Untrusted,
};
// What the person here decided about a computer whose key signed a seal.
enum class SealComputerTrust { Confirmed, Unconfirmed, Refused };
// Seals the Matter in `project_folder` (only when it has a `.lm`) for
// `organisation_id`, signed with this computer's key; writes over a seal
// already there. Throws when it cannot be written.
void seal_matter(const std::filesystem::path &project_folder, const std::string &matter_id, const std::string &organisation_id,
	const std::string &actor_id, const std::string &device_id, const std::string &now);
MatterSealState matter_seal_state(const std::filesystem::path &project_folder, const std::string &matter_id,
	const std::string &organisation_id);
// Keeps the seal of a Matter listed here, as it opens: this computer seals it
// for `organisation_id` when it has no seal (it was made before the seal
// existed), when its seal is this computer's own and no longer holds, or when
// this computer sealed it while signed in to no organisation and now is.
// A seal of another organisation, or a colleague's, is never written over.
// Throws when a seal it should write cannot be written.
void keep_seal(const std::filesystem::path &project_folder, const std::string &matter_id, const std::string &organisation_id,
	const std::string &actor_id, const std::string &device_id, const std::string &now);
// Whose computers' seals are the firm's besides this computer's own: the
// application installs Tool/DeviceTrust's decisions (a key the person here
// confirmed, or revoked). Without it, every other computer is unconfirmed.
void set_seal_trust(std::function<SealComputerTrust(const std::string &actor_id, const std::string &public_key)> trust);
// Whether this computer carries the Matter `matter_id` in `folder` by the
// firm's exchange: its lawyer accepted the Matter's key from a computer they
// confirmed (D-123). The application installs Matter/MatterCoordination's.
// Without it, none.
void set_seal_carrier(std::function<bool(const std::string &matter_id, const std::filesystem::path &folder)> carries);
// Whether the Matter in `folder` is the firm's, so that it is offered here
// when it is not listed: its seal is Ours, or the firm's exchange carries it.
bool is_firms_matter(const std::filesystem::path &project_folder, const std::string &matter_id, const std::string &organisation_id);
// Whether the person may open the Matter in `folder` from it, when it is not
// listed here: it is the firm's, or sealed for the firm by a computer not
// confirmed here yet, which its person then confirms from what it wrote.
bool may_open_matter(const std::filesystem::path &project_folder, const std::string &matter_id, const std::string &organisation_id);

// The kind of picture `bytes` hold, by their first bytes: "image/jpeg",
// "image/png", "image/gif" or "image/webp"; empty for anything else.
std::string picture_type(const std::string &bytes);
// Whether `photo` is a member's photo the window may show: a data URL of a
// JPEG or PNG whose bytes are one, of at most 96 KB. A member record is read
// by everyone who shares the Matter, so nothing else in it is believed.
bool is_member_photo(const std::string &photo);
// Whether anyone but `my_actor_id` has opened this Matter from their own
// computer: its data is then theirs too (Beta 1 #142).
bool has_other_members(const std::filesystem::path &project_folder, const std::string &my_actor_id);

// A computer that asks to join the Matter through the firm's coordination
// (D-123), by the device key its request was signed with. It holds no Matter
// yet, so none of the Matter's records names it.
struct AskingDevice {
	std::string actor_id;
	std::string device_id;
	std::string device_key;
};

// Sets the asking computers among the Matter's members, each with its member,
// marked asking: one that already stands there takes the key its request was
// signed with, the one whose code its lawyer compares; one whose member the
// Matter does not name yet brings that member, named by `name_of` (empty when
// no record of the firm names them). One that `is_refused` - the person here
// said it is not theirs - is left out.
void add_asking_devices(std::vector<SharedMember> &members, const std::vector<AskingDevice> &asking,
	const std::function<std::string(const std::string &actor_id)> &name_of,
	const std::function<bool(const AskingDevice &device)> &is_refused);

// A change Singularity wrote to a document that a later save outside Singularity
// replaced without it: the version that holds the change, the one
// it was made to, and the document as it is now - all three kept - with the
// words of the change the current document no longer has.
struct OverwrittenChange {
	std::string id;
	std::string document_id;
	std::string name;
	std::string relative_path;
	std::string found_at;
	std::string found_by_device_id;
	// The version Singularity wrote, and who wrote it.
	int written_version = 0;
	std::string written_fingerprint;
	std::string written_by_actor_id;
	std::string written_operation_id;
	// The version it was made to: what the later save was based on.
	int base_version = 0;
	std::string base_fingerprint;
	// The document when the overwrite was found.
	int current_version = 0;
	std::string current_fingerprint;
	// Up to three passages of the change, each at most 240 characters.
	std::vector<std::string> missing_passages;
	// What a lawyer decided: "kept_current" or "restored_written"; empty while
	// it waits.
	std::string resolution;
	std::string resolved_at;
	std::string resolved_by_actor_id;
};

// The words of a document, one paragraph each, normalised; nothing when it
// cannot be read.
using ParagraphReader = std::function<std::optional<std::vector<std::string>>(const std::filesystem::path &file)>;

// Looks at one change found on disk (an event with no actor): when the
// version it replaced is one Singularity wrote, and the change that write
// made is missing from the document now, the overwrite is recorded (once, by
// id) and returned. `versions_root` is where kept versions are
// (Document/DocumentVersionStore).
std::optional<OverwrittenChange> detect_overwritten_change(const std::filesystem::path &project_folder,
	const std::filesystem::path &versions_root, const matters::Matter &matter, const documents::DocumentEvent &event,
	const std::filesystem::path &current_file, const ParagraphReader &read_paragraphs, const std::string &device_id,
	const std::string &now);

std::vector<OverwrittenChange> load_overwritten_changes(const std::filesystem::path &project_folder);
// Records a lawyer's decision; refuses one already decided.
OverwrittenChange resolve_overwritten_change(const std::filesystem::path &project_folder, const std::string &id,
	const std::string &resolution, const std::string &actor_id, const std::string &now);

// Who made a change, as far as Singularity knows it.
enum class ChangeAuthor {
	You,        // this person, through Singularity (on any of their computers)
	Member,     // another member, through Singularity
	Workspace,  // Singularity wrote it before it recorded who asked
	External,   // found in the folder; nobody Singularity knows made it
};

struct AttributedChange {
	documents::DocumentEvent event;
	ChangeAuthor author = ChangeAuthor::External;
	// The member's name when the author is a member; empty otherwise, or when
	// the member never named themselves.
	std::string author_name;
};

// The Matter's recent document changes, newest first, at most `most`, each
// named by who made it. A change found on disk that is exactly what an Singularity
// write made is that write, and is not listed twice.
std::vector<AttributedChange> attributed_changes(const matters::Matter &matter, const std::vector<SharedMember> &members,
	const std::string &my_actor_id, std::size_t most);

} // namespace singularity::matter
