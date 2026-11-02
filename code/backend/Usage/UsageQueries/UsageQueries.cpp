#include "Usage/UsageQueries/UsageQueries.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"

#include <algorithm>
#include <stdexcept>

namespace singularity::usage {

std::vector<routing::RoutingLogEntry> list_recorded_usage() {
  const auto workspace = node::configured_workspace_path();
  if (!workspace) return {};
  return routing::load_routing_log(*workspace);
}

UsageSummary get_usage_summary(
    const UsagePeriod& period, const std::optional<std::string>& project_id) {
  // A Node nobody has set up yet has recorded no usage: an empty summary, not
  // an error beside Home's first step.
  const auto configured = node::configured_workspace_path();
  if (!configured) {
    if (project_id.has_value()) throw std::runtime_error("This Project is no longer available.");
    return reconcile_usage({}, {}, period, std::nullopt);
  }
  const std::filesystem::path workspace_path = *configured;
  // One Project's usage reads that Project only (reconcile_usage() counts no
  // other when project_id is given); the workspace total reads them all.
  std::vector<matters::Matter> projects;
  if (project_id.has_value()) {
    auto one = matters::load_matter(workspace_path, *project_id);
    if (!one) throw std::runtime_error("This Project is no longer available.");
    projects.push_back(std::move(*one));
  } else {
    projects = matters::load_matters(workspace_path);
  }
  if (project_id.has_value()) {
    const auto found = projects.begin();
    // A Project whose data cannot be read has no usage to report: zero
    // operations here would be a false statement about real spend, and a
    // statement built from it would be signed as if it were complete.
    if (found->data_state == matters::ProjectDataState::Unavailable
        || found->data_state == matters::ProjectDataState::Damaged) {
      throw matters::ProjectDataError(found->data_state,
          found->data_state == matters::ProjectDataState::Damaged
            ? "This Project's Singularity data can't be read, so its usage can't be reported."
            : "This Project's folder or Singularity data isn't available, so its usage can't be reported.");
    }
  }
  return reconcile_usage(
      projects, routing::load_routing_log(workspace_path), period, project_id);
}

} // namespace singularity::usage
