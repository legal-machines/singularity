#pragma once

#include "Document/Types/DocumentEvent.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

// The data room's journal, as a request is given it: one JSON record per line,
// newest first, written by Singularity from its own observations
// (Document/Types/DocumentEvent.hpp). A model answers "what is new in the data
// room?" from these records the way it answers anything else from what it is
// given - nothing about such a question is recognised or decided in code.
//
// A run of documents that appeared together (a folder added, a data room
// imported) is one record with their number and the first few names, so an
// import of three hundred files does not push everything else out.
//
// Header-only and pure.
namespace singularity::documents::journal {

inline constexpr std::size_t kRecordsForRequest = 30;
// Documents that appeared within this many seconds of each other, in an
// unbroken run, arrived together.
inline constexpr long long kTogetherSeconds = 120;
inline constexpr std::size_t kTogetherMinimum = 5;
inline constexpr std::size_t kNamesShown = 5;

namespace detail {

// Seconds from an ISO-8601 UTC timestamp ("2026-09-18T01:10:00Z"); -1 when it
// cannot be read. Only differences are used, so days are counted the civil way.
inline long long seconds_of(const std::string &timestamp) {
	if (timestamp.size() < 19) return -1;
	const auto number = [&](std::size_t from, std::size_t length) -> long long {
		long long value = 0;
		for (std::size_t index = from; index < from + length; ++index) {
			if (timestamp[index] < '0' || timestamp[index] > '9') return -1;
			value = value * 10 + (timestamp[index] - '0');
		}
		return value;
	};
	const long long year = number(0, 4), month = number(5, 2), day = number(8, 2);
	const long long hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
	if (year < 0 || month < 1 || month > 12 || day < 1 || hour < 0 || minute < 0 || second < 0) return -1;
	// Days from the civil date (Howard Hinnant's algorithm).
	const long long y = month <= 2 ? year - 1 : year;
	const long long era = (y >= 0 ? y : y - 399) / 400;
	const long long yoe = y - era * 400;
	const long long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return ((era * 146097 + doe - 719468) * 24 + hour) * 3600 + minute * 60 + second;
}

inline std::string folder_of(const std::string &relative_path) {
	const auto slash = relative_path.find_last_of('/');
	return slash == std::string::npos ? std::string() : relative_path.substr(0, slash);
}

inline nlohmann::ordered_json base_record(const DocumentEvent &event, const char *change) {
	return nlohmann::ordered_json{{"kind", "data_room_change"}, {"source", "singularity_journal"},
		{"at", event.at}, {"change", change}};
}

inline nlohmann::ordered_json record_of(const DocumentEvent &event) {
	nlohmann::ordered_json record = base_record(event, to_string(event.kind));
	record["document"] = event.name;
	const std::string folder = folder_of(event.relative_path);
	if (!folder.empty()) record["folder"] = folder;
	if (event.kind == DocumentEventKind::Moved) {
		if (event.previous_name != event.name) record["previousName"] = event.previous_name;
		const std::string previous_folder = folder_of(event.previous_relative_path);
		if (previous_folder != folder) record["previousFolder"] = previous_folder.empty() ? "(the Project's own folder)" : previous_folder;
	}
	if (event.kind == DocumentEventKind::Changed) {
		record["version"] = event.version;
		if (event.previous_version > 0) record["previousVersion"] = event.previous_version;
		if (event.previous_size_bytes > 0 && event.size_bytes > 0) record["sizeChangeBytes"] = event.size_bytes - event.previous_size_bytes;
	}
	if (!event.file_modified_at.empty() && event.kind != DocumentEventKind::Removed) record["fileModifiedAt"] = event.file_modified_at;
	// Found on disk unless Singularity wrote it.
	if (event.actor == "singularity") record["madeBy"] = "Singularity";
	return record;
}

} // namespace detail

// `events` is the Project's journal, oldest first. Resolves at most
// `maximum_records` lines, newest first, each ending in a line feed.
inline std::vector<std::string> records_for_request(const std::vector<DocumentEvent> &events,
		std::size_t maximum_records = kRecordsForRequest) {
	std::vector<nlohmann::ordered_json> records;
	std::size_t index = 0;
	while (index < events.size()) {
		const DocumentEvent &first = events[index];
		// A run of arrivals, each close on the one before it.
		std::size_t end = index + 1;
		if (first.kind == DocumentEventKind::Added && first.actor.empty()) {
			long long previous = detail::seconds_of(first.at);
			while (end < events.size() && events[end].kind == DocumentEventKind::Added && events[end].actor.empty()) {
				const long long at = detail::seconds_of(events[end].at);
				if (previous < 0 || at < 0 || at - previous > kTogetherSeconds) break;
				previous = at;
				++end;
			}
		}
		if (end - index >= kTogetherMinimum) {
			nlohmann::ordered_json record = detail::base_record(events[end - 1], "added");
			record["documents"] = end - index;
			nlohmann::ordered_json names = nlohmann::ordered_json::array();
			for (std::size_t shown = index; shown < end && shown < index + kNamesShown; ++shown) names.push_back(events[shown].name);
			record["firstNames"] = std::move(names);
			records.push_back(std::move(record));
			index = end;
			continue;
		}
		records.push_back(detail::record_of(first));
		++index;
	}

	std::vector<std::string> lines;
	for (auto record = records.rbegin(); record != records.rend() && lines.size() < maximum_records; ++record) {
		lines.push_back(record->dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n");
	}
	return lines;
}

} // namespace singularity::documents::journal
