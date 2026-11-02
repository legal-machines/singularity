#pragma once

// Where what an operation's protected requests sent is kept (SentPassages in
// RoutingLog.hpp): "<workspace>/.singularity-workspace/outbound/<project>/<operation>.json",
// one folder per Project, so the Project's data takes it along when it goes.
// Header-only so a Project's removal (Matter/MatterActions) lets go of it
// without linking the log.

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace singularity::routing::sent_passages_folder {

// A name for an id on disk (a Project's folder, an operation's file): the id
// itself when it is only letters, digits, '-' and '_', otherwise the id's
// bytes in hexadecimal - so no id can name a path outside the folder.
inline std::string safe_name(const std::string &id) {
	bool isPlain = id.empty() == false && id.size() <= 128;
	for (const char letter : id) {
		const bool isAllowed = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')
			|| (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
		if (isAllowed == false) isPlain = false;
	}
	if (isPlain == true) return id;
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string name = "x";
	for (std::size_t index = 0; index < id.size() && index < 96; ++index) {
		const unsigned char byte = static_cast<unsigned char>(id[index]);
		name += kDigits[byte >> 4];
		name += kDigits[byte & 0x0f];
	}
	return name;
}

inline std::filesystem::path folder(const std::filesystem::path &workspace_path) {
	return workspace_path / ".singularity-workspace" / "outbound";
}

inline std::filesystem::path path(const std::filesystem::path &workspace_path, const std::string &matter_id,
		const std::string &operation_id) {
	return folder(workspace_path) / safe_name(matter_id) / (safe_name(operation_id) + ".json");
}

// Where an operation's passages were kept before they were kept by Project.
inline std::filesystem::path unscoped_path(const std::filesystem::path &workspace_path, const std::string &operation_id) {
	return folder(workspace_path) / (safe_name(operation_id) + ".json");
}

// The Project a kept file was written for; empty when it cannot be read.
inline std::string matter_of(const std::filesystem::path &file) {
	try {
		std::ifstream stream(file, std::ios::binary);
		const nlohmann::json value = nlohmann::json::parse(stream, nullptr, false);
		if (value.is_object() == false) return {};
		return value.value("matterId", std::string());
	} catch (...) {
		return {};
	}
}

// Lets go of everything kept of what the Project's requests sent - with its
// data, or with the Project. Never throws.
inline void forget_project(const std::filesystem::path &workspace_path, const std::string &matter_id) {
	if (matter_id.empty() == true) return;
	try {
		const std::filesystem::path root = folder(workspace_path);
		std::error_code error;
		std::filesystem::remove_all(root / safe_name(matter_id), error);
		std::vector<std::filesystem::path> theirs;
		error.clear();
		for (std::filesystem::directory_iterator entry(root, error), end; error.value() == 0 && entry != end; entry.increment(error)) {
			std::error_code fileError;
			if (entry->is_regular_file(fileError) == false || entry->path().extension() != ".json") continue;
			if (matter_of(entry->path()) == matter_id) theirs.push_back(entry->path());
		}
		for (const std::filesystem::path &file : theirs) {
			std::error_code removeError;
			std::filesystem::remove(file, removeError);
		}
	} catch (...) {
	}
}

} // namespace singularity::routing::sent_passages_folder
