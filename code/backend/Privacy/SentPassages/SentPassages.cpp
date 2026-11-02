#include "Privacy/SentPassages/SentPassages.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Platform/TextFold/TextFold.hpp"
#include "Routing/ProtectedInference/SourceMaterialSpans.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace singularity::privacy::sent_passages {
namespace {

// The longest run of words one marker is taken to have hidden: an address, an
// amount written out in words. Longer than this, a reading is not tried.
constexpr std::size_t kLongestOriginal = 1024;
// A passage whose markers stand side by side can be read too many ways to
// weigh them all; past this many weighings it is said to be ambiguous.
constexpr std::size_t kMostWeighings = 2000000;
// How well a reading must match a known spelling of its value to settle a
// marker that another reading of the passage reads differently: the same
// words, or each word a form of one of the value's words.
constexpr int kSupported = 70;

bool is_marker_letter(char letter) {
	return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9')
		|| letter == '_';
}

// Whether `at` starts a character of `text` (never the middle of a UTF-8 one).
bool is_character_start(const std::string &text, std::size_t at) {
	return at >= text.size() || (static_cast<unsigned char>(text[at]) & 0xC0) != 0x80;
}

bool is_word_break(unsigned char letter) {
	return letter == ' ' || letter == '\t' || letter == '\n' || letter == '\r' || letter == ',' || letter == '.'
		|| letter == ';' || letter == ':' || letter == '(' || letter == ')' || letter == '"' || letter == '\''
		|| letter == '/' || letter == '-' || letter == '[' || letter == ']';
}

bool is_blank_edge(const std::string &text, std::size_t from, std::size_t to, bool isStart) {
	if (from >= to) return false;
	const unsigned char letter = static_cast<unsigned char>(isStart == true ? text[from] : text[to - 1]);
	if (letter == ' ' || letter == '\t' || letter == '\n' || letter == '\r') return true;
	// A no-break space (U+00A0) is written C2 A0.
	if (isStart == true) return to - from >= 2 && static_cast<unsigned char>(text[from]) == 0xC2
		&& static_cast<unsigned char>(text[from + 1]) == 0xA0;
	return to - from >= 2 && static_cast<unsigned char>(text[to - 2]) == 0xC2 && static_cast<unsigned char>(text[to - 1]) == 0xA0;
}

std::vector<std::string> words_of(const std::string &folded) {
	std::vector<std::string> words;
	std::string word;
	for (const char letter : folded) {
		if (is_word_break(static_cast<unsigned char>(letter)) == true) {
			if (word.empty() == false) words.push_back(word);
			word.clear();
			continue;
		}
		word += letter;
	}
	if (word.empty() == false) words.push_back(word);
	return words;
}

// Two written forms of one word: the same, or - a name declined, a plural -
// sharing most of their opening (at least three bytes, and three fifths of the
// longer one). No language is assumed: a changed ending is a changed ending in
// any script.
bool is_same_word(const std::string &left, const std::string &right) {
	if (left == right) return true;
	std::size_t common = 0;
	while (common < left.size() && common < right.size() && left[common] == right[common]) ++common;
	const std::size_t longest = std::max(left.size(), right.size());
	return common >= 3 && common * 5 >= longest * 3;
}

struct Spelling {
	std::string folded;
	std::vector<std::string> words;
};

// How well `folded` reads as one of a value's known spellings: the same words
// (100), each word a form of one of its words, as many (85) or fewer (70), or
// some of its words (up to 40).
int likeness(const std::string &folded, const std::vector<Spelling> &spellings) {
	if (spellings.empty() == true) return 0;
	const std::vector<std::string> words = words_of(folded);
	int best = 0;
	for (const Spelling &spelling : spellings) {
		if (folded == spelling.folded) return 100;
		if (words.empty() == true || spelling.words.empty() == true) continue;
		std::size_t matched = 0;
		for (const std::string &word : words) {
			for (const std::string &known : spelling.words) {
				if (is_same_word(word, known) == false) continue;
				++matched;
				break;
			}
		}
		int value = 0;
		if (matched == words.size()) value = words.size() == spelling.words.size() ? 85 : 70;
		else value = static_cast<int>(40 * matched / words.size());
		best = std::max(best, value);
	}
	return best;
}

// Everything an alignment knows about each hidden run of the sent form: its
// marker's known spellings.
struct Weigher {
	const std::string &local;
	std::vector<std::vector<Spelling>> spellings;

