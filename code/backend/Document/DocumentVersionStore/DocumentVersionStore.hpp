#pragma once

#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace singularity::documents {

// The previous version of a Project document Singularity is about to replace. It
// is kept in Project data, never beside the document: in `.lm` for a Project
// that has it (so "Remove Singularity data" removes it and a clean copy never
// carries it), otherwise in the workspace's Singularity data for that Project.
// Nothing is kept for a document Singularity creates.
//
// The first version kept of a document is the document as it was before Singularity
// ever changed it - its original - and is kept for as long as the
// document is: whatever Singularity writes to it later, the lawyer can always go
// back to what they had. Of the versions after it the newest are kept, so the
// store holds kRetainedDocumentVersions versions of one document at most.

inline constexpr std::size_t kRetainedDocumentVersions = 5;

// A kept version is "v<N>-<mutation>" plus the document's own extension; its
// sidecar is "v<N>-<mutation>.version.json", a name no kept document can take
// (a JSON document's version is "v<N>-<mutation>.json").
inline constexpr std::string_view kDocumentVersionSidecarSuffix = ".version.json";

inline std::string document_version_stem(const std::string &file_name) {
	return file_name.substr(0, file_name.find('.'));
}

inline bool is_document_version_sidecar(const std::string &file_name) {
	return file_name.size() > kDocumentVersionSidecarSuffix.size()
		&& file_name.compare(file_name.size() - kDocumentVersionSidecarSuffix.size(), kDocumentVersionSidecarSuffix.size(),
			kDocumentVersionSidecarSuffix) == 0;
}

inline std::filesystem::path document_versions_root(const std::optional<std::string> &projectDirectory,
		const std::filesystem::path &workspacePath, const std::string &projectId) {
	if (projectDirectory && std::filesystem::is_directory(std::filesystem::path(*projectDirectory) / ".lm"))
		return std::filesystem::path(*projectDirectory) / ".lm" / "Documents" / "Versions";
	return workspacePath / ".singularity-workspace" / "document-versions" / projectId;
}

struct PreservedDocumentVersion {
	std::filesystem::path path;
	std::string fingerprint;
};

// One version Singularity kept of a document, as its sidecar records it.
struct KeptDocumentVersion {
	std::filesystem::path path;
	int version = 0;
	std::string fingerprint;
	// When it was kept (UTC, ISO 8601); empty for a version an earlier Singularity
	// kept without saying.
	std::string kept_at;
	std::string mutation_id;
	// The document as it was before Singularity first changed it.
	bool is_original = false;
	// The computer whose Singularity kept it (Platform/ActorIdentity device
	// id); empty for a version kept before this was recorded.
	std::string device_id;
};

inline std::string document_version_timestamp() {
	const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm utc{};
#if defined(_WIN32)
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	char text[32];
	std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return text;
}

