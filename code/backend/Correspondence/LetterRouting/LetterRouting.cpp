#include "Correspondence/LetterRouting/LetterRouting.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace singularity::correspondence {
namespace {

constexpr std::size_t kMostEvidence = 8;

std::string lowered(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return text;
}

std::string who(const Party &party) {
	return party.name.empty() ? party.address : party.name;
}

// A file's name without its kind: "Harborline Mutual NDA".
std::string title_of(const std::string &file_name) {
	return std::filesystem::u8path(file_name).stem().u8string();
}

// Whether `text` (in lower case) holds `name` as words of its own, not inside
// a longer word. Names shorter than four characters say nothing.
bool names(const std::string &text, const std::string &name) {
	const std::string wanted = lowered(one_line(name, 400));
	if (wanted.size() < 4) return false;
	std::size_t at = text.find(wanted);
	while (at != std::string::npos) {
		const bool starts = at == 0 || std::isalnum(static_cast<unsigned char>(text[at - 1])) == 0;
		const std::size_t after = at + wanted.size();
		const bool ends = after >= text.size() || std::isalnum(static_cast<unsigned char>(text[after])) == 0;
		if (starts && ends) return true;
		at = text.find(wanted, at + 1);
	}
	return false;
}

bool holds_address(const Letter &letter, const std::string &address) {
	if (address_of(letter.from) == address) return true;
	for (const Party &party : letter.to) {
		if (address_of(party) == address) return true;
	}
	for (const Party &party : letter.cc) {
		if (address_of(party) == address) return true;
	}
	return false;
}

} // namespace

std::string cut_text(const std::string &text, std::size_t most) {
	if (text.size() <= most) return text;
	std::size_t end = most;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
	return text.substr(0, end);
}

std::string one_line(const std::string &value, std::size_t most) {
	std::string line;
	bool space = false;
	for (const char character : value) {
		if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
			space = !line.empty();
			continue;
		}
		if (space) line.push_back(' ');
		space = false;
		line.push_back(character);
	}
	return cut_text(line, most);
}

std::string address_of(const Party &party) {
	return lowered(one_line(party.address, 320));
}

std::string day_of(const std::string &moment) {
	return moment.size() >= 10 ? moment.substr(0, 10) : moment;
}

const Letter *letter_with_message_id(const matters::Matter &matter, const std::string &internet_message_id) {
	if (internet_message_id.empty()) return nullptr;
	for (const Letter &letter : matter.correspondence) {
		if (letter.internet_message_id == internet_message_id) return &letter;
	}
	return nullptr;
}

// The rule of the Matter's intake the letter answers to, or null: the account
// it reached and the mailbox it came from, each exactly (D-228).
const IntakeRule *intake_rule_for(const matters::Matter &matter, const MessageFacts &facts) {
	const std::string mailbox = lowered(one_line(facts.mailbox, 320));
	const std::string from = address_of(facts.from);
	if (mailbox.empty() || from.empty()) return nullptr;
	for (const IntakeRule &rule : matter.correspondence_intake) {
		if (lowered(one_line(rule.mailbox, 320)) == mailbox && lowered(one_line(rule.from, 320)) == from) return &rule;
	}
	return nullptr;
}

