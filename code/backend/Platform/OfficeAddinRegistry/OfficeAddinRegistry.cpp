#include "Platform/OfficeAddinRegistry/OfficeAddinRegistry.hpp"

#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include "Platform/AppPaths/AppPaths.hpp"

#include <fstream>
#include <sstream>
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

bool register_manifest(const std::string &addin_id, const std::filesystem::path &manifest, bool) {
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
// "Powerpoint"), for each that is installed. Outlook has none: the person adds
// its manifest from a file.
struct Application {
	const char *container;
	const char *bundle;
};
constexpr Application kApplications[] = {
	{"com.microsoft.Word", "/Applications/Microsoft Word.app"},
	{"com.microsoft.Excel", "/Applications/Microsoft Excel.app"},
	{"com.microsoft.Powerpoint", "/Applications/Microsoft PowerPoint.app"},
};

std::filesystem::path wef_folder(const std::filesystem::path &home, const char *container) {
	return home / "Library" / "Containers" / container / "Data" / "Documents" / "wef";
}

std::filesystem::path placed(const std::filesystem::path &folder, const std::string &addin_id) {
	return folder / (addin_id + ".manifest.xml");
}

// What was placed, beside the manifest in Singularity's own records: the
// manifest's words as placed, and the applications they went to.
std::filesystem::path placement_record(const std::string &addin_id, const std::filesystem::path &manifest) {
	return manifest.parent_path() / (addin_id + ".placed");
}

std::string contents_of(const std::filesystem::path &file) {
	std::ifstream input(file, std::ios::binary);
	if (!input) return {};
	std::ostringstream text;
	text << input.rdbuf();
	return text.str();
}

// The applications the manifest's present words were placed in; none when
// the words changed since (a new version) or nothing was placed.
std::vector<std::string> placed_with(const std::string &addin_id, const std::filesystem::path &manifest) {
	const std::string record = contents_of(placement_record(addin_id, manifest));
	const std::string words = contents_of(manifest);
	const std::size_t split = record.find("\n--\n");
	if (words.empty() || split == std::string::npos || record.substr(split + 4) != words) return {};
	std::vector<std::string> applications;
	std::istringstream lines(record.substr(0, split));
	for (std::string line; std::getline(lines, line);) {
		if (!line.empty()) applications.push_back(line);
	}
	return applications;
}

void remember_placed(const std::string &addin_id, const std::filesystem::path &manifest, const std::vector<std::string> &applications) {
	const std::filesystem::path record = placement_record(addin_id, manifest);
	std::error_code error;
	if (applications.empty()) {
		std::filesystem::remove(record, error);
		return;
	}
	std::ostringstream text;
	for (const std::string &application : applications) text << application << "\n";
	text << "--\n" << contents_of(manifest);
	const std::filesystem::path staged = record.parent_path() / (record.filename().string() + ".staged");
	{
		std::ofstream output(staged, std::ios::binary | std::ios::trunc);
		if (!output) return;
		output << text.str();
		if (!output) return;
	}
	std::filesystem::rename(staged, record, error);
}

} // namespace

bool is_supported() {
	return true;
}

bool register_manifest(const std::string &addin_id, const std::filesystem::path &manifest, bool again) {
	if (!again && !placed_with(addin_id, manifest).empty()) return true;
	const auto home = home_directory();
	if (!home) return false;
	std::vector<std::string> applications;
	for (const Application &application : kApplications) {
		std::error_code error;
		// Only an application installed here, and opened once: its container
		// holds its documents' folder. Entering it is what macOS asks about.
		if (!std::filesystem::exists(application.bundle, error)) continue;
		const std::filesystem::path folder = wef_folder(*home, application.container);
		if (!std::filesystem::exists(folder.parent_path(), error)) continue;
		std::filesystem::create_directories(folder, error);
		if (error) continue;
		std::filesystem::copy_file(manifest, placed(folder, addin_id), std::filesystem::copy_options::overwrite_existing, error);
		if (!error) applications.push_back(application.container);
	}
	remember_placed(addin_id, manifest, applications);
	return !applications.empty();
}

bool is_registered(const std::string &addin_id, const std::filesystem::path &manifest) {
	return !placed_with(addin_id, manifest).empty();
}

bool unregister_manifest(const std::string &addin_id) {
	const auto home = home_directory();
	if (!home) return true;
	for (const Application &application : kApplications) {
		std::error_code error;
		if (!std::filesystem::exists(application.bundle, error)) continue;
		std::filesystem::remove(placed(wef_folder(*home, application.container), addin_id), error);
	}
	return true;
}

#else

bool is_supported() {
	return false;
}

bool register_manifest(const std::string &, const std::filesystem::path &, bool) {
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
