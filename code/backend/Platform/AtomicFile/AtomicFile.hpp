#pragma once

#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <copyfile.h>
#include <sys/attr.h>
#include <sys/stdio.h>
#else
#include <sys/xattr.h>
#endif
#endif
#include <vector>

#include "Platform/DiskSpace/DiskSpace.hpp"

namespace singularity::platform {

// Staged replacement of a user's file: the new bytes are built and verified in
// a hidden sibling of the destination (same directory, so the commit is one
// rename on one filesystem; dot-prefixed, so no document scan registers it and
// no clean copy carries it), then committed atomically. A failure before the
// commit leaves the destination byte-for-byte untouched.

inline constexpr std::string_view kStagedFileMarker = ".lm-tmp-";

// ".<file name>.lm-tmp-<16 hex>" beside `destination`.
//
// Every staged write begins here, so this is where a full disk is refused: a
// staged replacement needs room for a second copy of the file beside the
// first, and a Word or Excel change on a full disk is refused as DiskFull,
// before anything is written, instead of failing half-way with a technical
// error.
inline std::filesystem::path staged_sibling_path(const std::filesystem::path &destination) {
	std::error_code sizeError;
	const std::uintmax_t currentSize = std::filesystem::file_size(destination, sizeError);
	require_free_space(destination.parent_path(), sizeError ? 0 : currentSize);
	static std::mutex mutex;
	static std::mt19937_64 random(std::random_device{}());
	std::uint64_t value = 0;
	{
		const std::lock_guard<std::mutex> lock(mutex);
		value = random();
	}
	char suffix[17];
	std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(value));
	return destination.parent_path()
		/ ("." + destination.filename().string() + std::string(kStagedFileMarker) + suffix);
}

// Singularity's own records - its settings, the Project list, the routing log - are
// rewritten whole, beside a `.previous` copy of the one each replaces: the room
// both need is asked for at once, so a full disk refuses the save as DiskFull
// before either file is begun. Their bytes then go through write_file_durably()
// and commit_staged_file(), as a document's do.
inline void require_room_for_record(const std::filesystem::path &record, std::uintmax_t bytes) {
	std::error_code sizeError;
	const std::uintmax_t currentSize = std::filesystem::file_size(record, sizeError);
	require_free_space(record.parent_path(), bytes + (sizeError ? 0 : currentSize));
}

// The folder one of Singularity's own records lives in, made if it is missing. A
// full disk is refused as DiskFull, like the save that needed the folder; any
// other failure is said without the folder's path.
inline void ensure_record_directory(const std::filesystem::path &directory) {
	std::error_code error;
	if (std::filesystem::is_directory(directory, error)) return;
	// Asked of the nearest folder that exists: a disk with no room for the
	// record is refused before any folder is begun.
	std::filesystem::path existing = directory;
	while (!existing.empty() && existing.has_parent_path() && existing != existing.parent_path()
			&& !std::filesystem::exists(existing, error))
		existing = existing.parent_path();
	if (!existing.empty()) require_free_space(existing, 0);
	error.clear();
	std::filesystem::create_directories(directory, error);
	if (!error) return;
	if (error.category() == std::generic_category() || error.category() == std::system_category()) {
		if (is_disk_full_error(error.value())) throw DiskFull();
#if defined(_WIN32)
		if (error.value() == static_cast<int>(ERROR_DISK_FULL) || error.value() == static_cast<int>(ERROR_HANDLE_DISK_FULL))
			throw DiskFull();
#endif
	}
	if (error == std::errc::no_space_on_device) throw DiskFull();
	throw std::runtime_error("Singularity could not make the folder it keeps this record in, so nothing was saved.");
}

// A staged or crash-leftover temporary name Singularity itself creates: the
// current hidden form, libzip's ".part" companion of a staged package, and the
// visible names earlier builds used (".singularity-workspace-tmp-", ".singularity-workspace-patch-").
inline bool is_staged_file_name(std::string_view name) noexcept {
	if (name.find(kStagedFileMarker) != std::string_view::npos) return true;
	return name.find(".singularity-workspace-tmp-") != std::string_view::npos || name.find(".singularity-workspace-patch-") != std::string_view::npos;
}

