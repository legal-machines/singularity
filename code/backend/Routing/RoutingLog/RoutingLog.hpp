#pragma once

#include "PapinianGuard/Types/PrivacyMetadata.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Usage/Types/ProcessingTelemetry.hpp"
#include "Usage/Types/UsageRecord.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace singularity::routing {

// One safe-field-only record of a real task attempt against a provider -
// Master Spec Sec.62. Never carries prompt/response text, an API key, a
// Papinian request namespace, or a restoration-map entry; see
// append_routing_log_entry().
struct RoutingLogEntry {
  std::string matter_id;
	// Exact durable MatterTask attempt identity. Kept beside task_id because
	// task_id intentionally identifies the logical retry chain.
	std::string operation_id;
  // The stable id of the *logical* task/retry chain (Master Spec Sec.66/67:
  // WP5-D's matters::task_group_id()), not necessarily this attempt's own
  // matters::MatterTask::id. For a task's first attempt the two happen to
  // be equal (there is no other attempt yet to distinguish them from); for
  // a retried attempt they differ - task_id stays the chain's original id
  // while the attempt gets its own distinct MatterTask::id. This is what
  // lets every attempt in a chain share one task_id while `attempt`
  // increments, so a routing-log consumer can group them without
  // cross-referencing matter.tasks.
  std::string task_id;
  // 1 for a task's first attempt; incremented for each Retry/Run-with-
  // another-model attempt sharing the same task_id (WP5-D) - see
  // matters::next_attempt_number().
  int attempt = 1;
  std::string time;
  std::optional<std::string> provider_id;
  std::optional<std::string> model_id;
	std::string execution_kind;
  std::string legal_task_preset_id;
  int legal_task_preset_version = 1;
  int document_count = 0;
  int chunk_count = 0;
  usage::ProcessingTelemetry processing;
  protected_api::papinian::PrivacyMetadata privacy_metadata;
  bool success = false;
  long long latency_ms = 0;
  providers::UsageMetadata usage;
  std::optional<std::string> finish_reason;
  // Set only when !success - the same stable code matters::MatterTask::
  // error_code carries, never the free-text message. The one exception is
  // privacy_metadata.decision_reason: Papinian Guard's own refusal, in its
  // fixed words for kinds and places, never a value or a provider's text.
  std::optional<providers::TaskErrorCode> error_code;
  // Auto routing (Master Spec Sec.61) is not built yet (WP5-E is deferred
  // until WP5-A-D are green and a deterministic rule exists) - always
  // nullopt until it does.
  std::optional<std::string> routing_reason;
	// True for an entry written with its outbound record (privacy_metadata.
	// outbound_calls, possibly empty because nothing left); false for one
	// written before Singularity kept that record, or one whose attempt's calls
	// could not be told from another request's: its calls are unknown.
	bool outbound_calls_recorded = false;
};

// What left this device for one operation - a chat turn, a Consilium seat -
// read from every Routing Log entry the operation wrote (the proposal and the
// write once the lawyer approved it are two runs of one operation). The
// source peek, the Guard page and a turn's "What left this device" all read
// this, and nothing else.
struct OutboundRecord {
	std::string operation_id;
	// The latest run's time and route: where the operation's answer came from.
	std::string time;
	std::string execution_kind;
	std::optional<std::string> provider_id;
	std::optional<std::string> model_id;
	std::string protection_mode = "off";
	std::size_t protected_count = 0;
	std::map<std::string, std::size_t> category_counts;
	// False when a run was written before calls were recorded: then only the
	// route, the protection and the counts above are known.
	bool calls_recorded = true;
	// Every call, in the order made.
	std::vector<protected_api::papinian::OutboundCall> calls;
	// What kind of work the operation was, when its task says: "matter_task"
	// for a chat turn, "context_automation", "memory_keeping".
	std::string operation_type;
	// The Project identities whose values the operation's requests replaced,
	// for a record whose markers were not kept. Read only when
	// protected_entities_whole is true: the list then names every Project
	// identity a marker stood in for, so one outside it was never replaced.
	std::vector<std::string> protected_entity_ids;
	bool protected_entities_whole = false;
};

// What an operation's own durable task says about it, read beside the Routing
// Log for what the log cannot say by itself: an operation that writes no entry
// of its own (a Context update, the upkeep of the Project's memory), and a web
// search or page an attempt ran that its entry missed (written on another
// thread, or after Singularity was opened again). Never instead of the log.
struct OperationTrace {
	std::string operation_id;
	std::string operation_type;
	std::string time;
	std::string execution_kind;
	std::optional<std::string> provider_id;
	std::optional<std::string> model_id;
	bool success = false;
	// The provider reported usage for it: a request that billed reached it.
	bool reported_usage = false;
	protected_api::papinian::PrivacyMetadata privacy_metadata;
	// Each web search or page the task's tool evidence says a research
	// provider was called for, in the order made.
	std::vector<protected_api::papinian::OutboundCall> research_calls;
};

