#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

// The Native API of agents (docs/contracts/native/Agent.json): JSON in, the
// agent state machine (Agent/AgentActions), JSON out. No rule of what an agent
// may do lives here.
namespace singularity::bridge::agent_bridge {

nlohmann::json list(const nlohmann::json &params);
nlohmann::json get(const nlohmann::json &params);
// agent.roles -> {roles}: the one catalogue of the roles a colleague can be
// begun in (Agent/AgentRoles), so the window keeps no copy of it.
nlohmann::json roles(const nlohmann::json &params);
nlohmann::json get_run(const nlohmann::json &params);
nlohmann::json create(const nlohmann::json &params);
// A colleague of the firm, in no Matter.
nlohmann::json hire(const nlohmann::json &params);
// agent.edit {agentId, name, character?, manner?, goal?, title?, ranksWith?,
// practice?, area?, answersTo?} -> AgentSummary; agent.archive {agentId,
// archived} -> AgentSummary; agent.delete {agentId} -> {deleted}.
nlohmann::json edit(const nlohmann::json &params);
// agent.requestEdit {the same as agent.edit} -> AgentSummary: from any of the
// administrator's computers; one another computer runs changes once that
// computer takes it.
nlohmann::json request_edit(const nlohmann::json &params);
nlohmann::json archive(const nlohmann::json &params);
nlohmann::json remove(const nlohmann::json &params);
// A colleague's memory, as its page shows it, and a note a member lets go
// of.
nlohmann::json memory(const nlohmann::json &params);
// The firm's colleagues that other computers of the firm run, as the firm's
// record holds them: every computer lists the whole firm's.
nlohmann::json firm_colleagues(const nlohmann::json &params);
nlohmann::json forget_memory(const nlohmann::json &params);
nlohmann::json approve(const nlohmann::json &params);
nlohmann::json decline(const nlohmann::json &params);
nlohmann::json pause(const nlohmann::json &params);
nlohmann::json set_model(const nlohmann::json &params);
// agent.setMemoryModel {agentId, providerId?, modelId?, off?}: what gathers its
// memory.
nlohmann::json set_memory_model(const nlohmann::json &params);
nlohmann::json resume(const nlohmann::json &params);
nlohmann::json stop(const nlohmann::json &params);
nlohmann::json update(const nlohmann::json &params);
// agent.setOffice {agentId, title, ranksWith, practice, area, answersTo} ->
// the agent: the colleague's office in the firm (Agent/AgentActions
// set_agent_office).
nlohmann::json set_office(const nlohmann::json &params);
nlohmann::json run(const nlohmann::json &params);
nlohmann::json cancel_run(const nlohmann::json &params);
nlohmann::json mark_reviewed(const nlohmann::json &params);
// A person's words to their colleagues in a thread, naming whom they are for
// (@: people by actor id, agents by id): kept for the team with no model call
// (Chat/ChatActions post_member_message), and taken up as work by every agent
// of this computer it names, which answers in the same thread
// (Agent/AgentActions take_up_mentions).
nlohmann::json post_message(const nlohmann::json &params);

// Work a lawyer gives an agent in their own words, and takes back before it
// begins (Agent/AgentActions assign_work).
nlohmann::json assign(const nlohmann::json &params);
nlohmann::json withdraw_assignment(const nlohmann::json &params);
nlohmann::json preview_setup(const nlohmann::json &params);
// The folder the panel should open in for a Matter: its own folder.
std::optional<std::string> matter_folder_for(const nlohmann::json &params);
// The chosen folder as the page reads it, or null when the lawyer cancelled.
nlohmann::json chosen_folder(const nlohmann::json &params, const std::optional<std::string> &folder);
// A file an agent reported on, by its path in the watched folder: never a path
// outside it.
std::string run_file_path(const nlohmann::json &params);

// The AI apps the lawyer connected (Agent/AiAppConnections). A new connection
// or key is answered with the key, once, for the lawyer to copy into the app;
// it is kept nowhere.
nlohmann::json ai_apps(const nlohmann::json &params);
nlohmann::json connect_app(const nlohmann::json &params);
nlohmann::json renew_app_key(const nlohmann::json &params);
nlohmann::json disconnect_app(const nlohmann::json &params);

} // namespace singularity::bridge::agent_bridge
