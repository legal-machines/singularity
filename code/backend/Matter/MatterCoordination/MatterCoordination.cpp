#include "Matter/MatterCoordination/MatterCoordination.hpp"

#include "Agent/Types/AgentIdentity.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Events/Events.hpp"
#include "Platform/Trace/Trace.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <monocypher.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

namespace singularity::matter::coordination {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using transport::SealedBytes;

// The most one file can be, as the firm's envelopes carry it.
constexpr std::uintmax_t kMostFileBytes = 48ull * 1024 * 1024;
// A file this computer writes from another computer's envelope, until it is whole.
constexpr const char *kPartSuffix = ".lm-coordination-part";
// The same, as earlier builds named it: never carried either.
constexpr const char *kOldPartSuffix = ".lm-relay-part";
// This computer's own key pair for coordination, in the credential store.
constexpr const char *kComputerKeySecret = "lm-coordination-x25519";
// A computer that asked to join and holds no key yet asks again this often.
constexpr std::int64_t kAskAgainSeconds = 600;
// A computer tells each Matter's others where its direct link may be reached
// (0x04, D-124) at once when that changes, and again this often; a card holds
// for a day.
constexpr std::int64_t kCardAgainSeconds = 6 * 60 * 60;
constexpr std::int64_t kCardLastsSeconds = 24 * 60 * 60;
// A computer that gave a Matter's invitation takes requests to join it from
// computers no card names for this long after (D-127).
constexpr std::int64_t kLobbySeconds = 60 * 60;
// What is said where no carrier reaches the firm's other computers.
constexpr const char *kNoWay = "This computer cannot reach the firm's other computers: no folder of the firm is chosen, "
	"and its direct link could not open on this network.";

std::mutex &round_mutex() {
	static std::mutex mutex;
	return mutex;
}

Environment &installed() {
	static Environment environment;
	return environment;
}

std::int64_t nowSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string nowText() {
	return installed().now ? installed().now() : std::string();
}

std::int64_t clockMicros() {
	if (installed().clock_micros) return installed().clock_micros();
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Seconds since 1970 of a time as a card says it, "2026-10-25T18:00:00Z".
std::optional<std::int64_t> secondsOf(const std::string &timestamp) {
	if (timestamp.size() != 20 || timestamp[4] != '-' || timestamp[7] != '-' || timestamp[10] != 'T' || timestamp.back() != 'Z')
		return std::nullopt;
	std::tm utc {};
	std::istringstream input(timestamp.substr(0, 19));
	input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
	if (input.fail() == true) return std::nullopt;
#if defined(_WIN32)
	const std::time_t seconds = _mkgmtime(&utc);
#else
	const std::time_t seconds = timegm(&utc);
#endif
	if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
	return static_cast<std::int64_t>(seconds);
}

std::string timestampOf(std::int64_t seconds) {
	const std::time_t time = static_cast<std::time_t>(seconds);
	std::tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &time);
#else
	gmtime_r(&time, &utc);
#endif
	char text[32];
	std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return text;
}

// The name of the person at this computer, as Settings gives it, for a copy a
// colleague's computer keeps beside a document. Asking for it waits on the
// store's lock, which the window holds while it reads a Matter, so it is
// asked once a run and again only after a round, every five minutes
// (measured live, 2026-10-26: a message waited 250 ms behind the window's
// read before it went, and a Matter sent whole asked once a file).
struct PersonName {
	bool isRead = false;
	std::string name;
	std::chrono::steady_clock::time_point read_at {};
};

PersonName &personNameKept() {
	static PersonName *const kept = new PersonName();
	return *kept;
}

std::string personName() {
	PersonName &kept = personNameKept();
	if (kept.isRead == false) {
		kept.name = installed().person_name ? installed().person_name() : std::string();
		kept.isRead = true;
		kept.read_at = std::chrono::steady_clock::now();
	}
	return kept.name;
}

void askPersonNameAgainWhenOld() {
	PersonName &kept = personNameKept();
	if (kept.isRead == true && std::chrono::steady_clock::now() - kept.read_at >= std::chrono::minutes(5)) kept.isRead = false;
}

// A file's bytes, read in one go; empty when it cannot be read.
std::string readBytes(const fs::path &path) {
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input) return std::string();
	const std::streamoff size = input.tellg();
	if (size <= 0) return std::string();
	std::string bytes(static_cast<std::size_t>(size), '\0');
	input.seekg(0, std::ios::beg);
	input.read(bytes.data(), static_cast<std::streamsize>(size));
	bytes.resize(static_cast<std::size_t>(input.gcount()));
	return bytes;
}

SealedBytes bytesOf(const std::string &text) {
	return SealedBytes(text.begin(), text.end());
}

