#include "Platform/SystemPermissions/SystemPermissions.hpp"
#include "Platform/SystemPermissions/FirewallState.hpp"

#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/UdpSocket/UdpSocket.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <netfw.h>
#include <oleauto.h>
#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <thread>

// What Windows must allow Singularity, asked in onboarding and checked.
// - "firewall": Windows Defender Firewall's own alert, "Allow Singularity to
//   communicate on these networks", which comes up the first time Singularity's
//   program opens a port others may reach, so the firm's computers reach this
//   one directly. The alert's answer is a pair of rules for the program (TCP
//   and UDP), read here without asking, through the firewall's own COM
//   interface, which any user may read.
// - "controlledFolders": Windows' ransomware protection, where it is on, keeps
//   programs it does not know out of Documents, where Matters live, and says
//   so in its own notification. Its setting and the programs allowed are read
//   from the registry, never changed here.
// The certificate Word, Excel, PowerPoint and Outlook need is asked by the
// Office domain (Office/OfficeService, which PlatformBridge routes
// "officeCertificate" to).
namespace singularity::platform::system_permissions {
namespace {

// The firewall's classes, named here so no import library is needed for their
// ids (netfw.h: NetFwPolicy2, INetFwPolicy2, INetFwRule).
const CLSID kNetFwPolicy2 = {0xe2b3c97f, 0x6ae1, 0x41ac, {0x81, 0x7a, 0xf6, 0xf9, 0x21, 0x66, 0xd7, 0xdd}};
const IID kINetFwPolicy2 = {0x98325047, 0xc671, 0x4174, {0x8d, 0x81, 0xde, 0xfc, 0xd3, 0xf0, 0x31, 0x86}};
const IID kINetFwRule = {0xaf230d27, 0xbaba, 0x4e42, {0xac, 0xed, 0xf5, 0x24, 0xf2, 0x2c, 0xfc, 0xe2}};

// COM for this call, whatever the thread already had.
class ComScope {
public:
	ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
	~ComScope() {
		if (SUCCEEDED(result_)) CoUninitialize();
	}
	ComScope(const ComScope &) = delete;
	ComScope &operator=(const ComScope &) = delete;

private:
	HRESULT result_;
};

std::wstring lowered(std::wstring text) {
	if (text.empty() == false) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
	return text;
}

// A rule's program, its environment variables expanded (%ProgramFiles% and
// the like), as the firewall itself reads it.
std::wstring expanded(const wchar_t *text) {
	const DWORD size = ExpandEnvironmentStringsW(text, nullptr, 0);
	if (size == 0) return text;
	std::wstring result(size, L'\0');
	const DWORD written = ExpandEnvironmentStringsW(text, result.data(), size);
	if (written == 0 || written > size) return text;
	result.resize(written - 1);
	return result;
}

// Singularity's own program, which Windows names its rules by.
std::wstring own_program() {
	try {
		return lowered(executable_path().wstring());
	} catch (const std::exception &) {
		return {};
	}
}

// What the firewall says of Singularity's program now. Never asks.
FirewallRules read_firewall() {
	FirewallRules rules;
	const ComScope com;
	INetFwPolicy2 *policy = nullptr;
	if (FAILED(CoCreateInstance(kNetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, kINetFwPolicy2, reinterpret_cast<void **>(&policy)))
		|| policy == nullptr) {
		return rules;
	}
	long current = 0;
	if (SUCCEEDED(policy->get_CurrentProfileTypes(&current))) rules.current = current;
	const long here = rules.current != 0 ? rules.current : (kFirewallPrivate | kFirewallPublic);
	for (const NET_FW_PROFILE_TYPE2 profile : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
		if ((here & profile) == 0) continue;
		VARIANT_BOOL on = VARIANT_FALSE;
		if (SUCCEEDED(policy->get_FirewallEnabled(profile, &on)) && on == VARIANT_TRUE) rules.enabled = true;
	}
	const std::wstring program = own_program();
	INetFwRules *all = nullptr;
	IUnknown *listing = nullptr;
	IEnumVARIANT *each = nullptr;
	bool read = false;
	if (SUCCEEDED(policy->get_Rules(&all)) && all != nullptr && SUCCEEDED(all->get__NewEnum(&listing)) && listing != nullptr
		&& SUCCEEDED(listing->QueryInterface(IID_IEnumVARIANT, reinterpret_cast<void **>(&each))) && each != nullptr) {
		read = true;
		VARIANT item;
		VariantInit(&item);
		ULONG fetched = 0;
		while (each->Next(1, &item, &fetched) == S_OK && fetched == 1) {
			INetFwRule *rule = nullptr;
			if (item.vt == VT_DISPATCH && item.pdispVal != nullptr
				&& SUCCEEDED(item.pdispVal->QueryInterface(kINetFwRule, reinterpret_cast<void **>(&rule))) && rule != nullptr) {
				BSTR name = nullptr;
				if (program.empty() == false && SUCCEEDED(rule->get_ApplicationName(&name)) && name != nullptr
					&& lowered(expanded(name)) == program) {
					NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
					VARIANT_BOOL enabled = VARIANT_FALSE;
					NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
					long protocol = 0;
					long profiles = 0;
					if (SUCCEEDED(rule->get_Direction(&direction)) && direction == NET_FW_RULE_DIR_IN
						&& SUCCEEDED(rule->get_Enabled(&enabled)) && enabled == VARIANT_TRUE
						&& SUCCEEDED(rule->get_Protocol(&protocol)) && (protocol == NET_FW_IP_PROTOCOL_UDP || protocol == NET_FW_IP_PROTOCOL_ANY)
						&& SUCCEEDED(rule->get_Profiles(&profiles)) && SUCCEEDED(rule->get_Action(&action))) {
						(action == NET_FW_ACTION_BLOCK ? rules.blocked : rules.allowed) |= profiles;
					}
				}
				if (name != nullptr) SysFreeString(name);
				rule->Release();
			}
			VariantClear(&item);
		}
	}
	if (each != nullptr) each->Release();
	if (listing != nullptr) listing->Release();
	if (all != nullptr) all->Release();
	policy->Release();
	rules.readable = read;
	return rules;
}

// Windows asks the first time the program opens a port others may reach: one
// socket of the direct link's own kind (dual-stack UDP), on a port Windows
// picks - never the link's own - closed at once. Its answer is the rules it
// writes, read until they come, for a minute at most.
Permission ask_firewall() {
	const Permission before = firewall_permission(read_firewall());
	// Once Windows has rules for the program it asks no more; the person
	// changes them in its settings.
	if (before.state != State::unknown || before.problem.empty() == false) return before;
	try {
		UdpSocket opened(0);
	} catch (const std::exception &) {
		return Permission{"firewall", State::unknown, "Singularity could not open a network port on this computer, so Windows did not ask. Check again."};
	}
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		const Permission now = firewall_permission(read_firewall());
		if (now.state != State::unknown) return now;
	}
	return Permission{"firewall", State::unknown,
		"Windows did not answer. If its question is still open, answer it; otherwise open settings, find Singularity, and tick it for the network you are on."};
}

// Windows' ransomware protection (Controlled folder access): its setting, by
// policy first, and the programs it lets into the folders it guards.
constexpr const wchar_t *kControlledFolders =
	L"SOFTWARE\\Microsoft\\Windows Defender\\Windows Defender Exploit Guard\\Controlled Folder Access";
constexpr const wchar_t *kControlledFoldersPolicy =
	L"SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Windows Defender Exploit Guard\\Controlled Folder Access";
constexpr const char *kControlledFoldersKey = "controlledFolders";
constexpr const char *kControlledFoldersRefused =
	"Windows' ransomware protection keeps Singularity out of your Documents folder, where your Matters live. Open settings, "
	"choose Allow an app through Controlled folder access, and add Singularity.";

std::optional<DWORD> machine_dword(const wchar_t *key, const wchar_t *name) {
	DWORD value = 0;
	DWORD size = sizeof(value);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return std::nullopt;
	return value;
}

// It keeps programs out only when set to block (1); audit (2), disk sectors
// only (3, 4) and off (0) let files be written.
bool controlled_folders_block() {
	std::optional<DWORD> setting = machine_dword(kControlledFoldersPolicy, L"EnableControlledFolderAccess");
	if (setting.has_value() == false) setting = machine_dword(kControlledFolders, L"EnableControlledFolderAccess");
	return setting == std::optional<DWORD>(1);
}

// Whether the program is among those the person or the firm's IT allowed
// (value names are the programs' paths).
bool allowed_into_controlled_folders(const wchar_t *key, const std::wstring &program) {
	HKEY allowed = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (std::wstring(key) + L"\\AllowedApplications").c_str(), 0, KEY_QUERY_VALUE, &allowed)
		!= ERROR_SUCCESS) {
		return false;
	}
	bool found = false;
	for (DWORD index = 0; found == false; ++index) {
		wchar_t name[2 * MAX_PATH] = {};
		DWORD length = static_cast<DWORD>(std::size(name));
		const LONG read = RegEnumValueW(allowed, index, name, &length, nullptr, nullptr, nullptr, nullptr);
		if (read == ERROR_NO_MORE_ITEMS) break;
		if (read == ERROR_SUCCESS) found = lowered(expanded(name)) == program;
	}
	RegCloseKey(allowed);
	return found;
}

