#include "Agent/McpServer/McpServer.hpp"
#include "Agent/AgentRoles/AgentRoles.hpp"

#include <set>
#include <utility>

namespace singularity::agents {
namespace {

using Json = nlohmann::json;

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

const char *const kServerVersion = "1.0.0";

Json error_reply(const Json &id, int code, const std::string &message) {
	return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", code}, {"message", message}}}};
}

Json result_reply(const Json &id, Json result) {
	return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json text_result(const std::string &text, bool is_error, const Json &data = nullptr) {
	Json result{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})}, {"isError", is_error}};
	if (data.is_object() == true) result["structuredContent"] = data;
	return result;
}

std::size_t characters_in(const std::string &text) {
	std::size_t count = 0;
	for (const char byte : text)
		if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) ++count;
	return count;
}

Json object_schema(Json properties, Json required) {
	return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
}

const char *const kInstructions =
	"Singularity is a legal workspace on this computer. Through this connection you may list the Matters the lawyer "
	"chose to share with you and their agents, and propose a new agent for one of those Matters. A proposal does "
	"nothing until the lawyer reviews and approves it in Singularity, choosing there which folder it may read, when "
	"it runs and which model it uses. You may read how an agent's runs went (counts and states, never the documents), "
	"and pause an agent only where the lawyer allowed it. You cannot read documents, choose files or folders, or change, run or approve "
	"agents through this connection.";

} // namespace

const char *const kMcpProtocolVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", nullptr};

Json mcp_tools() {
	const Json readOnly{{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}};
	// The roles an app may propose a colleague in (Agent/AgentRoles): every role
	// but one that needs a Playbook, which only the lawyer chooses.
	Json kinds = Json::array();
	for (const agents::AgentRole &role : agents::agent_roles())
		if (role.uses_playbook == false) kinds.push_back(role.id);
	return Json::array({
		Json{
			{"name", "list_matters"},
			{"title", "List shared Matters"},
			{"description", "Lists the Matters the lawyer allowed this app to see in Singularity: each Matter's id and name."},
			{"inputSchema", object_schema(Json::object(), Json::array())},
			{"annotations", readOnly},
		},
		Json{
			{"name", "list_agents"},
			{"title", "List agents"},
			{"description", "Lists the agents in the Matters this app may see, or in one of them: name, what it does and its state "
				"(waiting_for_approval, running, needs_review, access_needed, failed, paused, stopped or watching). "
				"Nothing of their documents or results."},
			{"inputSchema", object_schema(Json{{"matterId", Json{{"type", "string"}, {"description", "Only this Matter's agents."}}}}, Json::array())},
			{"annotations", readOnly},
		},
		Json{
			{"name", "get_agent"},
			{"title", "Get an agent's state"},
			{"description", "One agent's name, what it does, its state and when it last ran and runs next."},
			{"inputSchema", object_schema(Json{{"agentId", Json{{"type", "string"}}}}, Json::array({"agentId"}))},
			{"annotations", readOnly},
		},
		Json{
			{"name", "get_agent_run"},
			{"title", "How an agent's run went"},
			{"description", "The latest run of an agent, or the run named: its state, when it ran, how many documents were added, "
				"changed, removed or moved, and whether the lawyer has a result to review. Never the documents' names or words."},
			{"inputSchema", object_schema(Json{{"agentId", Json{{"type", "string"}}}, {"runId", Json{{"type", "string"}}}}, Json::array({"agentId"}))},
			{"annotations", readOnly},
		},
		Json{
			{"name", "pause_agent"},
			{"title", "Pause an agent"},
			{"description", "Pauses an agent in a Matter this app may see, when the lawyer allowed this app to pause agents. "
				"Only the lawyer resumes it, in Singularity."},
			{"inputSchema", object_schema(Json{{"agentId", Json{{"type", "string"}}}}, Json::array({"agentId"}))},
			{"annotations", Json{{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
		},
		Json{
			{"name", "draft_agent"},
			{"title", "Propose an agent"},
			{"description", "Proposes a standing agent for one Matter. The proposal is inert: it does nothing until the lawyer "
				"reviews it in Singularity and approves it, choosing there the folder it may read, when it runs and which "
				"model it uses - or declines it. You cannot choose its files, folder or permissions. kind: data_room_watch, "
				"deadlines and new_documents report what changed in the Matter's documents; matter_summary, status_update "
				"and custom carry out the instruction as a request in the Matter."},
			{"inputSchema", object_schema(Json{
				{"matterId", Json{{"type", "string"}, {"description", "A Matter id from list_matters."}}},
				{"name", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 80}, {"description", "A short name the lawyer will recognise."}}},
				{"instruction", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 2000},
					{"description", "What the agent should look for or do, in plain words, as the lawyer would write it."}}},
				{"kind", Json{{"type", "string"}, {"enum", kinds}}},
				{"schedule", Json{{"type", "string"}, {"enum", Json::array({"manual", "on_change", "daily", "weekly"})},
					{"description", "When it would run. The lawyer may change it before approving."}}},
				{"time", Json{{"type", "string"}, {"pattern", "^[0-2][0-9]:[0-5][0-9]$"}, {"description", "Local time for daily or weekly runs, HH:MM."}}},
				{"weekday", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 6}, {"description", "0 is Sunday; for weekly runs."}}},
			}, Json::array({"matterId", "name", "instruction"}))},
			{"annotations", Json{{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}}},
		},
	});
}

