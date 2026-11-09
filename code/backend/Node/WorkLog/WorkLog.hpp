#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::node::work_log {

/*
 * A design partner's record of its model calls.
 *
 * While this computer's seat belongs to a design partner (account.ownSeat's
 * designPartner), every request Singularity sends a model for a chat - a
 * conversation, a thread, a colleague's work in it - is kept with the answer
 * as it came: the body exactly as it left, past the final check of client
 * data protection (masked where protection was on), never the text before
 * protection or the map from masked values to real ones, never a credential.
 * Kept for 14 days; nothing leaves the firm's computers until a member
 * chooses to send that one chat's log, sealed to Legal Machines
 * (Node/OpenPgpMessage). Every other computer keeps nothing, and turning the
 * record off erases what this computer kept.
 *
 * Where it is kept.
 * - a Matter's chat: in the Matter's own `.lm/WorkLog/`, one file a call,
 * which the Matter's exchange carries to each of its computers, so a member
 * on any of them sends the log of a thread its colleague worked in on
 * another computer;
 * - any other chat: in this computer's own records (`Work Log/`, one file for
 *   each chat), as the computer that answers it made its calls.
 */

// One model call, as it went and came back.
struct Call {
	// When it was sent, UTC, ISO 8601, and how long it took.
	std::string at;
	long long milliseconds = 0;
	std::string provider_id;
	std::string model_id;
	// What part of Singularity asked (the request's protection namespace).
	std::string purpose;
	// Sent under client data protection, its values masked.
	bool is_protected = false;
	// The request body exactly as it was sent.
	std::string request;
	// The answer as read, before anything was restored: its text, tool calls,
	// finish reason and the provider's id for it, as JSON.
	std::string answer;
	// Where the call failed, what failed; empty when it did not.
	std::string failure;
	long long input_tokens = -1;
	long long output_tokens = -1;
	// Which computer made it: a digest of its device id, one for all its
	// calls; empty for a call kept before the record said.
	std::string device;
};

// Where the record lives: this computer's own records' `Work Log` folder,
// which the app names as it starts. Until it is named nothing is kept, so a
// program that never names it (a test, a tool) records nothing.
void set_directory(const std::filesystem::path &directory);

// The Matters whose own folder keeps their chats' calls, as the app says them
// when it starts. Unsaid, every call stays in this computer's own records.
struct MatterRecords {
	// The folder of a Matter whose `.lm` this computer holds; nothing for any
	// other id (a personal chat's channel, a Matter not set up here).
	std::function<std::optional<std::filesystem::path>(const std::string &matter_id)> folder_of;
	// Every such folder: where turning the record off erases this computer's calls.
	std::function<std::vector<std::filesystem::path>()> folders;
	// This computer's device id, whose digest names its own calls' files.
	std::string device_id;
};
void set_matter_records(MatterRecords records);

// Whether this computer keeps the record: said from the seat's own record
// each time it is read, and kept across a restart. Turned off, every call
// this computer kept is erased: its own records, and its own calls in each
// Matter's folder (another computer's there are that computer's to erase).
void set_kept(bool keep);
bool is_kept();

// The chat the model calls of this thread are made for, while it lives:
// set where the work of one chat begins, and restored as it ends.
class ChatScope {
public:
	ChatScope(std::string matter_id, std::string chat_id);
	~ChatScope();
	ChatScope(const ChatScope &) = delete;
	ChatScope &operator=(const ChatScope &) = delete;

private:
	std::optional<std::pair<std::string, std::string>> previous_;
};

// Whether a call made now on this thread would be kept: the record is kept and
// a chat is in scope. Checked before anything is copied for it.
bool wanted();

// Keeps a call for the chat in scope; nothing when not wanted().
void record(const Call &call);

// Now, UTC, as a call's `at` says it.
std::string now_text();

// The calls kept for a chat, oldest first, from the last 14 days: this
// computer's own and, for a Matter's chat, every computer's in its folder.
std::vector<Call> calls_of(const std::string &matter_id, const std::string &chat_id);

} // namespace singularity::node::work_log