	int weigh(std::size_t hiddenIndex, std::size_t from, std::size_t to) const {
		int value = 0;
		// A value is written without the space around it: a reading that takes
		// the space in is the worse one.
		if (is_blank_edge(local, from, to, true) == true) value -= 20;
		if (is_blank_edge(local, from, to, false) == true) value -= 20;
		const std::vector<Spelling> &known = spellings.at(hiddenIndex);
		if (known.empty() == true) return value;
		const std::size_t length = to - from;
		bool isNear = false;
		for (const Spelling &spelling : known)
			if (length <= spelling.folded.size() * 2 + 16 && spelling.folded.size() <= length * 3 + 16) isNear = true;
		if (isNear == false) return value;
		return value + likeness(platform::folded_text(local.substr(from, length)), known);
	}

	// How well the words from `from` to `to` read as a known spelling of the
	// value: 0 when none is known.
	int support(std::size_t hiddenIndex, std::size_t from, std::size_t to) const {
		const std::vector<Spelling> &known = spellings.at(hiddenIndex);
		if (known.empty() == true || to <= from) return 0;
		return likeness(platform::folded_text(local.substr(from, to - from)), known);
	}
};

// One reading step: the words from `from` to `to` for one marker.
struct Edge {
	std::size_t from = 0;
	std::size_t to = 0;
	std::size_t reached = 0;
};

// One way of reaching a place in the local text: its weight so far, and the
// way it came (the place before, and which of that place's two ways).
struct Way {
	long long weight = 0;
	std::size_t previous = 0;
	int previousRank = -1;
	std::size_t from = 0;
	std::size_t to = 0;
};

// The two best ways to one place: two, so that a tie between the best readings
// of a whole passage is seen.
struct Place {
	Way ways[2];
	int count = 0;

	void offer(const Way &way) {
		if (count < 2) {
			ways[count] = way;
			++count;
		} else if (way.weight > ways[1].weight) {
			ways[1] = way;
		} else {
			return;
		}
		if (count == 2 && ways[1].weight > ways[0].weight) std::swap(ways[0], ways[1]);
	}
};

} // namespace

std::string base_marker(const std::string &written) {
	if (written.size() < 5 || written.compare(0, 2, "[[") != 0 || written.compare(written.size() - 2, 2, "]]") != 0)
		return written;
	const std::string inside = written.substr(2, written.size() - 4);
	const std::size_t part = inside.find('.');
	const std::string name = part == std::string::npos ? inside : inside.substr(0, part);
	if (name.empty() == true) return written;
	for (const char letter : name)
		if (is_marker_letter(letter) == false) return written;
	return "[[" + name + "]]";
}

std::string category_noun(const std::string &category) {
	static const std::map<std::string, std::string> nouns = {
		{"PERSON", "Person"},
		{"LEGAL_ENTITY", "Company"},
		{"ORGANISATION", "Organisation"},
		{"LOCATION", "Location"},
		{"ADDRESS", "Address"},
		{"EMAIL", "Email"},
		{"PHONE", "Phone number"},
		{"BANK_ACCOUNT", "Bank account"},
		{"TAX_ID", "Tax ID"},
		{"CASE_NUMBER", "Case number"},
		{"CONTRACT_IDENTIFIER", "Contract number"},
		{"COURT", "Court"},
		{"JUDGE", "Judge"},
		{"DATE", "Date"},
		{"MONEY_AMOUNT", "Amount"},
		{"PERCENTAGE", "Percentage"},
		{"NUMBER", "Number"},
		{"CUSTOM_TERM", "Custom term"},
		{"URL", "Web address"},
	};
	const auto found = nouns.find(category);
	return found == nouns.end() ? "Value" : found->second;
}

