#include "Platform/SystemPermissions/SystemPermissions.hpp"

// Linux asks Singularity for nothing before its work: onboarding lists
// nothing.
namespace singularity::platform::system_permissions {

std::vector<Permission> list() {
	return {};
}

Permission ask(const std::string &key) {
	return Permission{key, State::not_needed, {}};
}

bool open_settings(const std::string &) {
	return false;
}

} // namespace singularity::platform::system_permissions
