#pragma once

#include "Matter/CoordinationSeal/CoordinationSeal.hpp"
#include "Matter/CoordinationTransport/CoordinationTransport.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace singularity::matter::coordination {

/*
 * A firm's shared Matters carried between its computers directly, sealed end to
 * end, by whatever carries the firm's envelopes: the firm's own folder in its
 * storage now, a direct link next. Nothing passes through Legal Machines.
 *
 * Each computer keeps the Matter in its own folder and sends what changed in it -
 * its documents and the Matter's `.lm` records, never a cache, an index, a lock
 * or a file half written - sealed under the Matter's key. What it takes from
 * the others it writes as the folder's own files, so the rest of Singularity
 * reads them as it reads any change to the folder.
 *
 * The way in, by the lawyers' check.
 * 1. a computer asks to join (0x03), with its own keys;
 * 2. on a computer that carries the Matter its lawyer compares the asking
 * computer's code and confirms it; that computer then gives it the Matter's
 * key (0x02) and the Matter whole;
 * 3. on the joining computer its lawyer compares the giving computer's code and
 * accepts the key; until then nothing is taken.
 *
 * What this computer knows of it - the firm's folder, the Matters it carries, how
 * far each has come, who asked and who offered - is this computer's own record
 * (`Coordination.json`), never a Matter's. Keys are in the credential store: this
 * computer's own and each Matter's.
 */

// A computer of a Matter's member that the person here confirmed as that
// member's: the member's actor id and the computer's device key.
struct ConfirmedComputer {
	std::string actor_id;
	std::string device_key;
	// The organisation the member's record in the Matter names, by Legal
	// Machines' id for it: the firm's record goes only to the computers of
	// this computer's own. Two firms of one name are two firms; a record that
	// names no id is of no firm.
	std::string organisation_id = {};
};

// A computer of one of the firm's seats that a lawyer here checked: its
// member, its device key and the X25519 key the check confirmed, and where
// its direct link may be reached. It counts as the firm's, as one confirmed
// in a shared Matter does.
struct CheckedFirmComputer {
	std::string actor_id;
	std::string device_id;
	std::string device_key;
	std::string coordination_key;
	std::vector<std::string> candidates;
	// The organisation it was checked in, by Legal Machines' id: it counts as
	// the firm's only while this computer is signed in to that one.
	std::string organisation_id = {};
};

// One of the firm's model accounts: the account as this computer's settings
// keep it (Platform/AppSettings connected_provider_json, no key in it) and
// its key from the credential store. The engine never writes either down:
// it seals them to a computer a lawyer here checked, or hands what it
// opened to keep_account.
struct FirmAccount {
	std::string provider_id;
	std::string entry;
	std::string credential;
};

// What an AI colleague's card in a Matter says of the computer that runs it,
// as that computer signed it: checked here against the id's binding to its
// key and against the device key a lawyer here confirmed for that computer.
struct AgentHostClaim {
	std::string host_device_id;
	std::string host_device_key;
	std::string id_nonce;
	std::string responsible_actor_id;
	std::int64_t counter = 0;
	std::string signature;
};

// A Matter on its way between this computer and another: how much of it
// there is and how much has come - to this computer from that one, or to
// that one from this. Sent, its files are the envelopes that carry it.
struct Transfer {
	std::string device_id;
	std::uint64_t files = 0;
	std::uint64_t files_here = 0;
	std::uint64_t bytes = 0;
	std::uint64_t bytes_here = 0;
	// Whether the direct link holds a live session with that computer now.
	bool reachable = false;
};

