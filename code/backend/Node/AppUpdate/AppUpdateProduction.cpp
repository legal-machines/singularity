#include "Node/AppUpdate/AppUpdate.hpp"

#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Node/LegalMachinesConnection/LegalMachinesConnection.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/OpenExternal/OpenExternal.hpp"
#include "Platform/UpdateInstall/UpdateInstall.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifndef NATIVE_APP_VERSION
#define NATIVE_APP_VERSION "0.0.0"
#endif

// A build that may try an update before it is offered: a development build
// only. A shipping build has none of it: its binary never holds the name
// SINGULARITY_UPDATE_FEED.
#if !defined(NDEBUG)
#define SINGULARITY_UPDATE_TEST_FEED 1
#endif

namespace singularity::node::update {

namespace {

// Legal Machines' release keys (Ed25519 with SHA-512, RFC 8032; base64 of 32
// bytes): the private half of each signs releases on the computer that
// publishes them and is kept nowhere else. A next key is added here before
// the one in use is retired.
const std::vector<std::string> kReleaseKeys = {
	"BiddF2p9Hxv5DEmQrWe4C8bkXfSoL8Hm9Qqr4cw/z5g=",
};

#if defined(SINGULARITY_UPDATE_TEST_FEED)
// Trying an update before it is offered: SINGULARITY_UPDATE_FEED names a
// folder holding latest.json - a release exactly as the site answers it,
// signed by the throwaway key SINGULARITY_UPDATE_KEY names - and its
// installer. Everything else is checked as for the site.
std::optional<std::filesystem::path> testFeed() {
	const char *value = std::getenv("SINGULARITY_UPDATE_FEED");
	if (value == nullptr || *value == '\0') return std::nullopt;
	return std::filesystem::u8path(value);
}
#endif

// Whether this build installs its updates itself (Status::automatic): a
// development build only from a test feed, never in place of the published one.
bool installsItself() {
#if defined(SINGULARITY_UPDATE_TEST_FEED)
	return testFeed().has_value();
#else
	return true;
#endif
}

// The window's view of updates, one for the app, and whether a download runs.
struct State {
	std::mutex mutex;
	Status status;
	bool isDownloading = false;
	bool isStarted = false;
	// Restart now was asked for: the new version opens once it is installed.
	bool reopens = false;
};

State &kept() {
	static State state;
	return state;
}

Status shownLocked(const State &state) {
	Status shown = state.status;
	shown.current_version = NATIVE_APP_VERSION;
	shown.automatic = installsItself();
	return shown;
}

// A version that could not install itself is installed by hand from then on;
// this note in the Updates folder holds why, in the member's words.
std::filesystem::path byHandNote(const std::filesystem::path &folder, const std::string &version) {
	return folder / ("by-hand-" + version);
}

std::string readText(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeText(const std::filesystem::path &path, const std::string &text) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}

// Once, at the first word of updates: what the last install did, and the
// Updates folder cleared of what it no longer needs - what an install left,
// partial downloads, and every installer and note of a version not later than
// this one.
void startLocked(State &state) {
	if (state.isStarted) return;
	state.isStarted = true;
	try {
		const std::filesystem::path folder = installers_folder();
		const std::optional<platform::update_install::Outcome> outcome = platform::update_install::take_outcome(folder);
		platform::update_install::tidy(folder);
		std::error_code error;
		for (const auto &entry : std::filesystem::directory_iterator(folder, error)) {
			std::error_code entryError;
			if (!entry.is_regular_file(entryError)) continue;
			const std::string version = file_version(entry.path().filename().u8string());
			if (entry.path().extension() == ".part" || version.empty() || compare_versions(version, NATIVE_APP_VERSION) <= 0) {
				std::filesystem::remove(entry.path(), entryError);
			}
		}
		if (outcome && compare_versions(outcome->version, NATIVE_APP_VERSION) > 0) {
			// Not installed: a failure sends that version to the member's hands,
			// with why; a refusal waits for the next close, said once.
			if (outcome->state == "failed") writeText(byHandNote(folder, outcome->version), outcome->detail);
			if (outcome->state != "installed") state.status.last_install = outcome;
		}
	} catch (...) {
	}
}

} // namespace

std::vector<std::string> release_public_keys() {
	std::vector<std::string> keys = kReleaseKeys;
#if defined(SINGULARITY_UPDATE_TEST_FEED)
	const char *key = std::getenv("SINGULARITY_UPDATE_KEY");
	if (key != nullptr && *key != '\0' && testFeed()) keys.emplace_back(key);
#endif
	return keys;
}

std::filesystem::path installers_folder() {
	return platform::computer_records_directory() / "Updates";
}

Status status() {
	State &state = kept();
	const std::lock_guard<std::mutex> lock(state.mutex);
	startLocked(state);
	return shownLocked(state);
}

Status check_now() {
	State &state = kept();
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		startLocked(state);
		// A download under way, or an installer already here, stays as it is.
		const std::string &now = state.status.state;
		if (state.isDownloading || now == "downloaded" || now == "ready") return shownLocked(state);
		state.status.state = "checking";
	}
	Fetch fetch = [](const std::string &url, int timeout_seconds) { return platform::https_get(url, {}, timeout_seconds); };
#if defined(SINGULARITY_UPDATE_TEST_FEED)
	if (const auto feed = testFeed()) {
		fetch = [folder = *feed](const std::string &, int) {
			platform::HttpResponse response;
			std::error_code error;
			if (!std::filesystem::is_regular_file(folder / "latest.json", error)) {
				response.status = 204;
				return response;
			}
			response.status = 200;
			response.body = readText(folder / "latest.json");
			return response;
		};
	}
#endif
	const Check found = check(NATIVE_APP_VERSION, this_os(), this_arch(), release_public_keys(),
		std::string(kLegalMachinesApiBase) + kLatestReleasePath, fetch);
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.status.state = found.state;
	state.status.release = found.release;
	state.status.detail = found.detail;
	state.status.checked_at = workspaces::current_timestamp();
	state.status.received = 0;
	state.status.installer.clear();
	return shownLocked(state);
}

