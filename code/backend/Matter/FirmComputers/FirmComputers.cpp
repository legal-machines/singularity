#include "Matter/FirmComputers/FirmComputers.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace singularity::matter::firm_computers {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr const char *kLabel = "singularity-d205-v1";
constexpr int kFormatVersion = 1;
// The listing a lawyer looks at is read again after this long.
constexpr auto kListingFresh = std::chrono::seconds(30);
// The background: what came for this computer, read every few seconds, and
// more often while a check is under way here; the firm's computers read again
// every twenty seconds, so a colleague's new computer shows soon after it
// signs in; this computer said again when what it says changed, and every
// ten minutes.
constexpr auto kQuietEvery = std::chrono::seconds(15);
constexpr auto kCheckingEvery = std::chrono::seconds(2);
constexpr auto kListingEvery = std::chrono::seconds(20);
constexpr auto kAnnounceEvery = std::chrono::minutes(10);

// One check with one other computer, from this side. In memory only: a check
// cut short by a restart begins again.
struct Session {
	// The other computer's keys and seat as the listing said them when the
	// check began, signed as its own: what its commitment is checked against.
	Computer peer;
	// This computer's own, as they were when the check began.
	std::string my_key;
	std::string my_coordination;
	std::string my_seat;
	std::string my_nonce;
	bool is_committed = false;
	std::string peer_commitment;
	std::string peer_nonce;
	bool is_revealed = false;
	std::string state = "waiting";
	std::string number;
	std::string problem;
};

// A message this computer is to send, for the session `key`.
struct Outgoing {
	std::string key;
	std::string to_device;
	std::string kind;
	std::string value;
};

struct State {
	// Held over the network's part of an operation, so what this computer sends
	// and reads goes in order. Never taken by checked().
	std::mutex io;
	// Held only for a moment, never over the network.
	std::mutex mutex;
	Environment environment;
	std::map<std::string, Session> sessions;
	// Commitments that came from computers whose check is not begun here yet.
	std::map<std::string, std::string> waiting_commitments;
	std::vector<Computer> listing;
	bool is_listing_read = false;
	Clock::time_point listed_at {};
	// The organisation whose computers `listing` holds: read again once
	// this computer is signed in to another.
	std::string listing_organisation;
	// What this computer last said of itself, and when.
	std::string said;
	Clock::time_point said_at {};
	// The background.
	std::mutex run_mutex;
	std::condition_variable woken;
	std::thread thread;
	bool is_running = false;
	bool is_stopping = false;
	bool is_woken = false;
};

State &state() {
	static State *const held = new State();
	return *held;
}

Environment environment_now(State &held) {
	const std::lock_guard<std::mutex> lock(held.mutex);
	return held.environment;
}

// A check is this computer's with one other: two computers checking each
// other in one test program keep theirs apart.
std::string key_of(const std::string &me, const std::string &device_id) {
	return me + "\n" + device_id;
}

std::string my_device(const Environment &environment) {
	return environment.device_id ? environment.device_id() : std::string();
}

std::string organisation_of(const Environment &environment) {
	return environment.organisation_id ? environment.organisation_id() : std::string();
}

std::string commitment_of(const std::string &device_key, const std::string &coordination_key, const std::string &seat_id,
		const std::string &nonce) {
	return platform::sha256_hex_digest(std::string(kLabel) + "\ncommit\n" + device_key + "\n" + coordination_key + "\n" + seat_id
		+ "\n" + nonce);
}

// The number both computers show: from both computers' keys and seats and
// both values, the computer with the lower device key first.
std::string number_of(const Session &session) {
	const Computer &peer = session.peer;
	const bool mineFirst = session.my_key < peer.device_key;
	const std::string mine = session.my_key + "\n" + session.my_coordination + "\n" + session.my_seat;
	const std::string theirs = peer.device_key + "\n" + peer.coordination_key + "\n" + peer.seat_id;
	const std::string first = mineFirst ? mine : theirs;
	const std::string second = mineFirst ? theirs : mine;
	const std::string nonces = mineFirst ? session.my_nonce + "\n" + session.peer_nonce : session.peer_nonce + "\n" + session.my_nonce;
	const std::string digest = platform::sha256_hex_digest(std::string(kLabel) + "\nnumber\n" + first + "\n" + second + "\n" + nonces);
	std::uint64_t value = 0;
	for (std::size_t at = 0; at < 16; ++at) value = (value << 4) | static_cast<std::uint64_t>(std::stoi(digest.substr(at, 1), nullptr, 16));
	std::string digits = std::to_string(value % 100000000ULL);
	while (digits.size() < 8) digits.insert(digits.begin(), '0');
	return digits.substr(0, 4) + " " + digits.substr(4);
}

