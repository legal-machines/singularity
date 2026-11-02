#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace singularity::matters {

// Time a member of the firm recorded on the Matter (D-181): who, on which
// day, for how long, on what, and whether it is billed. A person's time is
// what they record, never inferred from what Singularity saw them do. Kept
// in the Matter's own data (MatterRepository, the "timeEntries" collection):
// one entry is one object, written whole, so two computers' entries merge.
struct TimeEntry {
	std::string id;
	std::string actor_id;
	std::string actor_name;
	// The day the work was done, as the member's calendar has it:
	// "2026-10-31".
	std::string date;
	// "09:30" where the member gave a start, else "".
	std::string start;
	int minutes = 0;
	// What the time was spent on, as the bill will read it.
	std::string text;
	bool billable = true;
	std::string created_at;
	std::string updated_at;
};

inline nlohmann::json time_entry_to_json(const TimeEntry &entry) {
	return nlohmann::json{{"id", entry.id}, {"actorId", entry.actor_id}, {"actorName", entry.actor_name},
		{"date", entry.date}, {"start", entry.start}, {"minutes", entry.minutes}, {"text", entry.text},
		{"billable", entry.billable}, {"createdAt", entry.created_at}, {"updatedAt", entry.updated_at}};
}

inline TimeEntry time_entry_from_json(const nlohmann::json &value) {
	TimeEntry entry;
	if (!value.is_object()) return entry;
	const auto text = [&](const char *key) {
		return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>() : std::string();
	};
	entry.id = text("id");
	entry.actor_id = text("actorId");
	entry.actor_name = text("actorName");
	entry.date = text("date");
	entry.start = text("start");
	entry.minutes = value.contains("minutes") && value.at("minutes").is_number_integer() ? value.at("minutes").get<int>() : 0;
	entry.text = text("text");
	entry.billable = !value.contains("billable") || !value.at("billable").is_boolean() || value.at("billable").get<bool>();
	entry.created_at = text("createdAt");
	entry.updated_at = text("updatedAt");
	return entry;
}

} // namespace singularity::matters