std::string textOf(const SealedBytes &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

// ---- Base64 (RFC 4648, padded), for names in an invitation ------------------------

constexpr const char *kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode64(const std::string &bytes) {
	std::string text;
	std::size_t at = 0;
	while (at + 2 < bytes.size()) {
		const unsigned value = (static_cast<unsigned char>(bytes[at]) << 16) | (static_cast<unsigned char>(bytes[at + 1]) << 8)
			| static_cast<unsigned char>(bytes[at + 2]);
		text += kAlphabet[(value >> 18) & 63];
		text += kAlphabet[(value >> 12) & 63];
		text += kAlphabet[(value >> 6) & 63];
		text += kAlphabet[value & 63];
		at += 3;
	}
	if (at < bytes.size()) {
		unsigned value = static_cast<unsigned char>(bytes[at]) << 16;
		if (at + 1 < bytes.size()) value |= static_cast<unsigned char>(bytes[at + 1]) << 8;
		text += kAlphabet[(value >> 18) & 63];
		text += kAlphabet[(value >> 12) & 63];
		text += at + 1 < bytes.size() ? kAlphabet[(value >> 6) & 63] : '=';
		text += '=';
	}
	return text;
}

std::optional<std::string> decode64(const std::string &text) {
	if (text.size() % 4 != 0) return std::nullopt;
	std::string bytes;
	for (std::size_t at = 0; at < text.size(); at += 4) {
		unsigned value = 0;
		int padding = 0;
		for (int index = 0; index < 4; ++index) {
			const char character = text[at + static_cast<std::size_t>(index)];
			if (character == '=' && at + 4 == text.size() && index >= 2) {
				++padding;
				value <<= 6;
				continue;
			}
			const char *found = character == '\0' ? nullptr : std::strchr(kAlphabet, character);
			if (found == nullptr || padding > 0) return std::nullopt;
			value = (value << 6) | static_cast<unsigned>(found - kAlphabet);
		}
		bytes += static_cast<char>((value >> 16) & 255);
		if (padding < 2) bytes += static_cast<char>((value >> 8) & 255);
		if (padding < 1) bytes += static_cast<char>(value & 255);
	}
	return bytes;
}

// ---- This computer's record ---------------------------------------------------------

// What a path last took, from whom: a change older than it is an earlier one.
// The name of whose change it is, for a copy kept beside the document; and for
// this computer's own change, the version it was made from, sent again with it.
struct PathMark {
	std::int64_t counter = 0;
	std::string sender;
	std::string name;
	std::optional<std::string> base;
};

bool operator==(const PathMark &left, const PathMark &right) {
	return left.counter == right.counter && left.sender == right.sender && left.name == right.name && left.base == right.base;
}

struct Linked {
	LinkedMatter view;
	std::vector<std::string> key_ids;
	std::vector<std::string> snapshot_for;
	std::int64_t asked_at = 0;
	std::map<std::string, PathMark> marks;
	// By device, the device keys of the computers given the Matter's key here
	// and of those whose key this computer's lawyer accepted: what their address
	// cards are checked against.
	std::map<std::string, std::string> given_device_keys;
	std::map<std::string, std::string> accepted_devices;
	// The newest address card taken from each of the Matter's computers, by its
	// counter: an older one, played back, teaches nothing.
	std::map<std::string, std::int64_t> card_counters;
	// And that card itself, by device: its X25519 key and addresses, public
	// halves only. The direct link keeps its computers only while the program
	// runs; after a restart they are taught to it again from here.
	std::map<std::string, seal::AddressCard> cards;
	// The computer whose invitation this one joined by, and where its direct
	// link may be reached (LMM2, D-127): called until the Matter's key comes.
	std::string inviter_device;
	std::string inviter_key;
	std::vector<std::string> inviter_candidates;
	// When this computer last gave the Matter's invitation (seconds since
	// 1970): its lobby stays open an hour after.
	std::int64_t invited_at = 0;
	// A personal channel's two members, by actor id, sorted (D-128); empty for
	// a Matter.
	std::vector<std::string> pair;
	// For a channel with an AI colleague: the computer that runs it, as first
	// verified here, by device and device key. No other is taken for it.
	std::string agent_host;
	std::string agent_host_key;
	// The members of the firm this computer's lawyer offered the Matter to
	// (D-205), by actor id, with when (seconds since 1970): their computers a
	// lawyer here checked are given its key as they ask.
	std::map<std::string, std::int64_t> offered_to;
	// The firm's record only (D-210): what of the firm's model accounts this
	// computer gave each computer - by device and account, a fingerprint of
	// what was given, never the key - and the accounts other computers gave
	// this one, by account, with the computer that gave it: never given on.
	std::map<std::string, std::string> accounts_given;
	std::map<std::string, std::string> accounts_taken;
};

struct Records {
	fs::path firm_folder;
	std::vector<Linked> matters;
};

std::vector<std::string> strings(const Json &value) {
	std::vector<std::string> list;
	if (value.is_array() == false) return list;
	for (const Json &item : value) {
		if (item.is_string() == true) list.push_back(item.get<std::string>());
	}
	return list;
}

// The records as this program last read or wrote them, and their bytes then:
// the file is read every round, but parsed again only when another program
// changed it, and written only when something in them changed - a round's
// time alone at most every half minute (measured live on 2026-10-26: parsing
// them took most of a round on a Matter of 2,000 files).
struct RecordsCache {
	bool valid = false;
	fs::path path;
	std::string bytes;
	std::string kept;
	std::chrono::steady_clock::time_point written_at {};
};
RecordsCache &recordsCache() {
	static RecordsCache *const cache = new RecordsCache();
	return *cache;
}
Records &cachedRecords() {
	static Records *const records = new Records();
	return *records;
}

Records parseRecords(const std::string &bytes) {
	Records records;
	if (bytes.empty() == true) return records;
	const Json value = Json::parse(bytes, nullptr, false);
	// A record of the relay that carried Matters before D-123 is not read: its
	// Matters are shared again.
	if (value.is_object() == false || value.value("version", 0) != 2) return records;
	records.firm_folder = fs::u8path(value.value("firmFolder", std::string()));
	for (const Json &entry : value.value("matters", Json::array())) {
		Linked linked;
		LinkedMatter &link = linked.view;
		link.matter_id = entry.value("matterId", std::string());
		link.name = entry.value("name", std::string());
		link.folder = fs::u8path(entry.value("folder", std::string()));
		link.state = entry.value("state", std::string("waiting"));
		link.key_id = entry.value("keyId", std::string());
		link.sent_counter = entry.value("sentCounter", static_cast<std::int64_t>(0));
		link.last_sync_at = entry.value("lastSyncAt", std::string());
		link.last_error = entry.value("lastError", std::string());
		// Held by name: a loop over items() of a temporary reads a value already gone.
		const Json known = entry.value("known", Json::object());
		for (const auto &[path, sha] : known.items()) {
			if (sha.is_string() == true) link.known[path] = sha.get<std::string>();
		}
		for (const Json &asking : entry.value("asking", Json::array())) {
			link.asking.push_back(AskingComputer{asking.value("actorId", std::string()), asking.value("deviceId", std::string()),
				asking.value("deviceKey", std::string()), asking.value("coordinationKey", std::string()),
				asking.value("code", std::string()), asking.value("at", std::string())});
		}
		for (const Json &offering : entry.value("offering", Json::array())) {
			link.offering.push_back(OfferingComputer{offering.value("deviceId", std::string()),
				offering.value("deviceKey", std::string()), offering.value("code", std::string()),
				offering.value("files", static_cast<std::uint64_t>(0)), offering.value("bytes", static_cast<std::uint64_t>(0))});
		}
		link.accepted_senders = strings(entry.value("acceptedSenders", Json::array()));
		const Json given = entry.value("given", Json::object());
		for (const auto &[device, key] : given.items()) {
			if (key.is_string() == true) link.given[device] = key.get<std::string>();
		}
		linked.key_ids = strings(entry.value("keyIds", Json::array()));
		linked.snapshot_for = strings(entry.value("snapshotFor", Json::array()));
		const auto keysOf = [&entry](const char *name, std::map<std::string, std::string> &devices) {
			const Json keys = entry.value(name, Json::object());
			for (const auto &[device, key] : keys.items()) {
				if (key.is_string() == true) devices[device] = key.get<std::string>();
			}
		};
		keysOf("givenDeviceKeys", linked.given_device_keys);
		keysOf("acceptedDevices", linked.accepted_devices);
		const Json counters = entry.value("cardCounters", Json::object());
		for (const auto &[device, counter] : counters.items()) {
			if (counter.is_number_integer() == true) linked.card_counters[device] = counter.get<std::int64_t>();
		}
		const Json cards = entry.value("cards", Json::object());
		for (const auto &[device, card] : cards.items()) {
			if (card.is_object() == false) continue;
			seal::AddressCard kept;
			kept.matter_id = link.matter_id;
			kept.device_id = device;
			kept.coordination_key = card.value("x", std::string());
			kept.candidates = strings(card.value("c", Json::array()));
			kept.expires_at = card.value("exp", std::string());
			kept.counter = card.value("n", static_cast<std::int64_t>(0));
			if (kept.coordination_key.empty() == false) linked.cards[device] = std::move(kept);
		}
		const Json inviter = entry.value("inviter", Json::object());
		linked.inviter_device = inviter.value("deviceId", std::string());
		linked.inviter_key = inviter.value("coordinationKey", std::string());
		linked.inviter_candidates = strings(inviter.value("candidates", Json::array()));
		linked.invited_at = entry.value("invitedAt", static_cast<std::int64_t>(0));
		linked.asked_at = entry.value("askedAt", static_cast<std::int64_t>(0));
		linked.pair = strings(entry.value("pair", Json::array()));
		const std::vector<std::string> host = strings(entry.value("agentHost", Json::array()));
		if (host.size() == 2) {
			linked.agent_host = host[0];
			linked.agent_host_key = host[1];
		}
		const Json offered = entry.value("offeredTo", Json::object());
		for (const auto &[actor, at] : offered.items()) {
			if (at.is_number_integer() == true) linked.offered_to[actor] = at.get<std::int64_t>();
		}
		const auto textsOf = [&entry](const char *name, std::map<std::string, std::string> &into) {
			const Json kept = entry.value(name, Json::object());
			for (const auto &[key, value] : kept.items()) {
				if (value.is_string() == true) into[key] = value.get<std::string>();
			}
		};
		textsOf("accountsGiven", linked.accounts_given);
		textsOf("accountsTaken", linked.accounts_taken);
		const Json marks = entry.value("marks", Json::object());
		for (const auto &[path, mark] : marks.items()) {
			if (mark.is_array() == false || mark.size() < 2 || mark[0].is_number_integer() == false || mark[1].is_string() == false) continue;
			PathMark kept{mark[0].get<std::int64_t>(), mark[1].get<std::string>(), std::string(), std::nullopt};
			if (mark.size() > 2 && mark[2].is_string() == true) kept.name = mark[2].get<std::string>();
			if (mark.size() > 3 && mark[3].is_string() == true) kept.base = mark[3].get<std::string>();
			linked.marks[path] = std::move(kept);
		}
		if (link.matter_id.empty() == false) records.matters.push_back(std::move(linked));
	}
	return records;
}

Records load() {
	RecordsCache &cache = recordsCache();
	const fs::path &path = installed().records;
	std::string bytes = readBytes(path);
	if (cache.valid == true && cache.path == path && cache.bytes == bytes) return cachedRecords();
	Records records = parseRecords(bytes);
	cache.valid = true;
	cache.path = path;
	cache.bytes = std::move(bytes);
	cache.kept.clear();
	cachedRecords() = records;
	return records;
}

// A Matter's record but for its two large maps - what the folder holds and
// what each path last took - and the time of its last round: what save()
// compares as text; the maps it compares as they are.
Json recordWithoutLargeMaps(const Linked &linked) {
	const LinkedMatter &link = linked.view;
	Json asking = Json::array();
	for (const AskingComputer &computer : link.asking) {
		asking.push_back(Json{{"actorId", computer.actor_id}, {"deviceId", computer.device_id}, {"deviceKey", computer.device_key},
			{"coordinationKey", computer.coordination_key}, {"code", computer.code}, {"at", computer.at}});
	}
	Json offering = Json::array();
	for (const OfferingComputer &computer : link.offering) {
		offering.push_back(Json{{"deviceId", computer.device_id}, {"deviceKey", computer.device_key}, {"code", computer.code},
			{"files", computer.files}, {"bytes", computer.bytes}});
	}
	Json cards = Json::object();
	for (const auto &[device, card] : linked.cards) {
		cards[device] = Json{{"x", card.coordination_key}, {"c", card.candidates}, {"exp", card.expires_at}, {"n", card.counter}};
	}
	Json record = Json{{"matterId", link.matter_id}, {"name", link.name}, {"folder", link.folder.u8string()},
		{"state", link.state}, {"keyId", link.key_id}, {"keyIds", linked.key_ids},
		{"sentCounter", link.sent_counter}, {"asking", asking}, {"offering", offering},
		{"acceptedSenders", link.accepted_senders}, {"given", link.given}, {"snapshotFor", linked.snapshot_for},
		{"givenDeviceKeys", linked.given_device_keys}, {"acceptedDevices", linked.accepted_devices},
		{"cardCounters", linked.card_counters}, {"cards", cards}, {"invitedAt", linked.invited_at},
		{"inviter", Json{{"deviceId", linked.inviter_device}, {"coordinationKey", linked.inviter_key},
			{"candidates", linked.inviter_candidates}}},
		{"askedAt", linked.asked_at}, {"lastError", link.last_error}};
	if (linked.pair.empty() == false) record["pair"] = linked.pair;
	if (linked.agent_host.empty() == false) record["agentHost"] = Json::array({linked.agent_host, linked.agent_host_key});
	if (linked.offered_to.empty() == false) record["offeredTo"] = linked.offered_to;
	if (linked.accounts_given.empty() == false) record["accountsGiven"] = linked.accounts_given;
	if (linked.accounts_taken.empty() == false) record["accountsTaken"] = linked.accounts_taken;
	return record;
}

// Whether two records' large maps are the same, Matter by Matter (the rest
// was compared as text, so the Matters stand in the same order).
bool sameLargeMaps(const Records &left, const Records &right) {
	if (left.matters.size() != right.matters.size()) return false;
	for (std::size_t index = 0; index < left.matters.size(); ++index) {
		if (left.matters[index].view.known != right.matters[index].view.known) return false;
		if (left.matters[index].marks != right.matters[index].marks) return false;
	}
	return true;
}

void save(const Records &records) {
	Json matters = Json::array();
	for (const Linked &linked : records.matters) matters.push_back(recordWithoutLargeMaps(linked));
	Json value = Json{{"format", "singularity-coordination"}, {"version", 2},
		{"firmFolder", records.firm_folder.u8string()}, {"matters", std::move(matters)}};
	const fs::path path = installed().records;
	RecordsCache &cache = recordsCache();
	// Unchanged but for the rounds' time: nothing is written, the time at
	// most every half minute.
	const std::string kept = value.dump();
	const auto now = std::chrono::steady_clock::now();
	if (cache.valid == true && cache.path == path && cache.kept == kept && now - cache.written_at < std::chrono::seconds(30)
		&& sameLargeMaps(records, cachedRecords()) == true) {
		cachedRecords() = records;
		return;
	}
	std::size_t index = 0;
	for (Json &matter : value["matters"]) {
		const Linked &linked = records.matters[index++];
		Json marks = Json::object();
		for (const auto &[place, mark] : linked.marks) {
			Json entry = Json::array({mark.counter, mark.sender, mark.name});
			if (mark.base.has_value() == true) entry.push_back(*mark.base);
			marks[place] = std::move(entry);
		}
		matter["known"] = linked.view.known;
		matter["marks"] = std::move(marks);
		matter["lastSyncAt"] = linked.view.last_sync_at;
	}
	std::error_code error;
	fs::create_directories(path.parent_path(), error);
	const fs::path staged = fs::u8path(path.u8string() + kPartSuffix);
	std::string bytes = value.dump(1);
	try {
		platform::write_file_durably(staged, bytes);
		if (platform::replace_file(staged, path).has_value() == true) {
			fs::remove(staged, error);
			cache.valid = false;
			return;
		}
	} catch (const std::exception &) {
		fs::remove(staged, error);
		cache.valid = false;
		return;
	}
	cache.valid = true;
	cache.path = path;
	cache.bytes = std::move(bytes);
	cache.kept = kept;
	cache.written_at = now;
	cachedRecords() = records;
}

Linked *find(Records &records, const std::string &matter_id) {
	for (Linked &linked : records.matters) {
		if (linked.view.matter_id == matter_id) return &linked;
	}
	return nullptr;
}

// ---- Keys -----------------------------------------------------------------------------

// The credential store's name for one of a Matter's keys.
std::string matterKeySecret(const std::string &matter_id, const std::string &key_id) {
	std::string name = "lm-matter-key-";
	for (const char character : matter_id + "-" + key_id) {
		const auto letter = static_cast<unsigned char>(character);
		name += std::isalnum(letter) != 0 || character == '_' || character == '-' ? character : '_';
	}
	return name;
}

std::optional<seal::MatterKey> matterKey(const std::string &matter_id, const std::string &key_id) {
	if (key_id.empty() == true || !installed().secret) return std::nullopt;
	std::optional<std::string> text;
	try {
		text = installed().secret(matterKeySecret(matter_id, key_id));
	} catch (const std::exception &) {
		return std::nullopt;
	}
	if (text.has_value() == false) return std::nullopt;
	const auto bytes = seal::key_from_text(*text);
	if (bytes.has_value() == false) return std::nullopt;
	seal::MatterKey key;
	key.bytes = *bytes;
	if (seal::matter_key_id(key) != key_id) return std::nullopt;
	return key;
}

void keepMatterKey(Linked &linked, const seal::MatterKey &key) {
	const std::string id = seal::matter_key_id(key);
	installed().keep_secret(matterKeySecret(linked.view.matter_id, id), seal::key_text(key.bytes));
	if (std::find(linked.key_ids.begin(), linked.key_ids.end(), id) == linked.key_ids.end()) linked.key_ids.push_back(id);
	linked.view.key_id = id;
	linked.view.state = "carried";
}

// This computer's own keys are made once, whoever asks first: the round, or
// the firm's computers saying this one (D-205), which never waits for a round.
std::mutex &computerKeysMutex() {
	static std::mutex *const held = new std::mutex();
	return *held;
}

seal::ComputerKeys computerKeys() {
	const std::lock_guard<std::mutex> lock(computerKeysMutex());
	std::optional<std::string> text;
	try {
		text = installed().secret(kComputerKeySecret);
	} catch (const std::exception &) {
	}
	if (text.has_value() == true) {
		const auto secret = seal::key_from_text(*text);
		if (secret.has_value() == true) return seal::computer_keys_from_secret(*secret);
	}
	const seal::ComputerKeys made = seal::make_computer_keys();
	installed().keep_secret(kComputerKeySecret, seal::key_text(made.secret));
	return made;
}

// ---- A change's own words -------------------------------------------------------------

struct ChangeBody {
	std::string path;
	bool deleted = false;
	std::string sha256;
	std::string modified_at;
	// Whose change it is, for a copy kept beside a document: inside the seal.
	std::string from_name;
	// The SHA-256 of the version the change was made from, as its computer
	// last agreed it with the others; empty for a new file. None from a
	// computer that does not say it.
	std::optional<std::string> base_sha256;
	std::string content;
};

std::string changePayload(const ChangeBody &body) {
	Json fields = Json{{"path", body.path}, {"deleted", body.deleted}, {"sha256", body.sha256},
		{"modifiedAt", body.modified_at}, {"from", body.from_name}};
	if (body.base_sha256.has_value() == true) fields["base"] = *body.base_sha256;
	const std::string head = fields.dump();
	std::string payload;
	const auto length = static_cast<std::uint32_t>(head.size());
	payload.push_back(static_cast<char>((length >> 24) & 255u));
	payload.push_back(static_cast<char>((length >> 16) & 255u));
	payload.push_back(static_cast<char>((length >> 8) & 255u));
	payload.push_back(static_cast<char>(length & 255u));
	payload += head;
	payload += body.content;
	return payload;
}

std::optional<ChangeBody> changeBody(const std::string &payload) {
	if (payload.size() < 4) return std::nullopt;
	const std::size_t length = (static_cast<std::size_t>(static_cast<unsigned char>(payload[0])) << 24)
		| (static_cast<std::size_t>(static_cast<unsigned char>(payload[1])) << 16)
		| (static_cast<std::size_t>(static_cast<unsigned char>(payload[2])) << 8) | static_cast<unsigned char>(payload[3]);
	if (length > payload.size() - 4) return std::nullopt;
	const Json head = Json::parse(payload.substr(4, length), nullptr, false);
	if (head.is_object() == false) return std::nullopt;
	ChangeBody body;
	body.path = head.value("path", std::string());
	body.deleted = head.value("deleted", false);
	body.sha256 = head.value("sha256", std::string());
	body.modified_at = head.value("modifiedAt", std::string());
	body.from_name = head.value("from", std::string());
	if (head.contains("base") == true && head["base"].is_string() == true) body.base_sha256 = head["base"].get<std::string>();
	body.content = payload.substr(4 + length);
	return body;
}

// The counter an envelope is sealed with: above the last one this computer
// sealed for the Matter, and above the clock, so a computer whose record was
// lost still counts on from where it was, and the firm's computers' counters
// stand roughly in the order things happened. Taking a change raises it above
// that change's (takeChange), so what a computer changes after seeing another
// computer's change counts after it, whatever the two clocks say.
std::int64_t nextCounter(LinkedMatter &link) {
	const std::int64_t clock = clockMicros();
	link.sent_counter = std::max(link.sent_counter + 1, clock);
	return link.sent_counter;
}

void sendEverywhere(const std::vector<std::shared_ptr<transport::Transport>> &transports, const std::string &matter_id,
		const std::string &envelope, const std::vector<std::string> &to_devices, std::string &error) {
	if (transports.empty() == true) {
		error = kNoWay;
		return;
	}
	bool isSent = false;
	std::string failed;
	for (const auto &carrier : transports) {
		try {
			carrier->send(matter_id, bytesOf(envelope), to_devices);
			isSent = true;
		} catch (const std::exception &failure) {
			failed = "The firm's " + carrier->name() + " could not take what this computer sent: " + failure.what();
		}
	}
	if (isSent == false) error = failed;
}

// ---- A Matter's folder ------------------------------------------------------------------

std::string readFile(const fs::path &path) {
	return readBytes(path);
}

// A file written whole or not at all, over the one it replaces: a reader never
// sees it half written (Platform/AtomicFile).
bool writeWhole(const fs::path &path, const std::string &bytes) {
	std::error_code error;
	fs::create_directories(path.parent_path(), error);
	const fs::path staged = fs::u8path(path.u8string() + kPartSuffix);
	try {
		platform::write_file_durably(staged, bytes);
	} catch (const std::exception &) {
		fs::remove(staged, error);
		return false;
	}
	if (platform::replace_file(staged, path).has_value() == true) {
		fs::remove(staged, error);
		return false;
	}
	return true;
}

// A path inside the folder: forward slashes, no way out of it.
std::optional<fs::path> inside(const fs::path &folder, const std::string &path) {
	if (path.empty() == true || path.size() > 1024 || path.find('\\') != std::string::npos || path.front() == '/'
			|| path.find(':') != std::string::npos) {
		return std::nullopt;
	}
	std::stringstream parts(path);
	std::string part;
	while (std::getline(parts, part, '/')) {
		if (part.empty() == true || part == "." || part == "..") return std::nullopt;
	}
	return folder / fs::u8path(path);
}

// The SHA-256 of each file, read again only when its size or time changed.
struct Seen {
	std::uintmax_t size = 0;
	fs::file_time_type time {};
	std::string sha;
};
std::map<std::string, std::map<std::string, Seen>> &seen_files() {
	static std::map<std::string, std::map<std::string, Seen>> seen;
	return seen;
}

std::string shaOf(const std::string &matterId, const std::string &relative, const fs::path &file, std::uintmax_t size,
		fs::file_time_type time) {
	Seen &entry = seen_files()[matterId][relative];
	if (entry.sha.empty() == false && entry.size == size && entry.time == time) return entry.sha;
	entry.sha = platform::sha256_hex_digest_of_file(file);
	entry.size = size;
	entry.time = time;
	return entry.sha;
}

std::string shaOf(const std::string &matterId, const std::string &relative, const fs::path &file) {
	std::error_code error;
	const std::uintmax_t size = fs::file_size(file, error);
	const fs::file_time_type time = fs::last_write_time(file, error);
	return shaOf(matterId, relative, file, size, time);
}

// A version of a document two computers changed, kept beside it and named for
// whose it is - "Supply Agreement (Gabriel Moore's copy).docx" -, never over
// anything. The same version kept there already - the other computer keeps it
// too, and sends it - is not kept twice.
void keepBeside(const fs::path &file, const std::string &whose, const std::string &content) {
	const std::string owner = whose.empty() == true ? std::string("another computer's") : whose + "'s";
	const std::string sha = platform::sha256_hex_digest(content);
	for (int number = 1; number < 1000; ++number) {
		const std::string count = number == 1 ? std::string() : " " + std::to_string(number);
		const fs::path copy = file.parent_path()
			/ fs::u8path(file.stem().u8string() + " (" + owner + " copy" + count + ")" + file.extension().u8string());
		std::error_code error;
		if (fs::exists(copy, error) == false) {
			(void)writeWhole(copy, content);
			return;
		}
		if (platform::sha256_hex_digest_of_file(copy) == sha) return;
	}
}

// Every file of the folder that travels, by its path; a file larger than the
// firm's envelopes carry is named in `tooLarge`, never left out unsaid.
// `isWhole` is false when the folder could not be read to its end: such a
// listing is never taken to mean a file was deleted.
struct Listing {
	std::map<std::string, fs::path> files;
	// Each file's size and time, as the walk found them: on Windows the walk
	// has them already, elsewhere one stat, never a call by path per question.
	std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>> facts;
	bool isWhole = true;
};

Listing travelling(const LinkedMatter &link, std::vector<std::string> *tooLarge = nullptr) {
	Listing listing;
	const fs::path::string_type folderNative = link.folder.native();
	std::error_code walking;
	fs::recursive_directory_iterator walk(link.folder, fs::directory_options::skip_permission_denied, walking);
	if (walking) {
		listing.isWhole = false;
		return listing;
	}
	for (const fs::recursive_directory_iterator end; walk != end; walk.increment(walking)) {
		if (walking) {
			listing.isWhole = false;
			break;
		}
		std::error_code entryError;
		if (walk->is_regular_file(entryError) == false || entryError) continue;
		// The walk's paths begin with the folder's own: the rest is the place in
		// it, with no comparison of path parts for each file.
		const fs::path::string_type &native = walk->path().native();
		std::string relative;
		if (native.size() > folderNative.size() && native.compare(0, folderNative.size(), folderNative) == 0) {
			std::size_t start = folderNative.size();
			while (start < native.size() && (native[start] == '/' || native[start] == fs::path::preferred_separator)) ++start;
			relative = fs::path(native.substr(start)).generic_u8string();
		} else {
			relative = walk->path().lexically_relative(link.folder).generic_u8string();
		}
		if (relative.empty() == true || relative.rfind("..", 0) == 0 || travels(relative) == false) continue;
#if defined(_LIBCPP_VERSION)
		// libc++'s walk knows only the type: one stat for the size and time,
		// not one for each. (Windows has both from the walk already; libstdc++
		// would not keep them.)
		fs::directory_entry entry = *walk;
		entry.refresh(entryError);
		if (entryError) continue;
#else
		const fs::directory_entry &entry = *walk;
#endif
		const std::uintmax_t size = entry.file_size(entryError);
		if (entryError) continue;
		if (size > kMostFileBytes) {
			if (tooLarge != nullptr) tooLarge->push_back(walk->path().filename().u8string());
			continue;
		}
		const fs::file_time_type time = entry.last_write_time(entryError);
		if (entryError) continue;
		listing.files[relative] = walk->path();
		listing.facts[relative] = {size, time};
	}
	return listing;
}

// A listed file's SHA-256, by the size and time the walk found.
std::string shaListed(const std::string &matterId, const Listing &listing, const std::string &relative, const fs::path &file) {
	const auto facts = listing.facts.find(relative);
	if (facts == listing.facts.end()) return shaOf(matterId, relative, file);
	return shaOf(matterId, relative, file, facts->second.first, facts->second.second);
}

// ---- What was taken ------------------------------------------------------------------------
//
// Every envelope this computer has taken, by its id, kept apart from the record
// so the record is not rewritten with them: one that comes again - by another
// carrier, or from a carrier that lost what it knew - is not taken twice.

std::map<std::string, std::set<std::string>> &taken_ids() {
	static std::map<std::string, std::set<std::string>> taken;
	return taken;
}

// When this computer last took, from each other computer of a Matter, a sealed
// envelope that opened under the Matter's key (D-137): by this computer and
// the Matter, then by device. For as long as the program runs, under the
// round's lock; never written to the record. An envelope that did not open,
// one for another computer and one taken before never count.
std::map<std::string, std::map<std::string, std::string>> &heardFrom() {
	static std::map<std::string, std::map<std::string, std::string>> *const heard =
		new std::map<std::string, std::map<std::string, std::string>>();
	return *heard;
}

void hear(const std::string &me, const std::string &matter_id, const std::string &device) {
	if (device.empty() == false && device != me) heardFrom()[me + "\n" + matter_id][device] = nowText();
}

// What each other computer last said it holds of a Matter (0x05): by this
// computer and the Matter, then by device, each file's SHA-256, size and the
// counter of the change it is. For as long as the program runs, under the
// round's lock; never written to the record. What of it this computer holds
// is how far the Matter has come (D-207).
struct HeldThere {
	std::string sha256;
	std::uint64_t size = 0;
	std::int64_t counter = 0;
};

std::map<std::string, std::map<std::string, std::map<std::string, HeldThere>>> &heldThere() {
	static auto *const held = new std::map<std::string, std::map<std::string, std::map<std::string, HeldThere>>>();
	return *held;
}

fs::path takenFile(const std::string &matter_id) {
	std::string name;
	for (const char character : matter_id) {
		name += std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '-' ? character : '_';
	}
	return installed().records.parent_path() / "Coordination Taken" / fs::u8path(name + ".ids");
}

std::set<std::string> &takenFor(const std::string &matter_id) {
	const std::string key = takenFile(matter_id).u8string();
	auto found = taken_ids().find(key);
	if (found != taken_ids().end()) return found->second;
	std::set<std::string> &ids = taken_ids()[key];
	std::ifstream input(takenFile(matter_id));
	std::string line;
	while (std::getline(input, line)) {
		if (line.empty() == false) ids.insert(line);
	}
	return ids;
}

// 64 bits of the envelope's id: enough to tell a million envelopes apart.
std::string shortId(const std::string &envelope_id) {
	return envelope_id.substr(0, 16);
}

void remember(const std::string &matter_id, const std::string &envelope_id) {
	std::set<std::string> &ids = takenFor(matter_id);
	if (ids.insert(shortId(envelope_id)).second == false) return;
	const fs::path file = takenFile(matter_id);
	std::error_code error;
	fs::create_directories(file.parent_path(), error);
	std::ofstream output(file, std::ios::app);
	output << shortId(envelope_id) << "\n";
}

// ---- What came ----------------------------------------------------------------------------

enum class Taken {
	// Written here, kept, or set aside for good: not offered again.
	Done,
	// Not yet: no key for it here, or a key that waits for the lawyer to accept
	// it. Offered again on a later round.
	Waiting,
};

bool isFor(const std::vector<std::string> &to_devices, const std::string &me) {
	return to_devices.empty() == true || std::find(to_devices.begin(), to_devices.end(), me) != to_devices.end();
}

// Whose version comes after when two computers changed one document without
// seeing each other's change: the higher counter, then the higher device, so
// every computer of the Matter decides alike.
bool comesAfter(std::int64_t counter, const std::string &device, std::int64_t other_counter, const std::string &other_device) {
	return counter != other_counter ? counter > other_counter : device > other_device;
}

// A document's change that says which version it was made from. Made from the
// version here, or where none is, it replaces it: a document deleted here and
// changed there comes back, and nothing is lost. Made beside a version here
// that its computer had not seen, the two are concurrent and both are kept
// (COORD-4): on every computer the later one keeps the name and the other
// stands beside it, named for whose it is, so the Matter's computers end with
// the same files and no version is ever written over.
Taken applyDocumentChange(Linked &linked, const ChangeBody &body, std::int64_t counter, const std::string &sender,
		const std::string &me, const fs::path &file) {
	LinkedMatter &link = linked.view;
	const auto marked = linked.marks.find(body.path);
	// An earlier change of the same computer is superseded by its own later one.
	if (marked != linked.marks.end() && marked->second.sender == sender && counter <= marked->second.counter) return Taken::Done;
	// What is not what it says it is is never written.
	if (platform::sha256_hex_digest(body.content) != body.sha256) return Taken::Done;
	const PathMark incoming{counter, sender, body.from_name, std::nullopt};
	std::error_code error;
	const bool isHere = fs::is_regular_file(file, error);
	const std::string local = isHere == true ? shaOf(link.matter_id, body.path, file) : std::string();
	if (isHere == true && local == body.sha256) {
		link.known[body.path] = body.sha256;
		if (marked == linked.marks.end() || comesAfter(counter, sender, marked->second.counter, marked->second.sender) == true) {
			linked.marks[body.path] = incoming;
		}
		return Taken::Done;
	}
	const auto replaceWithIt = [&]() {
		linked.marks[body.path] = incoming;
		if (writeWhole(file, body.content) == true) {
			link.known[body.path] = body.sha256;
			(void)shaOf(link.matter_id, body.path, file);
		}
	};
	if (isHere == false || local == *body.base_sha256) {
		replaceWithIt();
		return Taken::Done;
	}
	// Concurrent. The version here, as the Matter's computers agreed it - this
	// computer's change or another's, by its mark -, or changed here and not
	// yet sent: that goes later in this round, counted after every change taken.
	const auto known = link.known.find(body.path);
	PathMark here{std::max(link.sent_counter + 1, clockMicros()), me, personName(), std::nullopt};
	if (known != link.known.end() && known->second == local) {
		here = marked != linked.marks.end() ? marked->second : PathMark{0, std::string(), std::string(), std::nullopt};
	}
	if (comesAfter(counter, sender, here.counter, here.sender) == true) {
		keepBeside(file, here.name, readFile(file));
		replaceWithIt();
	} else {
		keepBeside(file, body.from_name, body.content);
	}
	return Taken::Done;
}

Taken applyBody(Linked &linked, const ChangeBody &body, std::int64_t counter, const std::string &sender, const std::string &me) {
	LinkedMatter &link = linked.view;
	const std::optional<fs::path> file = inside(link.folder, body.path);
	if (file.has_value() == false || travels(body.path) == false) return Taken::Done;
	const bool isRecord = body.path.rfind(".lm/", 0) == 0;
	if (isRecord == false && body.deleted == false && body.base_sha256.has_value() == true) {
		return applyDocumentChange(linked, body, counter, sender, me, *file);
	}
	// A change that does not say what it was made from, a deletion, a record.
	const auto marked = linked.marks.find(body.path);
	const bool isOlder = marked != linked.marks.end() && counter <= marked->second.counter;
	std::error_code error;
	if (isOlder == true) {
		// An earlier change of the same computer is superseded by its own later
		// one; a Matter's record is one computer's, and the newer stands
		// (docs/MATTER_STORAGE.md); an earlier change to a document from another
		// computer is kept beside it, never lost and never written over it.
		if (marked->second.sender == sender || isRecord == true || body.deleted == true) return Taken::Done;
		if (platform::sha256_hex_digest(body.content) != body.sha256) return Taken::Done;
		if (fs::is_regular_file(*file, error) == true && shaOf(link.matter_id, body.path, *file) == body.sha256) return Taken::Done;
		keepBeside(*file, body.from_name, body.content);
		return Taken::Done;
	}
	linked.marks[body.path] = PathMark{counter, sender, body.from_name, std::nullopt};
	if (body.deleted == true) {
		// Deleted there: deleted here, unless it changed here since.
		if (fs::is_regular_file(*file, error) == true) {
			const auto known = link.known.find(body.path);
			if (known != link.known.end() && shaOf(link.matter_id, body.path, *file) == known->second) fs::remove(*file, error);
		}
		link.known.erase(body.path);
		seen_files()[link.matter_id].erase(body.path);
		return Taken::Done;
	}
	// What is not what it says it is is never written.
	if (platform::sha256_hex_digest(body.content) != body.sha256) return Taken::Done;
	if (fs::is_regular_file(*file, error) == true) {
		const std::string local = shaOf(link.matter_id, body.path, *file);
		if (local == body.sha256) {
			link.known[body.path] = body.sha256;
			return Taken::Done;
		}
		const auto known = link.known.find(body.path);
		const bool isChangedHere = known == link.known.end() || known->second != local;
		// A document both computers changed keeps both.
		if (isChangedHere == true && isRecord == false) {
			keepBeside(*file, body.from_name, body.content);
			return Taken::Done;
		}
	}
	if (writeWhole(*file, body.content) == true) {
		link.known[body.path] = body.sha256;
		(void)shaOf(link.matter_id, body.path, *file);
	}
	return Taken::Done;
}

Taken takeChange(Linked &linked, const std::string &envelope, const std::string &me, bool *opened_out = nullptr) {
	const std::optional<seal::Outside> outside = seal::outside_of(envelope);
	if (outside.has_value() == false || outside->header.matter_id != linked.view.matter_id) return Taken::Done;
	if (outside->header.from_device == me || isFor(outside->header.to_devices, me) == false) return Taken::Done;
	const std::optional<seal::MatterKey> key = matterKey(linked.view.matter_id, outside->key_id);
	if (key.has_value() == false) return Taken::Waiting;
	const std::optional<seal::OpenedChange> opened = seal::open_change(*key, envelope);
	if (opened.has_value() == false) {
		linked.view.last_error = "A change in the firm's folder was not sealed as it should be, and was set aside.";
		return Taken::Done;
	}
	hear(me, linked.view.matter_id, opened->header.from_device);
	if (opened_out != nullptr) *opened_out = true;
	// What this computer changes from now on counts after what it has seen.
	linked.view.sent_counter = std::max(linked.view.sent_counter, opened->header.counter);
	const std::optional<ChangeBody> body = changeBody(opened->plaintext);
	if (body.has_value() == false) return Taken::Done;
	return applyBody(linked, *body, opened->header.counter, opened->header.from_device, me);
}

// ---- Personal channels (D-128) --------------------------------------------------------------
//
// The private conversation of two members, carried like a Matter between their
// computers only: kept beside this computer's records, never in a Matter's
// folder or the firm's storage (ORG-15). Its key goes, with no invitation and
// no new comparison of codes, to each computer of either member that a lawyer
// here confirmed in a Matter they share; and it is taken only from one.

std::set<std::string> &taughtThisRun();

std::string myActor() {
	return installed().actor_id ? installed().actor_id() : std::string();
}

// The AI colleagues this computer runs, asked again a minute later at the
// soonest: each answer reads the agents' records.
struct HostedAgents {
	std::vector<std::string> ids;
	std::chrono::steady_clock::time_point asked_at {};
};

HostedAgents &hostedAgentsKept() {
	static HostedAgents *const kept = new HostedAgents();
	return *kept;
}

std::vector<std::string> hostedAgents() {
	const Environment &environment = installed();
	if (!environment.hosted_agents) return {};
	HostedAgents &kept = hostedAgentsKept();
	const auto now = std::chrono::steady_clock::now();
	if (kept.asked_at == std::chrono::steady_clock::time_point {} || now - kept.asked_at >= std::chrono::minutes(1)) {
		kept.ids = environment.hosted_agents();
		kept.asked_at = now;
	}
	return kept.ids;
}

// Whom this computer speaks for in a personal channel: its member, and each AI
// colleague it runs.
bool isMyActor(const std::string &actor_id) {
	if (actor_id.empty() == true) return false;
	if (actor_id == myActor()) return true;
	const std::vector<std::string> hosted = hostedAgents();
	return std::find(hosted.begin(), hosted.end(), actor_id) != hosted.end();
}

const std::string kPersonalPrefix = "pc_";

bool isPersonalId(const std::string &id) {
	return id.rfind(kPersonalPrefix, 0) == 0;
}

// The firm's own record (D-203): a channel carried as a personal one is, whose
// members are every member of the firm. Its one "member" says so.
const std::string kFirmMembers = "*firm";

bool isFirmChannel(const Linked &linked) {
	return linked.pair.size() == 1 && linked.pair.front() == kFirmMembers;
}

// Whether a confirmed computer is of this computer's own firm: its member's
// record names the same organisation, whatever its case and spaces (D-203).
std::string organisationKey(std::string name) {
	std::string kept;
	bool wasSpace = true;
	for (const char character : name) {
		const bool isSpace = character == ' ' || character == '\t';
		if (isSpace) {
			if (wasSpace == false) kept += ' ';
		} else {
			kept += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}
		wasSpace = isSpace;
	}
	while (kept.empty() == false && kept.back() == ' ') kept.pop_back();
	return kept;
}

bool isOfMyFirm(const std::string &organisation) {
	const Environment &environment = installed();
	const std::string mine = environment.organisation_name ? organisationKey(environment.organisation_name()) : std::string();
	return organisationKey(organisation) == mine;
}

// The firm's record's id here, from the organisation this computer's member
// is signed in to.
std::string firmChannelId() {
	const Environment &environment = installed();
	return firm_channel_id(environment.organisation_id ? environment.organisation_id() : std::string());
}

fs::path personalRoot() {
	return installed().records.parent_path() / "Personal";
}

bool isPersonal(const Linked &linked) {
	return linked.pair.empty() == false;
}

// A computer of a Matter's member a lawyer here confirmed: its member, its
// device key, its X25519 key and where it may be reached.
struct Colleague {
	std::string actor_id;
	std::string device_key;
	std::string coordination_key;
	std::vector<std::string> candidates;
	// The organisation its member's record names (D-203).
	std::string organisation;
};

// Who a Matter's computer is, as its member records say: asked again a minute
// later at the soonest, since each answer reads the Matter's records.
struct KnownComputer {
	std::optional<ConfirmedComputer> computer;
	std::chrono::steady_clock::time_point asked_at {};
};

std::map<std::string, KnownComputer> &knownComputers() {
	static std::map<std::string, KnownComputer> *const known = new std::map<std::string, KnownComputer>();
	return *known;
}

std::optional<ConfirmedComputer> confirmedComputer(const fs::path &matter_folder, const std::string &device_id) {
	const Environment &environment = installed();
	if (!environment.confirmed_computer) return std::nullopt;
	KnownComputer &known = knownComputers()[matter_folder.u8string() + "\n" + device_id];
	const auto now = std::chrono::steady_clock::now();
	if (known.asked_at == std::chrono::steady_clock::time_point {} || now - known.asked_at >= std::chrono::minutes(1)) {
		known.computer = environment.confirmed_computer(matter_folder, device_id);
		known.asked_at = now;
	}
	return known.computer;
}

// Every confirmed computer of the members of the Matters this computer
// carries, by device, with the X25519 key a checked card or a confirmed
// request gave for it.
std::map<std::string, Colleague> confirmedColleagues(const Records &records, const std::string &me) {
	std::map<std::string, Colleague> found;
	// The firm's computers a lawyer here checked (D-205), of this computer's own
	// firm by the check itself; one a shared Matter also names is taken from it
	// below, where its record says more.
	const Environment &environment = installed();
	if (environment.checked_firm_computers) {
		std::vector<CheckedFirmComputer> checked;
		try {
			checked = environment.checked_firm_computers();
		} catch (const std::exception &) {
		}
		for (const CheckedFirmComputer &computer : checked) {
			if (computer.device_id.empty() || computer.device_id == me || computer.coordination_key.empty()) continue;
			Colleague &colleague = found[computer.device_id];
			colleague.actor_id = computer.actor_id;
			colleague.device_key = computer.device_key;
			colleague.coordination_key = computer.coordination_key;
			colleague.candidates = computer.candidates;
			colleague.organisation = environment.organisation_name ? environment.organisation_name() : std::string();
		}
	}
	for (const Linked &linked : records.matters) {
		if (isPersonal(linked) == true || linked.view.state != "carried") continue;
		std::map<std::string, std::pair<std::string, std::vector<std::string>>> keys;
		for (const auto &[device, coordination_key] : linked.view.given) keys[device] = {coordination_key, {}};
		if (linked.inviter_device.empty() == false && linked.accepted_devices.count(linked.inviter_device) > 0) {
			keys[linked.inviter_device] = {linked.inviter_key, linked.inviter_candidates};
		}
		for (const auto &[device, card] : linked.cards) keys[device] = {card.coordination_key, card.candidates};
		for (const auto &[device, key] : keys) {
			if (device == me || key.first.empty() == true) continue;
			const std::optional<ConfirmedComputer> computer = confirmedComputer(linked.view.folder, device);
			if (computer.has_value() == false || computer->actor_id.empty() == true) continue;
			Colleague &colleague = found[device];
			colleague.actor_id = computer->actor_id;
			colleague.device_key = computer->device_key;
			colleague.organisation = computer->organisation;
			colleague.coordination_key = key.first;
			if (colleague.candidates.empty() == true) colleague.candidates = key.second;
		}
	}
	return found;
}

// Whether a lawyer here checked another of the firm's computers (D-205).
bool hasCheckedFirmComputers() {
	const Environment &environment = installed();
	if (!environment.checked_firm_computers) return false;
	const std::string me = environment.device_id ? environment.device_id() : std::string();
	try {
		for (const CheckedFirmComputer &computer : environment.checked_firm_computers()) {
			if (computer.device_id != me && computer.coordination_key.empty() == false) return true;
		}
	} catch (const std::exception &) {
	}
	return false;
}

Linked newFirmChannel(const std::string &id) {
	Linked linked;
	linked.view.matter_id = id;
	linked.view.folder = personalRoot() / fs::u8path(id);
	linked.view.state = "waiting";
	linked.pair = {kFirmMembers};
	return linked;
}

Linked newPersonalChannel(const std::string &id, const std::string &actor, const std::string &other_actor) {
	Linked linked;
	linked.view.matter_id = id;
	linked.view.folder = personalRoot() / fs::u8path(id);
	linked.view.state = "waiting";
	linked.pair = {actor, other_actor};
	std::sort(linked.pair.begin(), linked.pair.end());
	return linked;
}

// The channel with each confirmed colleague is listened for, so one the
// colleague's computer made arrives here without an invitation.
void listenForPersonalChannels(Records &records, const std::map<std::string, Colleague> &colleagues) {
	const std::string actor = myActor();
	const std::vector<std::string> hosted = hostedAgents();
	// The firm's record is listened for by every computer that knows another of
	// the firm's, so one a computer made arrives without an invitation (D-203).
	if (colleagues.empty() == false) {
		const std::string firm = firmChannelId();
		if (find(records, firm) == nullptr) records.matters.push_back(newFirmChannel(firm));
	}
	for (const auto &[device, colleague] : colleagues) {
		if (actor.empty() == false && colleague.actor_id != actor) {
			const std::string id = personal_channel_id(actor, colleague.actor_id);
			if (find(records, id) == nullptr) records.matters.push_back(newPersonalChannel(id, actor, colleague.actor_id));
		}
		// Each member's channel with each colleague this computer runs.
		for (const std::string &agent : hosted) {
			if (agent == colleague.actor_id) continue;
			const std::string id = personal_channel_id(colleague.actor_id, agent);
			if (find(records, id) == nullptr) records.matters.push_back(newPersonalChannel(id, colleague.actor_id, agent));
		}
	}
}

// What an AI colleague's card says of its host, asked again a minute later at
// the soonest: each answer reads the Matter's cards.
struct KnownClaim {
	std::optional<AgentHostClaim> claim;
	std::chrono::steady_clock::time_point asked_at {};
};

std::map<std::string, KnownClaim> &knownClaims() {
	static std::map<std::string, KnownClaim> *const known = new std::map<std::string, KnownClaim>();
	return *known;
}

std::optional<AgentHostClaim> agentClaim(const fs::path &matter_folder, const std::string &agent_id) {
	const Environment &environment = installed();
	if (!environment.agent_host_claim) return std::nullopt;
	KnownClaim &known = knownClaims()[matter_folder.u8string() + "\n" + agent_id];
	const auto now = std::chrono::steady_clock::now();
	if (known.asked_at == std::chrono::steady_clock::time_point {} || now - known.asked_at >= std::chrono::minutes(1)) {
		known.claim = environment.agent_host_claim(matter_folder, agent_id);
		known.asked_at = now;
	}
	return known.claim;
}

// The computer that runs the AI colleague `agent_id`, for its channel `linked`,
// when a Matter's card says so and every check holds (D-128):
// - a colleague's id made from its host's key must come from that key and the
//   card's nonce, so no other computer can speak for it; an older id, random,
//   has no nonce;
// - the card must be signed by that key;
// - that key must be the one a lawyer here confirmed for that computer, as the
//   computer of the lawyer the card says answers for the colleague;
// - and it must be the first host verified for this channel: another is never
//   taken. (For an older id that first one is trusted as first seen.)
std::optional<std::pair<std::string, Colleague>> agentHost(const Records &records, Linked &linked, const std::string &agent_id,
		const std::map<std::string, Colleague> &colleagues) {
	const Environment &environment = installed();
	// A host verified before stays the colleague's, whatever a later card says:
	// a card another computer wrote over it cuts nothing off. It is reached
	// while a lawyer here still confirms that computer.
	if (linked.agent_host.empty() == false) {
		const auto pinned = colleagues.find(linked.agent_host);
		if (pinned == colleagues.end() || pinned->second.device_key != linked.agent_host_key) return std::nullopt;
		Colleague host = pinned->second;
		host.actor_id = agent_id;
		return std::make_pair(linked.agent_host, host);
	}
	if (!environment.verify) return std::nullopt;
	for (const Linked &matter : records.matters) {
		if (isPersonal(matter) == true || matter.view.state != "carried") continue;
		const std::optional<AgentHostClaim> claim = agentClaim(matter.view.folder, agent_id);
		if (claim.has_value() == false || claim->signature.empty() == true || claim->host_device_key.empty() == true) continue;
		if (claim->id_nonce.empty() == false && agents::bound_agent_id(claim->host_device_key, claim->id_nonce) != agent_id) continue;
		const std::string signedText = agents::agent_card_signed_text(agent_id, claim->host_device_id, claim->host_device_key,
			claim->id_nonce, claim->responsible_actor_id, claim->counter);
		if (environment.verify(claim->host_device_key, signedText, claim->signature) == false) continue;
		const std::optional<ConfirmedComputer> computer = confirmedComputer(matter.view.folder, claim->host_device_id);
		if (computer.has_value() == false || computer->device_key != claim->host_device_key
				|| computer->actor_id != claim->responsible_actor_id) {
			continue;
		}
		const auto known = colleagues.find(claim->host_device_id);
		if (known == colleagues.end()) continue;
		linked.agent_host = claim->host_device_id;
		linked.agent_host_key = claim->host_device_key;
		Colleague host = known->second;
		host.actor_id = agent_id;
		return std::make_pair(claim->host_device_id, host);
	}
	return std::nullopt;
}

// The computers of a personal channel: each confirmed computer of either
// member, and for a member that is an AI colleague run elsewhere, the computer
// that runs it, as its verified card says.
std::map<std::string, Colleague> channelComputers(const Records &records, Linked &linked,
		const std::map<std::string, Colleague> &colleagues) {
	std::map<std::string, Colleague> computers;
	for (const auto &[device, colleague] : colleagues) {
		if (isFirmChannel(linked) == true ? isOfMyFirm(colleague.organisation)
				: std::find(linked.pair.begin(), linked.pair.end(), colleague.actor_id) != linked.pair.end()) {
			computers[device] = colleague;
		}
	}
	for (const std::string &member : linked.pair) {
		if (member.rfind("agent_", 0) != 0 || isMyActor(member) == true) continue;
		const auto host = agentHost(records, linked, member, colleagues);
		if (host.has_value() == true) computers[host->first] = host->second;
	}
	return computers;
}

bool isOfPair(const Linked &linked, const std::string &actor_id) {
	// Every member of the firm is of the firm's record.
	if (isFirmChannel(linked) == true) return actor_id.empty() == false;
	return std::find(linked.pair.begin(), linked.pair.end(), actor_id) != linked.pair.end();
}

// Whether a confirmed computer is one of a channel's: of either member of a
// personal channel, or of this computer's own firm for the firm's record.
bool isOfChannel(const Linked &linked, const Colleague &colleague) {
	if (isFirmChannel(linked) == true) return colleague.actor_id.empty() == false && isOfMyFirm(colleague.organisation);
	return isOfPair(linked, colleague.actor_id);
}

// The two members' confirmed computers, told to the direct link as the
// channel's, once a run each.
std::vector<std::string> teachPersonal(const Linked &linked, const std::map<std::string, Colleague> &colleagues) {
	std::vector<std::string> newly;
	const Environment &environment = installed();
	if (!environment.learned) return newly;
	for (const auto &[device, colleague] : colleagues) {
		if (isOfChannel(linked, colleague) == false) continue;
		seal::AddressCard peer;
		peer.matter_id = linked.view.matter_id;
		peer.device_id = device;
		peer.coordination_key = colleague.coordination_key;
		peer.candidates = colleague.candidates;
		std::string memo = environment.device_id() + "\n" + peer.matter_id + "\n" + device + "\n" + peer.coordination_key;
		for (const std::string &candidate : peer.candidates) memo += "\n" + candidate;
		if (taughtThisRun().insert(memo).second == false) continue;
		environment.learned(peer);
		newly.push_back(device);
	}
	return newly;
}

// The channel's key, sealed to each of the two members' confirmed computers
// that does not hold it yet, and the channel whole after it; and again to one
// met in this run, since a direct link takes nothing for a channel it was not
// yet told of, and that computer may not have been listening when it went.
void givePersonalKeys(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports,
		const seal::MatterKey &key, const std::string &me, const std::map<std::string, Colleague> &colleagues,
		const std::vector<std::string> &met) {
	const Environment &environment = installed();
	for (const auto &[device, colleague] : colleagues) {
		if (isOfChannel(linked, colleague) == false) continue;
		const bool isGiven = linked.view.given.count(device) > 0;
		if (isGiven == true && std::find(met.begin(), met.end(), device) == met.end()) continue;
		if (environment.refused_device_key && environment.refused_device_key(colleague.device_key) == true) continue;
		const auto recipient = seal::key_from_text(colleague.coordination_key);
		if (recipient.has_value() == false) continue;
		seal::KeyGift gift;
		gift.matter_id = linked.view.matter_id;
		gift.from_device = me;
		gift.from_device_key = environment.device_key();
		gift.to_device = device;
		gift.key = key;
		gift.counter = nextCounter(linked.view);
		sendEverywhere(transports, linked.view.matter_id, seal::seal_key(gift, *recipient, environment.sign), {device},
			linked.view.last_error);
		linked.view.given[device] = colleague.coordination_key;
		linked.given_device_keys[device] = colleague.device_key;
		if (isGiven == false && std::find(linked.snapshot_for.begin(), linked.snapshot_for.end(), device) == linked.snapshot_for.end()) {
			linked.snapshot_for.push_back(device);
		}
	}
}

// A channel's key, kept: when both computers made one at once, each holds both,
// and both seal with the one whose id comes first.
void keepPersonalKey(Linked &linked, const seal::MatterKey &key) {
	keepMatterKey(linked, key);
	linked.view.key_id = *std::min_element(linked.key_ids.begin(), linked.key_ids.end());
	std::error_code error;
	fs::create_directories(linked.view.folder, error);
}

Taken takeKey(Linked &linked, const std::string &envelope, const std::string &me, const seal::ComputerKeys &mine,
		const std::map<std::string, Colleague> &colleagues) {
	LinkedMatter &link = linked.view;
	const std::optional<seal::KeyOutside> outside = seal::key_outside(envelope);
	if (outside.has_value() == false || outside->matter_id != link.matter_id) return Taken::Done;
	if (outside->to_device != me || outside->from_device == me) return Taken::Done;
	if (isPersonal(linked) == true) {
		// Only from a confirmed computer of one of the two members, and to one.
		const auto sender = colleagues.find(outside->from_device);
		const bool isForMe = isFirmChannel(linked) == true
			|| std::any_of(linked.pair.begin(), linked.pair.end(), [](const std::string &member) { return isMyActor(member); });
		if (isForMe == false || sender == colleagues.end()
				|| sender->second.device_key != outside->from_device_key || isOfChannel(linked, sender->second) == false) {
			return Taken::Done;
		}
		const std::optional<seal::KeyGift> gift = seal::open_key(envelope, mine, outside->from_device_key, installed().verify);
		if (gift.has_value() == false) return Taken::Done;
		const std::string giftId = seal::matter_key_id(gift->key);
		const bool isNew = std::find(linked.key_ids.begin(), linked.key_ids.end(), giftId) == linked.key_ids.end();
		keepPersonalKey(linked, gift->key);
		if (isNew == true && giftId != link.key_id) {
			// Both computers made the channel's key, and this one goes on sealing
			// with its own, the first by id: the giver holds it only if this one's
			// gift of it was taken there, which it may not have been - it can come
			// before the giver is known as one of the channel's (a joiner's member
			// record comes once its window opens the Matter; measured live,
			// 2026-11-02). So it is given again, with the channel whole after it.
			link.given.erase(outside->from_device);
			return Taken::Done;
		}
		// The computer that gave it holds it: it is not given back.
		link.given[outside->from_device] = sender->second.coordination_key;
		linked.given_device_keys[outside->from_device] = outside->from_device_key;
		// What this computer wrote under a key of its own goes to it again, under
		// the one both now seal with.
		if (isNew == true && std::find(linked.snapshot_for.begin(), linked.snapshot_for.end(), outside->from_device)
				== linked.snapshot_for.end()) {
			linked.snapshot_for.push_back(outside->from_device);
		}
		return Taken::Done;
	}
	const bool isAccepted = std::find(link.accepted_senders.begin(), link.accepted_senders.end(), outside->from_device_key)
		!= link.accepted_senders.end();
	if (isAccepted == false) {
		// Not before this computer's lawyer has compared the giving computer's code.
		const auto offered = std::find_if(link.offering.begin(), link.offering.end(),
			[&](const OfferingComputer &computer) { return computer.device_key == outside->from_device_key; });
		if (offered == link.offering.end()) {
			link.offering.push_back(OfferingComputer{outside->from_device, outside->from_device_key,
				installed().code_of ? installed().code_of(outside->from_device_key) : std::string(), outside->files, outside->bytes});
		} else if (outside->bytes > 0) {
			// What is coming, as the giving computer said it last (D-207).
			offered->files = outside->files;
			offered->bytes = outside->bytes;
		}
		return Taken::Waiting;
	}
	const std::optional<seal::KeyGift> gift = seal::open_key(envelope, mine, outside->from_device_key, installed().verify);
	if (gift.has_value() == false) return Taken::Done;
	keepMatterKey(linked, gift->key);
	link.offering.erase(std::remove_if(link.offering.begin(), link.offering.end(),
		[&](const OfferingComputer &computer) { return computer.device_key == outside->from_device_key; }), link.offering.end());
	return Taken::Done;
}

Taken takeJoinRequest(Linked &linked, const std::string &envelope, const std::string &me) {
	LinkedMatter &link = linked.view;
	if (isPersonal(linked) == true) return Taken::Done;
	const std::optional<seal::JoinRequest> request = seal::open_join_request(envelope, installed().verify);
	if (request.has_value() == false || request->matter_id != link.matter_id || request->device_id == me) return Taken::Done;
	// Only a computer that carries the Matter can let another in.
	if (link.state != "carried" || link.given.count(request->device_id) > 0) return Taken::Done;
	const std::string coordinationKey = seal::key_text(request->coordination_key);
	auto asking = std::find_if(link.asking.begin(), link.asking.end(),
		[&](const AskingComputer &computer) { return computer.device_id == request->device_id; });
	AskingComputer computer{request->actor_id, request->device_id, request->device_key, coordinationKey,
		installed().code_of ? installed().code_of(request->device_key) : std::string(), request->at};
	if (asking == link.asking.end()) {
		link.asking.push_back(computer);
	} else {
		*asking = computer;
	}
	return Taken::Done;
}

// ---- Where the firm's computers are (0x04, D-124) -------------------------------------------

// What this computer last told each Matter's computers of where it may be
// reached: for as long as the program runs, under the round's lock.
struct SentCard {
	std::int64_t at = 0;
	std::vector<std::string> candidates;
};

std::map<std::string, SentCard> &sentCards() {
	static std::map<std::string, SentCard> *const cards = new std::map<std::string, SentCard>();
	return *cards;
}

// The device key this computer holds for one of a Matter's computers, as its
// lawyer confirmed it: given the key here, its key accepted here, or
// confirmed among the Matter's members (Tool/DeviceTrust). Nothing for one no
// lawyer here confirmed, or one refused since.
std::optional<std::string> confirmedKeyOf(const Linked &linked, const std::string &device_id) {
	const Environment &environment = installed();
	std::optional<std::string> key;
	const auto given = linked.given_device_keys.find(device_id);
	const auto accepted = linked.accepted_devices.find(device_id);
	if (given != linked.given_device_keys.end()) key = given->second;
	else if (accepted != linked.accepted_devices.end()) key = accepted->second;
	else if (environment.confirmed_device_key) key = environment.confirmed_device_key(linked.view.folder, device_id);
	if (key.has_value() == true && environment.refused_device_key && environment.refused_device_key(*key) == true) return std::nullopt;
	return key;
}

Taken takeCard(Linked &linked, const std::string &envelope, const std::string &me) {
	const std::optional<seal::Outside> outside = seal::outside_of(envelope);
	if (outside.has_value() == false || outside->header.matter_id != linked.view.matter_id) return Taken::Done;
	const std::string &device = outside->header.from_device;
	if (device == me) return Taken::Done;
	// Kept for when this computer holds the Matter's key.
	const std::optional<seal::MatterKey> key = matterKey(linked.view.matter_id, outside->key_id);
	if (key.has_value() == false) return Taken::Waiting;
	// Learned only of a computer a lawyer here confirmed (D-095, D-123): the card
	// is checked against the device key held for it, never against one it names.
	// One not confirmed yet waits until it is, or until a card sealed when its
	// counter says (microseconds since 1970) has run out.
	const std::optional<std::string> deviceKey = confirmedKeyOf(linked, device);
	if (deviceKey.has_value() == false) {
		const std::int64_t sealedAt = outside->header.counter / 1000000;
		return nowSeconds() - sealedAt > kCardLastsSeconds ? Taken::Done : Taken::Waiting;
	}
	const std::optional<seal::AddressCard> card = seal::open_address_card(*key, envelope, *deviceKey, installed().verify);
	if (card.has_value() == false || card->coordination_key.empty() == true) return Taken::Done;
	const std::optional<std::int64_t> expires = secondsOf(card->expires_at);
	if (expires.has_value() == false || *expires <= nowSeconds()) return Taken::Done;
	std::int64_t &newest = linked.card_counters[device];
	if (card->counter <= newest) return Taken::Done;
	newest = card->counter;
	hear(me, linked.view.matter_id, device);
	linked.cards[device] = *card;
	if (installed().learned) installed().learned(*card);
	return Taken::Done;
}

// ---- What is passed on (D-140) --------------------------------------------------------------

// A change another computer sealed, passed on as it came to the Matter's other
// computers this one knows: a computer sends to those it knows, and a computer
// that let two colleagues' computers in is the one both know. So a Matter of
// three lawyers comes together though two of them never met. Only the sealed
// bytes move, under the key each of them holds; nothing is opened for them or
// sealed again. A computer takes an envelope once, whichever way it came
// (taken_ids), so one that comes back is set aside. A carrier every computer
// reads, as the firm's folder is read, needs no passing on.
void relay(const Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports,
		const std::string &envelope, const std::string &me) {
	if (isPersonal(linked) == true) return;
	const std::optional<seal::Outside> outside = seal::outside_of(envelope);
	if (outside.has_value() == false) return;
	std::set<std::string> known;
	for (const auto &[device, key] : linked.given_device_keys) known.insert(device);
	for (const auto &[device, key] : linked.accepted_devices) known.insert(device);
	for (const auto &[device, card] : linked.cards) known.insert(device);
	std::vector<std::string> onward;
	for (const std::string &device : known) {
		if (device == me || device == outside->header.from_device) continue;
		if (outside->header.to_devices.empty() == false && isFor(outside->header.to_devices, device) == false) continue;
		// Never to a computer no lawyer here confirmed, or one refused since.
		if (confirmedKeyOf(linked, device).has_value() == false) continue;
		onward.push_back(device);
	}
	if (onward.empty() == true) return;
	for (const auto &carrier : transports) {
		if (carrier->reaches_every_computer() == true) continue;
		try {
			carrier->send(linked.view.matter_id, bytesOf(envelope), onward);
		} catch (const std::exception &) {
			// The computer that sealed it sends it again; the next one passes it on then.
		}
	}
	platform::trace::record("coordination", "passed_on", {{"bytes", envelope.size()}, {"computers", onward.size()}});
}

// ---- What goes ----------------------------------------------------------------------------

void sendChange(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports, const seal::MatterKey &key,
		const ChangeBody &body, const std::vector<std::string> &to_devices, const std::string &me, std::int64_t as_counter = 0) {
	seal::Header header;
	header.matter_id = linked.view.matter_id;
	header.from_device = me;
	header.to_devices = to_devices;
	// A change sent again is the change it was: its counter, not a new one.
	header.counter = as_counter > 0 ? as_counter : nextCounter(linked.view);
	platform::trace::record("coordination", "sent", {{"bytes", body.content.size()}, {"again", as_counter > 0}});
	if (to_devices.empty() == true) linked.marks[body.path] = PathMark{header.counter, me, body.from_name, body.base_sha256};
	sendEverywhere(transports, linked.view.matter_id, seal::seal_change(key, header, changePayload(body)), to_devices,
		linked.view.last_error);
}

void sendChanges(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports, const seal::MatterKey &key,
		const std::string &me, const Listing &listing, const std::vector<std::string> &tooLarge) {
	LinkedMatter &link = linked.view;
	if (tooLarge.empty() == false) {
		std::string names;
		for (const std::string &name : tooLarge) names += (names.empty() == true ? "" : ", ") + name;
		link.last_error = names + (tooLarge.size() == 1 ? " is" : " are") + " larger than the firm's envelopes carry "
			"(48 MB), so " + (tooLarge.size() == 1 ? "it stays" : "they stay") + " on this computer.";
	}
	for (const auto &[path, file] : listing.files) {
		const std::string sha = shaListed(link.matter_id, listing, path, file);
		const auto known = link.known.find(path);
		if (known != link.known.end() && known->second == sha) continue;
		ChangeBody body;
		body.path = path;
		body.sha256 = sha;
		body.modified_at = nowText();
		body.from_name = personName();
		body.base_sha256 = known != link.known.end() ? known->second : std::string();
		body.content = readFile(file);
		sendChange(linked, transports, key, body, {}, me);
		link.known[path] = sha;
	}
	// A file is gone only when the whole folder was read and it was not in it.
	if (listing.isWhole == false) return;
	for (auto known = link.known.begin(); known != link.known.end();) {
		if (listing.files.count(known->first) > 0 || travels(known->first) == false) {
			++known;
			continue;
		}
		ChangeBody gone;
		gone.path = known->first;
		gone.deleted = true;
		gone.modified_at = nowText();
		sendChange(linked, transports, key, gone, {}, me);
		seen_files()[link.matter_id].erase(known->first);
		known = link.known.erase(known);
	}
}

// The computers sent a Matter whole in this run, by Matter: one that tells its
// inventory before the whole has reached it is not sent it all again.
std::set<std::string> &snapshotsThisRun() {
	static std::set<std::string> *const sent = new std::set<std::string>();
	return *sent;
}

void sendInventory(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports, const seal::MatterKey &key,
		const std::string &me, const std::string &to_device, const Listing &listing);

// The Matter whole, to computers just let in: what it holds first, so each
// knows how much is coming before the files do (D-207), then each file.
void sendSnapshots(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports, const seal::MatterKey &key,
		const std::string &me, const Listing &listing) {
	if (linked.snapshot_for.empty() == true) return;
	for (const std::string &device : linked.snapshot_for) sendInventory(linked, transports, key, me, device, listing);
	for (const std::string &device : linked.snapshot_for) snapshotsThisRun().insert(me + "\n" + linked.view.matter_id + "\n" + device);
	for (const auto &[path, file] : listing.files) {
		ChangeBody body;
		body.path = path;
		body.sha256 = shaListed(linked.view.matter_id, listing, path, file);
		body.modified_at = nowText();
		body.from_name = personName();
		const auto known = linked.view.known.find(path);
		body.base_sha256 = known != linked.view.known.end() ? known->second : std::string();
		body.content = readFile(file);
		sendChange(linked, transports, key, body, linked.snapshot_for, me);
	}
	linked.snapshot_for.clear();
}

// ---- Meeting again (0x05) ---------------------------------------------------------------

// The computers this computer told its inventory to in this run, by Matter.
std::set<std::string> &inventoriesThisRun() {
	static std::set<std::string> *const told = new std::set<std::string>();
	return *told;
}

// Every file of the Matter that travels, as this computer holds it - its place,
// SHA-256, size and the change it is -, to a computer it meets again: each then
// sends the other its own changes the other does not hold (0x05). Once a run.
void sendInventory(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports, const seal::MatterKey &key,
		const std::string &me, const std::string &to_device, const Listing &listing) {
	LinkedMatter &link = linked.view;
	if (inventoriesThisRun().insert(me + "\n" + link.matter_id + "\n" + to_device).second == false) return;
	seal::Inventory inventory;
	inventory.header.matter_id = link.matter_id;
	inventory.header.from_device = me;
	inventory.header.to_devices = {to_device};
	inventory.header.counter = nextCounter(link);
	for (const auto &[path, file] : listing.files) {
		if (inside(link.folder, path).has_value() == false) continue;
		seal::InventoryEntry entry;
		entry.path = path;
		entry.sha256 = shaListed(link.matter_id, listing, path, file);
		if (entry.sha256.size() != 64) continue;
		const auto facts = listing.facts.find(path);
		entry.size = facts == listing.facts.end() ? 0 : static_cast<std::uint64_t>(facts->second.first);
		const auto mark = linked.marks.find(path);
		if (mark != linked.marks.end()) {
			entry.counter = mark->second.counter;
			entry.sender = mark->second.sender;
		}
		inventory.files.push_back(std::move(entry));
	}
	try {
		sendEverywhere(transports, link.matter_id, seal::seal_inventory(key, inventory), {to_device}, link.last_error);
	} catch (const std::invalid_argument &) {
		// Only what travels is listed; were one refused, the next meeting tells again.
		inventoriesThisRun().erase(me + "\n" + link.matter_id + "\n" + to_device);
	}
}

// Another computer's inventory: this computer's own changes it does not hold,
// or holds older, go to it again, each as the change it was, so the rules of
// older and newer decide as for any change. A file changed here since it was
// last sent goes with this round's changes anyway, and another computer's
// change is that computer's to send. The other is told this computer's
// inventory in turn, once a run.
Taken takeInventory(Linked &linked, const std::string &envelope, const std::string &me,
		const std::vector<std::shared_ptr<transport::Transport>> &transports, const std::function<const Listing &()> &listed) {
	LinkedMatter &link = linked.view;
	const std::optional<seal::Outside> outside = seal::outside_of(envelope);
	if (outside.has_value() == false || outside->header.matter_id != link.matter_id) return Taken::Done;
	if (outside->header.from_device == me || isFor(outside->header.to_devices, me) == false) return Taken::Done;
	const std::optional<seal::MatterKey> key = matterKey(link.matter_id, outside->key_id);
	if (key.has_value() == false) return Taken::Waiting;
	const std::optional<seal::Inventory> inventory = seal::open_inventory(*key, envelope);
	if (inventory.has_value() == false) return Taken::Done;
	const std::string other = inventory->header.from_device;
	hear(me, link.matter_id, other);
	// What it holds, so the lawyer is told how far the Matter has come (D-207).
	std::map<std::string, HeldThere> &said = heldThere()[me + "\n" + link.matter_id][other];
	said.clear();
	for (const seal::InventoryEntry &entry : inventory->files) said[entry.path] = HeldThere{entry.sha256, entry.size, entry.counter};
	const Listing &listing = listed();
	if (snapshotsThisRun().count(me + "\n" + link.matter_id + "\n" + other) > 0) {
		sendInventory(linked, transports, *key, me, other, listing);
		return Taken::Done;
	}
	std::map<std::string, const seal::InventoryEntry *> held;
	for (const seal::InventoryEntry &entry : inventory->files) held[entry.path] = &entry;
	// What the other computer lacks, or holds older, goes again as the change it
	// was - its counter and its author's name - whoever made it: a Matter of
	// three lawyers comes together though a change of one reached the others
	// only through this computer, and was lost on the way (D-140). Each file
	// goes only where the other's counter for it is older, so nothing comes back.
	for (const auto &[path, file] : listing.files) {
		const auto mark = linked.marks.find(path);
		if (mark == linked.marks.end()) continue;
		const std::string sha = shaListed(link.matter_id, listing, path, file);
		const auto known = link.known.find(path);
		if (known == link.known.end() || known->second != sha) continue;
		const auto there = held.find(path);
		if (there != held.end() && (there->second->sha256 == sha || there->second->counter >= mark->second.counter)) continue;
		ChangeBody body;
		body.path = path;
		body.sha256 = sha;
		body.modified_at = nowText();
		body.from_name = mark->second.sender == me || mark->second.name.empty() == true ? personName() : mark->second.name;
		body.base_sha256 = mark->second.base;
		body.content = readFile(file);
		sendChange(linked, transports, *key, body, {other}, me, mark->second.counter);
	}
	// What was deleted here, and the other still holds as it was before: the
	// deletion goes again, as the change it was, whoever made it. Lost on the
	// way, it would leave the file there, and the file would come back.
	for (const auto &[path, mark] : linked.marks) {
		if (link.known.count(path) > 0 || listing.files.count(path) > 0 || travels(path) == false) continue;
		const auto there = held.find(path);
		if (there == held.end() || there->second->counter >= mark.counter) continue;
		ChangeBody gone;
		gone.path = path;
		gone.deleted = true;
		gone.modified_at = nowText();
		gone.from_name = mark.sender == me || mark.name.empty() == true ? personName() : mark.name;
		sendChange(linked, transports, *key, gone, {other}, me, mark.counter);
	}
	sendInventory(linked, transports, *key, me, other, listing);
	return Taken::Done;
}

// Every computer of the Matter a lawyer here confirmed, told to the direct
// link (D-127): the newest checked card of each - its key and addresses -, a
// computer given the key here by the X25519 key its confirmed request named,
// and the one whose invitation this computer joined by once its key was
// accepted here. The link keeps them only while the program runs, so they are
// taught again after a restart; within a run, each only when what is known of
// it changed. One refused since is never taught.
std::set<std::string> &taughtThisRun() {
	static std::set<std::string> *const taught = new std::set<std::string>();
	return *taught;
}

std::vector<std::string> teachPeers(const Linked &linked) {
	std::vector<std::string> newly;
	const Environment &environment = installed();
	if (!environment.learned) return newly;
	std::map<std::string, seal::AddressCard> peers;
	for (const auto &[device, coordination_key] : linked.view.given) {
		if (coordination_key.empty() == true) continue;
		seal::AddressCard peer;
		peer.matter_id = linked.view.matter_id;
		peer.device_id = device;
		peer.coordination_key = coordination_key;
		peers[device] = std::move(peer);
	}
	if (linked.inviter_device.empty() == false && linked.inviter_key.empty() == false
			&& linked.accepted_devices.count(linked.inviter_device) > 0) {
		seal::AddressCard inviter;
		inviter.matter_id = linked.view.matter_id;
		inviter.device_id = linked.inviter_device;
		inviter.coordination_key = linked.inviter_key;
		inviter.candidates = linked.inviter_candidates;
		peers[linked.inviter_device] = std::move(inviter);
	}
	// A checked card stands over both: it is signed by the confirmed device key.
	for (const auto &[device, card] : linked.cards) peers[device] = card;
	for (auto &[device, peer] : peers) {
		if (confirmedKeyOf(linked, device).has_value() == false) continue;
		peer.matter_id = linked.view.matter_id;
		std::string memo = environment.device_id() + "\n" + peer.matter_id + "\n" + device + "\n" + peer.coordination_key;
		for (const std::string &candidate : peer.candidates) memo += "\n" + candidate;
		if (taughtThisRun().insert(memo).second == false) continue;
		environment.learned(peer);
		newly.push_back(device);
	}
	return newly;
}

// The computer whose invitation this one joined by, told to the direct link
// as a computer of the Matter (LMM2, D-127): the request to join goes there,
// and its key gift comes back the same way.
void teachInviter(const Linked &linked) {
	if (linked.inviter_device.empty() == true || linked.inviter_key.empty() == true || !installed().learned) return;
	seal::AddressCard inviter;
	inviter.matter_id = linked.view.matter_id;
	inviter.device_id = linked.inviter_device;
	inviter.coordination_key = linked.inviter_key;
	inviter.candidates = linked.inviter_candidates;
	installed().learned(inviter);
}

void askToJoin(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports, const std::string &me) {
	const Environment &environment = installed();
	const seal::ComputerKeys mine = computerKeys();
	seal::JoinRequest request;
	request.matter_id = linked.view.matter_id;
	request.device_id = me;
	request.actor_id = environment.actor_id ? environment.actor_id() : std::string();
	request.device_key = environment.device_key();
	request.coordination_key = mine.public_key;
	request.at = nowText();
	sendEverywhere(transports, linked.view.matter_id, seal::seal_join_request(request, environment.sign), {}, linked.view.last_error);
	linked.asked_at = nowSeconds();
}

// Tells the Matter's other computers where this computer's direct link may be
// reached, with its X25519 key, under the Matter's key and its own device's
// signature (0x04, D-124): at once when that changes, and every few hours.
void sendCard(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports, const seal::MatterKey &key,
		const std::string &me, const seal::ComputerKeys &mine) {
	const Environment &environment = installed();
	if (!environment.candidates || !environment.sign) return;
	std::vector<std::string> candidates = environment.candidates();
	if (candidates.empty() == true) return;
	std::sort(candidates.begin(), candidates.end());
	SentCard &sent = sentCards()[me + "\n" + linked.view.matter_id];
	const std::int64_t now = nowSeconds();
	if (sent.candidates == candidates && now - sent.at < kCardAgainSeconds) return;
	seal::AddressCard card;
	card.matter_id = linked.view.matter_id;
	card.device_id = me;
	card.candidates = candidates;
	card.coordination_key = seal::key_text(mine.public_key);
	card.expires_at = timestampOf(now + kCardLastsSeconds);
	card.counter = nextCounter(linked.view);
	sendEverywhere(transports, linked.view.matter_id, seal::seal_address_card(key, card, environment.sign), {}, linked.view.last_error);
	sent.at = now;
	sent.candidates = std::move(candidates);
}

void giveKeyTo(Linked *linked, const std::string &device_id, const std::vector<std::shared_ptr<transport::Transport>> &transports);

// ---- The firm's model accounts (0x06, D-210) -----------------------------------------

// What of an account was given to a computer, as a fingerprint: the account's
// entry and key as they were, never the key itself.
std::string accountPrint(const std::string &device_id, const FirmAccount &account) {
	return platform::sha256_hex_digest("singularity-d210-account\n" + device_id + "\n" + account.provider_id + "\n" + account.entry
		+ "\n" + account.credential);
}

// An account one of the firm's computers gave this one: taken only from a
// computer a lawyer here checked, with the very device key it was checked
// with, opened straight into this computer's settings and credential store.
// One this computer connected itself is never replaced by one given.
Taken takeAccount(Linked &linked, const std::string &envelope, const std::string &me, const seal::ComputerKeys &mine,
		const std::map<std::string, Colleague> &colleagues) {
	if (isFirmChannel(linked) == false) return Taken::Done;
	const std::optional<seal::AccountOutside> outside = seal::account_outside(envelope);
	if (outside.has_value() == false || outside->to_device != me || outside->matter_id != linked.view.matter_id) return Taken::Done;
	const auto sender = colleagues.find(outside->from_device);
	// From a computer no lawyer here has checked yet: it waits until one has.
	if (sender == colleagues.end()) return Taken::Waiting;
	if (sender->second.device_key != outside->from_device_key) return Taken::Done;
	const Environment &environment = installed();
	if (!environment.verify || !environment.keep_account) return Taken::Done;
	std::optional<seal::AccountGift> gift = seal::open_account(envelope, mine, sender->second.device_key, environment.verify);
	if (gift.has_value() == false || gift->provider_id.empty() == true) return Taken::Done;
	if (linked.accounts_taken.count(gift->provider_id) == 0 && environment.accounts_here) {
		std::vector<FirmAccount> here;
		try {
			here = environment.accounts_here();
		} catch (const std::exception &) {
		}
		const bool isOwn = std::any_of(here.begin(), here.end(), [&gift](const FirmAccount &account) {
			return account.provider_id == gift->provider_id;
		});
		for (FirmAccount &account : here) crypto_wipe(reinterpret_cast<std::uint8_t *>(account.credential.data()), account.credential.size());
		if (isOwn == true) return Taken::Done;
	}
	FirmAccount account{gift->provider_id, gift->entry, gift->credential};
	try {
		environment.keep_account(account, gift->removed);
	} catch (const std::exception &) {
		crypto_wipe(reinterpret_cast<std::uint8_t *>(account.credential.data()), account.credential.size());
		return Taken::Done;
	}
	crypto_wipe(reinterpret_cast<std::uint8_t *>(account.credential.data()), account.credential.size());
	if (gift->removed == true) linked.accounts_taken.erase(gift->provider_id);
	else linked.accounts_taken[gift->provider_id] = outside->from_device;
	return Taken::Done;
}

// The model accounts connected on this computer, given sealed to each of the
// firm's computers a lawyer here checked: again when one changes, and to a
// computer met again in this run (`met`), which may have missed one; and
// taken back where this computer has an account no more. An account given to
// this computer is never given on.
void giveAccounts(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports, const std::string &me,
		const std::map<std::string, Colleague> &colleagues, const std::vector<std::string> &met) {
	const Environment &environment = installed();
	if (isFirmChannel(linked) == false || !environment.accounts_here || !environment.sign) return;
	// Looked at every half minute, and at once for a computer met again: the
	// accounts' keys are not read from the credential store every round.
	static std::map<std::string, std::chrono::steady_clock::time_point> lookedAt;
	const auto now = std::chrono::steady_clock::now();
	auto &last = lookedAt[environment.device_id() + "\n" + linked.view.matter_id];
	if (met.empty() == true && last != std::chrono::steady_clock::time_point {} && now - last < environment.accounts_every) return;
	last = now;
	std::vector<FirmAccount> here;
	try {
		here = environment.accounts_here();
	} catch (const std::exception &) {
		return;
	}
	here.erase(std::remove_if(here.begin(), here.end(), [&linked](const FirmAccount &account) {
		return account.provider_id.empty() == true || linked.accounts_taken.count(account.provider_id) > 0;
	}), here.end());
	for (const auto &[device, colleague] : colleagues) {
		if (device == me || colleague.actor_id.empty() == true || isOfChannel(linked, colleague) == false) continue;
		if (environment.refused_device_key && environment.refused_device_key(colleague.device_key) == true) continue;
		const auto recipient = seal::key_from_text(colleague.coordination_key);
		if (recipient.has_value() == false) continue;
		const bool isMet = std::find(met.begin(), met.end(), device) != met.end();
		std::set<std::string> current;
		for (const FirmAccount &account : here) {
			current.insert(account.provider_id);
			const std::string key = device + "\n" + account.provider_id;
			const std::string print = accountPrint(device, account);
			const auto given = linked.accounts_given.find(key);
			if (isMet == false && given != linked.accounts_given.end() && given->second == print) continue;
			seal::AccountGift gift;
			gift.matter_id = linked.view.matter_id;
			gift.from_device = me;
			gift.from_device_key = environment.device_key();
			gift.to_device = device;
			gift.counter = nextCounter(linked.view);
			gift.provider_id = account.provider_id;
			gift.entry = account.entry;
			gift.credential = account.credential;
			sendEverywhere(transports, linked.view.matter_id, seal::seal_account(gift, *recipient, environment.sign), {device},
				linked.view.last_error);
			linked.accounts_given[key] = print;
		}
		for (auto given = linked.accounts_given.begin(); given != linked.accounts_given.end();) {
			if (given->first.rfind(device + "\n", 0) != 0 || current.count(given->first.substr(device.size() + 1)) > 0) {
				++given;
				continue;
			}
			seal::AccountGift gift;
			gift.matter_id = linked.view.matter_id;
			gift.from_device = me;
			gift.from_device_key = environment.device_key();
			gift.to_device = device;
			gift.counter = nextCounter(linked.view);
			gift.provider_id = given->first.substr(device.size() + 1);
			gift.credential = "-";
			gift.removed = true;
			sendEverywhere(transports, linked.view.matter_id, seal::seal_account(gift, *recipient, environment.sign), {device},
				linked.view.last_error);
			given = linked.accounts_given.erase(given);
		}
	}
	for (FirmAccount &account : here) crypto_wipe(reinterpret_cast<std::uint8_t *>(account.credential.data()), account.credential.size());
}

// The confirmed computers of the members a Matter was offered to (D-205), told
// to the direct link as the Matter's, so their request to join is heard.
void teachOffered(const Linked &linked, const std::map<std::string, Colleague> &colleagues) {
	const Environment &environment = installed();
	if (linked.offered_to.empty() == true || !environment.learned) return;
	for (const auto &[device, colleague] : colleagues) {
		if (linked.offered_to.count(colleague.actor_id) == 0 || colleague.coordination_key.empty() == true) continue;
		seal::AddressCard peer;
		peer.matter_id = linked.view.matter_id;
		peer.device_id = device;
		peer.coordination_key = colleague.coordination_key;
		peer.candidates = colleague.candidates;
		std::string memo = environment.device_id() + "\n" + peer.matter_id + "\n" + device + "\n" + peer.coordination_key;
		for (const std::string &candidate : peer.candidates) memo += "\n" + candidate;
		if (taughtThisRun().insert(memo).second == false) continue;
		environment.learned(peer);
	}
}

// A computer that asks to join a Matter offered to its member, and is that
// member's as a lawyer here confirmed it, with the very keys it asks with, is
// given the Matter's key: the lawyer chose that member (D-205).
void giveOffered(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports,
		const std::map<std::string, Colleague> &colleagues) {
	if (linked.offered_to.empty() == true || linked.view.state != "carried" || isPersonal(linked) == true) return;
	std::vector<std::string> devices;
	for (const AskingComputer &asking : linked.view.asking) {
		const auto colleague = colleagues.find(asking.device_id);
		if (colleague == colleagues.end() || linked.offered_to.count(colleague->second.actor_id) == 0) continue;
		if (asking.actor_id != colleague->second.actor_id || asking.device_key != colleague->second.device_key
				|| asking.coordination_key != colleague->second.coordination_key) {
			continue;
		}
		const Environment &environment = installed();
		if (environment.refused_device_key && environment.refused_device_key(asking.device_key) == true) continue;
		devices.push_back(asking.device_id);
	}
	for (const std::string &device : devices) {
		try {
			giveKeyTo(&linked, device, transports);
		} catch (const std::exception &failure) {
			linked.view.last_error = failure.what();
		}
	}
}

// `colleagues` are a personal channel's computers (none for a Matter); `firm`
// every computer of the firm a lawyer here confirmed, for a Matter given to a
// member by choosing them (D-205).
void roundFor(Linked &linked, const std::vector<std::shared_ptr<transport::Transport>> &transports, const std::string &me,
		const seal::ComputerKeys &mine, const std::map<std::string, Colleague> &colleagues, const std::map<std::string, Colleague> &firm) {
	LinkedMatter &link = linked.view;
	std::error_code error;
	// A personal channel's folder is this computer's own: made when its key is.
	if (isPersonal(linked) == true && link.state == "carried") fs::create_directories(link.folder, error);
	if (isPersonal(linked) == false && fs::is_directory(link.folder, error) == false) {
		link.last_error = "The Matter's folder is not on this computer now.";
		return;
	}
	link.last_error.clear();
	const std::vector<std::string> met = isPersonal(linked) == true ? teachPersonal(linked, colleagues)
		: link.state == "carried" ? teachPeers(linked) : std::vector<std::string>();
	if (isPersonal(linked) == false && link.state == "carried") teachOffered(linked, firm);
	// What came first: a document changed both here and there is then seen as
	// changed on both sides, and both are kept.
	std::set<std::string> taken;
	std::size_t changesTaken = 0;
	// The folder as it was when an inventory came, read once a round.
	std::optional<Listing> listedForInventories;
	const std::function<const Listing &()> listedOnce = [&]() -> const Listing & {
		if (listedForInventories.has_value() == false) listedForInventories = travelling(link);
		return *listedForInventories;
	};
	for (const auto &carrier : transports) {
		std::vector<SealedBytes> arrived;
		try {
			arrived = carrier->poll(link.matter_id);
		} catch (const std::exception &failure) {
			link.last_error = "The firm's " + carrier->name() + " could not be read: " + failure.what();
			continue;
		}
		// The firm's computers' counters stand in the order things happened:
		// what came is taken in that order, whatever order it arrived in.
		std::vector<std::pair<std::int64_t, std::string>> ordered;
		for (const SealedBytes &bytes : arrived) {
			const std::string envelope = textOf(bytes);
			const std::optional<seal::Outside> outside = seal::outside_of(envelope);
			ordered.emplace_back(outside.has_value() == true ? outside->header.counter : 0, envelope);
		}
		std::stable_sort(ordered.begin(), ordered.end(),
			[](const auto &first, const auto &second) { return first.first < second.first; });
		for (const auto &entry : ordered) {
			const std::string &envelope = entry.second;
			const std::string id = transport::envelope_id(bytesOf(envelope));
			if (taken.insert(id).second == false || takenFor(link.matter_id).count(shortId(id)) > 0) {
				carrier->ack(link.matter_id, id);
				continue;
			}
			Taken result = Taken::Done;
			switch (seal::kind_of(envelope)) {
			case seal::Kind::Change: {
				bool opened = false;
				result = takeChange(linked, envelope, me, &opened);
				if (result == Taken::Done) {
					++changesTaken;
					if (opened == true) relay(linked, transports, envelope, me);
				}
				platform::trace::record("coordination", "took", {{"bytes", envelope.size()}});
				break;
			}
			case seal::Kind::KeyWrap:
				result = takeKey(linked, envelope, me, mine, colleagues);
				break;
			case seal::Kind::JoinRequest:
				result = takeJoinRequest(linked, envelope, me);
				break;
			case seal::Kind::AddressCard:
				result = takeCard(linked, envelope, me);
				break;
			case seal::Kind::Inventory:
				result = takeInventory(linked, envelope, me, transports, listedOnce);
				break;
			case seal::Kind::AccountGift:
				result = takeAccount(linked, envelope, me, mine, colleagues);
				break;
			default:
				result = Taken::Done;
				break;
			}
			if (result == Taken::Done) {
				remember(link.matter_id, id);
				carrier->ack(link.matter_id, id);
			}
		}
	}
	// The window shows what came at once (D-127): it reads the Matter again on
	// this news, which this computer's own rounds do not wake on.
	if (changesTaken > 0) {
		platform::events::publish("matter.changed", nlohmann::json{{"projectId", link.matter_id}, {"from", "coordination"}});
	}
	giveOffered(linked, transports, firm);
	const std::optional<seal::MatterKey> key = matterKey(link.matter_id, link.key_id);
	if (link.state == "carried" && key.has_value() == true) {
		// The folder read once for all that goes, after what came was written.
		std::vector<std::string> tooLarge;
		const Listing listing = travelling(link, &tooLarge);
		if (isPersonal(linked) == true) givePersonalKeys(linked, transports, *key, me, colleagues, met);
		if (isFirmChannel(linked) == true) giveAccounts(linked, transports, me, colleagues, met);
		sendSnapshots(linked, transports, *key, me, listing);
		sendChanges(linked, transports, *key, me, listing, tooLarge);
		// A channel's computers are known from the Matters: it tells no address.
		if (isPersonal(linked) == false) sendCard(linked, transports, *key, me, mine);
		// A computer met again in this run is told what this one holds.
		for (const std::string &device : met) sendInventory(linked, transports, *key, me, device, listing);
		// An invitation given lately: its computer may be called about it.
		if (linked.invited_at > 0 && nowSeconds() - linked.invited_at < kLobbySeconds && installed().open_lobby) {
			installed().open_lobby(link.matter_id, linked.invited_at + kLobbySeconds);
		}
	} else if (link.state != "carried" && isPersonal(linked) == false) {
		teachInviter(linked);
		if (nowSeconds() - linked.asked_at >= kAskAgainSeconds) askToJoin(linked, transports, me);
	}
	if (link.last_error.empty() == true) link.last_sync_at = nowText();
}

std::vector<std::shared_ptr<transport::Transport>> carriers(const Records &records) {
	const Environment &environment = installed();
	// A computer that shares and joins no Matter opens no carrier: nothing
	// binds a network port for a lawyer who never asked for it.
	if (!environment.transports || records.matters.empty() == true) return {};
	return environment.transports(records.firm_folder, computerKeys());
}

// How far a Matter has come to this computer from each other (D-207). Before
// its key is taken here: what each offer said the Matter is, and the sealed
// bytes of it already on this computer. After: of what each computer said it
// holds, each file this computer holds as it was said, or a later change of
// - so a file changed or deleted since is never waited for. A Matter whole
// here is no transfer.
std::vector<Transfer> receivingOf(const Linked &linked, const std::string &me, const std::vector<std::string> &connected) {
	const LinkedMatter &link = linked.view;
	std::vector<Transfer> coming;
	const auto isReachable = [&](const std::string &device) {
		return std::find(connected.begin(), connected.end(), device) != connected.end();
	};
	if (link.state != "carried") {
		const Environment &environment = installed();
		const std::uint64_t sealed = environment.sealed_here ? environment.sealed_here(link.matter_id) : 0;
		for (const OfferingComputer &offer : link.offering) {
			if (offer.files == 0 && offer.bytes == 0) continue;
			Transfer transfer;
			transfer.device_id = offer.device_id;
			transfer.files = offer.files;
			transfer.bytes = offer.bytes;
			transfer.bytes_here = std::min(sealed, offer.bytes);
			transfer.reachable = isReachable(offer.device_id);
			coming.push_back(std::move(transfer));
		}
		return coming;
	}
	const auto said = heldThere().find(me + "\n" + link.matter_id);
	if (said == heldThere().end()) return coming;
	for (const auto &[device, files] : said->second) {
		Transfer transfer;
		transfer.device_id = device;
		transfer.reachable = isReachable(device);
		for (const auto &[path, held] : files) {
			transfer.files += 1;
			transfer.bytes += held.size;
			const auto known = link.known.find(path);
			const auto mark = linked.marks.find(path);
			const bool isHere = (known != link.known.end() && known->second == held.sha256)
				|| (mark != linked.marks.end() && held.counter > 0 && mark->second.counter >= held.counter);
			if (isHere == true) {
				transfer.files_here += 1;
				transfer.bytes_here += held.size;
			}
		}
		if (transfer.files_here < transfer.files) coming.push_back(std::move(transfer));
	}
	return coming;
}

CoordinationStatus statusOf(const Records &records) {
	CoordinationStatus result;
	result.firm_folder = records.firm_folder;
	for (const auto &carrier : carriers(records)) result.transports.push_back(carrier->name());
	const Environment &environment = installed();
	if (environment.code_of && environment.device_key) result.device_code = environment.code_of(environment.device_key());
	const std::string actor = myActor();
	const std::string me = environment.device_id ? environment.device_id() : std::string();
	// Whether a Matter or the firm's record travels, as the lawyer is told it
	// (D-137), and what of it is on its way, and how far it has come (D-207).
	const auto travelling = [&](const Linked &linked) {
		LinkedMatter view = linked.view;
		view.offered_to = linked.offered_to;
		if (environment.connected) view.connected = environment.connected(view.matter_id);
		const auto heard = heardFrom().find(me + "\n" + view.matter_id);
		if (heard != heardFrom().end()) view.heard = heard->second;
		view.receiving = receivingOf(linked, me, view.connected);
		if (environment.sending) view.sending = environment.sending(view.matter_id);
		return view;
	};
	for (const Linked &linked : records.matters) {
		if (isPersonal(linked) == false) {
			result.matters.push_back(travelling(linked));
			continue;
		}
		// The firm's record is no conversation of the member's: it is said apart.
		if (isFirmChannel(linked) == true) {
			result.firm = travelling(linked);
			continue;
		}
		LinkedMatter channel = linked.view;
		for (const std::string &member : linked.pair) {
			if (member != actor && (channel.other_actor_id.empty() == true || isMyActor(member) == false)) channel.other_actor_id = member;
		}
		result.personal.push_back(std::move(channel));
	}
	return result;
}

// ---- The thread -----------------------------------------------------------------------------

struct Rounds {
	std::mutex mutex;
	std::condition_variable wake;
	std::thread thread;
	bool is_stopping = false;
	bool is_running = false;
	bool is_woken = false;
};
Rounds &rounds() {
	static Rounds *state = new Rounds();
	return *state;
}

// What the app said it wrote to a shared Matter, by Matter, waiting for the
// round thread: sent before anything else is read (D-127).
struct Written {
	std::mutex mutex;
	std::map<std::string, std::set<std::string>> paths;
};
Written &written() {
	static Written *const kept = new Written();
	return *kept;
}

} // namespace

