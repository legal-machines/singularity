#pragma once

#include "Agent/AiAppChannel/AiAppChannel.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace singularity::agents {

// The AI apps the lawyer connected to Singularity (the firm's name, AI apps), and
// what each may do. A connection is made only by the lawyer, here: it names
// the Matters the app may see, and gives the app one key, shown once. With
// it the app may
//
//   - list those Matters and their agents (names and states, nothing of
//     their documents or results);
//   - propose an agent for one of them - a draft that does nothing until the
//     lawyer approves it in Singularity, choosing its folder, schedule and
//     model there;
//   - read how an agent's runs went - their state and counts, never the
//     documents' names or words;
//   - pause an agent, only when the lawyer allowed it when connecting the app.
//
// It may not read a document, choose a folder or a permission, run, change,
// resume or approve an agent, or reach anything outside those Matters. Every
// request is decided here, by Singularity, against the connection as it is
// now: a disconnected app is refused at its next request. Who asked, who
// approved and what ran are kept apart (Agent::requested_by,
// approved_by_actor_id, and the run's own record).

struct AiAppConnection {
	int format_version = 1;
	// "aic_" and 16 hex characters.
	std::string id;
	// The lawyer's own name for it: "Claude Code".
	std::string label;
	std::vector<std::string> matter_ids;
	// Whether the lawyer allowed the app to pause those Matters' agents.
	bool may_pause_agents = false;
	// The key's SHA-256; the key itself is shown once and never kept.
	std::string key_sha256;
	std::string created_at;
	std::string created_by_actor_id;
	std::optional<std::string> revoked_at;
	std::optional<std::string> last_used_at;
	// What the app called itself, in its own words, never verified.
	std::string last_client_name;
	// The program first seen using the key, when the system verified its
	// signature: later requests must come from the same one.
	std::string bound_team_id;
	std::string bound_signing_id;
	std::string bound_signer;
	int drafts_proposed = 0;
};

struct AiAppActivity {
	std::string at;
	std::string tool;
	bool is_refused = false;
	std::string detail;
};

// A new connection or a new key: the key, shown once, and the helper the app
// starts.
struct AiAppSetup {
	AiAppConnection connection;
	std::string key;
};

struct AiAppMatter {
	std::string id;
	std::string name;
};

struct AiAppDependencies {
	// The Matters on this computer.
	std::function<std::vector<AiAppMatter>()> list_matters;
};

void install_ai_app_dependencies(AiAppDependencies dependencies);

AiAppSetup connect_ai_app(const std::string &label, const std::vector<std::string> &matter_ids, const std::string &actor_id,
	bool may_pause_agents = false);
// A new key for the same connection; the old one stops working at once.
AiAppSetup renew_ai_app_key(const std::string &connection_id);
// Disconnects the app: its next request is refused, and the drafts it
// proposed that the lawyer has not decided are declined.
AiAppConnection revoke_ai_app(const std::string &connection_id);
std::vector<AiAppConnection> list_ai_apps();
// Newest first; at most `most`.
std::vector<AiAppActivity> ai_app_activity(const std::string &connection_id, std::size_t most);

// Where a key names its connection: "singularity_<connection id>_<64 hex>".
std::optional<std::string> connection_id_of_key(const std::string &key);
// The signer's organisation as a lawyer reads it: "Anthropic PBC".
std::string signer_organisation(const std::string &signer);

// Decides one request - the key, the connection's Matters and limits, the
// program that sent it - then answers it. Never throws: a refusal is an
// answer, in words the AI app can pass on.
AiAppAnswer answer_request(const AiAppRequest &request);

// Answers what the helpers leave, on its own thread, while Singularity is
// open, and says that it is open.
class AiAppInbox {
public:
	static AiAppInbox &shared();
	void start();
	void stop();
	// The connections changed: how often to look follows them.
	void connections_changed();
	~AiAppInbox();

private:
	AiAppInbox() = default;
	void loop();

	std::mutex mutex_;
	std::condition_variable wake_;
	std::thread thread_;
	bool is_started_ = false;
	bool is_stopping_ = false;
	std::atomic<bool> has_connections_{false};
};

} // namespace singularity::agents
