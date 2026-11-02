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
