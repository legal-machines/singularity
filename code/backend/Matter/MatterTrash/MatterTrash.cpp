#include "Matter/MatterTrash/MatterTrash.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace singularity::matter::trash {
namespace {

std::mutex &trash_mutex() {
	static auto *kept = new std::mutex();
	return *kept;
}

std::filesystem::path trash_file(const std::filesystem::path &workspace) {
	return workspace / ".singularity-workspace" / "trashed-matters.json";
}

// A Matter's id as the workspace keeps them: a plain name, never a path.
bool is_kept_id(const std::string &id) {
	if (id.empty() || id.size() > 128) return false;
	for (const char character : id) {
		const bool isPlain = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
			|| (character >= '0' && character <= '9') || character == '_' || character == '-';
		if (!isPlain) return false;
	}
	return true;
}

std::map<std::string, std::string> read_locked(const std::filesystem::path &workspace) {
	std::map<std::string, std::string> kept;
	std::ifstream input(trash_file(workspace), std::ios::binary);
	if (!input) return kept;
	try {
		const nlohmann::json record = nlohmann::json::parse(input);
		const auto matters = record.find("matters");
		if (matters == record.end() || !matters->is_object()) return kept;
		for (const auto &[id, at] : matters->items()) {
			if (is_kept_id(id) && at.is_string()) kept[id] = at.get<std::string>();
		}
	} catch (const std::exception &) {
		// Unreadable: nothing is in the Trash, and nothing is deleted from it.
	}
	return kept;
}

// Written whole beside the list and moved into place, as the workspace's
// other records are; a Trash that cannot be written keeps the Matter listed,
// never anything lost.
void write_locked(const std::filesystem::path &workspace, const std::map<std::string, std::string> &kept) {
	const std::filesystem::path target = trash_file(workspace);
	platform::ensure_record_directory(target.parent_path());
	nlohmann::json matters = nlohmann::json::object();
	for (const auto &[id, at] : kept) matters[id] = at;
	const std::filesystem::path staged = platform::staged_sibling_path(target);
	try {
		platform::write_file_durably(staged, nlohmann::json{{"formatVersion", 1}, {"matters", matters}}.dump());
		platform::commit_staged_file(staged, target, platform::StagedCommitMode::ReplaceExisting, false);
	} catch (...) {
		platform::discard_staged_file(staged);
		throw;
	}
}

std::optional<std::time_t> parsed(const std::string &moment) {
	std::tm parts{};
	std::istringstream text(moment);
	text >> std::get_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
	if (text.fail()) return std::nullopt;
#if defined(_WIN32)
	const std::time_t seconds = _mkgmtime(&parts);
#else
	const std::time_t seconds = timegm(&parts);
#endif
	if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
	return seconds;
}

std::string formatted(std::time_t seconds) {
	std::tm parts{};
#if defined(_WIN32)
	gmtime_s(&parts, &seconds);
#else
	gmtime_r(&seconds, &parts);
#endif
	std::ostringstream text;
	text << std::put_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
	return text.str();
}

} // namespace

std::string deletes_at(const std::string &trashed_at) {
	const auto seconds = parsed(trashed_at);
	if (!seconds) return {};
	return formatted(*seconds + static_cast<std::time_t>(kKeptDays) * 24 * 60 * 60);
}

std::vector<Trashed> list(const std::filesystem::path &workspace) {
	const std::lock_guard<std::mutex> lock(trash_mutex());
	std::vector<Trashed> found;
	for (const auto &[id, at] : read_locked(workspace)) found.push_back(Trashed{id, at, deletes_at(at)});
	return found;
}

Trashed put(const std::filesystem::path &workspace, const std::string &id, const std::string &now) {
	if (!is_kept_id(id)) throw std::invalid_argument("This Matter can't go to the Trash.");
	const std::lock_guard<std::mutex> lock(trash_mutex());
	auto kept = read_locked(workspace);
	const auto [entry, isNew] = kept.try_emplace(id, now);
	if (isNew) write_locked(workspace, kept);
	return Trashed{id, entry->second, deletes_at(entry->second)};
}

bool take_back(const std::filesystem::path &workspace, const std::string &id) {
	const std::lock_guard<std::mutex> lock(trash_mutex());
	auto kept = read_locked(workspace);
	if (kept.erase(id) == 0) return false;
	write_locked(workspace, kept);
	return true;
}

std::vector<std::string> expired(const std::filesystem::path &workspace, const std::string &now) {
	const std::lock_guard<std::mutex> lock(trash_mutex());
	const auto current = parsed(now);
	std::vector<std::string> due;
	if (!current) return due;
	for (const auto &[id, at] : read_locked(workspace)) {
		const auto trashed = parsed(at);
		// A date it cannot read is never taken as passed.
		if (trashed && *current - *trashed >= static_cast<std::time_t>(kKeptDays) * 24 * 60 * 60) due.push_back(id);
	}
	return due;
}

} // namespace singularity::matter::trash