Suggestion suggest_among(const std::vector<matters::Matter> &matters, const MessageFacts &facts) {
	Suggestion suggestion;
	const std::string message_id = one_line(facts.internet_message_id, 1000);

	// The letter itself, filed already.
	for (const matters::Matter &matter : matters) {
		if (const Letter *filed = letter_with_message_id(matter, message_id)) {
			MatterEvidence entry;
			entry.matter_id = matter.id;
			entry.matter_name = matter.name;
			entry.strongest = "filed";
			entry.letter_id = filed->id;
			entry.evidence.push_back("Filed here by " + (filed->filing.actor_name.empty() ? std::string("a member") : filed->filing.actor_name)
				+ " on " + day_of(filed->filing.at) + ".");
			suggestion.matters.push_back(std::move(entry));
		}
	}
	if (!suggestion.matters.empty()) {
		suggestion.outcome = "filed";
		return suggestion;
	}

	// A rule of a Matter's intake (D-228): the account it reached and the
	// mailbox it came from. One Matter's rule files it there; more than one is
	// for the member to choose.
	for (const matters::Matter &matter : matters) {
		const IntakeRule *rule = intake_rule_for(matter, facts);
		if (rule == nullptr) continue;
		MatterEvidence entry;
		entry.matter_id = matter.id;
		entry.matter_name = matter.name;
		entry.strongest = "rule";
		entry.evidence.push_back("A rule of its intake: letters to " + rule->mailbox + " from " + rule->from + ".");
		suggestion.matters.push_back(std::move(entry));
	}
	if (!suggestion.matters.empty()) {
		suggestion.outcome = suggestion.matters.size() == 1 ? "rule" : "ambiguous";
		return suggestion;
	}

	// The people in it other than the mailbox's own, by their exact address.
	const std::string own = lowered(one_line(facts.mailbox, 320));
	std::vector<Party> people;
	std::set<std::string> seen;
	const auto add = [&](const Party &party) {
		const std::string address = address_of(party);
		if (address.empty() || address == own || seen.count(address) > 0) return;
		seen.insert(address);
		people.push_back(party);
	};
	add(facts.from);
	for (const Party &party : facts.to) add(party);
	for (const Party &party : facts.cc) add(party);
	const std::string subject = lowered(one_line(facts.subject, 500));
	const std::string conversation = one_line(facts.conversation_id, 1000);

	std::vector<std::pair<int, MatterEvidence>> found;
	for (const matters::Matter &matter : matters) {
		MatterEvidence entry;
		entry.matter_id = matter.id;
		entry.matter_name = matter.name;
		int strength = 0;
		const auto note = [&](int level, const char *kind, const std::string &sentence) {
			if (entry.evidence.size() >= kMostEvidence) return;
			entry.evidence.push_back(sentence);
			if (level > strength) {
				strength = level;
				entry.strongest = kind;
			}
		};
		// A letter of the same conversation, by the provider's own id.
		if (!conversation.empty()) {
			for (const Letter &letter : matter.correspondence) {
				if (letter.conversation_id != conversation) continue;
				note(3, "conversation", "A letter of this conversation is filed here: “" + letter.subject + "” from "
					+ who(letter.from) + ", " + day_of(letter.sent_at) + ".");
				break;
			}
		}
		// Someone in it is in a letter filed here.
		for (const Party &person : people) {
			const std::string address = address_of(person);
			for (const Letter &letter : matter.correspondence) {
				if (!holds_address(letter, address)) continue;
				note(2, "participant", who(person) + " (" + person.address + ") is in a letter filed here: “" + letter.subject + "”.");
				break;
			}
		}
		// Its subject or an attachment names the Matter or one of its documents.
		if (names(subject, matter.name)) note(1, "reference", "The subject names the Matter, “" + matter.name + "”.");
		if (!matter.appearance.short_name.empty() && lowered(matter.appearance.short_name) != lowered(matter.name)
			&& names(subject, matter.appearance.short_name)) {
			note(1, "reference", "The subject names the Matter, “" + matter.appearance.short_name + "”.");
		}
		std::set<std::string> named;
		for (const matters::MatterDocument &document : matter.documents) {
			if (!document.available) continue;
			const std::string title = title_of(document.name);
			if (title.size() < 6 || named.count(lowered(title)) > 0) continue;
			if (names(subject, title)) {
				named.insert(lowered(title));
				note(1, "reference", "The subject names “" + title + "”, a document of this Matter.");
				continue;
			}
			for (const LetterAttachment &attachment : facts.attachments) {
				if (lowered(title_of(attachment.name)) != lowered(title)) continue;
				named.insert(lowered(title));
				note(1, "reference", "The attachment “" + attachment.name + "” has the name of a document of this Matter.");
				break;
			}
		}
		if (strength > 0) found.emplace_back(strength, std::move(entry));
	}
	std::stable_sort(found.begin(), found.end(), [](const auto &one, const auto &other) {
		if (one.first != other.first) return one.first > other.first;
		return one.second.evidence.size() > other.second.evidence.size();
	});
	const auto conversations = std::count_if(found.begin(), found.end(), [](const auto &entry) { return entry.first == 3; });
	for (auto &entry : found) suggestion.matters.push_back(std::move(entry.second));

	if (suggestion.matters.empty()) {
		suggestion.outcome = "unfiled";
	} else if (suggestion.matters.size() == 1) {
		suggestion.outcome = "suggested";
	} else {
		// One Matter holding the conversation outweighs the rest; anything else
		// is the member's choice, never made for them.
		suggestion.outcome = conversations == 1 ? "suggested" : "ambiguous";
	}
	return suggestion;
}