bool is_signed(const Environment &environment, const Computer &computer) {
	return environment.verify && environment.verify(computer.device_key,
		announcement_message(computer.device_id, computer.coordination_key), computer.signature);
}

std::string actor_of(const Environment &environment, const std::string &seat_id) {
	return environment.actor_of ? environment.actor_of(seat_id) : "seat:" + seat_id;
}

std::vector<Computer> read_records(const Environment &environment) {
	std::vector<Computer> kept;
	if (!environment.records) return kept;
	std::ifstream file(environment.records(), std::ios::binary);
	if (!file) return kept;
	const Json value = Json::parse(file, nullptr, false);
	if (!value.is_object() || !value.contains("checked") || !value.at("checked").is_array()) return kept;
	for (const Json &entry : value.at("checked")) {
		if (!entry.is_object()) continue;
		Computer computer;
		computer.seat_id = entry.value("seatId", std::string());
		computer.device_id = entry.value("deviceId", std::string());
		computer.device_key = entry.value("deviceKey", std::string());
		computer.coordination_key = entry.value("coordinationKey", std::string());
		computer.organisation_id = entry.value("organisationId", std::string());
		if (computer.device_id.empty() || computer.device_key.empty() || computer.coordination_key.empty()) continue;
		kept.push_back(std::move(computer));
	}
	return kept;
}

// Whether a check is the organisation's: checked in it, or kept from before
// checks named their organisation and listed by its listing with the very keys
// the check confirmed.
bool is_of_organisation(const Computer &record, const std::string &organisation, const std::vector<Computer> &listing,
		bool is_listing_its) {
	if (organisation.empty()) return false;
	if (record.organisation_id.empty() == false) return record.organisation_id == organisation;
	if (is_listing_its == false) return false;
	return std::any_of(listing.begin(), listing.end(), [&record](const Computer &listed) {
		return listed.device_id == record.device_id && listed.device_key == record.device_key;
	});
}

// The checks of `organisation` among those kept, each with its id.
std::vector<Computer> records_of(const std::vector<Computer> &kept, const std::string &organisation,
		const std::vector<Computer> &listing, bool is_listing_its) {
	std::vector<Computer> found;
	for (const Computer &record : kept) {
		if (is_of_organisation(record, organisation, listing, is_listing_its) == false) continue;
		found.push_back(record);
		found.back().organisation_id = organisation;
	}
	return found;
}

void write_records(const Environment &environment, const std::vector<Computer> &checked) {
	if (!environment.records) return;
	Json list = Json::array();
	for (const Computer &computer : checked) {
		Json entry{{"seatId", computer.seat_id}, {"deviceId", computer.device_id}, {"deviceKey", computer.device_key},
			{"coordinationKey", computer.coordination_key}, {"checkedAt", environment.now ? environment.now() : std::string()}};
		if (computer.organisation_id.empty() == false) entry["organisationId"] = computer.organisation_id;
		list.push_back(std::move(entry));
	}
	const fs::path path = environment.records();
	std::error_code error;
	fs::create_directories(path.parent_path(), error);
	const fs::path staged = platform::staged_sibling_path(path);
	platform::write_file_durably(staged, Json{{"formatVersion", kFormatVersion}, {"checked", list}}.dump(2) + "\n");
	platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
}

void fail(Session &session, const std::string &problem) {
	session.state = "failed";
	session.number.clear();
	session.problem = problem;
}

