#pragma once

#include "Matter/Types/TimeEntry.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The time members record on a Matter (D-181): the lawyers' own part of the
// bill, beside the AI colleagues' work that Usage counts. A member records
// their own time, on the day they did the work, and changes or takes back
// only their own; nothing is recorded for them. Kept in the Matter's data, so
// the Matter's other computers have it as they have its threads.
namespace singularity::billing {

class TimeRefused : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// What a member gives when they record or change time. On a change, a field
// left unset stays as it was.
struct TimeInput {
	std::optional<std::string> date;
	std::optional<std::string> start;
	std::optional<int> minutes;
	std::optional<std::string> text;
	std::optional<bool> billable;
};

// The most one entry may hold: a day.
inline constexpr int kMostMinutes = 24 * 60;

// Records the member at this computer's time on the Matter. Refused, and said
// why, for a day that is no date, a start that is no time, minutes outside one
// to a day's, or no words for what the time was spent on.
matters::TimeEntry record_time(const std::string &matter_id, const TimeInput &input);

// Changes an entry the member at this computer recorded; another's is refused.
matters::TimeEntry update_time(const std::string &matter_id, const std::string &entry_id, const TimeInput &input);

// Takes back an entry the member at this computer recorded; another's is
// refused.
void remove_time(const std::string &matter_id, const std::string &entry_id);

// The Matter's time, every member's, the latest day first.
std::vector<matters::TimeEntry> time_of(const std::string &matter_id);

} // namespace singularity::billing