// Every version kept of one document whose sidecar says what it is, oldest
// first (by the document version it was, then by when it was kept). A file
// whose identity is not recorded is never presented as a previous version.
// The original is the one its sidecar marks so; in a store an earlier Singularity
// kept, where none is marked, it is the oldest kept.
[[nodiscard]] inline std::vector<KeptDocumentVersion> list_kept_document_versions(
		const std::filesystem::path &versionsRoot, const std::string &documentId) noexcept {
	std::vector<KeptDocumentVersion> kept;
	std::error_code error;
	const std::filesystem::path directory = versionsRoot / documentId;
	if (documentId.empty() || !std::filesystem::is_directory(directory, error)) return kept;
	bool hasMarkedOriginal = false;
	for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end;
			entry.increment(error)) {
		const std::string name = entry->path().filename().string();
		if (name.empty() || name.front() == '.' || !is_document_version_sidecar(name)) continue;
		try {
			std::ifstream input(entry->path(), std::ios::binary);
			if (!input) continue;
			const nlohmann::json sidecar = nlohmann::json::parse(input, nullptr, false);
			if (!sidecar.is_object() || !sidecar.contains("file") || !sidecar.at("file").is_string()) continue;
			if (!sidecar.contains("version") || !sidecar.at("version").is_number_integer()) continue;
			// The kept copy lies beside its sidecar: a name, never a path. In a
			// shared Matter anyone with the folder can write a sidecar, and one
			// naming "../.." must never send a restore to a file elsewhere.
			const std::string file = sidecar.at("file").get<std::string>();
			if (file.empty() || file == "." || file == ".." || file.find('/') != std::string::npos
					|| file.find('\\') != std::string::npos || file.find('\0') != std::string::npos)
				continue;
			KeptDocumentVersion version;
			version.path = directory / std::filesystem::u8path(file);
			std::error_code linkError;
			if (std::filesystem::is_symlink(version.path, linkError)) continue;
			version.version = sidecar.at("version").get<int>();
			version.fingerprint = sidecar.value("fingerprint", std::string());
			version.kept_at = sidecar.value("keptAt", std::string());
			version.mutation_id = sidecar.value("mutationId", std::string());
			version.is_original = sidecar.value("original", false);
			version.device_id = sidecar.value("deviceId", std::string());
			std::error_code missing;
			if (!std::filesystem::is_regular_file(version.path, missing) || missing) continue;
			if (version.is_original) hasMarkedOriginal = true;
			kept.push_back(std::move(version));
		} catch (const std::exception &) {
			continue;
		}
	}
	std::sort(kept.begin(), kept.end(), [](const KeptDocumentVersion &left, const KeptDocumentVersion &right) {
		if (left.version != right.version) return left.version < right.version;
		return left.kept_at < right.kept_at;
	});
	if (!hasMarkedOriginal && !kept.empty()) kept.front().is_original = true;
	return kept;
}

// The version kept of one document at `version`, if one is.
[[nodiscard]] inline std::optional<KeptDocumentVersion> find_kept_document_version(
		const std::filesystem::path &versionsRoot, const std::string &documentId, int version) noexcept {
	std::optional<KeptDocumentVersion> found;
	for (const KeptDocumentVersion &kept : list_kept_document_versions(versionsRoot, documentId))
		if (kept.version == version) found = kept;
	return found;
}

// The newest version Singularity kept of one document that is not the
// document as it is now (`currentFingerprint`) - what "what changed" is
// measured against: a write keeps the version it wrote as well as the one
// before it, and the version it wrote is the document itself until something
// changes it. Nothing when no such version was kept.
[[nodiscard]] inline std::optional<KeptDocumentVersion> latest_kept_document_version(
		const std::filesystem::path &versionsRoot, const std::string &documentId,
		const std::string &currentFingerprint = std::string()) noexcept {
	const std::vector<KeptDocumentVersion> kept = list_kept_document_versions(versionsRoot, documentId);
	for (auto version = kept.rbegin(); version != kept.rend(); ++version)
		if (currentFingerprint.empty() || version->fingerprint != currentFingerprint) return *version;
	return std::nullopt;
}

