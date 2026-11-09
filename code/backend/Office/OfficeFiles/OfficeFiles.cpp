#include "Office/OfficeFiles/OfficeFiles.hpp"

#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Office/OfficeApps/OfficeApps.hpp"
#include "Office/OfficePaths/OfficePaths.hpp"
#include "Office/OfficeService/OfficeService.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/OpenExternal/OpenExternal.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace singularity::office {
namespace {

// What a pane may attach to a message: Outlook's own limit is near it.
constexpr std::uintmax_t kMaxAttachmentBytes = 30ULL * 1024 * 1024;

std::vector<matters::Matter> matters_here() {
	try {
		// The member's desk is no Matter a pane works in.
		std::vector<matters::Matter> all = matters::load_matters(node::require_active_workspace_path());
		all.erase(std::remove_if(all.begin(), all.end(), [](const matters::Matter &entry) { return matter::is_desk(entry); }),
			all.end());
		return all;
	} catch (const std::exception &) {
		return {};
	}
}

std::optional<matters::Matter> matter_here(const std::string &matter_id) {
	try {
		return matters::load_matter(node::require_active_workspace_path(), matter_id);
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

} // namespace

DescribedFile describe_file(const std::string &location) {
	DescribedFile described;
	if (location.empty()) {
		described.kind = "unsaved";
		return described;
	}
	if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
		described.kind = "online";
		const auto slash = location.find_last_of('/');
		described.name = slash == std::string::npos ? location : location.substr(slash + 1);
		return described;
	}
	const std::filesystem::path file = std::filesystem::u8path(path_of_location(location));
	described.name = file.filename().u8string();
	// The file Singularity opened the application with, to show its pane.
	if (is_starter_file(file, office_folder())) {
		described.kind = "starter";
		return described;
	}
	described.kind = "outside";
	for (const matters::Matter &matter : matters_here()) {
		if (!matter.directory_path || matter.directory_path->empty()) continue;
		const auto relative = relative_inside(std::filesystem::u8path(*matter.directory_path), file);
		if (!relative) continue;
		FileInMatter place;
		place.matter_id = matter.id;
		place.matter_name = matter.name;
		place.relative_path = *relative;
		// The record of the file. When two computers sharing the Matter's folder
		// both recorded the same new file, the one still available is the file;
		// the other is a record of it the folder no longer answers to.
		const matters::MatterDocument *chosen = nullptr;
		for (const auto &document : matter.documents) {
			if (document.relative_path.empty() || !same_place(document.relative_path, *relative)) continue;
			const bool isAvailable = document.available && document.lifecycle_state != matters::DocumentLifecycleState::Unavailable;
			if (chosen == nullptr || (isAvailable && !(chosen->available
					&& chosen->lifecycle_state != matters::DocumentLifecycleState::Unavailable))) {
				chosen = &document;
			}
		}
		if (chosen != nullptr) place.document_id = chosen->id;
		described.kind = "in_matter";
		described.place = place;
		break;
	}
	return described;
}

Filed file_into_matter(const std::string &matter_id, const std::string &name, const std::string &bytes) {
	const auto matter = matter_here(matter_id);
	if (!matter || !matter->directory_path || matter->directory_path->empty()) {
		throw std::runtime_error("That Matter is not open on this computer, so the file was not put into it.");
	}
	const std::filesystem::path folder = std::filesystem::u8path(*matter->directory_path);
	std::error_code error;
	if (!std::filesystem::is_directory(folder, error)) {
		throw std::runtime_error("The Matter's folder is not reachable from this computer now, so the file was not put into it.");
	}
	const std::filesystem::path target = free_path_in(folder, safe_file_name(name));
	platform::write_file_durably(target, bytes);
	// Listed at once and read in the background, so the window finds it as soon
	// as it reads the Matter again, never waiting on the folder's watch or on
	// its reading.
	(void)matter::detect_affected_paths(matter_id, {target.u8string()});
	matter::read_documents_in_background(matter_id);
	Filed filed;
	filed.matter_id = matter_id;
	filed.name = target.filename().u8string();
	filed.relative_path = relative_inside(folder, target).value_or(filed.name);
	return filed;
}

namespace {

// A document of the Matter, as a file inside its folder on this computer.
std::filesystem::path document_file(const std::string &matter_id, const std::string &document_id) {
	const auto matter = matter_here(matter_id);
	if (!matter || !matter->directory_path) throw std::runtime_error("That Matter is not open on this computer.");
	const auto document = std::find_if(matter->documents.begin(), matter->documents.end(),
		[&document_id](const auto &entry) { return entry.id == document_id; });
	if (document == matter->documents.end() || document->relative_path.empty()) {
		throw std::runtime_error("That document is not in the Matter's folder on this computer.");
	}
	const std::filesystem::path folder = std::filesystem::u8path(*matter->directory_path);
	const std::filesystem::path file = folder / std::filesystem::u8path(document->relative_path);
	if (!relative_inside(folder, file)) throw std::runtime_error("That document is not inside the Matter's folder.");
	return file;
}

} // namespace

void open_document(const std::string &matter_id, const std::string &document_id) {
	const std::filesystem::path file = document_file(matter_id, document_id);
	const std::string path = file.u8string();
	if (!platform::may_open_as_document(path)) throw std::runtime_error("That file is not a document Singularity opens.");
	if (!platform::open_in_default_application(path)) throw std::runtime_error("The document could not be opened on this computer.");
}

DocumentContent document_content(const std::string &matter_id, const std::string &document_id) {
	const auto matter = matter_here(matter_id);
	if (!matter || !matter->directory_path) throw std::runtime_error("That Matter is not open on this computer.");
	const auto document = std::find_if(matter->documents.begin(), matter->documents.end(),
		[&document_id](const auto &entry) { return entry.id == document_id; });
	if (document == matter->documents.end() || document->relative_path.empty()) {
		throw std::runtime_error("That document is not in the Matter's folder on this computer.");
	}
	const std::filesystem::path folder = std::filesystem::u8path(*matter->directory_path);
	const std::filesystem::path file = folder / std::filesystem::u8path(document->relative_path);
	if (!relative_inside(folder, file)) throw std::runtime_error("That document is not inside the Matter's folder.");
	std::error_code error;
	const auto size = std::filesystem::file_size(file, error);
	if (error) throw std::runtime_error("That document could not be read from the Matter's folder.");
	if (size > kMaxAttachmentBytes) throw std::runtime_error("That document is too large to attach to a message.");
	std::ifstream input(file, std::ios::binary);
	std::ostringstream bytes;
	bytes << input.rdbuf();
	DocumentContent content;
	content.name = file.filename().u8string();
	content.bytes = bytes.str();
	return content;
}

} // namespace singularity::office
