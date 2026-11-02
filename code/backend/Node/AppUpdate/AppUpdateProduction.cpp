#include "Node/AppUpdate/AppUpdate.hpp"

#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Node/LegalMachinesConnection/LegalMachinesConnection.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/OpenExternal/OpenExternal.hpp"

#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifndef NATIVE_APP_VERSION
#define NATIVE_APP_VERSION "0.0.0"
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

// The window's view of updates, one for the app, and whether a download runs.
struct State {
	std::mutex mutex;
	Status status;
	bool isDownloading = false;
};

State &kept() {
	static State state;
	return state;
}

Status shownLocked(const State &state) {
	Status shown = state.status;
	shown.current_version = NATIVE_APP_VERSION;
	return shown;
}

} // namespace

std::vector<std::string> release_public_keys() {
	return kReleaseKeys;
}

std::filesystem::path installers_folder() {
	if (const auto home = platform::home_directory()) {
		const std::filesystem::path downloads = *home / "Downloads";
		std::error_code error;
		if (std::filesystem::is_directory(downloads, error)) return downloads;
	}
	return platform::computer_records_directory() / "Updates";
}

Status status() {
	State &state = kept();
	const std::lock_guard<std::mutex> lock(state.mutex);
	return shownLocked(state);
}

Status check_now() {
	State &state = kept();
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		// A download under way, or an installer already here, stays as it is.
		if (state.isDownloading || state.status.state == "downloaded") return shownLocked(state);
		state.status.state = "checking";
	}
	const Check found = check(NATIVE_APP_VERSION, this_os(), this_arch(), release_public_keys(),
		std::string(kLegalMachinesApiBase) + kLatestReleasePath,
		[](const std::string &url, int timeout_seconds) { return platform::https_get(url, {}, timeout_seconds); });
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
		if (state.isDownloading) throw std::runtime_error("The new version is downloading already.");
		if (!state.status.release) throw std::runtime_error("No new version is known yet. Check for updates first.");
		release = *state.status.release;
		state.isDownloading = true;
		state.status.state = "downloading";
		state.status.received = 0;
		state.status.detail.clear();
	}
	try {
		const std::filesystem::path installer = download(release, installers_folder(),
			[](const std::string &url, const std::filesystem::path &destination, const platform::DownloadProgress &progress) {
				return platform::https_download_file(url, {}, destination, 3600, progress);
			},
			[&state](std::uint64_t received) {
				const std::lock_guard<std::mutex> lock(state.mutex);
				state.status.received = received;
			});
		const std::lock_guard<std::mutex> lock(state.mutex);
		state.isDownloading = false;
		state.status.state = "downloaded";
		state.status.received = release.bytes;
		state.status.installer = installer.u8string();
		state.status.detail = "Singularity " + release.version + " is downloaded. Quit Singularity, then run the installer.";
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

} // namespace singularity::node::update
