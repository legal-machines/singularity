#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace singularity::matter::firm_computers {

// The firm's computers. A computer signed in to one of the firm's seats says
// itself to Legal Machines - its device key, its X25519 key signed by the
// device key, and where its direct link may be reached - and finds the firm's
// other computers there. Nothing here trusts what Legal Machines lists: a
// computer counts as the firm's only once a lawyer here has checked it with
// its own lawyer, by a number both computers show.
//
// The check is commit-then-reveal, as Bluetooth's numeric comparison is: each
// computer first sends a commitment to its keys, its seat and a fresh random
// value, and reveals the value only once it holds the other's commitment,
// checking the other's value against its commitment before any number shows.
// Legal Machines passes the messages on and can do no more than drop or delay
// them: a key it put in makes the numbers differ.
//
// No lock of this module is held while it waits for Legal Machines, so the
// coordination's round, which asks checked() every round, never waits for the
// network.

struct Computer {
	std::string seat_id;
	// "administrator" or "member", as Legal Machines lists the seat; empty
	// where it does not say. Only for the lawyer's eyes: nothing trusts it.
	std::string role;
	std::string device_id;
	std::string device_key;
	std::string coordination_key;
	std::string signature;
	std::vector<std::string> addresses;
	std::string announced_at;
	// The organisation a lawyer here checked it in, by Legal Machines' id: it
	// counts as the firm's only while this computer is signed in to that very
	// organisation, never to another of the same name. Empty in a check kept
	// before checks named theirs: such a one counts only where the listing of
	// the organisation signed in to lists it, and is then kept as that
	// organisation's.
	std::string organisation_id = {};
};

struct Message {
	std::string from_device;
	std::string kind;
	std::string value;
};

// What the module needs, installed by the app (FirmComputersProduction) and by
// tests.
struct Environment {
	std::function<std::string()> device_id;
	std::function<std::string()> device_key;
	std::function<std::string()> coordination_key;
	// The seat signed in here; empty when none.
	std::function<std::string()> seat_id;
	// The organisation of that seat, by Legal Machines' id; empty when none.
	// The listing, the checks and what counts are that organisation's alone.
	std::function<std::string()> organisation_id;
	std::function<std::vector<std::string>()> candidates;
	std::function<std::string(const std::string &message)> sign;
	std::function<bool(const std::string &key, const std::string &message, const std::string &signature)> verify;
	// A fresh random value for one check: 32 bytes as hex.
	std::function<std::string()> nonce;
	// Legal Machines.
	std::function<void(const Computer &self)> announce;
	std::function<std::vector<Computer>()> list;
	std::function<void(const std::string &to_device, const std::string &kind, const std::string &value)> send;
	std::function<std::vector<Message>()> inbox;
	// The lawyer's own decisions (Tool/DeviceTrust).
	std::function<bool(const std::string &actor_id, const std::string &device_key)> is_confirmed;
	std::function<bool(const std::string &device_key)> is_refused;
	std::function<void(const std::string &actor_id, const std::string &device_key)> confirm;
	std::function<void(const std::string &actor_id, const std::string &device_key)> refuse;
	// Where the checked computers are kept, in this computer's own records.
	std::function<std::filesystem::path()> records;
	std::function<std::string()> now;
	// The member a seat's computer counts as: "seat:<id>".
	std::function<std::string(const std::string &seat_id)> actor_of;
	std::function<std::string(const std::string &device_key)> code_of;
	// Something the lawyer may want to see changed: another computer began a
	// check with this one, or a check is ready or failed. Called with no lock
	// held.
	std::function<void()> changed;
};
void install_environment(Environment environment);

// What a computer signs to say an X25519 key is its own.
std::string announcement_message(const std::string &device_id, const std::string &coordination_key);

// Says this computer to Legal Machines, signed. Quiet: nothing is said
// without a seat signed in here. Throws what Legal Machines answered.
void announce();

// The firm's computers as this computer sees them, by the listing as last
// read: read again here when it is older than half a minute.
struct View {
	std::string seat_id;
	std::string role;
	std::string device_id;
	// Its device key's code (Platform/DeviceKey).
	std::string code;
	// "this" | "checked" | "new" | "refused" | "unsigned" (its keys are not
	// signed as its own: it can't be checked).
	std::string state;
	std::string announced_at;
};
std::vector<View> computers();
// The same from the listing as last read, never asking Legal Machines: for
// the window's frequent reads.
std::vector<View> computers_as_known();

// One check with one computer.
struct Check {
	std::string device_id;
	// "waiting" for the other computer, "ready" with the number to compare,
	// "failed", or "none" when no check with it is under way.
	std::string state;
	// "4821 0937" when ready.
	std::string number;
	std::string problem;
};
// Begins the check with `device_id` from this side, or goes on with one it
// began; reads what came from the other computer.
Check start_check(const std::string &device_id);
// Where the check with `device_id` stands, after reading what came.
Check check_state(const std::string &device_id);
// The computers that began a check with this one, not yet begun here, as
// last read (the background reads what came every few seconds).
std::vector<std::string> asking();
// The lawyer saw the same number on both computers: the other computer is
// its seat's, kept with the X25519 key the check confirmed. Throws
// std::invalid_argument when the check is not ready.
void match(const std::string &device_id);
// The lawyer refused it: never shown again.
void refuse(const std::string &device_id);
// The lawyer closed the check without deciding: it ends here.
void cancel(const std::string &device_id);

// The computers a lawyer here checked, with the keys the check confirmed -
// whatever Legal Machines lists since: a computer listed with other keys is a
// new one to check, never put in its place - and their addresses now. None
// that Legal Machines no longer lists (its seat removed): taking trust away
// is the one thing it may do. Only the organisation's this computer is signed
// in to, each with its id; none when it is signed in to none. Never asks
// Legal Machines.
std::vector<Computer> checked();
// Every computer a lawyer here checked in the organisation signed in to, as
// this computer keeps it - listed by Legal Machines now or not: what a seat's
// removal refuses. Another organisation's checks are never its.
std::vector<Computer> checked_records();

// One pass of the background: this computer said again when due, the listing
// read again when due, and what came for this computer read.
void background_once();
// The background on a thread of its own, from start() to stop(); wake() runs a
// pass at once.
void start();
void stop() noexcept;
void wake() noexcept;

// Forgets every check under way and the cached listing (tests).
void reset_for_tests();

} // namespace singularity::matter::firm_computers
