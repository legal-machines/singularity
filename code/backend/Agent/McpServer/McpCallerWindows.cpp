#include "Agent/McpServer/McpCaller.hpp"
#include "Agent/McpServer/McpCallerWindows.hpp"

#include <windows.h>
#include <appmodel.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>

#include <optional>
#include <string>
#include <vector>

namespace singularity::agents {
namespace {

std::string utf8_of(const std::wstring &text) {
	if (text.empty()) return {};
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) return {};
	std::string result(static_cast<std::size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
	return result;
}

// A kernel handle, closed when it goes out of scope.
class Handle {
public:
	explicit Handle(HANDLE value) : value_(value) {}
	~Handle() {
		if (is_open()) CloseHandle(value_);
	}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	bool is_open() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
	HANDLE get() const { return value_; }

private:
	HANDLE value_;
};

// When a process began, in 100-nanosecond units; 0 when Windows does not say.
ULONGLONG began_at(HANDLE process) {
	FILETIME created{};
	FILETIME exited{};
	FILETIME kernel{};
	FILETIME user{};
	if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
	return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// The process that started this one, from Windows' own list of processes.
DWORD parent_process_id() {
	const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	if (!snapshot.is_open()) return 0;
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	const DWORD self = GetCurrentProcessId();
	for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry))
		if (entry.th32ProcessID == self) return entry.th32ParentProcessID;
	return 0;
}

std::wstring image_path_of(HANDLE process) {
	std::wstring path(32768, L'\0');
	DWORD size = static_cast<DWORD>(path.size());
	if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
	path.resize(size);
	return path;
}

// A packaged app's family name, "Claude_pzs8sxrjxfjjc"; empty for any other
// program.
std::string package_family_of(HANDLE process) {
	UINT32 length = 0;
	if (GetPackageFamilyName(process, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0) return {};
	std::wstring name(length, L'\0');
	if (GetPackageFamilyName(process, &length, name.data()) != ERROR_SUCCESS) return {};
	name.resize(length > 0 ? length - 1 : 0);
	return utf8_of(name);
}

struct Signature {
	std::string signer;
	std::string subject;
};

// The signer of a file Windows verified, read from WinVerifyTrust's state:
// the leaf certificate's subject name and its whole subject.
Signature signer_of(HANDLE state) {
	Signature signature;
	CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData(state);
	CRYPT_PROVIDER_SGNR *signer = provider != nullptr ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
	CRYPT_PROVIDER_CERT *leaf = signer != nullptr ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
	if (leaf == nullptr || leaf->pCert == nullptr || leaf->pCert->pCertInfo == nullptr) return signature;
	wchar_t name[512] = {};
	if (CertGetNameStringW(leaf->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 512) > 1)
		signature.signer = utf8_of(name);
	CERT_NAME_BLOB *subject = &leaf->pCert->pCertInfo->Subject;
	const DWORD length = CertNameToStrW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, nullptr, 0);
	if (length > 1) {
		std::wstring text(length, L'\0');
		CertNameToStrW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, text.data(), length);
		text.resize(length - 1);
		signature.subject = utf8_of(text);
	}
	return signature;
}

// WinVerifyTrust over `data`, with the system's own policy: a chain to a root
// this computer trusts, a signature over the file's exact bytes, a
// certificate valid for signing code. Revocation is taken only as already
// cached, never asked of the network. The signer is read before the state is
// closed.
std::optional<Signature> verified(WINTRUST_DATA &data, LONG *status) {
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	HWND no_window = static_cast<HWND>(INVALID_HANDLE_VALUE);
	data.cbStruct = sizeof(data);
	data.dwUIChoice = WTD_UI_NONE;
	data.fdwRevocationChecks = WTD_REVOKE_NONE;
	data.dwStateAction = WTD_STATEACTION_VERIFY;
	data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
	const LONG result = WinVerifyTrust(no_window, &action, &data);
	if (status != nullptr) *status = result;
	std::optional<Signature> signature;
	if (result == ERROR_SUCCESS) signature = signer_of(data.hWVTStateData);
	data.dwStateAction = WTD_STATEACTION_CLOSE;
	WinVerifyTrust(no_window, &action, &data);
	return signature;
}

std::optional<Signature> embedded_signature(const std::wstring &path, LONG *status) {
	WINTRUST_FILE_INFO file{};
	file.cbStruct = sizeof(file);
	file.pcwszFilePath = path.c_str();
	WINTRUST_DATA data{};
	data.dwUnionChoice = WTD_CHOICE_FILE;
	data.pFile = &file;
	return verified(data, status);
}

