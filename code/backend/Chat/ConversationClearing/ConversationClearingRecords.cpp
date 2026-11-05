#include "Chat/ConversationClearing/ConversationClearing.hpp"

#include "Platform/AppSettings/AppSettings.hpp"

namespace singularity::chat::conversation_clearing {

std::filesystem::path default_records_file() {
	return platform::computer_records_directory() / "Cleared conversations.json";
}

} // namespace singularity::chat::conversation_clearing