void install_environment(Environment environment) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	installed() = std::move(environment);
	seen_files().clear();
	taken_ids().clear();
	// A new program reads its records afresh. Its direct link knows no one:
	// everyone is taught again, and told again what this computer holds.
	recordsCache() = RecordsCache {};
	personNameKept() = PersonName {};
	taughtThisRun().clear();
	inventoriesThisRun().clear();
	snapshotsThisRun().clear();
	knownComputers().clear();
	knownClaims().clear();
	hostedAgentsKept() = HostedAgents {};
}

bool travels(const std::string &relative_path) {
	if (relative_path.empty() == true) return false;
	// Asked of every file of every Matter each round: split by hand, as a
	// stream costs more than the rest of the check (measured on Windows).
	std::vector<std::string> parts;
	for (std::size_t start = 0; start < relative_path.size();) {
		const std::size_t slash = relative_path.find('/', start);
		const std::size_t end = slash == std::string::npos ? relative_path.size() : slash;
		parts.push_back(relative_path.substr(start, end - start));
		start = end + 1;
	}
	if (parts.empty() == true) return false;
	if (parts.front() == ".lm" && parts.size() > 1 && (parts[1] == "Cache" || parts[1] == "Index" || parts[1] == "Locks")) {
		return false;
	}
	const auto endsWith = [](const std::string &text, const char *suffix) {
		const std::size_t size = std::char_traits<char>::length(suffix);
		return text.size() >= size && text.compare(text.size() - size, std::string::npos, suffix) == 0;
	};
	for (const std::string &name : parts) {
		std::string lowered = name;
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
		if (name.rfind("~$", 0) == 0 || lowered == ".ds_store" || lowered == "thumbs.db" || lowered == "desktop.ini") return false;
		if (platform::is_staged_file_name(name) == true || name.find(".lm-tmp-") != std::string::npos) return false;
		// The file Singularity writes for a moment to learn a folder takes its
		// files (Node/LegacyWorkspace validate_directory).
		if (name.rfind(".singularity-workspace-write-test-", 0) == 0) return false;
		if (endsWith(lowered, kPartSuffix) == true || endsWith(lowered, kOldPartSuffix) == true) return false;
	}
	return true;
}

