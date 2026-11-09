#pragma once

#include <optional>
#include <string>
#include <vector>

namespace singularity::platform::update_helper {

// Singularity's updater on Windows, singularity-update.exe. Windows cannot
// replace a program while it runs, so as Singularity ends it copies the
// updater out of its install folder and starts it (UpdateInstallWindows.cpp).
// The updater waits for Singularity to close, checks the downloaded installer
// against the SHA-256 its signed entry gave while it holds the file so that
// nothing can change it, runs it silently into Singularity's own folder with
// the rights an install for every user needs, and starts Singularity again as
// the person, never elevated: an elevated Singularity takes no files dragged
// in from Explorer. While it installs it shows one small window, which says
// what happens and lets Windows ask for permission in front of the person
// rather than from the taskbar. It leaves how it went in the Updates folder,
// in the outcome every system shares (UpdateInstall.cpp), which Singularity
// reads at its next start. The part here is free of Windows and tested on
// every system; UpdateHelperWindows.cpp does the rest.

// What the updater is asked to do.
struct Request {
	unsigned long process_id = 0; // Singularity's own process, waited for
	std::wstring installer;       // the installer downloaded and checked, a full path
	std::string sha256;           // its SHA-256 as the signed entry says, 64 hex digits
	std::string version;          // the version it installs
	std::wstring application;     // Singularity.exe, started again when relaunch is set
	bool relaunch = false;        // start Singularity again once the update is done
	std::wstring folder;          // the Updates folder, where the outcome is left
};

// The request from the updater's command line (its arguments after the
// program's own name), or nothing when a part is missing, repeated or
// malformed.
std::optional<Request> request_of(const std::vector<std::wstring> &arguments);

// The arguments Singularity starts the updater with, as request_of reads them.
std::vector<std::wstring> arguments_of(const Request &request);

// One command line of `program` and `arguments`, each quoted so that
// CommandLineToArgvW gives them back as they are.
std::wstring command_line_of(const std::wstring &program, const std::vector<std::wstring> &arguments);

// What the installer is given: silently (/S), and into the folder Singularity
// runs from (/D=, which NSIS takes last and unquoted, spaces and all). Without
// /D= it would install into its default folder, and a Singularity installed
// in another folder would stay as it was.
std::wstring installer_parameters_of(const Request &request);

// Whether `actual`, a SHA-256 in hex, is `expected`: 64 hex digits each, of
// either case.
bool same_sha256(const std::string &expected, const std::string &actual);

// The member's one sentence when Windows was not given permission, and when
// the update could not install itself for `why` (plain words, no full stop).
std::string refusal_sentence(const std::string &version);
std::string failure_sentence(const std::string &version, const std::string &why);

// The updater's window while it installs: its heading, and its words before
// Windows has given permission and once it has.
std::string window_heading(const Request &request);
std::string window_words(const Request &request, bool permitted);

// The updater's own words when an install it ran failed and left no
// Singularity to say so: `failure` (failure_sentence), then where its
// installer is.
std::string by_hand_words(const std::string &failure);

} // namespace singularity::platform::update_helper