// What the coordination works through. The application installs its own
// (MatterCoordinationProduction.cpp); a test installs one of its own.
struct Environment {
	std::filesystem::path records;
	std::function<std::string()> device_id;
	// This computer's device key (Ed25519, base64), its signature and the check
	// of another's (Platform/DeviceKey), and the code people compare.
	std::function<std::string()> device_key;
	seal::Sign sign;
	seal::Verify verify;
	std::function<std::string(const std::string &device_key)> code_of;
	// The member at this computer (an actor id) and their name, which goes inside
	// the seal so a copy kept beside a document says whose it is.
	std::function<std::string()> actor_id;
	std::function<std::string()> person_name;
	// A secret kept in the credential store, by name.
	std::function<std::optional<std::string>(const std::string &name)> secret;
	std::function<void(const std::string &name, const std::string &value)> keep_secret;
	// What carries the firm's envelopes, given the firm's folder this computer
	// was set to (empty: none set) and this computer's own keys, which its
	// direct link speaks with.
	std::function<std::vector<std::shared_ptr<transport::Transport>>(const std::filesystem::path &firm_folder,
		const seal::ComputerKeys &keys)> transports;
	std::function<std::string()> now;
	// This computer's clock, in microseconds since 1970: where a change's counter
	// starts from. None: the system's.
	std::function<std::int64_t()> clock_micros;
	// Where this computer's direct link may be reached ("udp:192.168.1.20:47380"),
	// told the Matter's other computers in its address card; none, and no card is
	// sent.
	std::function<std::vector<std::string>()> candidates;
	// What a checked card of another of a Matter's computers says: where its
	// direct link may be reached, and its X25519 key.
	std::function<void(const seal::AddressCard &card)> learned;
	// The device key of a computer of the Matter in `matter_folder` that the
	// person here confirmed as a colleague's (Tool/DeviceTrust), as the Matter's
	// member records state it; nothing when none is.
	std::function<std::optional<std::string>(const std::filesystem::path &matter_folder, const std::string &device_id)>
		confirmed_device_key;
	// Whether the person here refused a device key as anyone's computer
	// (Tool/DeviceTrust): its cards teach nothing, whoever let it in.
	std::function<bool(const std::string &device_key)> refused_device_key;
	// The person here compared the code of the computer with `device_key` when
	// they took the Matter in `matter_folder` (accept_key). Once one member's
	// record there states that key, it counts as that member's computer
	// (Tool/DeviceTrust), as comparing codes under a message would; a computer
	// the person here refused stays refused. True once it counts.
	std::function<bool(const std::filesystem::path &matter_folder, const std::string &device_key)> compared_when_taken;
	// The member whose computer `device_id` is, among the members of the Matter
	// in `matter_folder`, when the person here confirmed it as theirs
	// (Tool/DeviceTrust) and has not refused it: what a personal channel's key
	// goes by. Nothing for any other computer.
	std::function<std::optional<ConfirmedComputer>(const std::filesystem::path &matter_folder, const std::string &device_id)>
		confirmed_computer;
	// The firm's computers a lawyer here checked: colleagues of this
	// computer's own firm, for its record and its members' personal channels.
	std::function<std::vector<CheckedFirmComputer>()> checked_firm_computers;
	// The firm's model accounts connected on this computer, which it gives,
	// sealed, to each of the firm's computers a lawyer here checked; and an
	// account one of those computers gave, kept here - its entry in this
	// computer's settings, its key in its credential store - or taken away
	// when `removed`.
	std::function<std::vector<FirmAccount>()> accounts_here;
	std::function<void(const FirmAccount &account, bool removed)> keep_account;
	// How often the accounts here are looked at, besides at once for a computer
	// met again: their keys are not read every round.
	std::chrono::seconds accounts_every {30};
	// The signed card of an AI colleague in the Matter in `matter_folder`;
	// nothing when it has none, or an older one with no signature.
	std::function<std::optional<AgentHostClaim>(const std::filesystem::path &matter_folder, const std::string &agent_id)>
		agent_host_claim;
	// The AI colleagues this computer runs (Agent/AgentStore), by agent id: it
	// carries their personal channels as theirs.
	std::function<std::vector<std::string>()> hosted_agents;
	// A Matter whose invitation was given lately: until `until_seconds` (since
	// 1970) a computer no card names may call this one about it, only to ask to
	// join.
	std::function<void(const std::string &matter_id, std::int64_t until_seconds)> open_lobby;
	// A computer whose request to join the lawyer here refused, by its X25519
	// key (key_text): its direct link speaks with it no more.
	std::function<void(const std::string &coordination_key)> refused;
	// The computers of a Matter the direct link holds a live encrypted session
	// with now, by device. None where it is not installed.
	std::function<std::vector<std::string>(const std::string &matter_id)> connected;
	// The organisation this computer's member is signed in to, by Legal
	// Machines' id for it: whose firm's record it carries, and the one firm
	// its computers are of. The firm's record and its key go only to a
	// computer of the same id, and are taken only from one; never by a name,
	// which two firms may share. None or empty: no firm, and the firm's record
	// goes nowhere.
	std::function<std::string()> organisation_id;
	// Whether the Matter in `folder` is sealed for another organisation than
	// the one this computer's member is signed in to: such a Matter is not
	// carried, to anyone or from anyone, while it is. None: every Matter is
	// carried.
	std::function<bool(const std::filesystem::path &folder, const std::string &matter_id)> is_other_organisations;
	// What the direct link carries of a Matter to each other computer and that
	// computer confirmed. None where it is not installed.
	std::function<std::vector<Transfer>(const std::string &matter_id)> sending;
	// The sealed bytes come for a Matter that wait for its key here.
	std::function<std::uint64_t(const std::string &matter_id)> sealed_here;
};
void install_environment(Environment environment);

