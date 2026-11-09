#pragma once

#include <nlohmann/json.hpp>

// The Native API of a Matter's correspondence: where a letter belongs,
// filing it, moving or unfiling it, the Matter's letters, and saving a
// letter's attachment. Parses, calls Correspondence/, serialises; no rule
// lives here.
namespace singularity::bridge::correspondence_bridge {

nlohmann::json suggest(const nlohmann::json &params);
nlohmann::json file(const nlohmann::json &params);
nlohmann::json move(const nlohmann::json &params);
nlohmann::json unfile(const nlohmann::json &params);
nlohmann::json list(const nlohmann::json &params);
// correspondence.draftReply {matterId, letterId, text} -> {opened, carriesText}:
// the reply opened in the member's mail app, never sent; carriesText false when
// the text was too long for a mail link and stays with the window to hand over.
nlohmann::json draft_reply(const nlohmann::json &params);
nlohmann::json save_attachment(const nlohmann::json &params);

} // namespace singularity::bridge::correspondence_bridge