// How a typed marker names itself to a lawyer: "[[Amount7]]" is "Amount 7",
// "[[Org1]]" "Org 1"; a compact "[[E17]]" names nothing ("").
static std::string own_label(const std::string &marker) {
	if (marker.size() < 5) return {};
	const std::string name = marker.substr(2, marker.size() - 4);
	std::size_t letters = 0;
	while (letters < name.size() && std::isalpha(static_cast<unsigned char>(name[letters])) != 0) ++letters;
	if (letters < 2 || letters == name.size()) return {};
	for (std::size_t index = letters; index < name.size(); ++index)
		if (std::isdigit(static_cast<unsigned char>(name[index])) == 0) return {};
	return name.substr(0, letters) + " " + name.substr(letters);
}

std::vector<MarkerFact> marker_facts(const std::vector<protected_api::papinian::OutboundCall> &calls) {
	std::vector<MarkerFact> facts;
	std::set<std::pair<std::string, std::string>> seen;
	std::map<std::string, std::size_t> numbers;
	// One label per value: a Project entity is one value whichever table sent
	// it; a marker with no entity is a value of its own table only. A typed
	// marker is labelled by its own words ("Amount 7") - what the lawyer reads
	// beside "The model saw [[Amount7]]" - unless another value already has
	// that label (the same marker of another table).
	std::map<std::string, std::string> labels;
	std::set<std::string> used;
	for (const protected_api::papinian::OutboundCall &call : calls) {
		if (call.kind != "model") continue;
		for (const protected_api::papinian::OutboundMarker &sent : call.markers) {
			const std::string marker = base_marker(sent.marker);
			if (marker.empty() == true || seen.insert({call.symbol_scope, marker}).second == false) continue;
			MarkerFact fact;
			fact.scope = call.symbol_scope;
			fact.marker = marker;
			fact.category = sent.category;
			fact.entity_id = sent.project_entity_id;
			const std::string noun = category_noun(sent.category);
			const std::string identity = fact.entity_id.empty() == false
				? "entity\n" + noun + "\n" + fact.entity_id : "marker\n" + fact.scope + "\n" + marker;
			const auto known = labels.find(identity);
			if (known != labels.end()) {
				fact.label = known->second;
			} else {
				fact.label = own_label(marker);
				if (fact.label.empty() == true || used.count(fact.label) > 0)
					do fact.label = noun + " " + std::to_string(++numbers[noun]);
					while (used.count(fact.label) > 0);
				used.insert(fact.label);
				labels.emplace(identity, fact.label);
			}
			facts.push_back(std::move(fact));
		}
	}
	return facts;
}

std::vector<MarkerFact> facts_in_scope(const std::vector<MarkerFact> &facts, const std::string &scope) {
	std::vector<MarkerFact> scoped;
	for (const MarkerFact &fact : facts)
		if (fact.scope == scope) scoped.push_back(fact);
	return scoped;
}

const MarkerFact *find_fact(const std::vector<MarkerFact> &facts, const std::string &marker) {
	for (const MarkerFact &fact : facts)
		if (fact.marker == marker) return &fact;
	return nullptr;
}

std::vector<Segment> sent_segments(const std::string &sent, const std::vector<MarkerFact> &facts) {
	std::vector<Segment> segments;
	std::string kept;
	std::size_t cursor = 0;
	while (cursor < sent.size()) {
		const std::size_t opened = sent.find("[[", cursor);
		if (opened == std::string::npos) break;
		const std::size_t closed = sent.find("]]", opened + 2);
		if (closed == std::string::npos) break;
		const std::string written = sent.substr(opened, closed + 2 - opened);
		const std::string marker = base_marker(written);
		const MarkerFact *fact = find_fact(facts, marker);
		if (fact == nullptr) {
			kept += sent.substr(cursor, opened + 2 - cursor);
			cursor = opened + 2;
			continue;
		}
		kept += sent.substr(cursor, opened - cursor);
		if (kept.empty() == false) segments.push_back({kept, false, "", "", "", ""});
		kept.clear();
		segments.push_back({written, true, written, marker, fact->label, fact->category});
		cursor = closed + 2;
	}
	kept += sent.substr(std::min(cursor, sent.size()));
	if (kept.empty() == false) segments.push_back({kept, false, "", "", "", ""});
	return segments;
}

