#pragma once

#include <functional>

namespace singularity::providers {

// Who may connect or remove the firm's model accounts on this computer: the
// firm's administrator (D-184), as the application installs it from the
// role this computer's account flow recorded (Organisation/FirmRole). With
// none installed, anyone may: a test, or a tool that runs with no firm.
void set_may_connect_accounts(std::function<bool()> may_connect);

// Throws std::runtime_error, in words that say who connects them, on a
// computer whose member may not connect or remove a model account.
void require_may_connect_accounts();

} // namespace singularity::providers