#if defined(_WIN32)
// MoveFileExW, retried for up to two seconds while another process - a
// scanner, an indexer, a sync client that has just opened one of the files -
// briefly holds it: Windows refuses the rename meanwhile, where POSIX never
// does. Any other failure is returned at once. ERROR_SUCCESS or the last error.
inline DWORD move_file_retrying_transient(const std::filesystem::path &source,
		const std::filesystem::path &destination, DWORD flags) {
	for (int attempt = 0;; ++attempt) {
		if (MoveFileExW(source.c_str(), destination.c_str(), flags) != 0) return ERROR_SUCCESS;
		const DWORD error = GetLastError();
		const bool transient =
			error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION;
		if (!transient || attempt == 40) return error;
		Sleep(50);
	}
}
#endif

// Moves `source` over `destination` in one rename on one file system, and says
// why it could not ("Windows error N", or the POSIX error) - std::nullopt on
// success. A file replaces a file; a directory moves only onto an absent
// destination (Windows cannot replace one), so a half-migrated destination is
// never merged into on any platform.
inline std::optional<std::string> replace_file(const std::filesystem::path &source,
		const std::filesystem::path &destination) {
#if defined(_WIN32)
	std::error_code directory_error;
	const DWORD flags = std::filesystem::is_directory(source, directory_error)
		? MOVEFILE_WRITE_THROUGH
		: MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
	const DWORD error = move_file_retrying_transient(source, destination, flags);
	if (error == ERROR_SUCCESS) return std::nullopt;
	return "Windows error " + std::to_string(error);
#else
	if (std::rename(source.c_str(), destination.c_str()) == 0) return std::nullopt;
	return std::error_code(errno, std::generic_category()).message();
#endif
}

// Writes `bytes` to `path` (created or truncated) and flushes them to stable
// storage before returning.
// A file's bytes on the disk itself, not in a cache a power loss empties.
inline void flush_file_to_disk(const std::filesystem::path &path) noexcept {
#if defined(_WIN32)
	// FlushFileBuffers needs a handle open for writing; MOVEFILE_WRITE_THROUGH
	// on a later rename does not flush the bytes themselves.
	const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle != INVALID_HANDLE_VALUE) {
		(void)FlushFileBuffers(handle);
		CloseHandle(handle);
	}
#else
	const int descriptor = ::open(path.c_str(), O_RDONLY);
	if (descriptor >= 0) {
#if defined(__APPLE__)
		if (::fcntl(descriptor, F_FULLFSYNC) != 0) (void)::fsync(descriptor);
#else
		(void)::fsync(descriptor);
#endif
		::close(descriptor);
	}
#endif
}

inline void write_file_durably(const std::filesystem::path &path, std::string_view bytes) {
	require_free_space(path.parent_path(), bytes.size());
	{
		errno = 0;
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output) {
			if (is_disk_full_error(errno)) throw DiskFull();
			throw std::runtime_error("A staged file could not be created.");
		}
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		output.flush();
		if (!output) {
			// The partial file is not left behind on a disk that is full.
			const bool isDiskFull = is_disk_full_error(errno);
			output.close();
			std::error_code ignored;
			if (isDiskFull == true) std::filesystem::remove(path, ignored);
			if (isDiskFull == true) throw DiskFull();
			throw std::runtime_error("A staged file could not be written completely.");
		}
	}
	flush_file_to_disk(path);
}

// A staged copy of `source`: its bytes only, in a new file Singularity may write.
// Never std::filesystem::copy_file - that gives the copy the source's own
// permissions, so a read-only document would yield a read-only stage the
// change could not be built in, and the lawyer would hear of a technical
// failure instead of the document's own state. The document's metadata is
// carried onto the stage only at the commit (carry_document_metadata).
inline void stage_copy_of_file(const std::filesystem::path &source, const std::filesystem::path &staged) {
	std::string bytes;
	{
		std::ifstream input(source, std::ios::binary);
		if (!input) throw std::runtime_error("The file could not be read, so nothing was changed.");
		bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		if (input.bad()) throw std::runtime_error("The file could not be read, so nothing was changed.");
	}
	write_file_durably(staged, bytes);
}

