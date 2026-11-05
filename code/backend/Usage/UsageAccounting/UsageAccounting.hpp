#pragma once

#include "Matter/Types/Matter.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::usage {

enum class CostClassification { Actual, Estimated, UsageOnly, Unavailable };

struct UsagePeriod {
  std::string start;
  std::string end;
};

// One model call as the bill of its work reads it (D-252): when it ran, which
// provider and model served it, the fingerprints of exactly what was sent and
// what came back, the provider's own id for the request, and the call's own
// tokens and cost as the provider reported them. It is the evidence that this
// request cost this much, and says nothing of what the request said: no
// marker, passage or value of the Matter is ever part of it.
struct BilledCall {
  std::string time;
  std::string provider_id;
  std::string model_id;
  // The provider that served it, where a gateway names it (D-116).
  std::string served_by;
  std::string provider_request_id;
  std::string payload_sha256;
  std::string response_sha256;
  std::optional<long long> input_tokens;
  std::optional<long long> cached_input_tokens;
  std::optional<long long> output_tokens;
  std::optional<long long> reasoning_tokens;
  std::optional<long long> cost_micros;
  std::string cost_currency;
  // "actual" or "estimated", and how it was known ("provider_reported").
  std::string cost_classification;
  std::string cost_methodology;
  // The protection the call left under: "off" when it left as written.
  std::string protection_mode;
  // Its place in its computer's work record (D-252), and the provider's own
  // record of its cost, once asked.
  std::string record_chain;
  std::optional<long long> record_index;
  std::optional<protected_api::papinian::CostConfirmation> confirmation;
};

// A model call as a bill lists it, from its outbound record.
[[nodiscard]] BilledCall billed_call_of(const protected_api::papinian::OutboundCall& call);

struct OperationUsageEvidence {
  std::string project_id;
  std::string operation_id;
  std::string task_id;
  int attempt = 1;
  std::string timestamp;
  std::optional<std::string> provider_id;
  std::optional<std::string> model_id;
  std::string execution_kind;
  std::string operation_type;
  std::string operation_role;
  std::string operation_status;
  std::optional<std::string> chat_id;
  std::optional<std::string> consilium_id;
  std::optional<std::string> workflow_run_id;
  std::vector<std::string> work_product_ids;
  // Whose work it was (D-252): the AI colleague whose turn or work the
  // operation is, empty for a person's own request; whether it began it
  // itself (its schedule, a change it noticed, a colleague's report); and the
  // person who asked for it, as Singularity knew them when it was written,
  // empty when no person did or it was not kept.
  std::string agent_id;
  bool agent_initiated = false;
  std::string asked_by_actor_id;
  std::string asked_by_name;
  // The AI colleague's name as its own work recorded it, where it did (its own
  // runs and the turns it began); empty otherwise.
  std::string agent_name;
  // Each model call it made, in the order made, with the evidence its bill
  // line rests on. `calls_recorded` is false when its calls are unknown: run
  // before Singularity kept them, or with no Routing Log entry here.
  std::vector<BilledCall> calls;
  bool calls_recorded = false;
  std::string privacy_mode;
  bool protected_external = false;
  // False for an online operation whose record never said how it was
  // protected: memory a provider's model kept before 24 Sep 2026 wrote no
  // protection mode, and reads "not recorded" rather than "protection off".
  bool protection_recorded = true;
  providers::UsageMetadata usage;
  CostClassification cost_classification = CostClassification::Unavailable;
  std::string evidence_status;
};

struct CurrencyTotal {
  std::string currency;
  std::optional<long long> actual_micros;
  std::optional<long long> estimated_micros;
};

struct UsageSummary {
  UsagePeriod period;
  std::vector<OperationUsageEvidence> operations;
  long long operation_count = 0;
  long long input_tokens = 0;
  long long output_tokens = 0;
  long long total_tokens = 0;
  long long actual_count = 0;
  long long estimated_count = 0;
  long long usage_only_count = 0;
  long long unavailable_count = 0;
  std::vector<CurrencyTotal> currency_totals;
};

[[nodiscard]] const char* to_string(CostClassification classification) noexcept;
[[nodiscard]] UsagePeriod validate_period(const std::string& start, const std::string& end);
[[nodiscard]] CostClassification classify_cost(const providers::UsageMetadata& usage) noexcept;
[[nodiscard]] std::string format_cost(long long amount_micros, const std::string& currency);

// Reconciles the canonical Project operation collection with its safe
// Routing Log evidence. No Usage record is persisted by this component.
[[nodiscard]] UsageSummary
reconcile_usage(const std::vector<matters::Matter>& projects,
                const std::vector<routing::RoutingLogEntry>& routing_entries,
                const UsagePeriod& period,
                const std::optional<std::string>& project_id = std::nullopt);

} // namespace singularity::usage