// Reads the firm's computers from Legal Machines, with no lock held but the
// io one; kept as they were when it can't be reached.
void refresh_listing(State &held, const Environment &environment) {
	if (!environment.list) return;
	// The organisation signed in to as the listing is asked for: what comes is
	// its computers.
	const std::string organisation = organisation_of(environment);
	std::optional<std::vector<Computer>> listed;
	try {
		listed = environment.list();
	} catch (const std::exception &) {
	}
	const std::lock_guard<std::mutex> lock(held.mutex);
	held.listed_at = Clock::now();
	if (listed.has_value() == false) {
		// Another organisation's listing is not this one's, though nothing came.
		if (held.listing_organisation != organisation) {
			held.listing.clear();
			held.is_listing_read = false;
		}
		return;
	}
	held.listing = std::move(*listed);
	held.is_listing_read = true;
	held.listing_organisation = organisation;
}

bool is_listing_stale(State &held, Clock::duration fresh) {
	Environment environment;
	{
		const std::lock_guard<std::mutex> lock(held.mutex);
		environment = held.environment;
	}
	const std::string organisation = organisation_of(environment);
	const std::lock_guard<std::mutex> lock(held.mutex);
	return held.is_listing_read == false || held.listing_organisation != organisation || Clock::now() - held.listed_at >= fresh;
}

// The listing as last read, when it is the listing of the organisation signed
// in to now; none else.
std::vector<Computer> listing_of_now(State &held, const Environment &environment) {
	const std::string organisation = organisation_of(environment);
	const std::lock_guard<std::mutex> lock(held.mutex);
	if (held.is_listing_read == false || organisation.empty() || held.listing_organisation != organisation) return {};
	return held.listing;
}

// The checks kept from before checks named their organisation, that the
// organisation signed in to lists now with their very keys, kept from now on
// as that organisation's.
void name_kept_checks(State &held, const Environment &environment) {
	const std::string organisation = organisation_of(environment);
	if (organisation.empty()) return;
	const std::vector<Computer> listing = listing_of_now(held, environment);
	if (listing.empty()) return;
	std::vector<Computer> kept = read_records(environment);
	bool isNamed = false;
	for (Computer &record : kept) {
		if (record.organisation_id.empty() == false || is_of_organisation(record, organisation, listing, true) == false) continue;
		record.organisation_id = organisation;
		isNamed = true;
	}
	if (isNamed == true) write_records(environment, kept);
}

std::vector<Message> fetch_inbox(const Environment &environment) {
	if (!environment.inbox) return {};
	try {
		return environment.inbox();
	} catch (const std::exception &) {
		return {};
	}
}

// Queues this side's reveal once it holds the other's commitment, and shows
// the number once the other's value matches its commitment - never before.
void advance_locked(Session &session, const std::string &key, std::vector<Outgoing> &out) {
	if (session.state == "failed") return;
	if (session.is_committed && !session.peer_commitment.empty() && !session.is_revealed) {
		out.push_back(Outgoing{key, session.peer.device_id, "reveal", session.my_nonce});
		session.is_revealed = true;
	}
	if (session.is_revealed && !session.peer_nonce.empty() && session.number.empty()) {
		session.number = number_of(session);
		session.state = "ready";
	}
}

// What came from the other computers, once: a commitment held, a value
// checked against the commitment held before it. Says whether anything the
// lawyer may want to see changed.
bool take_locked(State &held, const std::string &me, const std::vector<Message> &messages, std::vector<Outgoing> &out) {
	bool changed = false;
	for (const Message &message : messages) {
		const std::string key = key_of(me, message.from_device);
		const auto found = held.sessions.find(key);
		if (message.kind == "commit") {
			if (found == held.sessions.end() || found->second.state == "failed") {
				// A check the other computer began, or began again.
				if (found != held.sessions.end()) held.sessions.erase(found);
				changed = held.waiting_commitments[key] != message.value || changed;
				held.waiting_commitments[key] = message.value;
				continue;
			}
			Session &session = found->second;
			if (!session.peer_commitment.empty() && session.peer_commitment != message.value) {
				fail(session, "The other computer's check changed on its way. Begin it again.");
				changed = true;
				continue;
			}
			session.peer_commitment = message.value;
			const std::string before = session.state;
			advance_locked(session, key, out);
			changed = changed || session.state != before;
		} else if (message.kind == "reveal") {
			if (found == held.sessions.end()) continue;
			Session &session = found->second;
			if (session.state != "waiting") continue;
			// A value before its commitment could be chosen to fit: never taken.
			if (session.peer_commitment.empty()) {
				fail(session, "The check came out of order. Begin it again.");
				changed = true;
				continue;
			}
			if (commitment_of(session.peer.device_key, session.peer.coordination_key, session.peer.seat_id, message.value)
					!= session.peer_commitment) {
				fail(session, "The other computer's check does not match what it promised. Begin it again.");
				changed = true;
				continue;
			}
			session.peer_nonce = message.value;
			advance_locked(session, key, out);
			changed = true;
		}
	}
	return changed;
}

