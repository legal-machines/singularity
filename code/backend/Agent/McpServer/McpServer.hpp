#pragma once

#include "Agent/AiAppChannel/AiAppChannel.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>

namespace singularity::agents {

// Singularity's Model Context Protocol server, as the AI app sees it: the
// helper the app starts (singularity-mcp) speaks MCP over its standard input
// and output - one JSON-RPC message per line - and passes each tool call to
// the open Singularity (Agent/AiAppChannel), which decides it
// (Agent/AiAppConnections). The helper itself decides nothing and holds no
// state but the app's own description of itself.
//
// Its tools are few and narrow: list_matters, list_agents, get_agent,
// get_agent_run, draft_agent and pause_agent. None reads a document, takes a
// path or a permission, or runs anything; a draft waits for the lawyer, and
// pausing is allowed only where the lawyer allowed it.

// The protocol versions this server speaks, newest first.
extern const char *const kMcpProtocolVersions[];

// What the AI app said of itself when it connected - its own words.
struct McpClient {
	std::string name;
	std::string version;
};

class McpSession {
public:
	// Passes one checked tool call on, and returns Singularity's answer.
	using Forward = std::function<AiAppAnswer(const std::string &tool, const nlohmann::json &arguments, const McpClient &client)>;

	explicit McpSession(Forward forward);

	// One line from the app; the line to write back, if any. A notification
	// is never answered.
	std::optional<std::string> handle_line(const std::string &line);
	std::optional<nlohmann::json> handle(const nlohmann::json &message);

	const McpClient &client() const;

private:
	nlohmann::json tool_result(const std::string &tool, const nlohmann::json &arguments);

	Forward forward_;
	McpClient client_;
};

// The tools as tools/list describes them.
nlohmann::json mcp_tools();

// The arguments of a call, checked against its tool's schema; the sentence
// that says what is wrong, or nothing.
std::optional<std::string> mcp_argument_problem(const std::string &tool, const nlohmann::json &arguments);

} // namespace singularity::agents