Alignment align(const std::string &local, const std::string &sent, const std::vector<MarkerFact> &facts) {

	Alignment result;
	const std::vector<Segment> segments = sent_segments(sent, facts);

	// The sent form as kept runs around its markers: kept[0] marker kept[1] ...
	// marker kept[n]. Two markers side by side have an empty kept run between.
	std::vector<std::string> kept(1);
	std::vector<const Segment *> hidden;
	for (const Segment &segment : segments) {
		if (segment.is_hidden == true) {
			hidden.push_back(&segment);
			kept.emplace_back();
			continue;
		}
		kept.back() += segment.text;
	}
	const std::size_t count = hidden.size();
	if (count == 0) {
		if (local != sent) return result;
		result.fit = Fit::Aligned;
		if (local.empty() == false) result.segments.push_back({local, false, "", "", "", ""});
		return result;
	}
	if (local.size() < kept.front().size() + kept.back().size() + count) return result;
	if (local.compare(0, kept.front().size(), kept.front()) != 0) return result;
	if (local.compare(local.size() - kept.back().size(), kept.back().size(), kept.back()) != 0) return result;

	Weigher weigher{local, {}};
	for (const Segment *segment : hidden) {
		std::vector<Spelling> known;
		const MarkerFact *fact = find_fact(facts, segment->marker);
		if (fact != nullptr) {
			std::set<std::string> seen;
			std::vector<std::string> spellings = fact->aliases;
			if (fact->entity_name.empty() == false) spellings.push_back(fact->entity_name);
			for (const std::string &spelling : spellings) {
				const std::string folded = platform::folded_text(spelling);
				if (folded.empty() == true || seen.insert(folded).second == false) continue;
				known.push_back({folded, words_of(folded)});
			}
		}
		weigher.spellings.push_back(std::move(known));
	}

	/* - - - - - - */

	// layers[i]: every place in the local text right after kept[i] that a
	// reading of the first i markers reaches, with its two best ways.
	std::vector<std::map<std::size_t, Place>> layers(count + 1);
	// edges[i]: every step a reading of marker i can take.
	std::vector<std::vector<Edge>> edges(count + 1);
	layers[0][kept.front().size()].offer(Way{0, 0, -1, 0, 0});
	const std::size_t lastStart = local.size() - kept.back().size();
	std::size_t weighings = 0;
	for (std::size_t index = 1; index <= count; ++index) {
		const std::string &next = kept[index];
		const bool isLast = index == count;
		for (const auto &[from, place] : layers[index - 1]) {
			std::vector<std::size_t> ends;
			if (isLast == true) {
				if (lastStart > from && lastStart - from <= kLongestOriginal) ends.push_back(lastStart);
			} else if (next.empty() == true) {
				// Side by side: the first marker's words end anywhere a character starts.
				for (std::size_t to = from + 1; to <= from + kLongestOriginal && to < lastStart; ++to)
					if (is_character_start(local, to) == true) ends.push_back(to);
			} else {
				std::size_t at = local.find(next, from + 1);
				while (at != std::string::npos && at <= from + kLongestOriginal && at + next.size() <= lastStart) {
					ends.push_back(at);
					at = local.find(next, at + 1);
				}
			}
			for (const std::size_t to : ends) {
				weighings += static_cast<std::size_t>(place.count);
				if (weighings > kMostWeighings) {
					result.fit = Fit::Ambiguous;
					return result;
				}
				const int weight = weigher.weigh(index - 1, from, to);
				const std::size_t reached = isLast == true ? local.size() : to + next.size();
				edges[index].push_back(Edge{from, to, reached});
				for (int rank = 0; rank < place.count; ++rank)
					layers[index][reached].offer(Way{place.ways[rank].weight + weight, from, rank, from, to});
			}
		}
		if (layers[index].empty() == true) return result;
	}

	const auto end = layers[count].find(local.size());
	if (end == layers[count].end()) return result;
	const Place &finished = end->second;
	if (finished.count > 1 && finished.ways[1].weight >= finished.ways[0].weight) {
		result.fit = Fit::Ambiguous;
		return result;
	}

	// Which markers every complete reading reads the same: back from the end,
	// the steps that lie on some complete reading. A marker read two ways is
	// settled only by a known spelling of its value (kSupported) - a better
	// score won on the other side of it proves nothing about it.
	std::vector<bool> isSettled(count + 1, true);
	{
		std::set<std::size_t> alive = {local.size()};
		for (std::size_t index = count; index >= 1; --index) {
			std::set<std::size_t> before;
			bool isFirst = true;
			std::size_t from = 0;
			std::size_t to = 0;
			for (const Edge &edge : edges[index]) {
				if (alive.count(edge.reached) == 0) continue;
				before.insert(edge.from);
				if (isFirst == true) {
					from = edge.from;
					to = edge.to;
					isFirst = false;
				} else if (edge.from != from || edge.to != to) {
					isSettled[index] = false;
				}
			}
			alive = std::move(before);
		}
	}

	/* - - - - - - */

	// Back from the end along the best way: the words each marker hid.
	std::vector<std::pair<std::size_t, std::size_t>> originals(count);
	std::size_t place = local.size();
	int rank = 0;
	for (std::size_t index = count; index >= 1; --index) {
		const Way &way = layers[index].at(place).ways[rank];
		originals[index - 1] = {way.from, way.to};
		place = way.previous;
		rank = way.previousRank;
	}
	for (std::size_t index = 1; index <= count; ++index) {
		if (isSettled[index] == true) continue;
		if (weigher.support(index - 1, originals[index - 1].first, originals[index - 1].second) >= kSupported) continue;
		result.fit = Fit::Ambiguous;
		return result;
	}
	if (kept.front().empty() == false) result.segments.push_back({kept.front(), false, "", "", "", ""});
	for (std::size_t index = 0; index < count; ++index) {
		const std::string original = local.substr(originals[index].first, originals[index].second - originals[index].first);
		result.segments.push_back({original, true, hidden[index]->sent_as, hidden[index]->marker, hidden[index]->label,
			hidden[index]->category});
		if (kept[index + 1].empty() == false) result.segments.push_back({kept[index + 1], false, "", "", "", ""});
	}
	result.fit = Fit::Aligned;
	return result;

}

