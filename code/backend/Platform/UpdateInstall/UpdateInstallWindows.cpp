// Singularity's own updates on Windows (UpdateInstall.hpp): the installer
// runs once Singularity is gone, through its updater, singularity-update.exe
// (Platform/UpdateHelper), which Singularity copies out of its install folder
// first: Windows replaces no program while it runs, and the installer empties
// that folder and fills it again. The updater checks the installer, runs it
// silently with the rights an install for every user needs - Windows asks the
// person once - and starts Singularity again as the person, never elevated.
// It leaves how it went in the Updates folder (record_outcome), which
// take_outcome reads at the next start.

#include "Platform/UpdateInstall/UpdateInstall.hpp"

#include "Platform/Sha256/Sha256.hpp"
#include "Platform/UpdateHelper/UpdateHelper.hpp"

#include <windows.h>

#include <string>
#include <system_error>

namespace singularity::platform::update_install {
namespace {

namespace helper = update_helper;

constexpr const wchar_t *kHelperName = L"singularity-update.exe";
// Each hand-over's folder under the user's temporary folder; tidy() knows
// them by this beginning alone.
constexpr const wchar_t *kFolderPrefix = L"Singularity-update-";

std::filesystem::path running_program() {
	std::wstring buffer(32768, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0 || length >= buffer.size()) return {};
	buffer.resize(length);
	return std::filesystem::path(buffer);
}

std::filesystem::path temporary_folder() {
	std::wstring buffer(MAX_PATH + 2, L'\0');
	const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
	if (length == 0 || length >= buffer.size()) return {};
	buffer.resize(length);
	return std::filesystem::path(buffer);
}

// The request the updater is given, for this process and `update`; its
// outcome is left beside the installer, in the Updates folder.
helper::Request request_for(const Update &update, const std::filesystem::path &program, bool reopen) {
	helper::Request request;
	request.process_id = GetCurrentProcessId();
	request.installer = update.installer.wstring();
	request.sha256 = update.sha256;
	request.version = update.version;
	request.application = program.wstring();
	request.relaunch = reopen;
	request.folder = update.installer.parent_path().wstring();
	return request;
}

// The uninstaller Windows has for Singularity: the UninstallString its
// installer leaves under the key CPACK_PACKAGE_INSTALL_REGISTRY_KEY names
// (deploy/backend/CMakeLists.txt), which the next installer runs first. The
// installer is a 32-bit program, so Windows keeps that key in its 32-bit
// view; the 64-bit one is read after it.
std::filesystem::path registered_uninstaller() {
	constexpr const wchar_t *kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\singularity";
	const REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
	for (const REGSAM view : views) {
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) continue;
		std::wstring value(32768, L'\0');
		DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
		const LSTATUS read = RegGetValueW(key, nullptr, L"UninstallString", RRF_RT_REG_SZ, nullptr, value.data(), &bytes);
		RegCloseKey(key);
		if (read != ERROR_SUCCESS || bytes < 2 * sizeof(wchar_t)) continue;
		value.resize(bytes / sizeof(wchar_t) - 1);
		// Written as a command: the path in quotes.
		if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
		if (!value.empty()) return std::filesystem::path(value);
	}
	return {};
}

std::string by_hand(const std::string &why) {
	return why + ", so this update is installed by hand.";
}

} // namespace

std::string prepare(const Update &update) {
	try {
		const auto program = running_program();
		if (program.empty()) return by_hand("Singularity could not tell where it is installed");
		// Only the Singularity Windows has installed is one its installer
		// replaces: the next installer removes the copy beside its uninstaller
		// and installs into the folder this one runs from (UpdateHelper, /D=),
		// so a copy run from anywhere else is installed by hand.
		std::error_code error;
		const auto uninstaller = registered_uninstaller();
		if (uninstaller.empty() || !std::filesystem::is_regular_file(uninstaller, error))
			return by_hand("Singularity was not installed by its installer on this computer");
		if (!std::filesystem::equivalent(uninstaller.parent_path(), program.parent_path(), error))
			return by_hand("This copy of Singularity is not the one installed on this computer");
		if (!std::filesystem::is_regular_file(program.parent_path() / kHelperName, error))
			return by_hand("Singularity's updater is missing from its folder");
		if (!std::filesystem::is_regular_file(update.installer, error)) return by_hand("The downloaded installer is gone");
		// What the updater would refuse, refused here, while the member can
		// still be told.
		if (!helper::request_of(helper::arguments_of(request_for(update, program, true))))
			return by_hand("Its installer was downloaded where the updater cannot take it");
		if (!helper::same_sha256(update.sha256, platform::sha256_hex_digest_of_file(update.installer)))
			return by_hand("The downloaded update changed on disk");
		return {};
	} catch (...) {
		return by_hand("The update could not be readied");
	}
}

bool hand_over(const Update &update, bool reopen) {
	try {
		const auto program = running_program();
		const auto temporary = temporary_folder();
		if (program.empty() || temporary.empty()) return false;
		const auto arguments = helper::arguments_of(request_for(update, program, reopen));
		if (!helper::request_of(arguments)) return false;

		// A fresh folder of the updater's own, outside the install folder.
		const auto folder = temporary / (std::wstring(kFolderPrefix) + std::to_wstring(GetCurrentProcessId()) + L"-"
			+ std::to_wstring(GetTickCount64()));
		std::error_code error;
		if (!std::filesystem::create_directories(folder, error) || error) return false;
		const auto copy = folder / kHelperName;
		if (!CopyFileW((program.parent_path() / kHelperName).c_str(), copy.c_str(), TRUE)) return false;

		// Detached, so it outlives this process; out of this process's job when
		// the job allows it, since a job that ends with Singularity would end
		// the updater too.
		std::wstring line = helper::command_line_of(copy.wstring(), arguments);
		STARTUPINFOW startup{};
		startup.cb = sizeof startup;
		PROCESS_INFORMATION process{};
		const DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
		BOOL started = CreateProcessW(copy.c_str(), line.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB,
			nullptr, folder.c_str(), &startup, &process);
		if (!started && GetLastError() == ERROR_ACCESS_DENIED)
			started = CreateProcessW(copy.c_str(), line.data(), nullptr, nullptr, FALSE, flags, nullptr, folder.c_str(),
				&startup, &process);
		if (!started) return false;
		// Singularity has the person's last click: the updater's window may come
		// to the front, and with it Windows' permission prompt.
		AllowSetForegroundWindow(process.dwProcessId);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return true;
	} catch (...) {
		return false;
	}
}

void tidy(const std::filesystem::path &) {
	// What a hand-over left: its updater's folder under the user's temporary
	// folder. The Updates folder itself is Node's.
	try {
		const auto temporary = temporary_folder();
		if (temporary.empty()) return;
		std::error_code error;
		for (std::filesystem::directory_iterator each(temporary, error), end; !error && each != end; each.increment(error)) {
			const auto name = each->path().filename().wstring();
			std::error_code kind;
			if (name.rfind(kFolderPrefix, 0) != 0 || !each->is_directory(kind)) continue;
			// A folder whose updater still runs stays, held by it, until a later start.
			std::error_code ignored;
			std::filesystem::remove_all(each->path(), ignored);
		}
	} catch (...) {
	}
}

} // namespace singularity::platform::update_install
