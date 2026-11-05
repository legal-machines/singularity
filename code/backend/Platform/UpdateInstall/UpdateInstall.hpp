#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace singularity::platform::update_install {

// Singularity installs its own new versions from 0.1.1 on (D-261; the owner,
// 2026-11-05: "make it so that from version 0.1.1 everything is automatic").
// Node/AppUpdate finds a version Legal Machines signed, downloads it and checks
// it byte for byte; this is the part each system does its own way: readying the
// installer while Singularity runs, and installing it once Singularity has
// ended - then opening the new version, when the member asked to restart.
//
//   macOS:   the new app is unpacked from its disk image into the Updates
//            folder and checked there; as Singularity ends, it takes the old
//            app's place in one step, and the old app, left in its stead, is
//            removed at the next start.
//   Windows: a small helper of its own, run from a folder of its own, waits for
//            Singularity to end, runs the installer once Windows allows it, and
//            opens the new version as the member, never elevated.
//   Linux:   the package is installed once Singularity has ended, after the
//            system asks for the member's password.
//
// Nothing here decides whether a version is genuine: only what Node/AppUpdate
// checked against Legal Machines' signature ever reaches it.

struct Update {
	std::string version;
	// The installer, whole and exactly the one Legal Machines signed, in
	// Singularity's own Updates folder.
	std::filesystem::path installer;
	// Its signed SHA-256, lowercase hex.
	std::string sha256;
};

// Readies `update` to install itself as Singularity ends. Empty when ready;
// otherwise the plain reason it is installed by hand instead. Runs on a worker
// and may take a while; never throws.
std::string prepare(const Update &update);

// As Singularity ends - its work stopped, just before the process exits:
// starts what installs `update` once this process is gone, and opens the new
// version afterwards when `reopen`. Returns at once; never throws. False when
// nothing could be started; the update then waits for the next close.
bool hand_over(const Update &update, bool reopen);

// What the last hand-over did.
struct Outcome {
	// "installed", "refused" (the system's permission was not given) or
	// "failed".
	std::string state;
	std::string version;
	// Plain words for the member.
	std::string detail;
};

// What the last hand-over in `folder` did, read once at start and then
// forgotten; nothing when there is nothing to say.
std::optional<Outcome> take_outcome(const std::filesystem::path &folder);

// Keeps `outcome` in `folder` for the next start; false when it cannot. One
// line, "state<TAB>version<TAB>detail", in outcome.txt, so a helper of any
// system can write it too.
bool record_outcome(const std::filesystem::path &folder, const Outcome &outcome);

// At start, before anything else of updates: removes what an install left in
// `folder` - the old app, a helper's folder, a disk image left attached.
void tidy(const std::filesystem::path &folder);

} // namespace singularity::platform::update_install
