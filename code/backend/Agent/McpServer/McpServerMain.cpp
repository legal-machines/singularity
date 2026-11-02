// singularity-mcp: the helper an AI app starts to reach Singularity on
// this computer (Agent/McpServer/McpServer.hpp). It speaks the Model Context
// Protocol on its standard input and output and nothing else: its standard
// output carries only protocol messages, and it opens no network port.
//
// The AI app gives it the key the lawyer copied from Singularity (the firm's
// name, AI apps) in the environment variable SINGULARITY_KEY.

#include "Agent/AiAppChannel/AiAppChannel.hpp"
#include "Agent/McpServer/McpCaller.hpp"
#include "Agent/McpServer/McpServer.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>

using namespace singularity::agents;

namespace {

constexpr auto kLongestWait = std::chrono::seconds(20);
constexpr auto kLookEvery = std::chrono::milliseconds(100);

std::string timestamp_now() {
	const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buffer;
}

AiAppAnswer refusal(const std::string &text) {
	AiAppAnswer answer;
	answer.is_refused = true;
	answer.text = text;
	return answer;
}

} // namespace

int main() {
	std::ios::sync_with_stdio(false);
	const char *keyValue = std::getenv("SINGULARITY_KEY");
	const std::string key = keyValue != nullptr ? keyValue : "";
	const AiAppCaller caller = identify_caller();

	McpSession session([&](const std::string &tool, const nlohmann::json &arguments, const McpClient &client) {
		if (key.empty() == true)
			return refusal("This Singularity connection has no key. In Singularity, choose the firm's name, then AI apps, "
						   "and copy this app's setup.");
		if (is_workspace_open() == false) return refusal("Singularity is not open on this computer. Open it, then ask again.");
		AiAppRequest request;
		request.id = new_request_id();
		request.key = key;
		request.tool = tool;
		request.arguments = arguments;
		request.client_name = client.name;
		request.client_version = client.version;
		request.caller = caller;
		request.sent_at = timestamp_now();
		leave_request(request);
		const auto started = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - started < kLongestWait) {
			std::optional<AiAppAnswer> answer = take_answer(request.id);
			if (answer.has_value() == true) return *answer;
			std::this_thread::sleep_for(kLookEvery);
		}
		// Not taken in time: it is withdrawn, so it is never decided later
		// with no one waiting for the answer.
		if (withdraw_request(request.id) == true)
			return refusal("Singularity did not answer in time, so nothing was asked of it. Ask again in a moment.");
		std::optional<AiAppAnswer> late = take_answer(request.id);
		if (late.has_value() == true) return *late;
		return refusal("Singularity took this request but did not answer in time. Use list_agents to see whether it was done.");
	});

	std::string line;
	while (std::getline(std::cin, line)) {
		const std::optional<std::string> reply = session.handle_line(line);
		if (reply.has_value() == false) continue;
		std::cout << *reply << '\n';
		std::cout.flush();
	}
	return 0;
}