// Sends what was queued, in order; a message that could not go ends its check,
// and nothing after it for that check goes.
void send_all(State &held, const Environment &environment, const std::vector<Outgoing> &out) {
	std::vector<std::string> broken;
	for (const Outgoing &message : out) {
		if (std::find(broken.begin(), broken.end(), message.key) != broken.end()) continue;
		bool isSent = false;
		try {
			if (environment.send) {
				environment.send(message.to_device, message.kind, message.value);
				isSent = true;
			}
		} catch (const std::exception &) {
		}
		if (isSent == true) continue;
		broken.push_back(message.key);
		const std::lock_guard<std::mutex> lock(held.mutex);
		const auto found = held.sessions.find(message.key);
		if (found != held.sessions.end()) fail(found->second, "Legal Machines could not be reached to pass the check on. Try again.");
	}
}

// Reads what came for this computer and sends what it calls for: the io lock
// held by the caller.
bool read_and_answer(State &held, const Environment &environment, const std::string &me, std::vector<Outgoing> &out) {
	const std::vector<Message> came = fetch_inbox(environment);
	bool changed = false;
	{
		const std::lock_guard<std::mutex> lock(held.mutex);
		changed = take_locked(held, me, came, out);
	}
	send_all(held, environment, out);
	return changed;
}

void tell(const Environment &environment, bool changed) {
	if (changed == false || !environment.changed) return;
	try {
		environment.changed();
	} catch (const std::exception &) {
	}
}

Check view_of(const std::string &device_id, const Session &session) {
	Check check;
	check.device_id = device_id;
	check.state = session.state;
	check.number = session.state == "ready" ? session.number : std::string();
	check.problem = session.problem;
	return check;
}

Check check_of(State &held, const std::string &key, const std::string &device_id) {
	const std::lock_guard<std::mutex> lock(held.mutex);
	const auto found = held.sessions.find(key);
	if (found == held.sessions.end()) {
		Check none;
		none.device_id = device_id;
		none.state = "none";
		return none;
	}
	return view_of(device_id, found->second);
}

// `listing` is the organisation's signed in to now (listing_of_now): a check
// made in another counts for none of its computers.
std::vector<View> views_of(const Environment &environment, const std::vector<Computer> &listing) {
	const std::string me = my_device(environment);
	const std::vector<Computer> kept = records_of(read_records(environment), organisation_of(environment), listing, true);
	std::vector<View> views;
	for (const Computer &computer : listing) {
		View view;
		view.seat_id = computer.seat_id;
		view.role = computer.role;
		view.device_id = computer.device_id;
		view.code = environment.code_of ? environment.code_of(computer.device_key) : std::string();
		view.announced_at = computer.announced_at;
		if (computer.device_id == me) view.state = "this";
		else if (!is_signed(environment, computer)) view.state = "unsigned";
		else if (environment.is_refused && environment.is_refused(computer.device_key)) view.state = "refused";
		else if (environment.is_confirmed && environment.is_confirmed(actor_of(environment, computer.seat_id), computer.device_key)) {
			// Checked with these very keys; other keys listed under the same
			// computer are a new one to check.
			const bool isSame = std::any_of(kept.begin(), kept.end(), [&computer](const Computer &record) {
				return record.device_id == computer.device_id && record.device_key == computer.device_key
					&& record.coordination_key == computer.coordination_key;
			});
			view.state = isSame ? "checked" : "new";
		} else view.state = "new";
		views.push_back(std::move(view));
	}
	return views;
}

