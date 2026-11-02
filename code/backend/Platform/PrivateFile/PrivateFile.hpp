#pragma once

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>

#include <aclapi.h>

#include <memory>
#include <vector>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace singularity::platform {

// A file or folder this computer keeps for its user alone - its signing key,
// what it recorded about an operation's authority, about trusted devices,
// about where a message came from. On macOS and Linux that is the owner's
// permission bits (0700 for a folder, 0600 for a file). On Windows those bits
// do not exist, and std::filesystem::permissions only sets the read-only
// attribute, so the same promise is an access control list that lets in only
// the user this process runs as, SYSTEM and the Administrators group - what a
// user's own profile folder allows, and what OpenSSH asks of a private key.
// There a restricted folder also passes its list on to what is made inside
// it; on macOS and Linux each file gets its own bits.

#if defined(_WIN32)
namespace private_file_detail {

// The SIDs a private file lets in, in this process.
class TrustedSids {
public:
	TrustedSids() {
		HANDLE token = nullptr;
		if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
			DWORD size = 0;
			GetTokenInformation(token, TokenUser, nullptr, 0, &size);
			user_.resize(size);
			if (size == 0 || !GetTokenInformation(token, TokenUser, user_.data(), size, &size)) user_.clear();
			CloseHandle(token);
		}
		DWORD size = static_cast<DWORD>(sizeof(system_));
		if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_, &size)) system_[0] = 0;
		size = static_cast<DWORD>(sizeof(administrators_));
		if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators_, &size)) administrators_[0] = 0;
	}

	bool complete() const { return !user_.empty() && IsValidSid(system()) && IsValidSid(administrators()); }
	PSID user() const { return reinterpret_cast<const TOKEN_USER *>(user_.data())->User.Sid; }
	PSID system() const { return const_cast<BYTE *>(system_); }
	PSID administrators() const { return const_cast<BYTE *>(administrators_); }

	bool trusts(PSID sid) const {
		return EqualSid(sid, user()) || EqualSid(sid, system()) || EqualSid(sid, administrators());
	}

private:
	std::vector<BYTE> user_;
	BYTE system_[SECURITY_MAX_SID_SIZE] = {};
	BYTE administrators_[SECURITY_MAX_SID_SIZE] = {};
};

struct LocalFreeDeleter {
	void operator()(void *memory) const { LocalFree(memory); }
};

// The access list of a private file or folder: the trusted accounts above,
// passed on to everything a folder holds. Empty when it cannot be built.
inline std::unique_ptr<ACL, LocalFreeDeleter> private_list(bool folder) {
	const TrustedSids sids;
	if (!sids.complete()) return nullptr;
	const DWORD inheritance = folder ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
	EXPLICIT_ACCESS_W entries[3] = {};
	const PSID trusted[3] = {sids.user(), sids.system(), sids.administrators()};
	for (int index = 0; index < 3; ++index) {
		entries[index].grfAccessPermissions = FILE_ALL_ACCESS;
		entries[index].grfAccessMode = SET_ACCESS;
		entries[index].grfInheritance = inheritance;
		entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
		entries[index].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
		entries[index].Trustee.ptstrName = static_cast<LPWSTR>(trusted[index]);
	}
	PACL list = nullptr;
	if (SetEntriesInAclW(3, entries, nullptr, &list) != ERROR_SUCCESS) return nullptr;
	return std::unique_ptr<ACL, LocalFreeDeleter>(list);
}

} // namespace private_file_detail

// The private access list (as above) for a file or folder being made:
// CreateFileW and CreateDirectoryW take attributes(), so the file is private
// from its first moment, and apply_to() makes an already open one private
// too (CREATE_ALWAYS keeps an existing file's own list). The list is
// protected: nothing the folder above grants is inherited.
class PrivateSecurity final {
public:
	explicit PrivateSecurity(bool folder) : list_(private_file_detail::private_list(folder)) {
		if (!list_ || !InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
			!SetSecurityDescriptorDacl(&descriptor_, TRUE, list_.get(), FALSE) ||
			!SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
			list_.reset();
			return;
		}
		attributes_.nLength = sizeof(attributes_);
		attributes_.lpSecurityDescriptor = &descriptor_;
	}
	PrivateSecurity(const PrivateSecurity &) = delete;
	PrivateSecurity &operator=(const PrivateSecurity &) = delete;

	// False when the list could not be built; nothing may then be made private.
	bool valid() const { return list_ != nullptr; }
	SECURITY_ATTRIBUTES *attributes(bool inheritable_handle = false) {
		attributes_.bInheritHandle = inheritable_handle ? TRUE : FALSE;
		return &attributes_;
	}
	// For a handle opened with WRITE_DAC.
	bool apply_to(HANDLE handle) const {
		return valid() && SetSecurityInfo(handle, SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, list_.get(),
			nullptr) == ERROR_SUCCESS;
	}

private:
	std::unique_ptr<ACL, private_file_detail::LocalFreeDeleter> list_;
	SECURITY_DESCRIPTOR descriptor_{};
	SECURITY_ATTRIBUTES attributes_{};
};
#endif

