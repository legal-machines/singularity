#pragma once

#include "Matter/SharedMatter/SharedMatterService.hpp"
#include "Matter/Types/Matter.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace singularity::bridge::matter_bridge {

nlohmann::json serialize(const matters::Matter &matter);
nlohmann::json serialize(const std::vector<matters::Matter> &matters);

nlohmann::json list(const nlohmann::json &params);
// matter.listSummaries: every Matter as a rail, Home or a header shows it -
// its chats with their last request and their count, no task, document or
// note (Beta 1 #265).
nlohmann::json list_summaries(const nlohmann::json &params);
nlohmann::json create(const nlohmann::json &params);
nlohmann::json update(const nlohmann::json &params);
nlohmann::json remove(const nlohmann::json &params);
nlohmann::json open_folder(const nlohmann::json &params);
nlohmann::json assign_folder(const nlohmann::json &params,
                             const std::optional<std::string> &selected_path);
// One dialog, one real action: creates a brand-new Project named after the
// chosen folder and assigns that exact folder to it in the same call - the
// direct "New Project" flow (Beta UX correction: as immediate as `git
// init`, never a separate create-then-assign ceremony). Returns null when
// the user cancels the folder picker; creates nothing in that case.
nlohmann::json create_from_folder(const std::optional<std::string> &selected_path);
// project.createSample: the sample matter this build carries, copied once into
// the default Projects folder and opened (matter::create_sample).
nlohmann::json create_sample(const nlohmann::json &params);
// A new Project begun by its name: its folder is made in the default Projects
// folder and opened (matter::create_named).
nlohmann::json create_named(const nlohmann::json &params);
// project.create with the folder the lawyer chose for it (matter.chooseNewFolder):
// its `.lm` is made there, beside the documents the folder holds, and the
// Project bears the name the lawyer gave it; refused for a folder that holds a
// Matter already. With none chosen, as create_named.
nlohmann::json create_named_in(const nlohmann::json &params, const std::optional<std::filesystem::path> &chosen);
nlohmann::json add_note(const nlohmann::json &params);
nlohmann::json remove_note(const nlohmann::json &params);
nlohmann::json add_issue(const nlohmann::json &params);
nlohmann::json update_issue_status(const nlohmann::json &params);

nlohmann::json save_ai_setup(const nlohmann::json &params);

nlohmann::json get_project_data_status(const nlohmann::json &params);
nlohmann::json set_up_project_data(const nlohmann::json &params);
nlohmann::json reveal_project_data(const nlohmann::json &params);
nlohmann::json prepare_project_data_clean_copy(const nlohmann::json &params,
                                               const std::optional<std::string> &selected_path);
nlohmann::json remove_project_data(const nlohmann::json &params);

// matter.listConflicts / matter.resolveConflict - every unresolved merge
// conflict this Project has, in any collection, and the person's answer to one
// (COL-1). A shared Project must be able to say "your change did not happen",
// or it is not safe to share.
nlohmann::json list_project_conflicts(const nlohmann::json &params);
nlohmann::json resolve_project_conflict(const nlohmann::json &params);
// A Matter shared between computers (Matter/SharedMatter): who works on it,
// its recent document changes named by who made them, and the changes a later
// save overwrote; and a lawyer's decision on one of those.
nlohmann::json shared_activity(const nlohmann::json &params);
nlohmann::json confirm_computer(const nlohmann::json &params);
nlohmann::json revoke_computer(const nlohmann::json &params);
nlohmann::json resolve_overwritten_change(const nlohmann::json &params);
// The firm as this computer knows it: people and agents across its Matters.
nlohmann::json firm_team(const nlohmann::json &params);
// The firm's team as project.firmTeam says it, for a team the caller gathered
// (organisation::firm_team_with_profiles, D-209).
nlohmann::json firm_team_json(const matter::FirmTeam &team);

nlohmann::json discover_projects(const nlohmann::json &params);
// Opens on this computer a Matter that discovery found in the folders the
// lawyer gave Singularity - how a second lawyer joins the firm's shared
// Matter. A folder discovery did not find there is refused.
nlohmann::json open_discovered(const nlohmann::json &params);
// The firm's coordination, directly between its computers and sealed end to
// end (D-123, D-124; Matter/MatterCoordination): the firm's folder this
// computer exchanges through, where it stands, a Matter shared, the invitation
// a lawyer gives a colleague, a Matter asked for by that invitation, and a key
// accepted once its giver's code is compared. No key ever reaches the window.
nlohmann::json coordination_use_firm_folder(const nlohmann::json &params);
nlohmann::json coordination_status(const nlohmann::json &params);
nlohmann::json coordination_share(const nlohmann::json &params);
nlohmann::json coordination_invitation(const nlohmann::json &params);
nlohmann::json coordination_join(const nlohmann::json &params);
nlohmann::json coordination_accept_key(const nlohmann::json &params);

// The firm's computers and the check two of them make of each other (D-205):
// coordination.firmComputers {} -> {computers, asking}, as last read (never
// asking Legal Machines); coordination.checkComputer {deviceId} and
// coordination.computerCheck {deviceId} -> {deviceId, state, number,
// problem}; coordination.keepComputer, .refuseComputer and
// .endComputerCheck {deviceId} -> the computers again.
nlohmann::json coordination_firm_computers(const nlohmann::json &params);
nlohmann::json coordination_check_computer(const nlohmann::json &params);
nlohmann::json coordination_computer_check(const nlohmann::json &params);
nlohmann::json coordination_keep_computer(const nlohmann::json &params);
nlohmann::json coordination_refuse_computer(const nlohmann::json &params);
nlohmann::json coordination_end_computer_check(const nlohmann::json &params);

// A Matter given to a lawyer of the firm by choosing them (D-205):
// coordination.giveMatter {projectId, actorId} -> {given, reaches};
// coordination.offers {} -> {offers}; coordination.acceptOffer {matterId,
// fromDeviceId} -> CoordinationStatus; coordination.declineOffer {matterId,
// fromDeviceId} -> {offers}.
nlohmann::json coordination_give_matter(const nlohmann::json &params);
nlohmann::json coordination_offers(const nlohmann::json &params);
nlohmann::json coordination_accept_offer(const nlohmann::json &params);
nlohmann::json coordination_decline_offer(const nlohmann::json &params);
// A personal conversation of this computer's member and another member
// (D-042): opened - its channel, its project and its chat made when they are
// not yet - and every one this member holds, made here or arrived from the
// other member's computer. Its key goes only to the two members' computers a
// lawyer confirmed in a Matter they share (Matter/MatterCoordination).
nlohmann::json open_personal_chat(const nlohmann::json &params);
nlohmann::json list_personal_chats(const nlohmann::json &params);

} // namespace singularity::bridge::matter_bridge