bool carries(const std::string &matter_id, const std::filesystem::path &folder) {
	if (matter_id.empty() == true) return false;
	// The records are replaced whole as they are written: read on their own,
	// they are the last ones written, never half of them. The path is the
	// installed environment's, set once as the program starts.
	const Json value = Json::parse(readBytes(installed().records), nullptr, false);
	if (value.is_object() == false || value.value("version", 0) != 2) return false;
	for (const Json &entry : value.value("matters", Json::array())) {
		if (entry.is_object() == false || entry.value("matterId", std::string()) != matter_id) continue;
		if (entry.value("state", std::string()) != "carried") continue;
		std::error_code error;
		if (fs::equivalent(fs::u8path(entry.value("folder", std::string())), folder, error) == true && !error) return true;
	}
	return false;
}

std::vector<CarriedMatter> carried_matters() {
	std::vector<CarriedMatter> carried;
	const Json value = Json::parse(readBytes(installed().records), nullptr, false);
	if (value.is_object() == false || value.value("version", 0) != 2) return carried;
	for (const Json &entry : value.value("matters", Json::array())) {
		if (entry.is_object() == false || entry.value("state", std::string()) != "carried") continue;
		const std::string id = entry.value("matterId", std::string());
		if (id.empty() == true || isPersonalId(id) == true || entry.contains("pair") == true) continue;
		carried.push_back(CarriedMatter{id, fs::u8path(entry.value("folder", std::string()))});
	}
	return carried;
}