// The application's: this computer's records folder, its device and device key,
// the credential store, and the firm's folder as a transport
// (MatterCoordinationProduction.cpp).
Environment production_environment();
// The organisation this computer's member is signed in to, by Legal Machines'
// id, as the application's settings say it: read at most once a minute; empty
// when none (MatterCoordinationProduction.cpp). Everything of a firm here is
// told apart by it, never by the firm's name.
std::string signed_in_organisation_id();

// A computer that asks to join a Matter this computer carries.
struct AskingComputer {
	std::string actor_id;
	std::string device_id;
	std::string device_key;
	std::string coordination_key;
	std::string code;
	std::string at;
};

// A computer that offers this computer a Matter's key, waiting for its lawyer
// to compare the giving computer's code and accept it.
struct OfferingComputer {
	std::string device_id;
	std::string device_key;
	std::string code;
	// The Matter's size as the giving computer said it: what is coming before
	// any of it opens. Zero when not said.
	std::uint64_t files = 0;
	std::uint64_t bytes = 0;
};

struct LinkedMatter {
	std::string matter_id;
	std::string name;
	std::filesystem::path folder;
	// "carried" once this computer holds the Matter's key; "waiting" while it
	// has asked to join and holds no key yet.
	std::string state;
	std::string key_id;
	// What this computer and the others agree the folder holds: each path
	// (forward slashes, inside the folder) by its SHA-256.
	std::map<std::string, std::string> known;
	// The counter of the last envelope this computer sealed for the Matter,
	// always rising, and the highest seen from each other computer: a change
	// sent again is set aside.
	std::int64_t sent_counter = 0;
	std::map<std::string, std::int64_t> seen_counters;
	std::vector<AskingComputer> asking;
	std::vector<OfferingComputer> offering;
	// The device keys whose key gifts this computer's lawyer accepted.
	std::vector<std::string> accepted_senders;
	// The computers given the key here, by device: their coordination keys,
	// for a new key when someone leaves.
	std::map<std::string, std::string> given;
	std::string last_sync_at;
	std::string last_error;
	// For a personal channel: the other member's actor id. Empty for a Matter.
	std::string other_actor_id;
	// The members this computer gave the Matter to by choosing them, by actor,
	// with when, in seconds since the epoch.
	std::map<std::string, std::int64_t> offered_to;
	// Whether the Matter travels, as the lawyer is told it, for this run of the
	// program only and never written to the record: the other computers its
	// direct link holds a live encrypted session with, by device, and when this
	// computer last took a sealed envelope that opened and held from each other
	// computer.
	std::vector<std::string> connected;
	std::map<std::string, std::string> heard;
	// What of the Matter is on its way, for this run only and never written to
	// the record: coming here from each computer that said what it holds or
	// offered it, and going to each this computer sends it.
	std::vector<Transfer> receiving;
	std::vector<Transfer> sending;
};