Status download_now() {
	State &state = kept();
	Release release;
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		startLocked(state);
		if (state.isDownloading) throw std::runtime_error("The new version is downloading already.");
		if (!state.status.release) throw std::runtime_error("No new version is known yet. Check for updates first.");
		release = *state.status.release;
		state.isDownloading = true;
		state.status.state = "downloading";
		state.status.received = 0;
		state.status.detail.clear();
	}
	try {
		Download fetch = [](const std::string &url, const std::filesystem::path &destination, const platform::DownloadProgress &progress) {
			return platform::https_download_file(url, {}, destination, 3600, progress);
		};
#if defined(SINGULARITY_UPDATE_TEST_FEED)
		if (const auto feed = testFeed()) {
			fetch = [folder = *feed, name = release.file_name](const std::string &, const std::filesystem::path &destination,
			                                                    const platform::DownloadProgress &progress) {
				std::error_code error;
				std::filesystem::copy_file(folder / std::filesystem::u8path(name), destination,
					std::filesystem::copy_options::overwrite_existing, error);
				if (error) return 404;
				if (progress) progress(std::filesystem::file_size(destination, error));
				return 200;
			};
		}
#endif
		const std::filesystem::path folder = installers_folder();
		const std::filesystem::path installer = download(release, folder, fetch,
			[&state](std::uint64_t received) {
				const std::lock_guard<std::mutex> lock(state.mutex);
				state.status.received = received;
			});
		// Readied to install itself, unless this build installs by hand or this
		// version could not install itself before.
		std::string why;
		bool isReadied = false;
		if (installsItself()) {
			std::error_code error;
			const std::filesystem::path note = byHandNote(folder, release.version);
			if (std::filesystem::is_regular_file(note, error)) {
				why = readText(note);
			} else {
				why = platform::update_install::prepare({release.version, installer, release.sha256});
				isReadied = why.empty();
			}
		}
		const std::lock_guard<std::mutex> lock(state.mutex);
		state.isDownloading = false;
		state.status.received = release.bytes;
		state.status.installer = installer.u8string();
		if (isReadied) {
			state.status.state = "ready";
			state.status.detail = ready_words(this_os());
		} else {
			state.status.state = "downloaded";
			state.status.detail = (why.empty() ? "Singularity " + release.version + " is downloaded." : why)
				+ " Quit Singularity, then run the installer.";
		}
	} catch (const platform::HttpTransportError &error) {
		const std::lock_guard<std::mutex> lock(state.mutex);
		state.isDownloading = false;
		state.status.state = "failed";
		state.status.detail = error.cancelled ? "The download stopped as Singularity closed."
			: "The download stopped: Legal Machines' site stopped answering. Try again.";
	} catch (const std::exception &error) {
		const std::lock_guard<std::mutex> lock(state.mutex);
		state.isDownloading = false;
		state.status.state = "failed";
		state.status.detail = error.what();
	}
	return status();
}

void show_installer() {
	std::string installer;
	{
		State &state = kept();
		const std::lock_guard<std::mutex> lock(state.mutex);
		installer = state.status.installer;
	}
	if (installer.empty()) throw std::runtime_error("No installer is downloaded yet.");
	if (!platform::reveal_in_file_manager(installer)) throw std::runtime_error("The installer's folder could not be shown.");
}

Status install_now() {
	State &state = kept();
	const std::lock_guard<std::mutex> lock(state.mutex);
	startLocked(state);
	if (state.status.state != "ready" || !state.status.release) {
		throw std::runtime_error("No new version is ready to install yet.");
	}
	state.reopens = true;
	return shownLocked(state);
}

void finish_on_exit() noexcept {
	try {
		State &state = kept();
		platform::update_install::Update update;
		bool reopen = false;
		{
			const std::lock_guard<std::mutex> lock(state.mutex);
			if (state.status.state != "ready" || !state.status.release || state.status.installer.empty()) return;
			update.version = state.status.release->version;
			update.installer = std::filesystem::u8path(state.status.installer);
			update.sha256 = state.status.release->sha256;
			reopen = state.reopens;
		}
		(void)platform::update_install::hand_over(update, reopen);
	} catch (...) {
	}
}

} // namespace singularity::node::update
