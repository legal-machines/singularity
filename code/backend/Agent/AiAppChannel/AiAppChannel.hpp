#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace singularity::agents {

// How another AI app's requests reach Singularity on this computer, with no
// network listener of any kind: the Singularity helper that app starts
// (singularity-mcp, Agent/McpServer) leaves each request as a file in this
// computer's own application data, and the open Singularity answers it there.
//
//   AI Apps/open.json                 Singularity is open: its process, and
//                                     when it last said so
//   AI Apps/inbox/<request id>.json   a request, written whole by one rename
//   AI Apps/taken/<request id>.json   a request Singularity claimed, for
//                                     the moment it takes it
//   AI Apps/outbox/<request id>.json  its answer, written the same way
//   AI Apps/connections/<id>.json     the apps the lawyer connected
//                                     (Agent/AiAppConnections)
//   AI Apps/activity/<id>.jsonl       what each one was answered
//
// The folder is the user's own and readable by no one else. A request carries
// its connection's key, so Singularity removes it as soon as it is read.
// Nothing here leaves this computer. Any program of this user can write here,
// so what Singularity reads is checked before it is trusted: these checks
// guard this channel only, never the Native Bridge.

std::filesystem::path ai_apps_directory();

// The program that started the helper, as the helper found it: its path, and
// its code signature when the system verified it.
struct AiAppCaller {
	std::string path;
	// The signing certificate's subject, "Developer ID Application:
	// Anthropic PBC (Q6L2SF6YDW)"; empty when not verified.
	std::string signer;
	std::string team_id;
	std::string signing_id;
	bool is_signer_verified = false;
};

struct AiAppRequest {
	std::string id;
	std::string key;
	std::string tool;
	nlohmann::json arguments = nlohmann::json::object();
	// What the app called itself when it connected - its own words, never
	// verified.
	std::string client_name;
	std::string client_version;
	AiAppCaller caller;
	std::string sent_at;
};

struct AiAppAnswer {
	std::string request_id;
	bool is_refused = false;
	// What the AI app reads.
	std::string text;
	nlohmann::json data = nullptr;
};

// A request id is 32 lowercase hex characters, and nothing else names a file
// here.
bool is_request_id(const std::string &value);
std::string new_request_id();

nlohmann::json request_to_json(const AiAppRequest &request);
std::optional<AiAppRequest> request_from_json(const nlohmann::json &value);
nlohmann::json answer_to_json(const AiAppAnswer &answer);
std::optional<AiAppAnswer> answer_from_json(const nlohmann::json &value);

// --- The helper's side --------------------------------------------------
void leave_request(const AiAppRequest &request);
// Removes a request Singularity has not taken yet; true when it was there,
// and then Singularity never takes it.
bool withdraw_request(const std::string &request_id);
// Reads an answer and removes its file; nothing while there is none.
std::optional<AiAppAnswer> take_answer(const std::string &request_id);
// Whether Singularity is open on this computer: its process lives and said
// so within the last `fresh_within`.
bool is_workspace_open(std::chrono::seconds fresh_within = std::chrono::seconds(45));

// --- Singularity's side -------------------------------------------------
void say_workspace_open();
void say_workspace_closed();
std::vector<std::string> waiting_request_ids();
// Reads a request and removes its file at once (it carries a key). It is
// first claimed under the file's own lock, which withdraw_request takes too
// (macOS and Linux: flock, then a move out of the inbox; Windows: a handle no
// other may hold while it is open), so a request is either taken here or
// withdrawn by its helper: never both, and never neither. Only a plain file of at most 64 KiB is read,
// never a link, a pipe or a folder, and its arguments must be flat (text,
// whole numbers, true or false, null); anything else is removed and refused.
// Its sent_at is the helper's, or, when the helper gave none that reads as a
// time, when its file was written.
std::optional<AiAppRequest> take_request(const std::string &request_id);
// Whether a taken request was sent more than `older_than` ago (or ahead):
// its helper gave up waiting (it waits 20 seconds) or is gone, so it is
// answered as expired and never acted on.
bool is_request_stale(const AiAppRequest &request, std::chrono::seconds older_than = std::chrono::seconds(30));
void leave_answer(const AiAppAnswer &answer);
// Answers no helper came for (it gave up waiting) are removed after a while,
// and so are requests no one took and any left half-taken.
void discard_stale_answers(std::chrono::seconds older_than);

} // namespace singularity::agents
