#include "Bridge/TimeBridge/TimeBridge.hpp"

#include "Billing/TimeEntries/TimeEntries.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace singularity::bridge::time_bridge {
namespace {

using Json = nlohmann::json;
namespace billing = singularity::billing;

std::string required_id(const Json &params, const char *name) {
	if (!params.contains(name) || !params.at(name).is_string() || params.at(name).get<std::string>().empty()) {
		throw std::invalid_argument(std::string(name) + " is required.");
	}
	return params.at(name).get<std::string>();
}

// Text the member gave, empty or not: whether it reads is Billing's to say.
std::string required_text(const Json &params, const char *name) {
	if (!params.contains(name) || !params.at(name).is_string()) throw std::invalid_argument(std::string(name) + " is required.");
	return params.at(name).get<std::string>();
}

std::optional<std::string> optional_text(const Json &params, const char *name) {
	if (!params.contains(name) || params.at(name).is_null()) return std::nullopt;
	if (!params.at(name).is_string()) throw std::invalid_argument(std::string(name) + " must be text.");
	return params.at(name).get<std::string>();
}

// Minutes as Billing judges them: beyond a day they are refused as any other
// time outside one to a day's, never wrapped into a number that reads.
int minutes_of(const Json &value, const char *name) {
	if (!value.is_number_integer()) throw std::invalid_argument(std::string(name) + " must be a whole number.");
	const long long minutes = value.get<long long>();
	return minutes < 0 || minutes > billing::kMostMinutes ? -1 : static_cast<int>(minutes);
}

int required_minutes(const Json &params, const char *name) {
	if (!params.contains(name)) throw std::invalid_argument(std::string(name) + " is required.");
	return minutes_of(params.at(name), name);
}

std::optional<int> optional_minutes(const Json &params, const char *name) {
	if (!params.contains(name) || params.at(name).is_null()) return std::nullopt;
	return minutes_of(params.at(name), name);
}

std::optional<bool> optional_flag(const Json &params, const char *name) {
	if (!params.contains(name) || params.at(name).is_null()) return std::nullopt;
	if (!params.at(name).is_boolean()) throw std::invalid_argument(std::string(name) + " must be true or false.");
	return params.at(name).get<bool>();
}

} // namespace

Json record(const Json &params) {
	billing::TimeInput input;
	input.date = required_text(params, "date");
	input.start = optional_text(params, "start");
	input.minutes = required_minutes(params, "minutes");
	input.text = required_text(params, "text");
	input.billable = optional_flag(params, "billable");
	return matters::time_entry_to_json(billing::record_time(required_id(params, "projectId"), input));
}

Json update(const Json &params) {
	billing::TimeInput input;
	input.date = optional_text(params, "date");
	input.start = optional_text(params, "start");
	input.minutes = optional_minutes(params, "minutes");
	input.text = optional_text(params, "text");
	input.billable = optional_flag(params, "billable");
	return matters::time_entry_to_json(
		billing::update_time(required_id(params, "projectId"), required_id(params, "entryId"), input));
}

Json remove(const Json &params) {
	billing::remove_time(required_id(params, "projectId"), required_id(params, "entryId"));
	return Json{{"removed", true}};
}

Json list(const Json &params) {
	Json entries = Json::array();
	for (const matters::TimeEntry &entry : billing::time_of(required_id(params, "projectId"))) {
		entries.push_back(matters::time_entry_to_json(entry));
	}
	// Whose entries are the member's at this computer: theirs to change.
	return Json{{"entries", entries}, {"actorId", platform::resolve_actor_identity().actor_id}};
}

} // namespace singularity::bridge::time_bridge
