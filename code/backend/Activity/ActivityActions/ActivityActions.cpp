#include "Activity/ActivityActions/ActivityActions.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "WorkProduct/WorkProductActions/WorkProductActions.hpp"

#include <algorithm>
#include <stdexcept>

namespace singularity::activity {
namespace {

auto find(std::vector<matters::Matter> &all, const std::string &id) {
  return std::find_if(all.begin(), all.end(), [&](const matters::Matter &entry) { return entry.id == id; });
}

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

} // namespace

matters::Matter add_note(const std::string &matter_id, std::string content) {
  content = trim(content);
  if (content.empty()) throw std::runtime_error("Enter note content.");
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace, matter_id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
	matters::MatterNote note;
	note.id = matters::generate_note_id();
	note.matter_id = matter_id;
	note.content = std::move(content);
	note.created_at = workspaces::current_timestamp();
	note.updated_at = note.created_at;
  found->notes.push_back(std::move(note));
  found->updated_at = workspaces::current_timestamp();
  matters::save_matter(workspace, *found);
  return *found;
}

matters::Matter remove_note(const std::string &matter_id, const std::string &note_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace, matter_id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
  const auto note = std::find_if(found->notes.begin(), found->notes.end(),
      [&](const matters::MatterNote &entry) { return entry.id == note_id; });
  if (note != found->notes.end()) {
    found->notes.erase(note);
    found->updated_at = workspaces::current_timestamp();
    matters::save_matter(workspace, *found);
  }
  return *found;
}

matters::Matter promote_finding(const std::string &matter_id, const std::string &finding_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace, matter_id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
  const matters::Finding *finding = nullptr;
  for (const auto &task : found->tasks) {
    const auto candidate = std::find_if(task.findings.begin(), task.findings.end(),
        [&](const matters::Finding &entry) { return entry.id == finding_id; });
    if (candidate != task.findings.end()) { finding = &*candidate; break; }
  }
  if (!finding) throw std::runtime_error("This finding no longer exists.");
  const auto timestamp = workspaces::current_timestamp();
  found->issues.push_back(matters::promote_finding_to_issue(
      *finding, matter_id, matters::generate_issue_id(), timestamp));
  found->updated_at = timestamp;
  matters::save_matter(workspace, *found);
  return *found;
}

matters::Matter update_issue_status(const std::string &matter_id, const std::string &issue_id,
                                    const std::string &status_value) {
	const matters::WorkProductDetail detail = work_product::get_detail(matter_id, issue_id);
	(void)work_product::update_issue_status(matter_id, issue_id, status_value, detail.object.revision);
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::vector<matters::Matter> all = matters::load_matters(node::require_active_workspace_path());
	const auto found = std::find_if(all.begin(), all.end(),
			[&](const matters::Matter &entry) { return entry.id == matter_id; });
	if (found == all.end()) throw std::runtime_error("This matter no longer exists.");
	return *found;
}

} // namespace singularity::activity