// ---- An email a member puts in the Matter's folder (D-180) ---------------------

namespace {

std::string trimmed_words(const std::string &text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// One entry of an address line as the person it names.
std::optional<Party> party_in(std::string entry) {
	// A group's name ("Deal team: a@x.example") is not a person.
	bool quoted = false;
	int angle = 0;
	for (std::size_t at = 0; at < entry.size(); ++at) {
		const char character = entry[at];
		if (character == '"' && (at == 0 || entry[at - 1] != '\\')) quoted = !quoted;
		else if (quoted == false && character == '<') ++angle;
		else if (quoted == false && character == '>' && angle > 0) --angle;
		else if (quoted == false && angle == 0 && character == ':') {
			entry = entry.substr(at + 1);
			break;
		}
	}
	entry = trimmed_words(entry);
	if (entry.empty()) return std::nullopt;
	Party party;
	const std::size_t open = entry.rfind('<');
	const std::size_t close = entry.rfind('>');
	if (open != std::string::npos && close != std::string::npos && close > open) {
		party.address = trimmed_words(entry.substr(open + 1, close - open - 1));
		party.name = trimmed_words(entry.substr(0, open));
	} else {
		party.address = entry;
	}
	if (party.name.size() >= 2 && party.name.front() == '"' && party.name.back() == '"') {
		party.name = party.name.substr(1, party.name.size() - 2);
	}
	std::string name;
	for (std::size_t at = 0; at < party.name.size(); ++at) {
		if (party.name[at] == '\\' && at + 1 < party.name.size()) ++at;
		name += party.name[at];
	}
	party.name = one_line(name, 200);
	party.address = one_line(party.address, 320);
	if (party.address.find('@') == std::string::npos || party.address.find(' ') != std::string::npos) return std::nullopt;
	return party;
}

// Days from 1970-01-01 to a civil date (proleptic Gregorian).
long long days_from_civil(long long year, unsigned month, unsigned day) {
	year -= month <= 2 ? 1 : 0;
	const long long era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(year - era * 400);
	const unsigned shifted = month > 2 ? month - 3 : month + 9;
	const unsigned doy = (153 * shifted + 2) / 5 + day - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<long long>(doe) - 719468;
}

void civil_from_days(long long days, long long &year, unsigned &month, unsigned &day) {
	days += 719468;
	const long long era = (days >= 0 ? days : days - 146096) / 146097;
	const unsigned doe = static_cast<unsigned>(days - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	year = static_cast<long long>(yoe) + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	day = doy - (153 * mp + 2) / 5 + 1;
	month = mp < 10 ? mp + 3 : mp - 9;
	year += month <= 2 ? 1 : 0;
}

} // namespace

std::vector<Party> parties_of(const std::string &header) {
	std::vector<Party> parties;
	std::string entry;
	bool quoted = false;
	int angle = 0;
	const auto take = [&]() {
		if (const std::optional<Party> party = party_in(entry)) parties.push_back(*party);
		entry.clear();
	};
	for (std::size_t at = 0; at < header.size(); ++at) {
		const char character = header[at];
		if (character == '"' && (at == 0 || header[at - 1] != '\\')) quoted = !quoted;
		else if (quoted == false && character == '<') ++angle;
		else if (quoted == false && character == '>' && angle > 0) --angle;
		if (quoted == false && angle == 0 && (character == ',' || character == ';')) {
			take();
			continue;
		}
		entry += character;
	}
	take();
	return parties;
}

std::string moment_of(const std::string &date) {
	// A comment ("(PDT)") says nothing the zone does not.
	std::string words;
	int depth = 0;
	for (const char character : date) {
		if (character == '(') ++depth;
		else if (character == ')' && depth > 0) --depth;
		else if (depth == 0) words += character == ',' ? ' ' : character;
	}
	std::vector<std::string> parts;
	{
		std::string part;
		for (const char character : words + " ") {
			if (std::isspace(static_cast<unsigned char>(character)) != 0) {
				if (part.empty() == false) parts.push_back(part);
				part.clear();
			} else {
				part += character;
			}
		}
	}
	static const std::vector<std::string> months{"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov",
		"dec"};
	std::size_t at = 0;
	if (at < parts.size() && std::isalpha(static_cast<unsigned char>(parts[at].front())) != 0) ++at;
	// A day, a month, a year and a time, at least.
	if (at + 3 >= parts.size()) return std::string();
	const auto number = [](const std::string &text, long long &value) {
		if (text.empty() || text.size() > 4 || std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) == false)
			return false;
		value = std::stoll(text);
		return true;
	};
	long long day = 0;
	long long year = 0;
	if (number(parts[at], day) == false || day < 1 || day > 31) return std::string();
	const auto month = std::find(months.begin(), months.end(), lowered(parts[at + 1].substr(0, 3)));
	if (month == months.end() || number(parts[at + 2], year) == false) return std::string();
	if (parts[at + 2].size() <= 2) year += year < 50 ? 2000 : 1900;
	long long hour = 0;
	long long minute = 0;
	long long second = 0;
	{
		const std::string &time = parts[at + 3];
		const std::size_t first = time.find(':');
		if (first == std::string::npos) return std::string();
		const std::size_t secondColon = time.find(':', first + 1);
		if (number(time.substr(0, first), hour) == false
				|| number(time.substr(first + 1, secondColon == std::string::npos ? std::string::npos : secondColon - first - 1), minute)
					== false
				|| (secondColon != std::string::npos && number(time.substr(secondColon + 1), second) == false))
			return std::string();
		if (hour > 23 || minute > 59 || second > 60) return std::string();
	}
	// The zone: "+hhmm" or "-hhmm", else a name the standard knows.
	long long offset = 0;
	if (at + 4 < parts.size()) {
		const std::string zone = parts[at + 4];
		if ((zone.front() == '+' || zone.front() == '-') && zone.size() == 5) {
			long long value = 0;
			if (number(zone.substr(1), value) == false) return std::string();
			offset = (value / 100) * 60 + value % 100;
			if (zone.front() == '-') offset = -offset;
		} else {
			static const std::vector<std::pair<std::string, long long>> named{{"ut", 0}, {"utc", 0}, {"gmt", 0}, {"z", 0},
				{"est", -300}, {"edt", -240}, {"cst", -360}, {"cdt", -300}, {"mst", -420}, {"mdt", -360}, {"pst", -480},
				{"pdt", -420}};
			const auto found = std::find_if(named.begin(), named.end(),
				[&](const auto &entry) { return entry.first == lowered(zone); });
			if (found != named.end()) offset = found->second;
		}
	}
	const unsigned monthNumber = static_cast<unsigned>(month - months.begin()) + 1;
	long long minutes = days_from_civil(year, monthNumber, static_cast<unsigned>(day)) * 1440 + hour * 60 + minute - offset;
	long long days = minutes >= 0 ? minutes / 1440 : (minutes - 1439) / 1440;
	minutes -= days * 1440;
	long long utcYear = 0;
	unsigned utcMonth = 0;
	unsigned utcDay = 0;
	civil_from_days(days, utcYear, utcMonth, utcDay);
	char moment[32];
	std::snprintf(moment, sizeof(moment), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", utcYear, utcMonth, utcDay, minutes / 60,
		minutes % 60, second > 59 ? 59LL : second);
	return moment;
}

std::string conversation_of(const EmailFile &email) {
	const std::string references = trimmed_words(email.references);
	if (references.empty() == false) {
		const std::size_t open = references.find('<');
		const std::size_t close = references.find('>', open == std::string::npos ? 0 : open);
		if (open != std::string::npos && close != std::string::npos) return references.substr(open, close - open + 1);
		const std::size_t space = references.find_first_of(" \t");
		return references.substr(0, space);
	}
	const std::string answers = trimmed_words(email.in_reply_to);
	if (answers.empty() == false) return answers;
	return trimmed_words(email.message_id);
}

std::optional<MessageFacts> facts_of_email(const EmailFile &email) {
	if (email.is_unsent == true || one_line(email.message_id, 1000).empty() == true) return std::nullopt;
	MessageFacts facts;
	facts.internet_message_id = one_line(email.message_id, 1000);
	facts.conversation_id = one_line(conversation_of(email), 1000);
	facts.subject = email.subject;
	const std::vector<Party> from = parties_of(email.from);
	if (from.empty() == false) facts.from = from.front();
	facts.to = parties_of(email.to);
	facts.cc = parties_of(email.cc);
	facts.sent_at = moment_of(email.date);
	facts.text = email.text;
	for (const auto &[name, size] : email.attachments) {
		LetterAttachment attachment;
		attachment.name = name;
		attachment.size = size;
		facts.attachments.push_back(std::move(attachment));
	}
	return facts;
}

} // namespace singularity::correspondence
