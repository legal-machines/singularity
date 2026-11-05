// Compiled with ARC (deploy/backend/CMakeLists.txt).
#include "Platform/SystemPermissions/SystemPermissions.hpp"

#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"

#import <AppKit/AppKit.h>
#import <Network/Network.h>
#import <Security/Security.h>
#include <dns_sd.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <vector>

namespace singularity::platform::system_permissions {
namespace {

// What was learned in this run.
std::mutex &known_mutex() {
	static std::mutex *kept = new std::mutex();
	return *kept;
}

std::map<std::string, Permission> &known() {
	static auto *kept = new std::map<std::string, Permission>();
	return *kept;
}

// What macOS allowed this application, kept in the app's own folder, so a
// permission already given is never asked again (the owner, 2026-11-05: "if
// some permission is already given, there is no need to ask about it at
// all"). macOS can't be asked silently whether it would allow; it keeps each
// answer for the application's code identity. Signed by a lasting identity
// (deploy/backend/CMakeLists.txt, "Singularity Dev"), that is the code's
// designated requirement, the same across builds and updates; signed ad hoc,
// each build is new to macOS, and is named by its executable's size and time.
std::string signing_requirement() {
	std::string text;
	SecCodeRef code = nullptr;
	if (SecCodeCopySelf(kSecCSDefaultFlags, &code) != errSecSuccess || code == nullptr) return text;
	SecStaticCodeRef staticCode = nullptr;
	if (SecCodeCopyStaticCode(code, kSecCSDefaultFlags, &staticCode) == errSecSuccess && staticCode != nullptr) {
		CFDictionaryRef information = nullptr;
		if (SecCodeCopySigningInformation(staticCode, kSecCSSigningInformation, &information) == errSecSuccess && information != nullptr) {
			uint32_t flags = 0;
			const auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(information, kSecCodeInfoFlags));
			if (number != nullptr) CFNumberGetValue(number, kCFNumberSInt32Type, &flags);
			SecRequirementRef requirement = nullptr;
			if ((flags & kSecCodeSignatureAdhoc) == 0
					&& SecCodeCopyDesignatedRequirement(staticCode, kSecCSDefaultFlags, &requirement) == errSecSuccess
					&& requirement != nullptr) {
				CFStringRef written = nullptr;
				if (SecRequirementCopyString(requirement, kSecCSDefaultFlags, &written) == errSecSuccess && written != nullptr) {
					const char *characters = [(__bridge NSString *)written UTF8String];
					if (characters != nullptr) text = characters;
					CFRelease(written);
				}
				CFRelease(requirement);
			}
			CFRelease(information);
		}
		CFRelease(staticCode);
	}
	CFRelease(code);
	return text;
}

std::string build_identity() {
	if (const std::string requirement = signing_requirement(); !requirement.empty()) return "signed " + requirement;
	std::error_code error;
	const std::filesystem::path executable = executable_path();
	const auto size = std::filesystem::file_size(executable, error);
	if (error) return {};
	const auto written = std::filesystem::last_write_time(executable, error);
	if (error) return {};
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(written.time_since_epoch()).count();
	return std::to_string(size) + "-" + std::to_string(static_cast<long long>(seconds));
}

std::filesystem::path allowed_record() {
	return application_data_directory() / "system-permissions.json";
}

// The keys allowed for this build, and for Office the applications it was
// allowed into.
struct Allowed {
	std::set<std::string> keys;
	std::set<std::string> office;
};

Allowed kept_allowed(const std::string &build) {
	Allowed allowed;
	if (build.empty()) return allowed;
	std::ifstream input(allowed_record(), std::ios::binary);
	if (!input) return allowed;
	const nlohmann::json record = nlohmann::json::parse(input, nullptr, false);
	if (!record.is_object() || record.value("build", std::string()) != build) return allowed;
	const auto keys = record.find("allowed");
	if (keys != record.end() && keys->is_array()) {
		for (const auto &key : *keys) if (key.is_string()) allowed.keys.insert(key.get<std::string>());
	}
	const auto office = record.find("office");
	if (office != record.end() && office->is_array()) {
		for (const auto &name : *office) if (name.is_string()) allowed.office.insert(name.get<std::string>());
	}
	return allowed;
}

void keep_allowed(const Allowed &allowed, const std::string &build) {
	if (build.empty()) return;
	const nlohmann::json record{{"formatVersion", 1}, {"build", build},
		{"allowed", std::vector<std::string>(allowed.keys.begin(), allowed.keys.end())},
		{"office", std::vector<std::string>(allowed.office.begin(), allowed.office.end())}};
	try {
		const std::filesystem::path target = allowed_record();
		ensure_record_directory(target.parent_path());
		const std::filesystem::path staged = staged_sibling_path(target);
		write_file_durably(staged, record.dump());
		commit_staged_file(staged, target, StagedCommitMode::ReplaceExisting, false);
	} catch (const std::exception &) {
		// Not kept: the next onboarding asks again, which macOS answers at once.
	}
}

std::vector<std::string> installed_office();

Permission remember(Permission permission) {
	const std::lock_guard<std::mutex> lock(known_mutex());
	known()[permission.key] = permission;
	const std::string build = build_identity();
	Allowed allowed = kept_allowed(build);
	if (permission.state == State::allowed && permission.problem.empty()) {
		allowed.keys.insert(permission.key);
		if (permission.key == "officeApps") {
			for (const std::string &name : installed_office()) allowed.office.insert(name);
		}
	} else if (permission.state == State::denied) {
		allowed.keys.erase(permission.key);
	}
	keep_allowed(allowed, build);
	return permission;
}

bool is_refusal(const std::error_code &error) {
	return error == std::errc::operation_not_permitted || error == std::errc::permission_denied;
}

// Word, Excel and PowerPoint installed here, by their bundles: reading
// /Applications asks nothing.
struct OfficeApplication {
	const char *name;
	const char *bundle;
	const char *container;
};
constexpr OfficeApplication kOffice[] = {
	{"Word", "/Applications/Microsoft Word.app", "com.microsoft.Word"},
	{"Excel", "/Applications/Microsoft Excel.app", "com.microsoft.Excel"},
	{"PowerPoint", "/Applications/Microsoft PowerPoint.app", "com.microsoft.Powerpoint"},
};

bool has_office() {
	for (const OfficeApplication &application : kOffice) {
		std::error_code error;
		if (std::filesystem::exists(application.bundle, error)) return true;
	}
	return false;
}

std::vector<std::string> installed_office() {
	std::vector<std::string> names;
	for (const OfficeApplication &application : kOffice) {
		std::error_code error;
		if (std::filesystem::exists(application.bundle, error)) names.push_back(application.name);
	}
	return names;
}

// The Documents folder, where Matters live: reading it is what macOS asks
// about, and it waits for the person's answer.
Permission ask_documents() {
	Permission permission{"documents", State::unknown, {}};
	const auto home = home_directory();
	if (!home) {
		permission.problem = "This Mac's home folder could not be found.";
		return permission;
	}
	std::error_code error;
	std::filesystem::directory_iterator listing(*home / "Documents", error);
	if (!error) {
		permission.state = State::allowed;
	} else if (is_refusal(error)) {
		permission.state = State::denied;
		permission.problem = "Allow Singularity in Privacy & Security, under Files & Folders, then check again.";
	} else {
		permission.problem = "The Documents folder could not be read.";
	}
	return permission;
}

// Word's, Excel's and PowerPoint's own folders, where the add-in goes. Each
// is another application's data, which macOS guards with its question "access
// data from other apps", once for each application. Each is entered and a
// mark written and taken away; nothing of the add-in is placed yet.
Permission ask_office() {
	Permission permission{"officeApps", State::unknown, {}};
	if (!has_office()) {
		permission.state = State::not_needed;
		return permission;
	}
	const auto home = home_directory();
	if (!home) {
		permission.problem = "This Mac's home folder could not be found.";
		return permission;
	}
	int allowed = 0;
	std::string refused;
	std::string unopened;
	for (const OfficeApplication &application : kOffice) {
		std::error_code error;
		if (!std::filesystem::exists(application.bundle, error)) continue;
		const std::filesystem::path documents = *home / "Library" / "Containers" / application.container / "Data" / "Documents";
		const bool exists = std::filesystem::exists(documents, error);
		if (is_refusal(error)) {
			refused = refused.empty() ? application.name : refused + ", " + application.name;
			continue;
		}
		if (!exists) {
			// Never opened here: its own folders come at its first start.
			unopened = unopened.empty() ? application.name : unopened + ", " + application.name;
			continue;
		}
		const std::filesystem::path folder = documents / "wef";
		std::filesystem::create_directories(folder, error);
		const std::filesystem::path mark = folder / ".singularity-check";
		if (!error) {
			std::ofstream(mark, std::ios::binary | std::ios::trunc) << "Singularity";
			std::filesystem::remove(mark, error);
		}
		if (error) {
			refused = refused.empty() ? application.name : refused + ", " + application.name;
			continue;
		}
		++allowed;
	}
	if (!refused.empty()) {
		permission.state = State::denied;
		permission.problem = "macOS did not let Singularity into " + refused
			+ ". Check again and choose Allow, or turn Singularity on in Full Disk Access.";
	} else if (!unopened.empty()) {
		permission.state = allowed > 0 ? State::allowed : State::unknown;
		permission.problem = "Open " + unopened + " once, then check again.";
	} else {
		permission.state = State::allowed;
	}
	return permission;
}

// The local network, where the firm's computers on the office network reach
// each other directly. Looking for Singularity on it is what macOS asks
// about; while its question stands the look is refused, so a refusal counts
// only once the person had time to answer.
Permission ask_local_network() {
	Permission permission{"localNetwork", State::unknown, {}};
	struct Look {
		std::mutex mutex;
		std::condition_variable changed;
		bool isReady = false;
		bool isRefused = false;
		bool isOver = false;
	};
	auto look = std::make_shared<Look>();
	nw_browse_descriptor_t descriptor = nw_browse_descriptor_create_bonjour_service("_singularity._udp", "local.");
	nw_parameters_t parameters = nw_parameters_create();
	nw_browser_t browser = nw_browser_create(descriptor, parameters);
	dispatch_queue_t queue = dispatch_queue_create("org.legalmachines.singularity.local-network", DISPATCH_QUEUE_SERIAL);
	nw_browser_set_queue(browser, queue);
	nw_browser_set_state_changed_handler(browser, ^(nw_browser_state_t state, nw_error_t error) {
		const std::lock_guard<std::mutex> lock(look->mutex);
		// Ready after a refusal: the person answered Allow.
		if (state == nw_browser_state_ready) {
			look->isReady = true;
			look->isRefused = false;
		}
		if (state == nw_browser_state_waiting && error != nil && nw_error_get_error_domain(error) == nw_error_domain_dns
				&& nw_error_get_error_code(error) == kDNSServiceErr_PolicyDenied) {
			look->isRefused = true;
		}
		if (state == nw_browser_state_failed || state == nw_browser_state_cancelled) look->isOver = true;
		look->changed.notify_all();
	});
	nw_browser_start(browser);
	{
		std::unique_lock<std::mutex> lock(look->mutex);
		look->changed.wait_for(lock, std::chrono::seconds(20), [&] { return look->isReady || look->isOver; });
		if (look->isReady && !look->isRefused) {
			permission.state = State::allowed;
		} else if (look->isRefused) {
			permission.state = State::denied;
			permission.problem = "Allow Singularity in Privacy & Security, under Local Network, then check again.";
		} else {
			permission.problem = "macOS did not answer. Check again.";
		}
	}
	nw_browser_cancel(browser);
	return permission;
}

} // namespace