// A function of a system library, by name. The catalog functions that take
// SHA-256 are declared in MinGW's mscat.h but missing from its import
// library, so both compilers find them this way. wintrust.dll is linked, so
// it is already loaded; otherwise it is loaded from System32 only.
template <typename Function>
Function system_function(const wchar_t *library, const char *name) {
	HMODULE module = GetModuleHandleW(library);
	if (module == nullptr) module = LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (module == nullptr) return nullptr;
	return reinterpret_cast<Function>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

using AcquireCatalogAdmin = BOOL(WINAPI *)(HCATADMIN *, const GUID *, PCWSTR, PCCERT_STRONG_SIGN_PARA, DWORD);
using HashFileForCatalog = BOOL(WINAPI *)(HCATADMIN, HANDLE, DWORD *, BYTE *, DWORD);

// Windows' own programs carry no signature in the file: Windows' catalogs list
// their SHA-256 hashes, and the catalog is what is signed.
std::optional<Signature> catalog_signature(const std::wstring &path) {
	const auto acquire = system_function<AcquireCatalogAdmin>(L"wintrust.dll", "CryptCATAdminAcquireContext2");
	const auto hash_file = system_function<HashFileForCatalog>(L"wintrust.dll", "CryptCATAdminCalcHashFromFileHandle2");
	if (acquire == nullptr || hash_file == nullptr) return std::nullopt;
	const Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!file.is_open()) return std::nullopt;
	HCATADMIN admin = nullptr;
	if (!acquire(&admin, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) return std::nullopt;
	std::optional<Signature> signature;
	DWORD size = 0;
	hash_file(admin, file.get(), &size, nullptr, 0);
	std::vector<BYTE> hash(size);
	if (size > 0 && hash_file(admin, file.get(), &size, hash.data(), 0)) {
		HCATINFO catalog = CryptCATAdminEnumCatalogFromHash(admin, hash.data(), size, 0, nullptr);
		if (catalog != nullptr) {
			CATALOG_INFO information{};
			information.cbStruct = sizeof(information);
			if (CryptCATCatalogInfoFromContext(catalog, &information, 0)) {
				// The catalog names its member by the hash, in upper-case hexadecimal.
				static const wchar_t *digits = L"0123456789ABCDEF";
				std::wstring tag;
				for (const BYTE byte : hash) {
					tag += digits[byte >> 4];
					tag += digits[byte & 0x0F];
				}
				WINTRUST_CATALOG_INFO member{};
				member.cbStruct = sizeof(member);
				member.pcwszCatalogFilePath = information.wszCatalogFile;
				member.pcwszMemberFilePath = path.c_str();
				member.pcwszMemberTag = tag.c_str();
				member.pbCalculatedFileHash = hash.data();
				member.cbCalculatedFileHash = size;
				member.hCatAdmin = admin;
				WINTRUST_DATA data{};
				data.dwUnionChoice = WTD_CHOICE_CATALOG;
				data.pCatalog = &member;
				signature = verified(data, nullptr);
			}
			CryptCATAdminReleaseCatalogContext(admin, catalog, 0);
		}
	}
	CryptCATAdminReleaseContext(admin, 0);
	return signature;
}

} // namespace

AiAppCaller identify_program(const std::filesystem::path &program, const std::string &package_family_name) {
	AiAppCaller caller;
	const std::wstring path = program.wstring();
	caller.path = utf8_of(path);
	LONG status = 0;
	std::optional<Signature> signature = embedded_signature(path, &status);
	// Only a file with no signature of its own is looked up in the catalogs:
	// one whose own signature fails is never rescued by them.
	if (signature.has_value() == false && status == TRUST_E_NOSIGNATURE) signature = catalog_signature(path);
	if (signature.has_value() == false || signature->signer.empty() || signature->subject.empty()) return caller;
	caller.signer = signature->signer;
	caller.team_id = signature->subject;
	if (package_family_name.empty() == false) {
		caller.signing_id = package_family_name;
	} else {
		std::wstring name = program.filename().wstring();
		CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
		caller.signing_id = utf8_of(name);
	}
	caller.is_signer_verified = caller.signing_id.empty() == false;
	return caller;
}

AiAppCaller identify_caller() {
	const DWORD parent = parent_process_id();
	if (parent == 0) return AiAppCaller{};
	const Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent));
	if (!process.is_open()) return AiAppCaller{};
	// A parent that has ended leaves its number to a later process: only one
	// that began before this helper is the program that started it.
	const ULONGLONG parentBegan = began_at(process.get());
	const ULONGLONG ownBegan = began_at(GetCurrentProcess());
	if (parentBegan == 0 || ownBegan == 0 || parentBegan > ownBegan) return AiAppCaller{};
	const std::wstring path = image_path_of(process.get());
	if (path.empty()) return AiAppCaller{};
	return identify_program(path, package_family_of(process.get()));
}

} // namespace singularity::agents
