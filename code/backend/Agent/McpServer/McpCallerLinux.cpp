#include "Agent/McpServer/McpCaller.hpp"

#include <climits>
#include <string>
#include <unistd.h>

namespace singularity::agents {

// Linux has no code signature the system verifies, so the AI app is named by
// its path alone and called unverified (McpCaller.hpp).
AiAppCaller identify_caller() {
	AiAppCaller caller;
	const std::string link = "/proc/" + std::to_string(::getppid()) + "/exe";
	char path[PATH_MAX] = {0};
	const ssize_t length = ::readlink(link.c_str(), path, sizeof(path) - 1);
	if (length > 0) caller.path.assign(path, static_cast<std::size_t>(length));
	return caller;
}

} // namespace singularity::agents