CoordinationStatus use_firm_folder(const fs::path &folder) {
	std::error_code error;
	if (folder.empty() == true || fs::is_directory(folder, error) == false) {
		throw std::runtime_error("That is not a folder on this computer: choose the folder of the firm's own storage "
			"that its computers keep in step.");
	}
	// It must take a file this computer writes.
	const fs::path probe = folder / ".lm-coordination-probe";
	try {
		platform::write_file_durably(probe, "Singularity");
	} catch (const std::exception &) {
		throw std::runtime_error("Singularity cannot write to that folder: choose one of the firm's storage that this computer can change.");
	}
	fs::remove(probe, error);
	const std::lock_guard<std::mutex> lock(round_mutex());
	Records records = load();
	records.firm_folder = folder;
	save(records);
	return statusOf(records);
}

CoordinationStatus status() {
	const std::lock_guard<std::mutex> lock(round_mutex());
	return statusOf(load());
}

CoordinationStatus leave_matter(const std::string &matter_id) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	Records records = load();
	const auto found = std::find_if(records.matters.begin(), records.matters.end(), [&matter_id](const Linked &linked) {
		return linked.view.matter_id == matter_id && isPersonal(linked) == false;
	});
	if (found != records.matters.end()) {
		records.matters.erase(found);
		save(records);
	}
	return statusOf(records);
}