std::optional<std::string> mcp_argument_problem(const std::string &tool, const Json &arguments) {
	if (arguments.is_object() == false) return "The arguments must be an object.";
	const auto isText = [&](const char *key) {
		const auto found = arguments.find(key);
		return found != arguments.end() && found->is_string() == true;
	};
	const auto isAbsentOrText = [&](const char *key) {
		const auto found = arguments.find(key);
		return found == arguments.end() || found->is_null() == true || found->is_string() == true;
	};
	std::set<std::string> allowed;
	if (tool == "list_matters") {
	} else if (tool == "list_agents") {
		allowed = {"matterId"};
		if (isAbsentOrText("matterId") == false) return "\"matterId\" must be text.";
	} else if (tool == "get_agent") {
		allowed = {"agentId"};
		if (isText("agentId") == false) return "Give the agent's id as \"agentId\".";
	} else if (tool == "get_agent_run") {
		allowed = {"agentId", "runId"};
		if (isText("agentId") == false) return "Give the agent's id as \"agentId\".";
		if (isAbsentOrText("runId") == false) return "\"runId\" must be text.";
	} else if (tool == "pause_agent") {
		allowed = {"agentId"};
		if (isText("agentId") == false) return "Give the agent's id as \"agentId\".";
	} else if (tool == "draft_agent") {
		allowed = {"matterId", "name", "instruction", "kind", "schedule", "time", "weekday"};
		if (isText("matterId") == false) return "Give the Matter's id as \"matterId\" (see list_matters).";
		if (isText("name") == false || arguments.at("name").get<std::string>().empty() == true) return "Give the agent a \"name\".";
		if (characters_in(arguments.at("name").get<std::string>()) > 80) return "\"name\" is longer than 80 characters.";
		if (isText("instruction") == false || arguments.at("instruction").get<std::string>().empty() == true)
			return "Give the agent its \"instruction\", in plain words.";
		if (characters_in(arguments.at("instruction").get<std::string>()) > 2000) return "\"instruction\" is longer than 2,000 characters.";
		if (isAbsentOrText("kind") == false || isAbsentOrText("schedule") == false || isAbsentOrText("time") == false)
			return "\"kind\", \"schedule\" and \"time\" must be text.";
		const auto weekday = arguments.find("weekday");
		if (weekday != arguments.end() && weekday->is_null() == false && weekday->is_number_integer() == false)
			return "\"weekday\" must be a whole number from 0 to 6.";
	} else {
		return std::nullopt;
	}
	for (const auto &[key, value] : arguments.items()) {
		(void)value;
		if (allowed.count(key) == 0) return "\"" + key + "\" is not an argument of " + tool + ".";
	}
	return std::nullopt;
}

