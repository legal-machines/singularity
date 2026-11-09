#pragma once

#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/UpdateInstall/UpdateInstall.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::node::update {

// Singularity's new versions come from Legal Machines' own site. The site
// names the latest version for this computer's system and processor; Legal
// Machines' release key signs what it names - the version, the installer's
// name, its size and its SHA-256 - so whoever holds the site alone can never
// hand a firm another program. Singularity asks when it opens, every few
// hours and when the member asks. From 0.1.1 on it installs what it finds
// itself: it downloads the new version into its own Updates folder, keeps it
// only if it is exactly the one signed, readies it to install
// (Platform/UpdateInstall), and installs it as Singularity ends - when the
// member asks to restart, or the next time Singularity closes - never in the
// middle of anyone's work. Where it cannot install itself, it says why and
// shows the installer for the member to run, as before. The request names the
// system, the processor and this version - nothing of the firm.

// Where the latest release is asked for, below Legal Machines' API
// (Node/LegalMachinesConnection kLegalMachinesApiBase), and the one place an
// installer is ever downloaded from.
inline constexpr const char *kLatestReleasePath = "/releases/latest";
inline constexpr const char *kDownloadPrefix = "https://legalmachines.org/singularity/download/";
inline constexpr int kCheckTimeoutSeconds = 8;
// No installer of Singularity comes near this; anything larger is refused.
inline constexpr std::uint64_t kMostInstallerBytes = 4ull * 1024 * 1024 * 1024;

// This build's system and processor, as the site names them: "windows",
// "macos" or "linux"; "x64" or "arm64".
std::string this_os();
std::string this_arch();

// A release as the site describes it for one system and processor.
struct Release {
	std::string version;
	std::string published_at;
	std::string notes;
	std::string os;
	std::string arch;
	std::string file_name;
	std::uint64_t bytes = 0;
	std::string sha256;
	std::string url;
	std::string signature;
};

// A version as releases carry one: three numbers, "0.10.0", nothing else.
bool is_version(const std::string &version);

// Which of two versions is later: negative, zero or positive. "0.10.0" comes
// after "0.9.3"; what is not a version comes before any version.
int compare_versions(const std::string &first, const std::string &second);

// The words a release's signature covers, one per line joined by a line
// feed with none after the last: "singularity-release/1", then version, os,
// arch, fileName, bytes, sha256, publishedAt, notesSha256 (the SHA-256 of the
// notes' UTF-8 as served) and url, each as name=value - so neither the
// installer's bytes nor its address change without Legal Machines' key.
std::string signed_text(const Release &release);

// What asking the site found.
struct Check {
	// "current": nothing later for this computer; "available": `release` is
	// later; "offline": the site did not answer, or offers no updates yet;
	// "refused": what came was not a release Legal Machines signed for this
	// computer. Neither of the last two is ever an alarm: nothing is offered,
	// and a check the member asked for says it could not check.
	std::string state;
	std::optional<Release> release;
	// Plain words for the member, never a transport error.
	std::string detail;
};

using Fetch = std::function<platform::HttpResponse(const std::string &url, int timeout_seconds)>;

// Asks `latest_url` for this system's latest release and keeps it only when
// one of `release_keys` (Ed25519, base64) signed it, it is for `os` and
// `arch`, its installer is named, sized and addressed as an installer of
// Singularity is, and it is later than `current_version`. Never throws.
Check check(const std::string &current_version, const std::string &os, const std::string &arch,
            const std::vector<std::string> &release_keys, const std::string &latest_url, const Fetch &fetch);

using Download = std::function<int(const std::string &url, const std::filesystem::path &destination,
	const platform::DownloadProgress &progress)>;

// The words that follow "Singularity 0.1.2 is ready." on `os`: when it
// installs, and what the system asks for then.
std::string ready_words(const std::string &os);

// The version an Updates folder's file is of - "singularity-0.1.2-macOS-arm64.dmg",
// "by-hand-0.1.2" - or empty when its name names none.
std::string file_version(const std::string &name);

// Downloads `release`'s installer into `folder` and returns its path: what
// arrives must be exactly the size and SHA-256 the signature covers, or it is
// removed and refused in plain words (std::runtime_error). An installer
// already there, whole, is not downloaded again; another file of that name is
// never overwritten.
std::filesystem::path download(const Release &release, const std::filesystem::path &folder, const Download &fetch,
                               const platform::DownloadProgress &progress);

// ---- This computer's updates, for the window (update.*) ------------------

// What the window shows of updates: the last check, a download under way and
// the installer downloaded.
struct Status {
	// "unknown" before the first check; then "checking", "current",
	// "available", "downloading", "ready" (downloaded and readied: it installs
	// as Singularity ends), "downloaded" (for the member to install by hand),
	// "offline", "refused" or "failed" (a download that did not finish).
	std::string state = "unknown";
	// Whether this build downloads and installs its updates itself: every
	// shipping build; a development build only from a test feed, so it is never
	// swapped for the published one.
	bool automatic = false;
	std::string current_version;
	std::optional<Release> release;
	// When the site was last asked, UTC ISO-8601; empty before.
	std::string checked_at;
	// While downloading: how much has arrived.
	std::uint64_t received = 0;
	// Once downloaded: the installer's path.
	std::string installer;
	std::string detail;
	// What the last install did, when there is something to say of it: an
	// install the system's permission was refused for, or one that failed.
	std::optional<platform::update_install::Outcome> last_install;
};

Status status();
// Asks the site now (Singularity opening, every few hours, the member's
// asking). A check while a download runs or after one finished leaves that
// as it is.
Status check_now();
// Downloads the release found, waiting until it is here or refused; its
// progress shows in status() meanwhile. The caller's HttpCancelScope stops it.
Status download_now();
// Shows the downloaded installer in its folder.
void show_installer();
// Restart now: the version readied installs as Singularity ends, and the new
// version opens then. The caller closes the window, Singularity's ordinary end.
Status install_now();
// Called as Singularity ends, whichever way - its work stopped, just before
// the process exits: hands a readied version over to be installed. Never
// throws.
void finish_on_exit() noexcept;

// The keys that sign Singularity's releases (base64 of 32 bytes each): the
// one in use, and a next one once it is made, so the key can change without
// leaving a firm behind. And the folder installers go to: Singularity's own
// Updates folder in this computer's records, which nothing else writes and no
// system asks permission for.
std::vector<std::string> release_public_keys();
std::filesystem::path installers_folder();

} // namespace singularity::node::update