struct CoordinationStatus {
	// The firm's folder this computer exchanges through; empty: none set.
	std::filesystem::path firm_folder;
	std::vector<std::string> transports;
	std::string device_code;
	std::vector<LinkedMatter> matters;
	// The personal channels of this computer's member, never among its Matters.
	std::vector<LinkedMatter> personal;
	// The firm's own record, never among the Matters or the member's
	// conversations: whether it travels, as a Matter's view says it. A
	// computer that waits for its firm tells by it a record heard from
	// another of the firm's computers from one that has not come yet.
	// Nothing while this computer holds no record of its firm.
	std::optional<LinkedMatter> firm;
};

// Sets the firm's folder this computer exchanges through: a folder of the
// firm's own storage that its computers keep in step (OneDrive or SharePoint,
// Dropbox, Google Drive, iCloud Drive, a NAS). Throws, in words for the lawyer,
// when it is not a folder this computer can write.
CoordinationStatus use_firm_folder(const std::filesystem::path &folder);

// Where the coordination stands.
CoordinationStatus status();

// This computer stops carrying a Matter: it sends and takes nothing of it any
// more. Its folder stays as it is, and the Matter's other computers keep it. A
// personal channel or a Matter not carried here is left as it is.
CoordinationStatus leave_matter(const std::string &matter_id);

// Carries a Matter open here to the firm's other computers: makes its key when
// it has none, and sends its folder on the next round.
CoordinationStatus share_matter(const std::string &matter_id, const std::string &name, const std::filesystem::path &folder);

// Asks to bring a firm's Matter to this computer, into `folder`: the Matter
// arrives once a computer that carries it has given this one its key and this
// computer's lawyer has accepted it.
CoordinationStatus join_matter(const std::string &matter_id, const std::string &name, const std::filesystem::path &folder);

// A personal channel: the private conversation of this computer's member and
// one other member. It is kept in this computer's own data beside its
// records, never in a Matter's folder or the firm's storage, and its key goes
// only to the two members' computers a lawyer here confirmed in a Matter they
// share, with no invitation. Its id is the same on every computer of the two,
// so either may make it and both find it.
struct PersonalChannel {
	std::string id;
	std::filesystem::path folder;
};

// "pc_" and 16 hex of the SHA-256 of the two actor ids, sorted, joined by a
// line break: the same whichever of the two asks.
std::string personal_channel_id(const std::string &actor_id, const std::string &other_actor_id);

// The channel of this computer's member and `other_actor_id`: made here with a
// key of its own when it is not yet, and carried from then on. Throws for an
// empty actor id, or the member's own.
PersonalChannel personal_channel(const std::string &other_actor_id);

// Listens for the channel of this computer's member and `other_actor_id`
// without making it: its key and files come once a computer of the two that
// carries it gives them. For a member whose colleagues' computers are not
// known from a shared Matter - an AI colleague, whose channel goes to the
// member's own computers - the window says which channels to listen for.
// Throws as personal_channel does.
void listen_for_personal_channel(const std::string &other_actor_id);

// The firm's own record: what every one of the firm's computers holds alike -
// its AI colleagues' settings and instructions, what they remember of the firm
// and its members, each member's read marks - carried as a personal channel
// is, sealed, to every computer of the firm a lawyer here confirmed in a
// Matter they share, with no invitation. It is kept in this computer's own
// data, as a `.lm` of its own, never offered or listed as a Matter or as a
// member's conversation. Its id is the same on every computer of the firm.
std::string firm_channel_id(const std::string &organisation_id);
// The firm's record here: made with a key of its own when it is not yet, and
// carried from then on.
PersonalChannel firm_channel();

// This computer's own X25519 key, its public half as text, and where its
// direct link may be reached now: what it says of itself to the firm's other
// computers.
std::string own_coordination_key();
std::vector<std::string> own_candidates();

