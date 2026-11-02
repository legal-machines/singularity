#include "Document/DocumentFileActions/DocumentFileActions.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"

#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Platform/TrashFile/TrashFile.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace singularity::document {
namespace {

std::string trim(const std::string& value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

bool is_safe_filename_component(const std::string& name) {
	if (name.empty()) return false;
	if (name == "." || name == "..") return false;
	if (name.find('/') != std::string::npos) return false;
	if (name.find('\\') != std::string::npos) return false;
	if (name.find('\0') != std::string::npos) return false;
	return true;
}

std::string join_relative(const std::string& folder, const std::string& name) {
	if (folder.empty()) return name;
	return folder + "/" + name;
}

std::string parent_of(const std::string& relative_path) {
	const auto slash = relative_path.find_last_of('/');
	if (slash == std::string::npos) return "";
	return relative_path.substr(0, slash);
}

std::string require_directory_path(const matters::Matter& state) {
	if (!state.directory_path || state.directory_path->empty()) {
		throw std::runtime_error("Choose this Matter's project folder first.");
	}
	return *state.directory_path;
}

void require_existing_folder(const std::filesystem::path& project_root, const std::string& folder_relative_path) {
	if (folder_relative_path.empty()) return;
	std::error_code error;
	if (std::filesystem::is_directory(project_root / folder_relative_path, error) == false) {
		throw std::runtime_error("That folder doesn't exist.");
	}
}

// Real, freshly re-derived truth for one specific document, immediately
// before a destructive/identity-changing action - see DocumentFileActions.hpp's
// top comment for why a merely-loaded/persisted snapshot is not enough
// (the filesystem watcher is asynchronous). Re-runs the exact Phase 08
// targeted-reconciliation pipeline over this document's last-known real
// path, then requires the result to still be Referenced, available and at
// `expected_version` - never a second identity system, never a full-Project
// rehash.
struct RevalidatedDocument {
	matters::MatterDocument document;
	std::string project_root;
};

RevalidatedDocument revalidate_document(const std::string& matter_id, const std::string& document_id,
		int expected_version) {
	const matters::Matter initial_state = matter::read_project_documents(matter_id);
	const std::string project_root = require_directory_path(initial_state);
	const auto found = std::find_if(initial_state.documents.begin(), initial_state.documents.end(),
			[&](const matters::MatterDocument& entry) { return entry.id == document_id; });
	if (found == initial_state.documents.end()) throw std::runtime_error("This document no longer exists.");
	if (found->source_mode != matters::DocumentSourceMode::Referenced) {
		throw std::runtime_error("This action isn't available for this document.");
	}
	if (documents::registry::is_relative_path_safe(project_root, found->relative_path) == false) {
		throw std::runtime_error("This document's location could not be safely resolved.");
	}

	const std::filesystem::path last_known_absolute_path = std::filesystem::path(project_root) / found->relative_path;
	const matters::Matter revalidated_state = matter::detect_affected_paths(matter_id, {last_known_absolute_path.string()});
	const auto revalidated = std::find_if(revalidated_state.documents.begin(), revalidated_state.documents.end(),
			[&](const matters::MatterDocument& entry) { return entry.id == document_id; });
	if (revalidated == revalidated_state.documents.end()) throw std::runtime_error("This document no longer exists.");
	if (revalidated->available == false) throw std::runtime_error("Singularity can no longer access this file.");
	if (revalidated->version != expected_version) {
		throw std::runtime_error("This document changed since you selected it.");
	}
	return RevalidatedDocument{*revalidated, project_root};
}

// Same-filesystem, collision-safe rename/move: create_hard_link() fails
// atomically (EEXIST) rather than clobbering an existing destination the
// way rename()/renameat() would silently overwrite on POSIX, and preserves
// the same inode DocumentRegistry's identity continuity relies on. Only
// once the new name exists is the old one removed; if that removal fails,
// the new link is rolled back so the file is never left reachable under
// both names.
void rename_or_move_within_project(const std::filesystem::path& old_absolute, const std::filesystem::path& new_absolute) {
	std::error_code link_error;
	std::filesystem::create_hard_link(old_absolute, new_absolute, link_error);
	if (link_error) throw std::runtime_error("Couldn't rename this file: " + link_error.message());

	std::error_code remove_error;
	std::filesystem::remove(old_absolute, remove_error);
	if (remove_error) {
		std::error_code rollback_error;
		std::filesystem::remove(new_absolute, rollback_error);
		if (rollback_error) {
			throw std::runtime_error(
					"This file now exists under both names and Singularity could not clean this up automatically: "
					+ remove_error.message());
		}
		throw std::runtime_error("Couldn't finish renaming this file: " + remove_error.message());
	}
}

} // namespace

matters::Matter rename_document(const std::string& matter_id, const std::string& document_id,
		int expected_version, const std::string& new_name) {
	const std::string trimmed_name = trim(new_name);
	if (is_safe_filename_component(trimmed_name) == false) throw std::runtime_error("Enter a valid file name.");

	const RevalidatedDocument revalidated = revalidate_document(matter_id, document_id, expected_version);
	const matters::MatterDocument& document = revalidated.document;
	const std::filesystem::path project_root = revalidated.project_root;

	const std::string new_relative_path = join_relative(parent_of(document.relative_path), trimmed_name);
	if (new_relative_path == document.relative_path) return matter::read_project_documents(matter_id);
	if (documents::registry::is_relative_path_safe(project_root, new_relative_path) == false) {
		throw std::runtime_error("That name isn't allowed.");
	}

	const std::filesystem::path old_absolute = project_root / document.relative_path;
	const std::filesystem::path new_absolute = project_root / new_relative_path;

	std::error_code exists_error;
	if (std::filesystem::exists(new_absolute, exists_error)) {
		throw std::runtime_error("A file named \"" + trimmed_name + "\" already exists here.");
	}

	rename_or_move_within_project(old_absolute, new_absolute);

	matter::detect_affected_paths(matter_id, {old_absolute.string(), new_absolute.string()});
	return matter::process_project_documents(matter_id);
}

matters::Matter move_document(const std::string& matter_id, const std::string& document_id,
		int expected_version, const std::string& destination_folder_relative_path) {
	const RevalidatedDocument revalidated = revalidate_document(matter_id, document_id, expected_version);
	const matters::MatterDocument& document = revalidated.document;
	const std::filesystem::path project_root = revalidated.project_root;

	const std::string destination_folder = trim(destination_folder_relative_path);
	const std::string filename = std::filesystem::path(document.relative_path).filename().string();
	const std::string new_relative_path = join_relative(destination_folder, filename);
	if (new_relative_path == document.relative_path) return matter::read_project_documents(matter_id);
	if (documents::registry::is_relative_path_safe(project_root, new_relative_path) == false) {
		throw std::runtime_error("That destination isn't allowed.");
	}
	require_existing_folder(project_root, destination_folder);

	const std::filesystem::path old_absolute = project_root / document.relative_path;
	const std::filesystem::path new_absolute = project_root / new_relative_path;

	std::error_code exists_error;
	if (std::filesystem::exists(new_absolute, exists_error)) {
		throw std::runtime_error("A file named \"" + filename + "\" already exists in that folder.");
	}

	rename_or_move_within_project(old_absolute, new_absolute);

	matter::detect_affected_paths(matter_id, {old_absolute.string(), new_absolute.string()});
	return matter::process_project_documents(matter_id);
}

matters::Matter remove_document(const std::string& matter_id, const std::string& document_id, int expected_version) {
	const RevalidatedDocument revalidated = revalidate_document(matter_id, document_id, expected_version);
	const matters::MatterDocument& document = revalidated.document;
	const std::filesystem::path project_root = revalidated.project_root;
	const std::filesystem::path absolute_path = project_root / document.relative_path;

	std::error_code exists_error;
	if (std::filesystem::is_regular_file(absolute_path, exists_error) == false) {
		throw std::runtime_error("Singularity can no longer access this file.");
	}

	const platform::TrashResult trash_result = platform::move_to_trash(absolute_path.string());
	if (trash_result == platform::TrashResult::Failed) {
		throw std::runtime_error(std::string("Singularity couldn't move this file to the ") + platform::trash_name()
			+ ". Nothing was deleted.");
	}
	if (trash_result == platform::TrashResult::Unsupported) {
		throw std::runtime_error(std::string("This computer doesn't support moving files to the ") + platform::trash_name()
			+ ", so Singularity didn't delete this file.");
	}

	matter::detect_affected_paths(matter_id, {absolute_path.string()});
	return matter::process_project_documents(matter_id);
}

matters::Matter create_folder(const std::string& matter_id, const std::string& parent_folder_relative_path,
		const std::string& folder_name) {
	const std::string trimmed_name = trim(folder_name);
	if (is_safe_filename_component(trimmed_name) == false) throw std::runtime_error("Enter a valid folder name.");

	const matters::Matter state = matter::read_project_documents(matter_id);
	const std::filesystem::path project_root = require_directory_path(state);
	const std::string parent_folder = trim(parent_folder_relative_path);
	const std::string new_relative_path = join_relative(parent_folder, trimmed_name);
	if (documents::registry::is_relative_path_safe(project_root, new_relative_path) == false) {
		throw std::runtime_error("That location isn't allowed.");
	}
	require_existing_folder(project_root, parent_folder);

	const std::filesystem::path new_absolute = project_root / new_relative_path;
	std::error_code exists_error;
	if (std::filesystem::exists(new_absolute, exists_error)) {
		throw std::runtime_error("A folder named \"" + trimmed_name + "\" already exists here.");
	}

	std::error_code create_error;
	std::filesystem::create_directory(new_absolute, create_error);
	if (create_error) throw std::runtime_error("Couldn't create this folder: " + create_error.message());

	return matter::detect_affected_paths(matter_id, {});
}

namespace {

// The name a file takes in `folder` when its own is taken: "Brief.pdf", then
// "Brief 2.pdf", "Brief 3.pdf" and on, as Finder keeps both.
std::string free_name_in(const std::filesystem::path& folder, const std::string& filename) {
	std::error_code error;
	if (std::filesystem::exists(folder / std::filesystem::u8path(filename), error) == false) return filename;
	const std::filesystem::path name = std::filesystem::u8path(filename);
	const std::string stem = name.stem().u8string();
	const std::string extension = name.extension().u8string();
	for (int number = 2; number < 10000; ++number) {
		const std::string candidate = stem + " " + std::to_string(number) + extension;
		if (std::filesystem::exists(folder / std::filesystem::u8path(candidate), error) == false) return candidate;
	}
	throw std::runtime_error("This folder already holds too many files named \"" + filename + "\".");
}

} // namespace

matters::Matter add_external_file(const std::string& matter_id, const std::string& destination_folder_relative_path,
		const std::string& source_absolute_path, bool move_source, bool keep_both) {
	std::error_code source_error;
	if (std::filesystem::is_regular_file(source_absolute_path, source_error) == false) {
		throw std::runtime_error("That file could not be found.");
	}

	const matters::Matter state = matter::read_project_documents(matter_id);
	const std::filesystem::path project_root = require_directory_path(state);
	const std::string destination_folder = trim(destination_folder_relative_path);
	std::string filename = std::filesystem::path(source_absolute_path).filename().string();
	if (keep_both == true) filename = free_name_in(project_root / std::filesystem::u8path(destination_folder), filename);
	const std::string new_relative_path = join_relative(destination_folder, filename);
	if (documents::registry::is_relative_path_safe(project_root, new_relative_path) == false) {
		throw std::runtime_error("That destination isn't allowed.");
	}
	require_existing_folder(project_root, destination_folder);

	const std::filesystem::path destination_absolute = project_root / new_relative_path;
	std::error_code exists_error;
	if (std::filesystem::exists(destination_absolute, exists_error)) {
		throw std::runtime_error("A file named \"" + filename + "\" already exists in that folder.");
	}

	if (move_source == false) {
		std::error_code copy_error;
		std::filesystem::copy_file(source_absolute_path, destination_absolute,
				std::filesystem::copy_options::none, copy_error);
		if (copy_error) throw std::runtime_error("Couldn't copy this file: " + copy_error.message());
	} else {
		std::error_code rename_error;
		std::filesystem::rename(source_absolute_path, destination_absolute, rename_error);
		if (rename_error) {
			// Cross-device: rename() cannot move across filesystems, so fall
			// back to copy-then-remove-original - but an explicit Move must
			// never report success while the original still exists, and must
			// never silently leave a duplicate under two locations either.
			std::error_code copy_error;
			std::filesystem::copy_file(source_absolute_path, destination_absolute,
					std::filesystem::copy_options::none, copy_error);
			if (copy_error) throw std::runtime_error("Couldn't move this file: " + copy_error.message());
			// The copy is on the disk before the original is removed: a crash
			// in between must never leave neither (a USB drive's only copy).
			platform::flush_file_to_disk(destination_absolute);

			std::error_code remove_error;
			std::filesystem::remove(source_absolute_path, remove_error);
			if (remove_error) {
				std::error_code rollback_error;
				std::filesystem::remove(destination_absolute, rollback_error);
				if (rollback_error) {
					throw std::runtime_error(
							"This file was copied into the Project, but Singularity could not remove the original and "
							"could not undo the copy either - both copies currently exist: " + remove_error.message());
				}
				throw std::runtime_error(
						"Singularity copied this file but could not remove the original, so the move was cancelled: "
						+ remove_error.message());
			}
		}
	}

	matter::detect_affected_paths(matter_id, {destination_absolute.string()});
	return matter::process_project_documents(matter_id);
}

} // namespace singularity::document