std::string said_of(const Computer &self) {
	std::string memo = self.seat_id + "\n" + self.device_id + "\n" + self.device_key + "\n" + self.coordination_key;
	for (const std::string &address : self.addresses) memo += "\n" + address;
	return memo;
}

// This computer as it says itself; nothing without a seat or a key.
std::optional<Computer> self_of(const Environment &environment) {
	if (!environment.seat_id || !environment.device_key || !environment.coordination_key || !environment.sign) return std::nullopt;
	Computer self;
	self.seat_id = environment.seat_id();
	if (self.seat_id.empty()) return std::nullopt;
	self.device_id = my_device(environment);
	self.device_key = environment.device_key();
	self.coordination_key = environment.coordination_key();
	if (self.device_id.empty() || self.device_key.empty() || self.coordination_key.empty()) return std::nullopt;
	if (environment.candidates) self.addresses = environment.candidates();
	return self;
}

void announce_with(State &held, const Environment &environment, const Computer &self) {
	Computer signed_self = self;
	signed_self.signature = environment.sign(announcement_message(self.device_id, self.coordination_key));
	environment.announce(signed_self);
	const std::lock_guard<std::mutex> lock(held.mutex);
	held.said = said_of(self);
	held.said_at = Clock::now();
}

} // namespace

void install_environment(Environment environment) {
	State &held = state();
	const std::lock_guard<std::mutex> lock(held.mutex);
	held.environment = std::move(environment);
	// Another computer's environment lists the firm afresh and says itself again.
	held.listing.clear();
	held.is_listing_read = false;
	held.listed_at = {};
	held.listing_organisation.clear();
	held.said.clear();
	held.said_at = {};
}

std::string announcement_message(const std::string &device_id, const std::string &coordination_key) {
	return "singularity-computer-v1\n" + device_id + "\n" + coordination_key;
}

void announce() {
	State &held = state();
	const std::lock_guard<std::mutex> io(held.io);
	const Environment environment = environment_now(held);
	if (!environment.announce) return;
	const std::optional<Computer> self = self_of(environment);
	if (self.has_value() == false) return;
	announce_with(held, environment, *self);
}

std::vector<View> computers() {
	State &held = state();
	const std::lock_guard<std::mutex> io(held.io);
	const Environment environment = environment_now(held);
	if (is_listing_stale(held, kListingFresh)) refresh_listing(held, environment);
	return views_of(environment, listing_of_now(held, environment));
}

std::vector<View> computers_as_known() {
	State &held = state();
	const Environment environment = environment_now(held);
	return views_of(environment, listing_of_now(held, environment));
}

Check start_check(const std::string &device_id) {
	State &held = state();
	const std::lock_guard<std::mutex> io(held.io);
	const Environment environment = environment_now(held);
	const std::string me = my_device(environment);
	const std::string key = key_of(me, device_id);
	bool isBegun = false;
	{
		const std::lock_guard<std::mutex> lock(held.mutex);
		const auto found = held.sessions.find(key);
		isBegun = found != held.sessions.end() && found->second.state != "failed";
	}
	std::vector<Outgoing> out;
	if (isBegun == false) {
		refresh_listing(held, environment);
		std::optional<Computer> peer;
		{
			const std::lock_guard<std::mutex> lock(held.mutex);
			for (const Computer &computer : held.listing) {
				if (computer.device_id == device_id) peer = computer;
			}
		}
		Session session;
		if (device_id == me) fail(session, "That is this computer.");
		else if (peer.has_value() == false) fail(session, "That computer is not one of the firm's any more.");
		else if (is_signed(environment, *peer) == false) fail(session, "That computer's keys are not signed as its own, so it can't be checked.");
		if (session.state != "failed") {
			session.peer = *peer;
			session.my_key = environment.device_key ? environment.device_key() : std::string();
			session.my_coordination = environment.coordination_key ? environment.coordination_key() : std::string();
			session.my_seat = environment.seat_id ? environment.seat_id() : std::string();
			if (session.my_key.empty() || session.my_coordination.empty() || session.my_seat.empty()) {
				fail(session, "Sign in to your seat of the firm first.");
			}
		}
		if (session.state != "failed") session.my_nonce = environment.nonce();
		const std::lock_guard<std::mutex> lock(held.mutex);
		if (session.state != "failed") {
			// A commitment it sent before this side began counts for this check.
			const auto waiting = held.waiting_commitments.find(key);
			if (waiting != held.waiting_commitments.end()) {
				session.peer_commitment = waiting->second;
				held.waiting_commitments.erase(waiting);
			}
			out.push_back(Outgoing{key, device_id, "commit",
				commitment_of(session.my_key, session.my_coordination, session.my_seat, session.my_nonce)});
			session.is_committed = true;
			advance_locked(session, key, out);
		}
		held.sessions.insert_or_assign(key, session);
	}
	send_all(held, environment, out);
	out.clear();
	tell(environment, read_and_answer(held, environment, me, out));
	return check_of(held, key, device_id);
}