std::optional<std::string> built_from(const std::string &content_sha256, const std::string &local) {
	if (content_sha256.empty() == true) return std::nullopt;
	if (platform::sha256_hex_digest(local) == content_sha256) return local;
	const std::string escaped = routing::source_material::as_prompt_text(local);
	if (escaped != local && platform::sha256_hex_digest(escaped) == content_sha256) return escaped;
	return std::nullopt;
}

std::vector<HiddenValue> fact_values(const std::vector<MarkerFact> &facts) {
	std::vector<HiddenValue> values;
	std::set<std::string> labels;
	for (const MarkerFact &fact : facts) {
		if (labels.insert(fact.label).second == false) {
			for (HiddenValue &value : values)
				if (value.label == fact.label && std::find(value.sent_as.begin(), value.sent_as.end(), fact.marker) == value.sent_as.end())
					value.sent_as.push_back(fact.marker);
			continue;
		}
		HiddenValue value;
		value.marker = fact.marker;
		value.category = fact.category;
		value.label = fact.label;
		value.entity_id = fact.entity_id;
		value.entity_name = fact.entity_name;
		value.sent_as.push_back(fact.marker);
		values.push_back(std::move(value));
	}
	return values;
}

std::vector<HiddenValue> hidden_values(const std::vector<Segment> &sent, const std::vector<Segment> &aligned,
		const std::vector<MarkerFact> &facts) {
	std::vector<const Segment *> originals;
	for (const Segment &segment : aligned)
		if (segment.is_hidden == true) originals.push_back(&segment);
	std::vector<HiddenValue> values;
	std::size_t hiddenIndex = 0;
	for (const Segment &segment : sent) {
		if (segment.is_hidden == false) continue;
		const std::size_t index = hiddenIndex++;
		// One value is one label (two markers of one Project entity are one).
		auto found = std::find_if(values.begin(), values.end(), [&](const HiddenValue &value) {
			return segment.label.empty() == false ? value.label == segment.label : value.marker == segment.marker;
		});
		if (found == values.end()) {
			HiddenValue value;
			value.marker = segment.marker;
			const MarkerFact *fact = find_fact(facts, segment.marker);
			if (fact != nullptr) {
				value.category = fact->category;
				value.label = fact->label;
				value.entity_id = fact->entity_id;
				value.entity_name = fact->entity_name;
			}
			values.push_back(std::move(value));
			found = values.end() - 1;
		}
		if (std::find(found->sent_as.begin(), found->sent_as.end(), segment.sent_as) == found->sent_as.end())
			found->sent_as.push_back(segment.sent_as);
		if (index < originals.size() && originals[index]->marker == segment.marker) {
			const std::string &original = originals[index]->text;
			if (std::find(found->originals.begin(), found->originals.end(), original) == found->originals.end())
				found->originals.push_back(original);
		}
	}
	return values;
}

