#include "Platform/SeatPasswordStore/SeatPasswordStore.hpp"

#include <windows.h>
#include <wincred.h>

#include <string>

namespace singularity::platform::seat_password_store {
namespace {

// Only Singularity's own entries: the prefix and something after it.
bool is_ours(const std::string &target) {
	const std::string prefix(kTargetPrefix);
	return target.size() > prefix.size() && target.compare(0, prefix.size(), prefix) == 0;
}

std::wstring wide(const std::string &text) {
	if (text.empty()) return {};
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring result(static_cast<std::size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
	return result;
}

// Who the entry is for, as Credential Manager lists it.
wchar_t kComment[] = L"Singularity: the password of a Legal Machines seat, kept at the member's choice";

} // namespace

std::optional<std::string> kind() {
	return std::string("windows");
}

bool save(const std::string &target, const std::string &user, const std::string &password) {
	if (is_ours(target) == false || password.empty() || password.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
	std::wstring name = wide(target);
	std::wstring account = wide(user);
	if (name.empty() || name.size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH || account.size() > CRED_MAX_USERNAME_LENGTH) return false;
	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = name.data();
	credential.UserName = account.empty() ? nullptr : account.data();
	credential.Comment = kComment;
	credential.CredentialBlobSize = static_cast<DWORD>(password.size());
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(password.data()));
	// On this computer only, never roaming with the Windows profile.
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	return CredWriteW(&credential, 0) == TRUE;
}

std::optional<std::string> read(const std::string &target) {
	if (is_ours(target) == false) return std::nullopt;
	const std::wstring name = wide(target);
	if (name.empty()) return std::nullopt;
	PCREDENTIALW found = nullptr;
	if (CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &found) != TRUE || found == nullptr) return std::nullopt;
	std::string password;
	if (found->CredentialBlob != nullptr && found->CredentialBlobSize > 0) {
		password.assign(reinterpret_cast<const char *>(found->CredentialBlob), found->CredentialBlobSize);
		// Windows' copy is wiped before it is given back.
		SecureZeroMemory(found->CredentialBlob, found->CredentialBlobSize);
	}
	CredFree(found);
	if (password.empty()) return std::nullopt;
	return password;
}

bool forget(const std::string &target) {
	if (is_ours(target) == false) return false;
	const std::wstring name = wide(target);
	return name.empty() == false && CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0) == TRUE;
}

} // namespace singularity::platform::seat_password_store