McpSession::McpSession(Forward forward) : forward_(std::move(forward)) {}

const McpClient &McpSession::client() const {
	return client_;
}

std::optional<std::string> McpSession::handle_line(const std::string &line) {
	if (line.find_first_not_of(" \t\r\n") == std::string::npos) return std::nullopt;
	const Json message = Json::parse(line, nullptr, false);
	if (message.is_discarded() == true) return error_reply(nullptr, kParseError, "Parse error").dump();
	const std::optional<Json> reply = handle(message);
	if (reply.has_value() == false) return std::nullopt;
	return reply->dump();
}

std::optional<Json> McpSession::handle(const Json &message) {
	if (message.is_object() == false) return error_reply(nullptr, kInvalidRequest, "A message must be one JSON-RPC object.");
	const auto method = message.find("method");
	const auto id = message.find("id");
	const bool isNotification = id == message.end();
	// A reply the app sent us, or anything without a method, asks nothing.
	if (method == message.end() || method->is_string() == false) {
		if (isNotification == true) return std::nullopt;
		return error_reply(*id, kInvalidRequest, "A request needs a method.");
	}
	if (isNotification == true) return std::nullopt;
	if (id->is_string() == false && id->is_number_integer() == false)
		return error_reply(nullptr, kInvalidRequest, "A request id must be a string or an integer.");
	const std::string name = method->get<std::string>();
	const Json params = message.contains("params") == true && message.at("params").is_object() == true ? message.at("params") : Json::object();

	if (name == "initialize") {
		std::string version = kMcpProtocolVersions[0];
		const auto asked = params.find("protocolVersion");
		if (asked != params.end() && asked->is_string() == true)
			for (const char *const *known = kMcpProtocolVersions; *known != nullptr; ++known)
				if (asked->get<std::string>() == *known) version = *known;
		const auto info = params.find("clientInfo");
		if (info != params.end() && info->is_object() == true) {
			if (info->contains("name") == true && info->at("name").is_string() == true) client_.name = info->at("name").get<std::string>().substr(0, 200);
			if (info->contains("version") == true && info->at("version").is_string() == true) client_.version = info->at("version").get<std::string>().substr(0, 60);
		}
		return result_reply(*id, Json{
			{"protocolVersion", version},
			{"capabilities", Json{{"tools", Json{{"listChanged", false}}}}},
			{"serverInfo", Json{{"name", "singularity"}, {"title", "Singularity"}, {"version", kServerVersion}}},
			{"instructions", kInstructions},
		});
	}
	if (name == "ping") return result_reply(*id, Json::object());
	if (name == "tools/list") return result_reply(*id, Json{{"tools", mcp_tools()}});
	if (name == "tools/call") {
		const auto tool = params.find("name");
		if (tool == params.end() || tool->is_string() == false) return error_reply(*id, kInvalidParams, "A tool call needs the tool's name.");
		bool isKnown = false;
		for (const Json &known : mcp_tools())
			if (known.at("name") == *tool) isKnown = true;
		if (isKnown == false) return error_reply(*id, kInvalidParams, "Unknown tool: " + tool->get<std::string>().substr(0, 60));
		const Json arguments = params.contains("arguments") == true ? params.at("arguments") : Json::object();
		return result_reply(*id, tool_result(tool->get<std::string>(), arguments.is_null() == true ? Json::object() : arguments));
	}
	return error_reply(*id, kMethodNotFound, "Method not found: " + name.substr(0, 60));
}

Json McpSession::tool_result(const std::string &tool, const Json &arguments) {
	const std::optional<std::string> problem = mcp_argument_problem(tool, arguments);
	if (problem.has_value() == true) return text_result(*problem, true);
	AiAppAnswer answer;
	try {
		answer = forward_(tool, arguments, client_);
	} catch (const std::exception &) {
		return text_result("Singularity could not be reached. Nothing was changed; ask again.", true);
	}
	return text_result(answer.text, answer.is_refused, answer.data);
}

} // namespace singularity::agents
