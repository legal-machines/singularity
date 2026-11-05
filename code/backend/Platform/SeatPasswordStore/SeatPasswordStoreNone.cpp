#include "Platform/SeatPasswordStore/SeatPasswordStore.hpp"

// macOS and Linux keep no seat password yet (D-243). On the Mac, the system's
// Passwords app takes a password for legalmachines.org only from an app signed
// with a Developer ID and associated domains, which Singularity has not yet; a
// login-keychain item of an ad hoc signed build would ask the person again
// with every version. The member types the password, and password managers
// read the sign-in's own autocomplete hints.

namespace singularity::platform::seat_password_store {

std::optional<std::string> kind() {
	return std::nullopt;
}

bool save(const std::string &, const std::string &, const std::string &) {
	return false;
}

std::optional<std::string> read(const std::string &) {
	return std::nullopt;
}

bool forget(const std::string &) {
	return false;
}

} // namespace singularity::platform::seat_password_store