enum class StagedCommitMode {
	// The destination exists and is replaced.
	ReplaceExisting,
	// The destination must not exist; an entry that appeared meanwhile is never
	// overwritten.
	CreateExclusive,
};

// Test seam: a fault raised immediately before the next commit of a user's
// document (commits of Singularity's own records pass `fault_injectable` false).
inline std::atomic<bool> &staged_commit_fault_for_testing() {
	static std::atomic<bool> value{false};
	return value;
}

// Word, Excel and PowerPoint mark a document open for editing with an owner
// file beside it: "~$" and the file's name - its first one or two characters
// dropped for a longer name. While it is there, the program holding the
// document writes its own copy back over anything written underneath it.
// The first one or two CHARACTERS: where a name is UTF-8 a Cyrillic or accented
// letter is two bytes or more, and "~$говор поставки.docx" is the owner file
// of "Договор поставки.docx" (dropping bytes never found it, and a change was
// written under an open Word that then saved its own copy over it).
// LibreOffice marks a document it has open with ".~lock.<name>#" instead.
inline bool office_document_is_open(const std::filesystem::path &document) {
	using StringType = std::filesystem::path::string_type;
	const StringType name = document.filename().native();
	const StringType mark = std::filesystem::path("~$").native();
	std::error_code error;
	std::size_t drop = 0;
	for (int characters = 0; characters <= 2 && drop < name.size(); ++characters) {
		if (std::filesystem::exists(document.parent_path() / (mark + name.substr(drop)), error)) return true;
		++drop;
		if constexpr (sizeof(typename StringType::value_type) == 1)
			while (drop < name.size() && (static_cast<unsigned char>(name[drop]) & 0xC0) == 0x80) ++drop;
	}
	const StringType libreOffice = std::filesystem::path(".~lock.").native() + name + std::filesystem::path("#").native();
	return std::filesystem::exists(document.parent_path() / libreOffice, error);
}

// The program a document of this kind is edited in, named by its extension:
// the one a lawyer is asked to close. Empty for a kind no one program owns.
inline std::string office_application_for(const std::filesystem::path &document) {
	std::string extension = document.extension().string();
	for (char &character : extension) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	if (extension == ".docx" || extension == ".doc" || extension == ".docm" || extension == ".dotx" || extension == ".rtf")
		return "Word";
	if (extension == ".xlsx" || extension == ".xls" || extension == ".xlsm" || extension == ".csv" || extension == ".tsv")
		return "Excel";
	if (extension == ".pptx" || extension == ".ppt") return "PowerPoint";
	return {};
}

// The refusal a change gets while its document is open elsewhere: nothing was
// written, and what to do is plain - naming the program the document is open
// in, not always Word.
inline std::string office_document_open_message(const std::string &document_name) {
	const std::string application = office_application_for(std::filesystem::u8path(document_name));
	if (application.empty())
		return document_name + " is open in another program. Close it there first - Singularity never writes under an "
			"open document, because that program would save its own copy over the change.";
	return document_name + " is open in " + application + ". Close it there first - Singularity never writes under an "
		"open document, because " + application + " would save its own copy over the change.";
}

// The refusal a change gets when its document is no longer the version it was
// prepared for - edited by the lawyer, a colleague, or another change meanwhile.
inline std::string document_changed_since_prepared_message(const std::string &document_name) {
	return document_name + " changed after this change was prepared, so nothing was written. Prepare it again for the "
		"version it has now.";
}

// Why a correct change to a Project document was not written: the document
// itself was not in a state Singularity may write it in. Each says what happened
// in plain words (what()) and is told apart by its kind, so the change ends as
// a conflict the lawyer can prepare again - never as a generic failure.
enum class DocumentWriteConflictKind {
	// The file on disk is no longer the version the change was prepared for.
	ChangedOnDisk,
	// Word, Excel or PowerPoint holds the file open.
	OpenElsewhere,
	// The file is marked read-only or locked.
	ReadOnly,
	// The name an approved new document was to have is taken now.
	NameTaken,
};

