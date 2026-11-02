#include "Billing/TimeEntries/TimeEntries.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/Events/Events.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <utility>

namespace singularity::billing {
namespace {

constexpr std::size_t kMostTextCharacters = 2000;
constexpr std::size_t kMostNameBytes = 200;

std::string trimmed(const std::string &text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	const std::size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

std::size_t characters_in(const std::string &text) {
	std::size_t count = 0;
	for (const unsigned char byte : text) {
		if ((byte & 0xC0u) != 0x80u) ++count;
	}
	return count;
}

// At most `most` bytes, never ending inside a character.
std::string cut(const std::string &text, std::size_t most) {
	if (text.size() <= most) return text;
	std::size_t end = most;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) --end;
	return text.substr(0, end);
}

bool is_digits(const std::string &text, std::size_t from, std::size_t count) {
	for (std::size_t index = from; index < from + count; ++index) {
		if (text[index] < '0' || text[index] > '9') return false;
	}
	return true;
}

int number_at(const std::string &text, std::size_t from, std::size_t count) {
	int value = 0;
	for (std::size_t index = from; index < from + count; ++index) value = value * 10 + (text[index] - '0');
	return value;
}

// A day of the calendar written as "2026-10-31".
bool is_day(const std::string &text) {
	if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
	if (!is_digits(text, 0, 4) || !is_digits(text, 5, 2) || !is_digits(text, 8, 2)) return false;
	const int year = number_at(text, 0, 4);
	const int month = number_at(text, 5, 2);
	const int day = number_at(text, 8, 2);
	if (year < 2000 || month < 1 || month > 12 || day < 1) return false;
	static constexpr int kDaysIn[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	const bool isLeap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	return day <= (month == 2 && isLeap ? 29 : kDaysIn[month - 1]);
}

// The minutes after midnight of a time of day written as "09:30".
std::optional<int> minutes_after_midnight(const std::string &text) {
	if (text.size() != 5 || text[2] != ':' || !is_digits(text, 0, 2) || !is_digits(text, 3, 2)) return std::nullopt;
	const int hour = number_at(text, 0, 2);
	const int minute = number_at(text, 3, 2);
	if (hour > 23 || minute > 59) return std::nullopt;
	return hour * 60 + minute;
}

// Today as this computer's calendar has it: the calendar the member records
// from is this computer's too.
std::string today_here() {
	const std::time_t now = std::time(nullptr);
	std::tm local{};
#if defined(_WIN32)
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	char text[16];
	std::snprintf(text, sizeof(text), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
	return text;
}

void apply(matters::TimeEntry &entry, const TimeInput &input) {
	if (input.date) entry.date = trimmed(*input.date);
	if (input.start) entry.start = trimmed(*input.start);
	if (input.minutes) entry.minutes = *input.minutes;
	if (input.text) entry.text = trimmed(*input.text);
	if (input.billable) entry.billable = *input.billable;
}

// An entry as a bill can read it, or refused with what to give instead.
void require_billable_reading(const matters::TimeEntry &entry) {
	if (!is_day(entry.date)) throw TimeRefused("Give the day the work was done, as 2026-10-31.");
	if (entry.date > today_here()) throw TimeRefused("Time is recorded for work done, and " + entry.date + " has not come yet.");
	std::optional<int> start;
	if (!entry.start.empty()) {
		start = minutes_after_midnight(entry.start);
		if (!start) throw TimeRefused("Give the start as a time of day, as 09:30, or leave it out.");
	}
	if (entry.minutes < 1 || entry.minutes > kMostMinutes) {
		throw TimeRefused("Give the time spent in minutes, from 1 to 1440, a whole day.");
	}
	if (start && *start + entry.minutes > kMostMinutes) {
		throw TimeRefused("An entry ends by midnight. Record the time after it on the next day.");
	}
	if (entry.text.empty()) throw TimeRefused("Say what the time was spent on. The bill reads it.");
	if (characters_in(entry.text) > kMostTextCharacters) {
		throw TimeRefused("Say what the time was spent on in at most 2000 characters.");
	}
}

std::string person_name(const platform::ActorIdentity &identity) {
	if (identity.display_name) {
		const std::string name = cut(trimmed(*identity.display_name), kMostNameBytes);
		if (!name.empty()) return name;
	}
	return "A member";
}

matters::Matter matter_here(const std::filesystem::path &workspace, const std::string &matter_id) {
	std::optional<matters::Matter> matter = matters::load_matter(workspace, matter_id);
	if (!matter) throw TimeRefused("That Matter is not open on this computer.");
	return std::move(*matter);
}

// The member's own entry, or refused: nobody changes another's time.
matters::TimeEntry &own_entry(matters::Matter &matter, const std::string &entry_id, const platform::ActorIdentity &identity) {
	const auto found = std::find_if(matter.time_entries.begin(), matter.time_entries.end(),
		[&](const matters::TimeEntry &entry) { return entry.id == entry_id; });
	if (found == matter.time_entries.end()) throw TimeRefused("That entry is no longer on this Matter.");
	if (found->actor_id != identity.actor_id) {
		throw TimeRefused("Only " + (found->actor_name.empty() ? std::string("the member who recorded it") : found->actor_name)
			+ " changes this entry. Record your own time instead.");
	}
	return *found;
}

void announce(const std::string &matter_id) {
	platform::events::publish("matter.changed", nlohmann::json{{"projectId", matter_id}});
}

} // namespace

matters::TimeEntry record_time(const std::string &matter_id, const TimeInput &input) {
	if (!input.date || !input.minutes || !input.text) {
		throw TimeRefused("Give the day, the time spent and what it was spent on.");
	}
	matters::TimeEntry entry;
	apply(entry, input);
	require_billable_reading(entry);
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	entry.id = "time_" + platform::secure_random_hex(8);
	entry.actor_id = identity.actor_id;
	entry.actor_name = person_name(identity);
	entry.created_at = workspaces::current_timestamp();
	entry.updated_at = entry.created_at;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = matter_here(workspace, matter_id);
		matter.time_entries.push_back(entry);
		matters::save_matter(workspace, matter);
	}
	announce(matter_id);
	return entry;
}

matters::TimeEntry update_time(const std::string &matter_id, const std::string &entry_id, const TimeInput &input) {
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	matters::TimeEntry changed;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = matter_here(workspace, matter_id);
		matters::TimeEntry &entry = own_entry(matter, entry_id, identity);
		changed = entry;
		apply(changed, input);
		if (matters::time_entry_to_json(changed) == matters::time_entry_to_json(entry)) return entry;
		require_billable_reading(changed);
		changed.updated_at = workspaces::current_timestamp();
		entry = changed;
		matters::save_matter(workspace, matter);
	}
	announce(matter_id);
	return changed;
}

void remove_time(const std::string &matter_id, const std::string &entry_id) {
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = matter_here(workspace, matter_id);
		own_entry(matter, entry_id, identity);
		matter.time_entries.erase(std::remove_if(matter.time_entries.begin(), matter.time_entries.end(),
			[&](const matters::TimeEntry &entry) { return entry.id == entry_id; }), matter.time_entries.end());
		matters::save_matter(workspace, matter);
	}
	announce(matter_id);
}

std::vector<matters::TimeEntry> time_of(const std::string &matter_id) {
	std::vector<matters::TimeEntry> entries;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		entries = matter_here(node::require_active_workspace_path(), matter_id).time_entries;
	}
	std::sort(entries.begin(), entries.end(), [](const matters::TimeEntry &left, const matters::TimeEntry &right) {
		if (left.date != right.date) return left.date > right.date;
		if (left.start != right.start) return left.start > right.start;
		if (left.created_at != right.created_at) return left.created_at > right.created_at;
		return left.id > right.id;
	});
	return entries;
}

} // namespace singularity::billing
