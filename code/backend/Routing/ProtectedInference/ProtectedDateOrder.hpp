#pragma once

// The order of protected dates.
//
// A date is replaced before a request leaves this device, and a model that
// sees [[E12]] and [[E31]] cannot tell which came first - a chronology comes
// back with the right events in the wrong order, a deadline cannot be compared
// with the day it was missed. The value stays here; what the model is given is
// the relation: each protected date's rank among the dates of the same
// request, 1 for the earliest, equal ranks for the same day. Nothing about a
// rank reveals a date.
//
// Header-only and free of project types so it can be proven on its own.

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::routing::protected_dates {

// The calendar day a piece of text names, as year * 10000 + month * 100 + day,
// for ordering only. A missing day or month is 0, so "June 2026" sorts before
// "June 3, 2026" and "2026" before both. The first date of a range is the
// range's. Nothing recognised: nullopt, and the date simply has no rank.
inline std::optional<long long> comparable_day(const std::string &text) {
	static const std::map<std::string, int> months{
		{"january", 1}, {"jan", 1}, {"february", 2}, {"feb", 2}, {"march", 3}, {"mar", 3}, {"april", 4}, {"apr", 4},
		{"may", 5}, {"june", 6}, {"jun", 6}, {"july", 7}, {"jul", 7}, {"august", 8}, {"aug", 8},
		{"september", 9}, {"sep", 9}, {"sept", 9}, {"october", 10}, {"oct", 10}, {"november", 11}, {"nov", 11},
		{"december", 12}, {"dec", 12},
		{"января", 1}, {"январь", 1}, {"февраля", 2}, {"февраль", 2}, {"марта", 3}, {"март", 3}, {"апреля", 4},
		{"апрель", 4}, {"мая", 5}, {"май", 5}, {"июня", 6}, {"июнь", 6}, {"июля", 7}, {"июль", 7}, {"августа", 8},
		{"август", 8}, {"сентября", 9}, {"сентябрь", 9}, {"октября", 10}, {"октябрь", 10}, {"ноября", 11},
		{"ноябрь", 11}, {"декабря", 12}, {"декабрь", 12},
	};
	// Words and numbers, with the separator that followed each number group so
	// "2026-03-18" and "18.03.2026" can be told from "March 18, 2026".
	struct Token {
		std::string text;
		bool numeric = false;
		char separator = ' ';
	};
	std::vector<Token> tokens;
	std::string current;
	const auto flush = [&](char separator) {
		if (current.empty()) return;
		Token token;
		token.numeric = std::all_of(current.begin(), current.end(),
			[](unsigned char character) { return std::isdigit(character) != 0; });
		token.text = std::move(current);
		token.separator = separator;
		tokens.push_back(std::move(token));
		current.clear();
	};
	for (const char character : text) {
		const auto byte = static_cast<unsigned char>(character);
		if (std::isalnum(byte) != 0 || byte >= 0x80) {
			// Cyrillic capitals fold by their second UTF-8 byte only for the common
			// range; month names are matched in lower case as documents write them.
			current.push_back(static_cast<char>(std::tolower(byte)));
		} else {
			flush(character);
		}
	}
	flush(' ');
	// "18th", "3rd": the number alone.
	for (Token &token : tokens) {
		if (token.numeric || token.text.size() < 3) continue;
		const std::string suffix = token.text.substr(token.text.size() - 2);
		const std::string head = token.text.substr(0, token.text.size() - 2);
		if ((suffix == "st" || suffix == "nd" || suffix == "rd" || suffix == "th") && !head.empty()
				&& std::all_of(head.begin(), head.end(), [](unsigned char character) { return std::isdigit(character) != 0; })) {
			token.text = head;
			token.numeric = true;
		}
	}
	const auto number = [](const Token &token) { return token.text.size() > 9 ? -1LL : std::stoll(token.text); };
	const auto isYear = [&](const Token &token) {
		return token.numeric && token.text.size() == 4 && number(token) >= 1800 && number(token) <= 2200;
	};
	const auto day = [](long long year, long long month, long long dayOfMonth) -> std::optional<long long> {
		if (month < 0 || month > 12 || dayOfMonth < 0 || dayOfMonth > 31) return std::nullopt;
		return year * 10000 + month * 100 + dayOfMonth;
	};

	for (std::size_t index = 0; index < tokens.size(); ++index) {
		const Token &token = tokens[index];
		// 2026-03-18, 2026/03/18, 2026.03.18
		if (isYear(token) && index + 2 < tokens.size() && tokens[index + 1].numeric && tokens[index + 2].numeric
				&& (token.separator == '-' || token.separator == '/' || token.separator == '.')
				&& number(tokens[index + 1]) >= 1 && number(tokens[index + 1]) <= 12 && tokens[index + 2].text.size() <= 2)
			return day(number(token), number(tokens[index + 1]), number(tokens[index + 2]));
		// 18.03.2026 (day first), 03/18/2026 (month first; day first when only that reads as a date)
		if (token.numeric && token.text.size() <= 2 && index + 2 < tokens.size() && tokens[index + 1].numeric
				&& tokens[index + 1].text.size() <= 2 && isYear(tokens[index + 2])
				&& (token.separator == '.' || token.separator == '/' || token.separator == '-')) {
			const long long first = number(token);
			const long long second = number(tokens[index + 1]);
			const bool dayFirst = token.separator == '.' || first > 12;
			return dayFirst ? day(number(tokens[index + 2]), second, first) : day(number(tokens[index + 2]), first, second);
		}
		const auto month = months.find(token.text);
		if (month == months.end()) continue;
		// March 18, 2026 / March 2026
		if (index + 2 < tokens.size() && tokens[index + 1].numeric && tokens[index + 1].text.size() <= 2
				&& isYear(tokens[index + 2]))
			return day(number(tokens[index + 2]), month->second, number(tokens[index + 1]));
		if (index + 1 < tokens.size() && isYear(tokens[index + 1])) {
			// 18 March 2026
			if (index > 0 && tokens[index - 1].numeric && tokens[index - 1].text.size() <= 2)
				return day(number(tokens[index + 1]), month->second, number(tokens[index - 1]));
			return day(number(tokens[index + 1]), month->second, 0);
		}
	}
	for (const Token &token : tokens)
		if (isYear(token)) return day(number(token), 0, 0);
	return std::nullopt;
}

// The rank of each dated value among all of them: 1 for the earliest, the same
// rank for the same day. Values that name no recognisable date are left out.
inline std::map<std::string, int> chronological_ranks(const std::vector<std::pair<std::string, std::string>> &symbolAndText) {
	std::vector<std::pair<long long, std::string>> dated;
	for (const auto &[symbol, text] : symbolAndText)
		if (const auto value = comparable_day(text)) dated.emplace_back(*value, symbol);
	std::sort(dated.begin(), dated.end());
	std::map<std::string, int> ranks;
	int rank = 0;
	std::optional<long long> previous;
	for (const auto &[value, symbol] : dated) {
		if (!previous || *previous != value) ++rank;
		previous = value;
		ranks.emplace(symbol, rank);
	}
	return ranks;
}

} // namespace singularity::routing::protected_dates
