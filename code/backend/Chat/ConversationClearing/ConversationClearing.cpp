#include "Chat/ConversationClearing/ConversationClearing.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <mutex>
#include <sstream>

namespace singularity::chat::conversation_clearing {
namespace {

std::mutex &records_mutex() {
	static auto *kept = new std::mutex();
	return *kept;
}

std::optional<std::filesystem::path> &testing_file() {
	static auto *kept = new std::optional<std::filesystem::path>();
	return *kept;
}

std::filesystem::path records_file() {
	if (testing_file()) return *testing_file();
	return default_records_file();
}

bool is_kept_id(const std::string &chat_id) {
	return !chat_id.empty() && chat_id.size() <= 200;
}

std::map<std::string, std::string> read_locked() {
	std::map<std::string, std::string> cleared;
	std::ifstream input(records_file(), std::ios::binary);
	if (!input) return cleared;
	try {
		const nlohmann::json kept = nlohmann::json::parse(input);
		const auto chats = kept.find("chats");
		if (chats == kept.end() || !chats->is_object()) return cleared;
		for (const auto &[chat_id, at] : chats->items()) {
			if (is_kept_id(chat_id) && at.is_string()) cleared[chat_id] = at.get<std::string>();
		}
	} catch (const std::exception &) {
		// Unreadable: nothing cleared, never a conversation lost.
	}
	return cleared;
}

void write_locked(const std::map<std::string, std::string> &cleared) {
	nlohmann::json chats = nlohmann::json::object();
	for (const auto &[chat_id, at] : cleared) chats[chat_id] = at;
	const std::filesystem::path file = records_file();
	platform::ensure_record_directory(file.parent_path());
	platform::write_file_durably(file, nlohmann::json{{"formatVersion", 1}, {"chats", chats}}.dump());
}

} // namespace

std::map<std::string, std::string> all() {
	const std::lock_guard<std::mutex> lock(records_mutex());
	return read_locked();
}

std::optional<std::string> cleared_before(const std::string &chat_id) {
	const std::lock_guard<std::mutex> lock(records_mutex());
	const auto cleared = read_locked();
	const auto found = cleared.find(chat_id);
	if (found == cleared.end()) return std::nullopt;
	return found->second;
}

std::optional<std::string> clear(const std::string &chat_id, const std::string &now) {
	if (!is_kept_id(chat_id)) throw std::invalid_argument("Name the conversation.");
	const std::lock_guard<std::mutex> lock(records_mutex());
	auto cleared = read_locked();
	std::optional<std::string> before;
	if (const auto found = cleared.find(chat_id); found != cleared.end()) before = found->second;
	cleared[chat_id] = now;
	write_locked(cleared);
	return before;
}

void restore(const std::string &chat_id, const std::optional<std::string> &cleared_before) {
	if (!is_kept_id(chat_id)) throw std::invalid_argument("Name the conversation.");
	const std::lock_guard<std::mutex> lock(records_mutex());
	auto cleared = read_locked();
	if (cleared_before) cleared[chat_id] = *cleared_before;
	else cleared.erase(chat_id);
	write_locked(cleared);
}

void use_records_file_for_testing(const std::filesystem::path &file) {
	const std::lock_guard<std::mutex> lock(records_mutex());
	testing_file() = file;
}

} // namespace singularity::chat::conversation_clearing
