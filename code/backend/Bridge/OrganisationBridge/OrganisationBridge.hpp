#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>

// The Native API of the organisation (docs/contracts/native/Organisation.json):
// the organisation read (Organisation/), serialised. No rule lives here.
namespace singularity::bridge::organisation_bridge {

// organisation.get -> OrganisationView: the viewer, the members (people and AI
// colleagues together), their areas, the work under way and the decisions
// waiting (Beta 1 #273, #274).
nlohmann::json get(const nlohmann::json &params);

// The viewer as every read for the new frontend carries it (Beta 1 #274): who
// is looking and what they may do, the same object organisation.get holds.
nlohmann::json viewer();

// organisation.setOwnOffice {position, practice} -> the viewer: the place in
// the firm the person at this computer states (Organisation/OrganisationProduction
// set_own_office), written into their member record of every Matter here.
nlohmann::json set_own_office(const nlohmann::json &params);

// organisation.setOwnPhoto {photo} -> the viewer: the photo the person at this
// computer shows the firm (D-163; Organisation/OrganisationProduction
// set_own_photo), a data URL, or null to take it away, written into their
// member record of every Matter here and never sent to Legal Machines.
nlohmann::json set_own_photo(const nlohmann::json &params);

// organisation.seatProfiles -> {profiles: [SeatProfile]}: who sits in each of
// the firm's seats, as the firm's record says (D-209).
nlohmann::json seat_profiles(const nlohmann::json &params);

// organisation.saveSeatProfile {seatId, name, rank?, title?, practice?, photo?}
// -> SeatProfile: written into the firm's record by the firm's administrator,
// for any seat, or by the member for their own (D-209).
nlohmann::json save_seat_profile(const nlohmann::json &params);
// organisation.setSeatState {seatId, state: "" | "archived" | "removed",
// password?} -> SeatProfile (D-214). `at_legal_machines` closes the seat there,
// or makes it again with the password, as `state` asks.
nlohmann::json set_seat_state(const nlohmann::json &params,
	const std::function<void(const std::string &seat_id, const std::string &state, const std::string &password)> &at_legal_machines);

// organisation.chooseOwnPhoto -> {picture} | null: the picture in the file the
// person chose in the system's own dialog (`path`), for the window to cut
// their photo from. Nothing is kept. Null when they cancelled.
nlohmann::json own_photo_picture(const std::optional<std::string> &path);

} // namespace singularity::bridge::organisation_bridge