std::string personal_channel_id(const std::string &actor_id, const std::string &other_actor_id) {
	std::vector<std::string> pair{actor_id, other_actor_id};
	std::sort(pair.begin(), pair.end());
	return kPersonalPrefix + platform::sha256_hex_digest(pair[0] + "\n" + pair[1]).substr(0, 16);
}

PersonalChannel personal_channel(const std::string &other_actor_id) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	const std::string actor = myActor();
	if (actor.empty() == true || other_actor_id.empty() == true || other_actor_id == actor) {
		throw std::invalid_argument("A personal conversation is between this computer's member and another member.");
	}
	const std::string id = personal_channel_id(actor, other_actor_id);
	Records records = load();
	Linked *linked = find(records, id);
	if (linked == nullptr) {
		records.matters.push_back(newPersonalChannel(id, actor, other_actor_id));
		linked = &records.matters.back();
	}
	if (matterKey(id, linked->view.key_id).has_value() == false) keepPersonalKey(*linked, seal::make_matter_key());
	std::error_code error;
	fs::create_directories(linked->view.folder, error);
	const PersonalChannel channel{id, linked->view.folder};
	save(records);
	wake();
	return channel;
}

std::string firm_channel_id(const std::string &organisation_id) {
	return kPersonalPrefix + "firm_"
		+ platform::sha256_hex_digest("firm\n" + (organisation_id.empty() == true ? std::string("local") : organisation_id)).substr(0, 16);
}