// Not needed where it is off, as on most computers; allowed when Singularity is
// listed; otherwise only a write can tell, as Windows also lets in programs it
// knows itself.
Permission controlled_folders_permission() {
	Permission permission{kControlledFoldersKey, State::not_needed, {}};
	if (controlled_folders_block() == false) return permission;
	const std::wstring program = own_program();
	const bool listed = program.empty() == false
		&& (allowed_into_controlled_folders(kControlledFolders, program) || allowed_into_controlled_folders(kControlledFoldersPolicy, program));
	permission.state = listed ? State::allowed : State::unknown;
	return permission;
}

// Asking is writing: a small hidden file in Documents, gone when it is closed.
// Refused, Windows says so in its own notification, where the person can
// allow Singularity.
Permission ask_controlled_folders() {
	Permission permission = controlled_folders_permission();
	if (permission.state != State::unknown) return permission;
	wchar_t documents[MAX_PATH] = {};
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents))) {
		permission.problem = "Windows did not say where your Documents folder is. Check again.";
		return permission;
	}
	const std::wstring mark = std::wstring(documents) + L"\\.singularity-check-" + std::to_wstring(GetCurrentProcessId());
	const HANDLE file = CreateFileW(mark.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		if (GetLastError() == ERROR_ACCESS_DENIED) {
			permission.state = State::denied;
			permission.problem = kControlledFoldersRefused;
		} else {
			permission.problem = "Singularity could not write in your Documents folder. Check again.";
		}
		return permission;
	}
	DWORD written = 0;
	const BOOL wrote = WriteFile(file, "Singularity", 11, &written, nullptr);
	CloseHandle(file);
	if (wrote == TRUE) {
		permission.state = State::allowed;
	} else {
		permission.state = State::denied;
		permission.problem = kControlledFoldersRefused;
	}
	return permission;
}

bool open(const wchar_t *program, const wchar_t *arguments) {
	const HINSTANCE opened = ShellExecuteW(nullptr, L"open", program, arguments, nullptr, SW_SHOWNORMAL);
	return reinterpret_cast<INT_PTR>(opened) > 32;
}

} // namespace

std::vector<Permission> list() {
	return {controlled_folders_permission(), firewall_permission(read_firewall())};
}

Permission ask(const std::string &key) {
	if (key == kControlledFoldersKey) return ask_controlled_folders();
	if (key == "firewall") return ask_firewall();
	return Permission{key, State::not_needed, {}};
}

bool open_settings(const std::string &key) {
	// Windows Security's Ransomware protection, where Controlled folder access
	// lists the programs it lets in.
	if (key == kControlledFoldersKey) return open(L"windowsdefender://ransomwareprotection", nullptr);
	// "Allow an app through Windows Defender Firewall".
	if (key == "firewall") return open(L"control.exe", L"/name Microsoft.WindowsFirewall /page pageConfigureApps");
	return false;
}

} // namespace singularity::platform::system_permissions