void merge_hidden_values(std::vector<HiddenValue> &into, const std::vector<HiddenValue> &more) {
	// One value is one label; a value with none (a marker no call named) is
	// its marker.
	const auto sameValue = [](const HiddenValue &left, const HiddenValue &right) {
		return left.label.empty() == false || right.label.empty() == false ? left.label == right.label : left.marker == right.marker;
	};
	for (const HiddenValue &value : more) {
		auto found = std::find_if(into.begin(), into.end(), [&](const HiddenValue &entry) { return sameValue(entry, value); });
		if (found == into.end()) {
			into.push_back(value);
			continue;
		}
		for (const std::string &written : value.sent_as)
			if (std::find(found->sent_as.begin(), found->sent_as.end(), written) == found->sent_as.end())
				found->sent_as.push_back(written);
		for (const std::string &original : value.originals)
			if (std::find(found->originals.begin(), found->originals.end(), original) == found->originals.end())
				found->originals.push_back(original);
	}
}

void learn_spellings(std::vector<MarkerFact> &facts, const std::vector<HiddenValue> &values) {
	for (const HiddenValue &value : values) {
		for (MarkerFact &fact : facts) {
			if (value.label.empty() == false ? fact.label != value.label : fact.marker != value.marker) continue;
			for (const std::string &original : value.originals)
				if (std::find(fact.aliases.begin(), fact.aliases.end(), original) == fact.aliases.end())
					fact.aliases.push_back(original);
		}
	}
}

const char *to_string(Reading reading) {
	switch (reading) {
		case Reading::Aligned:
			return "aligned";
		case Reading::Ambiguous:
			return "ambiguous";
		case Reading::Changed:
			return "changed";
		case Reading::Unaligned:
			return "unaligned";
		case Reading::Unavailable:
			return "unavailable";
	}
	return "unavailable";
}

PassageReading read_passage(const std::string &sent, const std::string &content_sha256,
		const std::optional<std::string> &local, const std::vector<MarkerFact> &facts) {
	PassageReading reading;
	reading.sent = sent_segments(sent, facts);
	if (local.has_value() == false) {
		reading.reading = Reading::Unavailable;
		reading.hidden = hidden_values(reading.sent, {}, facts);
		return reading;
	}
	const std::optional<std::string> built = built_from(content_sha256, *local);
	if (built.has_value() == false) {
		reading.reading = Reading::Changed;
		reading.hidden = hidden_values(reading.sent, {}, facts);
		return reading;
	}
	Alignment alignment = align(*built, sent, facts);
	if (alignment.fit == Fit::Aligned) {
		reading.reading = Reading::Aligned;
		reading.local = std::move(alignment.segments);
		reading.hidden = hidden_values(reading.sent, reading.local, facts);
		return reading;
	}
	reading.reading = alignment.fit == Fit::Ambiguous ? Reading::Ambiguous : Reading::Unaligned;
	reading.hidden = hidden_values(reading.sent, {}, facts);
	return reading;
}

} // namespace singularity::privacy::sent_passages
