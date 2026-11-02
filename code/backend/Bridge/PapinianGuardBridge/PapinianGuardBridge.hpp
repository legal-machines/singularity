#pragma once

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::papinian_guard_bridge {

nlohmann::json get_policy(const nlohmann::json &params);
nlohmann::json save_policy(const nlohmann::json &params);
nlohmann::json preview(const nlohmann::json &params);
// What Papinian Guard hides in one passage of a Project: the passage is run
// through the same Guard, under the same policy and with the same Project
// entities, as a request that would carry it - and what came back is what
// would be replaced, by what kind of symbol. Read-only: nothing is sent,
// stored or added to the Project's entity registry.
nlohmann::json preview_passage(const nlohmann::json &params);
// One document of a Project as a model receives it: each passage as written
// and as masked, and each symbol with the spellings it stands for
// (privacy_actions::preview_document_protection). Read-only, for the window.
nlohmann::json preview_document(const nlohmann::json &params);

} // namespace singularity::bridge::papinian_guard_bridge
