#pragma once

#include <nlohmann/json.hpp>

namespace singularity::bridge::update_bridge {

// Singularity's new versions from Legal Machines' site (Node/AppUpdate): what
// is known now, asking the site now, downloading the version found - its
// progress read from the status meanwhile - showing the installer in its
// folder, and Restart now, which installs the version readied as Singularity
// ends (D-261).
nlohmann::json status(const nlohmann::json &params);
nlohmann::json check(const nlohmann::json &params);
nlohmann::json download(const nlohmann::json &params);
nlohmann::json show(const nlohmann::json &params);
nlohmann::json install(const nlohmann::json &params);

} // namespace singularity::bridge::update_bridge