Check check_state(const std::string &device_id) {
	State &held = state();
	const std::lock_guard<std::mutex> io(held.io);
	const Environment environment = environment_now(held);
	const std::string me = my_device(environment);
	std::vector<Outgoing> out;
	tell(environment, read_and_answer(held, environment, me, out));
	return check_of(held, key_of(me, device_id), device_id);
}

std::vector<std::string> asking() {
	State &held = state();
	const std::lock_guard<std::mutex> lock(held.mutex);
	const std::string mine = key_of(my_device(held.environment), "");
	std::vector<std::string> devices;
	for (const auto &[key, commitment] : held.waiting_commitments) {
		static_cast<void>(commitment);
		if (key.rfind(mine, 0) == 0) devices.push_back(key.substr(mine.size()));
	}
	return devices;
}

void match(const std::string &device_id) {
	State &held = state();
	Environment environment;
	Computer peer;
	{
		const std::lock_guard<std::mutex> lock(held.mutex);
		const auto found = held.sessions.find(key_of(my_device(held.environment), device_id));
		if (found == held.sessions.end() || found->second.state != "ready") {
			throw std::invalid_argument("Compare the number with the other computer first.");
		}
		environment = held.environment;
		peer = found->second.peer;
		held.sessions.erase(found);
	}
	if (environment.confirm) environment.confirm(actor_of(environment, peer.seat_id), peer.device_key);
	// Checked in the organisation signed in to: its computer for no
	// other.
	peer.organisation_id = organisation_of(environment);
	std::vector<Computer> kept = read_records(environment);
	kept.erase(std::remove_if(kept.begin(), kept.end(), [&peer](const Computer &record) { return record.device_id == peer.device_id; }),
		kept.end());
	kept.push_back(peer);
	write_records(environment, kept);
	tell(environment, true);
}

void refuse(const std::string &device_id) {
	State &held = state();
	Environment environment;
	std::optional<Computer> peer;
	{
		const std::lock_guard<std::mutex> lock(held.mutex);
		environment = held.environment;
		const std::string key = key_of(my_device(environment), device_id);
		const auto found = held.sessions.find(key);
		if (found != held.sessions.end() && found->second.peer.device_key.empty() == false) peer = found->second.peer;
		if (peer.has_value() == false) {
			for (const Computer &computer : held.listing) {
				if (computer.device_id == device_id) peer = computer;
			}
		}
		held.sessions.erase(key);
		held.waiting_commitments.erase(key);
	}
	if (peer.has_value() && environment.refuse) environment.refuse(actor_of(environment, peer->seat_id), peer->device_key);
	tell(environment, true);
}

void cancel(const std::string &device_id) {
	State &held = state();
	const std::lock_guard<std::mutex> lock(held.mutex);
	const std::string key = key_of(my_device(held.environment), device_id);
	held.sessions.erase(key);
	held.waiting_commitments.erase(key);
}

std::vector<Computer> checked_records() {
	State &held = state();
	const Environment environment = environment_now(held);
	const std::vector<Computer> listing = listing_of_now(held, environment);
	return records_of(read_records(environment), organisation_of(environment), listing, listing.empty() == false);
}

