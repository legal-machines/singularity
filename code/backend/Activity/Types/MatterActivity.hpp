#pragma once

#include "Context/SourceMarkers/SourceMarkers.hpp"
#include "WorkProduct/Types/WorkProducts.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::matters {

struct MatterNote {
  std::string id;
  std::string matter_id;
  std::string content;
  std::string created_at;
	std::string updated_at;
	std::optional<std::string> title;
	std::vector<context::ResolvedSourceReference> source_refs;
	WorkProductOrigin origin;
	long long revision = 1;
};

enum class IssueStatus { Open, Reviewed, Resolved };

struct Issue {
  std::string id;
  std::string matter_id;
  std::string title;
  std::string description;
  IssueStatus status = IssueStatus::Open;
  std::vector<context::ResolvedSourceReference> source_refs;
  std::string created_at;
  std::string updated_at;
	std::optional<std::string> origin_finding_id;
	long long revision = 1;
};

[[nodiscard]] const char *to_string(IssueStatus status) noexcept;
[[nodiscard]] std::optional<IssueStatus> issue_status_from_string(const std::string &value) noexcept;

} // namespace singularity::matters
