#pragma once

#include "Matter/FirmComputers/FirmComputers.hpp"
#include "Matter/MatterCoordination/MatterCoordination.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::matter::firm_computers {

// The firm's computers as the app finds them: through Legal Machines with
// this computer's session, its own device key and X25519 key, and the
// lawyer's decisions in Tool/DeviceTrust.
Environment production_environment();

// The computers a lawyer here checked, as the coordination counts a member's
// computer: their member "seat:<id>", their keys and where they are now.
std::vector<coordination::CheckedFirmComputer> checked_for_coordination();

// The device key of `device_id` when a lawyer here checked it as one of the
// firm's computers and its seat is the administrator's by Legal Machines' own
// list of the firm's seats; none otherwise, and none when that list cannot be
// read. Never what a computer says of itself: the role the firm's computer
// list carries is only for the lawyer's eyes.
std::optional<std::string> administrator_computer_key(const std::string &device_id);

} // namespace singularity::matter::firm_computers
