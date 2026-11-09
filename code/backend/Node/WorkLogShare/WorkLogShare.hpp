#pragma once

#include "Node/OpenPgpMessage/OpenPgpMessage.hpp"
#include "Node/WorkLog/WorkLog.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::node::work_log_share {

/*
 * A design partner's work log, sent to Legal Machines.
 *
 * One chat's calls from its work log (Node/WorkLog), with the conversation's
 * messages as the member sees them, what the member says went wrong and this
 * computer's facts, packed as a zip (log.json and calls/NNNN.json), signed by
 * this computer's device key, put in a letter Seal reads (multipart/mixed, its
 * headers inside), sealed on this computer to Legal Machines' keys, pinned here
 * by their fingerprints and never fetched (Node/OpenPgpMessage), and posted
 * with this computer's sign-in to the mail server's /intake/singularity/log,
 * which hands it to those addresses. Nothing of it is readable outside the
 * seal: the server sees its size and who sent it.
 */

// Where a sealed work log is posted.
inline constexpr const char *kIntakeUrl = "https://mail.legalmachines.org/intake/singularity/log";
// The most a sealed log may be, as the intake takes it (24 MB armored).
inline constexpr std::size_t kMostArmoredBytes = 24u * 1024 * 1024;

// Legal Machines' keys a work log is sealed to, as pinned in this build:
// diagnostics@legalmachines.org's, which Legal Machines opens in Seal, and
// claude@legalmachines.org's. Each is read from its published key and held to
// its pinned fingerprints; a key that does not hold to them is left out.
std::vector<openpgp::Recipient> recipients();
// Whether every pinned key is here: no log is sent to fewer than all of them.
bool recipients_ready();
// For tests: the keys to seal to instead of the pinned ones; nothing (nullopt)
// gives the pinned ones back.
void set_recipients_for_testing(std::optional<std::vector<openpgp::Recipient>> given);

// What a member sees before sending a chat's work log.
struct Preview {
	std::size_t calls = 0;
	std::size_t protected_calls = 0;
	std::size_t unprotected_calls = 0;
	std::size_t failed_calls = 0;
	// The first and the last call's time, UTC; empty with none.
	std::string from;
	std::string to;
	// What the calls take as kept, before they are packed.
	std::size_t bytes = 0;
	// Whether this computer can send it now, and why not in words.
	bool can_send = false;
	std::string reason;
};
Preview preview(const std::string &matter_id, const std::string &chat_id);

// One message of the conversation as the member's page shows it.
struct Message {
	std::string author;
	std::string at;
	std::string text;
};

// Who sends a log, and from what.
struct Facts {
	std::string organisation;
	std::string seat;
	std::string app_version;
	std::string build;
	std::string platform;
	// This computer's device key (base64), whose signature the letter carries.
	std::string device_public_key;
};

struct Request {
	std::string matter_id;
	std::string chat_id;
	std::string chat_title;
	// What the member says went wrong.
	std::string note;
	std::vector<Message> messages;
	Facts facts;
};

// What the intake said once it took the log.
struct Sent {
	std::string sha256;
	std::size_t bytes = 0;
	std::size_t recipients = 0;
};

// Signs with this computer's device key (base64 Ed25519).
using Signer = std::function<std::string(const std::string &message)>;
// Posts a body with this computer's sign-in: the answer's status and body.
using Poster = std::function<std::pair<long, std::string>(const std::string &url, const std::string &content_type,
	const std::string &body)>;

// The log packed as a zip, for the letter and for tests.
std::string zip_of(const Request &request, const std::vector<work_log::Call> &calls, const std::string &sent_at);
// The letter Seal reads, the zip and its signature attached, before sealing.
std::string letter_of(const Request &request, const std::vector<work_log::Call> &calls, const Signer &sign,
	const std::string &sent_at);
// Sends the chat's log: sealed to recipients(), posted with `post`. Throws
// std::runtime_error, in words a member reads, where it cannot be sent.
Sent send(const Request &request, const Signer &sign, const Poster &post);

} // namespace singularity::node::work_log_share