// What is known without asking: what this run learned, else what macOS allowed
// this build before - for Office, into every Office application here now.
std::vector<Permission> list() {
	std::vector<Permission> found;
	const Allowed allowed = kept_allowed(build_identity());
	const std::vector<std::string> office = installed_office();
	const bool isOfficeAllowed = std::all_of(office.begin(), office.end(),
		[&allowed](const std::string &name) { return allowed.office.count(name) > 0; });
	const std::lock_guard<std::mutex> lock(known_mutex());
	for (const char *key : {"documents", "officeApps", "localNetwork"}) {
		const auto kept = known().find(key);
		Permission permission = kept != known().end() ? kept->second : Permission{key, State::unknown, {}};
		if (kept == known().end() && allowed.keys.count(key) > 0
				&& (std::string(key) != "officeApps" || isOfficeAllowed)) permission.state = State::allowed;
		if (std::string(key) == "officeApps" && office.empty()) permission.state = State::not_needed;
		found.push_back(permission);
	}
	return found;
}

Permission ask(const std::string &key) {
	if (key == "documents") return remember(ask_documents());
	if (key == "officeApps") return remember(ask_office());
	if (key == "localNetwork") return remember(ask_local_network());
	return Permission{key, State::not_needed, {}};
}

bool open_settings(const std::string &key) {
	NSString *pane = nil;
	if (key == "documents") pane = @"Privacy_FilesAndFolders";
	else if (key == "officeApps") pane = @"Privacy_AllFiles";
	else if (key == "localNetwork") pane = @"Privacy_LocalNetwork";
	if (pane == nil) return false;
	NSURL *url = [NSURL URLWithString:[@"x-apple.systempreferences:com.apple.preference.security?" stringByAppendingString:pane]];
	__block BOOL opened = NO;
	if ([NSThread isMainThread]) {
		opened = [NSWorkspace.sharedWorkspace openURL:url];
	} else {
		dispatch_sync(dispatch_get_main_queue(), ^{
			opened = [NSWorkspace.sharedWorkspace openURL:url];
		});
	}
	return opened == YES;
}

} // namespace singularity::platform::system_permissions