std::string own_coordination_key() {
	return seal::key_text(computerKeys().public_key);
}

std::vector<std::string> own_candidates() {
	const Environment &environment = installed();
	return environment.candidates ? environment.candidates() : std::vector<std::string>();
}

PersonalChannel firm_channel() {
	const std::lock_guard<std::mutex> lock(round_mutex());
	const std::string id = firmChannelId();
	Records records = load();
	Linked *linked = find(records, id);
	if (linked == nullptr) {
		records.matters.push_back(newFirmChannel(id));
		linked = &records.matters.back();
	}
	// Made with a key of its own where none came yet: when two computers made
	// one, both are kept and the first by id seals, as for a personal channel.
	if (matterKey(id, linked->view.key_id).has_value() == false) keepPersonalKey(*linked, seal::make_matter_key());
	std::error_code error;
	fs::create_directories(linked->view.folder, error);
	const PersonalChannel channel{id, linked->view.folder};
	save(records);
	wake();
	return channel;
}

void listen_for_personal_channel(const std::string &other_actor_id) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	const std::string actor = myActor();
	if (actor.empty() == true || other_actor_id.empty() == true || other_actor_id == actor) {
		throw std::invalid_argument("A personal conversation is between this computer's member and another member.");
	}
	const std::string id = personal_channel_id(actor, other_actor_id);
	Records records = load();
	if (find(records, id) != nullptr) return;
	records.matters.push_back(newPersonalChannel(id, actor, other_actor_id));
	save(records);
	wake();
}

CoordinationStatus share_matter(const std::string &matter_id, const std::string &name, const fs::path &folder) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	if (isPersonalId(matter_id) == true) throw std::invalid_argument("A personal conversation is never shared as a Matter.");
	Records records = load();
	Linked *linked = find(records, matter_id);
	if (linked == nullptr) {
		records.matters.push_back(Linked{});
		linked = &records.matters.back();
		linked->view.matter_id = matter_id;
	}
	linked->view.name = name;
	linked->view.folder = folder;
	if (matterKey(matter_id, linked->view.key_id).has_value() == false) {
		keepMatterKey(*linked, seal::make_matter_key());
	}
	linked->view.state = "carried";
	save(records);
	return statusOf(records);
}

namespace {

// Asks to join by an invitation; with `giver_device_key`, the key gift of the
// computer that offered the Matter is taken as it comes (D-205).
CoordinationStatus joinBy(const Invitation &invitation, const fs::path &folder, const std::string &giver_device_key) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	if (isPersonalId(invitation.matter_id) == true) throw std::invalid_argument("That is not an invitation to a Matter.");
	Records records = load();
	std::error_code error;
	fs::create_directories(folder, error);
	Linked *linked = find(records, invitation.matter_id);
	if (linked == nullptr) {
		records.matters.push_back(Linked{});
		linked = &records.matters.back();
		linked->view.matter_id = invitation.matter_id;
		linked->view.state = "waiting";
	}
	linked->view.name = invitation.name;
	linked->view.folder = folder;
	if (invitation.coordination_key.empty() == false) {
		linked->inviter_device = invitation.device_id;
		linked->inviter_key = invitation.coordination_key;
		linked->inviter_candidates = invitation.candidates;
	}
	// A computer both lawyers checked (D-205): its key gift needs no code
	// compared, since the lawyer here accepted the Matter it offered.
	if (giver_device_key.empty() == false && invitation.device_id.empty() == false) {
		if (std::find(linked->view.accepted_senders.begin(), linked->view.accepted_senders.end(), giver_device_key)
				== linked->view.accepted_senders.end()) {
			linked->view.accepted_senders.push_back(giver_device_key);
		}
		linked->accepted_devices[invitation.device_id] = giver_device_key;
	}
	// The carriers open once this computer has a Matter of the firm's.
	const auto transports = carriers(records);
	if (transports.empty() == true) throw std::runtime_error(kNoWay);
	if (records.firm_folder.empty() == true && linked->inviter_key.empty() == true) {
		throw std::runtime_error("This invitation names no computer to call, and no folder of the firm is chosen: ask your "
			"colleague for the invitation again, or choose the firm's folder.");
	}
	if (linked->view.state != "carried") {
		teachInviter(*linked);
		askToJoin(*linked, transports, installed().device_id());
	}
	save(records);
	return statusOf(records);
}

} // namespace

CoordinationStatus join_matter(const Invitation &invitation, const fs::path &folder) {
	return joinBy(invitation, folder, std::string());
}

CoordinationStatus take_offered_matter(const Invitation &invitation, const std::string &giver_device_key, const fs::path &folder) {
	if (giver_device_key.empty() == true || invitation.device_id.empty() == true || invitation.coordination_key.empty() == true) {
		throw std::invalid_argument("The offer names no computer of the firm to take the Matter from.");
	}
	return joinBy(invitation, folder, giver_device_key);
}

