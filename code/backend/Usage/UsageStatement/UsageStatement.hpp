#pragma once

#include "Usage/UsageAccounting/UsageAccounting.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace singularity::usage {

enum class StructuredStatementFormat { Json, Csv };

struct StatementBundleResult {
  std::filesystem::path pdf_path;
  std::filesystem::path structured_path;
  std::string structured_format;
  std::string generated_at;
  std::string content_hash;
  long long operation_count = 0;
};

[[nodiscard]] StructuredStatementFormat
structured_statement_format_from_string(const std::string& value);
[[nodiscard]] nlohmann::json build_statement(const UsageSummary& summary,
                                             const std::string& project_id,
                                             const std::string& generated_at);
[[nodiscard]] bool verify_statement_hash(const nlohmann::json& statement);

// Writes one PDF and one structured companion from the same in-memory
// statement and therefore the same manifest/content hash. Existing files are
// never overwritten. Generated artifacts are exports, not Project state.
[[nodiscard]] StatementBundleResult
write_statement_bundle(const std::filesystem::path& destination_directory,
                       const nlohmann::json& statement,
                       StructuredStatementFormat structured_format);

// ---- The AI work statement (D-252) --------------------------------------------------

// What a firm sends its client with the invoice: each AI request on a Matter in
// a period - when, whose work, at whose asking, on which model, at the
// provider's exact price - with each model call's evidence (the provider's own
// id for it, the fingerprints of exactly what was sent and what came back),
// signed by this computer's key. Never a word of what a request said. A PDF
// to print, and a signed file anyone can check without sending it anywhere
// (docs/AI_WORK_STATEMENT.md).
struct WorkStatementNames {
  std::string firm;
  std::string matter_id;
  std::string matter_name;
  // The period as the firm's bill names it ("November 2026").
  std::string period_name;
  // The AI colleagues by id: their names and the titles the firm gave them.
  struct Colleague {
    std::string name;
    std::string title;
  };
  std::map<std::string, Colleague> colleagues;
  // The Matter's threads by chat id: their titles.
  std::map<std::string, std::string> threads;
};

// The part of this computer's work record a statement carries (D-252): for
// each chain of its calls, the entries from its first call to the stamp that
// covers its last, and those stamps; empty when there is none to carry.
struct WorkRecordExcerpt {
  struct Entry {
    long long index = 0;
    std::string leaf;
    std::string prev;
    std::string hash;
  };
  struct Stamp {
    long long index = 0;
    std::string hash;
    std::string time;
    std::string key_id;
    std::string signature;
  };
  struct Chain {
    std::string id;
    // The device key code of the computer that keeps it.
    std::string key_code;
    std::vector<Entry> entries;
    std::vector<Stamp> stamps;
  };
  std::vector<Chain> chains;
};

[[nodiscard]] nlohmann::json build_work_statement(const UsageSummary& summary, const WorkStatementNames& names,
                                                  const std::string& issued_at,
                                                  const WorkRecordExcerpt& record = WorkRecordExcerpt());

// The bytes a statement's signature is over: its body as JSON, its keys in
// order, with nothing between them, in UTF-8 (docs/AI_WORK_STATEMENT.md).
[[nodiscard]] std::string work_statement_bytes(const nlohmann::json& statement);

// The signed file: the statement, the SHA-256 of its bytes, and the signature
// of those bytes by the key whose public half (base64) and code are given.
[[nodiscard]] nlohmann::json signed_work_statement(const nlohmann::json& statement, const std::string& public_key,
                                                   const std::string& key_code,
                                                   const std::function<std::string(const std::string&)>& sign);

// Whether a signed file's fingerprint is its statement's: the signature itself
// is checked with the key by its reader (Platform/DeviceKey, or the public
// check).
[[nodiscard]] bool work_statement_print_holds(const nlohmann::json& signed_file);

struct WorkStatementFiles {
  std::filesystem::path pdf_path;
  std::filesystem::path signed_path;
  std::string statement_sha256;
  long long request_count = 0;
};

// Writes the statement's PDF and its signed file into the folder, side by side
// under one name, numbered beside an earlier statement of the same name and
// never over it.
[[nodiscard]] WorkStatementFiles write_work_statement(const std::filesystem::path& folder,
                                                      const nlohmann::json& signed_file);

// ---- This computer's work record (D-252) ---------------------------------------------
//
// Every model call that leaves this computer is written into a chain as it is
// made: each entry holds the fingerprint of the call as a statement lists it
// (its leaf) and the fingerprint of the entry before it, so an entry taken out
// or changed breaks every one after it. Legal Machines timestamps the chain's
// head from that fingerprint alone (legalmachines.org's /singularity/api/
// timestamp), so no call can have been added after its stamp. A statement
// carries the part of the chain its calls are in, and legalmachines.org/check
// checks it in the client's browser.
namespace work_record {

// All zeros: what the first entry holds as the one before it.
inline const std::string kGenesis(64, '0');

// The fields of a call its leaf is the fingerprint of: exactly the ones a
// statement lists for it, with the values it lists, so its reader recomputes
// the leaf from the statement alone.
[[nodiscard]] nlohmann::json leaf_fields(const BilledCall& call);
// The same fields, from a call as a statement lists it.
[[nodiscard]] nlohmann::json leaf_fields_of_statement_call(const nlohmann::json& call);
[[nodiscard]] std::string leaf_of(const nlohmann::json& fields);
// An entry's fingerprint: SHA-256 of the one before it and its leaf, as their
// lower-case hex, one after the other.
[[nodiscard]] std::string link_of(const std::string& prev, const std::string& leaf);

struct Entry {
  long long index = 0;
  std::string time;
  std::string leaf;
  std::string prev;
  std::string hash;
};

using Stamp = WorkRecordExcerpt::Stamp;

// One computer's chain in `folder`: chain.json names it, entries.jsonl and
// stamps.jsonl each hold one JSON object a line, appended as they come. A
// last line cut off by a crash is passed over. Thread-safe.
class Chain {
public:
  explicit Chain(std::filesystem::path folder);
  std::string id();
  // Writes the call in and gives it its place (record_chain, record_index).
  // Never throws: a record that cannot be written leaves the call as it was.
  void append(protected_api::papinian::OutboundCall& call);
  std::optional<Entry> head();
  // The entries `from` to `to`, both included, that the chain holds.
  std::vector<Entry> entries(long long from, long long to);
  std::vector<Stamp> stamps();
  // Kept only when it stamps an entry of this chain, at that entry's hash.
  bool add_stamp(const Stamp& stamp);
  // Whether the head is newer than the last stamp.
  bool needs_stamp();
  // When the last entry was written; nothing before the first.
  std::optional<std::chrono::steady_clock::time_point> last_written();

private:
  void load();
  std::filesystem::path folder_;
  std::mutex mutex_;
  bool loaded_ = false;
  std::string id_;
  std::vector<Entry> entries_;
  std::vector<Stamp> stamps_;
  std::optional<std::chrono::steady_clock::time_point> written_;
};

// This computer's chain once the app has installed it (install), else none.
Chain* this_computer();
// Keeps this computer's record in `folder` from now on: every model call that
// leaves is written into it (Routing/RoutingLog's work_record_sink).
void install(const std::filesystem::path& folder);

// The part of the chain a statement's calls need: from the first of them to
// the first stamp at or after the last, with those stamps.
[[nodiscard]] WorkRecordExcerpt excerpt_for(Chain& chain, const std::string& key_code, const UsageSummary& summary);

} // namespace work_record

} // namespace singularity::usage
