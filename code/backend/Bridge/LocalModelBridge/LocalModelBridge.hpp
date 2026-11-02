#pragma once

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>

namespace singularity::bridge::local_model_bridge {

nlohmann::json list(const nlohmann::json &params);
nlohmann::json runtime_state(const nlohmann::json &params);
nlohmann::json papinian_status(const nlohmann::json &params);
nlohmann::json authorize(const nlohmann::json &params);
nlohmann::json sync(const nlohmann::json &params);
nlohmann::json load(const nlohmann::json &params);
nlohmann::json unload(const nlohmann::json &params);
nlohmann::json remove(const nlohmann::json& params);

// The owner's local-model path: a model file the person already has on this
// Mac becomes usable through local inference without Singularity downloading or copying it.
// `use_existing_file` takes the chosen path (the UI-thread caller supplies it
// from the file picker); `forget_existing_file` removes only Singularity's own
// link to it.
nlohmann::json use_existing_file(const std::optional<std::string> &selected_path);
nlohmann::json forget_existing_file(const nlohmann::json &params);

} // namespace singularity::bridge::local_model_bridge