// What a lawyer gives a colleague to bring a Matter to their computer, as one
// line of text: the Matter's id and name, and where this computer's direct
// link may be reached, with its device and its X25519 key, so the colleague's
// computer calls it with no folder between. "LMM1:..." (id and name alone) is
// still read. Nothing in it is secret: the key goes only by the lawyers'
// check, and a changed invitation shows there, in the code of the computer
// that offers the key. Giving it opens the lobby for an hour.
std::string invitation(const std::string &matter_id);
struct Invitation {
	std::string matter_id;
	std::string name;
	std::string device_id;
	std::string coordination_key;
	std::vector<std::string> candidates;
};
std::optional<Invitation> read_invitation(const std::string &text);

// Asks, by an invitation, to bring a Matter to this computer, into `folder`:
// as join_matter, and, where the invitation names the inviting computer's
// direct link, by calling it there.
CoordinationStatus join_matter(const Invitation &invitation, const std::filesystem::path &folder);

// The lawyer here refused a computer that asked to join: its request goes,
// and its direct link is spoken with no more.
CoordinationStatus refuse(const std::string &matter_id, const std::string &device_id);

// After the lawyer here confirmed an asking computer: gives it the Matter's
// key, sealed to its own key, and sends it the Matter whole. Throws, in words
// for the lawyer, when no such computer asked or this computer holds no key.
CoordinationStatus give_key(const std::string &matter_id, const std::string &device_id);

// After the lawyer here compared the code of a computer that offers the
// Matter's key: takes the key on the next round. Throws for a computer that
// offers none.
CoordinationStatus accept_key(const std::string &matter_id, const std::string &device_id);

// A Matter given to a member of the firm by choosing them: their computers
// that a lawyer here checked (Matter/FirmComputers) - or confirmed in a
// Matter they share - are told to the direct link as the Matter's and, as
// they ask to join with the very keys confirmed, given its key, with no code
// compared: the lawyer chose that member. Returns the Matter's invitation,
// for the offer that goes to the member in the two members' personal
// channel. Throws, in words for the lawyer, when this computer does not
// carry the Matter.
std::string offer_matter(const std::string &matter_id, const std::string &actor_id);

// What of a Matter travels, as its snapshot would send it.
struct MatterSize {
	std::uint64_t files = 0;
	std::uint64_t bytes = 0;
};
MatterSize matter_size(const std::string &matter_id);

// Takes a Matter a member of the firm offered, by the invitation the offer
// carries, from the computer whose device key is `giver_device_key`, one a
// lawyer here checked: as join_matter, with that computer's key gift taken
// as it comes, since the lawyer here accepted the offer.
CoordinationStatus take_offered_matter(const Invitation &invitation, const std::string &giver_device_key,
		const std::filesystem::path &folder);

// One round for every Matter this computer carries or waits for. Never throws;
// a failure is kept on the Matter it was for.
void sync_once();

// Rounds on a thread of its own, every two seconds, from start() to stop().
void start();
void stop() noexcept;
// A round at once, not at the next tick: something came for a Matter, or this
// computer wrote to one. Wakes merge into one round; never throws.
void wake() noexcept;

// Whether a path inside a Matter's folder travels: never a cache, an index or a
// lock of `.lm`, a staged or temporary file, an Office lock file or a system
// file.
bool travels(const std::string &relative_path);

// Whether this computer carries the Matter `matter_id` in `folder`: its lawyer
// accepted the Matter's key from a computer they confirmed, or this computer
// made the key. Read from the records as last written, apart from any round,
// so a Matter found on disk never waits behind a sync.
bool carries(const std::string &matter_id, const std::filesystem::path &folder);

// A Matter this computer carries with the firm's other computers, in its folder
// here.
struct CarriedMatter {
	std::string matter_id;
	std::filesystem::path folder;
};

// Every Matter this computer carries, read as `carries` reads them; never a
// personal channel.
std::vector<CarriedMatter> carried_matters();

} // namespace singularity::matter::coordination
