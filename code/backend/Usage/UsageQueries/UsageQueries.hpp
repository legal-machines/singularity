#pragma once

#include "Usage/UsageAccounting/UsageAccounting.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::usage {

std::vector<routing::RoutingLogEntry> list_recorded_usage();
UsageSummary get_usage_summary(
    const UsagePeriod& period, const std::optional<std::string>& project_id = std::nullopt);

} // namespace singularity::usage
