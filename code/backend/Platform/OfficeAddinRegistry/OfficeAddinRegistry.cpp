#include "Platform/OfficeAddinRegistry/OfficeAddinRegistry.hpp"

#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include "Platform/AppPaths/AppPaths.hpp"
#endif

namespace singularity::platform::office_addins {

#if defined(_WIN32)

namespace {

constexpr const wchar_t *kDeveloperKey = L"Software\\Microsoft\\Office\\16.0\\WEF\\Developer";

std::wstring wide(const std::string &text) {
	return std::filesystem::u8path(text).wstring();
}

} // namespace

bool is_supported() {
	return true;
}

bool register_manifest(const std::string &addin_id, const std::filesystem::path &manifest) {
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kDeveloperKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
		return false;
	}
	const std::wstring name = wide(addin_id);
	const std::wstring value = manifest.wstring();
	const LONG written = RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
		static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
	RegCloseKey(key);
	return written == ERROR_SUCCESS;
}

bool is_registered(const std::string &addin_id, const std::filesystem::path &manifest) {
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kDeveloperKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
	const std::wstring name = wide(addin_id);
	DWORD type = 0;
	DWORD size = 0;
	bool registered = false;
	if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_SZ && size > 0) {
		std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, L'\0');
		if (RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<BYTE *>(value.data()), &size) == ERROR_SUCCESS) {
			registered = std::wstring(value.data()) == manifest.wstring();
		}
	}
	RegCloseKey(key);
	return registered;
}

bool unregister_manifest(const std::string &addin_id) {
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kDeveloperKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return true;
	const LONG removed = RegDeleteValueW(key, wide(addin_id).c_str());
	RegCloseKey(key);
	return removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND;
}

#elif defined(__APPLE__)

namespace {

// Word's, Excel's and PowerPoint's own folders for add-ins loaded from a
// manifest, by each application's container (PowerPoint's is spelt
// "Powerpoint"). Outlook has none: the person adds its manifest from a file.
std::vector<std::filesystem::path> wef_folders() {
	std::vector<std::filesystem::path> folders;
	const auto home = home_directory();
	if (!home) return folders;
	for (const char *application : {"com.microsoft.Word", "com.microsoft.Excel", "com.microsoft.Powerpoint"}) {
		folders.push_back(*home / "Library" / "Containers" / application / "Data" / "Documents" / "wef");
	}
	return folders;
}

std::filesystem::path placed(const std::filesystem::path &folder, const std::string &addin_id) {
	return folder / (addin_id + ".manifest.xml");
}

} // namespace

bool is_supported() {
	return true;
}

bool register_manifest(const std::string &addin_id, const std::filesystem::path &manifest) {
	bool any = false;
	for (const auto &folder : wef_folders()) {
		std::error_code error;
		// Only where that application is installed: its container exists.
		if (!std::filesystem::exists(folder.parent_path(), error)) continue;
		std::filesystem::create_directories(folder, error);
		std::filesystem::copy_file(manifest, placed(folder, addin_id), std::filesystem::copy_options::overwrite_existing, error);
		if (!error) any = true;
	}
	return any;
}

bool is_registered(const std::string &addin_id, const std::filesystem::path &) {
	for (const auto &folder : wef_folders()) {
		std::error_code error;
		if (std::filesystem::exists(placed(folder, addin_id), error)) return true;
	}
	return false;
}

bool unregister_manifest(const std::string &addin_id) {
	for (const auto &folder : wef_folders()) {
		std::error_code error;
		std::filesystem::remove(placed(folder, addin_id), error);
	}
	return true;
}

#else

bool is_supported() {
	return false;
}

bool register_manifest(const std::string &, const std::filesystem::path &) {
	return false;
}

bool is_registered(const std::string &, const std::filesystem::path &) {
	return false;
}

bool unregister_manifest(const std::string &) {
	return true;
}

#endif

} // namespace singularity::platform::office_addins