// One passage of an operation's protected requests exactly as it left this
// device (protected_api::papinian::OutboundPassage), kept once however many
// of the operation's requests carried it in that same form.
struct SentPassage {
	std::string kind;
	std::string chunk_id;
	std::string content_sha256;
	std::string sent;
	// The protection table its markers were numbered by (OutboundCall::
	// symbol_scope): the same text under two tables is two passages.
	std::string symbol_scope;
	// The requests that carried it: when, and to which provider and model.
	std::vector<protected_api::papinian::OutboundCall> calls;
};

// Everything an operation's protected requests sent, passage by passage: kept
// beside the Routing Log in "<workspace>/.singularity-workspace/outbound/<project>/", one
// file per operation, written with the operation's entries, and removed with
// the Project's data (forget_project_sent_passages). Only what left - markers
// and all - never what a marker stood for, never the text a request was built
// from. `is_recorded` is false for an operation that has no such file: one
// written before passages were kept, one that sent none, one let go when all
// that is kept passed its ceiling (the oldest go first), or one whose file
// cannot be read. What a marker stood for is read beside the Project's own
// text when asked (Privacy/PassageDisclosure), never kept here.
struct SentPassages {
	std::string operation_id;
	std::string matter_id;
	bool is_recorded = false;
	std::vector<SentPassage> passages;
};

// A per-request token/latency projection of a RoutingLogEntry - Master Spec
// Sec.63. Deliberately not a second persisted store: every field Sec.63 asks
// for is already durable on the RoutingLogEntry the same request produces,
// so a second write path would just persist the same tokens/latency twice
// under a different schema. Usage is this query view over the one durable
// log, which is what makes it "distinct from the single embedded snapshot
// on MatterTask" - a real per-request record, not the last-task-only field
// on the Matter itself.
usage::UsageRecord to_usage_record(const RoutingLogEntry &entry);

// "<workspace>/.singularity-workspace/routing-log.json"
std::filesystem::path routing_log_path(const std::filesystem::path &workspace_path);

// Empty when no log exists yet. A corrupt primary file recovers from
// ".previous" exactly like matters::load_matters(); an unrecoverable file
// throws std::runtime_error rather than silently returning an empty log.
std::vector<RoutingLogEntry> load_routing_log(const std::filesystem::path &workspace_path);

// Loads the current log, appends `entry`, and saves atomically (unique temp
// file, checked flush/close, atomic rename, ".previous" recovery copy - the
// same durable-store pattern matters::save_matters() uses; see ADR-010). A
// corrupt primary file that load_routing_log() recovered from ".previous" is
// overwritten with fresh data on the next successful append rather than
// blocking it - telemetry has nothing worth force-preserving the way a
// corrupt Matter manifest does. Throws std::runtime_error on failure. This
// is operational telemetry, not legal work product: callers must not let a
// failure here block or roll back an already-persisted MatterTask (see
// native_bridge.cpp's matter.runTask, which appends the log entry in its
// own try/catch after the task outcome is already decided).
//
// The entry is written with its operation's whole outbound record: every call
// noted under the operation (OutboundRecord.hpp) since it last wrote one, and
// every call noted on the calling thread in the `latency_ms` the attempt took
// - a failed or local attempt keeps no result that names its operation. It is
// called on the thread that ran the attempt. The calls' passages as they left
// go to the operation's sent-passage file (SentPassages), never into the log;
// a file that cannot be written leaves the entry written.
void append_routing_log_entry(const std::filesystem::path &workspace_path,
                              const RoutingLogEntry &entry);

// What became of the entry's request at the protection boundary, where its
// writer did not say: "sent", "local", "blocked" (Papinian Guard refused it,
// or could not protect it) or "failed". The mode a failed request ran under is
// its writer's to say, which knows the policy it was held to.
// append_routing_log_entry applies it to every entry.
void settle_protection_decision(RoutingLogEntry &entry);

// "<workspace>/.singularity-workspace/outbound/<project>/<operation>.json"
std::filesystem::path sent_passages_path(const std::filesystem::path &workspace_path, const std::string &matter_id,
		const std::string &operation_id);

// The operation's sent passages, kept for `matter_id`; not recorded when there
// is no file, it cannot be read, or it was written for another Project.
SentPassages load_sent_passages(const std::filesystem::path &workspace_path, const std::string &matter_id,
		const std::string &operation_id);

// Lets go of everything kept of what the Project's requests sent - with its
// data, or with the Project. Never throws.
void forget_project_sent_passages(const std::filesystem::path &workspace_path, const std::string &matter_id);

// The Project's outbound records, newest first, at most `limit` operations.
std::vector<OutboundRecord> outbound_records(const std::vector<RoutingLogEntry> &entries,
		const std::string &matter_id, std::size_t limit);

// The same, completed by the Project's own tasks (`traces`): a task that sent
// something and wrote no entry is a record of its own, whose model calls are
// not known; a web search or page its task kept that its entry missed is added
// to its record. `isTruncated`, when given, says whether older operations were
// left out by `limit`.
std::vector<OutboundRecord> outbound_records(const std::vector<RoutingLogEntry> &entries,
		const std::vector<OperationTrace> &traces, const std::string &matter_id, std::size_t limit,
		bool *isTruncated);

} // namespace singularity::routing
