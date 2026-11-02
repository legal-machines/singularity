#include "Agent/McpServer/McpCaller.hpp"

namespace singularity::agents {

// Where the system offers no verified code signatures, every caller is
// named unverified.
AiAppCaller identify_caller() {
	return AiAppCaller{};
}

} // namespace singularity::agents