std::vector<Computer> checked() {
	State &held = state();
	const Environment environment = environment_now(held);
	const std::string organisation = organisation_of(environment);
	// Signed in to no organisation, no computer is the firm's.
	if (organisation.empty()) return {};
	bool isListed = false;
	{
		const std::lock_guard<std::mutex> lock(held.mutex);
		isListed = held.is_listing_read && held.listing_organisation == organisation;
	}
	const std::vector<Computer> listed = isListed ? listing_of_now(held, environment) : std::vector<Computer>();
	std::vector<Computer> kept = records_of(read_records(environment), organisation, listed, isListed);
	std::vector<Computer> found;
	for (Computer &record : kept) {
		if (environment.is_refused && environment.is_refused(record.device_key)) continue;
		if (environment.is_confirmed && !environment.is_confirmed(actor_of(environment, record.seat_id), record.device_key)) continue;
		if (isListed) {
			const auto now = std::find_if(listed.begin(), listed.end(),
				[&record](const Computer &computer) { return computer.device_id == record.device_id; });
			// Its seat removed, or the computer itself: no more.
			if (now == listed.end()) continue;
			// Where it may be reached now; its keys stay those the check confirmed.
			if (now->device_key == record.device_key) record.addresses = now->addresses;
		}
		found.push_back(std::move(record));
	}
	return found;
}

void background_once() {
	State &held = state();
	const std::lock_guard<std::mutex> io(held.io);
	const Environment environment = environment_now(held);
	const std::optional<Computer> self = self_of(environment);
	if (self.has_value() == false) return;
	bool isDue = false;
	{
		const std::lock_guard<std::mutex> lock(held.mutex);
		isDue = held.said != said_of(*self) || Clock::now() - held.said_at >= kAnnounceEvery;
	}
	if (isDue == true && environment.announce) {
		try {
			announce_with(held, environment, *self);
		} catch (const std::exception &) {
			// Said again at the next pass.
		}
	}
	bool changed = false;
	if (is_listing_stale(held, kListingEvery)) {
		const std::vector<View> before = computers_as_known();
		refresh_listing(held, environment);
		name_kept_checks(held, environment);
		const std::vector<View> after = computers_as_known();
		changed = before.size() != after.size()
			|| !std::equal(before.begin(), before.end(), after.begin(), [](const View &a, const View &b) {
				return a.device_id == b.device_id && a.state == b.state;
			});
	}
	std::vector<Outgoing> out;
	changed = read_and_answer(held, environment, self->device_id, out) || changed;
	tell(environment, changed);
}

void start() {
	State &held = state();
	const std::lock_guard<std::mutex> lock(held.run_mutex);
	if (held.is_running == true) return;
	held.is_stopping = false;
	held.is_running = true;
	held.thread = std::thread([&held]() {
		for (;;) {
			try {
				background_once();
			} catch (const std::exception &) {
			}
			bool isChecking = false;
			{
				const std::lock_guard<std::mutex> lock(held.mutex);
				isChecking = std::any_of(held.sessions.begin(), held.sessions.end(),
					[](const auto &entry) { return entry.second.state == "waiting"; });
			}
			std::unique_lock<std::mutex> wait(held.run_mutex);
			held.woken.wait_for(wait, isChecking ? kCheckingEvery : kQuietEvery,
				[&held]() { return held.is_stopping || held.is_woken; });
			if (held.is_stopping == true) return;
			held.is_woken = false;
		}
	});
}

void stop() noexcept {
	State &held = state();
	{
		const std::lock_guard<std::mutex> lock(held.run_mutex);
		if (held.is_running == false) return;
		held.is_stopping = true;
	}
	held.woken.notify_all();
	if (held.thread.joinable()) held.thread.join();
	const std::lock_guard<std::mutex> lock(held.run_mutex);
	held.is_running = false;
}

void wake() noexcept {
	State &held = state();
	{
		const std::lock_guard<std::mutex> lock(held.run_mutex);
		held.is_woken = true;
	}
	held.woken.notify_all();
}

void reset_for_tests() {
	State &held = state();
	const std::lock_guard<std::mutex> lock(held.mutex);
	held.sessions.clear();
	held.waiting_commitments.clear();
	held.listing.clear();
	held.is_listing_read = false;
	held.listed_at = {};
	held.listing_organisation.clear();
	held.said.clear();
	held.said_at = {};
}

} // namespace singularity::matter::firm_computers
