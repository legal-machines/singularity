#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace singularity::organisation {

// Who sits in each of the firm's seats (D-209): their name, their role on the
// firm's scale, their own title, their practice and their photo. A seat at
// Legal Machines names no one (D-204); this is where the firm says who it is,
// in its own record (D-203), sealed between its own computers and never sent
// to Legal Machines. The firm's administrator writes it for any seat, a member
// for their own; each computer writes a file of its own for each seat it
// writes, and the newest stands.
struct SeatProfile {
	std::string seat_id;
	std::string name;
	// One of the firm's positions (Organisation/Types/FirmOffice), or empty.
	std::string rank;
	// Their own words for what they do ("Head of Litigation"), or empty.
	std::string title;
	std::string practice;
	// What they would have the firm know of them, in their own words, a few
	// lines (the owner, 2026-11-02: "the colleague's description must be
	// edited too"), or empty.
	std::string about;
	// A small square photo as a data URL (D-163), or empty.
	std::string photo;
	// When it was written (UTC, "2026-11-02T01:00:00Z") and by which computer.
	std::string at;
	std::string written_by;
	// Where the colleague stands with the firm (D-214): empty at work,
	// "archived" with their seat closed and their history kept, or "removed"
	// from the firm, whose record keeps only that they were.
	std::string state;
};

// Where the firm's record is on this computer, and what is said when a file
// is written there; installed by the app (NativeBridge), a test's own in its
// tests.
struct ProfilePlace {
	// The firm's record folder, or nothing while this computer holds none.
	std::function<std::optional<std::filesystem::path>()> firm;
	std::function<std::string()> device_id;
	std::function<std::string()> now;
	// Whether a photo is one the firm's records take (Matter/SharedMatter
	// is_member_photo).
	std::function<bool(const std::string &)> is_photo;
	// Told the folder written, so it goes to the firm's other computers at once.
	std::function<void(const std::filesystem::path &)> written;
};
void install_profile_place(ProfilePlace place);

// Every seat the firm's record says who sits in, the newest of each, ordered
// by seat id. Empty without a firm record here.
std::vector<SeatProfile> seat_profiles();
std::optional<SeatProfile> seat_profile(const std::string &seat_id);

// Writes what is said of one seat into the firm's record, after its words are
// checked, stamped with this computer and the time. Throws
// std::invalid_argument with the sentence the person reads; std::runtime_error
// when this computer holds no firm record yet.
SeatProfile save_seat_profile(SeatProfile person);

// The firm's AI Setup (D-210): which model each of the firm's roles is given,
// as an administrator's computer has it - the model's provider and its id,
// never a key. A member's computer, which connects no account of its own
// (D-184), uses it with the accounts the firm's computers gave it. Each
// computer writes a file of its own (`.lm/AI Setup/<device>.json`); the newest
// stands.
struct FirmModelRole {
	std::string role;
	std::string provider_id;
	std::string model_id;
};
struct FirmAiSetup {
	std::vector<FirmModelRole> roles;
	std::string at;
	std::string written_by;
};
// The newest the firm's record holds, or nothing.
std::optional<FirmAiSetup> firm_ai_setup();
// Writes this computer's into the firm's record when it differs from what the
// record holds newest; whether it wrote. A role with no model is left out.
// Throws std::runtime_error when this computer holds no firm record yet.
bool save_firm_ai_setup(const std::vector<FirmModelRole> &roles);

// A computer the firm knew as one of a seat's: the seat, its device key.
struct SeatComputer {
	std::string seat_id;
	std::string device_key;
};
// The computers of the seats the firm removed (D-214), each key once, never
// one of `own_seat`'s: every computer of the firm refuses them, so nothing
// more of a Matter goes to them and nothing of theirs is taken there.
std::vector<SeatComputer> computers_of_removed(const std::set<std::string> &removed_seats, const std::string &own_seat,
	const std::vector<SeatComputer> &known);

} // namespace singularity::organisation