// Lets only this computer's user (as above) at an existing file or folder.
// False when it could not be done - a file system that keeps no such
// permissions, an account that may not change them - and the caller decides
// what that means; nothing else about the file changes.
inline bool restrict_to_owner(const std::filesystem::path &path) {
	std::error_code error;
	const bool folder = std::filesystem::is_directory(path, error);
	if (error) return false;
#if defined(_WIN32)
	const auto list = private_file_detail::private_list(folder);
	if (!list) return false;
	std::wstring name = path.wstring();
	// Protected: nothing the folder above grants is inherited any more.
	return SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, list.get(), nullptr)
		== ERROR_SUCCESS;
#else
	std::filesystem::permissions(path,
		folder ? std::filesystem::perms::owner_all
			   : std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
		std::filesystem::perm_options::replace, error);
	return !error;
#endif
}

// Whether no one but this computer's user (as above) may open the file or
// folder: what a test asks, and what may be checked before trusting a file.
// False as well when it cannot be told.
inline bool is_restricted_to_owner(const std::filesystem::path &path) {
#if defined(_WIN32)
	const private_file_detail::TrustedSids sids;
	if (!sids.complete()) return false;
	PACL list = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	std::wstring name = path.wstring();
	if (GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &list,
			nullptr, &descriptor) != ERROR_SUCCESS)
		return false;
	const std::unique_ptr<void, private_file_detail::LocalFreeDeleter> owned(descriptor);
	// No list at all lets everyone in.
	if (list == nullptr) return false;
	for (DWORD index = 0; index < list->AceCount; ++index) {
		void *entry = nullptr;
		if (!GetAce(list, index, &entry)) return false;
		const auto *header = static_cast<const ACE_HEADER *>(entry);
		// An entry only for what is made inside the folder grants nothing here.
		if ((header->AceFlags & INHERIT_ONLY_ACE) != 0 || header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
		const auto *allowed = static_cast<const ACCESS_ALLOWED_ACE *>(entry);
		if (!sids.trusts(const_cast<DWORD *>(&allowed->SidStart))) return false;
	}
	return true;
#else
	struct stat status {};
	return ::stat(path.c_str(), &status) == 0 && (status.st_mode & 0077) == 0;
#endif
}

// Creates `directory` (and any missing parents, which keep ordinary
// permissions) private to this computer's user from its first moment; an
// existing folder is made private. False - never a throw - when that could
// not be done, so a caller fails closed instead of going on with a folder
// another user could read (local inference's per-launch credential lives in
// one).
inline bool create_private_directory(const std::filesystem::path &directory) {
	std::error_code error;
#if defined(_WIN32)
	PrivateSecurity security(/*folder=*/true);
	if (!security.valid()) return false;
	if (directory.has_parent_path()) std::filesystem::create_directories(directory.parent_path(), error);
	if (error) return false;
	if (CreateDirectoryW(directory.c_str(), security.attributes()) != FALSE) return true;
	if (GetLastError() != ERROR_ALREADY_EXISTS || !std::filesystem::is_directory(directory, error)) return false;
	return restrict_to_owner(directory);
#else
	std::filesystem::create_directories(directory, error);
	if (error) return false;
	std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
		std::filesystem::perm_options::replace, error);
	return !error;
#endif
}

// Creates or replaces `file`, private to this computer's user, and writes
// `contents` whole. False - never a throw - when any of it failed.
inline bool write_private_file(const std::filesystem::path &file, std::string_view contents) {
#if defined(_WIN32)
	PrivateSecurity security(/*folder=*/false);
	if (!security.valid()) return false;
	const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE | WRITE_DAC, 0, security.attributes(),
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return false;
	// CREATE_ALWAYS keeps the list of a file that already existed.
	bool complete = security.apply_to(handle);
	std::size_t written = 0;
	while (complete && written < contents.size()) {
		const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(contents.size() - written, 1U << 30));
		DWORD count = 0;
		if (!WriteFile(handle, contents.data() + written, chunk, &count, nullptr) || count == 0) complete = false;
		else written += count;
	}
	return CloseHandle(handle) != FALSE && complete;
#else
	const int descriptor = ::open(file.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
	if (descriptor < 0) return false;
	// O_CREAT's mode applies only to a new file; an existing one is tightened.
	bool complete = ::fchmod(descriptor, 0600) == 0;
	std::size_t written = 0;
	while (complete && written < contents.size()) {
		const ssize_t count = ::write(descriptor, contents.data() + written, contents.size() - written);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) complete = false;
		else written += static_cast<std::size_t>(count);
	}
	return ::close(descriptor) == 0 && complete;
#endif
}

} // namespace singularity::platform
