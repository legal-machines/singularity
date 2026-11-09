#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Who may use Singularity's Office connection. Reaching the loopback port
// proves nothing: any program on this computer can. So a pane is paired once,
// as a device is.
//
//   1. the pane asks to pair, for Word, Excel or Outlook; it is shown a code,
//      and is handed a one-time secret only it holds;
//   2. Singularity's own window shows the person the same code and asks
//      whether to allow it - nothing is allowed without that answer;
//   3. the pane collects its token with the secret.
//
// Each call then carries the token, a moment and a nonce used once. The token
// is kept here only as its SHA-256, it ends after 90 days or when the person
// removes the client, and it reaches only the methods its kind of client
// needs (may_call). Which Matter a call names is checked by the connection
// against the Matters open on this computer.
namespace singularity::office {

enum class ClientKind { word, excel, outlook, powerpoint };

std::optional<ClientKind> client_kind_of(const std::string &word);
const char *word_of(ClientKind kind);

// The Native API methods each kind of client may call: a document's and a
// workbook's Matter, threads, reviews and colleagues; Outlook's filing and
// attaching. Never a provider, a key, a setting, a model, the window, or the
// connection's own pairing and removal.
bool may_call(ClientKind kind, const std::string &method);

struct PairingStart {
	std::string id;
	// Six digits, shown in the pane and in Singularity's window.
	std::string code;
	// Held by the pane alone; it collects the token with it.
	std::string secret;
};

struct PendingPairing {
	std::string id;
	ClientKind kind = ClientKind::word;
	std::string label;
	std::string code;
	std::string requested_at;
};

struct PairingOutcome {
	// "waiting", "allowed", "declined", "expired" or "unknown".
	std::string state;
	// The token, once, when the pairing was allowed.
	std::string token;
	std::string client_id;
};

struct PairedClient {
	std::string id;
	ClientKind kind = ClientKind::word;
	std::string label;
	std::string paired_at;
	std::string last_used_at;
	std::string expires_at;
	std::string token_sha256;
};

// Whether any of `clients` is a pane of Word, Excel or PowerPoint: one that
// reached Singularity through its add-in for documents, which is then in Office
// whatever version of Singularity placed it.
bool has_document_pane(const std::vector<PairedClient> &clients);

class OfficeClients final {
public:
	// The clients are kept in `folder` (clients.json, this user's alone).
	explicit OfficeClients(std::filesystem::path folder);

	// A pane asks to pair. Nothing when too many ask at once or one was just
	// refused: a program cannot keep the person's window asking.
	std::optional<PairingStart> begin_pairing(ClientKind kind, const std::string &label);

	// What waits for the person's answer.
	std::vector<PendingPairing> pending();

	// The person's answer, from Singularity's window only.
	bool decide_pairing(const std::string &pairing_id, bool allow);

	// The pane collects its token with its secret, once.
	PairingOutcome finish_pairing(const std::string &pairing_id, const std::string &secret);

	// The client a token belongs to, while it runs; its use is recorded.
	std::optional<PairedClient> authorize(const std::string &token);

	// Whether a call's moment is now (within two minutes) and its nonce has not
	// been used by that client before. A call that fails it is not answered.
	bool is_fresh(const std::string &client_id, const std::string &moment_milliseconds, const std::string &nonce);

	std::vector<PairedClient> clients();
	bool revoke(const std::string &client_id);
	void revoke_all();

private:
	struct Pending {
		PendingPairing request;
		std::string secret_sha256;
		std::chrono::steady_clock::time_point expires;
		// "waiting", "allowed" or "declined".
		std::string state = "waiting";
		bool collected = false;
	};

	void load_locked();
	void save_locked();
	void forget_expired_locked();

	std::filesystem::path folder_;
	std::mutex mutex_;
	bool loaded_ = false;
	std::vector<PairedClient> clients_;
	std::map<std::string, Pending> pending_;
	std::chrono::steady_clock::time_point quiet_until_{};
	// Nonces seen, by client, with when they were seen.
	std::map<std::string, std::map<std::string, std::chrono::steady_clock::time_point>> nonces_;
};

} // namespace singularity::office
