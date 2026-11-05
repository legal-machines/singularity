#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::node::work_log {

/*
 * A design partner's record of its model calls (D-257; the owner,
 * 2026-11-05: "full logs are not what everyone has in the release build: they
 * are on only for accounts marked as design partners, and only when they
 * decide to send a particular chat or thread").
 *
 * While this computer's seat belongs to a design partner (account.ownSeat's
 * designPartner), every request Singularity sends a model for a chat - a
 * conversation, a thread, a colleague's work in it - is kept here with the
 * answer as it came: the body exactly as it left, past the final check of
 * client data protection (masked where protection was on), never the text
 * before protection or the map from masked values to real ones (AG-SEC-3),
 * never a credential. Kept on this computer alone, under its own records
 * (`Work Log/`, one file for each chat), for 14 days; nothing leaves it until
 * a member chooses to send that one chat's log, sealed to Legal Machines
 * (Node/OpenPgpMessage). Every other computer keeps nothing, and turning the
 * record off erases what it kept.
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
};

// Where the record lives: this computer's own records' `Work Log` folder,
// which the app names as it starts. Until it is named nothing is kept, so a
// program that never names it (a test, a tool) records nothing.
void set_directory(const std::filesystem::path &directory);

// Whether this computer keeps the record: said from the seat's own record
// each time it is read, and kept across a restart. Turned off, every call
// kept is erased.
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

// The calls kept for a chat, oldest first, from the last 14 days.
std::vector<Call> calls_of(const std::string &matter_id, const std::string &chat_id);

} // namespace singularity::node::work_log