CoordinationStatus join_matter(const std::string &matter_id, const std::string &name, const fs::path &folder) {
	Invitation invitation;
	invitation.matter_id = matter_id;
	invitation.name = name;
	return join_matter(invitation, folder);
}

std::string invitation(const std::string &matter_id) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	if (isPersonalId(matter_id) == true) throw std::invalid_argument("A personal conversation has no invitation.");
	Records records = load();
	Linked *linked = find(records, matter_id);
	const std::string name = linked == nullptr ? std::string() : linked->view.name;
	const std::string plain = "LMM1:" + matter_id + ":" + encode64(name);
	if (linked == nullptr) return plain;
	// The carriers open first: where the direct link may be reached is what the
	// invitation names.
	static_cast<void>(carriers(records));
	const Environment &environment = installed();
	const std::vector<std::string> candidates = environment.candidates ? environment.candidates() : std::vector<std::string>();
	linked->invited_at = nowSeconds();
	if (environment.open_lobby) environment.open_lobby(matter_id, linked->invited_at + kLobbySeconds);
	save(records);
	if (candidates.empty() == true) return plain;
	// Public halves only: the device's id, its X25519 key and its addresses.
	const Json link{{"d", environment.device_id()}, {"x", seal::key_text(computerKeys().public_key)}, {"c", candidates}};
	return "LMM2:" + matter_id + ":" + encode64(name) + ":" + encode64(link.dump());
}

std::optional<Invitation> read_invitation(const std::string &text) {
	std::string trimmed = text;
	while (trimmed.empty() == false && std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) trimmed.pop_back();
	while (trimmed.empty() == false && std::isspace(static_cast<unsigned char>(trimmed.front())) != 0) trimmed.erase(trimmed.begin());
	const bool isTwo = trimmed.rfind("LMM2:", 0) == 0;
	if (trimmed.rfind("LMM1:", 0) != 0 && isTwo == false) return std::nullopt;
	const std::size_t idEnd = trimmed.find(':', 5);
	if (idEnd == std::string::npos) return std::nullopt;
	const std::size_t nameEnd = isTwo == true ? trimmed.find(':', idEnd + 1) : std::string::npos;
	if (isTwo == true && nameEnd == std::string::npos) return std::nullopt;
	Invitation result;
	result.matter_id = trimmed.substr(5, idEnd - 5);
	const std::optional<std::string> name =
		decode64(nameEnd == std::string::npos ? trimmed.substr(idEnd + 1) : trimmed.substr(idEnd + 1, nameEnd - idEnd - 1));
	if (result.matter_id.empty() == true || name.has_value() == false) return std::nullopt;
	for (const char character : result.matter_id) {
		if (std::isalnum(static_cast<unsigned char>(character)) == 0 && character != '_' && character != '-') return std::nullopt;
	}
	result.name = *name;
	if (isTwo == true) {
		const std::optional<std::string> linkText = decode64(trimmed.substr(nameEnd + 1));
		if (linkText.has_value() == false) return std::nullopt;
		const Json link = Json::parse(*linkText, nullptr, false);
		if (link.is_object() == false) return std::nullopt;
		result.device_id = link.value("d", std::string());
		result.coordination_key = link.value("x", std::string());
		if (result.device_id.empty() == true || seal::key_from_text(result.coordination_key).has_value() == false) return std::nullopt;
		// Addresses of the link alone, and a few of them.
		for (const std::string &candidate : strings(link.value("c", Json::array()))) {
			if (candidate.rfind("udp:", 0) == 0 && candidate.size() <= 64 && result.candidates.size() < 16) result.candidates.push_back(candidate);
		}
	}
	return result;
}

CoordinationStatus refuse(const std::string &matter_id, const std::string &device_id) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	Records records = load();
	Linked *linked = find(records, matter_id);
	if (linked != nullptr) {
		auto asking = std::find_if(linked->view.asking.begin(), linked->view.asking.end(),
			[&](const AskingComputer &computer) { return computer.device_id == device_id; });
		if (asking != linked->view.asking.end()) {
			if (installed().refused) installed().refused(asking->coordination_key);
			linked->view.asking.erase(asking);
		}
	}
	save(records);
	return statusOf(records);
}

namespace {

// Gives a computer that asked to join the Matter its key, sealed to the X25519
// key its request named, and the Matter whole after it; the caller holds the
// round's lock and saves the records.
void giveKeyTo(Linked *linked, const std::string &device_id, const std::vector<std::shared_ptr<transport::Transport>> &transports) {
	const std::string &matter_id = linked->view.matter_id;
	const std::optional<seal::MatterKey> key = matterKey(matter_id, linked->view.key_id);
	if (key.has_value() == false) throw std::runtime_error("This computer does not hold this Matter's key.");
	auto asking = std::find_if(linked->view.asking.begin(), linked->view.asking.end(),
		[&](const AskingComputer &computer) { return computer.device_id == device_id; });
	if (asking == linked->view.asking.end()) throw std::runtime_error("That computer has not asked to join this Matter.");
	const auto recipient = seal::key_from_text(asking->coordination_key);
	if (recipient.has_value() == false) throw std::runtime_error("That computer's request to join could not be read.");
	const Environment &environment = installed();
	// Its direct link is spoken with by the X25519 key its request named, the
	// request whose code the lawyer here compared, at the address it called
	// from (D-127).
	if (environment.learned) {
		seal::AddressCard asker;
		asker.matter_id = matter_id;
		asker.device_id = device_id;
		asker.coordination_key = asking->coordination_key;
		environment.learned(asker);
	}
	seal::KeyGift gift;
	gift.matter_id = matter_id;
	gift.from_device = environment.device_id();
	gift.from_device_key = environment.device_key();
	gift.to_device = device_id;
	gift.key = *key;
	gift.counter = nextCounter(linked->view);
	// What of the Matter travels, as the snapshot that follows will send it, so
	// the lawyer taking it knows how much is coming before any of it opens (D-207).
	const Listing listing = travelling(linked->view);
	gift.files = listing.files.size();
	for (const auto &[path, file] : listing.files) {
		static_cast<void>(file);
		const auto facts = listing.facts.find(path);
		if (facts != listing.facts.end()) gift.bytes += static_cast<std::uint64_t>(facts->second.first);
	}
	sendEverywhere(transports, matter_id, seal::seal_key(gift, *recipient, environment.sign), {device_id}, linked->view.last_error);
	linked->view.given[device_id] = asking->coordination_key;
	linked->given_device_keys[device_id] = asking->device_key;
	linked->view.asking.erase(asking);
	if (std::find(linked->snapshot_for.begin(), linked->snapshot_for.end(), device_id) == linked->snapshot_for.end()) {
		linked->snapshot_for.push_back(device_id);
	}
}

} // namespace

CoordinationStatus give_key(const std::string &matter_id, const std::string &device_id) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	Records records = load();
	Linked *linked = find(records, matter_id);
	if (linked == nullptr || linked->view.state != "carried") {
		throw std::runtime_error("This computer does not carry this Matter, so it cannot let another computer in.");
	}
	const auto transports = carriers(records);
	giveKeyTo(linked, device_id, transports);
	save(records);
	return statusOf(records);
}

std::string offer_matter(const std::string &matter_id, const std::string &actor_id) {
	if (actor_id.empty() == true) throw std::invalid_argument("Choose the colleague to give the Matter to.");
	{
		const std::lock_guard<std::mutex> lock(round_mutex());
		Records records = load();
		Linked *linked = find(records, matter_id);
		if (linked == nullptr || linked->view.state != "carried" || isPersonal(*linked) == true) {
			throw std::runtime_error("This computer does not carry this Matter yet, so it cannot give it.");
		}
		linked->offered_to[actor_id] = nowSeconds();
		save(records);
	}
	wake();
	return invitation(matter_id);
}

MatterSize matter_size(const std::string &matter_id) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	Records records = load();
	const Linked *linked = find(records, matter_id);
	MatterSize size;
	if (linked == nullptr || linked->view.state != "carried") return size;
	const Listing listing = travelling(linked->view);
	size.files = listing.files.size();
	for (const auto &[path, file] : listing.files) {
		static_cast<void>(file);
		const auto facts = listing.facts.find(path);
		if (facts != listing.facts.end()) size.bytes += static_cast<std::uint64_t>(facts->second.first);
	}
	return size;
}

CoordinationStatus accept_key(const std::string &matter_id, const std::string &device_id) {
	const std::lock_guard<std::mutex> lock(round_mutex());
	Records records = load();
	Linked *linked = find(records, matter_id);
	if (linked == nullptr) throw std::runtime_error("This computer has not asked to join this Matter.");
	const auto offering = std::find_if(linked->view.offering.begin(), linked->view.offering.end(),
		[&](const OfferingComputer &computer) { return computer.device_id == device_id; });
	if (offering == linked->view.offering.end()) throw std::runtime_error("That computer has not offered this Matter's key.");
	if (std::find(linked->view.accepted_senders.begin(), linked->view.accepted_senders.end(), offering->device_key)
			== linked->view.accepted_senders.end()) {
		linked->view.accepted_senders.push_back(offering->device_key);
	}
	linked->accepted_devices[device_id] = offering->device_key;
	save(records);
	return statusOf(records);
}

// The computers whose codes the person here compared when taking a Matter
// count as their members' once a member record states their key: taking a
// Matter with the giver's code compared is the comparison Compare codes asks
// for, so it is not asked again for that computer (2026-10-29: Marcus compared
// Dana's code taking Project Frost, then was asked again before Unicorn took up
// her request). Each is looked at until it counts, then no more while Singularity
// runs.
std::set<std::string> &comparedSettled() {
	static std::set<std::string> settled;
	return settled;
}

void confirmComparedComputers(const Linked &linked) {
	if (!installed().compared_when_taken || linked.view.state != "carried" || isPersonal(linked) == true) return;
	for (const auto &[device, key] : linked.accepted_devices) {
		const std::string id = linked.view.matter_id + "\n" + key;
		if (key.empty() == true || comparedSettled().count(id) > 0) continue;
		try {
			if (installed().compared_when_taken(linked.view.folder, key) == true) comparedSettled().insert(id);
		} catch (const std::exception &) {
		}
	}
}

void sync_once() {
	try {
		// What a round costs, part by part (SINGULARITY_TRACE_FILE): counts and
		// durations only, never a path or a name.
		platform::trace::Span round("coordination", "round");
		const std::lock_guard<std::mutex> lock(round_mutex());
		const double started = platform::trace::now_ms();
		Records records = load();
		round.add("loadMs", platform::trace::now_ms() - started);
		if (records.matters.empty() == true) {
			// A computer whose lawyer checked another of the firm's (D-205) listens
			// for the firm's record, though it carries no Matter yet.
			if (hasCheckedFirmComputers() == false) return;
			records.matters.push_back(newFirmChannel(firmChannelId()));
		}
		double at = platform::trace::now_ms();
		const auto transports = carriers(records);
		round.add("carriersMs", platform::trace::now_ms() - at);
		if (transports.empty() == true) {
			for (Linked &linked : records.matters) {
				linked.view.last_error = kNoWay;
			}
			save(records);
			return;
		}
		const std::string me = installed().device_id();
		at = platform::trace::now_ms();
		const seal::ComputerKeys mine = computerKeys();
		round.add("keysMs", platform::trace::now_ms() - at);
		const std::map<std::string, Colleague> colleagues = confirmedColleagues(records, me);
		listenForPersonalChannels(records, colleagues);
		std::map<std::string, std::map<std::string, Colleague>> channels;
		for (Linked &linked : records.matters) {
			if (isPersonal(linked) == true) channels[linked.view.matter_id] = channelComputers(records, linked, colleagues);
		}
		const std::map<std::string, Colleague> none;
		// What the app said it just wrote goes at once, before any folder is read;
		// the rounds below find it sent and read on (D-127).
		std::map<std::string, std::set<std::string>> named;
		{
			Written &kept = written();
			const std::lock_guard<std::mutex> writtenLock(kept.mutex);
			named.swap(kept.paths);
		}
		std::size_t sentAtOnce = 0;
		for (Linked &linked : records.matters) {
			const auto paths = named.find(linked.view.matter_id);
			if (paths == named.end() || linked.view.state != "carried") continue;
			const std::optional<seal::MatterKey> key = matterKey(linked.view.matter_id, linked.view.key_id);
			if (key.has_value() == false) continue;
			for (const std::string &path : paths->second) {
				const std::optional<fs::path> file = inside(linked.view.folder, path);
				std::error_code error;
				if (file.has_value() == false || travels(path) == false || fs::is_regular_file(*file, error) == false) continue;
				if (fs::file_size(*file, error) > kMostFileBytes || error) continue;
				const std::string sha = shaOf(linked.view.matter_id, path, *file);
				const auto known = linked.view.known.find(path);
				if (known != linked.view.known.end() && known->second == sha) continue;
				ChangeBody body;
				body.path = path;
				body.sha256 = sha;
				body.modified_at = nowText();
				body.from_name = personName();
				body.base_sha256 = known != linked.view.known.end() ? known->second : std::string();
				body.content = readFile(*file);
				sendChange(linked, transports, *key, body, {}, me);
				linked.view.known[path] = sha;
				++sentAtOnce;
			}
		}
		round.add("sentAtOnce", sentAtOnce);
		for (Linked &linked : records.matters) {
			platform::trace::Span matter("coordination", "matterRound", {{"files", linked.view.known.size()}});
			try {
				const auto computers = channels.find(linked.view.matter_id);
				roundFor(linked, transports, me, mine, computers == channels.end() ? none : computers->second, colleagues);
			} catch (const std::exception &failure) {
				linked.view.last_error = std::string("This computer could not finish its round for this Matter: ") + failure.what();
			}
			confirmComparedComputers(linked);
		}
		at = platform::trace::now_ms();
		save(records);
		round.add("saveMs", platform::trace::now_ms() - at);
		askPersonNameAgainWhenOld();
	} catch (...) {
	}
}

void start() {
	Rounds &state = rounds();
	const std::lock_guard<std::mutex> lock(state.mutex);
	if (state.is_running == true) return;
	state.is_stopping = false;
	state.is_running = true;
	// What this computer writes to a Matter goes at once: a person's message, an
	// AI colleague's record, a document's change (D-127). Its own news of what
	// came does not wake it.
	static std::once_flag observing;
	std::call_once(observing, []() {
		platform::events::observe([](const std::string &name, const nlohmann::json &payload) {
			if (name != "matter.changed" && name != "operation.changed" && name != "agent.changed") return;
			if (payload.is_object() == true && payload.value("from", std::string()) == "coordination") return;
			// A save that names its files: those go first, before the folder is read.
			if (payload.is_object() == true && payload.contains("paths") == true && payload.at("paths").is_array() == true) {
				const std::string matter = payload.value("projectId", std::string());
				if (matter.empty() == false) {
					Written &kept = written();
					const std::lock_guard<std::mutex> lock(kept.mutex);
					for (const nlohmann::json &path : payload.at("paths")) {
						if (path.is_string() == true) kept.paths[matter].insert(path.get<std::string>());
					}
				}
			}
			wake();
		});
	});
	state.thread = std::thread([&state]() {
		for (;;) {
			sync_once();
			std::unique_lock<std::mutex> wait(state.mutex);
			if (state.wake.wait_for(wait, std::chrono::seconds(2), [&state]() { return state.is_stopping || state.is_woken; }) == true) {
				if (state.is_stopping == true) return;
				// A burst - a colleague writing many records - is one round; a short
				// wait, since every round costs its readers a moment.
				state.wake.wait_for(wait, std::chrono::milliseconds(25), [&state]() { return state.is_stopping; });
				if (state.is_stopping == true) return;
				state.is_woken = false;
			}
		}
	});
}

void wake() noexcept {
	try {
		platform::trace::record("coordination", "wake");
		Rounds &state = rounds();
		{
			const std::lock_guard<std::mutex> lock(state.mutex);
			state.is_woken = true;
		}
		state.wake.notify_all();
	} catch (...) {
	}
}

void stop() noexcept {
	Rounds &state = rounds();
	std::thread thread;
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		if (state.is_running == false) return;
		state.is_stopping = true;
		state.is_running = false;
		thread = std::move(state.thread);
	}
	state.wake.notify_all();
	if (thread.joinable() == true) thread.join();
}

} // namespace singularity::matter::coordination
