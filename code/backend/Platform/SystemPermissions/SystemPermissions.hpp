#pragma once

#include <string>
#include <vector>

// What the computer itself must allow Singularity, asked during onboarding,
// before the work that needs it, and checked (the owner, 2026-11-04: "the app
// forgot to ask in advance for the permissions it needs on the onboarding
// page; all of this must be asked by us in advance, and we must check that
// it was given"). Each system has its own:
// - macOS (SystemPermissionsMacOS.mm): the Documents folder, where Matters
//   live; Word's, Excel's and PowerPoint's own folders, where the add-in goes,
//   which macOS guards as "data from other apps"; the local network, where
//   the firm's computers reach each other;
// - Windows (SystemPermissionsWindows.cpp): what Windows asks;
// - Linux: none.
namespace singularity::platform::system_permissions {

enum class State { allowed, denied, unknown, not_needed };

inline const char *word(State state) {
	switch (state) {
		case State::allowed: return "allowed";
		case State::denied: return "denied";
		case State::not_needed: return "not_needed";
		default: return "unknown";
	}
}

struct Permission {
	std::string key;
	State state = State::unknown;
	// Why it is not allowed, or what to do first, in the person's words;
	// empty when it is allowed.
	std::string problem;
};

// This system's permissions as known without asking: what this run learned,
// else what the system allowed this same build before, which macOS keeps per
// build (D-249); any other is unknown. Never brings up a question of the
// system's.
std::vector<Permission> list();

// Asks the system for one - its own question comes up where it asks - and
// says what came of it. May wait while the person answers. An unknown key is
// not needed.
Permission ask(const std::string &key);

// Opens the place in the system's settings where the person allows it.
bool open_settings(const std::string &key);

} // namespace singularity::platform::system_permissions
