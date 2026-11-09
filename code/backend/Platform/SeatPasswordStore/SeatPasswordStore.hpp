#pragma once

#include <optional>
#include <string>

namespace singularity::platform::seat_password_store {

// Where a Legal Machines seat's password may be kept for this computer's user,
// only when the member chooses so after signing in: on Windows, the user's
// Credential Manager, as a generic credential that never roams
// (CRED_PERSIST_LOCAL_MACHINE), which the person sees and can delete there
// too. macOS and Linux keep none here yet: kind is empty, and saving keeps
// nothing.
//
// Every entry's target begins with kTargetPrefix, and the account bridge names
// the rest from the organisation's email and the seat: no call reads, writes
// or removes another program's credential. A password read here goes only to
// Legal Machines' sign-in. It is never logged, and never handed to the page.
inline constexpr const char *kTargetPrefix = "Legal Machines/Singularity/";

// "windows" where a seat's password can be kept; empty where it cannot.
std::optional<std::string> kind();

// Keeps `password` under `target` for this user, replacing what was there.
// False when nothing was kept: a target not Singularity's, an empty
// password, or the store refused.
bool save(const std::string &target, const std::string &user, const std::string &password);

// The password kept under `target`, if one is.
std::optional<std::string> read(const std::string &target);

// Removes what is kept under `target`. False when nothing was there.
bool forget(const std::string &target);

} // namespace singularity::platform::seat_password_store
