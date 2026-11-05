#pragma once

#include "Platform/SystemPermissions/SystemPermissions.hpp"

namespace singularity::platform::system_permissions {

// What Windows Defender Firewall's rules for Singularity's own program mean for
// the firm's computers reaching this one directly (D-244), read without asking
// (SystemPermissionsWindows.cpp). Profiles are the firewall's own bits: domain
// 1, private 2, public 4.
struct FirewallRules {
	// Whether Windows answered at all.
	bool readable = false;
	// Whether the firewall is on for a network this computer is on now.
	bool enabled = false;
	// The profiles of the networks this computer is on now; none when it is on
	// no network.
	long current = 0;
	// The profiles an enabled inbound UDP rule for the program allows, and
	// blocks.
	long allowed = 0;
	long blocked = 0;
};

inline constexpr long kFirewallPrivate = 2;
inline constexpr long kFirewallPublic = 4;

// A rule that blocks wins over one that allows, as in the firewall itself.
// With no rule for the program yet, Windows has not asked: unknown. Windows'
// alert writes the rules for the kinds of network the person ticked, and on
// this kind of computer every network may be public, so it is judged by the
// networks this computer is on now, never by "private" alone.
inline Permission firewall_permission(const FirewallRules &rules) {
	Permission permission{"firewall", State::unknown, {}};
	if (rules.readable == false) {
		permission.problem = "Windows did not say whether its firewall lets your colleagues' computers reach this one. Check again.";
		return permission;
	}
	if (rules.enabled == false) {
		permission.state = State::not_needed;
		return permission;
	}
	const long here = rules.current != 0 ? rules.current : (kFirewallPrivate | kFirewallPublic);
	if ((rules.blocked & here) != 0) {
		permission.state = State::denied;
		permission.problem = "Windows Firewall blocks Singularity on this network. Open settings, find Singularity, and tick it for the network you are on.";
	} else if ((rules.allowed & here) == here) {
		permission.state = State::allowed;
	} else if (rules.allowed != 0) {
		permission.state = State::denied;
		permission.problem = "Windows Firewall allows Singularity on another kind of network, not this one. Open settings, find Singularity, and tick it for the network you are on.";
	}
	return permission;
}

} // namespace singularity::platform::system_permissions