inline const char *to_string(DocumentWriteConflictKind kind) noexcept {
	switch (kind) {
		case DocumentWriteConflictKind::ChangedOnDisk: return "target_changed_on_disk";
		case DocumentWriteConflictKind::OpenElsewhere: return "target_open_elsewhere";
		case DocumentWriteConflictKind::ReadOnly: return "target_read_only";
		case DocumentWriteConflictKind::NameTaken: return "target_name_taken";
	}
	return "target_changed_on_disk";
}

class DocumentWriteConflict final : public std::runtime_error {
public:
	DocumentWriteConflict(DocumentWriteConflictKind kind, const std::string &message)
		: std::runtime_error(message)
		, kind_(kind)
	{
	}

	DocumentWriteConflictKind kind() const noexcept { return kind_; }

private:
	DocumentWriteConflictKind kind_;
};

// True when the file is marked read-only or locked: a Read only permission,
// Finder's "Locked", the Windows read-only attribute, a read-only volume. A
// replacing rename needs only the folder's permission, so without this check
// such a file would be silently replaced by a writable copy of itself.
inline bool document_is_write_protected(const std::filesystem::path &document) {
#if defined(_WIN32)
	const DWORD attributes = GetFileAttributesW(document.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
	struct stat status;
	if (::stat(document.c_str(), &status) != 0) return false;
#if defined(__APPLE__)
	if ((status.st_flags & (UF_IMMUTABLE | SF_IMMUTABLE | UF_APPEND | SF_APPEND)) != 0) return true;
#endif
	return ::access(document.c_str(), W_OK) != 0 && errno != ENOENT;
#endif
}

inline std::string document_write_protected_message(const std::string &document_name) {
#if defined(__APPLE__)
	const std::string where = "in Finder's Get Info";
#elif defined(_WIN32)
	const std::string where = "in its Properties";
#else
	const std::string where = "in its permissions";
#endif
	return document_name + " is marked read-only or locked on this computer, so Singularity did not change it and nothing "
		"was written. If it should change, allow changes to it (" + where + "), then prepare the change again.";
}

// Gives a staged replacement the destination's own metadata - its permissions,
// ownership, ACL, Finder tags and comments and its other extended attributes,
// and the date it was created - so the document keeps them once the rename
// puts the new bytes in its place, as it does when Word saves it. Its
// modification time is then now: this is a new version of the document. Best
// effort: bytes that were verified are never held back for a tag.
inline void carry_document_metadata(const std::filesystem::path &destination, const std::filesystem::path &staged) noexcept {
#if defined(_WIN32)
	// Its attributes (hidden, archive; a read-only one is never replaced) and the
	// date it was created. Tunnelling would restore the date only for a rename
	// within seconds of a delete, not for a replacing move.
	const DWORD attributes = GetFileAttributesW(destination.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		const DWORD settable = attributes & (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM
			| FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
		(void)SetFileAttributesW(staged.c_str(), settable == 0 ? FILE_ATTRIBUTE_NORMAL : settable);
	}
	const HANDLE source = CreateFileW(destination.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (source == INVALID_HANDLE_VALUE) return;
	FILETIME created;
	const bool isRead = GetFileTime(source, &created, nullptr, nullptr) != 0;
	CloseHandle(source);
	if (isRead == false) return;
	const HANDLE target = CreateFileW(staged.c_str(), FILE_WRITE_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (target == INVALID_HANDLE_VALUE) return;
	(void)SetFileTime(target, &created, nullptr, nullptr);
	CloseHandle(target);
#else
	struct stat status;
	if (::stat(destination.c_str(), &status) != 0) return;
#if defined(__APPLE__)
	const int source = ::open(destination.c_str(), O_RDONLY);
	const int target = ::open(staged.c_str(), O_WRONLY);
	bool isCarried = false;
	// COPYFILE_METADATA without COPYFILE_DATA: the staged bytes stay as they are.
	if (source >= 0 && target >= 0) isCarried = ::fcopyfile(source, target, nullptr, COPYFILE_METADATA) == 0;
	if (source >= 0) ::close(source);
	if (target >= 0) ::close(target);
	if (isCarried == false) (void)::chmod(staged.c_str(), status.st_mode & 07777);
	(void)::utimes(staged.c_str(), nullptr);
	// COPYFILE_METADATA leaves the creation date behind, and the stage was
	// created now: Finder's "Created" is set back to the document's own. Set
	// after the modification time, which moves a creation date only forward.
	struct attrlist list = {};
	list.bitmapcount = ATTR_BIT_MAP_COUNT;
	list.commonattr = ATTR_CMN_CRTIME;
	struct timespec created = status.st_birthtimespec;
	(void)::setattrlist(staged.c_str(), &list, &created, sizeof(created), 0);
#else
	// Owner and group first (a change of owner clears set-id bits), then the
	// permissions, then every extended attribute: user.* tags and comments, and
	// the POSIX ACL, which Linux keeps as system.posix_acl_* attributes. The
	// creation date cannot be set here; the kernel keeps it for itself.
	if (::chown(staged.c_str(), status.st_uid, status.st_gid) != 0)
		(void)::chown(staged.c_str(), static_cast<uid_t>(-1), status.st_gid);
	(void)::chmod(staged.c_str(), status.st_mode & 07777);
	const ssize_t namesSize = ::listxattr(destination.c_str(), nullptr, 0);
	if (namesSize > 0) {
		std::vector<char> names(static_cast<std::size_t>(namesSize));
		const ssize_t listed = ::listxattr(destination.c_str(), names.data(), names.size());
		for (ssize_t offset = 0; listed > 0 && offset < listed;) {
			const char *name = names.data() + offset;
			offset += static_cast<ssize_t>(std::char_traits<char>::length(name)) + 1;
			const ssize_t valueSize = ::getxattr(destination.c_str(), name, nullptr, 0);
			if (valueSize < 0) continue;
			std::vector<char> value(static_cast<std::size_t>(valueSize) + 1);
			const ssize_t read = ::getxattr(destination.c_str(), name, value.data(), value.size());
			if (read >= 0) (void)::setxattr(staged.c_str(), name, value.data(), static_cast<std::size_t>(read), 0);
		}
	}
#endif
#endif
}

// A document is written by one writer at a time, from the moment its target is
// resolved until the write is committed and reconciled, whichever tool writes
// it: two approvals landing together would otherwise each prove the same old
// version, and the second rename would silently undo the first change. Keyed
// by the document's full path; the thread that holds it may take it again.
//
// In a Matter shared between computers the same holds across them: the first
// hold also leaves a record in the Matter's own `.lm/Locks`, created only if
// none is there (exclusive creation, which a network share keeps), and a
// writer finding another computer's fresh record writes nothing. A record two
// minutes old was left by a writer that stopped, and is taken over. On a
// synced folder a record arrives late, if at all: a save made elsewhere at the
// same moment is found afterwards (Matter/SharedMatter), not prevented here.
class DocumentWriteLock final {
public:
	explicit DocumentWriteLock(const std::filesystem::path &document)
		: key_(key_for(document))
	{
		std::unique_lock<std::mutex> lock(registry_mutex());
		std::map<std::string, Holder> &held = holders();
		const std::thread::id self = std::this_thread::get_id();
		registry_released().wait(lock, [&] {
			const auto found = held.find(key_);
			return found == held.end() || found->second.owner == self;
		});
		Holder &holder = held[key_];
		holder.owner = self;
		++holder.depth;
		if (holder.depth != 1) return;
		lock.unlock();
		record_ = shared_record_for(document);
		if (take_shared_record() == true) return;
		// Another computer writes it now: nothing is held here either.
		record_.reset();
		release();
		throw DocumentWriteConflict(DocumentWriteConflictKind::OpenElsewhere,
			document.filename().u8string() + " is being written by Singularity on another computer. Nothing was written; "
			"try again in a moment.");
	}

	~DocumentWriteLock() {
		release();
	}

	DocumentWriteLock(const DocumentWriteLock &) = delete;
	DocumentWriteLock &operator=(const DocumentWriteLock &) = delete;

	// Whether some writer holds `document` now (tests).
	static bool is_held(const std::filesystem::path &document) {
		const std::lock_guard<std::mutex> lock(registry_mutex());
		return holders().count(key_for(document)) > 0;
	}

private:
	struct Holder {
		std::thread::id owner;
		std::size_t depth = 0;
	};

	void release() {
		bool isLast = false;
		{
			const std::lock_guard<std::mutex> lock(registry_mutex());
			std::map<std::string, Holder> &held = holders();
			const auto found = held.find(key_);
			if (found != held.end() && --found->second.depth == 0) {
				held.erase(found);
				isLast = true;
			}
		}
		if (isLast == true) drop_shared_record();
		registry_released().notify_all();
	}

	// This process, as its records say: who left a record, so only it removes it.
	static const std::string &process_token() {
		static const std::string token = [] {
			std::random_device device;
			return std::to_string(device()) + "-" + std::to_string(device());
		}();
		return token;
	}

	// The Matter's `.lm` is found from the document: the nearest folder above
	// it that has one. A document outside any Matter store has no record.
	static std::optional<std::filesystem::path> shared_record_for(const std::filesystem::path &document) {
		std::error_code error;
		std::filesystem::path folder = std::filesystem::absolute(document, error).parent_path();
		if (error) return std::nullopt;
		for (int depth = 0; depth < 32 && !folder.empty(); ++depth) {
			if (std::filesystem::is_directory(folder / ".lm", error)) {
				// Named by the document's place in the Matter - its path from the
				// folder that holds `.lm`, case folded - and a hash every computer
				// computes alike (FNV-1a), so two computers that mount the share
				// at different places, or build with different libraries, name
				// one document's record the same and hold each other off. Its
				// absolute path and std::hash named it differently on each.
				std::error_code relativeError;
				const std::filesystem::path relative = std::filesystem::relative(
					std::filesystem::weakly_canonical(std::filesystem::absolute(document, relativeError), relativeError),
					std::filesystem::weakly_canonical(folder, relativeError), relativeError);
				std::string key = relativeError ? key_for(document) : relative.generic_u8string();
				for (char &character : key) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
				std::uint64_t hash = 14695981039346656037ull;
				for (const char character : key) {
					hash ^= static_cast<unsigned char>(character);
					hash *= 1099511628211ull;
				}
				char name[32];
				std::snprintf(name, sizeof(name), "w_%016llx.lock", static_cast<unsigned long long>(hash));
				return folder / ".lm" / "Locks" / name;
			}
			const std::filesystem::path parent = folder.parent_path();
			if (parent == folder) break;
			folder = parent;
		}
		return std::nullopt;
	}

	bool take_shared_record() {
		if (!record_) return true;
		std::error_code error;
		std::filesystem::create_directories(record_->parent_path(), error);
		// A share no record can be written to keeps the write's own checks.
		if (error) {
			record_.reset();
			return true;
		}
		for (int attempt = 0; attempt < 2; ++attempt) {
			std::FILE *file = std::fopen(record_->string().c_str(), "wx");
			if (file != nullptr) {
				std::fputs(process_token().c_str(), file);
				std::fclose(file);
				// Two computers taking over one stale record can each remove it
				// and write their own: the record is ours only if it still says so.
				std::ifstream input(*record_, std::ios::binary);
				const std::string holder((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
				if (holder != process_token()) return false;
				is_record_ours_ = true;
				return true;
			}
			if (errno != EEXIST) {
				record_.reset();
				return true;
			}
			const auto written = std::filesystem::last_write_time(*record_, error);
			if (error) {
				error.clear();
				continue;
			}
			if (std::filesystem::file_time_type::clock::now() - written < std::chrono::minutes(2)) return false;
			std::filesystem::remove(*record_, error);
		}
		return false;
	}

	void drop_shared_record() {
		if (!record_ || is_record_ours_ == false) return;
		std::ifstream input(*record_, std::ios::binary);
		const std::string holder((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		input.close();
		std::error_code error;
		if (holder == process_token()) std::filesystem::remove(*record_, error);
		record_.reset();
		is_record_ours_ = false;
	}

	// Case is folded: on a case-insensitive volume "Lease.docx" and "lease.docx"
	// are one file, and holding two names of one file apart is the failure this
	// lock exists to prevent. On a case-sensitive volume it only waits a little.
	static std::string key_for(const std::filesystem::path &document) {
		std::error_code error;
		std::filesystem::path full = std::filesystem::weakly_canonical(std::filesystem::absolute(document, error), error);
		if (error) full = document.lexically_normal();
		std::string key = full.generic_u8string();
		for (char &character : key) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		return key;
	}

	static std::mutex &registry_mutex() {
		static std::mutex mutex;
		return mutex;
	}

	static std::condition_variable &registry_released() {
		static std::condition_variable released;
		return released;
	}

	static std::map<std::string, Holder> &holders() {
		static std::map<std::string, Holder> held;
		return held;
	}

	std::string key_;
	std::optional<std::filesystem::path> record_;
	bool is_record_ours_ = false;
};

// `fault_injectable` marks a commit of a user's document (Singularity's own records
// pass false): such a document is never replaced while it is marked read-only
// or locked, and keeps its own metadata across the replacement.
inline void commit_staged_file(const std::filesystem::path &staged, const std::filesystem::path &destination,
		StagedCommitMode mode, bool fault_injectable = true) {
	if (fault_injectable && staged_commit_fault_for_testing().exchange(false))
		throw std::runtime_error("A simulated failure interrupted the commit.");
	// A user's document: its staged bytes reach the disk before its name moves
	// to them. A stage built by another writer - libzip's own commit of a Word
	// package - was never flushed, and a power loss after the rename could
	// leave the lawyer's document empty.
	if (fault_injectable) flush_file_to_disk(staged);
	if (fault_injectable && mode == StagedCommitMode::ReplaceExisting) {
		if (document_is_write_protected(destination))
			throw DocumentWriteConflict(DocumentWriteConflictKind::ReadOnly,
				document_write_protected_message(destination.filename().u8string()));
		carry_document_metadata(destination, staged);
	}
#if defined(_WIN32)
	const DWORD flags = MOVEFILE_WRITE_THROUGH | (mode == StagedCommitMode::ReplaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
	const DWORD failure = move_file_retrying_transient(staged, destination, flags);
	if (failure != ERROR_SUCCESS) {
		if (failure == ERROR_DISK_FULL || failure == ERROR_HANDLE_DISK_FULL) throw DiskFull();
		throw std::runtime_error(mode == StagedCommitMode::CreateExclusive && failure == ERROR_ALREADY_EXISTS
			? "A file already exists at the chosen destination." : "The file could not be committed atomically.");
	}
#else
	if (mode == StagedCommitMode::ReplaceExisting) {
		if (std::rename(staged.c_str(), destination.c_str()) != 0) {
			if (is_disk_full_error(errno)) throw DiskFull();
			throw std::runtime_error("The file could not be committed atomically: "
				+ std::error_code(errno, std::generic_category()).message());
		}
	} else {
#if defined(__APPLE__)
		if (::renamex_np(staged.c_str(), destination.c_str(), RENAME_EXCL) != 0) {
			if (is_disk_full_error(errno)) throw DiskFull();
			throw std::runtime_error(errno == EEXIST ? "A file already exists at the chosen destination."
				: "The file could not be committed atomically: " + std::error_code(errno, std::generic_category()).message());
		}
#else
		if (::link(staged.c_str(), destination.c_str()) != 0) {
			if (is_disk_full_error(errno)) throw DiskFull();
			throw std::runtime_error(errno == EEXIST ? "A file already exists at the chosen destination."
				: "The file could not be committed atomically: " + std::error_code(errno, std::generic_category()).message());
		}
		(void)::unlink(staged.c_str());
#endif
	}
	const int directory = ::open(destination.parent_path().c_str(), O_RDONLY);
	if (directory >= 0) {
		(void)::fsync(directory);
		::close(directory);
	}
#endif
}

// Removes a staged file and libzip's companion of it, ignoring absence.
inline void discard_staged_file(const std::filesystem::path &staged) noexcept {
	std::error_code ignored;
	std::filesystem::remove(staged, ignored);
	const std::filesystem::path directory = staged.parent_path();
	const std::string prefix = staged.filename().string() + ".";
	for (std::filesystem::directory_iterator entry(directory, ignored), end; !ignored && entry != end;
			entry.increment(ignored)) {
		const std::string name = entry->path().filename().string();
		if (name.rfind(prefix, 0) == 0 && name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0)
			std::filesystem::remove(entry->path(), ignored);
	}
}

} // namespace singularity::platform