// Copies `source` (the document at `version`, whose bytes must hash to
// `fingerprint`) into the version store before it is replaced, durably and
// with a sidecar recording what it is. Throws when the copy cannot be made or
// does not match: a change is never committed without its previous version.
//
// A version already kept with these very bytes is not kept twice - a change
// that stopped after its version was kept and is made again, or a document
// restored to a version and changed again, costs no place in the store.
inline PreservedDocumentVersion preserve_document_version(const std::filesystem::path &versionsRoot,
		const std::string &documentId, const std::string &relativePath, int version, const std::string &fingerprint,
		const std::filesystem::path &source, const std::string &mutationId, const std::string &keeperDeviceId = std::string()) {
	if (documentId.empty() || documentId.find('/') != std::string::npos || documentId.find('\\') != std::string::npos
			|| documentId == "." || documentId == "..")
		throw std::runtime_error("The document's previous version could not be kept: its identity is invalid.");
	const std::vector<KeptDocumentVersion> alreadyKept = list_kept_document_versions(versionsRoot, documentId);
	for (const KeptDocumentVersion &kept : alreadyKept) {
		if (kept.version != version || kept.fingerprint != fingerprint) continue;
		if (platform::sha256_hex_digest_of_file(kept.path) != fingerprint) continue;
		if (platform::sha256_hex_digest_of_file(source) != fingerprint)
			throw std::runtime_error("The document changed while its previous version was being kept, so nothing was changed.");
		return {kept.path, fingerprint};
	}
	const std::filesystem::path directory = versionsRoot / documentId;
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	if (error) throw std::runtime_error("The document's previous version could not be kept, so nothing was changed.");
	const std::string stem = "v" + std::to_string(version) + "-" + mutationId.substr(0, 16);
	const std::filesystem::path destination = directory / (stem + source.extension().string());
	const std::filesystem::path staged = platform::staged_sibling_path(destination);
	const std::string sidecarName = stem + std::string(kDocumentVersionSidecarSuffix);
	const std::filesystem::path stagedSidecar = directory / ("." + sidecarName + ".staged");
	try {
		platform::stage_copy_of_file(source, staged);
		if (platform::sha256_hex_digest_of_file(staged) != fingerprint)
			throw std::runtime_error("The document changed while its previous version was being kept, so nothing was changed.");
		// The first version kept is the document before Singularity changed it at all.
		const bool isOriginal = alreadyKept.empty();
		platform::write_file_durably(stagedSidecar, nlohmann::json{
			{"format", "singularity-document-version"}, {"schemaVersion", 1},
			{"documentId", documentId}, {"relativePath", relativePath}, {"version", version},
			{"fingerprint", fingerprint}, {"mutationId", mutationId},
			{"file", destination.filename().u8string()}, {"keptAt", document_version_timestamp()},
			{"original", isOriginal}, {"deviceId", keeperDeviceId}}.dump());
		platform::commit_staged_file(staged, destination, platform::StagedCommitMode::ReplaceExisting, false);
		platform::commit_staged_file(stagedSidecar, directory / sidecarName,
			platform::StagedCommitMode::ReplaceExisting, false);
	} catch (const std::filesystem::filesystem_error &) {
		platform::discard_staged_file(staged);
		std::filesystem::remove(stagedSidecar, error);
		throw std::runtime_error("The document's previous version could not be kept, so nothing was changed.");
	} catch (...) {
		platform::discard_staged_file(staged);
		std::filesystem::remove(stagedSidecar, error);
		throw;
	}
	return {destination, fingerprint};
}

// Keeps the document's original and the newest versions after it -
// kRetainedDocumentVersions in all - and removes the ones between.
//
// In a Matter shared between computers each computer prunes only what its own
// Singularity kept (`keeperDeviceId`, and versions kept before computers were
// recorded): a version another computer kept is that computer's to keep, and
// is never removed here.
inline void prune_document_versions(const std::filesystem::path &versionsRoot, const std::string &documentId,
		const std::string &keeperDeviceId = std::string()) noexcept {
	std::vector<KeptDocumentVersion> kept = list_kept_document_versions(versionsRoot, documentId);
	if (!keeperDeviceId.empty())
		kept.erase(std::remove_if(kept.begin(), kept.end(), [&](const KeptDocumentVersion &version) {
			return !version.is_original && !version.device_id.empty() && version.device_id != keeperDeviceId;
		}), kept.end());
	if (kept.size() <= kRetainedDocumentVersions) return;
	std::size_t originals = 0;
	for (const KeptDocumentVersion &version : kept)
		if (version.is_original) ++originals;
	const std::size_t newestKept = kRetainedDocumentVersions > originals ? kRetainedDocumentVersions - originals : 0;
	std::size_t newestSeen = 0;
	std::error_code error;
	for (std::size_t index = kept.size(); index-- > 0;) {
		const KeptDocumentVersion &version = kept[index];
		if (version.is_original) continue;
		if (newestSeen < newestKept) {
			++newestSeen;
			continue;
		}
		const std::filesystem::path sidecar = version.path.parent_path()
			/ (document_version_stem(version.path.filename().string()) + std::string(kDocumentVersionSidecarSuffix));
		std::filesystem::remove(version.path, error);
		std::filesystem::remove(sidecar, error);
	}
}

} // namespace singularity::documents
