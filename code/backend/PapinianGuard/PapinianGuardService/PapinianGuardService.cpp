#include "Platform/Trace/Trace.hpp"
#include "Platform/TextFold/TextFold.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"

#include "PapinianGuard/CommonWords/CommonWords.hpp"
#include "PapinianGuard/PatternScanner/PatternScanner.hpp"

#include "Context/RussianNames/RussianNames.hpp"
#include "Context/TemporalSemantics/TemporalSemantics.hpp"
#include "PapinianGuard/DeterministicDetector/DeterministicDetector.hpp"
#include "PapinianGuard/ProtectedMarkers/ProtectedMarkers.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "PapinianGuard/RestorationGrammar/RestorationGrammar.hpp"
#include "PapinianGuard/RestorationGrammar/SpelledFigures.hpp"
#include "PapinianGuard/SemanticRoles/RoleLexicon.hpp"
#include "PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.hpp"
#include "Routing/Observability/OperationEvent.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <ctime>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace singularity::protected_api::papinian {

namespace temporal = singularity::context::temporal;
namespace russian_names = singularity::context::russian_names;

// A quantity as a text writes it - "$2.8 million", "0,1 %", "18 400 000
// (восемнадцать миллионов) рублей", a workbook's bare "18400000" - found by
// its figures and read for its value only when a value this operation holds
// could be it.
struct WrittenQuantity {
	std::size_t start = 0;
	std::size_t length = 0;
	// The significant figures of its number, "184" for 18 400 000 and for 18.4
	// million, "1" for 0,1 % and for 0.001: the cheap first test.
	std::string figures;
	// The workbook reading's note right after it, " (shown in ₽)", read with it.
	std::string note;
	// A currency, a scale, a percent or the amount in words is written with the
	// number, so it is a value wherever it stands.
	bool marked = false;
	// A percent sign or word, or a note that shows it as a percentage.
	bool percent = false;
	// A bare number its surroundings make structure or a part of a date, or a
	// formula's constant - read only for a number that could be a held value.
	bool structure = false;
	bool structure_read = false;
	std::size_t number_start = 0;
	std::size_t number_end = 0;
	std::size_t digits = 0;
	bool decimals = false;
	// Written with cents: two figures after its last separator, "61300.00".
	bool cents = false;
	bool read = false;
	std::optional<semantic::Value> value;
	std::string key;
	// "7,375 %" is 7.375 % the Russian way and 7375 % the English way, and
	// restoration writes the Russian way into Russian prose: the other reading
	// of a number with one comma before three figures, which a held value may
	// be. Empty when the number reads one way only.
	std::optional<semantic::Value> other_value;
	std::string other_key;
};

// A date a text writes to the day or the month, with the year.
struct WrittenDate {
	std::size_t start = 0;
	std::size_t length = 0;
	std::string key;
};

// One amount, rate, count, date or period of a text, and the fact of the
// material it was found to be (the fact registry, DESIGN3 9.3): the row the
// transform writes its symbol from. Recorded by the collection that read the
// same bytes, so the transform never decides identity by value.
struct FactPlace {
	std::size_t start = 0;
	std::size_t length = 0;
	std::size_t row = 0;
};

struct WrittenText {
	std::vector<WrittenQuantity> quantities;
	bool dates_read = false;
	std::vector<WrittenDate> dates;
	// Days of the year written with no year ("July 1", «1 июля»), keyed "DOY:0701".
	bool days_read = false;
	std::vector<WrittenDate> days;
	// Each quantity or date the collection attributed to a fact, in text order.
	std::vector<FactPlace> facts;
};

// The rows of a table by the values they stand for (value_rows() below).
struct ValueRows {
	std::map<std::string, std::size_t> by_key;
	// Money rows by their amount alone: a workbook's bare number is the amount
	// its column is in, and "18 400 000 рублей" is the row a bare cell began.
	std::map<std::string, std::vector<std::size_t>> money_by_amount;
	std::set<std::string> figures;
	bool quantities = false;
	bool dates = false;
	bool days_of_year = false;
};

struct WrittenValueCache {
	std::map<std::string, WrittenText> texts;
	// The value rows last derived, and the table state they were derived from.
	ValueRows rows;
	std::size_t rows_signature = static_cast<std::size_t>(-1);
};

namespace {

namespace obs = singularity::routing::observability;

// A count or a period Singularity worked out itself (ProtectedRelations): "9", "54
// days". It stands for no writing of the lawyer's material, so no number the
// text writes is that row, and none is looked for as its leak - a small number
// standing anywhere would otherwise become a symbol, or block a send.
//
// DESIGN4 §4.2 splits what this means: such a row is kept out of detection by
// its value's writing in fresh material (value_rows, the identity scan), and
// out of the bare-number leak search - but its writing with its unit or noun
// is still looked for as its leak (derived_writings, validate_masking). A
// count (Kind::Count) and a yes/no (Kind::Boolean) are Singularity's own too.
bool counted_by_singularity(const RequestMappingEntry &entry) {
	return entry.derived && entry.value
		&& (entry.open_value || entry.value->kind == semantic::Kind::Number || entry.value->kind == semantic::Kind::Duration
			|| entry.value->kind == semantic::Kind::Count || entry.value->kind == semantic::Kind::Boolean);
}

// The writings of a derived count or period that say what they are: every
// writing the row has met that carries a word ("42 days", "3 invoices" as a
// restoration wrote it), and a period's own display in English and Russian.
// Never a bare number, which could be anything; never a yes/no.
std::vector<std::string> derived_writings(const RequestMappingEntry &entry) {
	std::vector<std::string> forms;
	if (!entry.value || entry.value->kind == semantic::Kind::Boolean) return forms;
	const auto hasWord = [](const std::string &form) {
		return std::any_of(form.begin(), form.end(), [](char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || static_cast<unsigned char>(c) >= 0xD0;
		}) && std::any_of(form.begin(), form.end(), [](char c) { return c >= '0' && c <= '9'; });
	};
	for (const std::string &form : entry.every_form())
		if (hasWord(form) && std::find(forms.begin(), forms.end(), form) == forms.end()) forms.push_back(form);
	if (entry.value->kind == semantic::Kind::Duration)
		for (const semantic::Language language : {semantic::Language::English, semantic::Language::Russian}) {
			try {
				const std::string shown = semantic::display(*entry.value, {}, language);
				if (hasWord(shown) && std::find(forms.begin(), forms.end(), shown) == forms.end()) forms.push_back(shown);
			} catch (const semantic::AlgebraError &) {
			}
		}
	return forms;
}

// A day of the year written in figures ("02.07", "3/4") is that day only where
// a word dates it (yearless_day): "3/4 of the price" is a fraction, even once
// "on 3/4" has made the writing a row.
bool yearless_writing_stands(const RequestMappingEntry &entry, const std::string &form, const std::string &text,
		std::size_t position, std::size_t length) {
	// Only a day Singularity read itself by its dating word: one the local model
	// found is a date wherever it stands, as the model found it.
	if (entry.source != DetectionSource::Deterministic) return true;
	const bool dayOfYear = entry.value && entry.value->kind == semantic::Kind::DayOfYear;
	if (!dayOfYear && entry.type != EntityType::Date) return true;
	if (form.size() > 5 || std::count_if(form.begin(), form.end(), [](char character) {
			return character == '.' || character == '/'; }) != 1
			|| std::any_of(form.begin(), form.end(), [](char character) {
				return !(character >= '0' && character <= '9') && character != '.' && character != '/'; }))
		return true;
	return yearless_day(text, position, length).has_value();
}

// A text as it reads. A passage can carry JSON text still escaped - a restored
// proposal reproduced in a correction was: "Party:\nNorthwind Capital LLC",
// the backslash and the letter n written out - and then the byte before every
// value on a new line is the letter "n": the company, the name, the date and
// the amount after it stood as no word and no number, and went out raw while
// the final check, reading the same way, passed. Every escape is read in place
// as what it stands for, two bytes for two bytes ("\n" becomes a space and a
// line break), so offsets into the view are offsets into the text, and every
// judgment of what stands where is made on the view. Text without a backslash
// is its own view.
//
// Escaped twice - a continuation carried inside another JSON string - the
// line break is written "\\n": an escaped backslash, then the letter n, and
// read one level deep the byte before every value was still that letter (a
// verifier's run, 25 Sep 2026: the amount, the company, the date and the
// ruble amount all left raw). However many backslashes stand before an escape
// letter, they are read as the one escape they were once: a stray "\\n" in a
// path costs a word boundary, never a value.
std::string reading_view(const std::string &text) {
	if (text.find('\\') == std::string::npos) return text;
	std::string view = text;
	for (std::size_t index = 0; index + 1 < view.size(); ++index) {
		if (text[index] != '\\') continue;
		std::size_t run = index;
		while (run < text.size() && text[run] == '\\') ++run;
		if (run >= text.size()) break;
		const char escaped = text[run];
		char reads = 0;
		switch (escaped) {
			case 'n': reads = '\n'; break;
			case 'r': reads = '\r'; break;
			case 't': reads = '\t'; break;
			case 'b':
			case 'f': reads = ' '; break;
			case '"': reads = '"'; break;
			case '/': reads = '/'; break;
			default: break;
		}
		// Backslashes before any other character stay as they are.
		if (reads == 0) {
			index = run - 1;
			continue;
		}
		for (std::size_t slash = index; slash < run; ++slash) view[slash] = ' ';
		view[run] = reads;
		index = run;
	}
	return view;
}

bool ranges_overlap(std::size_t left_start, std::size_t left_length, std::size_t right_start,
                    std::size_t right_length) {
  return left_start < right_start + right_length && right_start < left_start + left_length;
}

int source_priority(DetectionSource source) {
  switch (source) {
    case DetectionSource::CustomTerm:
      return 0;
    case DetectionSource::Deterministic:
      return 1;
    case DetectionSource::SemanticModel:
      return 2;
  }
  return 2;
}

int entity_specificity(const CanonicalEntity &entity) {
  return entity.type ? semantic_specificity(*entity.type) : 4;
}

bool valid_namespace(const std::string &value) {
  if (value.size() < 8 || value.size() > 32) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 128 && std::isalnum(character) != 0;
  });
}

std::optional<EntityType> deterministic_type(const Match &match, const std::string &text) {
  if (match.label == singularity::protected_api::to_string(Category::MonetaryAmounts)) {
    return EntityType::MoneyAmount;
  }
  if (match.label == singularity::protected_api::to_string(Category::Emails)) return EntityType::Email;
  if (match.label == singularity::protected_api::to_string(Category::PhoneNumbers)) {
    return EntityType::Phone;
  }
  // The shapes found by form rather than by category: without a kind, the
  // model's legend called a street address a "protected value" and the
  // preview a URL.
  if (match.label == address_label) return EntityType::Address;
  if (match.label == person_label) return EntityType::Person;
  if (match.label == percentage_label) return EntityType::Percentage;
  // A party's holding - its shares or units - is the plain quantity it is.
  if (match.label == holding_label) return EntityType::Number;
  if (match.label != singularity::protected_api::to_string(Category::Identifiers)) return std::nullopt;

  const bool digits_only = std::all_of(match.value.begin(), match.value.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
  if (digits_only && match.value.size() == 20) return EntityType::BankAccount;
  if (digits_only && (match.value.size() == 10 || match.value.size() == 12)) {
    return EntityType::TaxId;
  }

  const auto context_start = match.start > 24 ? match.start - 24 : 0;
  auto context = text.substr(context_start, match.start - context_start);
  std::transform(context.begin(), context.end(), context.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (context.find("case") != std::string::npos || context.find("docket") != std::string::npos) {
    return EntityType::CaseNumber;
  }
  // "по делу № А40-12345/2026", "Дело № 2-123/2026": the case's number.
  for (const char *caseWord : {"\xd0\xb4\xd0\xb5\xd0\xbb\xd0\xbe", "\xd0\xb4\xd0\xb5\xd0\xbb\xd0\xb0",
         "\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x83", "\xd0\x94\xd0\xb5\xd0\xbb\xd0\xbe", "\xd0\x94\xd0\xb5\xd0\xbb\xd0\xb0"}) {
    const std::size_t found = context.rfind(caseWord);
    if (found != std::string::npos && (found == 0 || context[found - 1] == ' ' || context[found - 1] == '('))
      return EntityType::CaseNumber;
  }
  return EntityType::ContractIdentifier;
}

std::vector<CanonicalEntity> deterministic_dates(const std::string &text, const std::vector<CanonicalEntity> &taken);

// A deal's code name - "Project Tern", "PROJECT LANTERN" - names the deal it
// hides, and the local model has no kind for it: a held-out engagement letter
// (#245) sent "Project Tern" out three times. "Project" and one or two
// capitalised words after it, the first never one of a project's ordinary
// compounds ("Project Manager", "Project Schedule"). Typed as an identifier.
std::vector<CanonicalEntity> deterministic_code_names(const std::string &text) {
	static const std::set<std::string> compounds{"manager", "managers", "management", "plan", "plans", "planning",
		"schedule", "schedules", "budget", "budgets", "team", "teams", "lead", "leader", "leaders", "director",
		"coordinator", "owner", "sponsor", "sponsors", "site", "sites", "area", "areas", "document", "documents",
		"documentation", "agreement", "agreements", "contract", "contracts", "scope", "description", "deliverable",
		"deliverables", "milestone", "milestones", "completion", "delivery", "fee", "fees", "cost", "costs", "account",
		"accounts", "number", "no", "name", "office", "phase", "phases", "period", "term", "timeline", "proposal",
		"charter", "summary", "overview", "report", "reports", "status", "update", "updates", "review", "results",
		"work", "works", "services", "materials", "equipment", "property", "land", "lands", "assets", "financing",
		"lender", "lenders", "company", "entity", "entities", "partner", "partners", "data", "information", "funds",
		"fund", "capital", "revenue", "revenues", "approval", "approvals", "requirements", "specification",
		"specifications", "staff", "personnel", "members", "participants", "stakeholders", "risk", "risks", "engineer",
		"engineering", "architect", "design", "development", "developer", "implementation", "execution", "kickoff",
		"start", "end", "closing", "close", "date", "dates", "year", "years", "month", "months", "quarter", "week",
		"day", "meeting", "meetings", "committee", "board", "steering", "governance", "labor", "location", "executive",
		"officer", "counsel", "attorney", "assistant", "analyst", "associate", "administrator", "accountant",
		"controller", "finance", "funding", "grant", "grants", "award", "code", "codes", "file", "files", "folder",
		"room", "list", "lists", "log", "tracker", "task", "tasks", "goals", "objectives", "and", "or", "of", "for",
		"the", "a", "an", "to", "in", "on", "is", "was", "will", "may", "shall", "has", "had",
		// Singularity's own words for a Project's parts, which its own
		// instructions write: "Project Context", "Project Memory".
		"context", "memory", "instructions", "instruction", "access", "wide", "registry", "reasoning", "below",
		"above", "knowledge", "notes", "settings", "chat", "chats", "history", "journal", "sources", "source",
		"workspace", "setup", "guard", "protection", "privacy", "model", "models", "sharing", "share", "view",
		"page", "home", "table", "tables", "playbook", "playbooks", "agent", "agents", "colleague", "colleagues",
		"inbox", "matter", "matters", "people", "roles", "role", "rules", "rule", "evidence", "facts", "search",
		"products", "product", "outputs", "output"};
	const auto letters = [](const std::string &word) {
		return word.size() >= 2 && std::all_of(word.begin(), word.end(), [](char character) {
			return std::isalpha(static_cast<unsigned char>(character)) != 0;
		});
	};
	const auto lowered = [](std::string word) {
		for (char &character : word) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		return word;
	};
	std::vector<CanonicalEntity> found;
	for (const char *marker : {"Project ", "PROJECT "}) {
		const bool shouted = marker[1] == 'R';
		for (std::size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + 1)) {
			if (at > 0 && std::isalnum(static_cast<unsigned char>(text[at - 1])) != 0) continue;
			std::size_t end = at + std::char_traits<char>::length(marker);
			std::size_t words = 0;
			while (words < 2 && end < text.size()) {
				std::size_t wordEnd = end;
				while (wordEnd < text.size() && std::isalpha(static_cast<unsigned char>(text[wordEnd])) != 0) ++wordEnd;
				const std::string word = text.substr(end, wordEnd - end);
				const bool capitalised = letters(word) && word.front() >= 'A' && word.front() <= 'Z'
					&& (shouted ? std::all_of(word.begin(), word.end(), [](char c) { return c >= 'A' && c <= 'Z'; })
						: std::any_of(word.begin() + 1, word.end(), [](char c) { return c >= 'a' && c <= 'z'; }));
				if (!capitalised || compounds.count(lowered(word)) > 0) break;
				++words;
				end = wordEnd;
				if (end >= text.size() || text[end] != ' ') break;
				++end;
			}
			if (words == 0) continue;
			while (end > at && text[end - 1] == ' ') --end;
			found.push_back(CanonicalEntity{at, end - at, EntityType::ContractIdentifier, 1.0, DetectionSource::Deterministic,
				"code_name", text.substr(at, end - at), {}});
		}
	}
	return found;
}

std::vector<CanonicalEntity> deterministic_entities(const std::string &text, const Policy &policy) {
  std::vector<CanonicalEntity> result;
  for (const auto &match : singularity::protected_api::detect(text, policy)) {
    const bool custom = match.label == "custom_term";
    result.push_back(CanonicalEntity{
        match.start,
        match.length,
        custom ? std::nullopt : deterministic_type(match, text),
        1.0,
        custom ? DetectionSource::CustomTerm : DetectionSource::Deterministic,
        match.label,
        match.value,
        {},
    });
  }
  // Dates, whenever protection is on - as the model's dates are (Policy.hpp).
  if (policy.mode != Mode::Off) {
    std::vector<CanonicalEntity> dates = deterministic_dates(text, result);
    result.insert(result.end(), std::make_move_iterator(dates.begin()), std::make_move_iterator(dates.end()));
    std::vector<CanonicalEntity> codeNames = deterministic_code_names(text);
    result.insert(result.end(), std::make_move_iterator(codeNames.begin()), std::make_move_iterator(codeNames.end()));
  }
  return result;
}

// The GLiNER2 PII checkpoint (see Gliner2Adapter.cpp - a generic third-party
// PII filter, not a legal-semantics model) has no notion that a phrase
// naming a body of law in general is always public, never confidential -  a
// live Phase 24.1D OpenRouter acceptance run proved it tags the plain phrase
// "Russian law" as LEGAL_ENTITY, masking the very jurisdiction word a model
// needs to plan real legal research (it then has no way to know what law
// was meant, and researches nothing jurisdiction-specific). A semantic match
// ending in one of a small, closed set of legal-system reference words names
// a body of law in general, never a specific confidential person, company,
// case or place, so it is exempted here, before it ever becomes a masking
// candidate. This is deliberately narrower than, and independent of, every
// other detector: a Project-registered entity or a deterministic PII
// pattern (money/email/phone/tax id/case number) that happens to end in one
// of these words is still fully masked by known_project_entities()/
// deterministic_entities() in reconcile_into_mapping() below, neither of
// which this exemption touches.
bool is_public_jurisdiction(const std::string &value);

// Whether every word of `words` says which law a phrase names: an ordinary
// word of the language, a jurisdiction, or the adjective of a country or a
// legal tradition.
// The adjective of a country, a region or a legal tradition: "European",
// "Russian", "English" - public, never anyone's name.
bool is_nationality_adjective(const std::string &word) {
  static const std::set<std::string> adjectives{"russian", "english", "american", "british", "french", "german",
      "swiss", "italian", "spanish", "dutch", "irish", "scottish", "welsh", "canadian", "mexican", "chinese",
      "japanese", "indian", "israeli", "singaporean", "australian", "european", "roman", "islamic", "sharia",
      "jewish", "talmudic", "napoleonic", "brazilian", "korean", "swedish", "norwegian", "danish", "finnish",
      "belgian", "austrian", "polish", "ukrainian", "kazakh", "cypriot", "luxembourg", "emirati", "federal",
      "u.s.", "us", "anglo-american", "continental", "asian", "african", "latin", "nordic", "scandinavian",
      "middle eastern", "north american", "south american", "east asian", "pan-european"};
  std::string lowered = word;
  for (char &character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return adjectives.count(lowered) > 0;
}

bool names_which_law(const std::string &words) {
  std::vector<std::string> parts;
  std::string part;
  for (const char character : words + " ") {
    if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
      if (!part.empty()) parts.push_back(part);
      part.clear();
    } else {
      part.push_back(character);
    }
  }
  for (std::size_t index = 0; index < parts.size(); ++index) {
    std::string lowered = parts[index];
    for (char &character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    while (!lowered.empty() && (lowered.back() == ',' || lowered.back() == '\'')) lowered.pop_back();
    if (lowered.size() > 2 && lowered.compare(lowered.size() - 2, 2, "'s") == 0) lowered.erase(lowered.size() - 2);
    if (lowered == "the" || lowered == "of" || lowered == "and" || is_common_word(lowered)
        || is_nationality_adjective(lowered) || is_public_jurisdiction(parts[index]))
      continue;
    // A jurisdiction of two or three words: "New York", "North Carolina".
    bool joined = false;
    for (std::size_t span = 3; span >= 2 && !joined; --span) {
      if (index + span > parts.size()) continue;
      std::string phrase = parts[index];
      for (std::size_t next = index + 1; next < index + span; ++next) phrase += " " + parts[next];
      if (is_public_jurisdiction(phrase)) {
        index += span - 1;
        joined = true;
      }
    }
    if (!joined) return false;
  }
  return true;
}

bool is_general_legal_system_reference(const std::string &value) {
  static const std::set<std::string> legalSystemWords{
      "law", "laws", "jurisdiction", "jurisdictions", "code", "codes",
      "statute", "statutes", "regulation", "regulations",
  };
  // The name of an enactment ("False Claims Act", "Dental Practice Act",
  // "Rule 10b-5") is as public as "Russian law", and masking it is worse: a
  // live data-room run sent a diligence question to the model with every
  // statute it turned on replaced by a placeholder. Only as the last word of a
  // longer name - a bare "Act" or "Rule" names nothing.
  static const std::set<std::string> enactmentWords{
      "act", "acts", "rule", "rules", "ordinance", "ordinances", "constitution",
      "treaty", "convention", "directive", "directives",
  };
  const std::size_t lastSeparator = value.find_last_of(" \t\r\n");
  std::string lastWord = lastSeparator == std::string::npos ? value : value.substr(lastSeparator + 1);
  std::transform(lastWord.begin(), lastWord.end(), lastWord.begin(),
      [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  while (!lastWord.empty() && std::ispunct(static_cast<unsigned char>(lastWord.back()))) lastWord.pop_back();
  // A firm is named "... Law" as well: "Hartwell Employment Law" (#245) went
  // out as a body of law. Written "Law" with a capital, the words before it
  // must all say which law - ordinary words ("Employment", "General
  // Corporation"), a jurisdiction ("Delaware") or its adjective ("Russian") -
  // for the phrase to be one; a word of no such kind names the firm.
  if ((lastWord == "law" || lastWord == "laws") && lastSeparator != std::string::npos
      && value[lastSeparator + 1] == 'L' && !names_which_law(value.substr(0, lastSeparator)))
    return false;
  if (legalSystemWords.count(lastWord) > 0) return true;
  return lastSeparator != std::string::npos && enactmentWords.count(lastWord) > 0;
}

std::string bare_term(const std::string &value);
std::string normalized_entity_form(const std::string &value);

// The Russian Federation by name, in any case: the state, never a client.
bool names_the_russian_federation(const std::string &value) {
  static const std::set<std::string> russianForms{
      "российскаяфедерация", "российскойфедерации", "российскуюфедерацию", "российскойфедерацией", "рф",
  };
  return russianForms.count(normalized_entity_form(value)) > 0;
}

// A jurisdiction named on its own - "State of New York", "Delaware", "England
// and Wales", "Российской Федерации": where an agreement's law and its parties'
// incorporation come from, as public as "New York law" and needed to reason
// about it. A live English run sent "governed by the laws of [[E5]]" to the
// model. Only the exact name: "New York Life Insurance Company" or an address
// in Texas is not a jurisdiction and stays masked.
bool is_public_jurisdiction(const std::string &value) {
  static const std::set<std::string> jurisdictions{
      "alabama", "alaska", "arizona", "arkansas", "california", "colorado", "connecticut", "delaware", "florida",
      "georgia", "hawaii", "idaho", "illinois", "indiana", "iowa", "kansas", "kentucky", "louisiana", "maine",
      "maryland", "massachusetts", "michigan", "minnesota", "mississippi", "missouri", "montana", "nebraska",
      "nevada", "new hampshire", "new jersey", "new mexico", "new york", "north carolina", "north dakota", "ohio",
      "oklahoma", "oregon", "pennsylvania", "rhode island", "south carolina", "south dakota", "tennessee", "texas",
      "utah", "vermont", "virginia", "washington", "west virginia", "wisconsin", "wyoming", "district of columbia",
      "united states", "united states of america", "u.s", "u.s.a", "usa", "england and wales", "england",
      "wales", "scotland", "northern ireland", "united kingdom",
      // Countries, where an agreement's law or forum is one of them.
      "russia", "russian federation", "germany", "france", "canada", "mexico", "china", "japan", "india",
      "ireland", "switzerland", "netherlands", "the netherlands", "singapore", "hong kong", "cayman islands",
      "british virgin islands", "luxembourg", "cyprus", "israel", "australia", "brazil", "italy", "spain",
      "sweden", "norway", "denmark", "finland", "belgium", "austria", "poland", "kazakhstan", "ukraine",
      "belarus", "united arab emirates", "puerto rico", "european union",
      "uk", "u.k", "great britain", "uae", "u.a.e", "prc", "people's republic of china", "south korea", "korea",
      "republic of korea", "new zealand", "bermuda", "jersey", "guernsey", "isle of man", "gibraltar", "malta",
      "portugal", "greece", "czech republic", "czechia", "hungary", "romania", "bulgaria", "estonia", "latvia",
      "lithuania", "turkey", "türkiye", "saudi arabia", "qatar", "bahrain", "kuwait", "oman", "egypt", "south africa",
      "nigeria", "kenya", "argentina", "chile", "colombia", "peru", "taiwan", "thailand", "vietnam", "indonesia",
      "malaysia", "philippines", "armenia", "azerbaijan", "georgia", "uzbekistan", "kyrgyzstan", "serbia", "mongolia",
      "bahamas", "barbados", "mauritius", "seychelles", "marshall islands", "liechtenstein", "monaco",
      "dubai", "abu dhabi", "difc", "adgm", "dubai international financial centre", "abu dhabi global market",
      "european economic area",
      "ontario", "quebec", "british columbia", "alberta", "manitoba", "saskatchewan", "nova scotia", "new brunswick",
      "newfoundland and labrador", "prince edward island", "yukon", "northwest territories", "nunavut",
      "new south wales", "victoria", "queensland", "western australia", "south australia", "tasmania",
      "guam", "u.s. virgin islands", "american samoa", "northern mariana islands",
  };
  std::string term = bare_term(value);
  for (const char *prefix : {"laws of the ", "laws of ", "state of ", "commonwealth of ", "republic of ", "kingdom of ",
          "province of ", "federal republic of "})
    if (term.rfind(prefix, 0) == 0) term = term.substr(std::char_traits<char>::length(prefix));
  if (term.rfind("the ", 0) == 0) term = term.substr(4);
  if (jurisdictions.count(term) > 0) return true;
  return names_the_russian_federation(value);
}

std::set<std::string> russian_case_forms(const std::string &word);
std::string term_word_form(const std::string &word);

// A Russian or foreign jurisdiction named in Russian, as a place, in any case:
// «Англия», «Кипр», «Республика Казахстан», «Беларусь», «г. Москва»,
// «Московская область», «Республика Татарстан», «Краснодарский край» - where a
// governing-law or a forum clause puts it, as public as "the laws of England".
// Every case of every word is read (russian_case_forms), never a list of forms.
// A place only: a company named «Алтай» or «Кипр» keeps its mask.
bool is_public_russian_jurisdiction(const std::string &value) {
  static const std::vector<std::vector<std::set<std::string>>> forms = [] {
    const std::vector<std::string> names{
      // Countries and unions a Russian contract names for its law or forum.
      "россия", "англия", "уэльс", "шотландия", "великобритания", "соединенное королевство", "кипр", "казахстан",
      "республика казахстан", "беларусь", "белоруссия", "республика беларусь", "украина", "армения",
      "республика армения", "азербайджан", "грузия", "узбекистан", "республика узбекистан", "киргизия", "кыргызстан",
      "таджикистан", "туркменистан", "молдова", "китай", "кнр", "германия", "франция", "швейцария", "нидерланды",
      "люксембург", "сингапур", "гонконг", "оаэ", "турция", "индия", "сша", "соединенные штаты", "соединенные штаты америки",
      "швеция", "финляндия", "эстония", "латвия", "литва", "польша", "италия", "испания", "австрия", "израиль",
      "сербия", "монголия", "вьетнам", "ирландия", "бельгия", "дания", "норвегия", "чехия", "венгрия", "греция",
      "европейский союз", "евросоюз", "еаэс",
      // Cities - «г. Москва» too - stay masked: in an address a city is part of where someone is.
      // Republics.
      "республика адыгея", "адыгея", "республика алтай", "республика башкортостан", "башкортостан", "башкирия",
      "республика бурятия", "бурятия", "республика дагестан", "дагестан", "республика ингушетия", "ингушетия",
      "республика калмыкия", "калмыкия", "республика карелия", "карелия", "республика коми", "республика крым", "крым",
      "республика мордовия", "мордовия", "республика татарстан", "татарстан", "республика тыва", "тыва",
      "удмуртская республика", "удмуртия", "республика хакасия", "хакасия", "чеченская республика", "чечня",
      "чувашская республика", "чувашия", "республика саха", "якутия", "республика северная осетия", "кабардино балкарская республика",
      "карачаево черкесская республика", "республика марий эл",
      // Krais.
      "алтайский край", "забайкальский край", "камчатский край", "краснодарский край", "красноярский край",
      "пермский край", "приморский край", "ставропольский край", "хабаровский край",
      // Autonomous okrugs and the autonomous oblast.
      "ханты мансийский автономный округ", "ямало ненецкий автономный округ", "ненецкий автономный округ",
      "чукотский автономный округ", "еврейская автономная область",
    };
    const std::vector<std::string> oblasts{"амурская", "архангельская", "астраханская", "белгородская", "брянская",
      "владимирская", "волгоградская", "вологодская", "воронежская", "ивановская", "иркутская", "калининградская",
      "калужская", "кемеровская", "кировская", "костромская", "курганская", "курская", "ленинградская", "липецкая",
      "магаданская", "московская", "мурманская", "нижегородская", "новгородская", "новосибирская", "омская",
      "оренбургская", "орловская", "пензенская", "псковская", "ростовская", "рязанская", "самарская", "саратовская",
      "сахалинская", "свердловская", "смоленская", "тамбовская", "тверская", "томская", "тульская", "тюменская",
      "ульяновская", "челябинская", "ярославская"};
    std::vector<std::string> all = names;
    for (const std::string &oblast : oblasts) all.push_back(oblast + " область");
    // Each name as its words, each word as every case it takes.
    std::vector<std::vector<std::set<std::string>>> result;
    for (const std::string &name : all) {
      std::vector<std::set<std::string>> words;
      std::size_t from = 0;
      while (from <= name.size()) {
        std::size_t end = name.find(' ', from);
        if (end == std::string::npos) end = name.size();
        const std::string word = name.substr(from, end - from);
        std::set<std::string> wordForms = russian_case_forms(word);
        // «Московская», «Краснодарский»: an adjective's cases too.
        const std::string feminine = "ая";
        if (word.size() > feminine.size() && word.compare(word.size() - feminine.size(), feminine.size(), feminine) == 0) {
          const std::string stem = word.substr(0, word.size() - feminine.size());
          for (const char *ending : {"ой", "ую", "ою"}) wordForms.insert(stem + ending);
        }
        words.push_back(std::move(wordForms));
        from = end + 1;
      }
      result.push_back(std::move(words));
    }
    return result;
  }();
  std::vector<std::string> words;
  std::size_t from = 0;
  while (from <= value.size()) {
    std::size_t end = value.find_first_of(" -\t", from);
    if (end == std::string::npos) end = value.size();
    const std::string form = term_word_form(value.substr(from, end - from));
    if (!form.empty()) words.push_back(form);
    from = end + 1;
  }
  if (words.empty() || words.size() > 5) return false;
  for (const std::vector<std::set<std::string>> &name : forms) {
    if (name.size() != words.size()) continue;
    bool same = true;
    for (std::size_t index = 0; index < words.size() && same; ++index) same = name[index].count(words[index]) > 0;
    if (same) return true;
  }
  return false;
}

// Lower-cased, with a leading article, surrounding quotes and brackets, a
// trailing possessive and outer punctuation removed: "the Buyer's" -> "buyer".
std::string bare_term(const std::string &value) {
  std::string term;
  term.reserve(value.size());
  for (const char character : value)
    term.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  const auto isOuter = [](unsigned char character) {
    return std::isspace(character) || character == '"' || character == '\'' || character == '(' || character == ')'
        || character == '[' || character == ']' || character == ',' || character == '.' || character == ';'
        || character == ':';
  };
  while (!term.empty() && isOuter(static_cast<unsigned char>(term.front()))) term.erase(term.begin());
  while (!term.empty() && isOuter(static_cast<unsigned char>(term.back()))) term.pop_back();
  for (const char *article : {"the ", "a ", "an ", "each ", "any ", "such "}) {
    const std::string prefix(article);
    if (term.compare(0, prefix.size(), prefix) == 0) { term.erase(0, prefix.size()); break; }
  }
  for (const char *possessive : {"'s", "\xe2\x80\x99s"}) {
    const std::string suffix(possessive);
    if (term.size() > suffix.size() && term.compare(term.size() - suffix.size(), suffix.size(), suffix) == 0)
      term.erase(term.size() - suffix.size());
  }
  return term;
}

// Defined below: the form a value is compared in, case and punctuation aside.
std::string normalized_entity_form(const std::string &value);

// The same drafting vocabulary in Russian, where a contract names its parties
// once and then writes the role for the rest of the document: "ООО «Магма»
// (далее — «Продавец»)". The PII checkpoint tags those capitalised defined
// terms as people and organisations exactly as it does "Buyer" and "Seller",
// and a live Russian run showed what masking them costs: asked which changes
// between two drafts hurt the buyer, the model answered twice - once assuming
// the masked word was the seller, once the buyer - because "Покупатель",
// "Покупателя" and "покупателю" had each become a different opaque symbol. It
// buys no privacy either: outside the document that defines it, "Продавец"
// names nobody.
//
// A whole-value match against a closed list, so «Магма», «Ковалевский Артём
// Сергеевич» or «Волго-Камский банк» is never touched. Russian inflects, so
// the list is written as a stem with the endings that stem actually takes -
// the forms themselves, never a guess at morphology. `wordOfPhrase` asks about
// one word of a longer phrase, where a description counts as well as a role.
bool written_with_capital(const std::string &value);

bool is_russian_role_term(EntityType type, const std::string &value, bool wordOfPhrase = false) {
  static const std::set<std::string> roleForms = [] {
    // Every ending each group really takes, singular and plural. A form is
    // listed, never derived: "продавец" and "продавца" are both spelled out
    // through their own stems rather than stripped by a rule.
    const std::vector<std::pair<std::string, std::vector<std::string>>> groups{
      // -тель / -арь type: покупатель, арендатель...
      {"покупател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"арендодател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"поручител", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"залогодател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"залогодержател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"правообладател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"работодател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"представител", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"руководител", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"исполнител", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"участник", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      // The people around a legal matter, by what they do in it - the Russian
      // "lawyer", "chair", "secretary": a live run had «юристу» masked as a person.
      {"юрист", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"адвокат", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"нотариус", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"бухгалтер", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"аудитор", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"консультант", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"председател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"секретар", {"ь", "я", "ю", "ём", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"работник", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"заказчик", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"подрядчик", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"поставщик", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"должник", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"арендатор", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"кредитор", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"инвестор", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"лицензиар", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"лицензиат", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"банк", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"истец", {""}}, {"истц", {"а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"ответчик", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"продавец", {""}}, {"продавц", {"а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"заёмщик", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"заемщик", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"сторон", {"а", "ы", "е", "у", "ой", "", "ам", "ами", "ах"}},
      {"обществ", {"о", "а", "у", "ом", "е", "ами", "ах"}},
      {"компани", {"я", "и", "ю", "ей", "ям", "ями", "ях"}},
      {"супруг", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах", "ой", "ою"}},
      // The parties of carriage and forwarding, and the people a record names
      // by what they do: a live Russian matter (24 Sep 2026) sent «Клиент» and
      // «Экспедитор» as a person hundreds of times, «получателя» besides.
      {"клиент", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"экспедитор", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"перевозчик", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"субподрядчик", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"страховщик", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"агент", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"принципал", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"оператор", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"грузоотправител", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"грузополучател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"получател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"отправител", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"страховател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"выгодоприобретател", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"заявител", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"свидетел", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      {"доверител", {"ь", "я", "ю", "ем", "е", "и", "ей", "ям", "ями", "ях"}},
      // What a form asks a person for, never the person: «фамилия», «имя»,
      // «отчество».
      {"фамили", {"я", "и", "ю", "ей"}},
      {"им", {"я", "ени", "енем", "ена", "ен"}},
      {"отчеств", {"о", "а", "у", "ом", "е"}},
    };
    std::set<std::string> forms;
    for (const auto &[stem, endings] : groups)
      for (const std::string &ending : endings) forms.insert(stem + ending);
    return forms;
  }();
  // Words that only ever describe a party or an act of one, never name it -
  // the Russian counterpart of personDescriptionWords above. Exempt only when
  // EVERY word of a multi-word value is one of them ("остальных участников",
  // "согласия работников"), so a real name is never reached.
  static const std::set<std::string> descriptionForms{
    "остальных", "остальные", "остальным", "ключевыми", "ключевые", "ключевых", "контролирующий",
    "контролирующего", "контролирующим", "общий", "общее", "общим", "общего", "собрание", "собрания",
    "собранием", "согласие", "согласия", "согласию", "согласием", "список", "списка", "списке",
    "соглашение", "соглашения", "соглашению", "соглашением", "лицо", "лица", "лицу", "лицом", "лиц",
    "третье", "третьего", "третьим", "бывший", "бывшего", "новый", "нового", "новым", "каждый",
    "каждого", "каждым", "любой", "любого", "иной", "иного", "генеральный", "генерального",
    "генеральным", "директор", "директора", "директору", "директором", "директоров",
    // «главный бухгалтер», «главного юриста».
    "главный", "главного", "главному", "главным", "главном", "главная", "главной", "главную",
    "исполнительный", "исполнительного", "исполнительному", "исполнительным", "финансовый", "финансового",
    "финансовому", "финансовым",
    // "физическим лицам", "юридических лиц".
    "лицам", "лицами", "лицах", "физическое", "физического", "физическому", "физическим", "физическом",
    "физические", "физических", "физическими", "юридическое", "юридического", "юридическому", "юридическим",
    "юридическом", "юридические", "юридических", "юридическими",
  };
  // A company's own bodies, named by what they are: "совет директоров",
  // "наблюдательного совета", "правлению", "общее собрание акционеров". A live
  // Russian run had «совета директоров» masked as a court, so "Совет директоров
  // одобрил" came back as "совета директоров одобрил". Only for the
  // organisation-like types: never a reason to unmask a person.
  static const std::set<std::string> bodyForms{
    "совет", "совета", "совету", "советом", "совете", "советы", "советов", "наблюдательный",
    "наблюдательного", "наблюдательному", "наблюдательным", "наблюдательном", "правление", "правления",
    "правлению", "правлением", "правлении", "акционер", "акционера", "акционеров", "акционерам",
    "акционерами", "акционеры", "участников", "ревизионная", "ревизионной", "ревизионную", "комиссия",
    "комиссии", "комиссию", "комиссией", "заседание", "заседания", "заседанию", "заседанием", "заседании",
    // Its departments and the services it uses: "юридического департамента",
    // "логистического отдела", "оператора электронного документооборота".
    "департамент", "департамента", "департаменту", "департаментом", "департаменте", "отдел", "отдела",
    "отделу", "отделом", "отделе", "юридический", "финансовый", "финансового", "финансовому", "финансовым",
    "финансовом", "логистический", "логистического", "логистическому", "логистическим", "логистическом",
    "электронный", "электронного", "электронному", "электронным", "электронном", "документооборот",
    "документооборота", "документообороту", "документооборотом", "документообороте",
  };
  const bool personLike = type == EntityType::Person || type == EntityType::Judge;
  const bool organisationLike = type == EntityType::Organisation || type == EntityType::LegalEntity
      || type == EntityType::Court;
  if (!personLike && !organisationLike) return false;
  // «М. П.»: where a signature block puts the seal, never somebody's initials.
  if (personLike && normalized_entity_form(value) == "мп") return true;
  std::vector<std::string> words;
  std::string word;
  const auto take = [&] {
    const std::string form = normalized_entity_form(word);
    if (!form.empty()) words.push_back(form);
    word.clear();
  };
  for (const char character : value) {
    if (character == ' ' || character == '-' || character == '/' || character == '\t' || character == '\n') take();
    else word.push_back(character);
  }
  take();
  if (words.empty()) return false;
  // A capitalised -ов or -ев form of a role is also a surname - «Клиентов»,
  // «Продавцов», «Юристов» - so a person written that way stays a name.
  if (personLike && words.size() == 1 && written_with_capital(value)) {
    const std::string &form = words.front();
    for (const char *ending : {"ов", "ев"}) {
      const std::size_t size = std::char_traits<char>::length(ending);
      if (form.size() > size && form.compare(form.size() - size, size, ending) == 0) return false;
    }
  }
  // One word that is the role itself: "Покупатель", "покупателю", "Общество".
  if (words.size() == 1 && !wordOfPhrase)
    return roleForms.count(words.front()) > 0 || (organisationLike && bodyForms.count(words.front()) > 0);
  // Several words, every one of them a role or a description of one.
  return std::all_of(words.begin(), words.end(), [&](const std::string &form) {
    return roleForms.count(form) > 0 || descriptionForms.count(form) > 0
        || (organisationLike && bodyForms.count(form) > 0);
  });
}

// Whether a value carries a capital letter in any script: "Законов", "Covenant".
// Text that cannot be read counts as capitalised, so doubt keeps a mask on.
bool written_with_capital(const std::string &value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    utf8proc_int32_t codePoint = 0;
    const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
        static_cast<utf8proc_ssize_t>(value.size() - offset), &codePoint);
    if (consumed <= 0) return true;
    const auto category = utf8proc_category(codePoint);
    if (category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LT) return true;
    offset += static_cast<std::size_t>(consumed);
  }
  return false;
}

// Words a Russian request or agreement uses for the instrument, the law and
// the paperwork themselves - "договору", "праву", "основное соглашение",
// "документов", "неустойка". The PII checkpoint tags them as contract
// identifiers and legal entities, and a live run (Sep 2026) showed the cost:
// each became a symbol, the model wrote an English amendment around the
// symbols, and restoration put the Russian case forms back into it ("governed
// by the same law as договору, namely праву"). They name nobody. Written like
// is_russian_role_term: whole-value match against listed forms, never a guess
// at morphology, so «Договор-Сервис» or a name that merely contains one of
// these words is never reached.
//
// Tagged as a person, a value is one of these words only where the text writes
// it without a capital: «Законов» and «Штрафов» are surnames as well as case
// forms of "law" and "fine", and the capital is how the text says which.
bool is_russian_generic_legal_term(EntityType type, const std::string &value) {
  static const std::set<std::string> nounForms = [] {
    const std::vector<std::pair<std::string, std::vector<std::string>>> groups{
      {"договор", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"контракт", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"документ", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"закон", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"штраф", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"соглашени", {"е", "я", "ю", "ем", "и", "й", "ям", "ями", "ях"}},
      {"приложени", {"е", "я", "ю", "ем", "и", "й", "ям", "ями", "ях"}},
      {"прав", {"о", "а", "у", "ом", "е", ""}},
      {"законодательств", {"о", "а", "у", "ом", "е"}},
      {"неустойк", {"а", "и", "е", "у", "ой", "ою"}},
      {"юрисдикци", {"я", "и", "ю", "ей", "ям", "ями", "ях"}},
      // The Project itself, as the lawyer names the work.
      {"проект", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      // The claims and papers of a dispute and a contract, and what caps them.
      // A live Russian matter (24 Sep 2026) sent «иска» as a legal entity - and
      // then replaced it inside «Переписка» - with «лимит ответственности»,
      // «реестр претензий» and «служебная записка» masked besides.
      {"иск", {"", "а", "у", "ом", "е", "и", "ов", "ам", "ами", "ах"}},
      {"претензи", {"я", "и", "ю", "ей", "й", "ям", "ями", "ях"}},
      {"требовани", {"е", "я", "ю", "ем", "и", "й", "ям", "ями", "ях"}},
      {"доверенност", {"ь", "и", "ью", "ей", "ям", "ями", "ях"}},
      {"ответственност", {"ь", "и", "ью"}},
      {"лимит", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"предел", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"ограничени", {"е", "я", "ю", "ем", "и", "й", "ям", "ями", "ях"}},
      {"реестр", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"протокол", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"акт", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"устав", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"кодекс", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"раздел", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"пункт", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"расчёт", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"расчет", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"записк", {"а", "и", "е", "у", "ой", "ою", "ам", "ами", "ах"}},
      {"заявк", {"а", "и", "е", "у", "ой", "ою", "ам", "ами", "ах"}},
      {"перевозк", {"а", "и", "е", "у", "ой", "ою", "ам", "ами", "ах"}},
      {"переписк", {"а", "и", "е", "у", "ой", "ою"}},
      {"уведомлени", {"е", "я", "ю", "ем", "и", "й", "ям", "ями", "ях"}},
      {"решени", {"е", "я", "ю", "ем", "и", "й", "ям", "ями", "ях"}},
      {"страховани", {"е", "я", "ю", "ем", "и"}},
      {"экспедици", {"я", "и", "ю", "ей"}},
      {"письм", {"о", "а", "у", "ом", "е"}},
      // A place an agreement names by its kind: "на складе Экспедитора".
      {"склад", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"хранилищ", {"е", "а", "у", "ем"}},
      // What a party may claim for a breach, named as a remedy: «возмещения
      // убытков, неустойки, штрафа и исполнения обязательства в натуре, а также
      // принятия обеспечительных мер». The checkpoint took the last clause for a
      // person, commas and all (a lane's check of a live run, 25 Sep 2026).
      {"убыт", {"ок", "ка", "ку", "ком", "ке", "ки", "ков", "кам", "ками", "ках"}},
      {"возмещени", {"е", "я", "ю", "ем", "и", "й"}},
      {"пен", {"я", "и", "ю", "ей", "ям", "ями", "ях"}},
      {"мер", {"а", "ы", "е", "у", "ой", "ою", "", "ам", "ами", "ах"}},
      {"обеспечени", {"е", "я", "ю", "ем", "и"}},
      {"исполнени", {"е", "я", "ю", "ем", "и"}},
      {"обязательств", {"о", "а", "у", "ом", "е", "", "ам", "ами", "ах"}},
      {"натур", {"а", "ы", "е", "у", "ой"}},
      {"компенсаци", {"я", "и", "ю", "ей", "й", "ям", "ями", "ях"}},
      {"реституци", {"я", "и", "ю", "ей"}},
      {"расторжени", {"е", "я", "ю", "ем", "и"}},
      {"ущерб", {"", "а", "у", "ом", "е"}},
      {"вред", {"", "а", "у", "ом", "е"}},
      {"санкци", {"я", "и", "ю", "ей", "й", "ям", "ями", "ях"}},
      {"выгод", {"а", "ы", "е", "у", "ой"}},
      {"приняти", {"е", "я", "ю", "ем", "и"}},
      {"применени", {"е", "я", "ю", "ем", "и"}},
      {"взыскани", {"е", "я", "ю", "ем", "и", "й"}},
      {"удержани", {"е", "я", "ю", "ем", "и"}},
      {"расход", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"процент", {"", "а", "у", "ом", "е", "ы", "ов", "ам", "ами", "ах"}},
      {"зачет", {"", "а", "у", "ом", "е"}},
      {"зачёт", {"", "а", "у", "ом", "е"}},
      {"запрет", {"", "а", "у", "ом", "е", "ы", "ов"}},
    };
    std::set<std::string> forms;
    for (const auto &[stem, endings] : groups)
      for (const std::string &ending : endings) forms.insert(stem + ending);
    return forms;
  }();
  // Words that only say which instrument is meant: "основное соглашение", "доп
  // соглашения", "применимому праву", "настоящего договора".
  static const std::set<std::string> qualifierForms{
    "доп", "дополнительное", "дополнительного", "дополнительному", "дополнительным", "дополнительном",
    "дополнительные", "дополнительных", "основной", "основное", "основного", "основному", "основным",
    "основном", "основная", "основную", "настоящий", "настоящего", "настоящему", "настоящим", "настоящем",
    "настоящее", "данный", "данного", "данному", "данным", "данном", "данное", "указанный", "указанного",
    "указанному", "указанным", "указанном", "указанное", "применимое", "применимого", "применимому",
    "применимым", "применимом", "применимый", "действующий", "действующего", "действующему", "действующее",
    "прежний", "прежнего", "прежнему", "прежнее", "новый", "нового", "новому", "новое", "этот", "этого",
    "этому", "это", "того", "тому", "же", "остальных", "остальные", "прочих", "иных",
    // The kind of contract, which names no party: "договор поставки", "договор
    // оказания услуг", "договору купли-продажи".
    "поставки", "аренды", "займа", "подряда", "оказания", "услуг", "купли", "продажи", "хранения",
    "перевозки", "страхования", "залога", "поручительства", "лицензионный", "лицензионного",
    "лицензионному", "кредитный", "кредитного", "кредитному", "трудовой", "трудового", "трудовому",
    // Which paper, which statute and whose: "служебная записка", "транспортной
    // экспедиции", "Гражданского кодекса Российской Федерации", "реестр
    // претензий и расчёт неустойки".
    "служебная", "служебной", "служебную", "служебные", "служебных", "транспортной", "транспортная",
    "транспортную", "транспортно", "экспедиционной", "экспедиционная", "гражданский", "гражданского",
    "гражданскому", "гражданским", "гражданском", "налоговый", "налогового", "налоговому", "налоговым",
    "налоговом", "федеральный", "федерального", "федеральному", "федеральным", "федеральном", "российской",
    "российская", "федерации", "федерация", "рф", "облачное", "облачного", "облачном", "о", "об", "по", "и",
    "с", "к", "для",
  };
  // Which remedy, and the words that join remedies in a list: «обеспечительных
  // мер», «упущенной выгоды», «реального ущерба», «а также», «в том числе».
  static const std::set<std::string> remedyForms = [] {
    std::set<std::string> forms{"а", "также", "либо", "или", "в", "за", "том", "числе"};
    for (const char *stem : {"обеспечительн", "денежн", "упущенн", "реальн", "прям", "косвенн", "договорн",
            "законн", "штрафн", "исключительн", "судебн", "временн", "предварительн", "односторонн"})
      for (const char *ending : {"ый", "ой", "ого", "ому", "ым", "ом", "ая", "ую", "ое", "ые", "ых", "ыми", "ий",
              "ее", "ие", "их", "ими"})
        forms.insert(std::string(stem) + ending);
    return forms;
  }();
  std::vector<std::string> words;
  std::string word;
  const auto take = [&] {
    const std::string form = normalized_entity_form(word);
    if (!form.empty()) words.push_back(form);
    word.clear();
  };
  for (const char character : value) {
    if (character == ' ' || character == '-' || character == '/' || character == '\t' || character == '\n') take();
    else word.push_back(character);
  }
  take();
  if (words.empty()) return false;
  const bool personLike = type == EntityType::Person || type == EntityType::Judge;
  if (personLike && written_with_capital(value)) return false;
  // A body of law named in general - "право Российской Федерации",
  // "законодательство Республики Казахстан" - is as public as "Russian law":
  // the value opens with the law itself, and every word after it says only
  // whose law it is, which right is meant or which party's role holds it ("право
  // аренды", "права Покупателя"). «Права» is also a person's rights: «права
  // Иванова» or «права требования к ООО «Альфа»» names whose they are, and one
  // word outside those lists keeps the whole value masked. Never for a person,
  // where the law word is the checkpoint reaching past a name.
  static const std::set<std::string> lawForms{
    "право", "права", "праву", "правом", "праве", "законодательство", "законодательства",
    "законодательству", "законодательством", "законодательстве",
  };
  // Whose law, in the genitive the phrase takes, written as its words run
  // together - the form every word here is compared in.
  static const std::set<std::string> jurisdictionForms{
    "россии", "республикиказахстан", "казахстана", "республикибеларусь", "беларуси", "республикиузбекистан",
    "узбекистана", "республикиармения", "армении", "кыргызскойреспублики", "азербайджанскойреспублики",
    "грузии", "украины", "англии", "англиииуэльса", "великобритании", "соединенногокоролевства",
    "соединённогокоролевства", "сша", "соединенныхштатов", "соединённыхштатов", "соединенныхштатовамерики",
    "соединённыхштатовамерики", "штатаньюйорк", "штатаделавэр", "штатакалифорния", "кипра",
    "республикикипр", "германии", "франции", "швейцарии", "нидерландов", "сингапура", "гонконга", "китая",
    "кнр", "европейскогосоюза", "евросоюза", "ес", "еаэс",
  };
  // Which right: "право собственности", "права требования".
  static const std::set<std::string> rightForms{
    "требования", "требований", "собственности", "пользования", "владения", "распоряжения",
  };
  if (!personLike && words.size() > 1 && lawForms.count(words.front()) > 0) {
    std::string joined;
    std::string spaced;
    for (std::size_t index = 1; index < words.size(); ++index) {
      joined += words[index];
      if (!spaced.empty()) spaced += ' ';
      spaced += words[index];
    }
    if (is_public_jurisdiction(joined) || jurisdictionForms.count(joined) > 0) return true;
    if (is_russian_role_term(EntityType::Organisation, spaced)) return true;
    if (std::all_of(words.begin() + 1, words.end(), [&](const std::string &form) {
          return qualifierForms.count(form) > 0 || rightForms.count(form) > 0;
        }))
      return true;
  }
  if (!personLike) {
    // A statute by its title - "Закон о ТЭД", "Федерального закона «О
    // персональных данных»": the law word, then "о" or "об" and what the law
    // is about. Every word before the law word must say which law.
    static const std::set<std::string> statuteForms{
      "закон", "закона", "закону", "законом", "законе", "кодекс", "кодекса", "кодексу", "кодексом", "кодексе",
    };
    for (std::size_t index = 0; index + 1 < words.size(); ++index) {
      if (statuteForms.count(words[index]) == 0) {
        if (qualifierForms.count(words[index]) == 0) break;
        continue;
      }
      if (words[index + 1] == "о" || words[index + 1] == "об") return true;
      break;
    }
    // A code by its abbreviation and the country that enacted it: "ГК РФ".
    // "ГК" alone is also a group of companies, and stays masked.
    static const std::set<std::string> codeForms{
      "гк", "гпк", "апк", "нк", "тк", "ук", "упк", "коап", "жк", "ск", "зк", "бк", "вк", "лк",
    };
    static const std::set<std::string> countryForms{"рф", "россии"};
    const bool code = std::any_of(words.begin(), words.end(), [&](const std::string &form) {
      return codeForms.count(form) > 0;
    });
    const bool country = std::any_of(words.begin(), words.end(), [&](const std::string &form) {
      return countryForms.count(form) > 0;
    });
    if (code && country && std::all_of(words.begin(), words.end(), [&](const std::string &form) {
          return codeForms.count(form) > 0 || countryForms.count(form) > 0;
        }))
      return true;
  }
  // A party's role or a body of the company beside the paper names nobody
  // either: "реестр претензий Клиента", "протокол совета директоров".
  const EntityType roleType = personLike ? EntityType::Person : EntityType::Organisation;
  bool anyNoun = false;
  for (const std::string &form : words) {
    if (nounForms.count(form) > 0) anyNoun = true;
    else if (qualifierForms.count(form) == 0 && remedyForms.count(form) == 0
        && !is_russian_role_term(roleType, form, true))
      return false;
  }
  return anyNoun;
}

// The English counterpart for the one label the generic-bodies list below never
// sees: the checkpoint tags "this Agreement" or "the prior contract" as a
// contract identifier. A plain instrument word, with at most words that say
// which instrument is meant, names nobody; a number or a party's name in the
// value ("Agreement No. NW-17", "Northwind Master Services Agreement") keeps it
// protected, because one word outside these lists keeps the mask on.
bool is_generic_instrument_reference(const std::string &value) {
  static const std::set<std::string> instruments{
      "agreement", "agreements", "contract", "contracts", "amendment", "amendments", "addendum", "addenda",
      "lease", "leases", "deed", "deeds", "instrument", "instruments", "letter", "schedule", "schedules",
      "exhibit", "exhibits", "annex", "annexes", "appendix", "document", "documents", "nda", "ndas",
  };
  // The papers of a US deal by their kind, which name no party only with the
  // words that say which paper (Larkspur data room, 11 Oct 2026, sent "Trademark
  // License Agreement", "R&W Insurance Policy" and "Spousal Consent" as symbols).
  // Alone each can be a company's name - "Waiver", "Charter", "Proxy" - and
  // stays protected.
  static const std::set<std::string> qualifiedInstruments{
      "policy", "policies", "certificate", "certificates", "bylaws", "by-laws", "charter", "warrant", "warrants",
      "proxy", "proxies", "consent", "consents", "waiver", "waivers", "note", "notes", "guaranty", "guaranties",
      "guarantee", "guarantees", "indenture", "indentures", "sublease", "subleases", "joinder", "joinders",
      "resolution", "resolutions", "minutes", "memorandum", "license", "licence", "licenses", "licences",
  };
  // The kind of agreement, which names no party: "Merger Agreement",
  // "Non-Compete Agreement", "Mutual NDA". "Merger Sub LLC" does not end in an
  // instrument and stays protected.
  static const std::set<std::string> qualifiers{
      "this", "that", "these", "those", "said", "such", "same", "prior", "previous", "current", "existing",
      "original", "underlying", "main", "principal", "new", "other", "every", "each", "any", "all", "relevant",
      "master", "services", "service", "supply", "purchase", "license", "licence", "loan", "credit",
      "employment", "framework", "side", "amended", "restated", "and", "of", "merger", "mutual", "non",
      "compete", "noncompete", "solicitation", "nonsolicitation", "disclosure", "nondisclosure",
      "confidentiality", "competition", "noncompetition", "severance", "separation", "release", "settlement",
      "consulting", "operating", "shareholders", "stockholders", "partnership", "escrow", "guaranty", "guarantee",
      "pledge", "security", "subscription", "indemnity", "indemnification",
      // What a deal's papers are about, as their titles say: "Stock Purchase
      // Agreement", "Management Services Agreement", "Business Associate
      // Agreement", "Executive Employment Agreement", "Equity Commitment Letter".
      // A party's name is none of these words, so "Larkspur Stock Purchase
      // Agreement" keeps its mask.
      "stock", "asset", "assets", "equity", "share", "shares", "unit", "units", "membership", "interest",
      "interests", "trademark", "trademarks", "patent", "patents", "copyright", "intellectual", "property", "ip",
      "management", "business", "associate", "executive", "officer", "director", "transfer", "restriction",
      "restrictions", "restrictive", "covenant", "assignment", "assumption", "intercompany", "transition",
      "distribution", "reseller", "voting", "investor", "investors", "rights", "registration", "data",
      "processing", "protection", "privacy", "joint", "venture", "development", "collaboration", "research",
      "participating", "provider", "providers", "network", "reimbursement", "professional", "administrative",
      "billing", "physician", "physicians", "medical", "clinical", "retention", "bonus", "incentive", "option",
      "options", "award", "awards", "restricted", "phantom", "convertible", "promissory", "subordination",
      "intercreditor", "collateral", "revolving", "term", "bridge", "senior", "facility", "commitment",
      "engagement", "retainer", "referral", "marketing", "sponsorship", "hosting", "software", "maintenance",
      "support", "saas", "insurance", "r&w", "rwi", "d&o", "e&o", "tail", "cyber", "liability", "umbrella",
      "malpractice", "general", "excess", "title", "spousal", "landlord", "tenant", "lender", "written", "board",
      "unanimous", "omnibus", "estoppel", "paying", "agent", "custody", "account", "control", "deposit",
      "operating", "limited", "lease", "sale", "bill", "power", "attorney", "trust", "letter", "ehr", "baa",
      // Which one of a series: "First Amendment", "Second Amended and Restated".
      "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth", "tenth",
      // What it amends, as its full title says: "First Amendment to Lease".
      "to",
  };
  const std::string term = bare_term(value);
  if (term.empty()) return false;
  static const std::set<std::string> definedInstruments{
      "statement of work", "statements of work", "order form", "order forms", "purchase order", "purchase orders",
      "work order", "work orders",
      // A paper named by what it does, which names no party.
      "notice of default", "notice of termination", "notice of breach", "notice of non-renewal",
      "notice of nonrenewal", "notice of renewal", "notice of exercise", "notice of claim", "notice of assignment",
      "notice of dispute", "notice to cure", "term sheet", "letter of intent", "side letter", "estoppel certificate",
      "demand letter", "cease and desist letter", "power of attorney",
      // A company's own constitutional papers, which name no party by their kind.
      "certificate of incorporation", "certificate of formation", "certificate of organization",
      "articles of incorporation", "articles of organization", "articles of association",
      "memorandum of association", "certificate of good standing", "certificate of designation",
      "certificate of designations", "certificate of amendment", "certificate of merger", "bill of sale",
      "deed of trust", "deeds of trust", "letter of credit", "letters of credit", "deed in lieu of foreclosure",
  };
  if (definedInstruments.count(term) > 0) return true;
  std::vector<std::string> words;
  std::string word;
  for (const char character : term) {
    if (character == ' ' || character == '-' || character == '/') {
      if (!word.empty()) words.push_back(word);
      word.clear();
    } else {
      word.push_back(character);
    }
  }
  if (!word.empty()) words.push_back(word);
  if (words.empty()) return false;
  const bool qualifiedHead = qualifiedInstruments.count(words.back()) > 0 && words.size() >= 2;
  if (instruments.count(words.back()) == 0 && !qualifiedHead) return false;
  for (const std::string &each : words)
    if (instruments.count(each) == 0 && qualifiers.count(each) == 0 && qualifiedInstruments.count(each) == 0)
      return false;
  return true;
}

// The role a party plays in an agreement is drafting vocabulary, not
// identity: every purchase agreement has a Buyer and a Seller, every lease a
// Landlord and a Tenant. The PII checkpoint tags these capitalised defined
// terms as people and organisations, and masking them removes the one thing
// the model needs to reason about an obligation - who owes it to whom ("[P3]
// shall indemnify [P4]" restores perfectly and was reasoned about blind). An
// exact match of the whole value against a closed list, so "Holt" or "Marsh
// Dental Group" is never touched; a Project-registered entity or a custom
// term that happens to be one of these words is still masked by
// known_project_entities()/deterministic_entities(), which this never sees.
bool is_generic_role_term(EntityType type, const std::string &value) {
  // Words that are never a surname: safe to exempt even when tagged a person.
  static const std::set<std::string> partyRoles{
      "buyer", "buyers", "seller", "sellers", "purchaser", "purchasers", "tenant", "tenants", "landlord",
      "landlords", "lessor", "lessors", "lessee", "lessees", "sublessee", "subtenant", "lender", "lenders",
      "borrower", "borrowers", "guarantor", "guarantors", "licensor", "licensors", "licensee", "licensees",
      "employer", "employee", "employees", "executive", "stockholder", "stockholders", "shareholder",
      "shareholders", "investor", "investors", "plaintiff", "plaintiffs", "defendant", "defendants", "claimant",
      "claimants", "respondent", "respondents", "relator", "patient", "patients", "physician", "physicians",
      // The capacities a party holds in a lawsuit (P0, 30 Oct 2026).
      "counterclaimant", "counterclaimants", "counterclaim-plaintiff", "counterclaim-plaintiffs",
      "counterclaim-defendant", "counterclaim-defendants", "cross-claimant", "cross-defendant",
      "third-party plaintiff", "third-party defendant", "petitioner", "petitioners", "appellant", "appellants",
      "appellee", "appellees", "movant", "intervenor",
      "party", "parties", "indemnifying party", "indemnified party", "disclosing party", "receiving party",
      "company", "companies", "ceo", "cfo", "coo", "cto", "cio", "ciso", "cmo", "cco", "clo",
      "gc", "vp", "svp", "evp", "manager", "managers", "director", "directors", "board of directors", "officer",
      "officers",
      "customer", "customers", "representative", "representatives", "founder", "founders", "individual",
      "individuals", "person", "persons",
      // The people around a legal matter, by what they do in it.
      "agent", "agents", "paying agent", "escrow agent", "registered agent", "registered agents",
      "administrative agent", "collateral agent",
      "lawyer", "lawyers", "attorney", "attorneys", "counsel", "general counsel", "legal counsel",
      "in-house counsel", "outside counsel", "chair", "chairman", "chairwoman", "chairperson", "secretary",
      "advisor", "advisors", "adviser", "advisers",
      // The same thing in a matrimonial or family matter: the role a person
      // holds in the proceeding, which names nobody on its own.
      "petitioner", "petitioners", "co-petitioner", "co-petitioners", "movant", "movants",
  };
  // Generic bodies and instruments: never a person's name, so only exempt for
  // the organisation-like types, where "Board" or "Agreement" names nobody.
  static const std::set<std::string> genericBodies{
      "board", "board of directors", "committee", "company group", "group", "parent", "subsidiary",
      "subsidiaries", "affiliate", "affiliates", "vendor", "vendors", "supplier", "suppliers", "customer",
      "customers", "client", "clients", "contractor", "contractors", "consultant", "provider", "providers",
      "payor", "payer", "practice", "managed practice", "management company", "member", "members", "partner",
      "partners", "holder", "holders", "agent", "administrative agent", "collateral agent", "escrow agent",
      "trustee", "governmental authority", "governmental authorities", "government", "agreement", "lease",
      "contract", "person", "persons", "third party", "third parties", "court", "courts", "arbitrator",
      // Where a dispute is heard, which names nobody. A real forensic run had
      // "arbitration" itself replaced by a placeholder, so a clause about how
      // disputes are resolved reached the model as a clause about a party -
      // and came back with the word restored as if it were a proper noun.
      "arbitration", "arbitrations", "arbitral tribunal", "arbitration tribunal", "arbitral panel",
      "arbitration panel", "tribunal", "tribunals", "mediation", "mediator", "mediators",
      "conciliation", "conciliator",
      // The lawyer's own words for the work itself - Singularity's Project, a
      // firm's matter. A real run masked "Project" in "What is this Project
      // about?" and the model answered about "the symbol Project". A code name
      // that merely begins with the word ("Project Atlas") is not this value.
      "project", "projects", "matter", "matters",
      "проект", "проекта", "проекте", "проекту", "проектом", "проекты", "проектов", "проектах",
      // A services agreement's own defined terms: capitalised because the
      // agreement defines them, never a party.
      "services", "service", "statement of work", "statements of work", "order form", "order forms",
      "purchase order", "purchase orders", "work order", "work orders", "deliverables", "products", "goods",
  };
  // A description of a kind of person, not a person: "independent contractor",
  // "Seller's spouse", "Chief Executive Officer", "Escrow Agent", "affected
  // individuals". Exempt only when EVERY word of the value is one of these - a
  // single word outside the list ("Major Payne", "Agent Cooper") keeps the
  // whole value masked. A real person whose name the Project knows is masked
  // from the Project's own registry whatever the model thinks of the word.
  static const std::set<std::string> personDescriptionWords{
      "person", "persons", "individual", "individuals", "another", "other", "any", "each", "such", "certain",
      "spouse", "spouses", "member", "members", "holder", "holders", "selling", "contractor", "contractors",
      "subcontractor", "subcontractors", "independent", "dentist", "dentists", "licensed", "physician",
      "physicians", "referring", "employee", "employees", "former", "current", "key", "personnel", "staff",
      "advisor", "advisors", "adviser", "advisers", "representative", "representatives", "founder", "founders",
      "payor", "payors", "payer", "payers", "workforce", "affected", "lineal", "descendant", "descendants",
      "immediate", "family", "disqualified", "officer", "officers", "responsible", "director", "directors",
      "clinical", "medical", "chief", "executive", "executives", "financial", "operating", "compliance",
      "privacy", "security", "administrative", "agent", "agents", "escrow", "collateral", "investor", "investors",
      "major", "patient", "patients", "party", "parties", "third", "counsel", "attorney", "attorneys", "manager",
      "managers", "owner", "owners", "beneficiary", "beneficiaries", "heir", "heirs", "successor", "successors",
      "assign", "assigns", "assignee", "assignees", "assignor", "transferee", "transferor", "indemnitee",
      "indemnitees", "indemnitor", "indemnified", "indemnifying", "obligor", "obligee", "grantor", "grantee",
      "trustee", "trustees", "signatory", "signatories", "witness", "witnesses", "notary", "auditor", "auditors",
      "consultant", "consultants", "customer", "customers", "client", "clients", "supplier",
      "suppliers", "vendor", "vendors", "provider", "providers", "professional", "professionals", "nurse",
      "nurses", "doctor", "doctors", "clinician", "clinicians", "applicant", "applicants", "relative",
      "relatives", "general", "president", "vice", "secretary", "treasurer", "chairman", "chair",
      "laboratory", "property", "associate", "associates", "senior", "junior", "managing", "deputy", "assistant",
      "interim", "acting", "lead", "head", "regional", "outside", "external", "internal", "nominee", "designee",
      "series", "a", "b", "c", "preferred", "common", "board", "observer",
  };
  const std::string term = bare_term(value);
  if (term.empty()) return false;
  if (partyRoles.count(term) > 0) return true;
  if (is_russian_role_term(type, value)) return true;
  if (type == EntityType::Person) {
    std::string word;
    bool any = false;
    bool every = true;
    const auto judge = [&] {
      if (word.size() > 2 && word.compare(word.size() - 2, 2, "'s") == 0) word.erase(word.size() - 2);
      while (!word.empty() && (word.back() == '\'' || word.back() == '.' || word.back() == ',')) word.pop_back();
      if (!word.empty()) {
        any = true;
        if (partyRoles.count(word) == 0 && personDescriptionWords.count(word) == 0) every = false;
      }
      word.clear();
    };
    for (const char character : term) {
      if (character == ' ' || character == '-' || character == '/') judge();
      else word.push_back(character);
    }
    judge();
    // Four or more plain lower-case words are a phrase the model mistook for a
    // name ("physician lists are out of date"), never a name.
    const bool lowerCasePhrase = std::count(term.begin(), term.end(), ' ') >= 3
        && std::none_of(value.begin(), value.end(),
            [](unsigned char character) { return std::isupper(character) != 0 || character >= 0x80; });
    if (any && lowerCasePhrase) return true;
    if (!any || !every) return false;
    // One word alone can be a surname - Holder, Major, Nurse. It is only a
    // description when the text itself writes it as a common noun.
    const bool oneWord = term.find_first_of(" -/") == std::string::npos;
    const bool writtenAsCommonNoun = std::none_of(value.begin(), value.end(),
        [](unsigned char character) { return std::isupper(character) != 0 || character >= 0x80; });
    return !oneWord || writtenAsCommonNoun;
  }
  const bool organisationLike = type == EntityType::Organisation || type == EntityType::LegalEntity
      || type == EntityType::Court;
  return organisationLike && genericBodies.count(term) > 0;
}

// The acronym of a statute, a regulator or a standard is public law, exactly
// as its full name is: "CLIA", "HIPAA", "the DOJ". Never for a person.
bool is_public_legal_acronym(const std::string &value) {
  static const std::set<std::string> acronyms{
      "hipaa", "hitech", "clia", "erisa", "cobra", "fcpa", "fca", "aks", "ada", "fmla", "flsa", "osha", "ucc",
      "irc", "irs", "sec", "doj", "ftc", "hhs", "ocr", "oig", "cms", "fda", "dea", "eeoc", "nlrb", "dol", "epa",
      "finra", "cfius", "hsr", "gaap", "ifrs", "gdpr", "ccpa", "cpra", "pci", "nist", "soc",
      // Public bodies and programmes of US health care and enforcement.
      "fbi", "medicare", "medicaid", "champva", "tricare", "leie", "uspto", "hhs-oig", "hhs oig", "hhs ocr",
      "oig leie", "hhs-ocr", "dgcl", "stark", "phi", "baa",
  };
  return acronyms.count(bare_term(value)) > 0;
}

bool contains_digit(const std::string &value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char character) { return std::isdigit(character) != 0; });
}

bool contains_word(const std::string &lowered, const std::set<std::string> &words) {
  std::string word;
  const auto flush = [&] {
    const bool found = !word.empty() && words.count(word) > 0;
    word.clear();
    return found;
  };
  for (const char character : lowered) {
    if (std::isalpha(static_cast<unsigned char>(character))) word.push_back(character);
    else if (flush()) return true;
  }
  return flush();
}

// A threshold or a share is how an agreement works, not who it is about: "more
// than fifty percent (50%) of the equity", "100% of the stock". The checkpoint
// tags percentages as money; masked, the model cannot compare the threshold
// with the facts (live: "if 100% is fifty percent or less..."). Exempt only
// when the value is a percentage with no currency in it - "$5,000,000 (2.5%)"
// stays an amount.
bool is_bare_percentage(const std::string &value) {
  std::string lowered;
  for (const char character : value)
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  const bool percentage = lowered.find('%') != std::string::npos || lowered.find("percent") != std::string::npos
      || lowered.find("per cent") != std::string::npos || lowered.find("basis point") != std::string::npos;
  if (!percentage) return false;
  static const std::set<std::string> currencyWords{
      "usd", "eur", "gbp", "rub", "chf", "jpy", "cny", "dollar", "dollars", "euro", "euros", "pound", "pounds",
      "ruble", "rubles", "rouble", "roubles", "yen", "yuan", "franc", "francs",
  };
  if (lowered.find('$') != std::string::npos || lowered.find("\xe2\x82\xac") != std::string::npos
      || lowered.find("\xc2\xa3") != std::string::npos || lowered.find("\xe2\x82\xbd") != std::string::npos
      || lowered.find("\xc2\xa5") != std::string::npos) return false;
  return !contains_word(lowered, currencyWords);
}

// Whether one lowercased Russian word counts, names money or names a time: a
// number word ("пятьсот", "млн", "полтора"), a currency ("рублей", "коп."), a
// month ("марта"), and for a date a day of the week or an ordinal ("пятницу",
// "первого"). Matched by the stems these words start with, like
// belongs_to_amount(): a word matched by mistake only keeps more protected.
bool counts_or_dates_in_russian(const std::string &form, bool date) {
	static const std::set<std::string> exactForms{
		"ноль", "нуля", "нулю", "нулём", "нулем", "один", "одна", "одно", "одного", "одной", "одному", "одним",
		"одном", "одну", "два", "две", "двух", "двум", "двумя", "три", "трёх", "трех", "трём", "трем", "тремя",
		"сто", "ста", "сотня", "сотни", "сотен", "сотню", "май", "мая", "мае", "маю", "маем", "руб",
	};
	static const std::vector<std::string> stems{
		"четыр", "пят", "шест", "семь", "семи", "семн", "седьм", "восем", "восьм", "девят", "девян", "десят",
		"одиннадц", "двенадц", "тринадц", "двадцат", "тридцат", "сорок", "двест", "трист", "тысяч", "тыс",
		"миллион", "млн", "миллиард", "млрд", "триллион", "трлн", "полтор", "полутор", "полмиллион", "полтысяч",
		"половин", "рубл", "коп", "доллар", "долл", "евро", "юан", "фунт", "франк", "иен", "январ", "феврал",
		"март", "апрел", "июн", "июл", "август", "сентябр", "октябр", "ноябр", "декабр",
	};
	static const std::vector<std::string> dateStems{
		"понедельник", "вторник", "сред", "четверг", "пятниц", "суббот", "воскресень", "перв", "втор", "трет",
		"четвёрт", "четверт",
	};
	if (exactForms.count(form) > 0) return true;
	for (const std::string &stem : stems)
		if (form.compare(0, stem.size(), stem) == 0) return true;
	if (!date) return false;
	for (const std::string &stem : dateStems)
		if (form.compare(0, stem.size(), stem) == 0) return true;
	return false;
}

// A "date" or an "amount" with nothing in it that dates or counts: the
// checkpoint tags the defined terms themselves ("Closing Date", "the date
// hereof", "fair market value", "Base Salary"). No figure, no month, no
// currency, no spelled number - nothing here could identify a deal.
bool is_empty_quantity_term(EntityType type, const std::string &value) {
  if (type != EntityType::Date && type != EntityType::MoneyAmount) return false;
  if (contains_digit(value)) return false;
  std::string lowered;
  for (const char character : value)
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  static const std::set<std::string> quantityWords{
      "january", "february", "march", "april", "may", "june", "july", "august", "september", "october",
      "november", "december", "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec",
      "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
      "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "twenty",
      "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "hundred", "thousand", "million",
      "billion", "trillion", "dollar", "dollars", "euro", "euros", "pound", "pounds", "usd", "eur", "gbp", "rub",
      "ruble", "rubles", "rouble", "roubles",
  };
  if (contains_word(lowered, quantityWords)) return false;
  // What a Russian agreement calls the money itself, with no figure: "не
  // превышает вознаграждения", "в размере платы", "стоимости услуг". A live run
  // had «вознаграждения» masked as an amount three times, so the model could
  // no longer see that the Board's cap and the contract's cap were measured
  // the same way. Only a value made wholly of these listed forms is empty: a
  // figure, a number word or a currency keeps it an amount.
  if (type == EntityType::MoneyAmount) {
    static const std::set<std::string> feeForms = [] {
      const std::vector<std::pair<std::string, std::vector<std::string>>> groups{
        {"вознаграждени", {"е", "я", "ю", "ем", "и", "й", "ям", "ями", "ях"}},
        {"оплат", {"а", "ы", "е", "у", "ой", "ою"}},
        {"плат", {"а", "ы", "е", "у", "ой", "ою"}},
        {"стоимост", {"ь", "и", "ью"}},
        {"цен", {"а", "ы", "е", "у", "ой", "ою"}},
        {"сумм", {"а", "ы", "е", "у", "ой", "ою"}},
        {"расход", {"ы", "ов", "ам", "ами", "ах"}},
        {"убытк", {"и", "ов", "ам", "ами", "ах"}},
        {"услуг", {"", "и", "ам", "ами", "ах"}},
      };
      std::set<std::string> forms{"уплаченного", "уплаченной", "уплаченных", "совокупного", "совокупной",
        "общей", "общего", "договорной", "договорного", "фиксированной", "фиксированного", "за", "по"};
      for (const auto &[stem, endings] : groups)
        for (const std::string &ending : endings) forms.insert(stem + ending);
      return forms;
    }();
    std::vector<std::string> words;
    std::string word;
    const auto take = [&] {
      const std::string form = normalized_entity_form(word);
      if (!form.empty()) words.push_back(form);
      word.clear();
    };
    for (const char character : value) {
      if (character == ' ' || character == '-' || character == '\t' || character == '\n') take();
      else word.push_back(character);
    }
    take();
    if (!words.empty() && std::all_of(words.begin(), words.end(),
        [&](const std::string &form) { return feeForms.count(form) > 0; })
        && std::any_of(words.begin(), words.end(), [&](const std::string &form) {
          return form != "за" && form != "по" && form.rfind("уплаченн", 0) != 0 && form.rfind("совокупн", 0) != 0
              && form.rfind("общ", 0) != 0 && form.rfind("договорн", 0) != 0 && form.rfind("фиксированн", 0) != 0; }))
      return true;
  }
  // The same rule in Russian, decided by what the words are and never by
  // listing phrases: "общая сумма требований", "стоимость услуг по Заявке" and
  // "дата претензии" are empty (a live Russian matter sent the first as an
  // amount, 24 Sep 2026), while a word that counts, names a currency or a
  // month, or a currency sign, keeps a value. Another script may write a
  // number this does not know, so only Latin and Cyrillic are judged.
  std::string word;
  bool cyrillicCounts = false;
  const auto judge = [&] {
    if (!word.empty() && counts_or_dates_in_russian(normalized_entity_form(word), type == EntityType::Date))
      cyrillicCounts = true;
    word.clear();
  };
  std::size_t offset = 0;
  while (offset < value.size()) {
    utf8proc_int32_t codePoint = 0;
    const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
        static_cast<utf8proc_ssize_t>(value.size() - offset), &codePoint);
    if (consumed <= 0) return false;
    const auto category = utf8proc_category(codePoint);
    if (category == UTF8PROC_CATEGORY_SC) return false;
    const bool letter = category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL
        || category == UTF8PROC_CATEGORY_LT || category == UTF8PROC_CATEGORY_LM || category == UTF8PROC_CATEGORY_LO;
    const bool latin = codePoint < 0x80;
    const bool cyrillic = codePoint >= 0x0400 && codePoint <= 0x04FF;
    if (letter && !latin && !cyrillic) return false;
    if (cyrillic) word.append(value, offset, static_cast<std::size_t>(consumed));
    else judge();
    offset += static_cast<std::size_t>(consumed);
  }
  judge();
  return !cyrillicCounts;
}

// "Holt's consent", "the Sellers' obligations": the possessive ending belongs to
// the sentence, not to the name. Left inside the detected value it makes
// "Holt's" a different identity from "Holt"; trimmed, the symbol replaces the
// name alone and the text still reads "[[E1]]'s consent".
std::size_t length_without_possessive(const std::string &text, std::size_t start, std::size_t length) {
	const auto endsWith = [&](const char *suffix) {
		const std::size_t size = std::char_traits<char>::length(suffix);
		return length > size && text.compare(start + length - size, size, suffix) == 0;
	};
	if (endsWith("'s") || endsWith("'S")) return length - 2;
	if (endsWith("\xe2\x80\x99s") || endsWith("\xe2\x80\x99S")) return length - 4;
	if (endsWith("s'")) return length - 1;
	if (endsWith("s\xe2\x80\x99")) return length - 3;
	return length;
}

// "Meridia Clinical Systems support", "Ashgrove landlord": the checkpoint
// reaches past the name and takes the ordinary word the sentence continues
// with. Left inside the detected value it does two kinds of damage at once -
// the word disappears from the text the model reads, and the name comes back
// written with it in every answer and every quotation ("without Meridia
// Clinical Systems support's prior written consent", which is in no document).
// A trailing word is dropped only when it is written in lower case and the
// language has it as an ordinary word (see CommonWords.hpp): the name itself
// stays masked, and what is left readable is a dictionary word that names
// nobody. A capitalised last word - "Larkspur Clinics of Texas", "Bank of the
// West" - is part of the name and never touched.
//
// This only measures; both callers decide the cut the same way. A person's name
// is never cut: a surname is often a dictionary word, and "did john baker sign"
// sent "baker" in clear. Any other name is cut only where what is left ends in
// a capitalised word (ends_with_capitalised_word): written all in lowercase -
// "georgia power", "acme holdings" - the last word may be the company's own,
// and it stays inside the mask.
std::size_t length_without_trailing_ordinary_word(const std::string &text, std::size_t start,
    std::size_t length) {
	for (int trimmed = 0; trimmed < 4; ++trimmed) {
		const std::size_t end = start + length;
		std::size_t wordStart = end;
		while (wordStart > start) {
			const unsigned char character = static_cast<unsigned char>(text[wordStart - 1]);
			if (character == ' ' || character == '\t' || character == '\n' || character == '\r') break;
			--wordStart;
		}
		if (wordStart == start || wordStart == end) return length;
		const std::string word = text.substr(wordStart, end - wordStart);
		const bool lowerCaseWord = std::all_of(word.begin(), word.end(), [](unsigned char character) {
			return character < 0x80 && std::islower(character) != 0;
		});
		if (!lowerCaseWord || !is_common_word(word)) return length;
		std::size_t cut = wordStart;
		while (cut > start) {
			const unsigned char character = static_cast<unsigned char>(text[cut - 1]);
			if (character != ' ' && character != '\t' && character != '\n' && character != '\r') break;
			--cut;
		}
		if (cut == start) return length;
		length = cut - start;
	}
	return length;
}

// Whether the last word of text[start, start + length) begins with a capital
// letter - "Meridia Clinical Systems", "Ashgrove", "Acme, Inc." - which is how
// a text shows that a name ends there. Plain ASCII only: anything else leaves
// the name whole.
bool ends_with_capitalised_word(const std::string &text, std::size_t start, std::size_t length) {
	const std::size_t end = start + length;
	std::size_t wordStart = end;
	while (wordStart > start) {
		const unsigned char character = static_cast<unsigned char>(text[wordStart - 1]);
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') break;
		--wordStart;
	}
	return wordStart < end && text[wordStart] >= 'A' && text[wordStart] <= 'Z';
}

// A length of time - "twelve months", "sixty (60) days", "12 месяцев" - is
// how an agreement works, not an amount of money or a date that identifies
// anyone. The checkpoint tags it as either; masked, the model cannot compare a
// twelve-month cap with a six-month one (a real run, 22 Sep 2026). Only when
// the value names a unit of time and nothing that prices or dates it: any
// currency, month name or year keeps it protected.
bool is_bare_duration(EntityType type, const std::string &value) {
  if (type != EntityType::MoneyAmount && type != EntityType::Date) return false;
  static const std::set<std::string> units{
      "day", "days", "week", "weeks", "month", "months", "year", "years", "hour", "hours", "quarter", "quarters",
  };
  static const std::vector<std::string> russianUnits{"дн", "ден", "недел", "месяц", "лет", "год"};
  static const std::set<std::string> blockers{
      "usd", "eur", "gbp", "rub", "chf", "jpy", "cny", "dollar", "dollars", "euro", "euros", "pound", "pounds",
      "ruble", "rubles", "rouble", "roubles", "yen", "yuan", "january", "february", "march", "april", "may", "june",
      "july", "august", "september", "october", "november", "december",
      // A rate is an amount paid per period, not a period: "12,500 per month".
      "per", "a", "an", "each", "every", "monthly", "annually", "yearly", "weekly", "daily", "в", "за",
      "ежемесячно", "ежегодно", "еженедельно",
  };
  static const std::vector<std::string> russianBlockers{"рубл", "руб", "доллар", "долл", "евро", "январ", "феврал",
      "март", "апрел", "мая", "май", "июн", "июл", "август", "сентябр", "октябр", "ноябр", "декабр"};
  for (const char *sign : {"$", "\xe2\x82\xac", "\xc2\xa3", "\xc2\xa5", "\xe2\x82\xbd"})
    if (value.find(sign) != std::string::npos) return false;
  bool unit = false;
  std::string word;
  const auto judge = [&]() -> bool {
    const std::string form = normalized_entity_form(word);
    word.clear();
    if (form.empty()) return true;
    if (std::all_of(form.begin(), form.end(), [](unsigned char character) { return std::isdigit(character) != 0; })) {
      if (form.size() == 4) return false;  // A year dates it.
      return true;
    }
    if (blockers.count(form) > 0) return false;
    for (const std::string &stem : russianBlockers) if (form.compare(0, stem.size(), stem) == 0) return false;
    if (units.count(form) > 0) unit = true;
    for (const std::string &stem : russianUnits) if (form.compare(0, stem.size(), stem) == 0) unit = true;
    return true;
  };
  for (const char character : value) {
    if (character == ' ' || character == '-' || character == '(' || character == ')' || character == ',') {
      if (!judge()) return false;
    } else {
      word.push_back(character);
    }
  }
  if (!judge()) return false;
  return unit;
}

// Where a value being judged comes from: detected in the text in hand, where it
// is written exactly as it stands, or recorded earlier as a Project registry
// alias, which is found however the text happens to type it.
enum class ValueOrigin { Detected, RegistryAlias };

// The concepts an agreement is made of - liability, indemnity, termination -
// named as concepts. The checkpoint tagged "liability" in "was the current
// liability position approved?" as a legal entity (22 Sep 2026), and the answer
// came back "the liability (liability) position". A closed list, matched against
// the whole value: an ordinary word that is also a company's name ("apple",
// "square", "target", "stripe") is not on it and stays protected, so this never
// becomes "a lowercase dictionary word is never sensitive".
//
// A phrase on the list is the concept. One word alone can also be a company's
// short name - Covenant Logistics Group, Inc. ("Covenant") - and a text that
// means the company writes it with a capital, so a single word is the concept
// only where the text writes it in lowercase. A Project's registry alias is
// matched however it is typed, so its case says nothing: a single word there is
// never taken for the concept.
std::string ascii_lower(std::string value);
bool has_capital(const std::string &value);

bool is_legal_concept(const std::string &value, ValueOrigin origin) {
  static const std::set<std::string> concepts{
      "liability", "liabilities", "limitation of liability", "liability cap", "cap", "indemnity", "indemnities",
      "indemnification", "warranty", "warranties", "representation", "representations", "covenant", "covenants",
      "obligation", "obligations", "termination", "renewal", "confidentiality", "confidential information",
      "negligence", "gross negligence", "willful misconduct", "wilful misconduct", "fraud", "breach", "claim",
      "claims",
      "material breach", "damages", "consequential damages", "remedy", "remedies", "penalty", "penalties",
      "liquidated damages", "force majeure", "assignment", "notice", "notices", "default", "consent",
      "carve-out", "carve-outs", "exclusivity", "non-compete", "non-solicitation", "severability",
      // Where a side stands in a negotiation: "our previous position", "the
      // current liability position" (a real request, 23 Sep 2026, came back
      // as "previous Position" and "the liability cap (liability)").
      "position", "positions", "previous position", "prior position", "current position", "original position",
      "our position", "negotiating position", "fallback position", "liability position",
      // The steps of a dispute, named as steps (review, 23 Sep 2026: each was
      // masked in "schedule the deposition", "sign the waiver"). Capitalised
      // alone - "Waiver" as the short name of Waiver Holdings, Inc. - it stays
      // a name, as every single word here does.
      "deposition", "depositions", "waiver", "waivers", "lawsuit", "lawsuits", "complaint", "complaints",
      "motion to compel",
      // Boilerplate the checkpoint tags as legal entities in a draft's own
      // clauses (live review, 24 Sep 2026): "at law or in equity", "may be
      // executed in counterparts". Capitalised - "Equity" in a company's name -
      // it stays a name.
      "equity", "counterpart", "counterparts",
      // The covenants and mechanics of employment and deal documents, as the
      // lawyer writes them in a question (review, 24 Sep 2026).
      "noncompete", "non-compete covenant", "noncompetition", "non-competition", "nonsolicitation",
      "non-solicitation covenant", "garden leave", "severance", "severance pay", "escrow", "earn-out", "earnout",
      "self-help", "set-off", "setoff", "holdover", "cure period", "notice period",
      // What a party may seek for a breach, named as a remedy. A live draft
      // (25 Sep 2026) sent "injunctive relief, equitable relief or specific
      // performance" as three legal entities' symbols - "injunctive" alone
      // as a fourth - the reviewer wrote around them, and the correction
      // came back reading "injunctive relief, injunctive relief".
      "relief", "injunctive relief", "injunction", "injunctions", "injunctive", "preliminary injunction",
      "permanent injunction", "temporary restraining order", "equitable relief", "equitable remedy",
      "equitable remedies", "specific performance", "specific relief", "declaratory relief",
      "declaratory judgment", "declaratory judgement", "interim relief", "interim measures", "provisional remedies",
      "restitution", "rescission", "reformation", "disgorgement", "right", "rights", "rights and remedies",
      "remedies at law", "remedy at law", "adequate remedy at law", "monetary remedy", "monetary remedies",
      "exclusive remedy", "sole remedy", "sole and exclusive remedy", "cumulative remedies", "actual damages",
      "direct damages", "indirect damages", "incidental damages", "special damages", "punitive damages",
      "exemplary damages", "compensatory damages", "monetary damages", "money damages", "lost profits",
      "loss of profits", "diminution in value", "attorneys' fees", "attorneys fees", "attorney fees",
      "attorneys\xe2\x80\x99 fees", "legal fees", "court costs", "costs", "interest", "late fees",
      "termination for cause", "termination for convenience", "right to terminate", "right of termination",
      "hold harmless", "duty to defend", "defense and indemnity", "mitigation", "duty to mitigate",
      "subrogation", "breach of contract", "breach of warranty", "breach of fiduciary duty", "fiduciary duty",
      "fiduciary duties", "unjust enrichment", "promissory estoppel", "estoppel", "good faith", "bad faith",
      "best efforts", "reasonable efforts", "commercially reasonable efforts",
      // A litigation's papers, steps and defences named as such (P0, 30 Oct
      // 2026: a pleading's headings went out as companies' symbols).
      "affirmative defense", "affirmative defenses", "affirmative defence", "affirmative defences",
      "protective order", "scheduling order", "case management order", "federal rules of civil procedure",
      "federal rule of civil procedure", "federal rules of evidence", "local rules", "local civil rules",
      "expert discovery", "fact discovery", "summary judgment", "motion to dismiss", "motion for summary judgment",
      "prayer for relief", "jury demand", "demand for jury trial", "comparative fault", "comparative negligence",
      "contributory negligence", "failure to mitigate", "statute of limitations", "statute of frauds",
      "unclean hands", "accord and satisfaction", "bates number", "bates numbers", "bates range", "bates ranges",
      "third-party complaint", "amended complaint", "first amended complaint", "second amended complaint",
      "answer and counterclaims", "answer and affirmative defenses", "reply to counterclaims",
      "reply to the counterclaims", "initial disclosures", "requests for production", "requests for admission",
      "document requests",
  };
  const std::string term = bare_term(value);
  if (concepts.count(term) > 0) {
    if (term.find(' ') != std::string::npos) return true;
    if (origin == ValueOrigin::RegistryAlias) return false;
    return std::none_of(value.begin(), value.end(), [](char character) { return character >= 'A' && character <= 'Z'; });
  }
  // The same concepts listed together, which the checkpoint takes as one
  // value: "injunctive relief, equitable relief or specific performance".
  // Every item must be one, as the item would be judged alone.
  static const std::vector<std::string> separators{", and/or ", ", and ", ", or ", " and/or ", " and ", " or ",
      ", ", "; ", "/"};
  std::vector<std::string> items;
  std::size_t from = 0;
  while (from <= term.size()) {
    std::size_t cut = std::string::npos;
    std::size_t cutLength = 0;
    for (const std::string &separator : separators) {
      const std::size_t found = term.find(separator, from);
      if (found != std::string::npos && (cut == std::string::npos || found < cut)) {
        cut = found;
        cutLength = separator.size();
      }
    }
    items.push_back(term.substr(from, cut == std::string::npos ? std::string::npos : cut - from));
    if (cut == std::string::npos) break;
    from = cut + cutLength;
  }
  if (items.size() < 2) return false;
  for (std::string item : items) {
    for (const char *lead : {"other ", "any ", "all ", "its ", "their ", "such ", "the "})
      if (item.rfind(lead, 0) == 0) item.erase(0, std::char_traits<char>::length(lead));
    if (concepts.count(item) == 0) return false;
    if (item.find(' ') != std::string::npos) continue;
    if (origin == ValueOrigin::RegistryAlias) return false;
    // A single word is the concept only in lower case, as above.
    const std::size_t at = ascii_lower(value).find(item);
    if (at == std::string::npos || has_capital(value.substr(at, item.size()))) return false;
  }
  return true;
}

// Public legal vocabulary the PII checkpoint mistakes for something to hide.
// Everything here is decided from the detected value alone, by closed lists
// and exact shapes - never a guess about a name.
// A clause the detector took for a name: nothing in it written with a
// capital, no quotation marks round a name, and it tells a time of day -
// "заседание закрыто в 17:40.", "the meeting closed at 5:40 p.m."
// A name of an organisation or a court is written as a name; one written all
// in lowercase stays masked unless it reads as a clause like these.
bool reads_as_clause_not_name(const std::string &value) {
  bool capital = false;
  bool quoted = false;
  std::size_t offset = 0;
  while (offset < value.size()) {
    utf8proc_int32_t codePoint = 0;
    const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
        static_cast<utf8proc_ssize_t>(value.size() - offset), &codePoint);
    if (consumed <= 0) return false;
    const auto category = utf8proc_category(codePoint);
    if (category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LT) capital = true;
    if (category == UTF8PROC_CATEGORY_PI || category == UTF8PROC_CATEGORY_PF || codePoint == '"' || codePoint == 0xAB
        || codePoint == 0xBB) quoted = true;
    offset += static_cast<std::size_t>(consumed);
  }
  if (capital || quoted || value.find(' ') == std::string::npos) return false;
  bool clockTime = false;
  for (std::size_t index = 1; index + 2 < value.size(); ++index)
    if (value[index] == ':' && std::isdigit(static_cast<unsigned char>(value[index - 1])) != 0
        && std::isdigit(static_cast<unsigned char>(value[index + 1])) != 0
        && std::isdigit(static_cast<unsigned char>(value[index + 2])) != 0)
      clockTime = true;
  // A time of day, not a full stop: "acme holdings ltd." ends a sentence too,
  // and is a company.
  return clockTime;
}

bool has_capital(const std::string &value);

// A defined term an agreement capitalises because it defines it, naming no
// party and no place: "the Premises", "the Restricted Period", "each Lease
// Year". Matched whole against a closed list, so "Premises LLC" or a company
// called "Term" in a longer name is never reached.
bool is_defined_legal_term(const std::string &value) {
  static const std::set<std::string> terms{
      "premises", "restricted period", "lease year", "lease years", "restricted territory", "territory",
      "noncompetition period", "non-competition period", "nonsolicitation period", "non-solicitation period",
      "severance period", "garden leave period", "earn-out period", "earnout period", "initial term",
      "renewal term", "extension term", "term", "cure period", "notice period", "confidential information",
      "restricted business", "competing business", "base rent", "additional rent", "security deposit",
      // The concepts a deal document capitalises as its defined terms: "The
      // Severance is payable", "The Earn-Out was never paid", "Escrow closes
      // Friday" (a verifier's run, 25 Sep 2026, sent each as a company).
      "severance", "severance pay", "indemnity", "indemnification", "escrow", "earn-out", "earnout",
      "noncompete", "non-compete", "noncompetition", "non-competition", "nonsolicitation", "non-solicitation",
      "garden leave", "liquidated damages", "self-help",
      // The defined terms of a US deal, capitalised because the agreement defines
      // them (Larkspur data room, 11 Oct 2026: each went to the model as a company
      // or an entity). One word of these is not a company's name either.
      "common stock", "preferred stock", "capital stock", "series a preferred", "series a preferred stock",
      "series b preferred", "series b preferred stock", "purchase price", "closing", "closing date",
      "effective date", "material adverse effect", "material adverse change", "fundamental representations",
      "health care representations", "healthcare representations", "third-party claims", "third party claims",
      "losses", "indebtedness", "closing indebtedness", "lien", "liens", "permitted liens", "permits",
      "material permits", "material contract", "material contracts", "real property", "leased real property",
      "owned real property", "intellectual property", "company intellectual property", "legal proceedings",
      "proceeding", "proceedings", "special indemnity", "special indemnity matters", "special indemnity escrow",
      "indemnity escrow", "escrow amount", "irrevocable proxy", "spousal consent", "fraud", "order", "orders",
      "governmental authority", "governmental order", "transactions", "disclosure schedule",
      "disclosure schedules", "data room", "virtual data room", "key employee", "key employees",
      "human resources", "compensation committee", "audit committee", "audit and compliance committee",
      "change of control", "change in control", "net working capital",
      "working capital", "affiliates", "subsidiaries", "taxes", "tax returns", "benefit plans",
      "company benefit plan", "company benefit plans", "employee plans", "privacy laws", "security incident",
      "personal information", "protected health information", "business associate", "covered entity",
      "federal health care program", "federal health care programs", "government reimbursement programs",
      "insurance policies", "r&w policy", "r&w insurance policy", "d&o tail policy", "tail policy",
      "required consents", "third-party consents", "recapture right", "recapture waiver", "exclusivity period",
      "outside date", "termination date", "interim period", "ordinary course", "ordinary course of business",
      "accounting principles", "financial statements", "balance sheet", "balance sheet date",
      "closing statement", "estimated closing statement", "payoff letter", "payoff letters", "escrow agent",
      "paying agent", "seller representative", "sellers' representative", "buyer indemnified parties",
      "seller indemnified parties", "indemnified parties", "deductible", "survival period",
      "pre-closing taxes", "transaction expenses", "company transaction expenses", "closing cash",
      "equity commitment letter", "debt commitment letter", "commitment letter", "anti-assignment",
      "confidentiality", "estoppel", "estoppels",
  };
  return terms.count(bare_term(value)) > 0;
}

// A document's title with its number in a series, the number cut off: "Amendment
// No. 2" is "Amendment", «Дополнительное соглашение № 2» is «Дополнительное
// соглашение». The value itself when no such number ends it; a code - "No.
// NW-17" - is no number in a series.
std::string without_number_in_series(const std::string &value) {
  static const std::regex numbered("^(.*[^ ]) +(?:[Nn][Oo]\\.?|[Nn]umber|#|\xe2\x84\x96) *[0-9]{1,3}$");
  std::smatch match;
  if (!std::regex_match(value, match, numbered)) return value;
  return match[1].str();
}

// An officer's or a member's title, which names nobody: "Managing Member",
// "Chief Financial Officer", "General Counsel", "Vice President of Finance".
// Every word a title's, and one of them the office itself.
bool is_officer_title(const std::string &value) {
  static const std::set<std::string> offices{
      "officer", "counsel", "member", "director", "manager", "president", "secretary", "treasurer", "partner",
      "chair", "chairman", "chairwoman", "chairperson", "controller", "comptroller", "trustee", "principal",
      // A court's offices: "United States District Judge" names no judge.
      "judge", "justice", "magistrate",
  };
  static const std::set<std::string> qualifiers{
      "chief", "executive", "financial", "operating", "legal", "technology", "information", "compliance",
      "general", "managing", "vice", "senior", "executive", "assistant", "deputy", "associate", "sole",
      "independent", "non-executive", "lead", "limited", "of", "finance", "operations", "the", "board",
      "human", "resources", "marketing", "revenue", "accounting", "risk", "privacy", "security", "medical",
      // The functions an officer heads (#245: "its Vice President of Supply Chain").
      "and", "supply", "chain", "procurement", "purchasing", "sales", "engineering", "product", "products",
      "research", "development", "strategy", "business", "corporate", "communications", "investor", "relations",
      "people", "talent", "commercial", "data", "digital", "customer", "manufacturing", "quality", "regulatory",
      "affairs", "government", "policy", "administrative", "administration", "planning", "facilities", "logistics",
      "treasury", "tax", "internal", "audit", "materials", "programs", "program", "scientific", "clinical",
      "contracts",
      // What a manager or a director manages (round 5: "Attention: Asset
      // Manager", "its Senior Portfolio Manager", "the Leasing Director").
      "asset", "assets", "property", "portfolio", "leasing", "lease", "project", "projects", "account", "accounts",
      "office", "branch", "regional", "region", "area", "plant", "site", "store", "facility", "fund", "investment",
      "investments", "client", "relationship", "construction", "real", "estate", "transaction", "transactions",
      // The court a judge sits on, by its kind and level.
      "united", "states", "district", "circuit", "bankruptcy", "presiding", "appellate", "superior", "supreme",
      "federal", "state", "county", "court",
  };
  std::string term = bare_term(value);
  // "its Vice President", "our General Counsel": whose officer it is changes nothing.
  for (const char *whose : {"its ", "their ", "our ", "his ", "her ", "your ", "my "}) {
    const std::string prefix(whose);
    if (term.compare(0, prefix.size(), prefix) == 0) { term.erase(0, prefix.size()); break; }
  }
  if (term.find(' ') == std::string::npos) return false;
  bool office = false;
  std::string word;
  for (std::size_t index = 0; index <= term.size(); ++index) {
    if (index == term.size() || term[index] == ' ') {
      if (!word.empty()) {
        if (offices.count(word) > 0) office = true;
        else if (qualifiers.count(word) == 0) return false;
      }
      word.clear();
    } else {
      word.push_back(term[index]);
    }
  }
  return office;
}

// A public court named by its kind and its seat - «Арбитражный суд города
// Москвы» in any case, "United States District Court for the Southern District
// of New York", "the Delaware Court of Chancery". A court decides the dispute,
// it is not a party to it; a private tribunal "при ООО «…»" or anything with a
// figure or a legal form stays masked.
bool is_public_court(const std::string &value) {
  if (contains_digit(value) || value.find("\xc2\xab") != std::string::npos || value.find('"') != std::string::npos)
    return false;
  // A jurisdiction's courts by the jurisdiction alone: "the courts of England
  // and Wales", "Delaware courts" (#245).
  {
    const std::string term = bare_term(value);
    for (const char *courts : {"courts of the ", "courts of ", "court of the ", "court of "})
      if (term.rfind(courts, 0) == 0 && is_public_jurisdiction(term.substr(std::char_traits<char>::length(courts))))
        return true;
    for (const char *courts : {" courts", " court"}) {
      const std::size_t size = std::char_traits<char>::length(courts);
      if (term.size() > size && term.compare(term.size() - size, size, courts) == 0
          && is_public_jurisdiction(term.substr(0, term.size() - size)))
        return true;
    }
  }
  std::vector<std::string> words;
  std::string word;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index == value.size() || value[index] == ' ' || value[index] == ',' || value[index] == '-') {
      const std::string form = normalized_entity_form(word);
      if (!form.empty()) words.push_back(form);
      word.clear();
    } else {
      word.push_back(value[index]);
    }
  }
  static const std::set<std::string> legalForms{"ооо", "оао", "зао", "пао", "ао", "llc", "inc", "ltd", "corp", "lp", "llp"};
  bool court = false;
  bool kind = false;
  static const std::vector<std::string> russianKinds{"арбитражн", "апелляционн", "кассационн", "верховн",
      "конституционн", "районн", "городск", "областн", "краев", "мирово", "окружн", "гарнизонн", "военн",
      "межрайонн"};
  static const std::set<std::string> englishKinds{"supreme", "district", "superior", "circuit", "chancery", "appeals",
      "appellate", "bankruptcy", "federal", "county", "high", "crown", "magistrates", "tax", "claims", "family",
      "probate", "commercial", "civil", "criminal", "municipal"};
  for (const std::string &form : words) {
    if (legalForms.count(form) > 0) return false;
    if (form == "суд" || form == "суда" || form == "суду" || form == "судом" || form == "суде" || form == "court")
      court = true;
    if (englishKinds.count(form) > 0) kind = true;
    for (const std::string &stem : russianKinds)
      if (form.compare(0, stem.size(), stem) == 0) kind = true;
  }
  return court && kind;
}

// How a value is written as a workbook formula: `shape` when it is nothing but
// cell references, numbers and operators, an operator between any two of them
// ("B8-C8-D8", "B7/B5*1.05", "SUM(B2:B10)"); `plainly` when that could be
// nothing else - written after "=", with a function, an anchor ("$B$12"), a
// range or arithmetic an identifier never has. A code is written with letters
// and digits and a hyphen or a slash too: "INV2026-001", "PO12345", and "Q3
// 2026" is a quarter, so a shape alone is a formula only where the text writes
// it as one (written_as_formula_at()).
struct FormulaShape {
  bool shape = false;
  bool plainly = false;
};

FormulaShape formula_shape(const std::string &value) {
  std::string text = value;
  while (!text.empty() && (text.back() == ' ' || text.back() == '}')) text.pop_back();
  std::size_t from = 0;
  bool opened = false;
  while (from < text.size() && (text[from] == ' ' || text[from] == '{' || text[from] == '=')) {
    if (text[from] != ' ') opened = true;
    ++from;
  }
  text = text.substr(from);
  if (text.empty()) return {};
  // Every operand stands whole: "DE123456789" is not the reference DE1234567
  // with more digits after it.
  static const std::regex formula(
      "^(?:[A-Z][A-Z0-9.]*\\(|\\$?[A-Z]{1,3}\\$?[0-9]{1,7}(?![0-9A-Za-z])|[0-9]+(?:\\.[0-9]+)?(?![0-9A-Za-z])"
      "|[-+*/^:(),!&<>= %])+$");
  static const std::regex reference("\\$?[A-Z]{1,3}\\$?[0-9]{1,7}");
  // Two operands side by side with nothing but a space between them are not
  // one formula: "Q3 2026".
  static const std::regex adjacent("[A-Za-z0-9)] +[A-Za-z0-9$(]");
  if (!std::regex_match(text, formula) || std::regex_search(text, adjacent)) return {};
  const auto references = std::distance(std::sregex_iterator(text.begin(), text.end(), reference), std::sregex_iterator());
  if (references == 0) return {};
  FormulaShape shape;
  const bool function = std::regex_search(text, std::regex("[A-Z][A-Z0-9.]*\\("));
  const bool arithmetic = text.find_first_of("+*^:()!&<>=%") != std::string::npos;
  const bool joined = text.find_first_of("-/") != std::string::npos;
  shape.shape = references >= 2 ? (joined || arithmetic) : true;
  shape.plainly = opened || function || text.find('$') != std::string::npos || (references >= 1 && arithmetic);
  if (!shape.shape) shape.plainly = false;
  return shape;
}

// A workbook's formula whatever the checkpoint took it for: "{=B7/B5}",
// "$B$12", "SUM(B2:B10)", "=B8-C8-D8". Judged from the value alone, so only
// what could be nothing else.
bool is_cell_formula(const std::string &value) {
  return formula_shape(value).plainly;
}

// Cell references prose names as the checkpoint's organisation: "Net is
// B8-C8-D8 in the model", "The formula B7/B5 is wrong", "See B12 for the
// rate" (a verifier's run, 25 Sep 2026, sent each as a symbol). References
// joined as a formula joins them, or one reference alone with a one-letter
// column - "PO12345", "INV2026" are codes and stay masked.
bool written_as_cell_references(const std::string &value) {
  static const std::regex chain("^\\$?[A-Z]{1,3}\\$?[0-9]{1,7}(?: ?[-+*/] ?\\$?[A-Z]{1,3}\\$?[0-9]{1,7})+$");
  static const std::regex single("^\\$?[A-Z]\\$?[0-9]{1,5}$");
  return std::regex_match(value, chain) || std::regex_match(value, single);
}

// What the lawyer calls the work itself, in lower case in a request: "the
// draft", "this memo", "the redline". A closed list; a capital keeps a name.
bool is_request_work_word(const std::string &value) {
  static const std::set<std::string> words{
      "draft", "drafts", "memo", "memos", "memorandum", "summary", "table", "spreadsheet", "workbook", "redline",
      "redlines", "markup", "letter", "email", "e-mail", "chronology", "timeline", "analysis", "answer",
      "question", "review", "comments", "clause", "section", "paragraph", "document", "file", "folder", "request",
      "report", "brief", "outline", "checklist", "schedule",
  };
  return !has_capital(value) && words.count(bare_term(value)) > 0;
}

// The word Singularity writes, in capitals, before its own lines to a model:
// "SINGULARITY CONTINUATION:", "SINGULARITY REQUIRED NEXT STEP:". Only that
// form is its own. "Singularity" written as a name is a name like any other,
// so a party a Matter calls so keeps its mask (D-186).
bool is_singularity_marker(const std::string &value) {
  return bare_term(value) == "singularity"
      && std::none_of(value.begin(), value.end(), [](char character) { return character >= 'a' && character <= 'z'; });
}

// Singularity's own names for the parts of a document it reads ("table-0",
// "column-2", "row-3"): how a change says which table, column or row it acts
// on. They name nothing in the world, and a checkpoint that takes one for a
// contract's identifier turns the change's own address into a symbol (Golden
// C, 25 Sep 2026). Only this exact shape: "NW-17" or "Table-A7" stay codes.
bool is_singularity_structural_handle(const std::string &value) {
  static const std::regex handle("^(?:table|column|row)-[0-9]{1,4}$");
  return std::regex_match(value, handle);
}

// Singularity's own name for a part of an agreement, where Singularity writes
// it: "[s-5.2]" in front of a clause of the material, "\"x-A-p-1\"" in its own
// instructions (DocumentIntelligence/LegalStructure names every block so: the
// title, s-, h- and p- for clauses, headings and paragraphs, x- for an
// exhibit and its parts, c- for a comment, fn- and en- for notes, hf- for a
// page header or footer, sig- for a signature page and t- for a table). It
// names no one; the same letters in the agreement's own words - "order p-12" -
// are the agreement's and keep their rules.
bool stands_as_structural_id(const std::string &text, std::size_t start, std::size_t length) {
  static const std::regex id(
      "^(?:title|x-[A-Z0-9]{1,3}(?:-(?:title|s-[0-9A-Za-z().]{1,12}|p-[0-9]{1,4}))?|(?:s|h)-[0-9A-Za-z().]{1,12}"
      "|(?:p|c|fn|en|hf|sig|t)-[0-9]{1,5})(?:~[0-9]{1,3})?$");
  if (length == 0 || start + length > text.size()) return false;
  if (std::regex_match(text.substr(start, length), id) == false) return false;
  if (start == 0 || start + length >= text.size()) return false;
  const char before = text[start - 1];
  const char after = text[start + length];
  return (before == '[' && after == ']') || (before == '"' && after == '"');
}

// A figure followed by a unit of time is a period, not money: "within fifteen
// (15) days", "sixty (60) business days". A live supply agreement's payment
// terms came back from the checkpoint as money amounts (24 Oct 2026), and the
// review could no longer see them. A figure with a currency of its own stays
// money whatever follows it.
bool reads_as_period(const std::string &text, std::size_t start, std::size_t length) {
  if (length == 0 || start + length > text.size()) return false;
  const std::string value = text.substr(start, length);
  for (const char *sign : {"$", "\xe2\x82\xac", "\xc2\xa3", "\xc2\xa5", "\xe2\x82\xbd"})
    if (value.find(sign) != std::string::npos) return false;
  std::string lowered = value;
  for (char &character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  for (const char *currency : {"usd", "eur", "gbp", "dollar", "euro", "pound", "cent", "rub", "руб"})
    if (lowered.find(currency) != std::string::npos) return false;
  // The words after it, as they stand: a qualifier, then the unit.
  std::size_t at = start + length;
  const auto nextWord = [&]() {
    while (at < text.size() && (text[at] == ' ' || text[at] == ')')) ++at;
    const std::size_t from = at;
    while (at < text.size() && std::isalpha(static_cast<unsigned char>(text[at])) != 0) ++at;
    std::string word = text.substr(from, at - from);
    for (char &character : word) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return word;
  };
  static const std::set<std::string> units{"day", "days", "week", "weeks", "month", "months", "year", "years",
      "hour", "hours", "quarter", "quarters"};
  static const std::set<std::string> qualifiers{"business", "calendar", "working", "banking", "consecutive", "full"};
  std::string word = nextWord();
  if (qualifiers.count(word) > 0) word = nextWord();
  return units.count(word) > 0;
}

std::string plain_phrase(const std::string &value);
bool ordinary_lowercase_words(const std::string &phrase);

// A deal's code name keeps its mask however it is written: "project falcon" is
// the deal a lawyer types in a hurry, never prose.
bool written_as_code_name(const std::string &value) {
  const std::string term = bare_term(value);
  for (const char *marker : {"project ", "operation ", "codename ", "code name "})
    if (term.rfind(marker, 0) == 0) return true;
  return false;
}

// Prose a document writes in lower case in ordinary words, whatever the
// checkpoint tagged it: "the data room", "common areas", "a security interest",
// "letters of credit", "a limited liability company", "license". A name in a
// document is written with a capital; a thirty-document US data room (11 Oct
// 2026) sent some seventy such phrases to the model as companies', places' and
// identifiers' symbols, and the registry kept every one of them masked in every
// later request. Asked only of a document's own text or of a registry alias the
// documents wrote so - a request typed in a hurry ("what did blue river say?")
// keeps its own rules - never of a person, and never of a deal's code name.
bool written_as_lowercase_prose(const std::string &value) {
  if (has_capital(value) || written_as_code_name(value)) return false;
  const std::string plain = plain_phrase(value);
  return !plain.empty() && ordinary_lowercase_words(plain);
}

// The kinds of value a phrase of prose can be mistaken for: a company, a legal
// entity, a place, a court, a judge, a contract's identifier - never a person,
// whose own rules stand, and never a figure, a date or an address.
bool prose_can_be_mistaken_for(EntityType type) {
  return type == EntityType::Organisation || type == EntityType::LegalEntity || type == EntityType::Location
      || type == EntityType::Court || type == EntityType::Judge || type == EntityType::ContractIdentifier;
}

// A public office or agency by what it is, with the jurisdiction it serves if
// any: "the Attorney General", "Texas Attorney General", "U.S. Attorney's
// Office", "United States Department of Justice", "California Franchise Tax
// Board", "Secretary of Health and Human Services". It acts in a matter and is
// named in the public record; masked, a diligence question about a
// government investigation reached the model as one about a party.
bool is_public_office(const std::string &value) {
  static const std::set<std::string> offices{
      "attorney general", "attorney general's office", "office of the attorney general", "attorney's office",
      "attorneys office", "u.s. attorney's office", "united states attorney's office", "department of justice",
      "office for civil rights", "office of inspector general", "office of the inspector general",
      "secretary of state", "secretary of health and human services", "department of health and human services",
      "internal revenue service", "securities and exchange commission", "federal trade commission",
      "federal bureau of investigation", "franchise tax board", "comptroller of public accounts",
      "department of revenue", "department of insurance", "department of health", "department of labor",
      "department of state", "division of corporations", "medical board", "board of dental examiners",
      "centers for medicare & medicaid services", "centers for medicare and medicaid services",
      "equal employment opportunity commission", "health and human services",
      // Data protection authorities (#245).
      "data protection commission", "data protection authority", "data protection commissioner",
      "information commissioner", "information commissioner's office", "supervisory authority",
      "european data protection board", "consumer financial protection bureau", "california privacy protection agency",
      "national labor relations board", "department of commerce", "federal communications commission",
      "food and drug administration", "environmental protection agency", "patent and trademark office",
      "u.s. patent and trademark office", "united states patent and trademark office", "copyright office",
  };
  std::string term = bare_term(value);
  if (offices.count(term) > 0) return true;
  // The jurisdiction the office serves before it: "Texas Attorney General",
  // "United States Department of Justice", "U.S. Attorney's Office".
  for (const std::string &office : offices) {
    if (term.size() <= office.size() + 1 || term.compare(term.size() - office.size(), office.size(), office) != 0
        || term[term.size() - office.size() - 1] != ' ')
      continue;
    const std::string served = term.substr(0, term.size() - office.size() - 1);
    if (served == "u.s." || served == "us" || served == "federal" || is_public_jurisdiction(served)) return true;
  }
  return false;
}

// Whether the words at [position, position + length) of `text` stand inside a
// public or generic phrase - a jurisdiction, a public office or a court ("the
// State of Delaware", "Secretary of State", "the United States District Court
// for the Western District of Texas"), or the title of a kind of paper or a
// defined term ("Trademark License Agreement", "Series A Preferred"). There the
// words are that phrase, whatever else the same word names: a client may be
// called "State" and still "the State of Texas" is Texas, and a word recorded
// alone ("Trademark") is not masked out of the agreement's title. A party's name
// in a title is no title's word: "Cascade Credit Agreement" keeps "Cascade". Asked of a detection, of a registry alias the lawyer never confirmed
// and of a leak check alike, so what is left unmasked is never then refused as a
// leak. Up to four words either side, on the same line.
bool written_as_state_of_formation(const std::string &text, std::size_t start, std::size_t length);

bool stands_in_public_phrase(const std::string &text, std::size_t position, std::size_t length) {
  if (length == 0 || position >= text.size() || length > text.size() - position) return false;
  // Where a party was formed: "a Delaware corporation" - the state, not a company.
  if (written_as_state_of_formation(text, position, length)) return true;
  const auto separates = [](char character) {
    return character == ' ' || character == '\t';
  };
  const auto stops = [](char character) {
    return character == '\n' || character == '\r' || character == ',' || character == ';' || character == ':'
        || character == '(' || character == ')' || character == '"' || character == '[' || character == ']';
  };
  // Word starts before the occurrence, nearest first, and word ends after it.
  std::vector<std::size_t> starts{position};
  std::size_t cursor = position;
  while (starts.size() < 5 && cursor > 0) {
    std::size_t back = cursor;
    while (back > 0 && separates(text[back - 1])) --back;
    if (back == 0 || back == cursor || stops(text[back - 1])) break;
    std::size_t wordStart = back;
    while (wordStart > 0 && !separates(text[wordStart - 1]) && !stops(text[wordStart - 1])) --wordStart;
    starts.push_back(wordStart);
    cursor = wordStart;
  }
  std::vector<std::size_t> ends{position + length};
  cursor = position + length;
  while (ends.size() < 5 && cursor < text.size()) {
    std::size_t ahead = cursor;
    while (ahead < text.size() && separates(text[ahead])) ++ahead;
    if (ahead == cursor || ahead >= text.size() || stops(text[ahead])) break;
    std::size_t wordEnd = ahead;
    while (wordEnd < text.size() && !separates(text[wordEnd]) && !stops(text[wordEnd])) ++wordEnd;
    ends.push_back(wordEnd);
    cursor = wordEnd;
  }
  // Only a word that public phrases are made of beside it makes the phrases
  // worth reading - "of", "the" and a capital alone do not. Missing one here
  // only keeps a mask on.
  static const std::set<std::string> anchors{
      "state", "states", "united", "commonwealth", "republic", "kingdom", "province", "federal", "u.s.", "u.s",
      "us", "new", "north", "south", "west", "east", "district", "court", "courts", "attorney", "attorney's",
      "attorneys", "general", "office", "department", "secretary", "board", "service", "bureau", "commission",
      "comptroller", "revenue", "inspector", "division", "county", "city", "island", "islands", "virgin", "cayman",
      "hong", "kong", "rhode", "carolina", "dakota", "virginia", "hampshire", "jersey", "mexico", "york",
      "columbia", "zealand", "korea", "arabia", "emirates", "america", "britain", "wales", "england", "ireland",
      "scotland", "isle", "chancery", "supreme", "superior", "circuit", "appeals", "bankruptcy", "justice",
      "civil", "rights", "health", "human", "services", "internal", "trade", "tax", "franchise", "public",
      "accounts", "corporations", "labor", "insurance", "medical", "dental", "examiners", "exchange", "securities",
      "employment", "opportunity", "investigation", "law", "laws",
      // The heads of a paper's title and of a defined term.
      "agreement", "agreements", "policy", "policies", "preferred", "stock", "license", "licence", "lease",
      "certificate", "incorporation", "formation", "proxy", "consent", "indemnity", "escrow", "series", "property",
      "proceedings", "representations", "effect", "schedule", "schedules", "note", "letter", "amendment",
      "contract", "deed", "claims", "matters", "agent", "committee", "resources", "capital", "price", "date",
  };
  bool anchored = false;
  const auto lowered_word = [&](std::size_t from, std::size_t to) {
    std::string word = text.substr(from, to - from);
    for (char &character : word) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    while (!word.empty() && (word.back() == '.' || word.back() == '\'')) {
      if (word == "u.s." || word == "u.s") break;
      word.pop_back();
    }
    return word;
  };
  for (std::size_t index = 1; index < starts.size() && !anchored; ++index) {
    std::size_t to = starts[index];
    while (to < text.size() && !separates(text[to]) && !stops(text[to])) ++to;
    anchored = anchors.count(lowered_word(starts[index], to)) > 0;
  }
  for (std::size_t index = 1; index < ends.size() && !anchored; ++index) {
    std::size_t from = ends[index];
    while (from > 0 && !separates(text[from - 1]) && !stops(text[from - 1])) --from;
    anchored = anchors.count(lowered_word(from, ends[index])) > 0;
  }
  if (!anchored) return false;
  // What a window adds around the occurrence must hold a word of its own: "the
  // Washington" is the occurrence with an article, never a phrase it stands in.
  const auto adds_a_word = [&](std::size_t from, std::size_t to) {
    static const std::set<std::string> articles{"the", "a", "an", "each", "any", "such", "this", "that"};
    const auto has_word = [&](std::size_t begin, std::size_t end) {
      std::size_t cursor = begin;
      while (cursor < end) {
        while (cursor < end && separates(text[cursor])) ++cursor;
        std::size_t wordEnd = cursor;
        while (wordEnd < end && !separates(text[wordEnd])) ++wordEnd;
        if (wordEnd > cursor && articles.count(lowered_word(cursor, wordEnd)) == 0) return true;
        cursor = wordEnd;
      }
      return false;
    };
    return has_word(from, position) || has_word(position + length, to);
  };
  for (const std::size_t from : starts)
    for (const std::size_t to : ends) {
      if (from == position && to == position + length) continue;
      if (!adds_a_word(from, to)) continue;
      const std::string phrase = text.substr(from, to - from);
      if (is_public_jurisdiction(phrase) || is_public_office(phrase) || is_public_court(phrase)
          || is_generic_instrument_reference(phrase) || is_defined_legal_term(phrase))
        return true;
      // A jurisdiction's own law: "Delaware law", "Texas laws". "Cascade law" is
      // no jurisdiction's and keeps its mask.
      const std::string term = bare_term(phrase);
      for (const char *law : {" law", " laws"}) {
        const std::size_t size = std::char_traits<char>::length(law);
        if (term.size() > size && term.compare(term.size() - size, size, law) == 0
            && is_public_jurisdiction(term.substr(0, term.size() - size)))
          return true;
      }
    }
  return false;
}

// Which draft a file is: "v3", "v4.1". Names no one.
bool is_version_label(const std::string &value) {
  static const std::regex label("^[vV][0-9]{1,3}(?:\\.[0-9]{1,3})?$");
  return std::regex_match(bare_term(value), label);
}

bool only_grammar_words(const std::string &words);

// A US state's or territory's postal code, written in capitals as the Postal
// Service writes it: "IL", "IN", "DC".
bool is_us_state_code(const std::string &value) {
  static const std::set<std::string> codes{"AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "HI", "ID",
      "IL", "IN", "IA", "KS", "KY", "LA", "ME", "MD", "MA", "MI", "MN", "MS", "MO", "MT", "NE", "NV", "NH", "NJ",
      "NM", "NY", "NC", "ND", "OH", "OK", "OR", "PA", "RI", "SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA", "WV",
      "WI", "WY", "DC", "PR", "GU", "VI", "AS", "MP"};
  return codes.count(value) > 0;
}

// Whether a word begins with a capital letter, whatever comes before its first
// letter: "Brightwater", "“Coastline", «Ромашка», "Österreich".
bool starts_with_capital_letter(const std::string &word) {
  std::size_t offset = 0;
  while (offset < word.size()) {
    utf8proc_int32_t codePoint = 0;
    const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(word.data()) + offset,
        static_cast<utf8proc_ssize_t>(word.size() - offset), &codePoint);
    if (consumed <= 0) return false;
    const auto category = utf8proc_category(codePoint);
    if (category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LT) return true;
    if (category == UTF8PROC_CATEGORY_LL || category == UTF8PROC_CATEGORY_LM || category == UTF8PROC_CATEGORY_LO
        || category == UTF8PROC_CATEGORY_ND)
      return false;
    offset += static_cast<std::size_t>(consumed);
  }
  return false;
}

std::size_t times_in(const std::string &value, const std::string &mark) {
  std::size_t count = 0;
  for (std::size_t at = value.find(mark); at != std::string::npos; at = value.find(mark, at + mark.size())) ++count;
  return count;
}

// The opening and closing marks a name may hold in pairs, and never alone.
const std::vector<std::pair<std::string, std::string>> &paired_marks() {
  static const std::vector<std::pair<std::string, std::string>> pairs{{"[", "]"}, {"(", ")"}, {"{", "}"},
      {"\xE2\x80\x9C", "\xE2\x80\x9D"}, {"\xC2\xAB", "\xC2\xBB"}};
  return pairs;
}

// A name may hold a pair of brackets or quotes - "Coastline Logistics (East),
// Inc.", 'the "Fund"', ООО «Ромашка» - never half of one.
bool marks_paired(const std::string &value) {
  for (const auto &[open, close] : paired_marks())
    if (times_in(value, open) != times_in(value, close)) return false;
  return times_in(value, "\"") % 2 == 0;
}

// A value's words as written, split at spaces.
std::vector<std::string> spaced_words(const std::string &value) {
  std::vector<std::string> words;
  std::string word;
  for (const char character : value) {
    if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
      if (!word.empty()) words.push_back(word);
      word.clear();
    } else {
      word.push_back(character);
    }
  }
  if (!word.empty()) words.push_back(word);
  return words;
}

// A word without the marks and punctuation around it: "(East)," is "East",
// "denied.]" is "denied", "“Coastline" is "Coastline".
std::string word_letters(const std::string &word) {
  static const std::vector<std::string> outer{"\"", "'", "(", ")", "[", "]", "{", "}", ",", ".", ";", ":", "!", "?",
      "\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x98", "\xE2\x80\x99", "\xC2\xAB", "\xC2\xBB"};
  std::string letters = word;
  bool trimmed = true;
  while (trimmed && !letters.empty()) {
    trimmed = false;
    for (const std::string &mark : outer) {
      if (letters.size() >= mark.size() && letters.compare(0, mark.size(), mark) == 0) {
        letters.erase(0, mark.size());
        trimmed = true;
      }
      if (letters.size() >= mark.size() && letters.compare(letters.size() - mark.size(), mark.size(), mark) == 0) {
        letters.erase(letters.size() - mark.size());
        trimmed = true;
      }
    }
  }
  return letters;
}

// The text around a name, never a name (P0, 30 Oct 2026): a span the local
// model cut where a sentence or a mark of the text happened to fall - "alleged
// in the Complaint.", "its nonpayment constitutes a breach. Plaintiff",
// "’s written [", "Brightwater Foods, LLC]\"}" (a tracked change's closing
// bracket and the JSON string around it). Such values stood in one Matter's
// registry and every request that carried one was refused at the final check.
// A name holds marks only in pairs; it never opens with the tail of a
// possessive, nor with a lowercase word of the language before its capitals;
// it never ends on a word that only joins, and no sentence ends inside it.
// Name particles ("von Trapp", "da Silva") and a lowercase code name with no
// capital at all ("project falcon") are names as before.
bool reads_as_cut_of_text(const std::string &value) {
  if (value.find('{') != std::string::npos || value.find('}') != std::string::npos
      || value.find("[[") != std::string::npos || value.find("]]") != std::string::npos)
    return true;
  if (!marks_paired(value)) return true;
  for (const char *tail : {"\xE2\x80\x99s", "'s"})
    if (value.rfind(tail, 0) == 0) return true;
  const std::vector<std::string> words = spaced_words(value);
  if (words.empty()) return false;
  static const std::set<std::string> particles{"de", "da", "di", "du", "del", "della", "der", "den", "des", "van",
      "von", "la", "le", "bin", "ibn", "al", "el", "ter", "ten", "zu", "y", "dos", "das", "st"};
  static const std::set<std::string> joining{"the", "a", "an", "and", "or", "nor", "of", "in", "on", "at", "to",
      "for", "by", "with", "as", "from", "that", "which", "who", "than", "but", "if", "into", "onto", "upon", "per"};
  const auto lowercaseStart = [](const std::string &word) { return !word.empty() && word[0] >= 'a' && word[0] <= 'z'; };
  bool capitalAfterFirst = false;
  for (std::size_t index = 1; index < words.size(); ++index)
    if (starts_with_capital_letter(word_letters(words[index]))) capitalAfterFirst = true;
  const std::string first = word_letters(words.front());
  if (capitalAfterFirst && lowercaseStart(first) && particles.count(first) == 0 && is_common_word(first)) return true;
  if (words.size() >= 2) {
    const std::string last = word_letters(words.back());
    if (lowercaseStart(last) && joining.count(last) > 0) return true;
  }
  // A sentence ends inside it, or at its end, after a lowercase word of the
  // language: "breach. Plaintiff", "was excused.". "Inc.", "K." and "Hon."
  // are a name's own.
  for (std::size_t index = 0; index < words.size(); ++index) {
    std::string written = words[index];
    for (const char *closer : {"\"", ")", "]", "\xE2\x80\x9D", "\xC2\xBB"}) {
      const std::size_t size = std::char_traits<char>::length(closer);
      while (written.size() > size && written.compare(written.size() - size, size, closer) == 0) written.erase(written.size() - size);
    }
    if (written.size() < 2 || written.back() != '.') continue;
    const std::string letters = word_letters(written);
    if (!lowercaseStart(letters) || !is_common_word(letters)) continue;
    if (index + 1 == words.size() || starts_with_capital_letter(word_letters(words[index + 1]))) return true;
  }
  return false;
}

// A litigation's own words for its papers, its steps and the defences it
// pleads (P0, 30 Oct 2026). The local model's "legal entity" label took a
// pleading's capitalised terms of art for companies: "Complaint" stood in nine
// of one Matter's documents and left as a symbol in every request. A company is
// never a pleading. Judged only where that label, a court's or a place's is
// what misfires: as an organisation a single word keeps its mask (REGISTRY-2 -
// "Waiver" may be Waiver Holdings' short name), and a lawyer's confirmed alias
// is masked whatever this says.
bool is_litigation_term_of_art(const std::string &value) {
  static const std::set<std::string> terms{
      "complaint", "complaints", "amended complaint", "counterclaim", "counterclaims", "crossclaim", "crossclaims",
      "cross-claim", "cross-claims", "answer", "answers", "reply", "replies", "rejoinder", "motion", "motions",
      "memorandum", "brief", "briefs", "petition", "petitions", "pleading", "pleadings", "affidavit", "affidavits",
      "declaration", "declarations", "exhibit", "exhibits", "deposition", "depositions", "interrogatory",
      "interrogatories", "subpoena", "subpoenas", "summons", "stipulation", "stipulations", "venue", "jurisdiction",
      "waiver", "estoppel", "laches", "comparative", "discovery", "defense", "defenses", "defence", "defences",
      "count", "counts", "order", "orders", "judgment", "judgement", "verdict", "appeal", "transcript", "docket",
      "caption", "relief", "allegation", "allegations", "admission", "admissions", "denial", "denials",
  };
  std::string term = bare_term(value);
  if (terms.count(term) > 0) return true;
  // A paper's part by its number or letter names no one either: "Count III",
  // "Exhibit 4", "Exhibit A".
  const std::size_t space = term.rfind(' ');
  if (space == std::string::npos) return false;
  const std::string head = term.substr(0, space);
  const std::string mark = term.substr(space + 1);
  if (head != "count" && head != "exhibit" && head != "schedule" && head != "appendix" && head != "annex") return false;
  const bool numeral = !mark.empty() && std::all_of(mark.begin(), mark.end(), [](char character) {
    return (character >= '0' && character <= '9') || character == 'i' || character == 'v' || character == 'x'
        || character == 'l';
  });
  return numeral || (mark.size() == 1 && mark[0] >= 'a' && mark[0] <= 'z');
}

// Whether the text writes a litigation's term of art as one wherever it stands
// with a capital mid-sentence: after an article or a possessive ("the
// Complaint", "Defendant’s Answer", "Plaintiff’s Reply"), or as a heading in
// capitals ("COUNTERCLAIMS"). A company the text names bare - "Payment is made
// by Waiver within ten days" - is a name, and keeps its mask.
std::string upper_case(const std::string &value);

bool written_as_term_of_art(const std::string &text, const std::string &word) {
  if (word.empty() || word.find(' ') != std::string::npos) return false;
  static const std::set<std::string> introducers{"the", "this", "that", "its", "their", "our", "his", "her", "said",
      "such", "any", "each", "every", "no", "a", "an", "plaintiff's", "defendant's", "plaintiffs'", "defendants'"};
  bool introduced = false;
  for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + word.size())) {
    const bool wordStart = at == 0 || std::isalnum(static_cast<unsigned char>(text[at - 1])) == 0;
    const std::size_t end = at + word.size();
    const bool wordEnd = end >= text.size() || std::isalnum(static_cast<unsigned char>(text[end])) == 0;
    if (!wordStart || !wordEnd) continue;
    if (upper_case(word) == word) {
      introduced = true;
      continue;
    }
    // The word before it, as written.
    std::size_t before = at;
    while (before > 0 && text[before - 1] == ' ') --before;
    if (before == 0) continue;
    const char last = text[before - 1];
    if (last == '.' || last == '\n' || last == ':' || last == ';') continue;
    std::size_t wordFrom = before;
    while (wordFrom > 0 && text[wordFrom - 1] != ' ' && text[wordFrom - 1] != '\n') --wordFrom;
    std::string previous = ascii_lower(text.substr(wordFrom, before - wordFrom));
    const std::string curly = "\xE2\x80\x99";
    for (std::size_t quote = previous.find(curly); quote != std::string::npos; quote = previous.find(curly))
      previous.replace(quote, curly.size(), "'");
    const bool possessive = previous.size() > 2
        && (previous.compare(previous.size() - 2, 2, "'s") == 0 || previous.back() == '\'');
    if (introducers.count(previous) > 0 || possessive) {
      introduced = true;
      continue;
    }
    return false;
  }
  return introduced;
}

// The forum a text stands in, by its kind alone: "This Court", "the District",
// "said Circuit". It decides the dispute and is no party to it; a court named
// with its seat is judged as a public court (is_public_court).
bool is_forum_reference(const std::string &value) {
  static const std::set<std::string> forums{"court", "courts", "district", "circuit", "tribunal", "division", "forum",
      "bench", "chambers"};
  std::string term = bare_term(value);
  for (const char *lead : {"this ", "that ", "said ", "its ", "their ", "our "})
    if (term.rfind(lead, 0) == 0) {
      term.erase(0, std::char_traits<char>::length(lead));
      break;
    }
  return forums.count(term) > 0;
}

// An email address has a mailbox and a domain: "dvance@coastline.example".
// "Response 5" was recorded as one.
bool reads_as_email_address(const std::string &value) {
  const std::string term = word_letters(value);
  const std::size_t at = term.find('@');
  if (at == std::string::npos || at == 0 || term.find('@', at + 1) != std::string::npos) return false;
  if (term.find(' ') != std::string::npos) return false;
  const std::size_t dot = term.find('.', at + 2);
  return dot != std::string::npos && dot + 1 < term.size();
}

// A case number carries a figure, and a count of a pleading is no case:
// "1:26-cv-07418" is one, "COUNT III" a heading.
bool reads_as_case_number(const std::string &value) {
  return contains_digit(value) && !is_litigation_term_of_art(value);
}

// What a value of its kind cannot be, by its shape: an address with no
// mailbox, a case number with no figure.
bool misses_its_kinds_shape(EntityType type, const std::string &value) {
  return (type == EntityType::Email && !reads_as_email_address(value))
      || (type == EntityType::CaseNumber && !reads_as_case_number(value));
}

// What the registry never records from the value alone, whatever it was
// tagged (REGISTRY-3): a value that misses its kind's shape, a name cut out of
// the sentence around it, and a litigation's term of art where the model's
// label is what misfires. In a request the text decides a single word where
// it stands: "Payment is made by Waiver within ten days" names a company.
bool named_by_shape_alone(EntityType type, const std::string &value) {
  if (misses_its_kinds_shape(type, value)) return true;
  // A date the registry keeps is a date as a whole: "March 6 email" was
  // recorded once with the noun after it, every later request masked the noun
  // with the date, and a reply read "March 6 email email" (2026-10-31). Only
  // the registry is judged so; a date in a text is found by its own shape.
  if (type == EntityType::Date) {
    const std::optional<semantic::Value> parsed = semantic::parse_value(value);
    return !parsed || (parsed->kind != semantic::Kind::Date && parsed->kind != semantic::Kind::DayOfYear);
  }
  const bool nameType = type == EntityType::Person || type == EntityType::Judge || type == EntityType::Organisation
      || type == EntityType::LegalEntity || type == EntityType::Court || type == EntityType::Location;
  if (nameType && reads_as_cut_of_text(value)) return true;
  return (type == EntityType::LegalEntity || type == EntityType::Court || type == EntityType::Location
             || type == EntityType::ContractIdentifier)
      && is_litigation_term_of_art(value);
}

bool is_public_legal_vocabulary(EntityType type, const std::string &value, ValueOrigin origin) {
  // What a value of its kind cannot be: an address with no mailbox, a case
  // number with no figure.
  if (misses_its_kinds_shape(type, value)) return true;
  // The forum a text stands in, by its kind alone: "This Court", "the District".
  if ((type == EntityType::LegalEntity || type == EntityType::Court || type == EntityType::Location)
      && is_forum_reference(value))
    return true;
  // Singularity's own marker word, its own names for a document's parts, a
  // workbook's references and formulas, and the lawyer's own words for the
  // work are never anybody's identity.
  if (is_singularity_marker(value) || is_singularity_structural_handle(value) || is_cell_formula(value)
      || is_request_work_word(value))
    return true;
  // Grammar alone, written in lower case - "in", "or", "of" - names nobody and
  // no place, whatever it was tagged. Beside a list of states ("Exclusive in
  // IL, IN, MI and WI") the checkpoint took the "in" before them for a place:
  // every "in" a request held went to the model as that place's symbol, and
  // the final check refused the request for the "in" of Singularity's own
  // "insertParagraph" (Amsterdam acceptance, 25 Oct 2026; D-110).
  if (!has_capital(value) && only_grammar_words(value)) return true;
  // A state's postal code names the state as its name does: "IN" is as public
  // as "Indiana", and is not the "In" a sentence opens with.
  if (type == EntityType::Location && is_us_state_code(value)) return true;
  const bool personType = type == EntityType::Person || type == EntityType::Judge;
  if (!personType && (is_defined_legal_term(value) || is_officer_title(value))) return true;
  // So does a title the checkpoint took for a person: "Attention: Portfolio
  // Manager" (round 6). A person is named by a name, never by a title's words
  // alone; "President Smith" is no title, and keeps its mask.
  if (type == EntityType::Person && is_officer_title(value)) return true;
  // A title or a public office the checkpoint took for a judge names nobody:
  // "Secretary", "Comptroller", "Attorney General" (Larkspur data room, 11 Oct
  // 2026). A judge is named by a name: "Judge Delgado" keeps its mask.
  if (type == EntityType::Judge && (is_officer_title(value) || is_generic_role_term(EntityType::Person, value)
      || is_public_office(value)))
    return true;
  if (!personType && is_public_office(value)) return true;
  if (type == EntityType::ContractIdentifier && is_version_label(value)) return true;
  if ((type == EntityType::Organisation || type == EntityType::LegalEntity || type == EntityType::Court)
      && written_as_cell_references(value))
    return true;
  if ((type == EntityType::Court || type == EntityType::Organisation || type == EntityType::LegalEntity
          || type == EntityType::Location) && is_public_court(value))
    return true;
  // A concept written in lower case is never a name, whatever it was tagged.
  if (personType && !has_capital(value) && is_legal_concept(value, ValueOrigin::Detected)) return true;
  // A jurisdiction's law, whatever it was tagged: "the law of England and
  // Wales" came back from the model as a court (#245).
  if (!personType) {
    const std::string term = bare_term(value);
    for (const char *law : {"laws of the ", "law of the ", "laws of ", "law of "})
      if (term.rfind(law, 0) == 0 && is_public_jurisdiction(term.substr(std::char_traits<char>::length(law))))
        return true;
  }
  if ((type == EntityType::Organisation || type == EntityType::LegalEntity || type == EntityType::Court)
      && reads_as_clause_not_name(value)) return true;
  // A place named exactly as a jurisdiction; as an organisation-like value only
  // in a jurisdiction's own form ("State of New York", "Commonwealth of
  // Virginia"), never a bare name a company or an alias could carry.
  if (type == EntityType::Location && (is_public_jurisdiction(value) || is_public_russian_jurisdiction(value))) return true;
  if ((type == EntityType::Organisation || type == EntityType::LegalEntity) && is_public_jurisdiction(value)) {
    const std::string term = bare_term(value);
    for (const char *prefix : {"state of ", "commonwealth of ", "laws of ", "the state of "})
      if (term.rfind(prefix, 0) == 0) return true;
    // "Право Российской Федерации": the state, however it is labelled.
    if (names_the_russian_federation(value)) return true;
  }
  // Never for a person: a surname such as "Law" or "Code" is identity.
  if (type != EntityType::Person && type != EntityType::Judge
      && (is_general_legal_system_reference(value) || is_public_legal_acronym(value))) return true;
  if (type == EntityType::Person || type == EntityType::Organisation || type == EntityType::LegalEntity
      || type == EntityType::Court) {
    if (is_generic_role_term(type, value)) return true;
  }
  if (type == EntityType::MoneyAmount && is_bare_percentage(value)) return true;
  if (is_bare_duration(type, value)) return true;
  // What a request or an agreement calls the instrument and the law: a name
  // type or a contract identifier, never a quantity, an address or a number.
  const bool nameOrInstrument = type == EntityType::Person || type == EntityType::Judge
      || type == EntityType::Organisation || type == EntityType::LegalEntity || type == EntityType::Court
      || type == EntityType::Location || type == EntityType::ContractIdentifier;
  if (nameOrInstrument && is_russian_generic_legal_term(type, value)) return true;
  // A title with its number in a series names nobody either: "See Amendment
  // No. 2", «Дополнительному соглашению № 2».
  if (!personType && nameOrInstrument) {
    const std::string title = without_number_in_series(value);
    if (title != value && (is_generic_instrument_reference(title) || is_russian_generic_legal_term(type, title))) return true;
  }
  if (type != EntityType::Person && type != EntityType::Judge && nameOrInstrument
      && is_generic_instrument_reference(value)) return true;
  if (type != EntityType::Person && type != EntityType::Judge && nameOrInstrument
      && is_legal_concept(value, origin)) return true;
  return is_empty_quantity_term(type, value);
}

// How this very text writes one word: how often it stands in lowercase, how
// often it stands capitalised where a capital means something (anywhere but the
// start of a sentence), and whether a determiner or possessive ever introduces
// it. Counted over whole words only, so "marsh" inside "marshal" counts as
// nothing.
struct WordUsage {
  std::size_t lowercase = 0;
  std::size_t capitalised = 0;
  // Of those capitalised, the ones written all in capitals: a heading's
  // styling ("ПЕРСОНАЛЬНЫЕ ДАННЫЕ"), which says nothing of a name.
  std::size_t shouted = 0;
  std::size_t sentence_initial = 0;
  bool after_determiner = false;
};

// Function words that introduce a common noun and never a name. Grammar, not
// legal or domain vocabulary: "the buyer", "their father", "each child".
bool introduces_common_noun(const std::string &word) {
  static const std::set<std::string> determiners{
      "a", "an", "the", "this", "that", "these", "those", "each", "every", "any", "all", "both", "either",
      "neither", "no", "some", "another", "such", "my", "your", "his", "her", "its", "our", "their", "whose",
  };
  return determiners.count(normalized_entity_form(word)) > 0;
}

// The words that name a kind of organisation. Cut from the end of a detected
// name they leave the part that identifies it: "remedy group", "default
// capital", "notice partners". Matched word by word, lowercased.
bool names_an_organisation_kind(const std::string &words) {
  static const std::set<std::string> kinds{
      "group", "groups", "capital", "fund", "funds", "partners", "partnership", "holdings", "holding",
      "ventures", "venture", "company", "co", "corp", "corporation", "inc", "incorporated", "llc", "llp", "lp",
      "ltd", "limited", "plc", "bank", "trust", "associates", "advisors", "advisers", "management", "labs",
      "systems", "technologies", "solutions", "industries", "enterprises", "international", "global",
      "services", "consulting", "investments", "properties", "realty", "energy", "power", "team", "firm",
  };
  std::string word;
  const auto check = [&]() {
    const bool kind = !word.empty() && kinds.count(normalized_entity_form(word)) > 0;
    word.clear();
    return kind;
  };
  for (const char character : words) {
    if (std::isalnum(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0x80) {
      word.push_back(character);
    } else if (check()) {
      return true;
    }
  }
  return check();
}

// Whether every word cut from a name is grammar - "about", "is", "to" - and
// nothing that could belong to the name itself.
bool only_grammar_words(const std::string &words) {
  static const std::set<std::string> grammar{
      "about", "is", "was", "are", "were", "be", "to", "of", "for", "and", "or", "in", "on", "at", "with", "by",
      "from", "that", "this", "these", "now", "today", "here", "there", "so", "as", "then",
  };
  std::string word;
  bool any = false;
  bool all = true;
  const auto take = [&]() {
    if (word.empty()) return;
    any = true;
    if (grammar.count(normalized_entity_form(word)) == 0) all = false;
    word.clear();
  };
  for (const char character : words) {
    if (std::isalnum(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0x80)
      word.push_back(character);
    else
      take();
  }
  take();
  return any && all;
}

// Whether text[lineStart, position) is only a clause's number, perhaps after
// the word that names the clause and between marks of emphasis: "3.2 ", "10.7
// ", "(a) ", "Section 3.2 ", "## 4. ", "**12.** ". What follows it on the line
// is the clause's heading, written with a capital whatever it is.
bool only_clause_number_before(const std::string &text, std::size_t lineStart, std::size_t position) {
	std::vector<std::string> words;
	std::string word;
	for (std::size_t index = lineStart; index < position; ++index) {
		const char character = text[index];
		if (character == ' ' || character == '\t' || character == '*' || character == '_' || character == '#'
				|| character == '>') {
			if (!word.empty()) words.push_back(word);
			word.clear();
		} else {
			word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
		}
	}
	if (!word.empty()) words.push_back(word);
	if (words.empty() || words.size() > 2) return false;
	const std::string &number = words.back();
	const bool figures = std::isdigit(static_cast<unsigned char>(number.front())) != 0
		&& std::all_of(number.begin(), number.end(), [](char character) {
			return std::isdigit(static_cast<unsigned char>(character)) != 0 || character == '.';
		});
	const bool bracketed = number.size() >= 3 && number.size() <= 6 && number.front() == '(' && number.back() == ')'
		&& std::all_of(number.begin() + 1, number.end() - 1, [](char character) {
			return std::isalnum(static_cast<unsigned char>(character)) != 0;
		});
	const bool lettered = number.size() == 2 && std::isalpha(static_cast<unsigned char>(number[0])) != 0
		&& number[1] == '.';
	if (!figures && !bracketed && !lettered) return false;
	if (words.size() == 1) return true;
	static const std::set<std::string> clauseWords{"section", "article", "clause", "paragraph", "schedule",
		"\xc2\xa7"};
	return clauseWords.count(words.front()) > 0;
}

// Whether a writing carries a lowercase letter of any script.
bool has_lowercase(const std::string &value) {
	std::size_t offset = 0;
	while (offset < value.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
			static_cast<utf8proc_ssize_t>(value.size() - offset), &codePoint);
		if (consumed <= 0) return false;
		if (utf8proc_category(codePoint) == UTF8PROC_CATEGORY_LL) return true;
		offset += static_cast<std::size_t>(consumed);
	}
	return false;
}

// Whether a writing carries a capital letter of any script.
bool has_capital(const std::string &value) {
	std::size_t offset = 0;
	while (offset < value.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
			static_cast<utf8proc_ssize_t>(value.size() - offset), &codePoint);
		if (consumed <= 0) return false;
		const auto category = utf8proc_category(codePoint);
		if (category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LT) return true;
		offset += static_cast<std::size_t>(consumed);
	}
	return false;
}

// Whether the word that ends at text[end] is a possessive - "Fernhollow's",
// "Halvane’s", "the parties'" - and not a contraction ("it's", "that's").
bool possessive_ends_at(const std::string &text, std::size_t end) {
  std::size_t stem = std::string::npos;
  if (end >= 2 && text[end - 1] == 's' && text[end - 2] == '\'') stem = end - 2;
  else if (end >= 4 && text[end - 1] == 's' && text.compare(end - 4, 3, "\xE2\x80\x99") == 0) stem = end - 4;
  else if (end >= 2 && text[end - 1] == '\'' && text[end - 2] == 's') stem = end - 1;
  else if (end >= 4 && text.compare(end - 3, 3, "\xE2\x80\x99") == 0 && text[end - 4] == 's') stem = end - 3;
  if (stem == std::string::npos || stem == 0) return false;
  std::size_t from = stem;
  while (from > 0 && (std::isalnum(static_cast<unsigned char>(text[from - 1])) != 0
      || static_cast<unsigned char>(text[from - 1]) >= 0x80))
    --from;
  if (from == stem) return false;
  std::string word = text.substr(from, stem - from);
  for (char &character : word) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  static const std::set<std::string> contractions{"it", "that", "what", "there", "here", "he", "she", "who", "let",
      "where", "how", "when", "why", "this", "one", "everyone", "someone", "nobody", "somebody"};
  return contractions.count(word) == 0;
}

WordUsage word_usage(const std::string &text, const std::string &token) {
  WordUsage usage;
  const std::string folded = normalized_entity_form(token);
  if (folded.empty()) return usage;
  const auto isWordCharacter = [&](std::size_t index) {
    const auto character = static_cast<unsigned char>(text[index]);
    return std::isalnum(character) != 0 || character == '_' || character >= 0x80;
  };
  for (std::size_t position = 0; position + token.size() <= text.size(); ++position) {
    if ((position > 0 && isWordCharacter(position - 1))
        || (position + token.size() < text.size() && isWordCharacter(position + token.size())))
      continue;
    const std::string occurrence = text.substr(position, token.size());
    if (normalized_entity_form(occurrence) != folded) continue;
    // Where a sentence begins, a capital says nothing about what the word is.
    std::size_t before = position;
    while (before > 0) {
      const char character = text[before - 1];
      if (character == ' ' || character == '\t' || character == '"' || character == '\'' || character == '('
          || character == '[' || character == '\xe2')
        --before;
      else
        break;
    }
    // A clause's heading after its number begins as a sentence does: "3.2
    // Counterparts; Electronic Delivery."
    std::size_t lineStart = position;
    while (lineStart > 0 && position - lineStart < 64 && text[lineStart - 1] != '\n') --lineStart;
    const bool sentenceInitial = before == 0 || text[before - 1] == '.' || text[before - 1] == '!'
        || text[before - 1] == '?' || text[before - 1] == '\n' || text[before - 1] == '\r'
        || text[before - 1] == '\t' || text[before - 1] == ':' || text[before - 1] == ';'
        || ((lineStart == 0 || text[lineStart - 1] == '\n') && only_clause_number_before(text, lineStart, position));
    // Written without a capital: the comparison of letters alone above makes
    // "non‑breaching party" and "non-breaching party" one phrase.
    if (!has_capital(occurrence)) {
      ++usage.lowercase;
      // The word immediately before this one, when there is one on the line.
      std::size_t wordEnd = position;
      while (wordEnd > 0 && (text[wordEnd - 1] == ' ' || text[wordEnd - 1] == '\t')) --wordEnd;
      std::size_t wordStart = wordEnd;
      while (wordStart > 0 && isWordCharacter(wordStart - 1)) --wordStart;
      if (wordEnd > wordStart && introduces_common_noun(text.substr(wordStart, wordEnd - wordStart)))
        usage.after_determiner = true;
      // A possessive picks out a noun as an article does: "Fernhollow's
      // engineers", "the parties' counsel" (#245). Never a contraction: "it's
      // smith who signed" may still name a man.
      if (wordEnd < position && possessive_ends_at(text, wordEnd)) usage.after_determiner = true;
      // "a reasonable recipient": an adjective or two between the article and
      // the word change nothing, when each is a word of the language in
      // lowercase (#245).
      std::size_t hopEnd = wordEnd;
      std::size_t hopStart = wordStart;
      for (int hop = 0; hop < 2 && !usage.after_determiner && hopEnd < position; ++hop) {
        const std::string between = text.substr(hopStart, hopEnd - hopStart);
        if (between.empty() || has_capital(between) || !is_common_word(between) || introduces_common_noun(between))
          break;
        std::size_t nextEnd = hopStart;
        while (nextEnd > 0 && (text[nextEnd - 1] == ' ' || text[nextEnd - 1] == '\t')) --nextEnd;
        if (nextEnd == hopStart) break;
        std::size_t nextStart = nextEnd;
        while (nextStart > 0 && isWordCharacter(nextStart - 1)) --nextStart;
        if ((nextEnd > nextStart && introduces_common_noun(text.substr(nextStart, nextEnd - nextStart)))
            || possessive_ends_at(text, nextEnd))
          usage.after_determiner = true;
        hopEnd = nextEnd;
        hopStart = nextStart;
      }
    } else if (sentenceInitial) {
      ++usage.sentence_initial;
    } else {
      ++usage.capitalised;
      if (!has_lowercase(occurrence)) ++usage.shouted;
    }
  }
  return usage;
}

// Whether this text uses the detected word as an ordinary word rather than as
// a name. Everything here is decided from the text in hand, never from a list
// of words somebody thought of in advance, and every condition is written so
// that doubt keeps the mask on:
//
//   - the detected mention itself is lowercase, so a capitalised mention is
//     always treated as a name;
//   - the word never stands capitalised anywhere else in this text except
//     where a sentence begins, so a real surname mentioned properly even once
//     stays protected - which is what keeps names that are also ordinary words
//     ("Marsh", "Cook", "Baker", "Child") masked;
//   - and the text either uses it as an ordinary word more than once or puts a
//     determiner in front of it, so a single stray lowercase writing of a name
//     is not enough to expose it.
//
// A word the Project's own Entity Registry or the person's custom terms know
// is masked before this is ever reached, so this can never unmask a party.
bool written_as_common_word(const std::string &text, const std::string &value, std::size_t start) {
  if (value.size() < 3 || start >= text.size()) return false;
  // The language must actually have this word. A name written in lowercase -
  // how a client's name reaches a document by accident - is not a word, so the
  // detector's judgement stands and it stays masked.
  if (is_common_word(value) == false) return false;
  if (value.find_first_of(" \t\n\r") != std::string::npos) return false;
  if (std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x80 && std::isalpha(character) == 0;
      }))
    return false;
  if (value != normalized_entity_form(value)) return false;
  const WordUsage usage = word_usage(text, value);
  if (usage.capitalised > 0) return false;
  // Only use after a word that introduces a common noun ("the children"): a
  // lowercase surname typed twice ("remind baker ... tell baker") is still a
  // person, and doubt keeps the mask on.
  return usage.lowercase >= 1 && usage.after_determiner;
}

// Defined with the word-boundary checks below.
bool stands_as_word(const std::string &text, std::size_t position, std::size_t length);

// A code, not a name: an identifier written with underscores - a prompt's own
// section and category labels ("CURRENT_REQUEST", "LEGAL_ENTITY") and a
// system's names ("singularity_journal"). A reviewing model wrote "LEGAL_ENTITY"
// in its findings, and the next round sent the word back as a symbol (24 Sep
// 2026). `capitals` asks for one written in capitals only.
bool written_as_code(const std::string &value, bool capitals) {
	if (value.size() < 3 || value.find('_') == std::string::npos || value.front() == '_' || value.back() == '_')
		return false;
	return std::all_of(value.begin(), value.end(), [&](unsigned char character) {
		if (character == '_' || (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z'))
			return true;
		return !capitals && character >= 'a' && character <= 'z';
	});
}

// A phrase in plain ASCII letters: U+2010 and U+2011, the hyphens a model
// writes "non‑breaching" with, become "-", and the typographic apostrophe "'".
// Empty when anything else is in it.
std::string plain_phrase(const std::string &value) {
	std::string plain;
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (value.compare(index, 3, "\xe2\x80\x90") == 0 || value.compare(index, 3, "\xe2\x80\x91") == 0) {
			plain.push_back('-');
			index += 2;
		} else if (value.compare(index, 3, "\xe2\x80\x99") == 0) {
			plain.push_back('\'');
			index += 2;
		} else if (static_cast<unsigned char>(value[index]) >= 0x80) {
			return {};
		} else {
			plain.push_back(value[index]);
		}
	}
	return plain;
}

// Whether a lowercase phrase is made of ordinary words: each is a word of the
// language (is_common_word(), its regular forms included), a word that joins
// others ("of", "an", "the"), or a prefix a hyphen joins to a word
// ("non-breaching", "pre-closing"); one at least is a word of the language.
bool ordinary_lowercase_words(const std::string &phrase) {
	static const std::set<std::string> joining{
		"a", "an", "the", "of", "to", "in", "on", "at", "by", "or", "and", "for", "as", "per", "its", "any", "all",
		"no", "not", "such", "each", "into", "upon", "with", "from",
	};
	static const std::set<std::string> prefixes{
		"non", "pre", "post", "re", "co", "sub", "anti", "inter", "intra", "over", "under", "self", "multi", "semi",
		"quasi", "cross", "counter", "mid", "out", "ex", "vice", "un", "bi", "tri",
	};
	std::string trimmed = phrase;
	if (trimmed.size() > 2 && trimmed.compare(trimmed.size() - 2, 2, "'s") == 0) trimmed.erase(trimmed.size() - 2);
	else if (!trimmed.empty() && trimmed.back() == '\'') trimmed.pop_back();
	if (trimmed.empty() || !std::all_of(trimmed.begin(), trimmed.end(), [](char character) {
			return (character >= 'a' && character <= 'z') || character == '-' || character == ' ';
		}))
		return false;
	// The law's own Latin inside a longer phrase is ordinary words too: "its
	// bona fide document-retention policies" (#245). Each such phrase reads as
	// one word of the language.
	{
		std::vector<std::string> words;
		for (std::size_t position = 0; position <= trimmed.size();) {
			const std::size_t end = std::min(trimmed.find(' ', position), trimmed.size());
			if (end > position) words.push_back(trimmed.substr(position, end - position));
			position = end + 1;
		}
		std::string joined;
		bool latin = false;
		for (std::size_t index = 0; index < words.size();) {
			std::size_t taken = 1;
			for (const std::size_t span : {std::size_t{4}, std::size_t{3}, std::size_t{2}}) {
				if (index + span > words.size()) continue;
				std::string candidate = words[index];
				for (std::size_t next = index + 1; next < index + span; ++next) candidate += " " + words[next];
				if (is_legal_latin_phrase(candidate)) {
					taken = span;
					break;
				}
			}
			if (!joined.empty()) joined.push_back(' ');
			joined += taken > 1 ? "law" : words[index];
			latin = latin || taken > 1;
			index += taken;
		}
		if (latin) trimmed = joined;
	}
	bool word = false;
	std::size_t position = 0;
	while (position < trimmed.size()) {
		const std::size_t end = trimmed.find_first_of(" -", position);
		const std::size_t stop = end == std::string::npos ? trimmed.size() : end;
		const std::string part = trimmed.substr(position, stop - position);
		const bool beforeHyphen = stop < trimmed.size() && trimmed[stop] == '-';
		if (!part.empty()) {
			if (is_common_word(part)) word = true;
			else if (joining.count(part) == 0 && !(beforeHyphen && prefixes.count(part) > 0)) return false;
		}
		position = stop + 1;
	}
	return word;
}

bool defined_as_role(const std::string &text, const std::string &value);

// Whether a detected company or legal entity is written wholly in lowercase in
// ordinary words and the text itself uses it as an ordinary phrase: "the
// non‑breaching party" (a live English review, 24 Sep 2026, sent it as a legal
// entity's symbol, and the reviewer asked for a redraft of what was its own
// clause), "an original", "the payee". As with written_as_common_word(), the
// text decides and doubt keeps the mask on: an article or a determiner opens
// the phrase, or stands before it somewhere in the text, and nowhere is it
// written with a capital except where a heading or a sentence begins or as the
// role the text defines in quotes. A company a request types in lowercase -
// "payments run through apple", "what did remedy group say?", "the status of
// project falcon" - has no such evidence and stays masked, and a Project's own
// parties are masked however they are typed (known_project_entities()).
bool written_in_ordinary_lowercase(const std::string &text, const std::string &value) {
	const std::string plain = plain_phrase(value);
	if (plain.empty() || !ordinary_lowercase_words(plain)) return false;
	const std::size_t space = plain.find(' ');
	const bool opensWithArticle = space != std::string::npos && introduces_common_noun(plain.substr(0, space));
	const WordUsage usage = word_usage(text, value);
	if (usage.capitalised > 0) {
		std::string title = plain;
		for (std::size_t index = 0; index < title.size(); ++index)
			if (index == 0 || title[index - 1] == ' ' || title[index - 1] == '-')
				title[index] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[index])));
		if (!defined_as_role(text, title)) return false;
	}
	return opensWithArticle || usage.after_determiner;
}

int surname_likeness(const std::string &word);

// Whether the words `term` are a Cyrillic term as a definition writes one:
// Cyrillic words only, the first with a capital, capitals only where a word
// begins, no figure and no legal form.
bool russian_term_shape(const std::string &term) {
	bool first = true;
	bool wordStart = true;
	std::size_t offset = 0;
	while (offset < term.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(term.data()) + offset,
			static_cast<utf8proc_ssize_t>(term.size() - offset), &codePoint);
		if (consumed <= 0) return false;
		const auto category = utf8proc_category(codePoint);
		const bool cyrillic = codePoint >= 0x0400 && codePoint <= 0x04FF;
		if (codePoint == ' ' || codePoint == '-') {
			wordStart = true;
		} else if (!cyrillic || (category != UTF8PROC_CATEGORY_LU && category != UTF8PROC_CATEGORY_LL)) {
			return false;
		} else {
			const bool capital = category == UTF8PROC_CATEGORY_LU;
			if (first && !capital) return false;
			// A capital only where a word begins: "Предел ответственности", not "ООО".
			if (capital && !wordStart) return false;
			first = false;
			wordStart = false;
		}
		offset += static_cast<std::size_t>(consumed);
	}
	return !first;
}

// Defined below, with the case forms of a term's words.
bool short_name_of_party(const std::string &text, std::size_t marker, const std::string &term);
bool russian_role_like(const std::string &text, const std::string &term);

// Whether the quoted term whose opening quote stands at text[found] is defined
// there - after «далее», «в дальнейшем» or «именуемое» with only the
// definition's own connectors between - and is not a party's own short name.
bool defined_at(const std::string &text, std::size_t found, const std::string &term) {
	const std::size_t lineStart = found == 0 ? std::string::npos : text.rfind('\n', found - 1);
	const std::size_t from = std::max(lineStart == std::string::npos ? 0 : lineStart + 1,
		found > 80 ? found - 80 : std::size_t{0});
	const std::string lead = text.substr(from, found - from);
	std::size_t marker = std::string::npos;
	for (const char *word : {"\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xb5\xd0\xb5",
			"\xd0\xb4\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xb5\xd0\xb9\xd1\x88\xd0\xb5\xd0\xbc",
			"\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x83\xd0\xb5\xd0\xbc"}) {  // далее, дальнейшем, именуем(ое)
		const std::size_t at = lead.rfind(word);
		if (at != std::string::npos && (marker == std::string::npos || at > marker)) marker = at;
	}
	if (marker == std::string::npos) return false;
	// Only the definition's own connectors stand between the marker and the
	// quote - dashes, commas, «совместно», «по отдельности», another defined
	// term in quotes; never the bracket's end or another party's name.
	static const std::set<std::string> connectors{
		"\xd1\x81\xd0\xbe\xd0\xb2\xd0\xbc\xd0\xb5\xd1\x81\xd1\x82\xd0\xbd\xd0\xbe",  // совместно
		"\xd0\xbf\xd0\xbe",  // по
		"\xd0\xbe\xd1\x82\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd0\xb8",  // отдельности
		"\xd1\x82\xd0\xb5\xd0\xba\xd1\x81\xd1\x82\xd1\x83",  // тексту
		"\xd0\xb0",  // а
		"\xd1\x82\xd0\xb0\xd0\xba\xd0\xb6\xd0\xb5",  // также
		"\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xb5\xd0\xb5",  // далее
		"\xd0\xb4\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xb5\xd0\xb9\xd1\x88\xd0\xb5\xd0\xbc",  // дальнейшем
		// именуемое, именуемый, именуемая, именуемые, именуемого, именуемой
		"\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x83\xd0\xb5\xd0\xbc\xd0\xbe\xd0\xb5", "\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x83\xd0\xb5\xd0\xbc\xd1\x8b\xd0\xb9",
		"\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x83\xd0\xb5\xd0\xbc\xd0\xb0\xd1\x8f", "\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x83\xd0\xb5\xd0\xbc\xd1\x8b\xd0\xb5",
		"\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x83\xd0\xb5\xd0\xbc\xd0\xbe\xd0\xb3\xd0\xbe", "\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x83\xd0\xb5\xd0\xbc\xd0\xbe\xd0\xb9",
	};
	const std::string between = lead.substr(marker);
	bool connected = true;
	std::size_t cursor = 0;
	while (connected && cursor < between.size()) {
		const char character = between[cursor];
		if (character == ' ' || character == '-' || character == ',' || character == ':') { ++cursor; continue; }
		if (between.compare(cursor, 3, "\xe2\x80\x94") == 0 || between.compare(cursor, 3, "\xe2\x80\x93") == 0) {
			cursor += 3;
			continue;
		}
		if (between.compare(cursor, 2, "\xc2\xab") == 0) {
			const std::size_t closing = between.find("\xc2\xbb", cursor);
			if (closing == std::string::npos) { connected = false; break; }
			cursor = closing + 2;
			continue;
		}
		std::size_t end = cursor;
		while (end < between.size() && between[end] != ' ' && between[end] != ',') ++end;
		connected = connectors.count(between.substr(cursor, end - cursor)) > 0;
		cursor = end;
	}
	if (!connected) return false;
	// A party's own short name - 'АО «Уральский завод ...» (далее — «Уралмаш»)',
	// 'ООО «Северная звезда» (далее — «Звезда»)' - is the party, and stays masked:
	// it takes the stem of a word of the name before it, in any case, or it has
	// no role's shape at all. Both went out readable when only a word repeated
	// with its own capital counted (reviewer, 26 Sep 2026).
	return !short_name_of_party(text, from + marker, term) && russian_role_like(text, term);
}

// Every case form, singular and plural, a Russian noun or a noun-like
// adjective may take, from the form a definition writes (lower case, «ё» as
// «е»): «займодавец» - «займодавца», «займодавцу» ...; «цессионарий» -
// «цессионария» ...; «потерпевший» - «потерпевшего» ...; «сторона» - «стороны»
// ...; «франчайзи» as it is. More forms than a word takes are harmless: only a
// term the text itself defines, and only a word written as one of its forms,
// is ever read by it.
std::set<std::string> russian_case_forms(const std::string &word) {
	std::set<std::string> forms{word};
	const auto ends = [&](const std::string &ending) {
		return word.size() > ending.size() + 2 && word.compare(word.size() - ending.size(), ending.size(), ending) == 0;
	};
	const auto add = [&](std::size_t cut, std::initializer_list<const char *> endings) {
		const std::string stem = word.substr(0, word.size() - cut);
		for (const char *ending : endings) forms.insert(stem + ending);
	};
	if (ends("\xd0\xb5\xd1\x86")) {  // -ец: займодавец, продавец - the vowel drops
		add(4, {"\xd1\x86\xd0\xb0", "\xd1\x86\xd1\x83", "\xd1\x86\xd0\xbe\xd0\xbc", "\xd1\x86\xd0\xb5", "\xd1\x86\xd1\x8b",
			"\xd1\x86\xd0\xbe\xd0\xb2", "\xd1\x86\xd0\xb0\xd0\xbc", "\xd1\x86\xd0\xb0\xd0\xbc\xd0\xb8", "\xd1\x86\xd0\xb0\xd1\x85"});
	} else if (ends("\xd0\xb8\xd0\xb9") || ends("\xd1\x8b\xd0\xb9") || ends("\xd0\xbe\xd0\xb9")) {
		// -ий as a noun (цессионарий), and -ий/-ый/-ой as an adjective (потерпевший, управляющий).
		if (ends("\xd0\xb8\xd0\xb9"))
			add(4, {"\xd0\xb8\xd1\x8f", "\xd0\xb8\xd1\x8e", "\xd0\xb8\xd0\xb5\xd0\xbc", "\xd0\xb8\xd0\xb8", "\xd0\xb8\xd0\xb5\xd0\xb2",
				"\xd0\xb8\xd1\x8f\xd0\xbc", "\xd0\xb8\xd1\x8f\xd0\xbc\xd0\xb8", "\xd0\xb8\xd1\x8f\xd1\x85"});
		add(4, {"\xd0\xbe\xd0\xb3\xd0\xbe", "\xd0\xb5\xd0\xb3\xd0\xbe", "\xd0\xbe\xd0\xbc\xd1\x83", "\xd0\xb5\xd0\xbc\xd1\x83",
			"\xd1\x8b\xd0\xbc", "\xd0\xb8\xd0\xbc", "\xd0\xbe\xd0\xbc", "\xd0\xb5\xd0\xbc", "\xd1\x8b\xd0\xb5", "\xd0\xb8\xd0\xb5",
			"\xd1\x8b\xd1\x85", "\xd0\xb8\xd1\x85", "\xd1\x8b\xd0\xbc\xd0\xb8", "\xd0\xb8\xd0\xbc\xd0\xb8"});
	} else if (ends("\xd0\xb8\xd1\x8f")) {  // -ия: компания
		add(4, {"\xd0\xb8\xd0\xb8", "\xd0\xb8\xd1\x8e", "\xd0\xb8\xd0\xb5\xd0\xb9", "\xd0\xb8\xd0\xb9", "\xd0\xb8\xd1\x8f\xd0\xbc",
			"\xd0\xb8\xd1\x8f\xd0\xbc\xd0\xb8", "\xd0\xb8\xd1\x8f\xd1\x85"});
	} else if (ends("\xd1\x8c")) {  // -ь: лизингодатель, пользователь
		add(2, {"\xd1\x8f", "\xd1\x8e", "\xd0\xb5\xd0\xbc", "\xd1\x91\xd0\xbc", "\xd0\xb5", "\xd0\xb8", "\xd0\xb5\xd0\xb9",
			"\xd1\x8c\xd1\x8e", "\xd1\x8f\xd0\xbc", "\xd1\x8f\xd0\xbc\xd0\xb8", "\xd1\x8f\xd1\x85"});
	} else if (ends("\xd0\xb9")) {  // -й
		add(2, {"\xd1\x8f", "\xd1\x8e", "\xd0\xb5\xd0\xbc", "\xd0\xb5", "\xd0\xb8", "\xd0\xb5\xd0\xb2", "\xd1\x8f\xd0\xbc",
			"\xd1\x8f\xd0\xbc\xd0\xb8", "\xd1\x8f\xd1\x85"});
	} else if (ends("\xd0\xb0")) {  // -а: сторона
		add(2, {"\xd1\x8b", "\xd0\xb8", "\xd0\xb5", "\xd1\x83", "\xd0\xbe\xd0\xb9", "\xd0\xbe\xd1\x8e", "", "\xd0\xb0\xd0\xbc",
			"\xd0\xb0\xd0\xbc\xd0\xb8", "\xd0\xb0\xd1\x85"});
	} else if (ends("\xd1\x8f")) {  // -я
		add(2, {"\xd0\xb8", "\xd0\xb5", "\xd1\x8e", "\xd0\xb5\xd0\xb9", "\xd0\xb5\xd1\x8e", "\xd1\x8c", "\xd0\xb9", "\xd1\x8f\xd0\xbc",
			"\xd1\x8f\xd0\xbc\xd0\xb8", "\xd1\x8f\xd1\x85"});
	} else if (ends("\xd0\xbe")) {  // -о: общество
		add(2, {"\xd0\xb0", "\xd1\x83", "\xd0\xbe\xd0\xbc", "\xd0\xb5", "", "\xd0\xb0\xd0\xbc", "\xd0\xb0\xd0\xbc\xd0\xb8", "\xd0\xb0\xd1\x85"});
	} else if (ends("\xd1\x8b") || ends("\xd0\xb8")) {
		// A plural a definition writes («Стороны», «Покупатели»), or a word that does
		// not decline («Франчайзи»): its plural cases, and the singular's.
		add(2, {"", "\xd0\xbe\xd0\xb2", "\xd0\xb5\xd0\xb9", "\xd0\xb0\xd0\xbc", "\xd1\x8f\xd0\xbc", "\xd0\xb0\xd0\xbc\xd0\xb8",
			"\xd1\x8f\xd0\xbc\xd0\xb8", "\xd0\xb0\xd1\x85", "\xd1\x8f\xd1\x85", "\xd0\xb0", "\xd0\xb5", "\xd1\x83", "\xd0\xbe\xd0\xb9",
			"\xd1\x8f", "\xd1\x8e", "\xd0\xb5\xd0\xbc", "\xd1\x8c"});
	} else if (!ends("\xd0\xb5") && !ends("\xd1\x83") && !ends("\xd1\x8e")) {  // a hard consonant: цедент, гарант, застройщик
		add(0, {"\xd0\xb0", "\xd1\x83", "\xd0\xbe\xd0\xbc", "\xd0\xb5\xd0\xbc", "\xd0\xb5", "\xd1\x8b", "\xd0\xb8", "\xd0\xbe\xd0\xb2",
			"\xd0\xb5\xd0\xb9", "\xd0\xb0\xd0\xbc", "\xd0\xb0\xd0\xbc\xd0\xb8", "\xd0\xb0\xd1\x85"});
	}
	return forms;
}

// A word in the form a definition's word is compared in: lower case, «ё» as «е».
std::string term_word_form(const std::string &word) {
	std::string form = normalized_entity_form(word);
	for (std::size_t at = form.find("\xd1\x91"); at != std::string::npos; at = form.find("\xd1\x91", at + 2)) form.replace(at, 2, "\xd0\xb5");
	return form;
}

std::vector<std::string> term_words(const std::string &term) {
	std::vector<std::string> words;
	std::size_t from = 0;
	while (from <= term.size()) {
		std::size_t end = term.find_first_of(" -", from);
		if (end == std::string::npos) end = term.size();
		if (end > from) words.push_back(term_word_form(term.substr(from, end - from)));
		from = end + 1;
	}
	return words;
}

std::size_t letters_in(const std::string &word) {
	return static_cast<std::size_t>(std::count_if(word.begin(), word.end(),
		[](char character) { return (static_cast<unsigned char>(character) & 0xC0) != 0x80; }));
}

// The first `letters` letters of a UTF-8 word.
std::string first_letters(const std::string &word, std::size_t letters) {
	std::size_t end = 0;
	for (std::size_t count = 0; end < word.size() && count < letters; ++count) {
		++end;
		while (end < word.size() && (static_cast<unsigned char>(word[end]) & 0xC0) == 0x80) ++end;
	}
	return word.substr(0, end);
}

bool ends_with_any(const std::string &word, std::initializer_list<const char *> endings) {
	return std::any_of(endings.begin(), endings.end(), [&](const char *ending) {
		const std::size_t size = std::char_traits<char>::length(ending);
		return word.size() > size + 2 && word.compare(word.size() - size, size, ending) == 0;
	});
}

// Whether a word of a Russian definition (lower case, «ё» as «е») is built as
// a role or a kind of thing is, never as a name: who does something («-тель»,
// «-ец», «-щик», «-чик», «-ник», «-ент», «-ант», «-арий», «-ар», «-иат», «-ор»,
// «-ер», «-ист»), a participle («Потерпевший», «Управляющий», «Поверенный»), a
// noun of kind («-ство», «-ция», «-ие», «-ия»), singular or plural. Morphology,
// no list of roles: «Хранитель», «Ссудодатель», «Дистрибьютор», «Бенефициар»
// read so unseen. A short name - «Уралмаш», «Звезда», «Техбуд» - has none of it.
bool russian_role_noun_shape(const std::string &word) {
	if (ends_with_any(word, {"тель", "тели", "ец", "цы", "щик", "щики", "чик", "чики", "ник", "ники", "ент", "енты", "ант",
			"анты", "арий", "арии", "ар", "ары", "иат", "иаты", "ор", "оры", "ер", "еры", "ист", "исты", "ство", "ства",
			"ция", "ции", "ие", "ия", "ии",
			"щий", "щая", "щее", "щие", "вший", "вшая", "вшие", "нный", "нная", "нное", "нные", "мый", "мая", "мые"}))
		return true;
	// The parties and the persons any contract has: «Сторона», «Стороны», «Третье лицо», «Банк».
	for (const char *stem : {"сторон", "лиц", "банк"})
		if (word.compare(0, std::char_traits<char>::length(stem), stem) == 0) return true;
	return false;
}

bool russian_adjective_shape(const std::string &word) {
	return ends_with_any(word, {"ый", "ий", "ой", "ая", "яя", "ое", "ее", "ые", "ие", "ье"});
}

// Whether the text writes a form of `word` in lower case as an ordinary word,
// outside quotation marks: «передать третьему лицу», «банк получателя».
bool written_as_russian_word(const std::string &text, const std::string &word) {
	for (const std::string &form : russian_case_forms(word)) {
		if (form.size() < 6) continue;
		for (std::size_t at = text.find(form); at != std::string::npos; at = text.find(form, at + form.size())) {
			if (!stands_as_word(text, at, form.size())) continue;
			const std::size_t lineStart = at == 0 ? std::string::npos : text.rfind('\n', at - 1);
			const std::string line = text.substr(lineStart == std::string::npos ? 0 : lineStart + 1,
				at - (lineStart == std::string::npos ? 0 : lineStart + 1));
			const std::size_t opened = line.rfind("\xc2\xab");
			const std::size_t closed = line.rfind("\xc2\xbb");
			if (opened != std::string::npos && (closed == std::string::npos || closed < opened)) continue;
			return true;
		}
	}
	return false;
}

// Whether a term a Russian definition writes is built as a role: each word a
// role, a kind of thing, or an ordinary word the text itself writes in lower
// case - and at least one of them not a bare adjective. The Russian twin of
// the English path's ordinary_lowercase_words(): doubt keeps the mask on.
bool russian_role_like(const std::string &text, const std::string &term) {
	const std::vector<std::string> words = term_words(term);
	if (words.empty()) return false;
	bool noun = false;
	for (const std::string &word : words) {
		if (russian_role_noun_shape(word) || written_as_russian_word(text, word)) {
			noun = true;
			continue;
		}
		if (words.size() == 1 || !russian_adjective_shape(word)) return false;
	}
	return noun;
}

// Whether `term`, defined by the marker word at text[marker], is the short
// name of the party named right before it: a word of the term begins as a word
// of the name does - in any case, with or without its capital: «Уралмаш» of
// «Уральский завод», «Звезда» of «Северная звезда», «Сбер» of «Сбербанк». A
// word of the name is one written with a capital or inside its quotes; the
// prose between (lowercase «доверенности», «генерального директора») and a
// role defined for the party before it («Арендодатель» beside «Арендатор») are
// not the name.
bool short_name_of_party(const std::string &text, std::size_t marker, const std::string &term) {
	const std::size_t lineStart = marker == 0 ? std::string::npos : text.rfind('\n', marker - 1);
	const std::size_t from = std::max(lineStart == std::string::npos ? 0 : lineStart + 1,
		marker > 320 ? marker - 320 : std::size_t{0});
	const std::string span = text.substr(from, marker - from);
	// Where the definitions before this one stand: a quoted term just after one is a role, not the name.
	std::vector<std::size_t> definitions;
	for (const char *word : {"\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xb5\xd0\xb5", "\xd0\xb4\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xb5\xd0\xb9\xd1\x88\xd0\xb5\xd0\xbc",
			"\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x83\xd0\xb5\xd0\xbc"})
		for (std::size_t at = span.find(word); at != std::string::npos; at = span.find(word, at + 1)) definitions.push_back(at);
	std::vector<std::string> nameWords;
	bool quoted = false;
	bool definedQuote = false;
	std::size_t offset = 0;
	while (offset < span.size()) {
		if (span.compare(offset, 2, "\xc2\xab") == 0) {
			quoted = true;
			definedQuote = std::any_of(definitions.begin(), definitions.end(),
				[&](std::size_t at) { return at < offset && offset - at <= 48; });
			offset += 2;
			continue;
		}
		if (span.compare(offset, 2, "\xc2\xbb") == 0) {
			quoted = false;
			definedQuote = false;
			offset += 2;
			continue;
		}
		utf8proc_int32_t codePoint = 0;
		utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(span.data()) + offset,
			static_cast<utf8proc_ssize_t>(span.size() - offset), &codePoint);
		if (consumed <= 0) {
			++offset;
			continue;
		}
		auto category = utf8proc_category(codePoint);
		if (category != UTF8PROC_CATEGORY_LU && category != UTF8PROC_CATEGORY_LL) {
			offset += static_cast<std::size_t>(consumed);
			continue;
		}
		const bool capital = category == UTF8PROC_CATEGORY_LU;
		const std::size_t start = offset;
		while (offset < span.size()) {
			consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(span.data()) + offset,
				static_cast<utf8proc_ssize_t>(span.size() - offset), &codePoint);
			if (consumed <= 0) break;
			category = utf8proc_category(codePoint);
			if (category != UTF8PROC_CATEGORY_LU && category != UTF8PROC_CATEGORY_LL) break;
			offset += static_cast<std::size_t>(consumed);
		}
		if ((capital || quoted) && !definedQuote) {
			// A legal form, or the line of business a name states, is not the name:
			// 'Общество с ограниченной ответственностью «Вега» (далее — «Общество»)'
			// defines the role «Общество», «Страховая компания «Вега»» the role
			// «Страховщик». The role's own shape (russian_role_like) still decides.
			const std::string word = term_word_form(span.substr(start, offset - start));
			static const char *forms[] = {"обществ", "товариществ", "партнерств", "компани", "корпораци", "акционерн",
				"ограниченн", "ответственност", "публичн", "непубличн", "закрыт", "открыт", "федеральн", "государствен",
				"муниципальн", "унитарн", "предприяти", "учреждени", "некоммерческ", "коммерческ", "организаци", "автономн",
				"фонд", "банк", "индивидуальн", "предпринимател", "кооператив", "казенн", "страхов", "лизинг", "управляющ",
				"инвестиционн", "торгов", "строительн", "микрофинанс", "коллекторск", "финансов", "кредитн", "холдинг", "групп",
				"агентств"};
			const bool legalForm = std::any_of(std::begin(forms), std::end(forms), [&](const char *stem) {
				return word.compare(0, std::char_traits<char>::length(stem), stem) == 0;
			});
			if (!legalForm) nameWords.push_back(word);
		}
	}
	for (const std::string &word : term_words(term)) {
		const std::size_t letters = letters_in(word);
		if (letters < 3) continue;
		for (const std::string &name : nameWords) {
			const std::size_t shared = std::min<std::size_t>({4, letters, letters_in(name)});
			if (shared >= 3 && first_letters(word, shared) == first_letters(name, shared)) return true;
		}
	}
	return false;
}

// Whether a capitalised Russian term is one the text defines in quotes after
// «далее», «в дальнейшем» or «именуемое» - "(далее — «Инцидент»)", "именуемое в
// дальнейшем «Клиент»", "(далее совместно — «Стороны»)": a defined term, never
// a party's name - in the case the definition writes it or in any other: once
// «Займодавец» is defined, «Займодавца», «Займодавцу» and «Займодавцем» are
// the same role, and masking each as a person of its own turned the parties
// round (the documented role flip). Any role a contract defines this way is
// read so - «Цедент», «Лизингополучатель», «Хранитель», «Потерпевший» - with
// no list to keep. Cyrillic words only, the first with a capital, no figure
// and no legal form; a party's own short name - 'ООО «Транслогистик» (далее —
// «Транслогистик»)' - stays masked with the name it repeats.
bool defined_as_russian_term(const std::string &text, const std::string &term) {
	if (!russian_term_shape(term)) return false;
	static const std::pair<std::string, std::string> quotes[] = {{"\xc2\xab", "\xc2\xbb"}, {"\"", "\""},
		{"\xe2\x80\x9c", "\xe2\x80\x9d"}};
	for (const auto &[open, close] : quotes) {
		const std::string quoted = open + term + close;
		for (std::size_t found = text.find(quoted); found != std::string::npos; found = text.find(quoted, found + 1))
			if (defined_at(text, found, term)) return true;
	}
	// Another case of a term defined in its own: every quoted definition in the
	// text whose words the term's words are forms of, word for word.
	const std::vector<std::string> words = term_words(term);
	if (words.empty() || words.size() > 4) return false;
	for (const auto &[open, close] : quotes) {
		for (std::size_t found = text.find(open); found != std::string::npos; found = text.find(open, found + open.size())) {
			const std::size_t start = found + open.size();
			const std::size_t end = text.find(close, start);
			if (end == std::string::npos || end - start > 96 || end == start) continue;
			const std::string defined = text.substr(start, end - start);
			if (defined == term || !russian_term_shape(defined)) continue;
			const std::vector<std::string> definedWords = term_words(defined);
			if (definedWords.size() != words.size()) continue;
			bool forms = true;
			for (std::size_t index = 0; index < words.size() && forms; ++index)
				forms = russian_case_forms(definedWords[index]).count(words[index]) > 0;
			if (forms && defined_at(text, found, defined)) return true;
		}
	}
	return false;
}

// Whether a detected company, legal entity or person is written wholly in
// lowercase Cyrillic words - prose, not a name. A live Russian draft (24 Sep
// 2026) had «персональными данными» protected as one symbol after the
// checkpoint tagged it in the lawyer's own request; the drafter wrote the
// symbol wherever personal data belonged, and every restoration froze the
// instrumental case: «передачей персональными данными третьим лицам». The
// English list of words cannot judge Russian, so the shape decides, and doubt
// keeps the mask on:
//
//   - Cyrillic letters only, none a capital, no figure, no legal form (ооо,
//     ао, ип, ...), and not written inside quotation marks;
//   - nowhere does the text write it with a capital except where a heading or
//     a sentence begins: "Громов" written as a name keeps "громову" masked;
//   - one word a person could be named by stays masked: "смирновой" has the
//     shape of a surname, "данными" has not.
bool written_in_lowercase_cyrillic(const std::string &text, const std::string &value, std::size_t start,
		EntityType type) {
	static const std::set<std::string> legalForms{"ооо", "оао", "зао", "пао", "ао", "ип", "нко", "ано", "гуп", "муп",
		"фгуп", "тоо", "кфх", "снт", "тсж"};
	bool letter = false;
	std::size_t offset = 0;
	while (offset < value.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
			static_cast<utf8proc_ssize_t>(value.size() - offset), &codePoint);
		if (consumed <= 0) return false;
		const auto category = utf8proc_category(codePoint);
		const bool cyrillic = codePoint >= 0x0400 && codePoint <= 0x04FF;
		if (category == UTF8PROC_CATEGORY_LL && cyrillic) letter = true;
		else if (codePoint != ' ' && codePoint != '-' && codePoint != 0x2010 && codePoint != 0x2011) return false;
		offset += static_cast<std::size_t>(consumed);
	}
	if (!letter) return false;
	std::vector<std::string> words;
	std::string word;
	for (std::size_t index = 0; index <= value.size(); ++index) {
		if (index == value.size() || value[index] == ' ') {
			if (!word.empty()) words.push_back(word);
			word.clear();
		} else {
			word.push_back(value[index]);
		}
	}
	if (words.empty() || std::any_of(words.begin(), words.end(), [](const std::string &part) {
			return legalForms.count(part) > 0;
		}))
		return false;
	// Not quoted: «транслогистик», "ромашка" keep their mask.
	const auto quoteAt = [&](std::size_t position, bool opening) {
		for (const char *quote : {"\xc2\xab", "\xc2\xbb", "\"", "\xe2\x80\x9c", "\xe2\x80\x9d", "\xe2\x80\x9e"}) {
			const std::size_t size = std::char_traits<char>::length(quote);
			if (opening ? (position >= size && text.compare(position - size, size, quote) == 0)
					: text.compare(position, size, quote) == 0)
				return true;
		}
		return false;
	};
	if (start != std::string::npos && (quoteAt(start, true) || quoteAt(start + value.size(), false))) return false;
	const bool personLike = type == EntityType::Person || type == EntityType::Judge;
	if (personLike && words.size() == 1 && surname_likeness(words.front()) >= 1) return false;
	const WordUsage usage = word_usage(text, value);
	return usage.capitalised == usage.shouted;
}

// Whether a lowercase term is made only of words for a kind of institution -
// "university", "bank", "trust company" - which say what a party is, never
// which one it is.
bool only_institution_kind_words(const std::string &lowered) {
	static const std::set<std::string> kinds{"university", "college", "school", "academy", "institute",
		"institution", "hospital", "clinic", "foundation", "bank", "fund", "trust", "company", "corporation",
		"partnership", "association", "society", "council", "authority", "agency", "laboratory", "board",
		"district", "lender", "insurer", "museum", "library", "union", "cooperative", "regents", "center", "centre"};
	bool any = false;
	std::size_t position = 0;
	while (position <= lowered.size()) {
		const std::size_t end = std::min(lowered.find(' ', position), lowered.size());
		const std::string word = lowered.substr(position, end - position);
		if (!word.empty()) {
			if (kinds.count(word) == 0) return false;
			any = true;
		}
		position = end + 1;
	}
	return any;
}

// Whether a capitalised term is a role the text itself defines in quotes -
// '(the "Breaching Party")', '("Customer")', '(each a "Party")', «…» the same:
// a defined term, not a party's name. Only a term of ordinary words, each
// written with a capital, and never a party's own short name: in 'Harbor Point
// Logistics, Inc. ("Harbor Point")' the quoted words are the name's, and stay
// masked with it.
bool defined_as_role(const std::string &text, const std::string &value) {
	std::string term = value;
	for (const char *quote : {"\"", "\xe2\x80\x9c", "\xe2\x80\x9d", "\xc2\xab", "\xc2\xbb"}) {
		const std::string mark(quote);
		if (term.size() > mark.size() && term.compare(0, mark.size(), mark) == 0) term.erase(0, mark.size());
		if (term.size() > mark.size() && term.compare(term.size() - mark.size(), mark.size(), mark) == 0)
			term.erase(term.size() - mark.size());
	}
	const std::string plain = plain_phrase(term);
	if (plain.empty()) return defined_as_russian_term(text, term);
	if (plain.size() < 3) return false;
	std::string lowered;
	bool capitals = false;
	for (std::size_t index = 0; index < plain.size(); ++index) {
		const char character = plain[index];
		const bool wordStart = index == 0 || plain[index - 1] == ' ' || plain[index - 1] == '-';
		if (wordStart && character >= 'A' && character <= 'Z') capitals = true;
		else if (wordStart && character >= 'a' && character <= 'z') return false;
		else if (!wordStart && character >= 'A' && character <= 'Z') return false;
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}
	if (!capitals || !ordinary_lowercase_words(lowered)) return false;
	static const std::set<std::string> definitionWords{"the", "a", "an", "each", "together", "collectively",
		"individually", "hereinafter", "herein", "referred", "to", "as", "and", "or", "called", "defined"};
	const std::pair<std::string, std::string> quotes[] = {{"\"", "\""}, {"\xe2\x80\x9c", "\xe2\x80\x9d"},
		{"\xc2\xab", "\xc2\xbb"}};
	for (const auto &[open, close] : quotes) {
		const std::string quoted = open + term + close;
		for (std::size_t found = text.find(quoted); found != std::string::npos; found = text.find(quoted, found + 1)) {
			// Back from the quote over the words a definition is written with, to
			// the bracket it opens: '(the "Breaching Party")', '(hereinafter "Seller")'.
			std::size_t cursor = found;
			for (int words = 0; words < 6; ++words) {
				while (cursor > 0 && (text[cursor - 1] == ' ' || text[cursor - 1] == ',')) --cursor;
				// Another term of the same definition before it: '("Lumenfield" or the
				// "Processor")' (#245) - over the quoted term to its opening quote.
				bool quotedBefore = false;
				for (const auto &[otherOpen, otherClose] : quotes) {
					if (cursor < otherClose.size() || text.compare(cursor - otherClose.size(), otherClose.size(), otherClose) != 0)
						continue;
					const std::size_t closeAt = cursor - otherClose.size();
					const std::size_t openAt = closeAt == 0 ? std::string::npos : text.rfind(otherOpen, closeAt - 1);
					if (openAt == std::string::npos || closeAt - openAt > 64
							|| text.find_first_of("\n()", openAt) < closeAt)
						continue;
					cursor = openAt;
					quotedBefore = true;
					break;
				}
				if (quotedBefore) continue;
				std::size_t wordStart = cursor;
				while (wordStart > 0 && std::isalpha(static_cast<unsigned char>(text[wordStart - 1])) != 0) --wordStart;
				if (wordStart == cursor) break;
				std::string word = text.substr(wordStart, cursor - wordStart);
				for (char &character : word) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
				if (definitionWords.count(word) == 0) break;
				cursor = wordStart;
			}
			if (cursor == 0 || text[cursor - 1] != '(') continue;
			// A party's own short name begins with a word of its name, written just
			// before the bracket: 'Harbor Point Logistics, Inc. ("Harbor Point")'.
			const std::size_t lineStart = text.rfind('\n', cursor - 1);
			const std::size_t from = std::max(lineStart == std::string::npos ? 0 : lineStart + 1,
				cursor > 160 ? cursor - 160 : std::size_t{0});
			const std::string before = text.substr(from, cursor - from);
			const std::string first = term.substr(0, term.find(' '));
			bool shortName = false;
			for (std::size_t position = before.find(first); !first.empty() && position != std::string::npos;
				 position = before.find(first, position + 1))
				if (stands_as_word(before, position, first.size())) shortName = true;
			// A word for what the party is, not which one: 'The Regents of Ashgrove
			// State University (the "University")', 'Northfield Community Bank
			// (the "Bank")' (#245). The words that tell it apart stay masked.
			if (!shortName || only_institution_kind_words(lowered)) return true;
		}
	}
	return false;
}

// Whether the word written at text[start, start + length) is a clause's
// heading: after the clause's number at the start of its line, or alone at the
// start of it, and followed by a full stop or a semicolon - never a speaker's
// colon, never inside a sentence. "3.2 Counterparts; Electronic Delivery.",
// "10.3 Waiver.", "**Notices.**".
bool stands_as_clause_heading(const std::string &text, std::size_t start, std::size_t length) {
  std::size_t lineStart = start;
  while (lineStart > 0 && start - lineStart < 64 && text[lineStart - 1] != '\n') --lineStart;
  if (lineStart > 0 && text[lineStart - 1] != '\n') return false;
  const bool alone = std::all_of(text.begin() + static_cast<std::ptrdiff_t>(lineStart),
      text.begin() + static_cast<std::ptrdiff_t>(start), [](char character) {
        return character == ' ' || character == '\t' || character == '*' || character == '_' || character == '#';
      });
  const bool numbered = only_clause_number_before(text, lineStart, start);
  if (!alone && !numbered) return false;
  std::size_t after = start + length;
  while (after < text.size() && (text[after] == '*' || text[after] == '_')) ++after;
  // A numbered heading may also be the whole of its line: "3. Inventions" (#245).
  if (numbered && (after == text.size() || text[after] == '\n' || text[after] == '\r')) return true;
  return after < text.size() && (text[after] == '.' || text[after] == ';');
}

// Whether a word the checkpoint took for a company or a legal entity is the
// heading of a clause and that clause's own ordinary word: "3.2 Counterparts;
// Electronic Delivery. This Amendment may be executed in counterparts" (a live
// English amendment, 24 Sep 2026: the checkpoint sent "Counterparts" as a
// LEGAL_ENTITY symbol, and the reviewing model marked its own clause for
// redrafting; on the same text the on-device model also tags the lowercase
// "counterparts"). As with written_as_common_word(), everything is decided from
// the text in hand and doubt keeps the mask on:
//
//   - one word of letters that the language has (is_common_word(), its plural
//     and other regular forms included);
//   - the text writes it both as a clause's heading (stands_as_clause_heading)
//     and in lowercase, and never with a capital except where a heading or a
//     sentence begins;
//   - the mention judged is one of those two writings.
//
// "Waiver", the short name of Waiver Holdings, written "paid by Waiver", stays
// masked; so does a heading the text never writes as an ordinary word, and a
// lowercase word no heading names ("apple", "square" as companies).
bool written_as_heading_word(const std::string &text, const std::string &value, std::size_t start) {
  if (value.size() < 3 || start + value.size() > text.size()) return false;
  if (!std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x80 && std::isalpha(character) != 0;
      }))
    return false;
  const bool lowercase = value == normalized_entity_form(value);
  const bool capitalised = value.front() >= 'A' && value.front() <= 'Z'
      && (std::all_of(value.begin() + 1, value.end(), [](char character) { return character >= 'a' && character <= 'z'; })
          || std::all_of(value.begin() + 1, value.end(), [](char character) { return character >= 'A' && character <= 'Z'; }));
  if (!lowercase && !capitalised) return false;
  if (!is_common_word(value)) return false;
  const WordUsage usage = word_usage(text, value);
  if (usage.capitalised > 0 || usage.lowercase == 0) return false;
  if (capitalised) return stands_as_clause_heading(text, start, value.size());
  // A lowercase mention: the clause's own heading writes the same word, in
  // title case or in capitals.
  std::string title = value;
  title.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(title.front())));
  std::string capitals = value;
  for (char &character : capitals) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  for (const std::string &heading : {title, capitals})
    for (std::size_t position = text.find(heading); position != std::string::npos;
         position = text.find(heading, position + 1))
      if (stands_as_word(text, position, heading.size()) && stands_as_clause_heading(text, position, heading.size()))
        return true;
  return false;
}

// Whether one word of a detected amount already belongs to the amount: a
// figure, a currency sign or code, or a word that counts or names a currency
// ("thirty", "million", "тридцать", "млн", "долларов"). Russian is matched by
// the stems its number and currency words start with; a word matched by
// mistake only keeps more of the value protected, never less.
// Whether a word counts: "sixty", "thousand", "sixty-one", «восемнадцать»,
// «миллионов», «тысячи». Russian is matched by the stems its number words
// start with; a reading built on a word matched by mistake does not parse,
// and is dropped by whoever reads it.
bool is_number_word(const std::string &word) {
  static const std::set<std::string> englishWords{
      "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
      "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "thirty",
      "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "hundred", "thousand", "million", "billion",
      "trillion",
  };
  static const std::vector<std::string> russianStems{
      "один", "одн", "два", "две", "двух", "двум", "три", "трех", "трём", "трем", "четыр", "пят", "шест", "сем",
      "восем", "девят", "десят", "двадцат", "тридцат", "сорок", "девяност", "сто", "двест", "трист", "тысяч",
      "миллион", "миллиард", "полтор", "полутор",
  };
  // "sixty-one": every part a number word.
  if (word.find('-') != std::string::npos) {
    std::size_t from = 0;
    bool any = false;
    while (from <= word.size()) {
      const std::size_t dash = std::min(word.find('-', from), word.size());
      if (dash == from || !is_number_word(word.substr(from, dash - from))) return false;
      any = true;
      from = dash + 1;
    }
    return any;
  }
  const std::string form = normalized_entity_form(word);
  if (form.empty()) return false;
  if (englishWords.count(form) > 0) return true;
  for (const std::string &stem : russianStems)
    if (form.compare(0, stem.size(), stem) == 0) return true;
  return false;
}

// Whether a word says how large a number is written in words: a thousand, a
// million, a billion. An amount in words worth protecting has one.
bool is_scale_word(const std::string &word) {
  const std::string form = normalized_entity_form(word);
  for (const char *stem : {"thousand", "million", "billion", "trillion", "тысяч", "миллион", "миллиард"})
    if (form.find(stem) != std::string::npos) return true;
  return false;
}

bool belongs_to_amount(const std::string &word) {
  if (is_number_word(word)) return true;
  static const std::set<std::string> englishWords{
      "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
      "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "thirty",
      "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "hundred", "thousand", "million", "billion",
      "trillion", "dollar", "dollars", "euro", "euros", "pound", "pounds", "ruble", "rubles", "rouble",
      "roubles", "yen", "yuan", "franc", "francs", "usd", "eur", "gbp", "rub", "chf", "jpy", "cny", "us",
  };
  static const std::vector<std::string> russianStems{
      "один", "одн", "два", "две", "три", "четыр", "пят", "шест", "сем", "восем", "девят", "десят",
      "двадцат", "тридцат", "сорок", "девяност", "сто", "двест", "трист", "тысяч", "тыс", "миллион", "млн",
      "миллиард", "млрд", "рубл", "руб", "доллар", "долл", "евро", "юан", "фунт", "франк", "иен",
      // "полтора миллиона": left out, the amount went out as "полтора [[E1]]".
      "полтор", "полутор",
  };
  for (const char character : word) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isdigit(byte) != 0 || character == '$') return true;
  }
  // Currency signs outside ASCII: euro, pound, yen, rouble.
  for (const char *sign : {"\xe2\x82\xac", "\xc2\xa3", "\xc2\xa5", "\xe2\x82\xbd"})
    if (word.find(sign) != std::string::npos) return true;
  const bool upperCode = word.size() == 3 && std::all_of(word.begin(), word.end(),
      [](unsigned char character) { return character >= 'A' && character <= 'Z'; });
  if (upperCode) return true;
  const std::string form = normalized_entity_form(word);
  if (form.empty()) return false;
  if (englishWords.count(form) > 0) return true;
  for (const std::string &stem : russianStems)
    if (form.compare(0, stem.size(), stem) == 0) return true;
  return false;
}

// Where a detected amount really begins. The checkpoint sometimes takes the
// words that introduce an amount with it ("неустойка в 30 млн долларов",
// "a penalty of $30,000,000"), and the symbol then stands for the noun as well:
// the model cannot inflect it, and the noun comes back in the case the request
// used, into whatever sentence the model wrote. The amount starts at its first
// word that counts or names money; when no such word is found the value is kept
// whole, so nothing is ever left readable that belonged to the amount.
std::size_t amount_start(const std::string &text, std::size_t start, std::size_t length) {
  const std::size_t end = start + length;
  std::size_t position = start;
  while (position < end) {
    while (position < end && (text[position] == ' ' || text[position] == '\t')) ++position;
    std::size_t wordEnd = position;
    while (wordEnd < end && text[wordEnd] != ' ' && text[wordEnd] != '\t') ++wordEnd;
    if (wordEnd == position) break;
    if (belongs_to_amount(text.substr(position, wordEnd - position))) return position;
    position = wordEnd;
  }
  return start;
}

bool continues_word_at(const std::string &text, std::size_t position);

// Where an amount written in words really begins: the checkpoint returned
// "thousand three hundred dollars" out of "sixty-one thousand three hundred
// dollars", and the amount went out as "sixty-one [[E2]]" - the words readable
// and the symbol a row of $1,300. Every number word right before the span
// belongs to it.
std::size_t amount_words_start(const std::string &text, std::size_t start) {
	std::size_t begin = start;
	while (true) {
		std::size_t cursor = begin;
		while (cursor > 0 && text[cursor - 1] == ' ') --cursor;
		if (cursor == begin || cursor == 0) break;
		std::size_t wordStart = cursor;
		while (wordStart > 0) {
			std::size_t previous = wordStart - 1;
			while (previous > 0 && (static_cast<unsigned char>(text[previous]) & 0xC0) == 0x80) --previous;
			if (text[previous] != '-' && !continues_word_at(text, previous)) break;
			wordStart = previous;
		}
		if (wordStart == cursor) break;
		const std::string word = text.substr(wordStart, cursor - wordStart);
		if (contains_digit(word) || !is_number_word(word)) break;
		begin = wordStart;
	}
	return begin;
}

// Whether a value carries a letter of any script - a name, a code such as
// "NW-17", a date written in words. Without one it is a number however it is
// punctuated: an amount, an account, a date written in digits. Bytes that are
// not UTF-8 count as a letter, so in doubt a value keeps its old matching.
bool has_letter(const std::string &value) {
	std::size_t offset = 0;
	while (offset < value.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
			static_cast<utf8proc_ssize_t>(value.size() - offset), &codePoint);
		if (consumed <= 0) return true;
		const auto category = utf8proc_category(codePoint);
		if (category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL || category == UTF8PROC_CATEGORY_LT
				|| category == UTF8PROC_CATEGORY_LM || category == UTF8PROC_CATEGORY_LO)
			return true;
		offset += static_cast<std::size_t>(consumed);
	}
	return false;
}

// Whether a value carries a currency sign: "$12", "12 ₽".
bool has_currency_sign(const std::string &value) {
	std::size_t offset = 0;
	while (offset < value.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
			static_cast<utf8proc_ssize_t>(value.size() - offset), &codePoint);
		if (consumed <= 0) return false;
		if (utf8proc_category(codePoint) == UTF8PROC_CATEGORY_SC) return true;
		offset += static_cast<std::size_t>(consumed);
	}
	return false;
}

// One number however it is grouped - "12", "0.64", "2 615 000" - rather than
// a date, a time or a run of numbers ("15.03.2026", "10:30", "10-12").
bool plain_number(const std::string &value) {
	std::size_t stops = 0;
	for (std::size_t index = 0; index < value.size(); ++index) {
		const char character = value[index];
		if (character == '/' || character == ':') return false;
		if (character == '-' && index > 0 && std::isdigit(static_cast<unsigned char>(value[index - 1])) != 0) return false;
		if (character == '.') ++stops;
	}
	return stops < 2;
}

// Whether a detected value is a number that names nothing where it stands.
// A number is sensitive or not by what is written around it, never by its
// length (number_meaning(), DeterministicDetector.hpp): the checkpoint's tag
// on "Row 10", "п. 9.4", "12 months", a formula's constant or zero is dropped
// - a live preview of a workbook (24 Sep 2026) had one zero cell, "B5: 0
// (shown in $)", tagged as money - while "12" under "Purchase price ($m)"
// stays an amount, and a number the checkpoint missed there is protected by
// detect() all the same. A number is never a person's or a company's name,
// whatever it was tagged: where its context makes it money or an identifier,
// detect() protects it as that.
bool detected_number_names_nothing(const std::string &text, std::size_t start, std::size_t length, EntityType type) {
	const std::string value = text.substr(start, length);
	if (!contains_digit(value) || has_currency_sign(value)) return false;
	if (has_letter(value)) return names_only_structure(text, start, length);
	// Figures a letter or two continue with no space are one code with them - a
	// statute's section, "280G", "409A" - never an amount on their own: a US data
	// room (11 Oct 2026) sent "Section 280G gross-up" as "[[Amount9]]G gross-up".
	// A magnitude ("5m", "2bn") or a ratio ("4.00x") is no such letter.
	{
		const std::size_t end = start + length;
		std::size_t letters = end;
		while (letters < text.size() && std::isalpha(static_cast<unsigned char>(text[letters])) != 0) ++letters;
		const bool stands_after_a_break = start == 0 || std::isalnum(static_cast<unsigned char>(text[start - 1])) == 0;
		if (letters > end && letters - end <= 2 && stands_after_a_break) {
			std::string suffix = text.substr(end, letters - end);
			for (char &character : suffix) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			static const std::set<std::string> magnitudes{"k", "m", "mm", "mn", "b", "bn", "t", "x"};
			if (magnitudes.count(suffix) == 0) return true;
		}
	}
	if (type == EntityType::Person || type == EntityType::Judge || type == EntityType::Organisation
			|| type == EntityType::LegalEntity || type == EntityType::Court)
		return true;
	return number_meaning(text, start, length) == NumberMeaning::Generic;
}

// Whether a number an identity is spelled with stands for that identity at
// text[position, position + length): as the whole number it is
// (stands_as_whole_number(): never inside "2026", "0.64", "B10" or
// "15.03.2026"), and where the words around it do not make it structure or a
// plain scalar - an amount of 12 is not "Row 12", "Section 12" or "12 months"
// - nor, for any identity but a date, the day or the year of a date ("12
// марта 2026"). A value with a currency sign is money wherever it stands whole.
bool number_stands_for(const std::string &text, std::size_t position, std::size_t length, const std::string &value,
		std::optional<EntityType> type) {
	if (!stands_as_whole_number(text, position, length)) return false;
	if (has_currency_sign(value)) return true;
	// A phone or an account is its whole writing wherever it stands whole:
	// "212.555.0199" reads like a clause's number, and a signature block's
	// number was held yet sent raw (reviewer, 26 Sep 2026).
	if (type == EntityType::Phone || type == EntityType::BankAccount) return true;
	// An identifier's own writing - figures with their separators, "7702-418856-03",
	// "0618" - is that identifier wherever it stands whole; the words around it
	// ("account No. ...") never make it structure (live EN2, 27 Sep 2026: an
	// escrow account typed as an identifier was re-sent raw in a draft under
	// review, missed by the transform and by this check alike).
	if (type == EntityType::ContractIdentifier || type == EntityType::CaseNumber || type == EntityType::TaxId) {
		std::size_t digits = 0;
		bool figures = !value.empty();
		for (const char character : value) {
			if (character >= '0' && character <= '9') ++digits;
			else if (character != '-' && character != '/' && character != '.' && character != ' ') figures = false;
		}
		if (figures && (digits >= 6 || (value.front() == '0' && digits >= 3))) return true;
	}
	const NumberMeaning meaning = number_meaning(text, position, length);
	if (meaning == NumberMeaning::Generic) return false;
	return meaning != NumberMeaning::DatePart || type == EntityType::Date || !plain_number(value);
}

// Defined with the word-boundary checks below.
bool continues_word_at(const std::string &text, std::size_t position);
bool joined_at(const std::string &text, std::size_t position);

// Whether a spelling with letters stands where it was found as a word, or as
// the stem of one: it begins a word, and nothing but an inflection ending
// continues it - "Транслогистик" in "Транслогистики" and "Транслогистикой",
// never "иска" inside "Переписка" or "записка". A live Russian matter (24 Sep
// 2026) sent a folder as "ПерепLEGAL ENTITY 9" after «иска» became a symbol.
//
// A Russian stem takes its case endings whatever it names. An English one
// takes a plural always for a person - a family, "the Whitfields" - and
// otherwise only where the stem is no ordinary word: "Northwinds" is the
// company's, while "States", "Partners" and "Targets" are the ordinary words,
// never a name's stem with an ending. A thirty-document US data room (11 Oct
// 2026) sent "United [[Place4]]s" because "State" stood as the stem of
// "States". Masking, the registry and the leak checks all read words this way,
// so what one leaves the other never refuses.
bool stands_as_word_or_stem(const std::string &text, std::size_t position, std::size_t length,
		bool english_plural) {
	static const std::set<std::string> endings{
		"а", "я", "у", "ю", "е", "и", "ы", "о", "ой", "ей", "ою", "ею", "ом", "ем", "ам", "ям", "ах", "ях", "ов", "ев",
		"ами", "ями", "ым", "им", "ых", "их", "ую", "юю", "ого", "его", "ому", "ему", "ыми", "ими", "ая", "яя", "ое",
		"ее", "ые", "ие", "ый", "ий",
	};
	static const std::set<std::string> englishEndings{"s", "es"};
	const std::size_t end = position + length;
	if (position > 0 && continues_word_at(text, position)) {
		std::size_t before = position - 1;
		while (before > 0 && position - before < 4 && (static_cast<unsigned char>(text[before]) & 0xC0) == 0x80)
			--before;
		// A seam (joined_at) is a word start: "1ДС", "AuditCorvane".
		if (continues_word_at(text, before) && !joined_at(text, position)) return false;
	}
	if (length == 0) return true;
	std::size_t last = end - 1;
	while (last > position && (static_cast<unsigned char>(text[last]) & 0xC0) == 0x80) --last;
	if (!continues_word_at(text, last) || joined_at(text, end)) return true;
	std::size_t tail = end;
	while (tail < text.size() && continues_word_at(text, tail)) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + tail,
			static_cast<utf8proc_ssize_t>(text.size() - tail), &codePoint);
		if (consumed <= 0) break;
		tail += static_cast<std::size_t>(consumed);
	}
	if (tail == end) return true;
	if (tail - end > 6) return false;
	const std::string ending = normalized_entity_form(text.substr(end, tail - end));
	if (endings.count(ending) > 0) return true;
	if (englishEndings.count(ending) == 0) return false;
	return english_plural || !is_common_word(text.substr(position, length));
}

// A detected span widened to what it stands in: a number to the whole number
// (detected_number_span(): "615 000" to "2 615 000", "087/26" to "СП-087/26"),
// and words to whole words - the start back to the start of its word, the end
// on to the end of its word unless only an inflection ending follows ("Вект"
// becomes "Векторский", "Транслогистик" stays a stem). The model sometimes cuts
// a span inside a word or a number; widened, it masks more, never less, and it
// is a spelling the frozen pass finds where it stands.
std::pair<std::size_t, std::size_t> word_aligned_span(const std::string &text, std::size_t start, std::size_t length) {
	if (length == 0) return {start, length};
	std::size_t end = start + length;
	const auto isDigit = [&](std::size_t index) { return index < text.size() && text[index] >= '0' && text[index] <= '9'; };
	if (!has_letter(text.substr(start, length))) {
		const std::pair<std::size_t, std::size_t> number = detected_number_span(text, start, end);
		return {number.first, number.second - number.first};
	}
	// A value in words that begins or ends with a figure keeps the whole number:
	// "615 000 рублей" cut out of "2 615 000 рублей" would send the millions.
	if (isDigit(start)) start = detected_number_span(text, start, start + 1).first;
	if (end > 0 && isDigit(end - 1)) end = std::max(end, detected_number_span(text, end - 1, end).second);
	const auto previousStart = [&](std::size_t position) {
		std::size_t before = position - 1;
		while (before > 0 && position - before < 4 && (static_cast<unsigned char>(text[before]) & 0xC0) == 0x80)
			--before;
		return before;
	};
	if (continues_word_at(text, start))
		while (start > 0 && continues_word_at(text, previousStart(start))) start = previousStart(start);
	if (!stands_as_word_or_stem(text, start, end - start, false)) {
		while (end < text.size() && continues_word_at(text, end)) {
			utf8proc_int32_t codePoint = 0;
			const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + end,
				static_cast<utf8proc_ssize_t>(text.size() - end), &codePoint);
			if (consumed <= 0) break;
			end += static_cast<std::size_t>(consumed);
		}
	}
	return {start, end - start};
}

bool inside_formula_text(const std::string &text, std::size_t position);

// Whether the text writes text[start, start + length) as a workbook's formula
// or reference: after "=", inside a "{=…}", or as a reference in the
// workbook reading's own lines ("B6 = …", "Row 2: A2 …").
bool written_as_formula_at(const std::string &text, std::size_t start, std::size_t length) {
  std::size_t before = start;
  while (before > 0 && text[before - 1] == ' ') --before;
  if (before > 0 && text[before - 1] == '=') return true;
  if (inside_formula_text(text, start)) return true;
  // "cell B12", "cells B2:B10", «ячейка B12»: prose naming a reference as one.
  std::size_t wordStart = before;
  while (wordStart > 0 && text[wordStart - 1] != ' ' && text[wordStart - 1] != '\n') --wordStart;
  std::string word = text.substr(wordStart, before - wordStart);
  for (char &character : word) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  if (before < start && (word == "cell" || word == "cells" || word == "range" || word.rfind("\xd1\x8f\xd1\x87\xd0\xb5\xd0\xb9\xd0\xba", 0) == 0
      || word.rfind("\xd0\xaf\xd1\x87\xd0\xb5\xd0\xb9\xd0\xba", 0) == 0))
    return true;
  std::size_t referenceEnd = start;
  while (referenceEnd < start + length && (text[referenceEnd] == '$' || (text[referenceEnd] >= 'A' && text[referenceEnd] <= 'Z')))
    ++referenceEnd;
  while (referenceEnd < start + length && text[referenceEnd] >= '0' && text[referenceEnd] <= '9') ++referenceEnd;
  return stands_as_cell_reference(text, start, referenceEnd - start);
}

bool is_name_word(const std::string &word);
bool is_patronymic(const std::string &word);
bool has_cyrillic_letter(const std::string &value);
std::optional<std::pair<std::string, std::string>> given_and_patronymic(const std::string &value);

// A Russian first name and patronymic the checkpoint returned without the
// surname standing right beside them: «Поручитель Лебедев Андрей Игоревич»
// came back as «Андрей Игоревич», and «Лебедев» went out readable beside its
// symbol (or as a symbol of its own - one person, two symbols). The name is
// the whole of it. Only a word that reads as a surname joins: never
// «Уважаемый», a verb or a role noun.
std::pair<std::size_t, std::size_t> with_surname_beside(const std::string &text, std::size_t start, std::size_t length) {
	if (!given_and_patronymic(text.substr(start, length))) return {start, length};
	const auto surnameLike = [](const std::string &word) {
		if (!is_name_word(word) || is_patronymic(word) || !has_cyrillic_letter(word)) return false;
		const std::string lower = russian_names::detail::lowered(word);
		if (russian_names::detail::recognised_first_name(lower)) return false;
		const int likeness = surname_likeness(word);
		if (likeness >= 2) return true;
		if (likeness < 1) return false;
		for (const char *ending : {"ый", "ий", "ой", "ая", "яя", "ое", "ее", "ые", "ие", "ым", "им", "ую"})
			if (russian_names::detail::ends_with(lower, ending)) return false;
		return true;
	};
	std::size_t gap = start;
	if (gap > 0 && text[gap - 1] == ' ') --gap;
	else if (gap >= 2 && text.compare(gap - 2, 2, "\xC2\xA0") == 0) gap -= 2;
	if (gap < start) {
		std::size_t wordStart = gap;
		while (wordStart > 0) {
			std::size_t previous = wordStart - 1;
			while (previous > 0 && (static_cast<unsigned char>(text[previous]) & 0xC0) == 0x80) --previous;
			if (!continues_word_at(text, previous)) break;
			wordStart = previous;
		}
		if (wordStart < gap && surnameLike(text.substr(wordStart, gap - wordStart))) return {wordStart, start + length - wordStart};
	}
	std::size_t after = start + length;
	if (after < text.size() && text[after] == ' ') ++after;
	else if (text.compare(after, 2, "\xC2\xA0") == 0) after += 2;
	else return {start, length};
	std::size_t wordEnd = after;
	while (wordEnd < text.size() && continues_word_at(text, wordEnd)) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + wordEnd,
			static_cast<utf8proc_ssize_t>(text.size() - wordEnd), &codePoint);
		if (consumed <= 0) break;
		wordEnd += static_cast<std::size_t>(consumed);
	}
	if (wordEnd > after && surnameLike(text.substr(after, wordEnd - after))) return {start, wordEnd - start};
	return {start, length};
}

// A blank or a bracketed placeholder a draft leaves for a fact it does not
// have - "____________", "[Name]", "[Title]", "[●]", "[Customer Authorized
// Signatory Name]", «[ФИО]». It stands for no one. The checkpoint took "[Name]"
// under both signatures of a live draft for one person, both became one
// symbol, and the reviewer asked for a redraft because the same individual
// signed for both parties (25 Sep 2026). Only words a placeholder is made of,
// so "[John Smith]" or "[Harbor Point]" - a real name a draft brackets for
// confirmation - stays masked.
bool is_drafting_placeholder(const std::string &text, std::size_t start, std::size_t length) {
	if (length == 0 || start + length > text.size()) return false;
	const auto fillAt = [&](const std::string &value, std::size_t index) -> std::size_t {
		const char character = value[index];
		if (character == '_' || character == '.' || character == '-' || character == '*' || character == ' '
				|| character == '\t' || character == '/')
			return 1;
		// "…", "●", "•", "–", "—"
		for (const char *mark : {"\xe2\x80\xa6", "\xe2\x97\x8f", "\xe2\x80\xa2", "\xe2\x80\x93", "\xe2\x80\x94"})
			if (value.compare(index, 3, mark) == 0) return 3;
		return 0;
	};
	const auto onlyFill = [&](const std::string &value) {
		if (value.empty()) return false;
		for (std::size_t index = 0; index < value.size();) {
			const std::size_t width = fillAt(value, index);
			if (width == 0) return false;
			index += width;
		}
		return true;
	};
	const std::string value = text.substr(start, length);
	if (value.size() >= 3 && onlyFill(value)) return true;
	std::string inner;
	if (value.size() >= 2 && value.front() == '[' && value.back() == ']') inner = value.substr(1, value.size() - 2);
	else if (start > 0 && text[start - 1] == '[' && start + length < text.size() && text[start + length] == ']')
		inner = value;
	else return false;
	static const std::set<std::string> words{
		"name", "names", "title", "titles", "date", "dates", "address", "addresses", "signature", "signatory",
		"signatories", "authorized", "authorised", "representative", "officer", "party", "parties", "company",
		"entity", "counterparty", "customer", "vendor", "supplier", "client", "buyer", "seller", "effective",
		"insert", "amount", "number", "no", "email", "e", "mail", "phone", "telephone", "fax", "city", "state",
		"country", "jurisdiction", "position", "capacity", "full", "legal", "registered", "office", "notice",
		"attention", "attn", "recipient", "sum", "price", "fee", "fees", "term", "period", "percentage", "percent",
		"rate", "day", "days", "month", "months", "year", "years", "tbd", "tbc", "blank", "other", "its", "the",
		"of", "for", "and", "or", "to", "be", "confirmed", "determined", "agreed", "specify", "reference", "first",
		"last", "middle", "print", "printed", "typed", "place", "here", "witness", "notary", "seal", "x", "xx",
		"xxx", "individual", "person", "contact", "details", "account", "id", "identification", "if", "any",
		"applicable", "each", "a", "an", "such", "street", "zip", "postal", "code", "time", "execution", "closing",
		"фио", "ф", "и", "о", "должность", "дата", "адрес", "наименование", "подпись", "реквизиты", "сумма",
		"номер", "указать", "вставить", "телефон", "город", "полное", "полностью", "организации", "организация",
		"представитель", "представителя", "уполномоченного", "уполномоченное", "лица", "лицо", "стороны", "сторона",
		"место", "печати", "мп", "инн", "огрн", "кпп", "договора", "подписания",
	};
	std::string word;
	std::size_t counted = 0;
	bool placeholder = true;
	const auto judge = [&] {
		if (word.empty()) return;
		++counted;
		const std::string form = normalized_entity_form(word);
		if (!form.empty() && words.count(form) == 0 && !onlyFill(word) && !is_generic_role_term(EntityType::Organisation, word)
				&& !is_russian_role_term(EntityType::Organisation, word))
			placeholder = false;
		word.clear();
	};
	for (std::size_t index = 0; index < inner.size(); ++index) {
		const char character = inner[index];
		if (character == ' ' || character == ',' || character == '(' || character == ')' || character == ':'
				|| character == ';' || character == '/' || character == '\'')
			judge();
		else word.push_back(character);
	}
	judge();
	return placeholder && (counted > 0 || onlyFill(inner) || inner.empty());
}

// "a Texas limited liability company", "a Delaware corporation": a
// jurisdiction the text names as where a party was formed, as public as the
// law that governs it. The checkpoint took "Texas" there for an organisation,
// and a draft's party definition read "a [[E4]] limited liability company".
// Only before a lower-case description of a kind of entity, so "Texas
// Corporation" or "Delaware Holdings" - a name - stays masked.
bool written_as_state_of_formation(const std::string &text, std::size_t start, std::size_t length) {
	if (!is_public_jurisdiction(text.substr(start, length))) return false;
	std::size_t after = start + length;
	while (after < text.size() && text[after] == ' ') ++after;
	static const std::vector<std::string> kinds{"limited liability company", "limited liability partnership",
		"limited partnership", "general partnership", "nonprofit corporation", "non-profit corporation",
		"not-for-profit corporation", "professional corporation", "public benefit corporation", "benefit corporation",
		"statutory trust", "business trust", "banking corporation", "insurance company", "public limited company",
		"private limited company", "limited company", "corporation", "company", "partnership", "trust"};
	for (const std::string &kind : kinds) {
		if (text.compare(after, kind.size(), kind) != 0) continue;
		const std::size_t end = after + kind.size();
		if (end >= text.size() || !std::isalpha(static_cast<unsigned char>(text[end]))) return true;
	}
	return false;
}

// What the model detected, as masking candidates: above the type's threshold,
// inside the text, a name without its possessive ending, and never public
// legal vocabulary. One place, so a value is judged the same way whether its
// detections were just scored or came from the durable store.
// ---------------------------------------------------------------------------
// Ordinary words by evidence. The local model sometimes tags a word no one is
// named by - a pronoun, a currency sign, a public abbreviation, a legal term
// in lower case, Singularity's own field names - and every such symbol takes from
// the model the very words the lawyer's request is made of ("which of de
// minimis, basket, caps applies?" reached it as three symbols, 26 Sep 2026)
// or ends a chat: «ДС» (дополнительное соглашение) tagged an organisation in
// a model's own earlier answer blocked every later turn. Each rule below
// needs evidence from the language or from the text itself, and applies only
// to what the model alone found: the Project's registry, the lawyer's custom
// terms and every deterministic kind are matched on their own, first, and are
// never let go here. Doubt keeps the mask on.

std::string upper_case(const std::string &value);
std::string normalized_entity_form(const std::string &value);

std::vector<std::string> phrase_words(const std::string &value) {
	std::vector<std::string> words;
	std::string word;
	for (const char character : value) {
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
			if (!word.empty()) words.push_back(word);
			word.clear();
		} else {
			word.push_back(character);
		}
	}
	if (!word.empty()) words.push_back(word);
	return words;
}

// Grammar words, never an identity on their own: the pronouns, determiners and
// the like of both languages. A closed list - it is grammar, not vocabulary.
bool closed_class_word(const std::string &value) {
	static const std::set<std::string> words{
		"he", "she", "it", "they", "him", "her", "his", "hers", "them", "their", "theirs", "we", "us", "our", "you",
		"your", "i", "me", "my", "this", "that", "these", "those", "who", "whom", "whose", "which", "one", "ones",
		"он", "она", "оно", "они", "его", "её", "ее", "ему", "ей", "им", "ими", "их", "него", "неё", "нее", "нему",
		"ней", "ним", "ними", "них", "мы", "вы", "я", "это", "этот", "эта", "эти", "того", "тот", "та", "те", "кто",
		"что", "который", "которая", "которое", "которые",
	};
	return value.find(' ') == std::string::npos && words.count(normalized_entity_form(value)) > 0;
}

// A currency, a percent or a unit alone - "₽", "USD", "руб.", "%", "days",
// «календарных дней» - counts nothing and names no one.
bool only_signs_and_units(const std::string &value) {
	static const std::set<std::string> units{
		"$", "€", "£", "¥", "₽", "%", "‰", "usd", "eur", "rub", "gbp", "cny", "jpy", "chf", "руб", "р", "коп",
		"dollar", "dollars", "ruble", "rubles", "rouble", "roubles", "euro", "euros", "cent", "cents", "percent",
		"per", "cent", "рубль", "рубля", "рублей", "рублям", "рублями", "копеек", "копейки", "процент", "процента",
		"процентов", "day", "days", "week", "weeks", "month", "months", "year", "years", "hour", "hours", "minute",
		"minutes", "business", "calendar", "working", "день", "дня", "дней", "дням", "неделя", "недели", "недель",
		"месяц", "месяца", "месяцев", "год", "года", "лет", "годам", "час", "часа", "часов", "минута", "минуты",
		"минут", "рабочих", "рабочий", "календарных", "календарный", "календарные",
	};
	bool any = false;
	for (const std::string &word : phrase_words(value)) {
		std::string stripped = word;
		while (!stripped.empty() && (stripped.back() == '.' || stripped.back() == ',')) stripped.pop_back();
		const std::string folded = normalized_entity_form(stripped);
		if (units.count(folded) > 0 || units.count(stripped) > 0) { any = true; continue; }
		return false;
	}
	return any;
}

// A name in lowerCamelCase, or a dotted path of such names: Singularity's own
// field and schema names ("localFacts", "artifactAction.operations[0]"), not
// a party's. Two lower-case letters at least before the first capital, so a
// brand written "eBay" or "iPhone" keeps its mask.
bool written_as_field_name(const std::string &value) {
	if (value.empty() || !(value.front() >= 'a' && value.front() <= 'z')) return false;
	bool capital = false;
	std::size_t lowerRun = 0;
	bool segmentStart = true;
	for (std::size_t index = 0; index < value.size(); ++index) {
		const char character = value[index];
		if (character >= 'a' && character <= 'z') { if (!capital) ++lowerRun; segmentStart = false; continue; }
		if (character >= 'A' && character <= 'Z') {
			if (segmentStart || (!capital && lowerRun < 2)) return false;
			capital = true;
			continue;
		}
		if (character >= '0' && character <= '9') { segmentStart = false; continue; }
		if (character == '.' || character == '[' || character == ']') { segmentStart = character == '.'; continue; }
		return false;
	}
	return capital;
}

struct TextWord {
	std::size_t start = 0;
	std::size_t end = 0;
	utf8proc_int32_t first = 0;
	bool capital_inside = false;
};

std::vector<TextWord> letter_words(const std::string &text) {
	std::vector<TextWord> words;
	std::size_t offset = 0;
	bool inside = false;
	while (offset < text.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + offset,
			static_cast<utf8proc_ssize_t>(text.size() - offset), &codePoint);
		if (consumed <= 0) { ++offset; inside = false; continue; }
		const auto category = utf8proc_category(codePoint);
		const bool letter = category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL
			|| category == UTF8PROC_CATEGORY_LT || category == UTF8PROC_CATEGORY_LO;
		if (letter) {
			if (!inside) words.push_back({offset, offset, codePoint, false});
			else if (category == UTF8PROC_CATEGORY_LU) words.back().capital_inside = true;
			words.back().end = offset + static_cast<std::size_t>(consumed);
			inside = true;
		} else {
			inside = false;
		}
		offset += static_cast<std::size_t>(consumed);
	}
	return words;
}

// Whether an abbreviation's letters are the first letters, in order, of words
// this text writes as ordinary words: «ДС» of «дополнительное соглашение»,
// "SLA" of "service level agreement". Every word after the first is written
// in lower case there - "Иван Сидоров" is never «ИС» - and an English word is
// a word of the language.
bool spelled_out_in(const std::string &text, const std::string &abbreviation) {
	std::vector<utf8proc_int32_t> letters;
	for (std::size_t offset = 0; offset < abbreviation.size();) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(abbreviation.data()) + offset,
			static_cast<utf8proc_ssize_t>(abbreviation.size() - offset), &codePoint);
		if (consumed <= 0) return false;
		letters.push_back(utf8proc_tolower(codePoint));
		offset += static_cast<std::size_t>(consumed);
	}
	if (letters.size() < 2) return false;
	const std::vector<TextWord> words = letter_words(text);
	for (std::size_t index = 0; index + letters.size() <= words.size(); ++index) {
		bool matches = true;
		for (std::size_t part = 0; part < letters.size() && matches; ++part) {
			const TextWord &word = words[index + part];
			if (utf8proc_tolower(word.first) != letters[part] || word.end - word.start < 4) { matches = false; break; }
			// Next to each other: only a space or a hyphen between them.
			if (part > 0) {
				const std::string between = text.substr(words[index + part - 1].end,
					word.start - words[index + part - 1].end);
				if (between.find_first_not_of(" -") != std::string::npos || between.empty()) { matches = false; break; }
				if (utf8proc_category(word.first) == UTF8PROC_CATEGORY_LU || word.capital_inside) { matches = false; break; }
			}
			const std::string written = text.substr(word.start, word.end - word.start);
			if (word.first < 0x80 && !is_common_word(written)) matches = false;
		}
		if (matches) return true;
	}
	return false;
}

// A public abbreviation written alone - in capitals, two to six letters, no
// figure: on the public list, or spelled out in this text itself.
bool ordinary_abbreviation(const std::string &text, const std::string &value) {
	if (value.find(' ') != std::string::npos || value.size() < 2 || contains_digit(value)) return false;
	std::size_t letters = 0;
	for (std::size_t offset = 0; offset < value.size();) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
			static_cast<utf8proc_ssize_t>(value.size() - offset), &codePoint);
		if (consumed <= 0) return false;
		if (utf8proc_category(codePoint) != UTF8PROC_CATEGORY_LU && codePoint != '&') return false;
		if (codePoint != '&') ++letters;
		offset += static_cast<std::size_t>(consumed);
	}
	if (letters < 2 || letters > 6) return false;
	return is_public_abbreviation(value) || spelled_out_in(text, value);
}

// A lower-case phrase that is the law's own Latin ("de minimis", "pro rata"),
// or a term the documents of this request define ("basket" where the purchase
// agreement defines '(the "Basket")') - and that this text never writes with a
// capital except where a sentence begins. A lower-case word with no such
// evidence keeps its mask: "payments run through apple" names a company.
bool ordinary_lowercase_phrase(const std::string &text, const std::string &value,
		const std::set<std::string> *defined_terms) {
	const std::string plain = plain_phrase(value);
	if (plain.empty()) return false;
	if (std::any_of(plain.begin(), plain.end(), [](char character) { return character >= 'A' && character <= 'Z'; }))
		return false;
	std::string singular = plain;
	if (singular.size() > 3 && singular.back() == 's') singular.pop_back();
	const bool defined = defined_terms != nullptr
		&& (defined_terms->count(normalized_entity_form(plain)) > 0 || defined_terms->count(normalized_entity_form(singular)) > 0);
	if (!is_legal_latin_phrase(plain) && !defined) return false;
	const WordUsage usage = word_usage(text, value);
	return usage.capitalised == usage.shouted;
}

// The terms the documents of a request define in quotes - '(the "Basket")',
// '(a "Claim Notice")', «(далее — «Инцидент»)» - in comparison form; never a
// party's short name, which defined_as_role() keeps masked.
std::set<std::string> terms_defined_in(const std::vector<SemanticLeaf> &leaves) {
	std::set<std::string> terms;
	const std::pair<std::string, std::string> quotes[] = {{"\"", "\""}, {"\xe2\x80\x9c", "\xe2\x80\x9d"},
		{"\xc2\xab", "\xc2\xbb"}};
	for (const SemanticLeaf &leaf : leaves) {
		const std::string &text = leaf.text;
		for (const auto &[open, close] : quotes) {
			for (std::size_t at = text.find(open); at != std::string::npos; at = text.find(open, at + 1)) {
				// Only a quote a definition's bracket opens shortly before - '(the
				// "Basket")', «(далее — «Инцидент»)»: the full reading of a term
				// is paid for definitions only.
				const std::size_t bracket = text.rfind('(', at);
				if (bracket == std::string::npos || at - bracket > 48
						|| text.find_first_of(")\n", bracket) < at)
					continue;
				const std::size_t from = at + open.size();
				const std::size_t to = text.find(close, from);
				if (to == std::string::npos || to - from < 3 || to - from > 60) continue;
				const std::string term = text.substr(from, to - from);
				if (term.find_first_of("\n\t\"") != std::string::npos) continue;
				if (defined_as_role(text, term)) terms.insert(normalized_entity_form(term));
			}
		}
		// A term a definition introduces by what it means (owner's decision, 26
		// Sep 2026; DESIGN4 §6.8): '"Net Sales" means ...', '"Contract Year" shall
		// mean ...', «Премия» означает ... - capitalised ordinary words, no figure,
		// no legal form: the contract's vocabulary, never a party's name.
		for (const auto &[open, close] : quotes) {
			for (std::size_t at = text.find(open); at != std::string::npos; at = text.find(open, at + 1)) {
				const std::size_t from = at + open.size();
				const std::size_t to = text.find(close, from);
				if (to == std::string::npos || to - from < 3 || to - from > 60) continue;
				const std::string term = text.substr(from, to - from);
				if (term.find_first_of("\n\t\"") != std::string::npos || contains_digit(term)) continue;
				std::size_t after = to + close.size();
				while (after < text.size() && text[after] == ' ') ++after;
				const bool means = text.compare(after, 5, "means") == 0 || text.compare(after, 10, "shall mean") == 0
					|| text.compare(after, 15, "has the meaning") == 0 || text.compare(after, 13, "is defined as") == 0
					|| text.compare(after, 16, "\xD0\xBE\xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB0\xD0\xB5\xD1\x82") == 0;  // означает
				if (!means) continue;
				const std::string plain = plain_phrase(term);
				if (!plain.empty()) {
					std::string lowered;
					bool capitals = true;
					for (std::size_t index = 0; index < plain.size(); ++index) {
						const bool wordStart = index == 0 || plain[index - 1] == ' ' || plain[index - 1] == '-';
						if (wordStart && !(plain[index] >= 'A' && plain[index] <= 'Z')) capitals = false;
						lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(plain[index]))));
					}
					if (capitals && ordinary_lowercase_words(lowered)) terms.insert(normalized_entity_form(term));
				} else if (russian_term_shape(term)) {
					terms.insert(normalized_entity_form(term));
				}
			}
		}
	}
	return terms;
}

// A capitalised phrase of ordinary words the text uses as a term of its own -
// "any Royalty Quarter", "each Contract Year": written after a word that
// counts or picks out instances of a thing (each, any, every, such), which no
// one's name is written after - or a party's role with only roles before it
// ("Sellers' Representative"). A company named in ordinary words ("Arctic
// Mechanical Services", "Granite Trust Company") is neither, and keeps its mask.
bool capitalised_term_of_ordinary_words(const std::string &text, const std::string &value, EntityType type) {
	static_cast<void>(type);
	static const std::set<std::string> roles{"seller", "sellers", "buyer", "buyers", "purchaser", "purchasers",
		"representative", "representatives", "agent", "agents", "party", "parties", "tenant", "tenants", "landlord",
		"landlords", "licensee", "licensees", "licensor", "licensors", "lender", "lenders", "borrower", "borrowers",
		"guarantor", "guarantors", "shareholder", "shareholders", "stockholder", "stockholders", "employer", "employee",
		"employees", "investor", "investors", "indemnitor", "indemnitee", "indemnitors", "indemnitees", "beneficiary",
		"beneficiaries", "holder", "holders", "subcontractor", "subcontractors"};
	const std::string plain = plain_phrase(value);
	if (plain.empty()) return false;
	const std::vector<std::string> words = phrase_words(plain);
	if (words.size() < 2 || words.size() > 4) return false;
	std::vector<std::string> lowered;
	for (const std::string &word : words) {
		if (word.front() < 'A' || word.front() > 'Z') return false;
		std::string low = word;
		for (char &character : low) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		if (!is_common_word(low)) return false;
		lowered.push_back(low);
	}
	const auto roleWord = [&](std::string word) {
		if (word.size() > 2 && word.compare(word.size() - 2, 2, "'s") == 0) word.erase(word.size() - 2);
		else if (!word.empty() && word.back() == '\'') word.pop_back();
		return roles.count(word) > 0;
	};
	if (std::all_of(lowered.begin(), lowered.end(), roleWord)) return true;
	static const std::set<std::string> counting{"each", "any", "every", "such"};
	for (std::size_t found = text.find(value); found != std::string::npos; found = text.find(value, found + 1)) {
		std::size_t cursor = found;
		while (cursor > 0 && text[cursor - 1] == ' ') --cursor;
		std::size_t begin = cursor;
		while (begin > 0 && std::isalpha(static_cast<unsigned char>(text[begin - 1])) != 0) --begin;
		std::string before = text.substr(begin, cursor - begin);
		for (char &character : before) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		if (counting.count(before) > 0) return true;
	}
	return false;
}

// The code inside an identifier span the model widened over its document's
// words: after "№", "No." or "#" when one stands in it, else from its first
// word with a figure. The span as it was when no words stand before a code.
std::pair<std::size_t, std::size_t> code_within(const std::string &text, std::size_t start, std::size_t length) {
	const std::string span = text.substr(start, length);
	std::size_t codeStart = std::string::npos;
	for (const char *sign : {"\xE2\x84\x96", "No.", "#", "Nr."}) {
		const std::size_t found = span.rfind(sign);
		if (found == std::string::npos) continue;
		std::size_t after = found + std::char_traits<char>::length(sign);
		while (after < span.size() && span[after] == ' ') ++after;
		if (codeStart == std::string::npos || after > codeStart) codeStart = after;
	}
	if (codeStart == std::string::npos) {
		std::size_t word = 0;
		while (word < span.size()) {
			std::size_t end = span.find(' ', word);
			if (end == std::string::npos) end = span.size();
			if (std::any_of(span.begin() + static_cast<std::ptrdiff_t>(word), span.begin() + static_cast<std::ptrdiff_t>(end),
					[](char character) { return character >= '0' && character <= '9'; })) {
				codeStart = word;
				break;
			}
			word = end + 1;
		}
	}
	if (codeStart == std::string::npos || codeStart == 0 || codeStart >= span.size()) return {start, length};
	const std::string head = span.substr(0, codeStart);
	if (!has_letter(head) || !contains_digit(span.substr(codeStart))) return {start, length};
	return {start + codeStart, length - codeStart};
}

// The operands of an expression of quantities written in one span - "a + b",
// "a = b + c", «a плюс b» - each trimmed of its spaces; nothing when no
// operator stands between two parts of it.
std::vector<std::pair<std::size_t, std::size_t>> arithmetic_operands(const std::string &text, std::size_t start,
		std::size_t length) {
	static const std::array<const char *, 9> operators{" + ", " = ", " \xC3\x97 ", " * ", " \xE2\x88\x92 ", " - ", " / ",
		" \xD0\xBF\xD0\xBB\xD1\x8E\xD1\x81 ", " \xD0\xBC\xD0\xB8\xD0\xBD\xD1\x83\xD1\x81 "};  // × − плюс минус
	std::vector<std::pair<std::size_t, std::size_t>> operands;
	const std::size_t end = start + length;
	std::size_t from = start;
	bool split = false;
	for (std::size_t cursor = start; cursor < end;) {
		std::size_t matched = 0;
		for (const char *mark : operators) {
			const std::size_t size = std::char_traits<char>::length(mark);
			if (cursor + size <= end && text.compare(cursor, size, mark) == 0) {
				matched = size;
				break;
			}
		}
		if (matched == 0) {
			++cursor;
			continue;
		}
		operands.emplace_back(from, cursor - from);
		cursor += matched;
		from = cursor;
		split = true;
	}
	if (!split) return {};
	operands.emplace_back(from, end - from);
	std::vector<std::pair<std::size_t, std::size_t>> trimmed;
	for (auto [operandStart, operandLength] : operands) {
		while (operandLength > 0 && text[operandStart] == ' ') { ++operandStart; --operandLength; }
		while (operandLength > 0 && text[operandStart + operandLength - 1] == ' ') --operandLength;
		if (operandLength > 0) trimmed.emplace_back(operandStart, operandLength);
	}
	return trimmed;
}

// Whether what the local model alone tagged at text[start, start + length) is
// ordinary by evidence (see above).
bool ordinary_by_evidence(const std::string &text, const std::string &value, std::size_t start, EntityType type,
		const std::set<std::string> *defined_terms) {
	const bool personLike = type == EntityType::Person || type == EntityType::Judge;
	const bool nameLike = personLike || type == EntityType::Organisation || type == EntityType::LegalEntity
		|| type == EntityType::Court || type == EntityType::Location;
	if (closed_class_word(value)) return true;
	if (only_signs_and_units(value)) return true;
	if (written_as_field_name(value)) return true;
	// One letter alone ("J", "J.") names no one: a column letter, a cell
	// reference's letter (live EN1, 26 Sep 2026: "Recalculation!J13" made "J" a
	// person, then every column J). Counted in characters, not bytes.
	if (nameLike) {
		std::size_t letters = 0;
		for (const char character : value) {
			const unsigned char byte = static_cast<unsigned char>(character);
			if (byte >= 0x80) {
				if ((byte & 0xC0) != 0x80) ++letters;
			} else if (std::isalpha(byte)) {
				++letters;
			}
		}
		if (letters < 2) return true;
		// A lower-case identifier written against "(" or "|" is protocol syntax
		// ("abs|positive_part(a)" in an operation's signature, live 26 Sep 2026),
		// never a name: a name is not written that way.
		const bool identifier = !value.empty() && std::all_of(value.begin(), value.end(),
			[](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
		const std::size_t end = start + value.size();
		if (identifier && end <= text.size()
				&& ((end < text.size() && (text[end] == '(' || text[end] == '|')) || (start > 0 && text[start - 1] == '|')))
			return true;
	}
	// A small number alone, with nothing around it saying what it counts, is
	// no one's value: claim "1" and "2", a round count. A number after "№",
	// "No." or "#" is an identifier's and stays.
	if (!value.empty() && value.size() <= 3
			&& std::all_of(value.begin(), value.end(), [](char character) { return character >= '0' && character <= '9'; })
			&& !after_number_sign(text, start)) {
		const NumberMeaning meaning = number_meaning(text, start, value.size());
		if (meaning == NumberMeaning::Unknown || meaning == NumberMeaning::Generic) return true;
	}
	if (type == EntityType::Organisation || type == EntityType::LegalEntity || type == EntityType::Court
			|| type == EntityType::Location) {
		if (ordinary_abbreviation(text, value)) return true;
		if (ordinary_lowercase_phrase(text, value, defined_terms)) return true;
	}
	if (nameLike && !personLike && written_in_lowercase_cyrillic(text, value, start, type)) return true;
	// A term a document of this request defines in quotes, written anywhere in
	// it: '(the "Sellers' Representative")' is a role, whichever passage names it.
	if (nameLike && defined_terms != nullptr && !contains_digit(value)
			&& defined_terms->count(normalized_entity_form(value)) > 0)
		return true;
	if (capitalised_term_of_ordinary_words(text, value, type)) return true;
	return false;
}

// The length of the possessor in text[start, start + length) when a possessive
// inside it is followed only by ordinary lowercase words - "Hospital" of
// "Hospital's staff" - else `length`.
std::size_t possessor_length(const std::string &text, std::size_t start, std::size_t length) {
	const std::string value = text.substr(start, length);
	for (const char *mark : {"'s ", "\xE2\x80\x99s ", "s' ", "s\xE2\x80\x99 "}) {
		const std::string possessive(mark);
		const std::size_t found = value.find(possessive);
		if (found == std::string::npos || found == 0) continue;
		const std::string rest = value.substr(found + possessive.size());
		const std::string plain = plain_phrase(rest);
		if (plain.empty() || !ordinary_lowercase_words(plain)) continue;
		// "s' " keeps the plural's own "s"; the others end where the apostrophe stands.
		return possessive.front() == 's' ? found + 1 : found;
	}
	return length;
}

// The text before `start` on its line, lowercased, its runs of spaces made one
// and the spaces at its end dropped.
std::string lowered_line_before(const std::string &text, std::size_t start) {
	std::size_t lineStart = start;
	while (lineStart > 0 && text[lineStart - 1] != '\n' && text[lineStart - 1] != '\r') --lineStart;
	std::string before;
	for (std::size_t index = lineStart; index < start; ++index) {
		const char character = text[index] == '\t' ? ' ' : text[index];
		if (character == ' ' && (before.empty() || before.back() == ' ')) continue;
		before.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}
	while (!before.empty() && before.back() == ' ') before.pop_back();
	return before;
}

// Whether the place at text[start] is the forum a clause names: right after
// "courts located in", "venue shall lie exclusively in", "arbitration seated
// in". The contract's chosen forum is as public as its chosen law, and a
// Playbook checks it (#245: three NDAs of the working day sent theirs as a
// place's symbol). A party's own place - "a warehouse in Santa Clara County" -
// stands after no such words, and keeps its mask.
bool names_the_forum(const std::string &text, std::size_t start) {
	static const std::vector<std::string> openings{"courts located in", "court located in", "courts sitting in",
		"court sitting in", "courts situated in", "court situated in", "courts in and for", "court in and for",
		"courts in", "court in", "courts of", "courts for", "court for", "venue in", "venue of", "venue shall be in",
		"venue shall be exclusively in", "venue shall lie in", "venue shall lie exclusively in", "venue will be in",
		"venue will lie in", "venue is in", "venue exclusively in", "venue lies in", "venue lies exclusively in",
		"arbitration seated in", "arbitration held in", "arbitration conducted in", "seat of arbitration shall be",
		"seat of the arbitration shall be", "seat of arbitration is", "seat of the arbitration is",
		"seat of arbitration will be", "seat of the arbitration will be"};
	const std::string before = lowered_line_before(text, start);
	for (const std::string &opening : openings)
		if (before.size() >= opening.size() && before.compare(before.size() - opening.size(), opening.size(), opening) == 0
				&& (before.size() == opening.size() || before[before.size() - opening.size() - 1] == ' '))
			return true;
	return false;
}

// Whether a detected value, in lowercase, says what a party is before the
// jurisdiction it was formed under - "university of the State of Ohio" out of
// "a public university of the State of Ohio" (#245): a description, as "a
// Delaware corporation" is (written_as_state_of_formation()). A capitalised
// name - "University of Ohio" - is not asked.
bool described_by_its_state(const std::string &value) {
	if (value.empty() || value.front() < 'a' || value.front() > 'z') return false;
	const std::size_t of = value.find(" of ");
	if (of == std::string::npos || !ordinary_lowercase_words(value.substr(0, of))) return false;
	std::string tail = value.substr(of + 4);
	if (tail.rfind("the ", 0) == 0) tail.erase(0, 4);
	for (const char *state : {"State of ", "Commonwealth of ", "Province of ", "Republic of "})
		if (tail.rfind(state, 0) == 0) tail.erase(0, std::char_traits<char>::length(state));
	return is_public_jurisdiction(tail);
}

bool generic_company_word(const std::string &word);

// A company's own department named by its function - "Legal Department",
// "Finance Department", "Human Resources Department" - says where a notice
// goes inside a party, not which party (#245): the party's name beside it
// stays masked. So does one named by its function's words alone, as
// departments now often are - "People Operations", "Revenue Operations",
// "People and Culture", "Legal Operations" (round 5) - when at least one of
// those words is a function's only, never a trade a company is named by:
// "Arctic Mechanical Services" keeps its mask.
bool names_an_internal_department(const std::string &value) {
	static const std::set<std::string> functions{"legal", "finance", "financial", "accounting", "accounts", "tax",
		"treasury", "compliance", "procurement", "purchasing", "sales", "marketing", "engineering", "operations",
		"security", "privacy", "risk", "audit", "internal audit", "contracts", "human resources", "hr", "it",
		"information technology", "investor relations", "communications", "facilities", "customer service",
		"quality", "regulatory", "credit", "billing", "payroll", "benefits", "business development",
		"corporate development", "research and development", "legal and compliance", "finance and accounting"};
	static const std::set<std::string> units{"department", "dept.", "dept", "team", "division"};
	// The words functions are named with, each alone: those above, and the
	// words the people, talent, revenue and finance functions add ("People and
	// Culture", "Talent Acquisition", "Customer Success", "Accounts Payable").
	static const std::set<std::string> functionWords = [] {
		std::set<std::string> words{"and", "&", "people", "culture", "talent", "acquisition", "recruiting", "revenue",
			"success", "affairs", "payable", "receivable", "collections", "payments"};
		for (const std::string &function : functions) {
			std::size_t position = 0;
			while (position <= function.size()) {
				const std::size_t end = std::min(function.find(' ', position), function.size());
				if (end > position) words.insert(function.substr(position, end - position));
				position = end + 1;
			}
		}
		return words;
	}();
	std::string lowered;
	for (const char character : value) {
		if (static_cast<unsigned char>(character) >= 0x80) return false;
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}
	const std::size_t space = lowered.rfind(' ');
	if (space == std::string::npos || value.front() < 'A' || value.front() > 'Z') return false;
	if (units.count(lowered.substr(space + 1)) > 0 && functions.count(lowered.substr(0, space)) > 0) return true;
	// Named by function words alone, with or without its unit: every word a
	// function's, each but a joining word written with a capital, and one of
	// them no trade's - two words at least, so "Legal" alone is judged as
	// written.
	const std::string named = units.count(lowered.substr(space + 1)) > 0 ? lowered.substr(0, space) : lowered;
	bool functionOnly = false;
	std::size_t words = 0;
	std::size_t position = 0;
	while (position <= named.size()) {
		const std::size_t end = std::min(named.find(' ', position), named.size());
		const std::string word = named.substr(position, end - position);
		if (!word.empty()) {
			if (functionWords.count(word) == 0) return false;
			const bool joining = word == "and" || word == "&";
			if (!joining && (value[position] < 'A' || value[position] > 'Z')) return false;
			if (!joining && !generic_company_word(word)) functionOnly = true;
			if (!joining) ++words;
		}
		position = end + 1;
	}
	return functionOnly && words >= 2;
}

bool legal_form_word(const std::string &word);

bool is_name_label_type(const std::optional<EntityType> &type) {
  return type && (*type == EntityType::Person || *type == EntityType::Judge || *type == EntityType::Organisation
      || *type == EntityType::LegalEntity || *type == EntityType::Court || *type == EntityType::Location);
}

// Trims what a name's span took of the text around it (P0, 30 Oct 2026): a
// closing mark it never opened ("LLC]", "LLC]\"}" of a tracked change in a
// JSON string), an opening mark it never closes ("written ["), the tail of a
// possessive it began inside ("’s written"), and a separator after it. The
// bytes left are the text's own, so a name keeps its exact writing.
void trim_marks_around_name(const std::string &text, std::size_t &from, std::size_t &to) {
  const auto endsWith = [&](const std::string &mark) {
    return to - from >= mark.size() && text.compare(to - mark.size(), mark.size(), mark) == 0;
  };
  const auto beginsWith = [&](const std::string &mark) {
    return to - from >= mark.size() && text.compare(from, mark.size(), mark) == 0;
  };
  bool trimmed = true;
  while (trimmed && to > from) {
    trimmed = false;
    while (to > from && (text[to - 1] == ' ' || text[to - 1] == ',' || text[to - 1] == ';' || text[to - 1] == ':')) {
      --to;
      trimmed = true;
    }
    while (to > from && text[from] == ' ') {
      ++from;
      trimmed = true;
    }
    if (to <= from) break;
    const std::string span = text.substr(from, to - from);
    for (const auto &[open, close] : paired_marks()) {
      if (endsWith(close) && times_in(span, open) < times_in(span, close)) {
        to -= close.size();
        trimmed = true;
        break;
      }
      if (endsWith(open)) {
        to -= open.size();
        trimmed = true;
        break;
      }
      if (beginsWith(open) && times_in(span, open) > times_in(span, close)) {
        from += open.size();
        trimmed = true;
        break;
      }
      if (beginsWith(close)) {
        from += close.size();
        trimmed = true;
        break;
      }
    }
    if (trimmed) continue;
    if (times_in(span, "\"") % 2 == 1) {
      if (endsWith("\"")) {
        --to;
        trimmed = true;
      } else if (beginsWith("\"")) {
        ++from;
        trimmed = true;
      }
      if (trimmed) continue;
    }
    for (const char *tail : {"\xE2\x80\x99s ", "'s "}) {
      if (beginsWith(tail)) {
        from += std::char_traits<char>::length(tail);
        trimmed = true;
        break;
      }
    }
  }
}

// The names inside a span that reads as text (reads_as_cut_of_text): each run
// of capitalised words, with the words that join a name inside it ("Bank of
// the West") and the legal form after it ("Brightwater Foods, LLC"). Each is
// judged afterwards like any span the model returned, so a party named inside
// a cut is still masked and a sentence's capitalised first word is not.
std::vector<std::pair<std::size_t, std::size_t>> name_runs_in(const std::string &text, std::size_t from, std::size_t to) {
  struct Word {
    std::size_t start = 0;
    std::size_t end = 0;
    bool capital = false;
    bool joins = false;
    bool form = false;
    bool figure = false;
    bool sentenceEnd = false;
  };
  static const std::set<std::string> joiningWords{"of", "and", "&", "the", "de", "du", "da", "di", "del", "della",
      "der", "den", "van", "von", "la", "le", "y", "for"};
  static const std::set<std::string> grammar{"the", "a", "an", "this", "that", "these", "those", "its", "their", "our",
      "his", "her", "said", "such", "any", "each", "all", "no"};
  std::vector<Word> words;
  for (std::size_t at = from; at < to;) {
    while (at < to && text[at] == ' ') ++at;
    if (at >= to) break;
    std::size_t end = at;
    while (end < to && text[end] != ' ') ++end;
    const std::string written = text.substr(at, end - at);
    const std::string letters = word_letters(written);
    Word word;
    word.start = at;
    word.end = end;
    word.form = legal_form_word(letters) || legal_form_word(written);
    word.capital = starts_with_capital_letter(letters) && !letters.empty();
    // A figure after a name's words is its own: "Freezer Room 3", "Building 7".
    word.figure = !letters.empty() && letters[0] >= '0' && letters[0] <= '9';
    word.joins = joiningWords.count(ascii_lower(letters)) > 0 && !word.capital;
    // A word that ends a sentence ends a run too: "breach. Plaintiff".
    word.sentenceEnd = !written.empty() && written.back() == '.' && !word.form && !word.capital;
    words.push_back(word);
    at = end;
  }
  std::vector<std::pair<std::size_t, std::size_t>> runs;
  std::size_t index = 0;
  while (index < words.size()) {
    if (!words[index].capital && !words[index].form) {
      ++index;
      continue;
    }
    std::size_t last = index;
    std::size_t next = index + 1;
    while (next < words.size() && !words[next - 1].sentenceEnd
        && (words[next].capital || words[next].form || words[next].joins || words[next].figure)) {
      if (words[next].capital || words[next].form || words[next].figure) last = next;
      ++next;
    }
    std::size_t start = words[index].start;
    std::size_t end = words[last].end;
    trim_marks_around_name(text, start, end);
    // A full stop after a word that is not a legal form is the sentence's.
    if (end > start && text[end - 1] == '.' && !legal_form_word(word_letters(text.substr(words[last].start,
            words[last].end - words[last].start))))
      --end;
    bool onlyGrammar = true;
    for (std::size_t inner = index; inner <= last; ++inner)
      if (grammar.count(ascii_lower(word_letters(text.substr(words[inner].start, words[inner].end - words[inner].start))))
          == 0)
        onlyGrammar = false;
    if (end > start && !onlyGrammar) runs.emplace_back(start, end);
    index = next;
  }
  return runs;
}

// The model's spans with what a name's span took of the text around it put
// back: trimmed of unpaired marks, and a span that reads as text replaced by
// the names inside it. Every other span is the model's own.
std::vector<ModelEntity> spans_of_names(const std::string &text, const std::vector<ModelEntity> &detected) {
  std::vector<ModelEntity> cleaned;
  cleaned.reserve(detected.size());
  for (const ModelEntity &entity : detected) {
    const std::optional<EntityType> type = entity_type_from_model_label(entity.label);
    if (!is_name_label_type(type) || entity.length == 0 || entity.start > text.size()
        || entity.length > text.size() - entity.start) {
      cleaned.push_back(entity);
      continue;
    }
    std::size_t from = entity.start;
    std::size_t to = entity.start + entity.length;
    trim_marks_around_name(text, from, to);
    if (to <= from) continue;
    // A phrase public as a whole - "the law of England and Wales", a
    // section's place "new Section 9A" - is judged whole, as before.
    const std::string span = text.substr(from, to - from);
    if (!reads_as_cut_of_text(span) || is_public_legal_vocabulary(*type, span, ValueOrigin::Detected)
        || stands_as_structural_id(text, from, to - from)) {
      ModelEntity kept = entity;
      kept.start = from;
      kept.length = to - from;
      cleaned.push_back(kept);
      continue;
    }
    for (const auto &[runFrom, runTo] : name_runs_in(text, from, to)) {
      ModelEntity run = entity;
      run.start = runFrom;
      run.length = runTo - runFrom;
      cleaned.push_back(run);
    }
  }
  return cleaned;
}

// `document_text`: `text` is a document's own text (a passage, a title), never
// the lawyer's request or an earlier answer - the one place written_as_lowercase_prose()
// is asked.
// A date's span ends with the date. The model once returned "March 6 email"
// of "its March 6 email" as the date, the colleague wrote "[[E20]] email", and
// the reply read "March 6 email email" (30 Oct 2026). Lowercase words after
// the date are the text's own - but only where what is left still reads as a
// date ("6 марта" keeps its month, "6:00 pm" its hour).
std::size_t date_length_without_trailing_words(const std::string &text, std::size_t start, std::size_t length) {
  if (semantic::parse_value(text.substr(start, length)).has_value()) return length;
  std::size_t kept = length;
  for (int words = 0; words < 3; ++words) {
    std::size_t wordEnd = kept;
    while (wordEnd > 0 && text[start + wordEnd - 1] == ' ') --wordEnd;
    std::size_t wordStart = wordEnd;
    while (wordStart > 0 && text[start + wordStart - 1] != ' ') --wordStart;
    if (wordStart == 0 || wordStart == wordEnd) break;
    const bool lowercase = std::all_of(text.begin() + static_cast<std::ptrdiff_t>(start + wordStart),
        text.begin() + static_cast<std::ptrdiff_t>(start + wordEnd), [](char c) { return c >= 'a' && c <= 'z'; });
    if (!lowercase) break;
    std::size_t cut = wordStart;
    while (cut > 0 && text[start + cut - 1] == ' ') --cut;
    kept = cut;
    const std::optional<semantic::Value> value = semantic::parse_value(text.substr(start, kept));
    if (value && (value->kind == semantic::Kind::Date || value->kind == semantic::Kind::DayOfYear)) return kept;
  }
  return length;
}

std::vector<CanonicalEntity> semantic_entities_from(
    const std::string &text, const std::vector<ModelEntity> &detected_as_returned,
    const GuardConfig &config, const std::set<std::string> *defined_terms = nullptr, bool document_text = false) {
  std::vector<CanonicalEntity> result;
  const std::vector<ModelEntity> detected = spans_of_names(text, detected_as_returned);
  for (const auto &entity : detected) {
    const auto type = entity_type_from_model_label(entity.label);
    if (!type || entity.length == 0 || entity.start > text.size() ||
        entity.length > text.size() - entity.start || !std::isfinite(entity.confidence) ||
        entity.confidence < 0.0 || entity.confidence > 1.0 ||
        entity.confidence < config.thresholds.for_type(*type)) {
      continue;
    }
    const bool isName = *type == EntityType::Person || *type == EntityType::Judge
        || *type == EntityType::Organisation || *type == EntityType::LegalEntity
        || *type == EntityType::Court || *type == EntityType::Location;
    // A value never runs across a table cell or a line - an address aside,
    // which is written on several: the model sometimes returns "Brennan
    // Holt<tab>Common", a name and the next cell, and a live Russian contract
    // (24 Sep 2026) had "ТЛ-14/2026\n\tг. Москва" protected as one identifier,
    // so every restoration wrote the heading's next line into the answer.
    std::size_t nameLength = entity.length;
    if (*type != EntityType::Address) {
      const std::size_t cellEnd = text.find_first_of("\t\n\r", entity.start);
      if (cellEnd != std::string::npos && cellEnd > entity.start && cellEnd < entity.start + entity.length) {
        nameLength = cellEnd - entity.start;
        while (nameLength > 0 && text[entity.start + nameLength - 1] == ' ') --nameLength;
      }
    }
    if (*type == EntityType::Date) nameLength = date_length_without_trailing_words(text, entity.start, nameLength);
    std::size_t length = isName ? length_without_possessive(text, entity.start, nameLength) : nameLength;
    // "The Hospital's staff", "Halvane's field technicians": the ordinary
    // lowercase words a possessive is followed by are the text's own; the
    // possessor alone is judged, and masked if it is a name (#245).
    if (isName) length = possessor_length(text, entity.start, length);
    if (length == 0) continue;
    // Judged before the name is trimmed, on the whole phrase the checkpoint
    // returned: "Russian law" and "the law of the State of Texas" name a body
    // of law, and neither is a name with an ordinary word after it.
    if (is_public_legal_vocabulary(*type, text.substr(entity.start, length), ValueOrigin::Detected)) continue;
    // A litigation's term of art the text writes as one, wherever it stands
    // with a capital: "the Complaint", "Defendant’s Answer", "COUNTERCLAIMS"
    // (written_as_term_of_art). A company the text names bare keeps its mask.
    if (*type != EntityType::Person && *type != EntityType::Judge) {
      const std::string phrase = text.substr(entity.start, length);
      if (is_litigation_term_of_art(phrase) && written_as_term_of_art(text, word_letters(phrase))) continue;
    }
    if (stands_as_structural_id(text, entity.start, length)) continue;
    if (*type == EntityType::MoneyAmount && reads_as_period(text, entity.start, length)) continue;
    // "@Kitsune" names a member of the thread, not a mailbox: an address has a
    // local part before its "@" and a domain after it (live run, 24 Oct 2026).
    if (*type == EntityType::Email && text[entity.start] == '@') continue;
    if (document_text && prose_can_be_mistaken_for(*type) && written_as_lowercase_prose(text.substr(entity.start, length)))
      continue;
    // A formula or a reference the text writes as one ("=B8-C8-D8", "A2" in a
    // workbook's row). The same letters and digits in a sentence are a code:
    // "Purchase order PO12345", "Docket No. CV2026".
    if (formula_shape(text.substr(entity.start, length)).shape && written_as_formula_at(text, entity.start, length)) continue;
    const bool personLike = *type == EntityType::Person || *type == EntityType::Judge;
    if (isName && !personLike) {
      const std::size_t trimmed = length_without_trailing_ordinary_word(text, entity.start, length);
      if (trimmed < length) {
        const std::string remainder = text.substr(entity.start, trimmed);
        const std::string cutAway = text.substr(entity.start + trimmed, length - trimmed);
        const bool conceptRemainder = is_legal_concept(remainder, ValueOrigin::Detected);
        const bool genericRemainder = is_generic_role_term(*type, remainder);
        // And again on what is left, for the case it exists for: "liability
        // position" trimmed of its ordinary last word is the concept "liability",
        // which the phrase test never saw - unless what was cut names a kind of
        // organisation: "remedy group" or "default capital" is a company whose
        // first word happens to be a legal concept, and stays masked whole.
        if (conceptRemainder && !names_an_organisation_kind(cutAway)) continue;
        // The same for a generic word the trimmed value is, when only grammar
        // was cut: "Project about" is the lawyer's own Project (real run, 23 Sep
        // 2026), while "project falcon" is a deal's code name and stays masked.
        if (genericRemainder && only_grammar_words(cutAway)) continue;
        // Otherwise the name is kept whole when what is left would not be a name
        // on its own, so "Project falcon" never loses "falcon" to the trim.
        if (!conceptRemainder && !genericRemainder && ends_with_capitalised_word(text, entity.start, trimmed))
          length = trimmed;
      }
    }
    std::size_t start = entity.start;
    if (*type == EntityType::MoneyAmount) {
      start = amount_start(text, entity.start, length);
      length -= start - entity.start;
      // An amount in words whose first words the span left out takes them back.
      std::size_t firstWordEnd = start;
      while (firstWordEnd < start + length && text[firstWordEnd] != ' ') ++firstWordEnd;
      if (is_number_word(text.substr(start, firstWordEnd - start))) {
        const std::size_t widened = amount_words_start(text, start);
        length += start - widened;
        start = widened;
      }
    }
    // A span cut inside a word or a number is widened to it (word_aligned_span).
    const std::pair<std::size_t, std::size_t> aligned = word_aligned_span(text, start, length);
    start = aligned.first;
    length = aligned.second;
    // The first name and patronymic stay a detection of their own as well, so
    // the same words elsewhere («Уважаемый Андрей Игоревич!») are found and
    // weighed as that person.
    std::optional<std::pair<std::size_t, std::size_t>> withoutSurname;
    if (personLike) {
      const std::pair<std::size_t, std::size_t> joined = with_surname_beside(text, start, length);
      if (joined.first != start || joined.second != length) withoutSurname = std::make_pair(start, length);
      std::tie(start, length) = joined;
    }
    // An amount's or a number's span never ends on a separator the text does
    // not continue with a figure: "to $1,540,800, reflecting" protected the
    // comma with the amount, and a live answer came back "$1,540,800,,"
    // (24 Sep 2026). "1,540,800" and "2.5" keep what is inside them.
    if (*type == EntityType::MoneyAmount || !has_letter(text.substr(start, length))) {
      const auto separator = [](char character) {
        return character == ',' || character == '.' || character == ':' || character == ';' || character == '/'
            || character == '-';
      };
      while (length > 0 && separator(text[start + length - 1])
             && !(start + length < text.size() && text[start + length] >= '0' && text[start + length] <= '9'))
        --length;
      if (length == 0) continue;
    }
    // An identifier the model took with its document's words - "Договор займа
    // № ФП-044" - is the code alone: the words stay the text's, and restored
    // they never read «договору займа Договор займа № ФП-044» (ab4 R2).
    if (*type == EntityType::ContractIdentifier || *type == EntityType::CaseNumber) {
      const std::pair<std::size_t, std::size_t> code = code_within(text, start, length);
      start = code.first;
      length = code.second;
    }
    // An expression of amounts is its amounts, each its own value: "1 122 301,37
    // ₽ + 1 107 013,70 ₽" once went out as one row that restored as the
    // expression and was taken for the total (ab4 R2). Never one opaque row: an
    // operand that reads as no value is left to the amounts' own detection.
    if (*type == EntityType::MoneyAmount || *type == EntityType::Number || *type == EntityType::Percentage) {
      const std::vector<std::pair<std::size_t, std::size_t>> operands = arithmetic_operands(text, start, length);
      if (!operands.empty()) {
        const bool readable = std::all_of(operands.begin(), operands.end(), [&](const auto &operand) {
          return semantic::parse_value(text.substr(operand.first, operand.second)).has_value();
        });
        if (readable)
          for (const auto &[operandStart, operandLength] : operands)
            result.push_back(CanonicalEntity{operandStart, operandLength, type, entity.confidence,
                DetectionSource::SemanticModel, entity.label, text.substr(operandStart, operandLength), {}});
        continue;
      }
    }
    const std::string value = text.substr(start, length);
    // Judged on the value as it will be masked, after every trim above: a
    // number by the words around it, never by its length.
    if (detected_number_names_nothing(text, start, length, *type)) continue;
    // Ordinary by evidence: a pronoun, a unit, a public abbreviation, an
    // ordinary phrase, a field name, a small bare number (ordinary_by_evidence).
    if (ordinary_by_evidence(text, value, start, *type, defined_terms)) continue;
    // The text-evidence rule is about people ("the children", "the mother"):
    // a lowercase person needs the text's own evidence that it is a word.
    if (personLike && written_as_common_word(text, value, start)) continue;
    // A document's own phrase of ordinary lowercase words is no one's name
    // either - "a reasonable person", "the non-breaching party" (#245: the NDA
    // working day sent each as a person's symbol) - on the evidence a company's
    // needs below: an article, a determiner or a possessive before it, and no
    // capital wherever the text writes it. A request keeps its own rules.
    if (personLike && document_text && written_in_ordinary_lowercase(text, value)) continue;
    // A legal concept heading a clause on its own line - "1. Confidentiality" -
    // is that clause's heading, whatever it was tagged (#245). A numbered list
    // of names ("1. Baker") stays a list of names.
    if (personLike && stands_as_clause_heading(text, start, length)
        && is_legal_concept(normalized_entity_form(value), ValueOrigin::Detected))
      continue;
    const bool organisationLike = *type == EntityType::Organisation || *type == EntityType::LegalEntity;
    // A company or a legal entity written wholly in lowercase ordinary words is
    // prose: "the non‑breaching party", "an original", "in equity". A
    // Project's own parties stay masked in any case through the registry.
    if (organisationLike && written_in_ordinary_lowercase(text, value)) continue;
    // A clause's heading the text also writes as an ordinary word is no company:
    // "3.2 Counterparts; ... may be executed in counterparts".
    if (organisationLike && written_as_heading_word(text, value, start)) continue;
    // The same for Russian, where the shape decides: «персональными данными».
    if ((personLike || organisationLike) && written_in_lowercase_cyrillic(text, value, start, *type)) continue;
    // A role the text itself defines in quotes is a defined term, not a party:
    // '(the "Breaching Party")'. The party's name before it stays masked.
    if ((personLike || organisationLike) && defined_as_role(text, value)) continue;
    // A code written with underscores names nobody: "LEGAL_ENTITY" whatever it
    // was tagged, "singularity_journal" as a person, a company or a place.
    if (written_as_code(value, true) || (isName && written_as_code(value, false))) continue;
    // A draft's blank or bracketed placeholder, and the state a party was
    // formed in, name no one.
    if (is_drafting_placeholder(text, start, length)) continue;
    if ((organisationLike || *type == EntityType::Location) && written_as_state_of_formation(text, start, length))
      continue;
    if ((organisationLike || *type == EntityType::Location) && described_by_its_state(value)) continue;
    // "European" of "European retailers", once the ordinary word is trimmed: a
    // nationality used before a lowercase noun describes it. Alone - "Swiss",
    // "American" - it may be a party's short name, and keeps its mask.
    if ((organisationLike || *type == EntityType::Location) && is_nationality_adjective(value)
        && start + length + 1 < text.size() && text[start + length] == ' '
        && text[start + length + 1] >= 'a' && text[start + length + 1] <= 'z')
      continue;
    // The forum a clause names, and a party's own department, name no one.
    if ((*type == EntityType::Location || *type == EntityType::Court) && names_the_forum(text, start)) continue;
    if (organisationLike && names_an_internal_department(value)) continue;
    // Inside a public phrase - "the State of Delaware", "Secretary of State" -
    // the words are that phrase (stands_in_public_phrase), as the registry and
    // the leak checks also read them.
    if ((isName || *type == EntityType::ContractIdentifier) && stands_in_public_phrase(text, start, length)) continue;
    result.push_back(CanonicalEntity{
        start,
        length,
        type,
        entity.confidence,
        DetectionSource::SemanticModel,
        entity.label,
        value,
        {},
    });
    if (withoutSurname)
      result.push_back(CanonicalEntity{withoutSurname->first, withoutSurname->second, type, entity.confidence,
          DetectionSource::SemanticModel, entity.label, text.substr(withoutSurname->first, withoutSurname->second), {}});
  }
  return result;
}

std::vector<CanonicalEntity> semantic_entities(const std::string &text,
                                               const ISemanticModelAdapter &model,
                                               const GuardConfig &config) {
  platform::trace::Span modelSpan("ner", "model.single",
      {{"bytes", text.size()}, {"fingerprint", platform::trace::fingerprint(text)}});
  const auto detected = model.detect(text);
  modelSpan.finish();
  return semantic_entities_from(text, detected, config);
}

// Structure and vocabulary, never facts (DESIGN4 §6.8, D12, and the owner's
// decision of 26 Sep 2026, OWNER_DECISIONS_CYCLE4 Q1). Exactly these are not
// masked, whatever a detector took them for:
//   - a workbook's cell reference or range in reference position - the "A12"
//     of "Row 12: A12 = ...", a reference inside a formula "{=SUM(B4:B18)}";
//   - the numeric function code of SUBTOTAL (1-11, 101-111) and AGGREGATE
//     (1-19) as their first argument;
//   - a tax's abbreviation (НДС, VAT, GST ...);
//   - a label of a word and a small number: «Порог 2», "Tier 3" - a lexicon
//     word before a one- or two-figure number, never after "№", "No." or "#";
//   - a defined term, a table's header, a row's or a sheet's label written
//     wholly in the role lexicon's legal-general words ("Net Sales", "Contract
//     Year", «Премия») - only where it stands as one: a term the text defines,
//     a sheet's name, a workbook reading's whole cell, a table's header cell;
//     never in running prose ("paid to Premium Credit" names a company). Never
//     a person, and never with a legal form or a figure in it, so a name, a
//     company, an amount, a date or a rate never moves (the inventory gate:
//     0 protected values move).
bool reference_shape(const std::string &value) {
	static const std::regex reference("^\\$?[A-Z]{1,3}\\$?[0-9]{1,7}(?::\\$?[A-Z]{1,3}\\$?[0-9]{1,7})?$");
	return std::regex_match(value, reference);
}

// A term this text introduces by a definition: '"Net Sales" means',
// '(the "Contract Year")', «Премия» означает.
bool defined_here(const std::string &text, const std::string &value) {
	const std::pair<std::string, std::string> quotes[] = {{"\"", "\""}, {"\xe2\x80\x9c", "\xe2\x80\x9d"},
		{"\xc2\xab", "\xc2\xbb"}};
	for (const auto &[open, close] : quotes) {
		const std::string quoted = open + value + close;
		for (std::size_t at = text.find(quoted); at != std::string::npos; at = text.find(quoted, at + 1)) {
			std::size_t after = at + quoted.size();
			while (after < text.size() && text[after] == ' ') ++after;
			if (text.compare(after, 5, "means") == 0 || text.compare(after, 10, "shall mean") == 0
					|| text.compare(after, 15, "has the meaning") == 0 || text.compare(after, 13, "is defined as") == 0
					|| text.compare(after, 16, "\xD0\xBE\xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB0\xD0\xB5\xD1\x82") == 0)  // означает
				return true;
		}
	}
	// '(the "Contract Year")' - a role the text defines, never a party's short
	// name ('Harbor Point Logistics, Inc. ("Harbor Point")').
	return defined_as_role(text, value);
}

// A table's header cell: "| Net Sales |" on a line a "|---|" line follows. A
// label first on a prose line ("Premium Credit: $5,000") is not one: a list of
// creditors writes companies so.
bool label_position(const std::string &text, std::size_t start, std::size_t end) {
	std::size_t lineStart = start;
	while (lineStart > 0 && text[lineStart - 1] != '\n') --lineStart;
	std::size_t before = start;
	while (before > lineStart && text[before - 1] == ' ') --before;
	std::size_t after = end;
	while (after < text.size() && text[after] == ' ') ++after;
	if (!(before > lineStart && text[before - 1] == '|' && after < text.size() && text[after] == '|')) return false;
	const std::size_t lineEnd = text.find('\n', end);
	if (lineEnd == std::string::npos) return false;
	std::size_t next = lineEnd + 1;
	while (next < text.size() && text[next] == ' ') ++next;
	return text.compare(next, 4, "|---") == 0 || text.compare(next, 5, "| ---") == 0 || text.compare(next, 4, "|:--") == 0;
}

bool structural_non_fact(const std::string &text, const CanonicalEntity &entity) {
	if (entity.source == DetectionSource::CustomTerm || entity.start > text.size() || entity.length == 0
			|| entity.start + entity.length > text.size())
		return false;
	const std::string value = text.substr(entity.start, entity.length);
	const std::optional<EntityType> type = entity.type;
	const bool quantity = type == EntityType::MoneyAmount || type == EntityType::Date || type == EntityType::Percentage
		|| type == EntityType::Email || type == EntityType::Phone || type == EntityType::BankAccount || type == EntityType::Address;
	const std::size_t lineStart = entity.start == 0 ? 0
		: (text.rfind('\n', entity.start - 1) == std::string::npos ? 0 : text.rfind('\n', entity.start - 1) + 1);
	const bool readingLine = text.compare(lineStart, 4, "Row ") == 0;
	// A reference in reference position.
	if (reference_shape(value)) {
		if (inside_formula_text(text, entity.start)) return true;
		const std::size_t end = entity.start + entity.length;
		const bool beforeEquals = text.compare(end, 3, " = ") == 0;
		const bool afterCellStart = entity.start >= 2
			&& (text.compare(entity.start - 2, 2, ": ") == 0 || text.compare(entity.start - 2, 2, "| ") == 0);
		if (readingLine && beforeEquals && afterCellStart) return true;
	}
	// A known function's numeric code.
	if (!value.empty() && value.size() <= 3 && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
		std::size_t before = entity.start;
		while (before > 0 && text[before - 1] == ' ') --before;
		const auto function = [&](const char *name) {
			const std::size_t size = std::char_traits<char>::length(name);
			return before >= size && text.compare(before - size, size, name) == 0;
		};
		const int code = std::stoi(value);
		if (function("SUBTOTAL(") && ((code >= 1 && code <= 11) || (code >= 101 && code <= 111))) return true;
		if (function("AGGREGATE(") && code >= 1 && code <= 19) return true;
	}
	if (quantity || type == EntityType::Person || type == EntityType::Judge) return false;
	// A tax's abbreviation.
	{
		static const std::set<std::string> taxes{"\xD0\x9D\xD0\x94\xD0\xA1", "\xD0\x9D\xD0\x94\xD0\xA4\xD0\x9B",  // НДС НДФЛ
			"\xD0\x95\xD0\x9D\xD0\x92\xD0\x94", "\xD0\xA3\xD0\xA1\xD0\x9D", "\xD0\x95\xD0\xA1\xD0\xA5\xD0\x9D",   // ЕНВД УСН ЕСХН
			"VAT", "GST", "HST", "PST", "QST", "IVA", "TVA", "MwSt", "USt"};
		std::string bare = value;
		while (!bare.empty() && (bare.back() == '.' || bare.back() == ' ')) bare.pop_back();
		if (taxes.count(bare) > 0) return true;
	}
	// A word and a small number: the number of a label.
	if (value.size() <= 2 && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })
			&& !after_number_sign(text, entity.start)) {
		const std::size_t end = entity.start + entity.length;
		const bool numberEnds = end >= text.size() || (!(text[end] >= '0' && text[end] <= '9') && text[end] != '%' && text[end] != ','
			&& text[end] != '.' && text.compare(end, 2, "\xC2\xA0") != 0 && !(text[end] == ' ' && end + 1 < text.size()
				&& text[end + 1] >= '0' && text[end + 1] <= '9'));
		std::size_t wordEnd = entity.start;
		if (numberEnds && wordEnd > 0 && text[wordEnd - 1] == ' ') {
			--wordEnd;
			std::size_t wordStart = wordEnd;
			while (wordStart > 0 && text[wordStart - 1] != ' ' && text[wordStart - 1] != '\n' && text[wordStart - 1] != '\t'
					&& text[wordStart - 1] != '|' && text[wordStart - 1] != '"' && text.compare(wordStart - 1, 1, "(") != 0)
				--wordStart;
			const std::string word = text.substr(wordStart, wordEnd - wordStart);
			const std::vector<std::string> folded = roles::lexicon_detail::folded_words(word);
			const unsigned char lead = word.empty() ? 0 : static_cast<unsigned char>(word[0]);
			const bool capital = (lead >= 'A' && lead <= 'Z') || (lead == 0xD0 && word.size() > 1
				&& static_cast<unsigned char>(word[1]) >= 0x90 && static_cast<unsigned char>(word[1]) <= 0xAF);
			if (capital && folded.size() == 1 && roles::lexicon_word(folded.front())) return true;
		}
	}
	// A defined term or a label in the lexicon's words.
	if (type == EntityType::Organisation || type == EntityType::LegalEntity || type == EntityType::ContractIdentifier) {
		if (contains_digit(value)) return false;
		const std::vector<std::string> words = roles::lexicon_detail::folded_words(value);
		if (words.empty() || words.size() > 5) return false;
		std::size_t letters = 0;
		for (const std::string &word : words) {
			if (!roles::lexicon_word(word)) return false;
			letters += word.size();
		}
		// Joining words alone ("of the") are no term.
		static const std::set<std::string> joining{"of", "and", "the", "to", "for", "on", "in", "by", "with", "at", "a", "an", "per",
			"no"};
		const std::size_t content = static_cast<std::size_t>(std::count_if(words.begin(), words.end(),
			[&](const std::string &word) { return joining.count(word) == 0; }));
		if (content == 0 || letters < 3) return false;
		// Only where the words stand as a term or a label - never in running
		// prose, where "Payment was made to Premium Credit" names a company made
		// of ordinary words. One word alone is a label only where a workbook's
		// reading writes it as one - a sheet's name, a cell's whole text: "Target"
		// in prose, or as a speaker's label, is a company as far as anyone can tell.
		const std::size_t end = entity.start + entity.length;
		const bool sheetName = entity.start >= 7 && text.compare(entity.start - 7, 7, "Sheet \"") == 0
			&& text.compare(end, 1, "\"") == 0;
		const bool wholeCell = readingLine && entity.start >= 3 && text.compare(entity.start - 3, 3, " = ") == 0
			&& (end >= text.size() || text.compare(end, 3, " | ") == 0 || text[end] == '\n');
		if (content < 2) return sheetName || wholeCell;
		return sheetName || wholeCell || defined_here(text, value) || label_position(text, entity.start, end);
	}
	return false;
}

std::vector<CanonicalEntity> reconcile(const std::string &text, std::vector<CanonicalEntity> candidates) {
  // Structure and the contract's own vocabulary are not facts (DESIGN4 §6.8).
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
      [&](const CanonicalEntity &entity) { return structural_non_fact(text, entity); }), candidates.end());
  // One deterministic ordering handles exact duplicates and broader overlaps:
  // custom > deterministic > semantic, then legal-specific semantic label,
  // confidence, longer span, and finally left-to-right position.
  std::stable_sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
    const auto left_priority = source_priority(left.source);
    const auto right_priority = source_priority(right.source);
    if (left_priority != right_priority) return left_priority < right_priority;
    const auto left_specificity = entity_specificity(left);
    const auto right_specificity = entity_specificity(right);
    if (left_specificity != right_specificity) return left_specificity > right_specificity;
    if (left.confidence != right.confidence) return left.confidence > right.confidence;
    if (left.length != right.length) return left.length > right.length;
    if (left.start != right.start) return left.start < right.start;
    return left.source_label < right.source_label;
  });

  std::vector<CanonicalEntity> accepted;
  for (const auto &candidate : candidates) {
    // A losing candidate is not simply dropped: if the higher-precedence
    // span(s) it overlaps do not fully contain it, the sliver of its own
    // range outside their span would otherwise survive substitution as
    // literal, unmapped protected text (the winner replaces only its own
    // span with one placeholder, not the candidate's). Grow every accepted
    // span the candidate's range touches - possibly bridging more than one,
    // since a wide candidate can overlap two independently-accepted spans at
    // once - to their combined union instead, so the accepted result always
    // covers every byte any candidate claimed.
    std::size_t union_start = candidate.start;
    std::size_t union_end = candidate.start + candidate.length;
    std::optional<CanonicalEntity> winner;
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto existing = accepted.begin(); existing != accepted.end();) {
        if (ranges_overlap(union_start, union_end - union_start, existing->start, existing->length)) {
          if (!winner) winner = *existing;
          union_start = std::min(union_start, existing->start);
          union_end = std::max(union_end, existing->start + existing->length);
          existing = accepted.erase(existing);
          changed = true;
        } else {
          ++existing;
        }
      }
    }
    if (!winner) {
      accepted.push_back(candidate);
    } else {
      // `candidate` sorts no higher than any span it overlapped (they were
      // all accepted earlier in this same precedence-ordered pass), so the
      // merged span keeps the first winner's type. Its restoration value
      // must cover the entire union, exactly like its byte range.
      // A span grown past the winner's own value writes more than that value:
      // it is known by its text, never by the value it began with.
      if (union_start != winner->start || union_end != winner->start + winner->length) winner->value_key.clear();
      winner->start = union_start;
      winner->length = union_end - union_start;
      winner->value = text.substr(union_start, winner->length);
      accepted.push_back(*winner);
    }
  }
  std::sort(accepted.begin(), accepted.end(), [](const auto &left, const auto &right) {
    if (left.start != right.start) return left.start < right.start;
    return left.length > right.length;
  });
  return accepted;
}

std::vector<CanonicalEntity>
expand_detected_values_to_all_occurrences(const std::string &text,
                                          const std::vector<CanonicalEntity> &detected) {
  std::vector<CanonicalEntity> expanded = detected;
  for (const auto &entity : detected) {
    if (entity.value.empty()) continue;
    // A number recurs only as the whole number it is, where its own context
    // does not make it structure (number_stands_for), and a spelling with
    // letters only where it begins a word (stands_as_word_or_stem) - a
    // person's own custom term aside, which is found wherever it stands.
    const bool number = !has_letter(entity.value);
    const bool custom = entity.source == DetectionSource::CustomTerm;
    // A day of the year in figures recurs only where a word dates it.
    const bool yearless = number && entity.source == DetectionSource::Deterministic
        && entity.value_key.rfind("DOY:", 0) == 0;
    std::size_t position = 0;
    while ((position = text.find(entity.value, position)) != std::string::npos) {
      const bool stands = number
          ? (custom ? stands_as_whole_number(text, position, entity.value.size())
              : number_stands_for(text, position, entity.value.size(), entity.value, entity.type)
                  && (!yearless || yearless_day(text, position, entity.value.size()).has_value()))
          : custom || stands_as_word_or_stem(text, position, entity.value.size(),
              entity.type == EntityType::Person || entity.type == EntityType::Judge);
      if (position != entity.start && stands) {
        auto occurrence = entity;
        occurrence.start = position;
        expanded.push_back(std::move(occurrence));
      }
      // Step by one byte, not by the matched value's length: a value that
      // overlaps itself in the text (e.g. "ana" inside "banana", found at
      // both byte 1 and byte 3) has a real second occurrence starting
      // inside the first. Stepping past the whole match would silently skip
      // it, leaving an exact, already-mapped value's raw text uncovered by
      // any accepted entity - which the fail-closed masking validator would
      // then correctly detect and block on send.
      position += 1;
    }
  }
  // Re-run the same precedence rules because a repeated value can overlap a
  // higher-priority deterministic/custom span at a different location. Once a
  // value is accepted as sensitive, every remaining exact occurrence must be
  // covered; otherwise the fail-closed masking validator would correctly block
  // common legal documents that repeat party names or identifiers.
  return reconcile(text, std::move(expanded));
}

bool contains_marker_family(const std::string &text, const std::string &family) {
	const std::regex pattern("\\[\\[\\s*" + family + "\\s*[0-9]+\\s*\\]\\]", std::regex_constants::icase);
	return std::regex_search(text, pattern);
}

std::string choose_marker_family(const std::string &text) {
	for (char family = 'E'; family <= 'Z'; ++family) {
		const std::string candidate(1, family);
		if (!contains_marker_family(text, candidate)) return candidate;
	}
	throw std::runtime_error("source text exhausts the compact protected-symbol families");
}

std::string choose_marker_family(const std::vector<SemanticLeaf> &leaves) {
	for (char family = 'E'; family <= 'Z'; ++family) {
		const std::string candidate(1, family);
		const bool collision = std::any_of(leaves.begin(), leaves.end(), [&](const auto &leaf) {
			return contains_marker_family(leaf.text, candidate);
		});
		if (!collision) return candidate;
	}
	throw std::runtime_error("source text exhausts the compact protected-symbol families");
}

std::string compact_marker(const std::string &family, std::size_t index) {
	return "[[" + family + std::to_string(index) + "]]";
}

// The words a typed symbol begins with (markers::prefix_for), compared case-blind.
bool typed_prefix(const std::string &letters) {
	static const std::set<std::string> prefixes{"person", "org", "place", "email", "phone", "url", "id", "amount", "rate",
		"date", "duration", "number", "term", "value"};
	std::string lowered = letters;
	for (char &character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	return prefixes.count(lowered) > 0;
}

// The letters of a symbol's identity: "Amount" of "Amount3", "E" of "E17".
std::string marker_letters(const std::string &base) {
	std::string letters;
	for (const char character : base)
		if (markers::is_ascii_letter(character)) letters += character;
	return letters;
}

// Settles how a fresh table writes its symbols: typed unless the texts it
// protects already write a typed symbol's shape themselves, when the text's
// own words could be read as one of its symbols - then compact.
void settle_marker_style(OperationProtectedSymbolTable &symbols, const std::vector<std::string_view> &texts) {
	if (symbols.marker_style != markers::MarkerStyle::Typed || !symbols.mapping.empty()) return;
	for (const std::string_view text : texts)
		if (markers::collides(text)) {
			symbols.marker_style = markers::MarkerStyle::Compact;
			return;
		}
}

void reject_unissued_operation_markers(const std::string &text,
		const OperationProtectedSymbolTable &symbols) {
	if (symbols.marker_family.empty()) return;
	// A typed symbol's shape a text writes must be one this table issued, as
	// issued: the text's own "[[Amount3]]" would otherwise be read back as it.
	if (symbols.marker_style == markers::MarkerStyle::Typed && text.find('[') != std::string::npos)
		for (const markers::Parsed &parsed : markers::find_all(text)) {
			if (text.compare(parsed.start, 2, "[[") != 0 || !typed_prefix(marker_letters(parsed.base))) continue;
			const std::string base = "[[" + parsed.base + "]]";
			const bool issued = std::any_of(symbols.mapping.begin(), symbols.mapping.end(),
				[&](const RequestMappingEntry &entry) { return entry.placeholder == base; });
			if (!issued || !parsed.canonical)
				throw std::runtime_error("source text contains an unissued protected symbol");
		}
	const std::regex pattern("\\[\\[\\s*" + symbols.marker_family
		+ "\\s*([0-9]+)\\s*\\]\\]", std::regex_constants::icase);
	for (std::sregex_iterator match(text.begin(), text.end(), pattern), end; match != end; ++match) {
		const std::string candidate = match->str();
		const bool issued = std::any_of(symbols.mapping.begin(), symbols.mapping.end(),
			[&](const RequestMappingEntry &entry) { return entry.placeholder == candidate; });
		if (!issued)
			throw std::runtime_error("source text contains an unissued protected symbol");
	}
}

// A letter, a mark or a digit in any script: what continues a word.
bool is_word_category(utf8proc_category_t category) {
	switch (category) {
		case UTF8PROC_CATEGORY_LU:
		case UTF8PROC_CATEGORY_LL:
		case UTF8PROC_CATEGORY_LT:
		case UTF8PROC_CATEGORY_LM:
		case UTF8PROC_CATEGORY_LO:
		case UTF8PROC_CATEGORY_MN:
		case UTF8PROC_CATEGORY_MC:
		case UTF8PROC_CATEGORY_ME:
		case UTF8PROC_CATEGORY_ND:
		case UTF8PROC_CATEGORY_NL:
		case UTF8PROC_CATEGORY_NO:
			return true;
		default:
			return false;
	}
}

// Whether the code point at `position` continues a word. Bytes that are not
// UTF-8 continue nothing, so in doubt a surname beside them is still found.
bool continues_word_at(const std::string &text, std::size_t position) {
	if (position >= text.size()) return false;
	utf8proc_int32_t codePoint = 0;
	const utf8proc_ssize_t consumed = utf8proc_iterate(
		reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + position,
		static_cast<utf8proc_ssize_t>(text.size() - position), &codePoint);
	if (consumed <= 0) return false;
	return is_word_category(utf8proc_category(codePoint));
}

// The first byte of the code point that ends right before `position`.
std::size_t code_point_before(const std::string &text, std::size_t position) {
	std::size_t before = position - 1;
	while (before > 0 && position - before < 4 && (static_cast<unsigned char>(text[before]) & 0xC0) == 0x80)
		--before;
	return before;
}

utf8proc_int32_t code_point_at(const std::string &text, std::size_t position) {
	utf8proc_int32_t codePoint = -1;
	if (position >= text.size()) return codePoint;
	const utf8proc_ssize_t consumed = utf8proc_iterate(
		reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + position,
		static_cast<utf8proc_ssize_t>(text.size() - position), &codePoint);
	return consumed <= 0 ? -1 : codePoint;
}

// Whether two words were written together with nothing between them, where the
// text still shows the seam: a number run into a word ("№ 1ДС", "ДС2"), or a
// word run into a capitalised one ("Records and AuditCorvane reply" - a
// heading glued to the name of a file). Both sides of such a seam are words of
// their own for the leak check and for the replacer alike: a name written
// there is still that name. A short capitalised stem is part of a name
// ("McHale", "MacDonald", «ДатаСфера»); only a run of four lower-case letters
// or more before the capital is a seam.
bool joined_at(const std::string &text, std::size_t position) {
	if (position == 0 || position >= text.size()) return false;
	const utf8proc_int32_t after = code_point_at(text, position);
	std::size_t before = code_point_before(text, position);
	const utf8proc_int32_t last = code_point_at(text, before);
	if (after < 0 || last < 0) return false;
	const auto category = [](utf8proc_int32_t point) { return utf8proc_category(point); };
	const auto letter = [&](utf8proc_int32_t point) {
		const auto kind = category(point);
		return kind == UTF8PROC_CATEGORY_LU || kind == UTF8PROC_CATEGORY_LL || kind == UTF8PROC_CATEGORY_LT
			|| kind == UTF8PROC_CATEGORY_LO;
	};
	const bool afterDigit = category(after) == UTF8PROC_CATEGORY_ND;
	const bool lastDigit = category(last) == UTF8PROC_CATEGORY_ND;
	if ((afterDigit && letter(last)) || (lastDigit && letter(after))) return true;
	// A word in capitals run into a capitalised one - "…Design LLCCorvane
	// reply": two capitals or more, then a capital that begins a word in lower
	// case.
	if (category(after) == UTF8PROC_CATEGORY_LU && category(last) == UTF8PROC_CATEGORY_LU) {
		utf8proc_int32_t scratch = 0;
		const utf8proc_ssize_t size = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + position,
			static_cast<utf8proc_ssize_t>(text.size() - position), &scratch);
		if (size <= 0 || category(code_point_at(text, position + static_cast<std::size_t>(size))) != UTF8PROC_CATEGORY_LL)
			return false;
		return before > 0 && category(code_point_at(text, code_point_before(text, before))) == UTF8PROC_CATEGORY_LU;
	}
	if (category(after) != UTF8PROC_CATEGORY_LU || category(last) != UTF8PROC_CATEGORY_LL) return false;
	std::size_t lower = 0;
	while (true) {
		if (category(code_point_at(text, before)) != UTF8PROC_CATEGORY_LL) break;
		++lower;
		if (before == 0 || lower >= 4) break;
		before = code_point_before(text, before);
	}
	return lower >= 4;
}

// Whether text[position, position + length) stands as a word of its own: no
// letter, mark or digit of any script right before or right after it. "Hale"
// stands so in "Hale's" and "(Hale)", not in "Haley" or "McHale" - and a name
// glued to a number or a heading still stands at the seam (joined_at).
bool stands_as_word(const std::string &text, std::size_t position, std::size_t length) {
	if (continues_word_at(text, position + length) && !joined_at(text, position + length)) return false;
	if (position == 0) return true;
	// The code point ending at `position` starts at most three bytes before its
	// last byte: step back over UTF-8 continuation bytes to its first.
	return !continues_word_at(text, code_point_before(text, position)) || joined_at(text, position);
}

// ---------------------------------------------------------------------------
// Values: one value is one symbol however it is written.

bool quantity_type(std::optional<EntityType> type) {
	return type && (*type == EntityType::MoneyAmount || *type == EntityType::Percentage || *type == EntityType::Date
		|| *type == EntityType::Number);
}

// The type a row takes for the value its writing reads as: a rate the
// checkpoint tagged as money is a rate.
std::optional<EntityType> type_of_value(const semantic::Value &value) {
	switch (value.kind) {
		case semantic::Kind::Money: return EntityType::MoneyAmount;
		case semantic::Kind::Percentage: return EntityType::Percentage;
		case semantic::Kind::Date: return EntityType::Date;
		case semantic::Kind::Number:
		case semantic::Kind::Duration: return EntityType::Number;
		case semantic::Kind::DayOfYear: return EntityType::Date;
		case semantic::Kind::Boolean: return std::nullopt;
	}
	return std::nullopt;
}

// The workbook reading's note right after a value, " (shown in ₽)" or " (shown
// as a percentage)", with the space before it; empty when there is none.
//
// Read for what the value is, never for its size. The reading writes the
// number the cell STORES (Document/XlsxExtractor), and "in thousands" or "in
// millions" is a display format's trailing commas, which only show it
// smaller: "B2 = 4800000 (shown in $ in thousands)" is $4,800,000, the same
// amount and the same symbol as "$4.8 million" - never $4.8 billion, which a
// live-shaped workbook read as a thousandfold figure in the lawyer's answer.
std::string shown_note_after(const std::string &text, std::size_t end) {
	std::size_t cursor = end;
	while (cursor < text.size() && text[cursor] == ' ') ++cursor;
	if (cursor >= text.size() || text.compare(cursor, 6, "(shown") != 0) return {};
	const std::size_t close = text.find(')', cursor);
	if (close == std::string::npos || close - cursor > 80) return {};
	std::string note = " " + text.substr(cursor, close + 1 - cursor);
	for (const char *scale : {" in thousands", " in millions", " in billions"}) {
		const std::size_t at = note.find(scale);
		if (at != std::string::npos) note.erase(at, std::char_traits<char>::length(scale));
	}
	return note;
}

bool writes_percent(const std::string &written);

// What a rate is for, as its own clause says: "18% per annum", "8% annual
// interest", "5% interest per annum", "an annual rate of 8%", «12 % годовых»,
// «0,1 % от неоплаченной суммы за каждый день просрочки». A rate masked alone
// ("18%") kept no period, and the legend told the model only "percent" (a
// verifier's run, 25 Sep 2026). One vocabulary with the algebra's own
// (semantic::rate_basis_of_clause). The period is no part of the rate's
// identity (semantic::value_key); None when the clause says nothing - and a
// rate of no known period is never counted over days or months.
semantic::RateBasis basis_after(const std::string &text, std::size_t start, std::size_t end) {
	return semantic::rate_basis_of_clause(text, start, end);
}

// What the header of a document table's column says its figures are counted
// in: the currency - RUB under «Итого, ₽», USD under "Amount ($)" - and the
// scale, 1 unless the header names one: "Revenue ($m)", "Amount ($000)",
// «Сумма, тыс. руб.». A figure "12.5" under "($m)" is $12,500,000, the same
// value as "$12.5 million" in prose; read as $12.50 once, the legend's sums and
// comparisons were a million times off (a verifier's run, 25 Sep 2026). Empty
// currency outside such a table or under a header that names none.
struct ColumnUnit {
	std::string currency;
	semantic::Rational scale = semantic::Rational(1);
};

// The scale a money column's header names: thousands ("thousands", "000",
// "$k", «тыс.»), millions ("millions", "$m", "mn", «млн») or billions
// ("billions", "$bn", «млрд»); 1 when it names none. One letter counts only
// glued to a currency sign ("$m", "€k"), never as a word of its own.
semantic::Rational scale_named(std::string_view label) {
	const semantic::Rational thousand(1000);
	const semantic::Rational million(1000000);
	const semantic::Rational billion(1000000000);
	const auto startsWith = [](const std::string &word, const char *prefix) {
		return word.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
	};
	std::size_t index = 0;
	while (index < label.size()) {
		const unsigned char byte = static_cast<unsigned char>(label[index]);
		const bool cyrillic = (byte == 0xD0 || byte == 0xD1) && index + 1 < label.size();
		if (std::isalnum(byte) == 0 && !cyrillic) {
			++index;
			continue;
		}
		const std::size_t from = index;
		while (index < label.size()) {
			const unsigned char each = static_cast<unsigned char>(label[index]);
			if (std::isalnum(each) != 0) ++index;
			else if ((each == 0xD0 || each == 0xD1) && index + 1 < label.size()) index += 2;
			else break;
		}
		std::string word(label.substr(from, index - from));
		for (char &character : word) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		const bool afterSign = (from >= 1 && label[from - 1] == '$')
			|| (from >= 2 && (label.substr(from - 2, 2) == "\xC2\xA3"))
			|| (from >= 3 && (label.substr(from - 3, 3) == "\xE2\x82\xAC" || label.substr(from - 3, 3) == "\xE2\x82\xBD"));
		if (word == "000" || word == "thousand" || word == "thousands" || word == "thous" || (afterSign && word == "k")
				|| startsWith(word, "\xD1\x82\xD1\x8B\xD1\x81") || startsWith(word, "\xD0\xA2\xD1\x8B\xD1\x81"))  // тыс, Тыс
			return thousand;
		if (word == "million" || word == "millions" || word == "mn" || word == "mm" || word == "mln" || (afterSign && word == "m")
				|| startsWith(word, "\xD0\xBC\xD0\xBB\xD0\xBD") || startsWith(word, "\xD0\x9C\xD0\xBB\xD0\xBD")  // млн, Млн
				|| startsWith(word, "\xD0\xBC\xD0\xB8\xD0\xBB\xD0\xBB\xD0\xB8\xD0\xBE\xD0\xBD"))  // миллион
			return million;
		if (word == "billion" || word == "billions" || word == "bn" || (afterSign && word == "b")
				|| startsWith(word, "\xD0\xBC\xD0\xBB\xD1\x80\xD0\xB4") || startsWith(word, "\xD0\x9C\xD0\xBB\xD1\x80\xD0\xB4")  // млрд, Млрд
				|| startsWith(word, "\xD0\xBC\xD0\xB8\xD0\xBB\xD0\xBB\xD0\xB8\xD0\xB0\xD1\x80\xD0\xB4"))  // миллиард
			return billion;
	}
	return semantic::Rational(1);
}

ColumnUnit column_unit(const std::string &text, std::size_t position) {
	const std::optional<std::pair<std::size_t, std::size_t>> header = table_column_header(text, position);
	if (!header) return {};
	const std::string_view label = std::string_view(text).substr(header->first, header->second - header->first);
	ColumnUnit unit;
	unit.currency = semantic::currency_named(label);
	if (!unit.currency.empty()) unit.scale = scale_named(label);
	return unit;
}

// A bare figure read as money in its column's currency and counted in its
// column's scale; nothing when the column names no currency.
std::optional<semantic::Value> money_in_column(const std::string &written, const ColumnUnit &unit) {
	if (unit.currency.empty()) return std::nullopt;
	std::optional<semantic::Value> money = semantic::parse_value(written, semantic::ParseHints{semantic::Kind::Money, unit.currency, false});
	if (!money || money->kind != semantic::Kind::Money) return std::nullopt;
	if (unit.scale != semantic::Rational(1)) {
		try {
			money->amount = money->amount * unit.scale;
		} catch (const semantic::AlgebraError &) {
			return std::nullopt;
		}
		if (money->amount.is_integer()) money->source_decimals = 0;
	}
	return money;
}

// Whether a word or a sign that says "percent" labels a bare figure where it
// stands: its column's header ("Rate (%)", «Ставка, %»), the cell before it in
// its row ("A5 = Rate (%) | B5 = 0.5"), the words before it on its line ("Rate,
// % per day: 0.1"), or a workbook reading's header row for its column. A "%"
// written after a figure ("3%") is that figure's, and labels nothing.
bool labelled_in_percent(const std::string &text, std::size_t start) {
	const auto names = [](std::string_view label) {
		for (std::size_t at = label.find('%'); at != std::string_view::npos; at = label.find('%', at + 1)) {
			std::size_t before = at;
			while (before > 0 && label[before - 1] == ' ') --before;
			if (before == 0 || !(label[before - 1] >= '0' && label[before - 1] <= '9')) return true;
		}
		std::string lower(label);
		for (char &character : lower) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		return lower.find("percent") != std::string::npos || lower.find("per cent") != std::string::npos
			|| lower.find("pct") != std::string::npos
			|| lower.find("\xd0\xbf\xd1\x80\xd0\xbe\xd1\x86\xd0\xb5\xd0\xbd\xd1\x82") != std::string::npos     // процент
			|| lower.find("\xd0\x9f\xd1\x80\xd0\xbe\xd1\x86\xd0\xb5\xd0\xbd\xd1\x82") != std::string::npos;    // Процент
	};
	if (const auto header = table_column_header(text, start); header && names(std::string_view(text).substr(header->first,
			header->second - header->first)))
		return true;
	const std::size_t lineStart = start == 0 ? 0 : (text.rfind('\n', start - 1) == std::string::npos ? 0 : text.rfind('\n', start - 1) + 1);
	const std::string before = text.substr(lineStart, start - lineStart);
	// The figure's own cell and the one before it.
	const auto separator = [&](std::size_t to) -> std::pair<std::size_t, std::size_t> {
		const std::size_t bar = to == 0 ? std::string::npos : before.rfind(" | ", to - 1);
		const std::size_t tab = to == 0 ? std::string::npos : before.rfind('\t', to - 1);
		if (bar != std::string::npos && (tab == std::string::npos || bar > tab)) return {bar, 3};
		if (tab != std::string::npos) return {tab, 1};
		return {std::string::npos, 0};
	};
	const auto [ownAt, ownSize] = separator(before.size());
	const std::size_t ownStart = ownAt == std::string::npos ? 0 : ownAt + ownSize;
	std::string own = before.substr(ownStart);
	if (ownAt == std::string::npos && own.size() > 48) own = own.substr(own.size() - 48);
	if (names(own)) return true;
	if (ownAt != std::string::npos) {
		const auto [previousAt, previousSize] = separator(ownAt);
		const std::size_t previousStart = previousAt == std::string::npos ? 0 : previousAt + previousSize;
		if (names(before.substr(previousStart, ownAt - previousStart))) return true;
	}
	// A workbook reading: "Row 7: ... | B7 = 0.5", its column's header in a row
	// above - "Row 1: A1 = Name | B1 = Rate (%)" - the topmost cell of words in it.
	static const std::regex cellRef("(?:^|: | \\| )\\$?([A-Z]{1,3})\\$?[0-9]{1,7} = $");
	std::smatch match;
	if (before.rfind("Row ", 0) != 0 || !std::regex_search(before, match, cellRef)) return false;
	const std::string column = match[1].str();
	const std::regex columnCell("(?:: | \\| )\\$?" + column + "\\$?[0-9]{1,7} = ([^|]*?)(?: \\||$)");
	std::string header;
	std::size_t lineEnd = lineStart;
	while (lineEnd > 0) {
		const std::size_t previousEnd = lineEnd - 1;
		const std::size_t previousStart = previousEnd == 0 ? 0
			: (text.rfind('\n', previousEnd - 1) == std::string::npos ? 0 : text.rfind('\n', previousEnd - 1) + 1);
		const std::string line = text.substr(previousStart, previousEnd - previousStart);
		if (line.rfind("Row ", 0) != 0) break;
		std::smatch cell;
		if (std::regex_search(line, cell, columnCell)) {
			const std::string value = cell[1].str();
			if (std::any_of(value.begin(), value.end(), [](char character) {
					return std::isalpha(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0xC0;
				}))
				header = value;
		}
		lineEnd = previousStart;
	}
	return !header.empty() && names(header);
}

// The words that label a figure in a workbook reading's row: "Row 12: A12 =
// Дней просрочки | C12 = 76" - the nearest cell of words before the figure's
// own cell. Empty outside such a row.
std::string workbook_row_label(const std::string &text, std::size_t start) {
	const std::size_t lineStart = start == 0 ? 0 : (text.rfind('\n', start - 1) == std::string::npos ? 0 : text.rfind('\n', start - 1) + 1);
	if (text.compare(lineStart, 4, "Row ") != 0) return {};
	std::string before = text.substr(lineStart, start - lineStart);
	const std::size_t ownBar = before.rfind(" | ");
	if (ownBar == std::string::npos) return {};
	before = before.substr(0, ownBar);
	while (!before.empty()) {
		const std::size_t bar = before.rfind(" | ");
		std::string cell = before.substr(bar == std::string::npos ? 0 : bar + 3);
		const std::size_t equals = cell.find(" = ");
		if (equals != std::string::npos) {
			const std::string value = cell.substr(equals + 3);
			if (std::any_of(value.begin(), value.end(), [](char character) {
					return std::isalpha(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0xC0;
				}))
				return value;
		}
		if (bar == std::string::npos) break;
		before = before.substr(0, bar);
	}
	return {};
}

// The value text[start, start + length) writes, read as its type says and with
// the note after it: a bare "18400000" before "(shown in ₽)" is rubles, "0.001"
// before "(shown as a percentage)" is a rate of 0.1 %. Nothing for a writing
// that is not wholly one value ("July 1" has no year) or for a name.
std::optional<semantic::Value> written_value(const std::string &text, std::size_t start, std::size_t length,
		std::optional<EntityType> type) {
	if (!quantity_type(type) || length == 0 || start + length > text.size()) return std::nullopt;
	semantic::ParseHints hints;
	if (*type == EntityType::MoneyAmount) hints.expect = semantic::Kind::Money;
	else if (*type == EntityType::Percentage) hints.expect = semantic::Kind::Percentage;
	else if (*type == EntityType::Date) hints.expect = semantic::Kind::Date;
	const std::string written = text.substr(start, length);
	const std::string note = shown_note_after(text, start + length);
	std::optional<semantic::Value> value;
	if (!note.empty()) value = semantic::parse_value(written + note, hints);
	// A rate in a cell or after its label with no percent written: "Interest
	// rate | 18" is 18 %, and so is anything of 1 or more. A figure under a label
	// or a header that says percent - "Rate (%) | 0.5", «Ставка, % в день | 0,1»
	// - is in percent points: 0.5 %, 0.1 %. A figure below 1 that nothing
	// places - "Gross margin | 0.64" - reads as 64 % or as 0.64 % alike, and is
	// given no value: an operation on it is UNKNOWN, never a rate a hundred
	// times off. A figure of a thousand or more is no rate anyone writes: "25000"
	// under "Interest" in a loan's table is the interest paid, once read as a
	// rate of 25,000 %.
	if (!value && *type == EntityType::Percentage && note.empty() && !writes_percent(written)) {
		if (const std::optional<semantic::Value> number = semantic::parse_value(written, semantic::ParseHints{semantic::Kind::Number, "", false});
				number && number->kind == semantic::Kind::Number) {
			if (number->amount >= semantic::Rational(1000) || number->amount <= semantic::Rational(-1000)) {
				hints.expect = semantic::Kind::Money;
				value = semantic::parse_value(written, hints);
			} else if (number->amount.is_zero() || number->amount >= semantic::Rational(1)
					|| number->amount <= semantic::Rational(-1) || labelled_in_percent(text, start)) {
				return semantic::make_percentage(number->amount / semantic::Rational(100),
					basis_after(text, start, start + length));
			} else {
				return std::nullopt;
			}
		}
	}
	if (!value) value = semantic::parse_value(written, hints);
	if (value && value->kind == semantic::Kind::Boolean) return std::nullopt;
	// A key-value sheet's row says what its bare figure is, before its column
	// does: «Дней просрочки | 76» under «Стоимость, ₽» is 76 days, never 76 ₽
	// (live R1, 27 Sep 2026: typed money, every day count on it failed).
	if (value && note.empty()
			&& (value->kind == semantic::Kind::Number || (value->kind == semantic::Kind::Money && value->currency.empty()))
			&& written.find_first_not_of("0123456789.,- \xC2\xA0") == std::string::npos)
		if (const std::optional<semantic::NamedMeasure> measure = semantic::measure_named(workbook_row_label(text, start))) {
			if (measure->kind == semantic::Kind::Duration) return semantic::make_duration(value->amount, measure->unit);
			semantic::Value count = *value;
			count.kind = semantic::Kind::Count;
			count.currency.clear();
			return count;
		}
	// A figure in a document's table is counted in what its column's header
	// says: "18 400 000" under «Итого, ₽» is rubles, as "(shown in ₽)" says of a cell.
	if (value && value->kind == semantic::Kind::Money && value->currency.empty()) {
		const ColumnUnit unit = column_unit(text, start);
		if (std::optional<semantic::Value> money = money_in_column(written, unit)) value = std::move(money);
		else value->currency = unit.currency;
	}
	if (value && value->kind == semantic::Kind::Percentage && value->basis == semantic::RateBasis::None)
		value->basis = basis_after(text, start, start + length);
	return value;
}

// A value's canonical amount as written in its key: "18400000" of
// "MONEY:RUB:18400000".
std::string amount_of_key(const std::string &key) {
	const std::size_t last = key.rfind(':');
	return last == std::string::npos ? std::string() : key.substr(last + 1);
}

// Whether a Money value's currency is unknown: a bare workbook number.
bool currency_unknown(const semantic::Value &value) {
	return value.kind == semantic::Kind::Money && value.currency.empty();
}

// A number's significant figures, however it is grouped or scaled: "18 400
// 000" and "18.4" are "184", "0,1" and "0.001" are "1".
std::string significant_figures(std::string_view written) {
	std::string digits;
	for (const char character : written)
		if (character >= '0' && character <= '9') digits.push_back(character);
	const std::size_t first = digits.find_first_not_of('0');
	if (first == std::string::npos) return {};
	const std::size_t last = digits.find_last_not_of('0');
	return digits.substr(first, last - first + 1);
}

// The same for a value this operation holds, from its exact decimal; empty
// when it has none (a third is never written in figures).
std::string significant_figures(const semantic::Value &value) {
	if (value.kind == semantic::Kind::Date || value.kind == semantic::Kind::DayOfYear || value.kind == semantic::Kind::Boolean)
		return {};
	try {
		const std::optional<std::string> exact = value.amount.exact_decimal(12);
		return exact ? significant_figures(*exact) : std::string();
	} catch (const semantic::AlgebraError &) {
		return {};
	}
}

// Whether a writing is a percentage by its own words: "3%", "0,1 процента",
// "5 percent", or a cell "(shown as a percentage)".
bool writes_percent(const std::string &written) {
	if (written.find('%') != std::string::npos) return true;
	std::string lowered;
	for (const char character : written)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	return lowered.find("percent") != std::string::npos || lowered.find("per cent") != std::string::npos
		|| lowered.find("\xd0\xbf\xd1\x80\xd0\xbe\xd1\x86\xd0\xb5\xd0\xbd\xd1\x82") != std::string::npos;
}

// Whether text[position] stands inside a workbook formula on its line: "{=B7/B5*1.05}".
bool inside_formula_text(const std::string &text, std::size_t position) {
	const std::size_t lineStart = position == 0 ? 0 : text.rfind('\n', position - 1);
	const std::size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
	const std::size_t open = text.rfind("{=", position);
	if (open == std::string::npos || open < from) return false;
	const std::size_t close = text.find('}', open);
	return close == std::string::npos || close > position;
}

void append_word_quantities(const std::string &text, std::vector<WrittenQuantity> &found);

// Where an amount in rubles ends when its kopecks follow it: "18 400 000
// (восемнадцать миллионов четыреста тысяч) рублей 00 копеек" is one amount, and
// masked without them it went out as "[[E1]] 00 копеек" and came back "18 400
// 000 ₽ 00 копеек" (a verifier's run, 25 Sep 2026). `end` itself otherwise.
std::size_t kopecks_after(const std::string &text, std::size_t start, std::size_t end) {
	const std::string written = text.substr(start, end - start);
	if (written.find("\xd1\x80\xd1\x83\xd0\xb1") == std::string::npos && written.find("\xe2\x82\xbd") == std::string::npos)
		return end;
	const auto gap = [&](std::size_t at) -> std::size_t {
		if (at < text.size() && text[at] == ' ') return at + 1;
		if (text.compare(at, 2, "\xC2\xA0") == 0) return at + 2;
		return std::string::npos;
	};
	std::size_t cursor = gap(end);
	if (cursor == std::string::npos) return end;
	const std::size_t digits = cursor;
	while (cursor < text.size() && cursor - digits < 3 && text[cursor] >= '0' && text[cursor] <= '9') ++cursor;
	if (cursor == digits || cursor - digits > 2 || (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9')) return end;
	cursor = gap(cursor);
	if (cursor == std::string::npos || text.compare(cursor, 6, "\xd0\xba\xd0\xbe\xd0\xbf") != 0) return end;
	const std::size_t word = cursor;
	cursor += 6;
	while (cursor < text.size() && continues_word_at(text, cursor)) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + cursor,
			static_cast<utf8proc_ssize_t>(text.size() - cursor), &codePoint);
		if (consumed <= 0) break;
		cursor += static_cast<std::size_t>(consumed);
	}
	// «коп.»: the abbreviation's own full stop.
	if (cursor == word + 6 && cursor < text.size() && text[cursor] == '.') ++cursor;
	return cursor;
}

// Every number a text writes, as the quantity it is written as. A file name's
// underscore separates as a space does - "Invoice_61300.00.pdf" writes the
// amount, where "ТЛ-14" writes a code's number - so the numbers are found in
// the text with its underscores read as spaces, the same length.
std::vector<WrittenQuantity> quantities_in(const std::string &written) {
	std::string text = written;
	std::replace(text.begin(), text.end(), '_', ' ');
	std::vector<WrittenQuantity> found;
	std::size_t position = 0;
	while (position < text.size()) {
		if (text[position] < '0' || text[position] > '9') {
			++position;
			continue;
		}
		const auto [numberStart, numberEnd] = detected_number_span(text, position, position + 1);
		position = std::max(numberEnd, position + 1);
		if (numberStart >= numberEnd || text[numberStart] < '0' || text[numberStart] > '9') continue;
		// Never a cell reference's row, a code's number or a part of a longer number.
		if (!stands_as_whole_number(text, numberStart, numberEnd - numberStart)) continue;
		WrittenQuantity quantity;
		const auto [spanStart, rublesEnd] = quantity_span(text, numberStart, numberEnd);
		const std::size_t spanEnd = kopecks_after(text, spanStart, rublesEnd);
		quantity.start = spanStart;
		quantity.length = spanEnd - spanStart;
		const std::string number = text.substr(numberStart, numberEnd - numberStart);
		quantity.figures = significant_figures(number);
		quantity.digits = static_cast<std::size_t>(std::count_if(number.begin(), number.end(),
			[](char character) { return character >= '0' && character <= '9'; }));
		const std::size_t separator = number.find_last_of(".,");
		quantity.decimals = separator != std::string::npos && number.size() - separator - 1 != 3;
		quantity.cents = separator != std::string::npos && number.size() - separator - 1 == 2;
		quantity.note = shown_note_after(text, spanEnd);
		const std::string written = text.substr(spanStart, spanEnd - spanStart);
		quantity.percent = writes_percent(written + quantity.note);
		quantity.marked = spanStart < numberStart || spanEnd > numberEnd || !quantity.note.empty();
		quantity.number_start = numberStart;
		quantity.number_end = numberEnd;
		if (!quantity.figures.empty()) found.push_back(std::move(quantity));
		position = std::max(position, spanEnd);
	}
	append_word_quantities(text, found);
	return found;
}

void read_quantity(const std::string &text, WrittenQuantity &quantity) {
	if (quantity.read) return;
	quantity.read = true;
	quantity.structure_read = true;
	if (inside_formula_text(text, quantity.number_start)) {
		quantity.structure = true;
	} else if (!quantity.marked) {
		const NumberMeaning meaning = number_meaning(text, quantity.number_start, quantity.number_end - quantity.number_start);
		quantity.structure = meaning == NumberMeaning::Generic || meaning == NumberMeaning::DatePart;
	}
	const std::string written = text.substr(quantity.start, quantity.length);
	quantity.value = semantic::parse_value(written + quantity.note);
	// A bare figure in a document table's money column is money in the header's
	// currency: the same value, and the same symbol, as the prose writing of it -
	// unless its row's label names what it counts (written_value: «Дней просрочки»).
	if (quantity.value && quantity.note.empty() && !quantity.structure
			&& (quantity.value->kind == semantic::Kind::Number || currency_unknown(*quantity.value))) {
		if (const std::optional<semantic::NamedMeasure> measure = semantic::measure_named(workbook_row_label(text, quantity.start))) {
			if (measure->kind == semantic::Kind::Duration) {
				quantity.value = semantic::make_duration(quantity.value->amount, measure->unit);
			} else {
				quantity.value->kind = semantic::Kind::Count;
				quantity.value->currency.clear();
			}
		} else if (std::optional<semantic::Value> money = money_in_column(written, column_unit(text, quantity.number_start))) {
			quantity.value = std::move(money);
		}
	}
	// A date written in figures ("10.02.2026") is read as a date, below.
	if (quantity.value && (quantity.value->kind == semantic::Kind::Date || quantity.value->kind == semantic::Kind::DayOfYear
			|| quantity.value->kind == semantic::Kind::Boolean))
		quantity.value.reset();
	if (quantity.value) quantity.key = semantic::value_key(*quantity.value);
	// One comma before exactly three figures and nothing else grouping the
	// number: "7,375" is also 7.375 (other_value above).
	const std::string number = text.substr(quantity.number_start, quantity.number_end - quantity.number_start);
	const std::size_t comma = number.find(',');
	if (quantity.value && comma != std::string::npos && number.find(',', comma + 1) == std::string::npos
			&& number.find_first_of(". ") == std::string::npos && number.find("\xC2\xA0") == std::string::npos
			&& number.size() - comma - 1 == 3) {
		std::string decimal = written;
		decimal[quantity.number_start - quantity.start + comma] = '.';
		std::optional<semantic::Value> other = semantic::parse_value(decimal + quantity.note);
		if (other && other->kind == quantity.value->kind) {
			quantity.other_key = semantic::value_key(*other);
			quantity.other_value = std::move(other);
		}
	}
}

// The row a written quantity is, by either of its readings, and the reading
// that made it so.
std::optional<std::size_t> row_of_quantity(const ValueRows &rows, const std::vector<RequestMappingEntry> &mapping,
		WrittenQuantity &quantity);

// Whether a word names a currency: "dollars", «рублей», "euro".
bool is_currency_word(const std::string &word) {
	const std::string form = normalized_entity_form(word);
	if (form.empty()) return false;
	for (const char *stem : {"dollar", "euro", "pound", "yen", "ruble", "rouble", "\xd1\x80\xd1\x83\xd0\xb1",
			"\xd0\xb4\xd0\xbe\xd0\xbb\xd0\xbb", "\xd0\xb5\xd0\xb2\xd1\x80\xd0\xbe"})
		if (form.compare(0, std::char_traits<char>::length(stem), stem) == 0) return true;
	return form == "usd" || form == "eur" || form == "gbp" || form == "rub";
}

// Whether a word says a percent: "percent", «процентов», "pct".
bool is_percent_word(const std::string &word) {
	const std::string form = normalized_entity_form(word);
	return form == "percent" || form == "percentage" || form == "pct"
		|| form.rfind("\xd0\xbf\xd1\x80\xd0\xbe\xd1\x86\xd0\xb5\xd0\xbd\xd1\x82", 0) == 0;
}

// A rate a Russian text writes in fraction words, which the words reader does
// not take: «одна десятая процента», «ноль целых пять сотых процента», «две
// тысячных процента». Nothing for any other writing.
std::optional<semantic::Value> russian_fraction_rate(const std::string &written) {
	std::vector<std::string> words;
	for (std::size_t from = 0; from < written.size();) {
		std::size_t to = written.find(' ', from);
		if (to == std::string::npos) to = written.size();
		const std::string form = normalized_entity_form(written.substr(from, to - from));
		if (!form.empty()) words.push_back(form);
		from = to + 1;
	}
	if (words.size() < 3 || !is_percent_word(words.back())) return std::nullopt;
	const std::string &fraction = words[words.size() - 2];
	const auto endsWithOneOf = [&](std::size_t stem) {
		for (const char *ending : {"\xd0\xb0\xd1\x8f", "\xd1\x8b\xd1\x85", "\xd0\xbe\xd0\xb9", "\xd1\x83\xd1\x8e",
				"\xd1\x8b\xd0\xb5"})  // ая ых ой ую ые
			if (fraction.size() == stem + std::char_traits<char>::length(ending) && fraction.compare(stem, std::string::npos, ending) == 0)
				return true;
		return false;
	};
	semantic::Int128 denominator = 0;
	for (const auto &[stem, value] : std::vector<std::pair<std::string, int>>{{"\xd0\xb4\xd0\xb5\xd1\x81\xd1\x8f\xd1\x82", 10},
			{"\xd1\x81\xd0\xbe\xd1\x82", 100}, {"\xd1\x82\xd1\x8b\xd1\x81\xd1\x8f\xd1\x87\xd0\xbd", 1000}})  // десят сот тысячн
		if (fraction.compare(0, stem.size(), stem) == 0 && endsWithOneOf(stem.size())) denominator = value;
	if (denominator == 0) return std::nullopt;
	// «ноль целых …»: the whole part, then the fraction's count.
	std::size_t countFrom = 0;
	semantic::Rational whole(0);
	for (std::size_t index = 0; index + 2 < words.size(); ++index) {
		if (words[index].compare(0, 8, "\xd1\x86\xd0\xb5\xd0\xbb\xd1\x8b") != 0 && words[index].compare(0, 8, "\xd1\x86\xd0\xb5\xd0\xbb\xd0\xb0") != 0
				&& words[index].compare(0, 8, "\xd1\x86\xd0\xb5\xd0\xbb\xd0\xbe") != 0)  // целы… цела… цело…
			continue;
		std::string wholeWords;
		for (std::size_t each = 0; each < index; ++each) wholeWords += (each > 0 ? " " : "") + words[each];
		if (wholeWords != "\xd0\xbd\xd0\xbe\xd0\xbb\xd1\x8c") {  // ноль
			const std::optional<semantic::Rational> read = semantic::parse_number_words(wholeWords);
			if (!read) return std::nullopt;
			whole = *read;
		}
		countFrom = index + 1;
		break;
	}
	std::string countWords;
	for (std::size_t each = countFrom; each + 2 < words.size(); ++each) countWords += (each > countFrom ? " " : "") + words[each];
	const std::optional<semantic::Rational> count = semantic::parse_number_words(countWords);
	if (!count || count->is_zero()) return std::nullopt;
	try {
		return semantic::make_percentage((whole + *count / semantic::Rational(denominator)) / semantic::Rational(100));
	} catch (const semantic::AlgebraError &) {
		return std::nullopt;
	}
}

// Amounts and rates written in words, which no figure gives away: "sixty-one
// thousand three hundred dollars", «восемнадцать миллионов четыреста тысяч
// рублей», "five hundred dollars", "eighteen percent", «одна десятая
// процента». Each is read whole (semantic::parse_value): a run of number words
// with a thousand, a million or a billion in it - an amount of a thousand or
// more, which words name that specifically - or one a currency or a percent
// closes. A rate spelled out beside its own figure ("18% (eighteen percent)")
// went out readable next to its symbol (a verifier's run, 25 Sep 2026). Never
// inside a quantity already found: "18 400 000 (восемнадцать миллионов …)
// рублей" is one.
void append_word_quantities(const std::string &text, std::vector<WrittenQuantity> &found) {
	struct Word {
		std::size_t start = 0;
		std::size_t end = 0;
	};
	// Most passages write no amount or rate in words at all: nothing to read.
	bool named = false;
	for (const char *stem : {"housand", "illion", "\xd1\x8b\xd1\x81\xd1\x8f\xd1\x87", "\xd0\xb8\xd0\xbb\xd0\xbb\xd0\xb8\xd0\xbe\xd0\xbd",
			"\xd0\xb8\xd0\xbb\xd0\xbb\xd0\xb8\xd0\xb0\xd1\x80\xd0\xb4", "HOUSAND", "ILLION", "ercent", "ERCENT", "er cent",
			"\xd1\x80\xd0\xbe\xd1\x86\xd0\xb5\xd0\xbd\xd1\x82", "ollar", "OLLAR", "uro", "URO", "ound", "\xd1\x80\xd1\x83\xd0\xb1",
			"\xd0\xb4\xd0\xbe\xd0\xbb\xd0\xbb", "\xd0\xb5\xd0\xb2\xd1\x80\xd0\xbe"})
		if (text.find(stem) != std::string::npos) named = true;
	if (!named) return;
	std::vector<Word> words;
	std::size_t position = 0;
	while (position < text.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + position,
			static_cast<utf8proc_ssize_t>(text.size() - position), &codePoint);
		if (consumed <= 0) {
			++position;
			continue;
		}
		const std::size_t next = position + static_cast<std::size_t>(consumed);
		const auto category = utf8proc_category(codePoint);
		const bool letter = category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL
			|| category == UTF8PROC_CATEGORY_LT || category == UTF8PROC_CATEGORY_LO;
		const bool hyphen = codePoint == '-' && !words.empty() && words.back().end == position && next < text.size()
			&& continues_word_at(text, next);
		if (letter || hyphen) {
			if (words.empty() || words.back().end != position) words.push_back({position, next});
			else words.back().end = next;
		}
		position = next;
	}
	const auto wordText = [&](std::size_t index) { return text.substr(words[index].start, words[index].end - words[index].start); };
	// Two words of one amount: nothing but spaces between them.
	const auto joined = [&](std::size_t left) {
		for (std::size_t index = words[left].end; index < words[left + 1].start; ++index)
			if (text[index] != ' ' && static_cast<unsigned char>(text[index]) != 0xC2 && static_cast<unsigned char>(text[index]) != 0xA0)
				return false;
		return words[left + 1].start > words[left].end;
	};
	const auto counts = [&](std::size_t index) {
		const std::string word = wordText(index);
		const std::string form = normalized_entity_form(word);
		// A fraction's words: «целых», «сотых» (whose stem «сот» counts nothing alone).
		return is_number_word(word) || form == "and" || form == "\xd0\xb8" || form.rfind("\xd1\x86\xd0\xb5\xd0\xbb", 0) == 0
			|| form.rfind("\xd1\x81\xd0\xbe\xd1\x82", 0) == 0;
	};
	for (std::size_t index = 0; index < words.size(); ++index) {
		if (!is_number_word(wordText(index))) continue;
		const std::size_t first = index;
		std::size_t last = index;
		while (last + 1 < words.size() && joined(last) && counts(last + 1)) ++last;
		while (last > first && !is_number_word(wordText(last))) --last;
		index = last;
		bool scale = false;
		for (std::size_t each = first; each <= last; ++each)
			if (is_scale_word(wordText(each))) scale = true;
		std::size_t end = words[last].end;
		bool currency = false;
		bool percent = false;
		if (last + 1 < words.size() && joined(last)) {
			const std::string next = wordText(last + 1);
			if (is_currency_word(next)) {
				end = words[last + 1].end;
				currency = true;
			} else if (is_percent_word(next)) {
				end = words[last + 1].end;
				percent = true;
			} else if (normalized_entity_form(next) == "per" && last + 2 < words.size() && joined(last + 1)
					&& normalized_entity_form(wordText(last + 2)) == "cent") {
				end = words[last + 2].end;
				percent = true;
			}
		}
		if (!scale && !currency && !percent) continue;
		// A word taken by mistake («стоимость» reads like «сто») is let go from
		// the left until the rest reads as one amount.
		for (std::size_t from = first; from <= last; ++from) {
			const std::size_t start = words[from].start;
			const bool overlaps = std::any_of(found.begin(), found.end(), [&](const WrittenQuantity &quantity) {
				return start < quantity.start + quantity.length && quantity.start < end;
			});
			if (overlaps) break;
			std::optional<semantic::Value> value = semantic::parse_value(text.substr(start, end - start));
			if (!value && percent) value = russian_fraction_rate(text.substr(start, end - start));
			if (!value) continue;
			const bool amount = value->kind == semantic::Kind::Money || value->kind == semantic::Kind::Number;
			const bool accepted = percent ? value->kind == semantic::Kind::Percentage
				: amount && ((currency && value->kind == semantic::Kind::Money) || value->amount >= semantic::Rational(1000));
			if (!accepted) continue;
			WrittenQuantity quantity;
			quantity.start = start;
			quantity.length = end - start;
			quantity.number_start = start;
			quantity.number_end = end;
			quantity.figures = significant_figures(*value);
			try {
				quantity.digits = value->amount.floored().to_decimal(0).size();
			} catch (const semantic::AlgebraError &) {
				continue;
			}
			quantity.marked = true;
			quantity.percent = percent;
			quantity.read = true;
			quantity.structure_read = true;
			quantity.key = semantic::value_key(*value);
			quantity.value = std::move(value);
			if (!quantity.figures.empty()) found.push_back(std::move(quantity));
			break;
		}
	}
}

// The key of a day written in figures, when it is one: "DATE:D:20260210".
std::string day_key(int year, int month, int day) {
	static const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (year < 1900 || year > 2199 || month < 1 || month > 12 || day < 1) return {};
	const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	if (day > lengths[month - 1] + (month == 2 && leap ? 1 : 0)) return {};
	char key[24];
	std::snprintf(key, sizeof key, "DATE:D:%04d%02d%02d", year, month, day);
	return key;
}

// The key of a month written in figures, when it is one: "DATE:M:202607".
std::string month_key(int year, int month) {
	if (year < 1900 || year > 2199 || month < 1 || month > 12) return {};
	char key[24];
	std::snprintf(key, sizeof key, "DATE:M:%04d%02d", year, month);
	return key;
}

// The year two figures stand for: the nearest one, never more than twenty
// years ahead ("12/31/35" is 2035, "11/14/83" 1983) - the window Word and
// Excel users' own files are read in. A date of birth is never in the future:
// «14.11.44 г.р.» is 1944. Read as 2000 and more once, a surety's «14.11.83
// г.р.» came back to the lawyer as «14 ноября 2083».
int year_of_two_figures(int figures, bool birth) {
	static const int current = [] {
		const std::time_t now = std::time(nullptr);
		std::tm parts{};
#ifdef _WIN32
		localtime_s(&parts, &now);
#else
		localtime_r(&now, &parts);
#endif
		return parts.tm_year + 1900;
	}();
	int year = 2000 + figures;
	if (year > current + 20 || (birth && year > current)) year -= 100;
	return year;
}

// Whether the words around text[start, end) say it is a date of birth:
// «14.11.83 г.р.», «года рождения», "born", "DOB", "date of birth".
bool written_as_birth_date(const std::string &text, std::size_t start, std::size_t end) {
	const auto lowered = [](std::string value) {
		for (char &character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		return value;
	};
	const std::string before = lowered(text.substr(start > 32 ? start - 32 : 0, start > 32 ? 32 : start));
	const std::string after = lowered(text.substr(end, 32));
	// г.р, рожд, родил, Рожд.
	for (const char *mark : {"\xd0\xb3.\xd1\x80", "\xd1\x80\xd0\xbe\xd0\xb6\xd0\xb4"})
		if (after.find(mark) != std::string::npos) return true;
	for (const char *mark : {"born", "birth", "dob", "\xd1\x80\xd0\xbe\xd0\xb6\xd0\xb4", "\xd1\x80\xd0\xbe\xd0\xb4\xd0\xb8\xd0\xbb",
			"\xd0\xa0\xd0\xbe\xd0\xb6\xd0\xb4"})
		if (before.find(mark) != std::string::npos) return true;
	return false;
}

// Days and months a file name, a code or a hurried hand writes in figures,
// which no one reads as prose does: "Notice_20260210.pdf", "Notice_2026_02_10",
// "2026/02/10", "notice-10-Feb-2026", "Feb-10-2026", "2/10/26", "10.02.26",
// "10-02-2026", "Notice_02-10-2026.pdf", "07/2026", "2026-07",
// "Report_2026-07.xlsx". A verifier's run (25 Sep 2026) sent every one of
// these raw beside the date's own symbol. Only ever matched against a date this
// operation holds, so a writing the two orders of day and month can both read -
// "02-10-2026" - is listed both ways and takes the date the operation has.
// Both orders are listed for every writing both can read; slashes are listed
// month first, full stops day first.
std::vector<WrittenDate> compact_dates_in(const std::string &text) {
	const auto digit = [&](std::size_t index) { return index < text.size() && text[index] >= '0' && text[index] <= '9'; };
	const auto letter = [&](std::size_t index) {
		return index < text.size() && std::isalpha(static_cast<unsigned char>(text[index])) != 0;
	};
	const auto separator = [&](std::size_t index) {
		return index < text.size() && (text[index] == '/' || text[index] == '.' || text[index] == '-' || text[index] == '_');
	};
	const auto number = [&](std::size_t from, std::size_t count) {
		int value = 0;
		for (std::size_t index = from; index < from + count; ++index) value = value * 10 + (text[index] - '0');
		return value;
	};
	const auto runEnd = [&](std::size_t from) {
		while (digit(from)) ++from;
		return from;
	};
	// A chain of figures goes on past `end`: "10.02.26.1", "2026-07-15".
	const auto continues = [&](std::size_t end) { return digit(end) || (separator(end) && digit(end + 1)); };
	// A month's name, whole or cut short after its third letter ("Mar",
	// "Sept", "March"), and nothing but its letters: "Marketing" names none.
	const auto monthAt = [&](std::size_t from, std::size_t to) -> int {
		static const char *names[] = {"january", "february", "march", "april", "may", "june", "july", "august",
			"september", "october", "november", "december"};
		if (to < from + 3 || to - from > 9) return 0;
		for (std::size_t index = from; index < to; ++index) if (!letter(index)) return 0;
		std::string month = text.substr(from, to - from);
		for (char &character : month) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		for (int index = 0; index < 12; ++index)
			if (std::string(names[index]).compare(0, month.size(), month) == 0) return index + 1;
		return 0;
	};
	// The month whose letters end right before `end`: "Mar" of "15-Mar-26".
	const auto monthBefore = [&](std::size_t end) -> int {
		std::size_t from = end;
		while (from > 0 && letter(from - 1)) --from;
		return monthAt(from, end);
	};
	std::vector<WrittenDate> dates;
	const auto add = [&](std::size_t start, std::size_t end, const std::string &key) {
		if (!key.empty()) dates.push_back({start, end - start, key});
	};
	std::size_t position = 0;
	while (position < text.size()) {
		if (!digit(position) || (position > 0 && digit(position - 1))) {
			++position;
			continue;
		}
		const std::size_t run = runEnd(position);
		const std::size_t length = run - position;
		// Part of a longer chain of figures: read from where it begins.
		const bool chained = position >= 2 && separator(position - 1) && digit(position - 2);
		if (length == 8 && !chained) {
			add(position, run, day_key(number(position, 4), number(position + 4, 2), number(position + 6, 2)));
		} else if (length == 4 && !chained && separator(run) && digit(run + 1)) {
			// The year first: "2026_02_10", "2026/02/10", "2026-07".
			const std::size_t monthEnd = runEnd(run + 1);
			if (monthEnd - run - 1 <= 2) {
				const int year = number(position, 4);
				const int month = number(run + 1, monthEnd - run - 1);
				if (text[monthEnd] == text[run] && digit(monthEnd + 1)) {
					const std::size_t dayEnd = runEnd(monthEnd + 1);
					if (dayEnd - monthEnd - 1 <= 2 && !continues(dayEnd))
						add(position, dayEnd, day_key(year, month, number(monthEnd + 1, dayEnd - monthEnd - 1)));
				} else if (!continues(monthEnd)) {
					add(position, monthEnd, month_key(year, month));
				}
			}
		} else if ((length == 4 || length == 2) && position >= 4 && separator(position - 1) && letter(position - 2)
				&& !continues(run) && !letter(run) && monthBefore(position - 1) != 0) {
			// A month by its letters beside the day: "10-Feb-2026", Excel's
			// "15-Mar-26"; or with its year alone, a month: "Mar-2026".
			const char mark = text[position - 1];
			const std::size_t before = position - 1;
			const int month = monthBefore(before);
			std::size_t monthStart = before;
			while (monthStart > 0 && letter(monthStart - 1)) --monthStart;
			std::size_t dayStart = monthStart >= 2 && text[monthStart - 1] == mark ? monthStart - 1 : monthStart;
			while (dayStart > 0 && digit(dayStart - 1) && monthStart - 1 - (dayStart - 1) <= 2) --dayStart;
			const bool day = monthStart >= 2 && dayStart < monthStart - 1 && text[monthStart - 1] == mark
				&& (dayStart == 0 || (!digit(dayStart - 1) && !letter(dayStart - 1)));
			const int year = length == 4 ? number(position, 4)
				: year_of_two_figures(number(position, 2), written_as_birth_date(text, dayStart, run));
			// "Mar-2026" stands alone: never the end of a code ("INV-Mar-2026") or a chain.
			const bool alone = monthStart == 0 || (!letter(monthStart - 1) && !digit(monthStart - 1)
				&& !(separator(monthStart - 1) && monthStart >= 2 && (letter(monthStart - 2) || digit(monthStart - 2))));
			if (day) add(dayStart, run, day_key(year, month, number(dayStart, monthStart - 1 - dayStart)));
			else if (length == 4 && alone) add(monthStart, run, month_key(year, month));
		} else if (length == 4 && position >= 6 && separator(position - 1)) {
			// A month by its letters after the day: "Feb-10-2026" (the day's
			// figures before the year are read here, not as a chain).
			const char mark = text[position - 1];
			std::size_t before = position - 1;
			if (digit(before - 1)) {
				std::size_t dayStart = before;
				while (dayStart > 0 && digit(dayStart - 1) && before - (dayStart - 1) <= 2) --dayStart;
				if (dayStart >= 2 && text[dayStart - 1] == mark && letter(dayStart - 2) && !continues(run)) {
					std::size_t monthStart = dayStart - 1;
					while (monthStart > 0 && letter(monthStart - 1)) --monthStart;
					const int month = monthAt(monthStart, dayStart - 1);
					if (month != 0 && (monthStart == 0 || !letter(monthStart - 1)))
						add(monthStart, run, day_key(number(position, 4), month, number(dayStart, before - dayStart)));
				}
			}
		} else if (length <= 2 && !chained && separator(run) && digit(run + 1)) {
			// The year last: "2/10/26", "10.02.26", "10-02-2026", "07/2026".
			const char mark = text[run];
			const std::size_t secondEnd = runEnd(run + 1);
			const std::size_t second = secondEnd - run - 1;
			const int first = number(position, length);
			// "10-2026" is no month when a month's name stands before it: "Feb-10-2026".
			std::size_t lettersStart = position >= 1 ? position - 1 : 0;
			while (lettersStart > 0 && letter(lettersStart - 1)) --lettersStart;
			const bool afterMonth = position >= 2 && separator(position - 1) && monthAt(lettersStart, position - 1) != 0;
			if (second == 4 && !continues(secondEnd) && !afterMonth) {
				add(position, secondEnd, month_key(number(run + 1, 4), first));
			} else if (second <= 2 && text[secondEnd] == mark && digit(secondEnd + 1)) {
				const std::size_t yearEnd = runEnd(secondEnd + 1);
				const std::size_t yearDigits = yearEnd - secondEnd - 1;
				if ((yearDigits == 2 || yearDigits == 4) && !continues(yearEnd)) {
					const int year = yearDigits == 2
						? year_of_two_figures(number(secondEnd + 1, 2), written_as_birth_date(text, position, yearEnd))
						: number(secondEnd + 1, 4);
					const int other = number(run + 1, second);
					const std::string monthFirst = day_key(year, first, other);
					const std::string dayFirst = day_key(year, other, first);
					// Both readings, each order first where it is the custom: a day-first
					// document writes slashes too ("05/06/2026" is 5 June in London and
					// Moscow), and a held date written so must never go out unmasked.
					const std::string &usual = mark == '.' ? dayFirst : monthFirst;
					const std::string &swapped = mark == '.' ? monthFirst : dayFirst;
					add(position, yearEnd, usual);
					if (swapped != usual) add(position, yearEnd, swapped);
				}
			}
		}
		position = run;
	}
	return dates;
}

// The key a found date is known by: its value's, or - for a slashed day both
// orders read differently in a text that does not say which it uses - a key of
// its own that names both readings ("DATE:A:20260506|20260605"). Such a date is
// protected as any date is, and each writing of it is one symbol, but it has no
// value: no order, deadline or count is worked out from a guess at its month.
std::string date_key_of(const temporal::FoundDate &found) {
	if (!found.ambiguous) return semantic::value_key(semantic::make_date(found.value));
	const std::string monthFirst = semantic::value_key(semantic::make_date(found.value));  // DATE:D:YYYYMMDD
	if (monthFirst.size() != 15) return "DATE:A:" + monthFirst;
	const std::string digits = monthFirst.substr(7);
	return "DATE:A:" + digits + "|" + digits.substr(0, 4) + digits.substr(6, 2) + digits.substr(4, 2);
}

// The order each part of a text writes its slashed days in. A prompt carries
// its documents as SOURCE blocks ("[SOURCE:DOC_2:CHUNK_1]\n...\n[/SOURCE]"):
// a block is read by its document's own slashes - every block of that document
// together - and the text outside every block by its own. Never one order for
// the whole prompt: a US invoice beside an English lease is two customs.
struct SlashScopes {
	struct Block {
		std::size_t from = 0;
		std::size_t to = 0;
		temporal::SlashOrder order = temporal::SlashOrder::Unknown;
	};
	std::vector<Block> blocks;
	temporal::SlashOrder outside = temporal::SlashOrder::Unknown;

	[[nodiscard]] temporal::SlashOrder at(std::size_t position) const {
		for (const Block &block : blocks)
			if (position >= block.from && position < block.to) return block.order;
		return outside;
	}
};

SlashScopes slash_scopes_of(const std::string &text) {
	static const std::string kMarker = "[SOURCE:DOC_";
	static const std::string kClosing = "\n[/SOURCE]";
	struct Found {
		std::size_t from = 0;
		std::size_t to = 0;
		std::string document;
	};
	std::vector<Found> found;
	for (std::size_t marker = text.find(kMarker); marker != std::string::npos; marker = text.find(kMarker, marker + 1)) {
		if (marker > 0 && text[marker - 1] != '\n') continue;
		const std::size_t lineEnd = text.find('\n', marker);
		if (lineEnd == std::string::npos || text[lineEnd - 1] != ']') continue;
		const std::size_t colon = text.find(':', marker + 8);
		if (colon == std::string::npos || colon > lineEnd) continue;
		const std::size_t closing = text.find(kClosing, lineEnd);
		if (closing == std::string::npos) continue;
		found.push_back({lineEnd + 1, closing, text.substr(marker + 8, colon - marker - 8)});
		marker = closing;
	}
	SlashScopes scopes;
	if (found.empty()) {
		scopes.outside = temporal::slash_order_of(text);
		return scopes;
	}
	std::map<std::string, std::string> byDocument;
	std::string outside;
	std::size_t cursor = 0;
	for (const Found &block : found) {
		outside += text.substr(cursor, block.from - cursor) + "\n";
		byDocument[block.document] += text.substr(block.from, block.to - block.from) + "\n";
		cursor = block.to;
	}
	outside += text.substr(cursor);
	std::map<std::string, temporal::SlashOrder> orders;
	for (const auto &[document, written] : byDocument) orders[document] = temporal::slash_order_of(written);
	for (const Found &block : found) scopes.blocks.push_back({block.from, block.to, orders[block.document]});
	scopes.outside = temporal::slash_order_of(outside);
	return scopes;
}

// Every date a text writes with its year, as temporal::find() reads it, found
// only around the years it writes: a long passage costs its years, not its
// length. A file name's underscore stands between words as a space does
// ("Notice_February 10, 2026"), and a day's ordinal letters are read as the day
// alone ("February 10th, 2026", "10th February 2026"): read without them, and
// every offset taken back to the text.
std::vector<temporal::FoundDate> dates_with_years(const std::string &text) {
	std::vector<std::pair<std::size_t, std::size_t>> windows;
	for (std::size_t position = 0; position + 4 <= text.size(); ++position) {
		if (text[position] < '1' || text[position] > '2') continue;
		if (position > 0 && text[position - 1] >= '0' && text[position - 1] <= '9') continue;
		bool year = true;
		for (std::size_t index = 1; index < 4; ++index)
			if (text[position + index] < '0' || text[position + index] > '9') year = false;
		if (!year || (position + 4 < text.size() && text[position + 4] >= '0' && text[position + 4] <= '9')) continue;
		std::size_t from = position > 48 ? position - 48 : 0;
		while (from > 0 && text[from - 1] != ' ' && text[from - 1] != '\n' && position - from < 72) --from;
		while (from < position && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80) ++from;
		std::size_t to = std::min(text.size(), position + 20);
		while (to < text.size() && text[to] != ' ' && text[to] != '\n' && to - position < 36) ++to;
		while (to < text.size() && (static_cast<unsigned char>(text[to]) & 0xC0) == 0x80) ++to;
		if (!windows.empty() && from <= windows.back().second) windows.back().second = std::max(windows.back().second, to);
		else windows.emplace_back(from, to);
		position += 3;
	}
	std::vector<temporal::FoundDate> dates;
	if (windows.empty()) return dates;
	// A slashed day is read in the order its own document shows, never the one
	// a window around its year happens to show, nor another document's: a US
	// invoice and an English lease in one prompt each write slashes their way.
	const SlashScopes scopes = slash_scopes_of(text);
	for (const auto &[from, to] : windows) {
		std::string window;
		std::vector<std::size_t> origin;
		window.reserve(to - from);
		origin.reserve(to - from + 1);
		for (std::size_t index = from; index < to; ++index) {
			if (index > from && index + 1 < to && text[index - 1] >= '0' && text[index - 1] <= '9') {
				std::string suffix = text.substr(index, 2);
				for (char &character : suffix) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
				const bool ordinal = suffix == "st" || suffix == "nd" || suffix == "rd" || suffix == "th";
				if (ordinal && (index + 2 == to || std::isalnum(static_cast<unsigned char>(text[index + 2])) == 0)) {
					++index;
					continue;
				}
			}
			window.push_back(text[index] == '_' ? ' ' : text[index]);
			origin.push_back(index);
		}
		for (temporal::FoundDate found : temporal::find(window, temporal::SlashOrder::Unknown)) {
			if (found.length == 0) continue;
			const std::size_t start = origin[found.offset];
			const std::size_t end = origin[found.offset + found.length - 1] + 1;
			if (found.ambiguous) {
				const temporal::SlashOrder order = scopes.at(start);
				if (order != temporal::SlashOrder::Unknown)
					if (const std::optional<temporal::TemporalValue> value = temporal::parse(window.substr(found.offset, found.length), order)) {
						found.value = *value;
						found.ambiguous = false;
					}
			}
			found.offset = start;
			found.length = end - start;
			dates.push_back(found);
		}
	}
	return dates;
}

// Every day of the year a text writes with no year ("July 1", «1 июля»), found
// only around the months it names with a figure beside them: most passages
// name none, and "may", "market" or "decision" name none either - an English
// month is read only as a name is written, with its capital, where a word
// begins.
std::vector<temporal::FoundDayOfYear> days_of_year_in(const std::string &text) {
	static const std::vector<std::string> months = [] {
		std::vector<std::string> stems{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		// янв фев мар апр мая июн июл авг сен окт ноя дек - each month's shortest
		// writing ("сент.", «фев»), in lower case and with a capital.
		const std::vector<std::string> russian{"\xd1\x8f\xd0\xbd\xd0\xb2", "\xd1\x84\xd0\xb5\xd0\xb2",
			"\xd0\xbc\xd0\xb0\xd1\x80", "\xd0\xb0\xd0\xbf\xd1\x80", "\xd0\xbc\xd0\xb0\xd1\x8f",
			"\xd0\xb8\xd1\x8e\xd0\xbd", "\xd0\xb8\xd1\x8e\xd0\xbb", "\xd0\xb0\xd0\xb2\xd0\xb3",
			"\xd1\x81\xd0\xb5\xd0\xbd", "\xd0\xbe\xd0\xba\xd1\x82", "\xd0\xbd\xd0\xbe\xd1\x8f",
			"\xd0\xb4\xd0\xb5\xd0\xba"};
		for (const std::string &stem : russian) {
			stems.push_back(stem);
			std::string capital = stem;
			const auto second = static_cast<unsigned char>(capital[1]);
			if (static_cast<unsigned char>(capital[0]) == 0xD0 && second >= 0xB0) capital[1] = static_cast<char>(second - 0x20);
			else if (static_cast<unsigned char>(capital[0]) == 0xD1 && second <= 0x8F) {
				capital[0] = static_cast<char>(0xD0);
				capital[1] = static_cast<char>(second + 0x20);
			}
			stems.push_back(capital);
		}
		return stems;
	}();
	const auto digitNear = [&](std::size_t from, std::size_t to) {
		for (std::size_t index = from; index < to && index < text.size(); ++index)
			if (text[index] >= '0' && text[index] <= '9') return true;
		return false;
	};
	std::vector<std::pair<std::size_t, std::size_t>> windows;
	for (const std::string &month : months) {
		for (std::size_t at = text.find(month); at != std::string::npos; at = text.find(month, at + month.size())) {
			if (at > 0) {
				std::size_t before = at - 1;
				while (before > 0 && at - before < 4 && (static_cast<unsigned char>(text[before]) & 0xC0) == 0x80) --before;
				if (continues_word_at(text, before)) continue;
			}
			// A day stands beside its month: "July 1", "1st of July", «1» июля.
			if (!digitNear(at > 10 ? at - 10 : 0, at + 14)) continue;
			std::size_t from = at > 16 ? at - 16 : 0;
			while (from > 0 && text[from - 1] != ' ' && text[from - 1] != '\n' && at - from < 28) --from;
			while (from < at && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80) ++from;
			std::size_t to = std::min(text.size(), at + 24);
			while (to < text.size() && text[to] != ' ' && text[to] != '\n' && to - at < 40) ++to;
			while (to < text.size() && (static_cast<unsigned char>(text[to]) & 0xC0) == 0x80) ++to;
			windows.emplace_back(from, to);
		}
	}
	if (windows.empty()) return {};
	std::sort(windows.begin(), windows.end());
	std::vector<std::pair<std::size_t, std::size_t>> merged;
	for (const auto &window : windows) {
		if (!merged.empty() && window.first <= merged.back().second) merged.back().second = std::max(merged.back().second, window.second);
		else merged.push_back(window);
	}
	std::vector<temporal::FoundDayOfYear> days;
	for (const auto &[from, to] : merged) {
		std::string window = text.substr(from, to - from);
		std::replace(window.begin(), window.end(), '_', ' ');
		for (temporal::FoundDayOfYear found : temporal::find_days_of_year(window)) {
			found.offset += from;
			days.push_back(found);
		}
	}
	return days;
}

// Defined with the detection of dates below.
std::optional<std::array<int, 3>> day_of_key(const std::string &key);
void read_day_ranges(const std::string &text, std::vector<WrittenDate> &found);

// Every date a text writes to the day, the month or the quarter with its
// year. A quarter is as specific as a month: restoration writes "Q3 2027" for a
// row read from «III квартал 2027 г.», and that writing sent again later must
// be the same symbol, never a raw date.
std::vector<WrittenDate> dates_in(const std::string &text) {
	// A document's number written like a date is no date (date_written_as_part).
	const auto numbered = [&](std::size_t start, std::size_t length) {
		const std::string written = text.substr(start, length);
		return written.find_first_not_of("0123456789./-") == std::string::npos && after_number_sign(text, start);
	};
	std::vector<WrittenDate> dates;
	for (const temporal::FoundDate &found : dates_with_years(text)) {
		if (found.value.precision == temporal::Precision::Year || numbered(found.offset, found.length)) continue;
		dates.push_back({found.offset, found.length, date_key_of(found)});
	}
	// A writing in figures read whole wins over a part of it prose reading took
	// for a date of its own ("10-2026" inside "02-10-2026"); one prose reading
	// already holds whole stays.
	const std::size_t prose = dates.size();
	std::vector<bool> inside(prose, false);
	for (WrittenDate &compact : compact_dates_in(text)) {
		if (numbered(compact.start, compact.length)) continue;
		const std::size_t end = compact.start + compact.length;
		const bool covered = std::any_of(dates.begin(), dates.begin() + static_cast<std::ptrdiff_t>(prose),
			[&](const WrittenDate &date) { return date.start <= compact.start && end <= date.start + date.length; });
		if (covered) continue;
		for (std::size_t index = 0; index < prose; ++index)
			if (compact.start <= dates[index].start && dates[index].start + dates[index].length <= end) inside[index] = true;
		dates.push_back(std::move(compact));
	}
	std::vector<WrittenDate> kept;
	kept.reserve(dates.size());
	for (std::size_t index = 0; index < dates.size(); ++index)
		if (index >= prose || !inside[index]) kept.push_back(std::move(dates[index]));
	// The ends of a range that write only a part of their day: "10" of «10-12
	// марта 2026 г.», "January 1" of "January 1 through March 31, 2026".
	std::vector<WrittenDate> ranged = kept;
	const std::size_t dated = ranged.size();
	for (const temporal::FoundDayOfYear &found : days_of_year_in(text))
		ranged.push_back({found.offset, found.length,
			semantic::value_key(semantic::make_day_of_year(found.value.month, found.value.day))});
	read_day_ranges(text, ranged);
	for (std::size_t index = dated; index < ranged.size(); ++index)
		if (ranged[index].key.compare(0, 5, "DATE:") == 0) kept.push_back(std::move(ranged[index]));
	return kept;
}

// Whether a date row's writing says its day by itself - "15.03.2026",
// "03.03.26", "March 3, 2026" - rather than only as a part of a range whose
// other end says the rest: a bare day ("10"), a day and a month in figures
// ("01.07"), a day of the year ("January 1", «30 июня») or a day with its
// year alone ("5, 2026"). Only a writing that says its day is looked for
// letter by letter: "10" is ten wherever else it stands, and "January 1" of
// 2026 is not January 1 of another year - a range's part is found as the
// range it is (dates_in).
bool writes_its_own_date(const RequestMappingEntry &entry, const std::string &form) {
	if (!entry.value || entry.value->kind != semantic::Kind::Date || form.empty()) return true;
	const auto digits = [&](std::size_t from, std::size_t to) {
		return to > from && to - from <= 2 && std::all_of(form.begin() + static_cast<std::ptrdiff_t>(from),
			form.begin() + static_cast<std::ptrdiff_t>(to), [](char character) { return character >= '0' && character <= '9'; });
	};
	if (digits(0, form.size())) return false;
	const std::size_t dot = form.find('.');
	if (dot != std::string::npos && digits(0, dot) && digits(dot + 1, form.size())) return false;
	const std::size_t space = form.find(' ');
	if (space != std::string::npos && form.size() - space - 1 == 4
			&& std::all_of(form.begin() + static_cast<std::ptrdiff_t>(space + 1), form.end(),
				[](char character) { return character >= '0' && character <= '9'; })
			&& (digits(0, space) || (space >= 2 && form[space - 1] == ',' && digits(0, space - 1))))
		return false;
	return !temporal::parse_day_of_year(form).has_value();
}

// The days of the year a text writes, keyed as a day of the year's value is.
std::vector<WrittenDate> written_days_of_year(const std::string &text) {
	std::vector<WrittenDate> days;
	for (const temporal::FoundDayOfYear &found : days_of_year_in(text))
		days.push_back({found.offset, found.length,
			semantic::value_key(semantic::make_day_of_year(found.value.month, found.value.day))});
	return days;
}

// Whether a date found at text[start, start + length) is written as a part of
// something that is no date: a formula ("{=DATE(…)}"), a symbol, a chain of
// figures ("1.10.02.2026"), a code ("INV-2026-03-03", "2026-03-03-001"), or the
// date of a public statute («от 27.07.2006 № 152-ФЗ»), which names the law.
// Another date beside it is no chain and no code: both ends of a range
// ("01.01.2026-31.12.2026", "2026-01-01/2026-12-31") and dates in a list
// ("15.03.2026,16.03.2026") are dates, and so is a date a file name joins to its
// words ("Minutes 03.03.2026-final.docx", "Notice-2026-03-03.pdf") - a verifier's
// run (25 Sep 2026) sent every one of these raw. `dates` are every date the
// text writes, as [start, end).
bool date_written_as_part(const std::string &text, std::size_t start, std::size_t length,
		const std::vector<std::pair<std::size_t, std::size_t>> &dates) {
	const std::size_t end = start + length;
	const auto digit = [&](std::size_t index) { return index < text.size() && text[index] >= '0' && text[index] <= '9'; };
	const auto alnum = [&](std::size_t index) {
		return index < text.size() && (std::isalnum(static_cast<unsigned char>(text[index])) != 0
			|| static_cast<unsigned char>(text[index]) >= 0x80);
	};
	const auto dateAt = [&](std::size_t index) {
		return std::any_of(dates.begin(), dates.end(), [&](const std::pair<std::size_t, std::size_t> &date) {
			return date.first <= index && index < date.second && (date.first != start || date.second != end);
		});
	};
	// A word of the language, written in letters and not a code's capitals:
	// "final", "Notice", never "INV" or "A1".
	const auto wordOfLetters = [&](std::size_t from, std::size_t to) {
		if (to < from + 3) return false;
		bool lower = false;
		for (std::size_t index = from; index < to; ++index) {
			if (std::isalpha(static_cast<unsigned char>(text[index])) == 0) return false;
			if (std::islower(static_cast<unsigned char>(text[index])) != 0) lower = true;
		}
		return lower;
	};
	const auto wordBefore = [&](std::size_t last) {
		std::size_t from = last + 1;
		while (from > 0 && std::isalpha(static_cast<unsigned char>(text[from - 1])) != 0) --from;
		return !(from > 0 && alnum(from - 1)) && wordOfLetters(from, last + 1);
	};
	const auto wordAfter = [&](std::size_t first) {
		std::size_t to = first;
		while (to < text.size() && std::isalpha(static_cast<unsigned char>(text[to])) != 0) ++to;
		return !alnum(to) && wordOfLetters(first, to);
	};
	if (inside_formula_text(text, start)) return true;
	const std::size_t opened = start >= 2 ? text.rfind("[[", start) : std::string::npos;
	if (opened != std::string::npos && start - opened < 24 && text.find("]]", opened) >= start) return true;
	if (start >= 2 && (text[start - 1] == '.' || text[start - 1] == ',' || text[start - 1] == ':') && digit(start - 2)
			&& !dateAt(start - 2))
		return true;
	if (end + 1 < text.size() && (text[end] == '.' || text[end] == ':') && digit(end + 1) && !dateAt(end + 1)) return true;
	// A code is written in figures and its own capitals: a date that names its
	// month in words ("12 марта 2026" of «10-12 марта 2026 г.») is a date
	// whatever a hyphen joins to it.
	const bool inFigures = std::none_of(text.begin() + static_cast<std::ptrdiff_t>(start),
		text.begin() + static_cast<std::ptrdiff_t>(end), [](char character) {
			return std::isalpha(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0x80;
		});
	if (inFigures && start >= 2 && (text[start - 1] == '-' || text[start - 1] == '/') && alnum(start - 2) && !dateAt(start - 2)
			&& !wordBefore(start - 2))
		return true;
	if (inFigures && end + 1 < text.size() && (text[end] == '-' || text[end] == '/') && alnum(end + 1) && !dateAt(end + 1)
			&& !wordAfter(end + 1))
		return true;
	// «Договор № 15/03/2026», "Invoice No. 2026-03-15": a document's number
	// written like a date is that number (the identifier rules protect it), and
	// never the date it looks like - restored as one, the number was lost.
	if (inFigures && after_number_sign(text, start)) return true;
	// «27.07.2006 № 152-ФЗ», «от 27 июля 2006 г. N 152-ФЗ».
	std::size_t cursor = end;
	const auto spaces = [&] {
		while (cursor < text.size() && (text[cursor] == ' ' || text.compare(cursor, 2, "\xC2\xA0") == 0))
			cursor += text[cursor] == ' ' ? 1 : 2;
	};
	spaces();
	if (text.compare(cursor, 3, "\xd0\xb3.") == 0) cursor += 3;
	else if (text.compare(cursor, 8, "\xd0\xb3\xd0\xbe\xd0\xb4\xd0\xb0") == 0) cursor += 8;
	spaces();
	if (text.compare(cursor, 3, "\xE2\x84\x96") == 0) cursor += 3;
	else if (cursor < text.size() && text[cursor] == 'N') ++cursor;
	else return false;
	spaces();
	while (digit(cursor)) ++cursor;
	return text.compare(cursor, 5, "-\xd0\xa4\xd0\x97") == 0 || text.compare(cursor, 7, "-\xd0\xa4\xd0\x9a\xd0\x97") == 0;
}

// The days a text writes in figures the way a file name, a form or a hurried
// hand does - "03.03.26", "3/3/26", "2026/03/03", "03-03-2026", "3-Mar-2026",
// "Notice_20260210.pdf" - which prose reading (temporal::find) does not take:
// every one went out raw in a verifier's run (25 Sep 2026). Only a whole day
// is a date here: "07/2026" and "2026-07" are as often a document's number
// («Договор № 07/2026») as a month. A writing the two orders of day and month
// both read, differently ("02-10-2026"), is a date all the same - its key is
// empty, and it is protected as the writing it is, never read as a guess. Two
// figures of a year after full stops need a day and a month of two figures
// each ("03.03.26"), since a clause's number ("п. 5.1.12") is written that way
// too; eight figures in a row are a day only where a name joins them to its
// words ("Notice_20260210", "scan-20260210.pdf"), never a number in prose.
std::vector<WrittenDate> compact_days_in(const std::string &text) {
	std::vector<WrittenDate> days;
	const auto digit = [&](std::size_t index) { return index < text.size() && text[index] >= '0' && text[index] <= '9'; };
	const auto joined = [&](std::size_t index) {
		return index < text.size() && (text[index] == '_' || text[index] == '-'
			|| (text[index] == '.' && index + 1 < text.size() && std::isalpha(static_cast<unsigned char>(text[index + 1])) != 0));
	};
	for (WrittenDate &found : compact_dates_in(text)) {
		const std::size_t end = found.start + found.length;
		const std::string written = text.substr(found.start, found.length);
		// A month is one here only by its name ("Mar-2026"): figures alone
		// ("07/2026") are as often a document's number.
		const bool named = std::any_of(written.begin(), written.end(),
			[](char character) { return std::isalpha(static_cast<unsigned char>(character)) != 0; });
		if (found.key.compare(0, 7, "DATE:D:") != 0 && !(named && found.key.compare(0, 7, "DATE:M:") == 0)) continue;
		const std::size_t figures = static_cast<std::size_t>(std::count_if(written.begin(), written.end(),
			[](char character) { return character >= '0' && character <= '9'; }));
		if (figures == written.size() && figures == 8) {
			const bool named = (found.start > 0 && (joined(found.start - 1)
					|| std::isalpha(static_cast<unsigned char>(text[found.start - 1])) != 0))
				|| joined(end);
			if (!named) continue;
		}
		if (written.find('.') != std::string::npos && written.find_first_not_of("0123456789.") == std::string::npos) {
			const std::size_t lastDot = written.rfind('.');
			const std::size_t firstDot = written.find('.');
			// The year first ("2026.03.03") is no clause's number.
			if (written.size() - lastDot - 1 == 2 && firstDot != 4 && (firstDot != 2 || lastDot - firstDot - 1 != 2)) continue;
		}
		if (digit(end)) continue;
		if (!days.empty() && days.back().start == found.start && days.back().length == found.length) {
			// Read both ways, as two different days: which one it is, no one says.
			if (days.back().key != found.key) days.back().key.clear();
			continue;
		}
		days.push_back(std::move(found));
	}
	return days;
}

// The year, month and day a day's key names ("DATE:D:20260312"), or the month
// and day of a day of the year's ("DOY:0312", year 0); nothing for another key.
std::optional<std::array<int, 3>> day_of_key(const std::string &key) {
	const auto figures = [&](std::size_t from, std::size_t count) {
		int value = 0;
		for (std::size_t index = from; index < from + count; ++index) {
			if (index >= key.size() || key[index] < '0' || key[index] > '9') return -1;
			value = value * 10 + (key[index] - '0');
		}
		return value;
	};
	if (key.size() == 15 && key.compare(0, 7, "DATE:D:") == 0) {
		const int year = figures(7, 4), month = figures(11, 2), day = figures(13, 2);
		if (year < 0 || month < 0 || day < 0) return std::nullopt;
		return std::array<int, 3>{year, month, day};
	}
	if (key.size() == 8 && key.compare(0, 4, "DOY:") == 0) {
		const int month = figures(4, 2), day = figures(6, 2);
		if (month < 0 || day < 0) return std::nullopt;
		return std::array<int, 3>{0, month, day};
	}
	return std::nullopt;
}

// A range of days that writes what its ends share once: «10-12 марта 2026 г.»,
// "1-3 July 2026", "March 3-5, 2026", «01.07-15.07.2026», «с 1 января по 31
// марта 2026 г.», "January 1 through March 31, 2026". Each end is the day it
// is: the start takes its month and year from the end ("10" is 10 March 2026,
// "January 1" is 1 January 2026, never a day of every year), and an end that
// writes only its day and year ("5, 2026") takes its month from the start. A
// hyphen before the end once read as a code's, and the whole range went out
// as written (a verifier's run, 25 Sep 2026). Found dates are rekeyed or added
// to `found`.
void read_day_ranges(const std::string &text, std::vector<WrittenDate> &found) {
	const auto digit = [&](std::size_t index) { return index < text.size() && text[index] >= '0' && text[index] <= '9'; };
	const auto asciiLetter = [&](std::size_t index) {
		return index < text.size() && std::isalpha(static_cast<unsigned char>(text[index])) != 0;
	};
	// A dash of a range, and where it ends: "-", "–", "—".
	const auto dashAt = [&](std::size_t index) -> std::size_t {
		if (index < text.size() && text[index] == '-') return 1;
		if (text.compare(index, 3, "\xE2\x80\x93") == 0 || text.compare(index, 3, "\xE2\x80\x94") == 0) return 3;
		return 0;
	};
	// The words that join a range's ends: "по", "до", "и", "through", "to", "and".
	const auto connectorEndingAt = [&](std::size_t end) -> std::size_t {
		static const char *words[] = {"\xd0\xbf\xd0\xbe", "\xd0\xb4\xd0\xbe", "\xd0\xb8", "through", "thru", "to", "until",
			"till", "and"};
		for (const char *word : words) {
			const std::size_t size = std::char_traits<char>::length(word);
			if (end < size + 1 || text.compare(end - size, size, word) != 0 || text[end - size - 1] != ' ') continue;
			return end - size;
		}
		return std::string::npos;
	};
	// Where the words before a range's end begin, past the dash or the word
	// that joins it to its start and the spaces around them; npos for none.
	const auto startSideEnd = [&](std::size_t endStart) -> std::size_t {
		std::size_t cursor = endStart;
		while (cursor > 0 && text[cursor - 1] == ' ') --cursor;
		const std::size_t spaced = endStart - cursor;
		std::size_t joined = std::string::npos;
		if (cursor >= 1 && text[cursor - 1] == '-') joined = cursor - 1;
		else if (cursor >= 3 && (text.compare(cursor - 3, 3, "\xE2\x80\x93") == 0 || text.compare(cursor - 3, 3, "\xE2\x80\x94") == 0))
			joined = cursor - 3;
		else if (spaced > 0) joined = connectorEndingAt(cursor);
		if (joined == std::string::npos) return std::string::npos;
		// A hyphen joins its ends with no space, or with one on each side.
		std::size_t before = joined;
		while (before > 0 && text[before - 1] == ' ') --before;
		if (text[joined] == '-' && (joined - before) != spaced) return std::string::npos;
		return before;
	};
	// A bare day of one or two figures standing on its own before `end`.
	const auto bareDayBefore = [&](std::size_t end) -> std::optional<std::pair<std::size_t, int>> {
		std::size_t start = end;
		while (start > 0 && digit(start - 1) && end - start < 3) --start;
		if (start == end || end - start > 2 || (start > 0 && digit(start - 1))) return std::nullopt;
		if (start > 0) {
			const unsigned char previous = static_cast<unsigned char>(text[start - 1]);
			const bool quoteOpen = (start >= 2 && text.compare(start - 2, 2, "\xC2\xAB") == 0)
				|| (start >= 3 && text.compare(start - 3, 3, "\xE2\x80\x9C") == 0);
			if (!quoteOpen && (previous >= 0x80 || std::isalnum(previous) != 0
					|| std::string_view(".,/:-_#$%&+=*").find(static_cast<char>(previous)) != std::string_view::npos))
				return std::nullopt;
			if (after_number_sign(text, start)) return std::nullopt;
			// A place in a document is numbered, not dated: "Section 5-7 July
			// 2026", "Items 3 and 5 March 2026", «п. 10-12 марта 2026 г.».
			std::size_t wordEnd = start;
			while (wordEnd > 0 && text[wordEnd - 1] == ' ') --wordEnd;
			if (wordEnd > 0 && text[wordEnd - 1] == '.') --wordEnd;
			std::size_t wordStart = wordEnd;
			while (wordStart > 0 && (asciiLetter(wordStart - 1) || static_cast<unsigned char>(text[wordStart - 1]) >= 0x80)) --wordStart;
			static const std::set<std::string> places{"section", "sections", "sec", "article", "articles", "art", "clause",
				"clauses", "paragraph", "paragraphs", "para", "item", "items", "schedule", "exhibit", "exhibits", "annex",
				"appendix", "part", "parts", "chapter", "chapters", "page", "pages", "p", "pp", "row", "rows", "line", "lines",
				"note", "notes", "table", "step", "steps", "rule", "rules", "no", "nos", "\xd0\xbf", "\xd0\xbf\xd0\xbf",
				"\xd1\x81\xd1\x82", "\xd1\x87", "\xd1\x81\xd1\x82\xd1\x80", "\xd0\xb3\xd0\xbb", "\xd0\xbf\xd0\xbe\xd0\xb4\xd0\xbf",
				"\xd0\xb0\xd0\xb1\xd0\xb7"};  // п пп ст ч стр гл подп абз
			// пункт, статья, раздел, глава, часть, приложение, страница, строка - in any case.
			static const char *placeStems[] = {"\xd0\xbf\xd1\x83\xd0\xbd\xd0\xba\xd1\x82", "\xd1\x81\xd1\x82\xd0\xb0\xd1\x82\xd1\x8c",
				"\xd1\x80\xd0\xb0\xd0\xb7\xd0\xb4\xd0\xb5\xd0\xbb", "\xd0\xb3\xd0\xbb\xd0\xb0\xd0\xb2",
				"\xd1\x87\xd0\xb0\xd1\x81\xd1\x82", "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xbb\xd0\xbe\xd0\xb6\xd0\xb5\xd0\xbd",
				"\xd1\x81\xd1\x82\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x86", "\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xba"};
			const std::string place = normalized_entity_form(text.substr(wordStart, wordEnd - wordStart));
			if (places.count(place) > 0) return std::nullopt;
			for (const char *stem : placeStems)
				if (place.rfind(stem, 0) == 0) return std::nullopt;
		}
		int day = 0;
		for (std::size_t index = start; index < end; ++index) day = day * 10 + (text[index] - '0');
		return std::make_pair(start, day);
	};
	// A day and a month in figures, «01.07», standing on its own before `end`.
	const auto figuresDayMonthBefore = [&](std::size_t end) -> std::optional<std::array<std::size_t, 3>> {
		std::size_t monthStart = end;
		while (monthStart > 0 && digit(monthStart - 1) && end - monthStart < 3) --monthStart;
		if (monthStart == end || end - monthStart > 2 || monthStart < 2 || text[monthStart - 1] != '.') return std::nullopt;
		const auto day = bareDayBefore(monthStart - 1);
		if (!day) return std::nullopt;
		int month = 0;
		for (std::size_t index = monthStart; index < end; ++index) month = month * 10 + (text[index] - '0');
		return std::array<std::size_t, 3>{day->first, static_cast<std::size_t>(month), static_cast<std::size_t>(day->second)};
	};
	// A day of the year a hyphen hid from reading ("March 3" of "March 3-5,
	// 2026", «30 июня» of «30 июня-2 июля 2026 г.»): its month and day, and where it begins.
	const auto dayOfYearBefore = [&](std::size_t end) -> std::optional<std::pair<std::size_t, temporal::DayOfYear>> {
		// "March 3": a word, a space, the day.
		std::size_t dayStart = end;
		while (dayStart > 0 && digit(dayStart - 1) && end - dayStart < 3) --dayStart;
		if (dayStart < end && dayStart >= 2 && text[dayStart - 1] == ' ' && asciiLetter(dayStart - 2)) {
			std::size_t from = dayStart - 1;
			if (from > 0 && text[from - 1] == '.') --from;
			while (from > 0 && asciiLetter(from - 1)) --from;
			if (text[from] >= 'A' && text[from] <= 'Z' && (from == 0 || !continues_word_at(text, from - 1)))
				if (const auto read = temporal::parse_day_of_year(text.substr(from, end - from))) return std::make_pair(from, *read);
		}
		// «30 июня»: the day, a space, the month's word.
		std::size_t wordStart = end;
		while (wordStart > 0 && (asciiLetter(wordStart - 1) || static_cast<unsigned char>(text[wordStart - 1]) >= 0x80)) --wordStart;
		if (wordStart < end && wordStart >= 2 && text[wordStart - 1] == ' ' && digit(wordStart - 2)) {
			const auto day = bareDayBefore(wordStart - 1);
			if (day)
				if (const auto read = temporal::parse_day_of_year(text.substr(day->first, end - day->first)))
					return std::make_pair(day->first, *read);
		}
		return std::nullopt;
	};
	const auto startsWithDay = [&](std::size_t at) {
		return digit(at) || (text.compare(at, 2, "\xC2\xAB") == 0 && digit(at + 2)) || (text[at] == '"' && digit(at + 1))
			|| (text.compare(at, 3, "\xE2\x80\x9C") == 0 && digit(at + 3));
	};
	const auto dotted = [&](const WrittenDate &date) {
		const std::string written = text.substr(date.start, date.length);
		return written.find('.') != std::string::npos && written.find_first_not_of("0123456789.") == std::string::npos;
	};
	std::map<std::size_t, std::size_t> byEnd;
	for (std::size_t index = 0; index < found.size(); ++index)
		if (found[index].length != 0) byEnd.emplace(found[index].start + found[index].length, index);
	const auto startKey = [&](int year, int month, int day, const std::array<int, 3> &last) {
		// "December 1 - January 31, 2027": a start after its end is the year before.
		if (month > last[1] || (month == last[1] && day > last[2])) --year;
		return day_key(year, month, day);
	};
	std::vector<WrittenDate> added;
	const std::size_t count = found.size();
	for (std::size_t index = 0; index < count; ++index) {
		const WrittenDate end = found[index];
		const std::optional<std::array<int, 3>> last = day_of_key(end.key);
		if (end.length == 0 || !last) continue;
		const std::size_t leftEnd = startSideEnd(end.start);
		if (leftEnd == std::string::npos) continue;
		const bool dayOfYear = last->at(0) == 0;
		const auto held = byEnd.find(leftEnd);
		if (held != byEnd.end()) {
			// A day of the year before a dated day is that day's year's.
			WrittenDate &first = found[held->second];
			const std::optional<std::array<int, 3>> start = day_of_key(first.key);
			if (!dayOfYear && start && start->at(0) == 0) {
				const std::string key = startKey(last->at(0), start->at(1), start->at(2), *last);
				if (!key.empty()) first.key = key;
			}
			continue;
		}
		if (const auto hidden = dayOfYearBefore(leftEnd); hidden && !dayOfYear) {
			const std::string key = startKey(last->at(0), static_cast<int>(hidden->second.month), static_cast<int>(hidden->second.day), *last);
			if (!key.empty()) added.push_back({hidden->first, leftEnd - hidden->first, key});
			continue;
		}
		if (const auto dayMonth = figuresDayMonthBefore(leftEnd); dayMonth && !dayOfYear && dotted(end)) {
			const std::string key = startKey(last->at(0), static_cast<int>(dayMonth->at(1)), static_cast<int>(dayMonth->at(2)), *last);
			if (!key.empty()) added.push_back({dayMonth->at(0), leftEnd - dayMonth->at(0), key});
			continue;
		}
		// A bare day before a day written from its day: «10-12 марта».
		if (const auto bare = bareDayBefore(leftEnd); bare && startsWithDay(end.start) && !dotted(end)
				&& bare->second >= 1 && bare->second < last->at(2)) {
			const std::string key = dayOfYear
				? semantic::value_key(semantic::make_day_of_year(static_cast<unsigned>(last->at(1)), static_cast<unsigned>(bare->second)))
				: day_key(last->at(0), last->at(1), bare->second);
			if (!key.empty()) added.push_back({bare->first, leftEnd - bare->first, key});
		}
	}
	// An end that writes only its day and its year, its month the start's:
	// "March 3-5, 2026", "March 3 – 5 2026".
	for (std::size_t index = 0; index < count; ++index) {
		WrittenDate &first = found[index];
		const std::optional<std::array<int, 3>> start = day_of_key(first.key);
		if (first.length == 0 || !start || start->at(0) != 0) continue;
		std::size_t cursor = first.start + first.length;
		const bool spaced = cursor < text.size() && text[cursor] == ' ';
		while (cursor < text.size() && text[cursor] == ' ') ++cursor;
		const std::size_t dash = dashAt(cursor);
		if (dash == 0 || (dash == 1 && spaced != (cursor + 1 < text.size() && text[cursor + 1] == ' '))) continue;
		cursor += dash;
		while (cursor < text.size() && text[cursor] == ' ') ++cursor;
		const std::size_t dayStart = cursor;
		while (digit(cursor) && cursor - dayStart < 3) ++cursor;
		if (cursor == dayStart || cursor - dayStart > 2) continue;
		int day = 0;
		for (std::size_t at = dayStart; at < cursor; ++at) day = day * 10 + (text[at] - '0');
		if (text.compare(cursor, 1, ",") == 0) ++cursor;
		if (cursor >= text.size() || text[cursor] != ' ') continue;
		while (cursor < text.size() && text[cursor] == ' ') ++cursor;
		const std::size_t yearStart = cursor;
		while (digit(cursor)) ++cursor;
		if (cursor - yearStart != 4 || continues_word_at(text, cursor)) continue;
		int year = 0;
		for (std::size_t at = yearStart; at < cursor; ++at) year = year * 10 + (text[at] - '0');
		const std::string startKeyed = day_key(year, start->at(1), start->at(2));
		const std::string endKeyed = day_key(year, start->at(1), day);
		if (startKeyed.empty() || endKeyed.empty() || day <= start->at(2)) continue;
		first.key = startKeyed;
		added.push_back({dayStart, cursor - dayStart, endKeyed});
	}
	// And the same where a hyphen hid the start from reading: "March 3-5, 2026".
	for (std::size_t position = text.find('-'); position != std::string::npos; position = text.find('-', position + 1)) {
		if (position == 0 || !digit(position - 1) || !digit(position + 1)) continue;
		const auto hidden = dayOfYearBefore(position);
		if (!hidden || byEnd.count(position) > 0) continue;
		std::size_t cursor = position + 1;
		const std::size_t dayStart = cursor;
		while (digit(cursor) && cursor - dayStart < 3) ++cursor;
		if (cursor - dayStart > 2) continue;
		int day = 0;
		for (std::size_t at = dayStart; at < cursor; ++at) day = day * 10 + (text[at] - '0');
		if (text.compare(cursor, 1, ",") == 0) ++cursor;
		if (cursor >= text.size() || text[cursor] != ' ') continue;
		++cursor;
		const std::size_t yearStart = cursor;
		while (digit(cursor)) ++cursor;
		if (cursor - yearStart != 4 || continues_word_at(text, cursor)) continue;
		int year = 0;
		for (std::size_t at = yearStart; at < cursor; ++at) year = year * 10 + (text[at] - '0');
		const std::string startKeyed = day_key(year, static_cast<int>(hidden->second.month), static_cast<int>(hidden->second.day));
		const std::string endKeyed = day_key(year, static_cast<int>(hidden->second.month), day);
		if (startKeyed.empty() || endKeyed.empty() || day <= static_cast<int>(hidden->second.day)) continue;
		const bool taken = std::any_of(found.begin(), found.end(), [&](const WrittenDate &date) {
			return date.length != 0 && date.start < cursor && hidden->first < date.start + date.length;
		});
		if (taken) continue;
		added.push_back({hidden->first, position - hidden->first, startKeyed});
		added.push_back({dayStart, cursor - dayStart, endKeyed});
	}
	for (WrittenDate &date : added) found.push_back(std::move(date));
}

// Every date a text writes with its day or its month and its year, and every
// day of the year it writes with none, as a detection of its own. Standard
// protects dates, and it cannot depend on the checkpoint seeing each one: a
// date of birth («14.11.1983 года рождения») it missed went out readable while
// nothing else looked for it. A year alone stays readable, as it always has;
// a date the patterns already took (inside an address, a code, a custom term)
// is theirs.
std::vector<CanonicalEntity> deterministic_dates(const std::string &text, const std::vector<CanonicalEntity> &taken) {
	std::vector<CanonicalEntity> dates;
	if (text.find_first_of("0123456789") == std::string::npos) return dates;
	// Known by the value it reads as, so a held date written another way is that
	// date's row: "February 10th, 2026" is the row "February 10, 2026" began.
	std::vector<WrittenDate> found;
	// A quarter is as specific as a month ("Q3 2026", «II квартала 2026»).
	for (const temporal::FoundDate &date : dates_with_years(text))
		if (date.value.precision == temporal::Precision::Day || date.value.precision == temporal::Precision::Month
				|| date.value.precision == temporal::Precision::Quarter)
			found.push_back({date.offset, date.length, date_key_of(date)});
	// A writing in figures that prose reading took only a part of, or none of.
	const std::size_t prose = found.size();
	for (WrittenDate &compact : compact_days_in(text)) {
		const std::size_t end = compact.start + compact.length;
		bool covered = false;
		for (std::size_t index = 0; index < prose; ++index) {
			const WrittenDate &date = found[index];
			if (date.start <= compact.start && end <= date.start + date.length) covered = true;
			else if (compact.start <= date.start && date.start + date.length <= end) found[index].length = 0;
		}
		if (!covered) found.push_back(std::move(compact));
	}
	for (const temporal::FoundDayOfYear &day : days_of_year_in(text))
		found.push_back({day.offset, day.length, semantic::value_key(semantic::make_day_of_year(day.value.month, day.value.day))});
	// A day written in figures without its year after a word that dates it -
	// «оплата от 02.07», "paid on 7/2" - is that day of the year: it went out
	// raw beside the masked «20.07.2026» of the same sentence (ab2 RU1, 26 Sep
	// 2026). The timeline tells the model which full date falls on it.
	for (std::size_t position = 0; position < text.size(); ++position) {
		if (text[position] < '0' || text[position] > '9' || (position > 0 && text[position - 1] >= '0' && text[position - 1] <= '9'))
			continue;
		std::size_t end = position;
		while (end < text.size() && end - position < 5
				&& ((text[end] >= '0' && text[end] <= '9') || text[end] == '.' || text[end] == '/'))
			++end;
		while (end > position && (text[end - 1] == '.' || text[end - 1] == '/')) --end;
		const std::optional<std::pair<int, int>> day = yearless_day(text, position, end - position);
		if (!day) continue;
		const bool covered = std::any_of(found.begin(), found.end(), [&](const WrittenDate &date) {
			return date.length != 0 && ranges_overlap(date.start, date.length, position, end - position);
		});
		if (!covered)
			found.push_back({position, end - position, semantic::value_key(semantic::make_day_of_year(
				static_cast<unsigned>(day->first), static_cast<unsigned>(day->second)))});
		position = end;
	}
	read_day_ranges(text, found);
	std::vector<std::pair<std::size_t, std::size_t>> spans;
	for (const WrittenDate &date : found)
		if (date.length != 0) spans.emplace_back(date.start, date.start + date.length);
	for (const WrittenDate &date : found) {
		if (date.length == 0 || date_written_as_part(text, date.start, date.length, spans)) continue;
		const auto overlapping = [&](const CanonicalEntity &entity) {
			return ranges_overlap(date.start, date.length, entity.start, entity.length);
		};
		if (std::any_of(taken.begin(), taken.end(), overlapping) || std::any_of(dates.begin(), dates.end(), overlapping))
			continue;
		CanonicalEntity entity{date.start, date.length, EntityType::Date, 1.0, DetectionSource::Deterministic, "dates",
			text.substr(date.start, date.length), {}};
		entity.value_key = date.key;
		dates.push_back(std::move(entity));
	}
	return dates;
}

WrittenText &written_text(const OperationProtectedSymbolTable &symbols, const std::string &text) {
	if (!symbols.written_values) symbols.written_values = std::make_shared<WrittenValueCache>();
	auto found = symbols.written_values->texts.find(text);
	if (found == symbols.written_values->texts.end())
		found = symbols.written_values->texts.emplace(text, WrittenText{quantities_in(text), false, {}}).first;
	return found->second;
}

ValueRows value_rows(const std::vector<RequestMappingEntry> &mapping) {
	ValueRows rows;
	for (std::size_t row = 0; row < mapping.size(); ++row) {
		const RequestMappingEntry &entry = mapping[row];
		if (!entry.value || entry.value_key.empty() || counted_by_singularity(entry)) continue;
		rows.by_key.emplace(entry.value_key, row);
		if (entry.value->kind == semantic::Kind::Date) {
			if (entry.value->date.precision != temporal::Precision::Year) rows.dates = true;
			continue;
		}
		if (entry.value->kind == semantic::Kind::DayOfYear) {
			rows.days_of_year = true;
			continue;
		}
		if (entry.value->kind == semantic::Kind::Money) rows.money_by_amount[amount_of_key(entry.value_key)].push_back(row);
		const std::string figures = significant_figures(*entry.value);
		if (!figures.empty()) {
			rows.figures.insert(figures);
			rows.quantities = true;
		}
	}
	return rows;
}

// The row a value belongs to: the row of the same value, or - one side's
// currency unknown - the one Money row of that amount. A plain number is no
// amount of money: "2,500 rentable square feet" beside a rent of $2,500 is a
// floor area, and joined to the rent it came back to the lawyer as "$2,500
// rentable square feet" (a verifier's run, 25 Sep 2026). It joins only a row
// whose own currency is not known either - a workbook's bare cell - unless it
// is written with cents, as only money is: "Invoice_61300.00.pdf".
std::optional<std::size_t> row_of_value(const ValueRows &rows, const std::vector<RequestMappingEntry> &mapping,
		const semantic::Value &value, const std::string &key, bool cents = false) {
	const auto exact = rows.by_key.find(key);
	if (exact != rows.by_key.end()) return exact->second;
	if (value.kind != semantic::Kind::Money && value.kind != semantic::Kind::Number) return std::nullopt;
	const auto amount = rows.money_by_amount.find(amount_of_key(key));
	if (amount == rows.money_by_amount.end()) return std::nullopt;
	const bool plain = value.kind == semantic::Kind::Number;
	const bool unknown = plain || currency_unknown(value);
	std::optional<std::size_t> only;
	for (const std::size_t row : amount->second) {
		if (plain && !cents && !currency_unknown(*mapping[row].value)) continue;
		if (!unknown && !currency_unknown(*mapping[row].value)) continue;
		if (only && *only != row) return std::nullopt;
		only = row;
	}
	return only;
}

std::optional<std::size_t> row_of_quantity(const ValueRows &rows, const std::vector<RequestMappingEntry> &mapping,
		WrittenQuantity &quantity) {
	if (quantity.value)
		if (const std::optional<std::size_t> row = row_of_value(rows, mapping, *quantity.value, quantity.key, quantity.cents))
			return row;
	if (quantity.other_value) return row_of_value(rows, mapping, *quantity.other_value, quantity.other_key);
	return std::nullopt;
}

// Whether a written quantity stands for `entry`'s value where it is written:
// money with its currency, four figures or cents; a rate as a percentage; a
// plain quantity of four figures or more - and a bare number never where its
// surroundings make it structure or a part of a date.
bool quantity_stands_for(const WrittenQuantity &quantity, const RequestMappingEntry &entry) {
	if (!quantity.value || !entry.value) return false;
	if (quantity.structure) return false;
	switch (entry.value->kind) {
		case semantic::Kind::Money:
			if (quantity.value->kind == semantic::Kind::Money && !currency_unknown(*quantity.value)) return true;
			return quantity.digits >= 4 || quantity.decimals;
		case semantic::Kind::Percentage:
			return quantity.percent;
		case semantic::Kind::Number:
		case semantic::Kind::Duration:
			return quantity.marked || quantity.digits >= 4;
		default:
			return false;
	}
}

// Whether a written quantity is a plain number of four figures or more - or an
// amount that does not say its currency - of an amount a Money row holds.
bool figures_of_held_amount(const ValueRows &rows, const WrittenQuantity &quantity) {
	if (!quantity.value || quantity.structure || quantity.digits < 4) return false;
	const bool plain = quantity.value->kind == semantic::Kind::Number || currency_unknown(*quantity.value);
	return plain && rows.money_by_amount.count(amount_of_key(quantity.key)) > 0;
}

// The spans of a text between ⟦ and ⟧: a protected value restored into a
// statement Singularity works out (Context/ComputedStatements), sent again later
// in history or in a correction. What is inside is protected as one operand.
struct OperandSpan {
	std::size_t start = 0;
	std::size_t end = 0;
	std::string inner;
};

const std::string kOperandOpen = "\xE2\x9F\xA6";   // ⟦
const std::string kOperandClose = "\xE2\x9F\xA7";  // ⟧

std::vector<OperandSpan> operand_spans(const std::string &text) {
	std::vector<OperandSpan> spans;
	std::size_t position = text.find(kOperandOpen);
	while (position != std::string::npos) {
		const std::size_t innerStart = position + kOperandOpen.size();
		const std::size_t close = text.find(kOperandClose, innerStart);
		if (close == std::string::npos) break;
		const std::size_t nextOpen = text.find(kOperandOpen, innerStart);
		if (nextOpen != std::string::npos && nextOpen < close) {
			position = nextOpen;
			continue;
		}
		std::string inner = text.substr(innerStart, close - innerStart);
		const std::size_t first = inner.find_first_not_of(" ");
		const std::size_t last = inner.find_last_not_of(" ");
		inner = first == std::string::npos ? std::string() : inner.substr(first, last - first + 1);
		if (!inner.empty() && inner.size() <= 400 && inner.find('\n') == std::string::npos)
			spans.push_back({position, close + kOperandClose.size(), std::move(inner)});
		position = text.find(kOperandOpen, close + kOperandClose.size());
	}
	return spans;
}

bool overlaps_operand(const std::vector<OperandSpan> &operands, std::size_t start, std::size_t length) {
	return std::any_of(operands.begin(), operands.end(), [&](const OperandSpan &operand) {
		return start < operand.end && operand.start < start + length;
	});
}

// The value an operand writes, typed: "⟦$2.8 million⟧", "⟦0,1 %⟧", "⟦July 2026⟧".
std::optional<semantic::Value> operand_value(const std::string &inner) {
	std::optional<semantic::Value> value = semantic::parse_value(inner);
	if (value && value->kind == semantic::Kind::Boolean) value.reset();
	return value;
}

// The writing and the key of an anchored writing: "$5,000,000|fact:0a1b…".
struct AnchoredWriting {
	std::string writing;
	std::string key;
};

std::optional<AnchoredWriting> anchored_writing(const std::string &inner) {
	const std::size_t at = inner.rfind(fact_anchor_marks::kKey);
	if (at == std::string::npos || at == 0) return std::nullopt;
	AnchoredWriting parts{inner.substr(0, at), inner.substr(at + fact_anchor_marks::kKey.size())};
	if (!fact_anchor_marks::is_key(parts.key)) return std::nullopt;
	return parts;
}

// Every written value of `text` that is a value `symbols` holds, as a detection
// of that value's row. While collecting, also every plain number of four
// figures or more that is the amount of a Money row without being that money -
// "18,400,000" beside "$18,400,000" and "18 400 000 рублей", "2,500 square
// feet" beside a rent of $2,500: the figures of a held amount never leave
// readable, and never under the amount's symbol either. Each is a detection of
// its own value, which becomes a row of its own.
std::vector<CanonicalEntity> known_values(const std::string &text, const OperationProtectedSymbolTable &symbols,
		const std::vector<std::pair<std::size_t, std::size_t>> &issuedMarkerRanges, bool collecting) {
	std::vector<CanonicalEntity> known;
	WrittenText &written = written_text(symbols, text);
	// Derived again only when a row's value changed: a request reads hundreds
	// of passages against the same table.
	std::size_t signature = symbols.mapping.size() * 1000003u;
	for (const RequestMappingEntry &entry : symbols.mapping) signature = signature * 31u + entry.value_key.size();
	if (symbols.written_values->rows_signature != signature) {
		symbols.written_values->rows = value_rows(symbols.mapping);
		symbols.written_values->rows_signature = signature;
	}
	const ValueRows &rows = symbols.written_values->rows;
	if (!rows.quantities && !rows.dates && !rows.days_of_year) return known;
	const std::vector<OperandSpan> operands = operand_spans(text);
	const auto excluded = [&](std::size_t start, std::size_t length) {
		const std::size_t end = start + length;
		return overlaps_operand(operands, start, length) || std::any_of(issuedMarkerRanges.begin(), issuedMarkerRanges.end(),
			[&](const auto &range) { return start < range.second && range.first < end; });
	};
	if (rows.quantities) {
		for (WrittenQuantity &quantity : written.quantities) {
			if (rows.figures.count(quantity.figures) == 0) continue;
			read_quantity(text, quantity);
			if (quantity.structure || !quantity.value) continue;
			const std::optional<std::size_t> row = row_of_quantity(rows, symbols.mapping, quantity);
			if (!row) {
				if (!collecting || !figures_of_held_amount(rows, quantity) || excluded(quantity.start, quantity.length)) continue;
				CanonicalEntity entity{quantity.start, quantity.length, type_of_value(*quantity.value), 1.0,
					DetectionSource::Deterministic, "operation_value", text.substr(quantity.start, quantity.length), {}};
				entity.value_key = quantity.key;
				known.push_back(std::move(entity));
				continue;
			}
			if (!quantity_stands_for(quantity, symbols.mapping[*row])) continue;
			if (excluded(quantity.start, quantity.length)) continue;
			const RequestMappingEntry &entry = symbols.mapping[*row];
			CanonicalEntity entity{quantity.start, quantity.length, entry.type, 1.0, DetectionSource::Deterministic,
				"operation_value", text.substr(quantity.start, quantity.length), {}};
			entity.value_key = entry.value_key;
			known.push_back(std::move(entity));
		}
	}
	if (rows.dates) {
		if (!written.dates_read) {
			written.dates = dates_in(text);
			written.dates_read = true;
		}
		for (const WrittenDate &date : written.dates) {
			const auto row = rows.by_key.find(date.key);
			if (row == rows.by_key.end() || excluded(date.start, date.length)) continue;
			const RequestMappingEntry &entry = symbols.mapping[row->second];
			CanonicalEntity entity{date.start, date.length, entry.type, 1.0, DetectionSource::Deterministic,
				"operation_value", text.substr(date.start, date.length), {}};
			entity.value_key = entry.value_key;
			known.push_back(std::move(entity));
		}
		// A held day written short, without its year and with no word to date
		// it - «оплату проводим 08.07.» beside «08.07.2026» elsewhere: two figures,
		// a full stop, two figures, whose day and month are those of exactly one
		// day this request holds, and which nothing around it makes money.
		std::map<std::string, std::vector<std::size_t>> heldByDay;
		for (const auto &[key, index] : rows.by_key)
			if (key.rfind("DATE:D:", 0) == 0 && key.size() == 15) heldByDay[key.substr(11, 4)].push_back(index);
		for (std::size_t position = 0; !heldByDay.empty() && position + 5 <= text.size(); ++position) {
			const auto digitAt = [&](std::size_t index) { return index < text.size() && text[index] >= '0' && text[index] <= '9'; };
			if (!digitAt(position) || !digitAt(position + 1) || text[position + 2] != '.' || !digitAt(position + 3)
					|| !digitAt(position + 4))
				continue;
			if (position > 0 && (digitAt(position - 1) || text[position - 1] == '.' || text[position - 1] == ','
					|| text[position - 1] == '/'))
				continue;
			const std::size_t end = position + 5;
			if (digitAt(end) || (end < text.size() && (text[end] == '/' || text[end] == '%'))
					|| ((text[end] == '.' || text[end] == ',') && digitAt(end + 1)))
				continue;
			const NumberMeaning meaning = number_meaning(text, position, 5);
			if (meaning == NumberMeaning::Money || meaning == NumberMeaning::FinancialRatio) continue;
			const std::string monthDay = text.substr(position + 3, 2) + text.substr(position, 2);
			const auto held = heldByDay.find(monthDay);
			if (held == heldByDay.end() || held->second.size() != 1 || excluded(position, 5)) continue;
			const RequestMappingEntry &entry = symbols.mapping[held->second.front()];
			if (!entry.value || entry.value->kind != semantic::Kind::Date) continue;
			CanonicalEntity entity{position, 5, entry.type, 1.0, DetectionSource::Deterministic, "operation_value",
				text.substr(position, 5), {}};
			entity.value_key = entry.value_key;
			known.push_back(std::move(entity));
			position = end;
		}
	}
	// A day of the year however it is written: "1 July" and «1 июля» are the
	// row "July 1" began.
	if (rows.days_of_year) {
		if (!written.days_read) {
			written.days = written_days_of_year(text);
			written.days_read = true;
		}
		for (const WrittenDate &day : written.days) {
			const auto row = rows.by_key.find(day.key);
			if (row == rows.by_key.end() || excluded(day.start, day.length)) continue;
			const RequestMappingEntry &entry = symbols.mapping[row->second];
			CanonicalEntity entity{day.start, day.length, entry.type, 1.0, DetectionSource::Deterministic,
				"operation_value", text.substr(day.start, day.length), {}};
			entity.value_key = entry.value_key;
			known.push_back(std::move(entity));
		}
	}
	return known;
}

// Defined with the organisation readings below.
bool written_as_short_name(const RequestMappingEntry &entry, const std::string &value);
// Whether a company's short name stands at text[position, position + length)
// as running text writes it: a whole word - and a Russian name also with the
// ending its case gives it («СтройИнвеста», «со СтройИнвестом», «Меридиана»),
// which ordinary Russian prose cannot do without. The ending stays the text's.
bool stands_as_short_name(const std::string &text, std::size_t position, std::size_t length);
// A writing in capitals, as a heading or a signature block writes a name.
std::string upper_case(const std::string &value);
bool opens_another_company_name(const std::string &text, std::size_t end);

// Defined with the person readings below: whether the name words right after
// a surname written at text[.., end) make it another person's full name.
bool different_person_follows(const std::string &text, std::size_t end, const OperationProtectedSymbolTable &symbols,
		std::size_t row);

// Whether a place where a spelling with letters stands in another case - or
// with a non-breaking or narrow space between its words - still names what the
// spelling names. `folded` is the text through platform::same_length_lowered.
// A spelling of two words or more does, however it is cased: "MEADOWBROOK
// INDUSTRIAL PARTNERS LLC" in a signature block is the company the agreement
// names in title case, and it once went out to the model readable. A single
// word does where it is not written all in lower case ("HALE" signing is the
// surname, "hale" in a sentence is a word) and is not one word of a heading in
// capitals ("PURCHASE PRICE") - unless the spelling itself is lower case.
bool names_in_another_case(const std::string &text, const std::string &folded, std::size_t position, std::size_t length,
		const std::string &form) {
	if (length == form.size() && text.compare(position, length, form) == 0) return true;
	if (form.find(' ') != std::string::npos) return true;
	const std::string written = text.substr(position, length);
	if (platform::same_length_lowered(form) == form) return true;
	if (folded.compare(position, length, written) == 0) return false;
	const auto capitalWord = [&](std::size_t from, std::size_t to) {
		const std::string word = text.substr(from, to - from);
		std::size_t letters = 0;
		for (const char character : word)
			if (std::isalpha(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0x80) ++letters;
		return letters >= 3 && folded.compare(from, to - from, word) != 0
			&& std::none_of(word.begin(), word.end(), [](char character) { return character >= 'a' && character <= 'z'; });
	};
	std::size_t before = position;
	while (before > 0 && text[before - 1] == ' ') --before;
	std::size_t beforeStart = before;
	while (beforeStart > 0 && text[beforeStart - 1] != ' ' && text[beforeStart - 1] != '\n') --beforeStart;
	std::size_t after = position + length;
	while (after < text.size() && text[after] == ' ') ++after;
	std::size_t afterEnd = after;
	while (afterEnd < text.size() && text[afterEnd] != ' ' && text[afterEnd] != '\n') ++afterEnd;
	const bool allCaps = std::none_of(written.begin(), written.end(), [](char character) { return character >= 'a' && character <= 'z'; });
	if (allCaps && ((before > beforeStart && capitalWord(beforeStart, before)) || (afterEnd > after && capitalWord(after, afterEnd))))
		return false;
	return true;
}

// The spelling as the folded text holds it, and with each document space
// between its words: what a case-blind search looks for.
std::vector<std::string> folded_needles(const std::string &form) {
	std::vector<std::string> needles{platform::same_length_lowered(form)};
	if (form.find(' ') != std::string::npos)
		for (const char *space : {"\xC2\xA0", "\xE2\x80\xAF", "\xE2\x80\x89"}) {
			std::string variant;
			for (const char character : needles.front()) {
				if (character == ' ') variant += space;
				else variant += character;
			}
			needles.push_back(variant);
		}
	return needles;
}

std::vector<CanonicalEntity> known_operation_entities(const std::string &text,
		const OperationProtectedSymbolTable &symbols, bool collecting = false) {
	std::vector<std::pair<std::size_t, std::size_t>> issuedMarkerRanges;
	for (const auto &entry : symbols.mapping) {
		std::size_t markerPosition = 0;
		while ((markerPosition = text.find(entry.placeholder, markerPosition)) != std::string::npos) {
			issuedMarkerRanges.emplace_back(markerPosition, markerPosition + entry.placeholder.size());
			markerPosition += entry.placeholder.size();
		}
	}
	// Every spelling this operation knows, found in one pass (PatternScanner):
	// a request carries hundreds of them, and searching the text once per
	// spelling cost the text's length multiplied by their number.
	//
	// The repeated search moved past a match before looking again, so a
	// spelling never matched inside its own previous match; the same rule is
	// kept here per spelling, and overlaps between DIFFERENT spellings are
	// reported exactly as they were - reconcile() settles them.
	// What the spellings are, right now. Changing any of them rebuilds the
	// automaton; reading passage after passage does not.
	std::size_t signature = symbols.mapping.size() * 1000003u;
	for (const auto &entry : symbols.mapping)
		signature = signature * 31u + entry.surface_forms.size() * 7u + entry.surname_forms.size() * 13u
			+ entry.short_forms.size() * 17u + entry.original.size();
	const auto isSurname = [](const RequestMappingEntry &entry, const std::string &form) {
		return std::find(entry.surname_forms.begin(), entry.surname_forms.end(), form) != entry.surname_forms.end();
	};
	if (symbols.spelling_signature != signature || !symbols.identity_scanner) {
		auto scanner = std::make_shared<PatternScanner>();
		symbols.identity_scanner_forms.clear();
		for (std::size_t row = 0; row < symbols.mapping.size(); ++row) {
			const RequestMappingEntry &entry = symbols.mapping[row];
			if (counted_by_singularity(entry)) continue;
			// Each spelling, case-blind and with a document's spaces between its
			// words (folded_needles): the text is scanned folded, every offset
			// where it stands in the text.
			const auto addForm = [&](const std::string &form) {
				symbols.identity_scanner_forms.emplace_back(row, form);
				const std::size_t tag = symbols.identity_scanner_forms.size() - 1;
				for (const std::string &needle : has_letter(form) ? folded_needles(form) : std::vector<std::string>{form})
					scanner->add(needle, tag);
			};
			for (const std::string &form : entry.every_form()) {
				// A spelling that is also a surname alone is looked for below, as a word.
				if (form.empty() || isSurname(entry, form) || !writes_its_own_date(entry, form)) continue;
				addForm(form);
			}
			for (const std::string &form : entry.surname_forms) {
				if (form.empty()) continue;
				addForm(form);
			}
			for (const std::string &form : entry.short_forms) {
				if (form.empty()) continue;
				addForm(form);
			}
		}
		scanner->build();
		symbols.identity_scanner = scanner;
		symbols.spelling_signature = signature;
	}
	std::vector<std::size_t> nextAllowed(symbols.identity_scanner_forms.size(), 0);
	// One pass finds the occurrences; they are then put back in the order the
	// repeated search produced them - spelling by spelling, in table order,
	// each spelling's occurrences left to right. reconcile() settles overlaps
	// by precedence and, among equals, by the order it is given them, so this
	// order is part of the answer and not a detail of how it was found.
	std::vector<std::pair<std::size_t, CanonicalEntity>> found;
	const std::string foldedText = platform::same_length_lowered(text);
	symbols.identity_scanner->scan(foldedText, [&](std::size_t position, std::size_t length, std::size_t tag) {
		if (position < nextAllowed[tag]) return;
		const RequestMappingEntry &entry = symbols.mapping[symbols.identity_scanner_forms[tag].first];
		// A surname alone is that person only where it stands as a word: "Hale"
		// inside "Haley" is not found, and does not move the search past itself.
		const std::string &form = symbols.identity_scanner_forms[tag].second;
		// In another case, it names the same only as names_in_another_case says.
		if (has_letter(form) && !names_in_another_case(text, foldedText, position, length, form)) return;
		const bool surname = isSurname(entry, form);
		if (surname && !stands_as_word(text, position, length)) return;
		// An organisation's short name alone is that organisation only as a whole
		// word: "Meadowbrook" is not inside "Meadowbrooke".
		const bool shortName = written_as_short_name(entry, form);
		if (shortName && (!stands_as_short_name(text, position, length) || opens_another_company_name(text, position + length))) return;
		// «Кузнецова Анна Павловна» is not Дмитрий Кузнецов in the genitive: a
		// surname followed by a first name, a patronymic or initials that are not
		// this person's belongs to that other full name.
		if (surname && different_person_follows(text, position + length, symbols, symbols.identity_scanner_forms[tag].first))
			return;
		// A number is that number only where it stands whole, and where the words
		// around it do not make it structure: "2026" is not the start of "20260",
		// an amount of 12 is not "Row 12" (number_stands_for). A rejected
		// occurrence moves nothing either. A person's custom term is the number
		// wherever it stands whole.
		const bool letters = has_letter(form);
		const bool custom = entry.source == DetectionSource::CustomTerm;
		if (!letters && !(custom ? stands_as_whole_number(text, position, length)
				: number_stands_for(text, position, length, form, entry.type)))
			return;
		if (!custom && !yearless_writing_stands(entry, form, text, position, length)) return;
		if (letters && !custom && stands_in_public_phrase(text, position, length)) return;
		// Any other spelling with letters begins a word, and only an inflection
		// ending may follow it: "иска" is never inside "Переписка". A person's
		// custom term is found wherever it stands, as it always was.
		if (letters && !surname && !shortName && entry.source != DetectionSource::CustomTerm
				&& !stands_as_word_or_stem(text, position, length,
					entry.type == EntityType::Person || entry.type == EntityType::Judge))
			return;
		nextAllowed[tag] = position + length;
		const std::size_t end = position + length;
		const bool insideIssuedMarker = std::any_of(issuedMarkerRanges.begin(), issuedMarkerRanges.end(),
			[&](const auto &range) { return position >= range.first && end <= range.second; });
		// The writing as it stands is what is replaced, and what the row keeps as
		// one more spelling of the same identity.
		if (!insideIssuedMarker) found.emplace_back(tag, CanonicalEntity{position, length,
			entry.type, 1.0, entry.source, "operation_identity", text.substr(position, length), {}});
	});
	std::stable_sort(found.begin(), found.end(), [](const auto &left, const auto &right) {
		if (left.first != right.first) return left.first < right.first;
		return left.second.start < right.second.start;
	});
	std::vector<CanonicalEntity> known;
	known.reserve(found.size());
	for (auto &[tag, entity] : found) known.push_back(std::move(entity));
	// And every other writing of a value this operation holds: the same amount,
	// rate or date written another way - "18 400 000 рублей" for a workbook's
	// "18400000 (shown in ₽)", a restored "$61,300.00" sent back in history.
	std::vector<CanonicalEntity> values = known_values(text, symbols, issuedMarkerRanges, collecting);
	known.insert(known.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
	return known;
}

// A value this operation holds is that value wherever the text writes it, and
// a bare number or a code a detector read inside that writing is not another
// identity: "18400000 (shown in $)" is the price whatever else the figures
// look like. An identifier's shape once took it, and the price cell lost its
// identity with the prose price while sharing a symbol with a share count (a
// verifier's run, 25 Sep 2026). Names are never let go this way.
void let_values_take_their_writings(std::vector<CanonicalEntity> &entities) {
	std::vector<std::pair<std::size_t, std::size_t>> values;
	for (const CanonicalEntity &entity : entities)
		if (entity.source_label == "operation_value" && !entity.value_key.empty())
			values.emplace_back(entity.start, entity.start + entity.length);
	if (values.empty()) return;
	entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const CanonicalEntity &entity) {
		if (entity.source_label == "operation_value" || entity.source == DetectionSource::CustomTerm) return false;
		const bool code = !has_letter(entity.value) || entity.type == EntityType::ContractIdentifier
			|| entity.type == EntityType::CaseNumber;
		return code && std::any_of(values.begin(), values.end(), [&](const auto &value) {
			return value.first <= entity.start && entity.start + entity.length <= value.second;
		});
	}), entities.end());
}

// An alias the Project recorded before names were trimmed, or one a document
// gave with the sentence's next word inside it: "Meridia Clinical Systems
// support". Registry data outlives a rule, so the same judgment is applied
// where the alias is used, not only where it was learnt. The name is still
// masked; only the ordinary word after it stops being part of the name - and
// only where a capitalised word ends the name, exactly as for a detection. A
// person's alias is never passed here.
std::string alias_without_trailing_ordinary_word(const std::string &alias) {
	const std::size_t length = length_without_trailing_ordinary_word(alias, 0, alias.size());
	if (length == alias.size() || !ends_with_capitalised_word(alias, 0, length)) return alias;
	// A saved name whose first word is a generic word or a legal concept is
	// kept whole: "Project falcon" cut to "Project" would never be found again.
	const std::string remainder = alias.substr(0, length);
	if (is_generic_role_term(EntityType::Organisation, remainder)
			|| is_legal_concept(remainder, ValueOrigin::RegistryAlias)) return alias;
	return remainder;
}

std::vector<CanonicalEntity> known_project_entities(const std::string &text,
		const OperationProtectedSymbolTable &symbols) {
	std::vector<std::pair<std::size_t, std::size_t>> issuedMarkerRanges;
	for (const auto &entry : symbols.mapping) {
		std::size_t markerPosition = 0;
		while ((markerPosition = text.find(entry.placeholder, markerPosition)) != std::string::npos) {
			issuedMarkerRanges.emplace_back(markerPosition, markerPosition + entry.placeholder.size());
			markerPosition += entry.placeholder.size();
		}
	}
	// A name the Project knows is that name however it is typed: "holt" in a
	// hurried question and "HOLT" in a signature block are the person the
	// documents call "Holt". Case is ignored for plain-Latin names only (byte
	// for byte, so offsets stay exact); the value recorded is the text as it
	// stands, which is what must be replaced.
	const auto lowered = [](const std::string &value) {
		std::string result = value;
		for (char &character : result)
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		return result;
	};
	const std::string loweredText = lowered(text);
	// The Project's own names, found in one pass each way (PatternScanner):
	// exactly as written for a name carrying non-Latin bytes, and case-folded
	// for a plain-Latin one, which is the rule this scan has always applied.
	std::size_t aliasSignature = symbols.project_entity_aliases.size() * 1000003u;
	for (const auto &[entityId, aliases] : symbols.project_entity_aliases) {
		aliasSignature = aliasSignature * 31u + entityId.size() + aliases.size() * 7u;
		for (const std::string &alias : aliases) aliasSignature = aliasSignature * 131u + alias.size();
	}
	for (const auto &[entityId, curated] : symbols.project_entity_curated_aliases)
		aliasSignature = aliasSignature * 37u + entityId.size() + curated.size() * 11u;
	if (symbols.alias_scanner_signature != aliasSignature || !symbols.alias_scanner_exact) {
		auto exactScanner = std::make_shared<PatternScanner>();
		auto foldedScanner = std::make_shared<PatternScanner>();
		symbols.alias_scanner_patterns.clear();
		for (const auto &[entityId, aliases] : symbols.project_entity_aliases) {
			const auto type = symbols.project_entity_types.find(entityId);
			if (type == symbols.project_entity_types.end()) continue;
			const bool personAlias = type->second == EntityType::Person || type->second == EntityType::Judge;
			const auto curatedAliases = symbols.project_entity_curated_aliases.find(entityId);
			for (const std::string &recorded : aliases) {
				// What the lawyer confirmed is masked as written, whatever the
				// vocabulary below would say of it.
				const bool curated = curatedAliases != symbols.project_entity_curated_aliases.end()
					&& curatedAliases->second.count(recorded) > 0;
				std::string alias = personAlias ? recorded : alias_without_trailing_ordinary_word(recorded);
				// An alias recorded across a line or a cell, as a detection once was
				// ("ТЛ-14/2026\n\tг. Москва"), is its first line; an address keeps its lines.
				if (type->second != EntityType::Address) {
					alias = alias.substr(0, alias.find_first_of("\t\n\r"));
					while (!alias.empty() && alias.back() == ' ') alias.pop_back();
				}
				if (alias.empty()) continue;
			// A role is not an identity here either. These aliases are not a
			// person's instruction: automatic preprocessing wrote them from what
			// the PII checkpoint tagged in the documents, and it tags "Покупатель"
			// and "Buyer" as people. A term the USER asked for is a different
			// thing entirely and is never touched - it arrives through
			// Policy::custom_terms and deterministic_entities(), not from here.
				// Everything a fresh detection would be judged by, so a word the
				// registry recorded by mistake stops being masked once the Guard
				// knows better, without rewriting the registry: public vocabulary,
				// prose the documents wrote in lower case ("data room", "license"),
				// a role or an office, a title made of ordinary words, and one
				// ordinary word that is no registered company's short name ("State",
				// "Medical"). A thirty-document US data room (11 Oct 2026) had each
				// of these masked in every request, and "State" inside "United States".
				// An ordinary word alone stays: a client may be called "State".
				if (!curated && !recorded_value_names_identity(type->second, alias)) continue;
				// Nor a place in a document or a code, whatever an earlier detection
				// recorded: "Section 9A", "LEGAL_ENTITY".
				const bool nameAlias = personAlias || type->second == EntityType::Organisation
					|| type->second == EntityType::LegalEntity || type->second == EntityType::Court
					|| type->second == EntityType::Location;
				if (!curated && (names_only_structure(alias, 0, alias.size()) || written_as_code(alias, true)
						|| (nameAlias && written_as_code(alias, false))))
					continue;
				// Nor a phrase of Russian prose an earlier detection recorded
				// («персональными данными»). Judged by its shape alone: this scanner
				// serves every text of the operation. One lowercase word the registry
				// recorded is an identity someone wrote so, and stays.
				const bool organisationAlias = type->second == EntityType::Organisation
					|| type->second == EntityType::LegalEntity;
				if (!curated && (personAlias || organisationAlias) && alias.find(' ') != std::string::npos
						&& written_in_lowercase_cyrillic(std::string(), alias, std::string::npos, type->second))
					continue;
				const bool plainLatin = std::none_of(alias.begin(), alias.end(),
					[](unsigned char character) { return character >= 0x80; });
				symbols.alias_scanner_patterns.push_back({alias, plainLatin ? lowered(alias) : alias, type->second, curated});
				(plainLatin ? *foldedScanner : *exactScanner).add(
					symbols.alias_scanner_patterns.back().needle, symbols.alias_scanner_patterns.size() - 1);
			}
		}
		exactScanner->build();
		foldedScanner->build();
		symbols.alias_scanner_exact = exactScanner;
		symbols.alias_scanner_folded = foldedScanner;
		symbols.alias_scanner_signature = aliasSignature;
	}
	std::vector<std::size_t> nextAllowed(symbols.alias_scanner_patterns.size(), 0);
	// Alias by alias, in registry order, exactly as the repeated search gave
	// them to reconcile() (see known_operation_entities() above).
	std::vector<std::pair<std::size_t, CanonicalEntity>> found;
	{
		const auto report = [&](std::size_t position, std::size_t length, std::size_t tag) {
			if (position < nextAllowed[tag]) return;
			const auto &pattern = symbols.alias_scanner_patterns[tag];
			const std::string &alias = pattern.alias;
			const std::string &needle = pattern.needle;
			const std::size_t end = position + length;
			// Written exactly as recorded, the name is found wherever it stands,
			// as it always was. In any other case it must be a word of its own:
			// "CA" is not the "ca" inside "recapture", and restoring that symbol
			// would rewrite the word as "reCApture".
			const bool exactCase = text.compare(position, needle.size(), alias) == 0;
			const auto isWordCharacter = [&](std::size_t index) {
				const auto character = static_cast<unsigned char>(text[index]);
				return std::isalnum(character) != 0 || character >= 0x80;
			};
			const bool wholeWord = (position == 0 || !isWordCharacter(position - 1))
				&& (end >= text.size() || !isWordCharacter(end));
			// A rejected occurrence never moved the search past itself before, and
			// does not now: the next occurrence of this name may start one byte on.
			if (!exactCase && !wholeWord) return;
			// A number is found only as the whole number it is, where the words
			// around it do not make it structure - "0" recorded as an alias once
			// masked every zero of a request, and an amount of 12 is not "Row 12"
			// (number_stands_for) - and a name written exactly as recorded only
			// where it begins a word, with at most an inflection ending after it
			// ("Транслогистик" in "Транслогистики").
			const bool letters = has_letter(alias);
			if (!letters && !number_stands_for(text, position, needle.size(), alias, pattern.type)) return;
			if (letters && exactCase && !stands_as_word_or_stem(text, position, needle.size(),
					pattern.type == EntityType::Person || pattern.type == EntityType::Judge))
				return;
			// Inside a public phrase the word is that phrase ("the State of
			// Delaware"), unless the lawyer confirmed it as written.
			if (letters && !pattern.curated && stands_in_public_phrase(text, position, needle.size())) return;
			nextAllowed[tag] = end;
			const bool insideIssuedMarker = std::any_of(issuedMarkerRanges.begin(), issuedMarkerRanges.end(),
				[&](const auto &range) { return position >= range.first && end <= range.second; });
			if (!insideIssuedMarker) found.emplace_back(tag, CanonicalEntity{position, needle.size(),
				pattern.type, 1.0, DetectionSource::Deterministic, "project_entity_registry",
				text.substr(position, needle.size()), {}});
		};
		symbols.alias_scanner_exact->scan(text, report);
		symbols.alias_scanner_folded->scan(loweredText, report);
	}
	std::stable_sort(found.begin(), found.end(), [](const auto &left, const auto &right) {
		if (left.first != right.first) return left.first < right.first;
		return left.second.start < right.second.start;
	});
	std::vector<CanonicalEntity> known;
	known.reserve(found.size());
	for (auto &[tag, entity] : found) known.push_back(std::move(entity));
	return known;
}

std::string normalized_entity_form(const std::string &value) {
	utf8proc_uint8_t *mapped = nullptr;
	const utf8proc_ssize_t mappedLength = utf8proc_map(
		reinterpret_cast<const utf8proc_uint8_t *>(value.data()),
		static_cast<utf8proc_ssize_t>(value.size()), &mapped,
		static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE
			| UTF8PROC_COMPAT | UTF8PROC_CASEFOLD));
	if (mappedLength < 0 || mapped == nullptr) {
		if (mapped != nullptr) std::free(mapped);
		return {};
	}
	std::string normalized;
	std::size_t offset = 0;
	const auto length = static_cast<std::size_t>(mappedLength);
	while (offset < length) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(mapped + offset,
			static_cast<utf8proc_ssize_t>(length - offset), &codePoint);
		if (consumed <= 0) break;
		const auto category = utf8proc_category(codePoint);
		const bool identityCharacter = category == UTF8PROC_CATEGORY_LU
			|| category == UTF8PROC_CATEGORY_LL || category == UTF8PROC_CATEGORY_LT
			|| category == UTF8PROC_CATEGORY_LM || category == UTF8PROC_CATEGORY_LO
			|| category == UTF8PROC_CATEGORY_ND || category == UTF8PROC_CATEGORY_NL
			|| category == UTF8PROC_CATEGORY_NO;
		if (identityCharacter)
			normalized.append(reinterpret_cast<const char *>(mapped + offset),
				static_cast<std::size_t>(consumed));
		offset += static_cast<std::size_t>(consumed);
	}
	std::free(mapped);
	return normalized;
}

std::string conservative_word_form_stem(std::string value) {
	static const std::vector<std::string> endings{
		"иями", "ями", "ами", "ого", "ему", "ому", "ыми", "ими",
		"ах", "ях", "ой", "ей", "ом", "ем", "ов", "ев", "а", "я", "ы", "и", "е", "у", "ю",
	};
	for (const std::string &ending : endings) {
		if (value.size() > ending.size() + 7
				&& value.compare(value.size() - ending.size(), ending.size(), ending) == 0) {
			value.erase(value.size() - ending.size());
			break;
		}
	}
	return value;
}

struct ProjectEntityResolution {
	std::optional<std::string> entity_id;
	bool ambiguous = false;
};

// The form two values are compared in - case, punctuation and a conservative
// word ending aside - computed once per distinct value in an operation.
const std::string &comparison_form(const OperationProtectedSymbolTable &symbols, const std::string &value) {
	const auto found = symbols.comparison_form_cache.find(value);
	if (found != symbols.comparison_form_cache.end()) return found->second;
	return symbols.comparison_form_cache
		.emplace(value, conservative_word_form_stem(normalized_entity_form(value))).first->second;
}

// Every Project entity by the comparison form of each of its aliases.
const std::map<std::string, std::vector<std::string>> &alias_index(const OperationProtectedSymbolTable &symbols) {
	std::size_t signature = symbols.project_entity_aliases.size();
	for (const auto &entry : symbols.project_entity_aliases) signature = signature * 31 + entry.second.size();
	if (symbols.alias_index_signature == signature) return symbols.alias_entities_by_form;
	symbols.alias_entities_by_form.clear();
	for (const auto &entry : symbols.project_entity_aliases) {
		for (const std::string &alias : entry.second) {
			auto &entities = symbols.alias_entities_by_form[comparison_form(symbols, alias)];
			if (std::find(entities.begin(), entities.end(), entry.first) == entities.end()) entities.push_back(entry.first);
		}
	}
	symbols.alias_index_signature = signature;
	return symbols.alias_entities_by_form;
}

ProjectEntityResolution project_entity_for_value(const OperationProtectedSymbolTable &symbols,
		const std::string &value) {
	const auto &index = alias_index(symbols);
	const auto found = index.find(comparison_form(symbols, value));
	if (found == index.end() || found->second.empty()) return {std::nullopt, false};
	if (found->second.size() > 1) return {std::nullopt, true};
	return {found->second.front(), false};
}

// Whether a writing has a Cyrillic letter.
bool has_cyrillic_letter(const std::string &value) {
	for (std::size_t index = 0; index + 1 < value.size(); ++index) {
		const auto lead = static_cast<unsigned char>(value[index]);
		if (lead == 0xD0 || lead == 0xD1) return true;
	}
	return false;
}

bool stands_as_short_name(const std::string &text, std::size_t position, std::size_t length) {
	if (stands_as_word(text, position, length)) return true;
	return has_cyrillic_letter(text.substr(position, length)) && stands_as_word_or_stem(text, position, length, false);
}

std::string upper_case(const std::string &value) {
	std::string result;
	result.reserve(value.size());
	std::size_t position = 0;
	while (position < value.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + position,
			static_cast<utf8proc_ssize_t>(value.size() - position), &codePoint);
		if (consumed <= 0) {
			result.push_back(value[position++]);
			continue;
		}
		utf8proc_uint8_t buffer[4];
		const utf8proc_ssize_t written = utf8proc_encode_char(utf8proc_toupper(codePoint), buffer);
		result.append(reinterpret_cast<const char *>(buffer), static_cast<std::size_t>(written));
		position += static_cast<std::size_t>(consumed);
	}
	return result;
}

// A Russian full name compared in the nominative: «Лебедева Андрея Игоревича»,
// «Лебедеву Андрею Игоревичу» and «Лебедев Андрей Игоревич» are one person.
// Empty for a name of one word, a name in another script, or one Singularity
// cannot read as a person (Context/RussianNames leaves it as written, and the
// form it is compared in is then its own).
const std::string &person_nominative_form(const OperationProtectedSymbolTable &symbols, const std::string &value) {
	static const std::string none;
	if (!has_cyrillic_letter(value) || value.find(' ') == std::string::npos) return none;
	const std::string cacheKey = "\x01" + value;
	const auto found = symbols.comparison_form_cache.find(cacheKey);
	if (found != symbols.comparison_form_cache.end()) return found->second;
	return symbols.comparison_form_cache
		.emplace(cacheKey, normalized_entity_form(russian_names::declined(value, russian_names::Case::Nominative)))
		.first->second;
}

bool person_type(std::optional<EntityType> type) {
	return type && (*type == EntityType::Person || *type == EntityType::Judge);
}

// Which row of an operation's table a value belongs to. The rules, the
// lowest matching row winning:
//   0. the same value (semantic::value_key): an amount, a rate or a date is
//      one row however it is written; a Money value whose currency is not
//      written joins the one row of that amount;
//   1. the same Project Registry entity;
//   2. the same text as the row's first spelling;
//   3. the same type and the same comparison form, of at least eight
//      characters - and never when the Registry says the value could name two
//      different entities: exact text stays safe then, but morphology must not
//      pick either identity, and a new symbol is safer than a false merge;
//   4. a Russian person's full name in the nominative, the same.
// Answered from lookups rather than by asking every row about every value: a
// data-room request holds a thousand rows and four thousand values, and that
// scan was most of what protecting it cost.
bool organisation_type(std::optional<EntityType> type);
std::string name_without_legal_form(const std::string &name);

// A company's name with its legal form set aside, in the form two names are
// compared in, and that legal form itself: "Northwind Freight Systems LLC" and
// "Northwind Freight Systems" compare as one name, the first with "llc".
std::pair<std::string, std::string> organisation_name_and_form(const OperationProtectedSymbolTable &symbols,
		const std::string &value) {
	const std::string base = name_without_legal_form(value);
	if (base.empty()) return {comparison_form(symbols, value), std::string()};
	return {comparison_form(symbols, base), normalized_entity_form(value.substr(base.size()))};
}

// Whether two writings of one type are one identity when their letters and
// figures agree: a name of eight bytes or more; a code with figures, of three
// bytes or more - "ФП-044/П" in a contract and "ФП-044-П" in its file's name are one
// code, written as each place lets it.
bool comparable_form(EntityType type, const std::string &form) {
	const bool code = type == EntityType::ContractIdentifier || type == EntityType::CaseNumber
		|| type == EntityType::TaxId || type == EntityType::BankAccount;
	if (code && form.size() >= 3 && contains_digit(form)) return true;
	return form.size() >= 8;
}

class MappingIndex final {
public:
	MappingIndex(const OperationProtectedSymbolTable &symbols) : symbols_(symbols) {
		for (std::size_t index = 0; index < symbols.mapping.size(); ++index) add(index);
	}

	void add(std::size_t index) {
		const RequestMappingEntry &entry = symbols_.mapping[index];
		if (entry.project_entity_id) by_project_entity_.emplace(*entry.project_entity_id, index);
		// "10" of «10-12 марта» is that day only in its range: another "10" is
		// found by its own value.
		if (writes_its_own_date(entry, entry.original)) by_original_.emplace(entry.original, index);
		if (entry.type) {
			const std::string &form = comparison_form(symbols_, entry.original);
			if (comparable_form(*entry.type, form))
				by_type_and_form_.emplace(std::make_pair(static_cast<int>(*entry.type), form), index);
		}
		if (organisation_type(entry.type)) {
			const std::string name = organisation_name_and_form(symbols_, entry.original).first;
			if (name.size() >= 8) by_organisation_name_[name].push_back(index);
		}
		if (person_type(entry.type))
			for (const std::string &writing : entry.every_form()) {
				const std::string &nominative = person_nominative_form(symbols_, writing);
				if (nominative.size() >= 8) by_person_nominative_.emplace(nominative, index);
			}
	}

	// The row a name or a code is: a person or an organisation keeps its
	// real-world identity, a code its writing. An amount, a rate, a count, a
	// date or a period is never found here by its value - it is a fact of the
	// material, found where it stands (attribute_fact): two facts of one value
	// are two rows.
	[[nodiscard]] std::optional<std::size_t> row_of(const CanonicalEntity &candidate,
			const ProjectEntityResolution &candidateProjectEntity) const {
		std::optional<std::size_t> row;
		const auto consider = [&](std::size_t index) { if (!row || index < *row) row = index; };
		if (candidateProjectEntity.entity_id) {
			const auto found = by_project_entity_.find(*candidateProjectEntity.entity_id);
			if (found != by_project_entity_.end()) consider(found->second);
		}
		const auto exact = by_original_.find(candidate.value);
		if (exact != by_original_.end()) consider(exact->second);
		if (!candidateProjectEntity.ambiguous && candidate.type) {
			const std::string &form = comparison_form(symbols_, candidate.value);
			if (comparable_form(*candidate.type, form)) {
				const auto found = by_type_and_form_.find(std::make_pair(static_cast<int>(*candidate.type), form));
				if (found != by_type_and_form_.end()) consider(found->second);
			}
		}
		// A company with and without its legal form is one company: "Northwind
		// Freight Systems" in the board's minutes is the "Northwind Freight
		// Systems LLC" of the agreement. Two different legal forms of one name
		// ("Acme Holdings LLC", "Acme Holdings Inc.") may be two companies and
		// stay two.
		if (!candidateProjectEntity.ambiguous && organisation_type(candidate.type)) {
			const auto [name, form] = organisation_name_and_form(symbols_, candidate.value);
			const auto found = name.size() >= 8 ? by_organisation_name_.find(name) : by_organisation_name_.end();
			if (found != by_organisation_name_.end())
				for (const std::size_t index : found->second) {
					const std::string rowForm = organisation_name_and_form(symbols_, symbols_.mapping[index].original).second;
					if (!form.empty() && !rowForm.empty() && form != rowForm) continue;
					consider(index);
				}
		}
		// A case form is weighed only where no writing already names a row: one
		// Russian writing can be two people's - «Кузнецова А. П.» is Андрей
		// Петрович's genitive and Анна Павловна herself - and a row that writing
		// already has keeps it, never the row another case form of it reached.
		if (row) return row;
		if (!candidateProjectEntity.ambiguous && person_type(candidate.type)) {
			const std::string &nominative = person_nominative_form(symbols_, candidate.value);
			if (nominative.size() >= 8) {
				const auto found = by_person_nominative_.find(nominative);
				if (found != by_person_nominative_.end() && person_type(symbols_.mapping[found->second].type))
					consider(found->second);
			}
		}
		return row;
	}

private:
	const OperationProtectedSymbolTable &symbols_;
	// emplace keeps the first (lowest) row for a key, which is the row a scan
	// of the table from its start would have found.
	std::map<std::string, std::size_t> by_project_entity_;
	std::map<std::string, std::size_t> by_original_;
	std::map<std::pair<int, std::string>, std::size_t> by_type_and_form_;
	std::map<std::string, std::vector<std::size_t>> by_organisation_name_;
	std::map<std::string, std::size_t> by_person_nominative_;
};

GuardValidation validate_masking(const std::string &protected_text,
                                 const std::vector<RequestMappingEntry> &mapping) {
  GuardValidation validation;
	// Remove only issued symbols from the visible scan. Unknown lookalikes stay
	// visible and can never hide an original protected value. Read as the text
	// reads, JSON escapes included (reading_view).
	std::string visible = reading_view(protected_text);
	for (const auto &entry : mapping) {
		std::size_t position = 0;
		while ((position = visible.find(entry.placeholder, position)) != std::string::npos) {
			visible.replace(position, entry.placeholder.size(), entry.placeholder.size(), '\0');
			position += entry.placeholder.size();
		}
	}
  // The same text in lower case, letter for letter at the same offsets: a name
  // is found however it is cased, in any script - "VICTOR HALE" in a signature
  // block, «КУЗНЕЦОВА» in a header - not only as it was first written.
  const std::string foldedVisible = platform::same_length_lowered(visible);
  // A place where a spelling with letters stands in another case, or with a
  // non-breaking or narrow space between its words, still names what the
  // spelling names when the spelling is two words or more, however it is
  // cased; a single word only where it is not written all in lower case
  // ("HALE" in a signature is the surname, "hale" in a sentence is a word),
  // and not as one word of a heading in capitals ("PURCHASE PRICE") - unless
  // the spelling itself is lower case.
  const auto variantNames = [&](const std::string &form, std::size_t position, std::size_t length) {
    return names_in_another_case(visible, foldedVisible, position, length, form);
  };
  // Every place a spelling stands: in any case, and with any of the spaces a
  // document puts between words.
  const auto placesOf = [&](const std::string &form, const std::function<void(std::size_t, std::size_t)> &each) {
    for (const std::string &needle : folded_needles(form))
      for (std::size_t position = foldedVisible.find(needle); position != std::string::npos;
           position = foldedVisible.find(needle, position + 1))
        each(position, needle.size());
  };
  std::set<std::string> leaks;
  // Every place a value still stands, with the row it is a writing of: what a
  // repair masks with that row's own symbol (LeakOccurrence). A place no row
  // can be named for is recorded with the row count as its row.
  std::set<std::pair<std::size_t, std::size_t>> placed;
  const auto occur = [&](std::size_t row, const std::string &form, std::size_t start, std::size_t length,
      LeakOccurrence::Kind kind) {
    leaks.insert(form);
    if (!placed.insert({start, length}).second) return;
    validation.occurrences.push_back(LeakOccurrence{row, form, start, length, kind});
  };
  for (std::size_t row = 0; row < mapping.size(); ++row) {
    const RequestMappingEntry &entry = mapping[row];
    // A count or a period Singularity worked out itself is its own number, not a
    // fact of the lawyer's material: "9" or "54" standing elsewhere is not that
    // row. Its writing with its unit or its noun is (DESIGN4 §4.2): a restored
    // "42 days", «42 дня» or "3 invoices" sent again is that derived fact, and
    // is found here and masked - never sent raw on review or a later turn.
    if (counted_by_singularity(entry)) {
      for (const std::string &form : derived_writings(entry))
        for (std::size_t position = visible.find(form); position != std::string::npos;
             position = visible.find(form, position + 1)) {
          const bool digitBefore = position > 0 && ((visible[position - 1] >= '0' && visible[position - 1] <= '9')
              || visible[position - 1] == '.' || visible[position - 1] == ',');
          if (digitBefore || !stands_as_word_or_stem(visible, position, form.size(), false)) continue;
          occur(row, form, position, form.size(), LeakOccurrence::Kind::Writing);
        }
      continue;
    }
    // An identifier's own figures - "0618", a six-figure account's number -
    // are that identifier wherever they stand whole, with or without the "№"
    // that first marked them (ab2 RU1: «по 0618» re-sent raw in a review).
    const bool identifier = entry.type == EntityType::ContractIdentifier || entry.type == EntityType::CaseNumber
        || entry.type == EntityType::TaxId;
    const auto identifierFigures = [](const std::string &form) {
      return !form.empty() && std::all_of(form.begin(), form.end(), [](char c) { return c >= '0' && c <= '9'; })
          && ((form[0] == '0' && form.size() >= 3) || form.size() >= 6);
    };
    const auto isSurname = [&](const std::string &form) {
      return std::find(entry.surname_forms.begin(), entry.surname_forms.end(), form) != entry.surname_forms.end();
    };
    for (const std::string &form : entry.every_form()) {
      if (form.empty() || isSurname(form) || !writes_its_own_date(entry, form)) continue;
      // Looked for exactly as it is replaced: a number where it stands whole
      // and its own context does not make it structure (number_stands_for), a
      // spelling with letters where it begins a word, a custom term wherever it
      // stands.
      const bool letters = has_letter(form);
      const bool custom = entry.source == DetectionSource::CustomTerm;
      // A spelling with letters, as it stands, in any case and spacing.
      if (letters) {
        placesOf(form, [&](std::size_t position, std::size_t length) {
          if (!variantNames(form, position, length)) return;
          // Judged as protection judged it, so what was rightly left is never a
          // leak: the same word boundaries, a plural only for a person, the same
          // public phrases.
          if (!custom && !stands_as_word_or_stem(visible, position, length,
                  entry.type == EntityType::Person || entry.type == EntityType::Judge))
            return;
          if (!custom && stands_in_public_phrase(visible, position, length)) return;
          occur(row, visible.substr(position, length), position, length, LeakOccurrence::Kind::Writing);
        });
        continue;
      }
      for (std::size_t position = visible.find(form); position != std::string::npos;
           position = visible.find(form, position + 1)) {
        const bool stands = letters ? (custom || (stands_as_word_or_stem(visible, position, form.size(),
                entry.type == EntityType::Person || entry.type == EntityType::Judge)
            && !stands_in_public_phrase(visible, position, form.size())))
            : custom ? stands_as_whole_number(visible, position, form.size())
            : (identifier && identifierFigures(form)) ? stands_as_whole_number(visible, position, form.size())
            : number_stands_for(visible, position, form.size(), form, entry.type)
                && yearless_writing_stands(entry, form, visible, position, form.size());
        if (!stands) continue;
        occur(row, form, position, form.size(), LeakOccurrence::Kind::Writing);
      }
    }
    // A surname alone, or an organisation's short name, is looked for exactly
    // as it was replaced: as a word.
    std::vector<std::string> wordForms = entry.surname_forms;
    wordForms.insert(wordForms.end(), entry.short_forms.begin(), entry.short_forms.end());
    for (std::size_t item = 0; item < wordForms.size(); ++item) {
      const std::string &form = wordForms[item];
      if (form.empty()) continue;
      const bool shortName = item >= entry.surname_forms.size();
      placesOf(form, [&](std::size_t position, std::size_t length) {
        if (!variantNames(form, position, length)) return;
        if (!(shortName ? stands_as_short_name(visible, position, length) : stands_as_word(visible, position, length)))
          return;
        if (shortName && opens_another_company_name(visible, position + length)) return;
        if (stands_in_public_phrase(visible, position, length)) return;
        occur(row, visible.substr(position, length), position, length, LeakOccurrence::Kind::SurnameOrShort);
      });
    }
  }
  // A statement's operand never leaves as written: protection turns every
  // ⟦…⟧ into its symbol, so a mark still here is a value that was not.
  for (const std::string &mark : {kOperandOpen, kOperandClose})
    for (std::size_t position = visible.find(mark); position != std::string::npos; position = visible.find(mark, position + 1))
      occur(mapping.size(), kOperandOpen, position, mark.size(), LeakOccurrence::Kind::OperandMark);
  // Nor does an earlier answer's fact anchor (⟪writing|fact:key⟫): protection
  // replaces the whole mark with its fact's symbol, so a mark still here - or
  // what is left of one - was not. Never repaired; always refused.
  for (const std::string_view mark : {fact_anchor_marks::kOpen, fact_anchor_marks::kClose, fact_anchor_marks::kKey})
    for (std::size_t position = visible.find(mark); position != std::string::npos; position = visible.find(mark, position + 1))
      occur(mapping.size(), std::string(mark), position, mark.size(), LeakOccurrence::Kind::AnchorMark);
  // Any other writing of a value this request holds that says what it is - an
  // amount with its currency, a rate with its percent, a date: "$61,300.00"
  // for a workbook's 61300 is that amount, and it was replaced where it stood.
  // And a plain number of four figures or more that writes a held amount's
  // figures: "18,400,000" beside "$18,400,000" is collected as a row of its
  // own, so one still here was never masked.
  const ValueRows rows = value_rows(mapping);
  if (rows.quantities) {
    for (WrittenQuantity &quantity : quantities_in(visible)) {
      if (rows.figures.count(quantity.figures) == 0 || (!quantity.marked && quantity.digits < 4)) continue;
      read_quantity(visible, quantity);
      if (quantity.structure || !quantity.value) continue;
      const std::optional<std::size_t> row = row_of_quantity(rows, mapping, quantity);
      if (row ? quantity_stands_for(quantity, mapping[*row]) : figures_of_held_amount(rows, quantity)) {
        std::size_t named = row ? *row : mapping.size();
        if (!row) {
          const auto amount = rows.money_by_amount.find(amount_of_key(quantity.key));
          if (amount != rows.money_by_amount.end() && amount->second.size() == 1) named = amount->second.front();
        }
        occur(named, visible.substr(quantity.start, quantity.length), quantity.start, quantity.length,
            LeakOccurrence::Kind::Quantity);
      }
    }
  }
  if (rows.dates)
    for (const WrittenDate &date : dates_in(visible)) {
      std::vector<std::string> keys{date.key};
      // A slashed day this text leaves open ("DATE:A:20260506|20260605") is
      // raw here as either of its readings: a date held as 5 June must not
      // leave as "05/06/2026" because this text does not say its order.
      if (date.key.compare(0, 7, "DATE:A:") == 0 && date.key.size() == 24) {
        keys.push_back("DATE:D:" + date.key.substr(7, 8));
        keys.push_back("DATE:D:" + date.key.substr(16, 8));
      }
      for (const std::string &key : keys) {
        const auto row = rows.by_key.find(key);
        if (row == rows.by_key.end()) continue;
        occur(row->second, visible.substr(date.start, date.length), date.start, date.length, LeakOccurrence::Kind::Date);
        break;
      }
    }
  if (rows.days_of_year)
    for (const WrittenDate &day : written_days_of_year(visible)) {
      const auto row = rows.by_key.find(day.key);
      if (row != rows.by_key.end())
        occur(row->second, visible.substr(day.start, day.length), day.start, day.length, LeakOccurrence::Kind::DayOfYear);
    }
  validation.leaked_values.assign(leaks.begin(), leaks.end());
  validation.masking_complete = validation.leaked_values.empty();
  std::sort(validation.occurrences.begin(), validation.occurrences.end(),
      [](const LeakOccurrence &left, const LeakOccurrence &right) {
        if (left.start != right.start) return left.start < right.start;
        return left.length > right.length;
      });
  return validation;
}

// Defined with the restoration spellings below: whether a writing carries a
// capital letter at all.
bool has_ascii_upper(const std::string &value);

// The code points of a word; empty when it is not UTF-8.
std::vector<utf8proc_int32_t> code_points(const std::string &word) {
	std::vector<utf8proc_int32_t> points;
	std::size_t offset = 0;
	while (offset < word.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(word.data()) + offset,
			static_cast<utf8proc_ssize_t>(word.size() - offset), &codePoint);
		if (consumed <= 0) return {};
		points.push_back(codePoint);
		offset += static_cast<std::size_t>(consumed);
	}
	return points;
}

bool is_capital(utf8proc_int32_t codePoint) {
	const auto category = utf8proc_category(codePoint);
	return category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LT;
}

bool is_letter(utf8proc_int32_t codePoint) {
	const auto category = utf8proc_category(codePoint);
	return category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL || category == UTF8PROC_CATEGORY_LT
		|| category == UTF8PROC_CATEGORY_LM || category == UTF8PROC_CATEGORY_LO;
}

// A word written as a name: a capital, then letters with at least one small
// one, an apostrophe or a hyphen only between letters - "Hale", "McDonald",
// "O'Neil", "Римский-Корсаков". Neither "HALE" nor "hale" is.
bool is_name_word(const std::string &word) {
	const std::vector<utf8proc_int32_t> points = code_points(word);
	if (points.size() < 2 || !is_capital(points.front())) return false;
	bool small = false;
	for (std::size_t index = 1; index < points.size(); ++index) {
		const utf8proc_int32_t point = points[index];
		if (point == '-' || point == '\'' || point == 0x2019) {
			if (index + 1 == points.size() || !is_letter(points[index - 1]) || !is_letter(points[index + 1])) return false;
			continue;
		}
		if (!is_letter(point)) return false;
		if (utf8proc_category(point) == UTF8PROC_CATEGORY_LL) small = true;
	}
	return small;
}

// Initials, each with its full stop cut off by the caller: "И", "J".
bool is_initials(const std::string &word) {
	const std::vector<utf8proc_int32_t> points = code_points(word);
	return points.size() == 1 && is_capital(points.front());
}

bool is_cyrillic(const std::string &word) {
	const std::vector<utf8proc_int32_t> points = code_points(word);
	return !points.empty() && points.front() >= 0x0400 && points.front() <= 0x04FF;
}

// How a Russian surname declines, by the ending it has in the nominative:
// every ending it takes in that one's place, listed rather than derived, and
// how surely the ending marks a surname rather than a first name ("Марина")
// or an adjective. The empty nominative is a surname ending in a consonant,
// which declines like a noun: Шевчук, Шевчука, Шевчуку.
struct SurnameDeclension {
	std::string_view nominative;
	std::vector<std::string_view> endings;
	int likeness = 0;
};

const std::vector<SurnameDeclension> &surname_declensions() {
	static const std::vector<SurnameDeclension> declensions{
		{"ская", {"ская", "ской", "скую", "скою"}, 2},
		{"цкая", {"цкая", "цкой", "цкую", "цкою"}, 2},
		{"ский", {"ский", "ского", "скому", "ским", "ском", "ские", "ских", "скими"}, 2},
		{"цкий", {"цкий", "цкого", "цкому", "цким", "цком", "цкие", "цких", "цкими"}, 2},
		{"ской", {"ской", "ского", "скому", "ским", "ском"}, 2},
		{"цкой", {"цкой", "цкого", "цкому", "цким", "цком"}, 2},
		{"ова", {"ова", "овой", "ову", "овою"}, 2},
		{"ева", {"ева", "евой", "еву", "евою"}, 2},
		{"ёва", {"ёва", "ёвой", "ёву", "ёвою"}, 2},
		{"ина", {"ина", "иной", "ину", "иною"}, 1},
		{"ына", {"ына", "ыной", "ыну", "ыною"}, 1},
		{"ов", {"ов", "ова", "ову", "овым", "ове", "овы", "овых", "овыми"}, 2},
		{"ев", {"ев", "ева", "еву", "евым", "еве", "евы", "евых", "евыми"}, 2},
		{"ёв", {"ёв", "ёва", "ёву", "ёвым", "ёве", "ёвы", "ёвых", "ёвыми"}, 2},
		{"ин", {"ин", "ина", "ину", "иным", "ине", "ины", "иных", "иными"}, 1},
		{"ын", {"ын", "ына", "ыну", "ыным", "ыне", "ыны", "ыных", "ыными"}, 1},
		{"ой", {"ой", "ого", "ому", "ым", "ом"}, 1},
		{"ый", {"ый", "ого", "ому", "ым", "ом"}, 1},
		{"ь", {"ь", "я", "ю", "ем", "е"}, 0},
		{"й", {"й", "я", "ю", "ем", "е"}, 0},
		{"", {"", "а", "у", "ом", "ем", "е"}, 0},
	};
	return declensions;
}

// The declension a nominative surname follows, by its ending; none for one
// that does not decline (Шевченко, Черных). At least two letters must stand
// before the ending.
const SurnameDeclension *declension_of(const std::string &nominative) {
	static const std::vector<std::string_view> consonants{
		"б", "в", "г", "д", "ж", "з", "к", "л", "м", "н", "п", "р", "с", "т", "ф", "х", "ц", "ч", "ш", "щ",
	};
	const auto endsWith = [&](std::string_view ending) {
		return nominative.size() >= ending.size()
			&& nominative.compare(nominative.size() - ending.size(), ending.size(), ending.data(), ending.size()) == 0;
	};
	for (const SurnameDeclension &declension : surname_declensions()) {
		if (nominative.size() < declension.nominative.size() + 4) continue;
		if (!declension.nominative.empty()) {
			if (endsWith(declension.nominative)) return &declension;
			continue;
		}
		for (const std::string_view consonant : consonants)
			if (endsWith(consonant)) return &declension;
	}
	return nullptr;
}

// Every nominative a written Russian surname can be a case form of, with its
// declension: "Громова" is Громов in the genitive or Громова herself. A
// reading counts only where the nominative really follows that declension, so
// "Иванова" is never taken for a form of "Иван".
std::vector<std::pair<std::string, const SurnameDeclension *>> surname_nominatives(const std::string &written) {
	std::vector<std::pair<std::string, const SurnameDeclension *>> nominatives;
	for (const SurnameDeclension &declension : surname_declensions()) {
		for (const std::string_view ending : declension.endings) {
			if (written.size() < ending.size() + 4
					|| written.compare(written.size() - ending.size(), ending.size(), ending.data(), ending.size()) != 0)
				continue;
			std::string nominative = written.substr(0, written.size() - ending.size());
			nominative.append(declension.nominative.data(), declension.nominative.size());
			if (declension_of(nominative) != &declension) continue;
			const bool known = std::any_of(nominatives.begin(), nominatives.end(),
				[&](const auto &reading) { return reading.first == nominative; });
			if (!known) nominatives.emplace_back(nominative, &declension);
		}
	}
	return nominatives;
}

// How surely a Russian word is a surname by its ending, in any case form:
// "Смирнову" is as sure as "Смирнов", "Марина" less so, "Анна" not at all.
int surname_likeness(const std::string &word) {
	int likeness = 0;
	for (const auto &[nominative, declension] : surname_nominatives(word)) {
		static_cast<void>(nominative);
		likeness = std::max(likeness, declension->likeness);
	}
	return likeness;
}

// A Russian patronymic in any case: Сергеевич, Сергеевича, Викторовну,
// Ильинична.
bool is_patronymic(const std::string &word) {
	for (const char *marker : {"ович", "евич", "овн", "евн", "ичн", "ьич"})
		if (word.find(marker) != std::string::npos) return true;
	return false;
}

// Every writing of a surname that is still that surname: the surname itself,
// and for a Russian one each of its case forms.
// The gender a surname's declension is a surname of: -ова, -ская and the rest
// in -а are a woman's; every other a man's.
russian_names::Gender declension_gender(const SurnameDeclension &declension) {
	static const std::set<std::string_view> feminine{"ская", "цкая", "ова", "ева", "ёва", "ина", "ына"};
	return feminine.count(declension.nominative) > 0 ? russian_names::Gender::Feminine : russian_names::Gender::Masculine;
}

// Every writing of a surname that is still that surname: the surname itself,
// and for a Russian one each of its case forms - the forms its bearer's gender
// takes, when that is known: «Кузнецова» is Дмитрий Кузнецов's genitive and
// Анна Кузнецова herself, «Кузнецовой» only hers, «Кузнецовым» only his.
std::vector<std::string> surname_writings(const std::string &surname,
		russian_names::Gender gender = russian_names::Gender::Unknown) {
	if (!is_cyrillic(surname)) return {surname};
	std::set<std::string> forms{surname};
	const bool gendered = gender == russian_names::Gender::Masculine || gender == russian_names::Gender::Feminine;
	for (const auto &[nominative, declension] : surname_nominatives(surname)) {
		if (gendered && declension_gender(*declension) != gender) continue;
		const std::string stem = nominative.substr(0, nominative.size() - declension->nominative.size());
		for (const std::string_view ending : declension->endings) forms.insert(stem + std::string(ending));
	}
	return std::vector<std::string>(forms.begin(), forms.end());
}

// A person's name read for its surname: the surname, the first letter of the
// given name when the name has one, and how many parts it has - "Victor Hale"
// is Hale, V, two parts; "И. Громов" is Громов, И; "Громов Иван Сергеевич" is
// Громов, И; "Ms. Morgan" is Morgan alone, one part. Empty when the value is
// not written as a name - a word in lowercase, capitals throughout, a phrase -
// or when the surname is shorter than three letters, which too many ordinary
// words equal ("He", "An", "Do").
struct PersonName {
	std::string surname;
	std::string initial;
	std::size_t parts = 0;
	// The given name written out, when the name has one ("Victor", "Андрей"),
	// and the first letter of every part before or beside the surname, in
	// order: "Лебедев Андрей Игоревич" is А, И; "M. A. Delgado" is M, A.
	std::string given;
	std::vector<std::string> initials;
};

PersonName person_name(const std::string &value) {
	static const std::set<std::string> titles{
		"Mr", "Mrs", "Ms", "Miss", "Mx", "Dr", "Prof", "Professor", "Judge", "Justice", "Hon", "Honorable",
		"Honourable", "Sir", "Dame", "Rev", "г-н", "г-жа", "господин", "госпожа", "Господин", "Госпожа", "судья",
		"Судья",
	};
	static const std::set<std::string> suffixes{"Jr", "Sr", "II", "III", "IV", "Esq", "PhD", "MD"};
	static const std::set<std::string> particles{
		"van", "von", "de", "der", "den", "da", "di", "du", "la", "le", "del", "della", "dos", "das", "bin", "ibn",
		"al",
	};
	// Words split at spaces, and after every full stop: "И.С.Громов".
	std::vector<std::string> tokens;
	std::string token;
	const auto flush = [&] {
		if (!token.empty()) tokens.push_back(token);
		token.clear();
	};
	for (std::size_t offset = 0; offset < value.size(); ++offset) {
		const char character = value[offset];
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
			flush();
			continue;
		}
		if (value.compare(offset, 2, "\xC2\xA0") == 0) {
			flush();
			++offset;
			continue;
		}
		token.push_back(character);
		if (character == '.') flush();
	}
	flush();

	struct Part {
		std::string text;
		bool initial = false;
	};
	std::vector<Part> parts;
	bool reversed = false;
	for (std::string word : tokens) {
		bool comma = false;
		while (!word.empty() && (word.back() == ',' || word.back() == ';')) {
			word.pop_back();
			comma = true;
		}
		if (!word.empty() && word.back() == '.') word.pop_back();
		for (const char *possessive : {"'s", "\xe2\x80\x99s"}) {
			const std::size_t size = std::char_traits<char>::length(possessive);
			if (word.size() > size && word.compare(word.size() - size, size, possessive) == 0) word.erase(word.size() - size);
		}
		if (word.empty()) continue;
		if (parts.empty() && titles.count(word) > 0) continue;
		if (!parts.empty() && suffixes.count(word) > 0) continue;
		if (particles.count(word) > 0) continue;
		if (is_initials(word)) {
			parts.push_back({word, true});
			continue;
		}
		if (!is_name_word(word)) return {};
		// "Hale, Victor": the surname first, as a list writes it.
		if (comma && parts.empty()) reversed = true;
		parts.push_back({word, false});
	}
	std::vector<std::size_t> names;
	for (std::size_t index = 0; index < parts.size(); ++index)
		if (!parts[index].initial) names.push_back(index);
	if (names.empty()) return {};

	std::size_t surname = names.back();
	const bool russian = is_cyrillic(parts[surname].text);
	if (names.size() > 1 && reversed) surname = names.front();
	else if (russian && names.size() == 3 && is_patronymic(parts[names[2]].text)) surname = names[0];
	else if (russian && names.size() == 3 && is_patronymic(parts[names[1]].text)) surname = names[2];
	else if (russian && names.size() == 2
			&& surname_likeness(parts[names[0]].text) > surname_likeness(parts[names[1]].text))
		surname = names[0];
	// «Андрей Игоревич», «Андрея Игоревича»: a first name and a patronymic, as a
	// letter addresses someone. No surname stands in it; whose name it is, is
	// read by given_patronymic_owner().
	if (russian && names.size() == 2 && parts.size() == 2 && is_patronymic(parts[names[1]].text)
			&& !is_patronymic(parts[names[0]].text) && surname_likeness(parts[names[0]].text) < 2)
		return {};
	if (code_points(parts[surname].text).size() < 3) return {};

	PersonName name;
	name.surname = parts[surname].text;
	name.parts = parts.size();
	for (std::size_t index = 0; index < parts.size(); ++index) {
		if (index == surname) continue;
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(parts[index].text.data()),
			static_cast<utf8proc_ssize_t>(parts[index].text.size()), &codePoint);
		if (consumed <= 0) continue;
		const std::string first = parts[index].text.substr(0, static_cast<std::size_t>(consumed));
		if (name.initial.empty()) name.initial = first;
		name.initials.push_back(first);
		if (name.given.empty() && !parts[index].initial) name.given = parts[index].text;
	}
	return name;
}

// Whether a detected person is written as a surname alone, a title at most
// before it - "Hale", "Ms. Morgan", "Громову" - rather than named in full.
bool written_as_surname_alone(const CanonicalEntity &entity) {
	if (!entity.type || (*entity.type != EntityType::Person && *entity.type != EntityType::Judge)) return false;
	const PersonName name = person_name(entity.value);
	return !name.surname.empty() && name.parts == 1;
}

// Whether a detected person is written with initials and a surname and nothing
// else: "M. Delgado", «А. И. Лебедев», «Лебедеву А. И.».
bool written_with_initials(const CanonicalEntity &entity) {
	if (!person_type(entity.type)) return false;
	const PersonName name = person_name(entity.value);
	return !name.surname.empty() && name.parts >= 2 && name.given.empty() && !name.initials.empty();
}

// Whether a detected person is named in full: a given name and a surname.
bool written_in_full(const CanonicalEntity &entity) {
	if (!person_type(entity.type)) return false;
	const PersonName name = person_name(entity.value);
	return !name.surname.empty() && name.parts >= 2 && !name.given.empty();
}

// The fullest writing of a person row: the most parts, a given name written
// out rather than an initial, and the row's own spelling on a tie.
std::string fullest_person_writing(const RequestMappingEntry &entry) {
	std::string fullest = entry.original;
	std::size_t bestParts = 0;
	bool bestGiven = false;
	bool first = true;
	for (const std::string &writing : entry.every_form()) {
		const PersonName name = person_name(writing);
		if (name.surname.empty()) continue;
		const bool given = !name.given.empty();
		if (first || name.parts > bestParts || (name.parts == bestParts && given && !bestGiven)) {
			fullest = writing;
			bestParts = name.parts;
			bestGiven = given;
			first = false;
		}
	}
	return fullest;
}

// A person's gender from the fullest writing of the name, where Russian shows
// it (a patronymic, a known first name, a surname's form); Unknown otherwise.
russian_names::Gender person_gender(const std::string &writing) {
	if (!has_cyrillic_letter(writing)) return russian_names::Gender::Unknown;
	const russian_names::Gender gender = russian_names::gender_of(writing);
	return gender == russian_names::Gender::Masculine || gender == russian_names::Gender::Feminine ? gender
		: russian_names::Gender::Unknown;
}

// A person row's gender from whichever of its writings shows it: «Д. Карповой»
// shows a woman where «Д. Карпова» alone could be a man in the genitive.
russian_names::Gender row_person_gender(const RequestMappingEntry &entry) {
	const russian_names::Gender fullest = person_gender(fullest_person_writing(entry));
	if (fullest != russian_names::Gender::Unknown) return fullest;
	for (const std::string &writing : entry.every_form()) {
		const russian_names::Gender gender = person_gender(writing);
		if (gender != russian_names::Gender::Unknown) return gender;
	}
	return entry.gender;
}

// The gender a Russian name written at text[start, start + length) must have
// for the sentence around it to read as written, when the name alone cannot
// say: each gender's nominative is put back where the name stands, taking the
// case its place asks for (RestorationGrammar), and the one whose sentence
// comes out unchanged is the name's. «Генеральному директору Лебедеву А. И.»
// stays so only for Лебедев - Лебедева would read «Лебедевой» there - and
// restoration then writes «Уважаемый …», «Лебедев А. И. подписал», never
// «Уважаемый(ая) Лебедеву А. И.» (a verifier's run, 25 Sep 2026). Unknown when
// both or neither do.
russian_names::Gender gender_in_sentence(const std::string &text, std::size_t start, std::size_t length) {
	const std::string value = text.substr(start, length);
	if (!has_cyrillic_letter(value)) return russian_names::Gender::Unknown;
	std::size_t from = start > 160 ? start - 160 : 0;
	const std::size_t lineStart = start == 0 ? std::string::npos : text.rfind('\n', start - 1);
	if (lineStart != std::string::npos && lineStart + 1 > from) from = lineStart + 1;
	while (from < start && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80) ++from;
	const std::string sentence = text.substr(from, start + length - from);
	std::optional<russian_names::Gender> only;
	for (const russian_names::Gender gender : {russian_names::Gender::Masculine, russian_names::Gender::Feminine}) {
		restoration_grammar::RestoredSpan span;
		span.offset = start - from;
		span.length = length;
		span.kind = restoration_grammar::SpanKind::Person;
		span.canonical = russian_names::declined(value, russian_names::Case::Nominative, gender);
		span.gender = gender;
		if (restoration_grammar::agree(sentence, {span}) != sentence) continue;
		if (only) return russian_names::Gender::Unknown;
		only = gender;
	}
	return only.value_or(russian_names::Gender::Unknown);
}

// Whether two given names are one: the same word, or one Russian first name in
// two of its cases («Анна», «Анны»).
bool same_given_name(const std::string &left, const std::string &right) {
	if (left == right) return true;
	if (has_cyrillic_letter(left) && has_cyrillic_letter(right)) {
		const auto leftName = russian_names::detail::recognised_first_name(russian_names::detail::lowered(left));
		const auto rightName = russian_names::detail::recognised_first_name(russian_names::detail::lowered(right));
		return leftName && rightName && leftName->first[0] == rightName->first[0];
	}
	if (left.size() != right.size()) return false;
	return std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
	});
}

// Whether the initials two writings of a name show can be one person's: each
// letter the same where both show one. "M." agrees with Marcus Delgado, «А. П.»
// never with Дмитрий Олегович.
bool initials_agree(const std::vector<std::string> &left, const std::vector<std::string> &right) {
	for (std::size_t index = 0; index < left.size() && index < right.size(); ++index)
		if (left[index] != right[index]) return false;
	return true;
}

// Brings the surname lookup up to date with the table: a person row that is
// new, or has been written another way since it was read, is read again.
void refresh_surname_index(const OperationProtectedSymbolTable &symbols) {
	if (symbols.surname_records.size() > symbols.mapping.size()) {
		symbols.surname_records.clear();
		symbols.surname_rows_by_form.clear();
	}
	symbols.surname_records.resize(symbols.mapping.size());
	for (std::size_t row = 0; row < symbols.mapping.size(); ++row) {
		const RequestMappingEntry &entry = symbols.mapping[row];
		if (!entry.type || (*entry.type != EntityType::Person && *entry.type != EntityType::Judge)) continue;
		std::size_t signature = entry.surface_forms.size() * 1000003u + entry.original.size();
		for (const std::string &form : entry.surface_forms) signature = signature * 31u + form.size();
		OperationProtectedSymbolTable::SurnameRecord &record = symbols.surname_records[row];
		if (record.signature == signature) continue;
		for (const std::string &form : record.forms) {
			const auto owners = symbols.surname_rows_by_form.find(form);
			if (owners == symbols.surname_rows_by_form.end()) continue;
			owners->second.erase(std::remove(owners->second.begin(), owners->second.end(), row), owners->second.end());
			if (owners->second.empty()) symbols.surname_rows_by_form.erase(owners);
		}
		record = OperationProtectedSymbolTable::SurnameRecord{};
		record.signature = signature;
		const std::string fullest = fullest_person_writing(entry);
		const PersonName full = person_name(fullest);
		record.given = full.given;
		record.initials = full.initials;
		const russian_names::Gender gender = row_person_gender(entry);
		for (const std::string &writing : entry.every_form()) {
			const PersonName name = person_name(writing);
			if (name.surname.empty()) continue;
			if (record.initial.empty()) record.initial = name.initial;
			if (record.given.empty()) record.given = name.given;
			if (record.initials.size() < name.initials.size() && initials_agree(record.initials, name.initials))
				record.initials = name.initials;
			for (const std::string &form : surname_writings(name.surname, gender))
				if (std::find(record.forms.begin(), record.forms.end(), form) == record.forms.end())
					record.forms.push_back(form);
		}
		if (record.initial.empty() && !record.initials.empty()) record.initial = record.initials.front();
		for (const std::string &form : record.forms) symbols.surname_rows_by_form[form].push_back(row);
	}
}

// The row a surname written alone names: the one person this table knows by
// it. Two people who share it with different given names - Victor Hale and
// Maria Hale - leave it shared, and then it gets a symbol of its own: a new
// symbol is safer than a false merge. A surname the checkpoint typed itself
// joins only a row of that type, so "Judge Morgan" is never counsel Dana
// Morgan. A surname written with initials or a given name ("M. Delgado",
// «Кузнецова А. П.») names only a person whose name those agree with.
struct SurnameOwner {
	std::optional<std::size_t> row;
	bool shared = false;
};

SurnameOwner surname_owner(const OperationProtectedSymbolTable &symbols, const std::string &form,
		std::optional<EntityType> type = std::nullopt, const PersonName *written = nullptr) {
	const auto owners = symbols.surname_rows_by_form.find(form);
	if (owners == symbols.surname_rows_by_form.end()) return {};
	const OperationProtectedSymbolTable::SurnameRecord *namedRecord = nullptr;
	std::optional<std::size_t> named;
	std::optional<std::size_t> any;
	for (const std::size_t row : owners->second) {
		if (type && symbols.mapping[row].type != type) continue;
		const OperationProtectedSymbolTable::SurnameRecord &record = symbols.surname_records[row];
		if (written != nullptr) {
			if (!initials_agree(written->initials, record.initials)) continue;
			if (!written->given.empty() && !record.given.empty() && !same_given_name(written->given, record.given)) continue;
		}
		if (!any || row < *any) any = row;
		if (record.initial.empty()) continue;
		if (namedRecord != nullptr && (record.initial != namedRecord->initial
				|| (!record.given.empty() && !namedRecord->given.empty() && !same_given_name(record.given, namedRecord->given))))
			return {std::nullopt, true};
		if (namedRecord == nullptr) namedRecord = &record;
		if (!named || row < *named) named = row;
	}
	return {named ? named : any, false};
}

// The one person row whose given name a name written alone is: "Marcus" is
// Marcus Delgado when no other person here is called Marcus. `shared`, when
// asked, says that two people here are.
std::optional<std::size_t> given_name_owner(const OperationProtectedSymbolTable &symbols, const std::string &value,
		std::optional<EntityType> type, bool *shared = nullptr) {
	if (shared != nullptr) *shared = false;
	std::optional<std::size_t> only;
	for (std::size_t row = 0; row < symbols.mapping.size() && row < symbols.surname_records.size(); ++row) {
		if (!person_type(symbols.mapping[row].type) || (type && symbols.mapping[row].type != type)) continue;
		const std::string &given = symbols.surname_records[row].given;
		if (given.empty() || !same_given_name(given, value)) continue;
		if (only) {
			if (shared != nullptr) *shared = true;
			return std::nullopt;
		}
		only = row;
	}
	return only;
}

// «Андрей Игоревич», «Андрея Игоревича»: a Russian first name and a
// patronymic with no surname - the first name as written and the patronymic's
// nominative. Nothing for any other writing.
std::optional<std::pair<std::string, std::string>> given_and_patronymic(const std::string &value) {
	if (!has_cyrillic_letter(value)) return std::nullopt;
	const std::vector<std::string> words = russian_names::detail::words_of(value);
	if (words.size() != 2 || !is_name_word(words[0]) || !is_name_word(words[1])) return std::nullopt;
	if (is_patronymic(words[0]) || surname_likeness(words[0]) >= 2) return std::nullopt;
	const auto patronymic = russian_names::detail::patronymic_nominative(russian_names::detail::lowered(words[1]));
	if (!patronymic) return std::nullopt;
	return std::make_pair(words[0], patronymic->first);
}

// The one person row a first name and a patronymic written alone name: the
// row whose name has that first name and that patronymic («Уважаемый Андрей
// Игоревич!» is Лебедев Андрей Игоревич when no one else here is Андрей
// Игоревич).
std::optional<std::size_t> given_patronymic_owner(const OperationProtectedSymbolTable &symbols, const std::string &value) {
	const auto reading = given_and_patronymic(value);
	if (!reading) return std::nullopt;
	std::optional<std::size_t> only;
	for (std::size_t row = 0; row < symbols.mapping.size(); ++row) {
		const RequestMappingEntry &entry = symbols.mapping[row];
		if (!person_type(entry.type) || entry.original == value) continue;
		const std::optional<PersonNameParts> parts = person_name_parts(entry);
		if (!parts || parts->last.empty() || parts->patronymic.empty()) continue;
		if (!same_given_name(parts->first, reading->first)
				|| russian_names::detail::lowered(parts->patronymic) != russian_names::detail::lowered(reading->second))
			continue;
		if (only) return std::nullopt;
		only = row;
	}
	return only;
}

// A word's first letter, as a name's initial is written.
std::string first_character(const std::string &word) {
	utf8proc_int32_t codePoint = 0;
	const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(word.data()),
		static_cast<utf8proc_ssize_t>(word.size()), &codePoint);
	return word.substr(0, consumed > 0 ? static_cast<std::size_t>(consumed) : std::size_t{1});
}

// The name words written right after a surname: a Russian first name, a
// patronymic, initials ("Кузнецова Анна Павловна", "Кузнецова А. П.") - up to
// two of them, with where they end. Nothing when a surname stands alone.
struct FollowingName {
	std::size_t end = 0;
	PersonName name;
};

std::optional<FollowingName> name_words_after(const std::string &text, std::size_t end) {
	FollowingName following;
	std::size_t cursor = end;
	for (int taken = 0; taken < 2; ++taken) {
		std::size_t next = cursor;
		if (next < text.size() && text[next] == ' ') ++next;
		else if (text.compare(next, 2, "\xC2\xA0") == 0) next += 2;
		else if (!(taken > 0 && next > 0 && text[next - 1] == '.')) break;
		std::size_t wordEnd = next;
		while (wordEnd < text.size() && continues_word_at(text, wordEnd)) {
			utf8proc_int32_t codePoint = 0;
			const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + wordEnd,
				static_cast<utf8proc_ssize_t>(text.size() - wordEnd), &codePoint);
			if (consumed <= 0) break;
			wordEnd += static_cast<std::size_t>(consumed);
		}
		if (wordEnd == next) break;
		const std::string word = text.substr(next, wordEnd - next);
		if (is_initials(word) && wordEnd < text.size() && text[wordEnd] == '.') {
			following.name.initials.push_back(word);
			cursor = wordEnd + 1;
			continue;
		}
		if (!is_cyrillic(word) || !is_name_word(word)) break;
		const std::string lower = russian_names::detail::lowered(word);
		const bool firstName = russian_names::detail::recognised_first_name(lower).has_value();
		const bool patronymic = russian_names::detail::patronymic_nominative(lower).has_value();
		if (!firstName && !patronymic) break;
		if (firstName && following.name.given.empty() && following.name.initials.empty()) following.name.given = word;
		following.name.initials.push_back(first_character(word));
		cursor = wordEnd;
	}
	if (following.name.initials.empty()) return std::nullopt;
	following.end = cursor;
	return following;
}

bool different_person_follows(const std::string &text, std::size_t end, const OperationProtectedSymbolTable &symbols,
		std::size_t row) {
	const std::optional<FollowingName> following = name_words_after(text, end);
	if (!following) return false;
	refresh_surname_index(symbols);
	if (row >= symbols.surname_records.size()) return false;
	const OperationProtectedSymbolTable::SurnameRecord &record = symbols.surname_records[row];
	if (!initials_agree(following->name.initials, record.initials)) return true;
	return !following->name.given.empty() && !record.given.empty() && !same_given_name(following->name.given, record.given);
}

// The words of a text as byte ranges - runs of letters, marks and digits in
// any script - and whether each begins with a capital.
struct WordSpan {
	std::size_t start = 0;
	std::size_t end = 0;
	bool capitalised = false;
};

std::vector<WordSpan> word_spans(const std::string &text) {
	std::vector<WordSpan> words;
	std::size_t position = 0;
	while (position < text.size()) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(
			reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + position,
			static_cast<utf8proc_ssize_t>(text.size() - position), &codePoint);
		if (consumed <= 0) {
			++position;
			continue;
		}
		const std::size_t next = position + static_cast<std::size_t>(consumed);
		if (is_word_category(utf8proc_category(codePoint))) {
			if (words.empty() || words.back().end != position) words.push_back({position, next, is_capital(codePoint)});
			else words.back().end = next;
		}
		position = next;
	}
	return words;
}

// Whether two words stand joined by one hyphen or apostrophe, as the parts of
// "Hale-Smith" or "O'Neil" do.
bool joined_by_one_mark(const std::string &text, const WordSpan &left, const WordSpan &right) {
	const std::string between = text.substr(left.end, right.start - left.end);
	return between == "-" || between == "'" || between == "\xe2\x80\x99";
}

// Whether the capitalised word `index` of a text can be a first name standing
// alone: a word written as a name, not a month or a day ("June 5"), not the
// first word of someone else's full name ("Marcus Smith"), and not a Russian
// word Singularity does not know as a first name. A word the language also has
// ("Grace", "Maria") counts where a sentence begins too, as a surname does:
// "Grace will countersign." went out readable while the person's symbol stood
// elsewhere (a verifier's run, 25 Sep 2026), and a word hidden needlessly costs
// less than a name sent.
bool stands_as_given_name(const std::string &text, const std::vector<WordSpan> &words, std::size_t index) {
	static const std::set<std::string> calendar{
		"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november",
		"december", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
	};
	const std::string form = text.substr(words[index].start, words[index].end - words[index].start);
	if (!is_name_word(form)) return false;
	const std::size_t end = words[index].end;
	if (index + 1 < words.size()) {
		const WordSpan &next = words[index + 1];
		const std::string between = text.substr(end, next.start - end);
		const bool oneSpace = between == " " || between == "\xC2\xA0";
		if (oneSpace && (next.capitalised || (text[next.start] >= '0' && text[next.start] <= '9'))) return false;
	}
	if (has_cyrillic_letter(form)) {
		if (!russian_names::detail::recognised_first_name(russian_names::detail::lowered(form))) return false;
		return !name_words_after(text, end).has_value();
	}
	std::string lower = form;
	for (char &character : lower) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	return calendar.count(lower) == 0;
}

// ---------------------------------------------------------------------------
// An organisation's short name: "Meadowbrook" is Meadowbrook Industrial
// Partners LLC wherever the documents write it alone.

bool organisation_type(std::optional<EntityType> type) {
	return type && (*type == EntityType::Organisation || *type == EntityType::LegalEntity);
}

// A company's legal form: "LLC", "L.L.C.", "Inc.", «ООО», "GmbH".
bool legal_form_word(const std::string &word) {
	static const std::set<std::string> forms{"llc", "lllp", "llp", "lp", "inc", "incorporated", "corp", "corporation", "co",
		"company", "ltd", "limited", "plc", "pllc", "pc", "gmbh", "ag", "sa", "sas", "sarl", "nv", "bv", "spa", "srl", "kg",
		"\xd0\xbe\xd0\xbe\xd0\xbe", "\xd0\xbe\xd0\xb0\xd0\xbe", "\xd0\xb7\xd0\xb0\xd0\xbe", "\xd0\xbf\xd0\xb0\xd0\xbe",
		"\xd0\xb0\xd0\xbe", "\xd0\xbd\xd0\xba\xd0\xbe", "\xd0\xb0\xd0\xbd\xd0\xbe", "\xd0\xb8\xd0\xbf", "\xd0\xb3\xd1\x83\xd0\xbf",
		"\xd0\xbc\xd1\x83\xd0\xbf", "\xd1\x84\xd0\xb3\xd1\x83\xd0\xbf", "\xd1\x82\xd0\xbe\xd0\xbe", "ooo"};
	return forms.count(normalized_entity_form(word)) > 0;
}

// A word any company's name may have - its trade, its kind, its reach:
// "Industrial", "Partners", "Holdings", "Foods", «Групп», «Холдинг». Never the
// name a company is known by alone.
bool generic_company_word(const std::string &word) {
	static const std::set<std::string> words{"industrial", "industries", "industry", "partners", "partner", "partnership",
		"holdings", "holding", "group", "groups", "foods", "food", "robotics", "capital", "management", "services",
		"service", "solutions", "systems", "technologies", "technology", "tech", "logistics", "properties", "property",
		"realty", "investments", "investment", "investors", "ventures", "venture", "enterprises", "enterprise",
		"international", "global", "worldwide", "trust", "bank", "bancorp", "fund", "funds", "associates", "consulting",
		"consultants", "clinical", "clinics", "clinic", "health", "healthcare", "medical", "energy", "manufacturing",
		"storage", "trading", "development", "developers", "construction", "advisors", "advisers", "advisory",
		"securities", "financial", "finance", "insurance", "pharmaceuticals", "pharma", "labs", "laboratories",
		"networks", "network", "media", "communications", "resources", "brands", "retail", "products", "equity",
		// A trade: "Northwind Freight Systems" is known as "Northwind".
		"freight", "shipping", "transport", "transportation", "airlines", "airways", "aviation", "motors",
		"automotive", "software", "analytics", "therapeutics", "biosciences", "sciences", "chemicals", "materials",
		"mining", "utilities", "entertainment", "studios", "publishing", "hospitality",
		"estate", "real", "land", "north", "south", "east", "west", "american", "national", "united", "first", "new",
		"general", "the", "and", "of", "for"};
	static const char *const russianStems[] = {"\xd0\xb3\xd1\x80\xd1\x83\xd0\xbf\xd0\xbf",  // групп
		"\xd1\x85\xd0\xbe\xd0\xbb\xd0\xb4\xd0\xb8\xd0\xbd\xd0\xb3",  // холдинг
		"\xd0\xba\xd0\xbe\xd0\xbc\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb8",  // компани
		"\xd0\xba\xd0\xbe\xd1\x80\xd0\xbf\xd0\xbe\xd1\x80\xd0\xb0\xd1\x86",  // корпорац
		"\xd0\xb1\xd0\xb0\xd0\xbd\xd0\xba",  // банк
		"\xd1\x84\xd0\xbe\xd0\xbd\xd0\xb4",  // фонд
		"\xd0\xba\xd0\xbe\xd0\xbd\xd1\x86\xd0\xb5\xd1\x80\xd0\xbd",  // концерн
		"\xd0\xb0\xd0\xb3\xd0\xb5\xd0\xbd\xd1\x82\xd1\x81\xd1\x82\xd0\xb2",  // агентств
		"\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb4\xd0\xbf\xd1\x80\xd0\xb8\xd1\x8f\xd1\x82\xd0\xb8",  // предприяти
		"\xd0\xb7\xd0\xb0\xd0\xb2\xd0\xbe\xd0\xb4",  // завод
		"\xd1\x84\xd0\xb0\xd0\xb1\xd1\x80\xd0\xb8\xd0\xba",  // фабрик
		"\xd1\x82\xd0\xbe\xd1\x80\xd0\xb3\xd0\xbe\xd0\xb2",  // торгов
		"\xd0\xbf\xd1\x80\xd0\xbe\xd0\xbc\xd1\x8b\xd1\x88\xd0\xbb\xd0\xb5\xd0\xbd",  // промышлен
		"\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb8\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c",  // строитель
		"\xd0\xb8\xd0\xbd\xd0\xb2\xd0\xb5\xd1\x81\xd1\x82\xd0\xb8\xd1\x86",  // инвестиц
		"\xd0\xba\xd0\xb0\xd0\xbf\xd0\xb8\xd1\x82\xd0\xb0\xd0\xbb",  // капитал
		"\xd0\xbf\xd0\xb0\xd1\x80\xd1\x82\xd0\xbd\xd0\xb5\xd1\x80",  // партнер
		"\xd1\x86\xd0\xb5\xd0\xbd\xd1\x82\xd1\x80",  // центр
		"\xd0\xbe\xd0\xb1\xd1\x89\xd0\xb5\xd1\x81\xd1\x82\xd0\xb2",  // обществ
		"\xd1\x82\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x80\xd0\xb8\xd1\x89\xd0\xb5\xd1\x81\xd1\x82\xd0\xb2",  // товариществ
		"\xd0\xb0\xd1\x81\xd1\x81\xd0\xbe\xd1\x86\xd0\xb8\xd0\xb0\xd1\x86",  // ассоциац
		"\xd1\x81\xd0\xbe\xd1\x8e\xd0\xb7",  // союз
		"\xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb8\xd1\x81",  // сервис
		"\xd1\x82\xd0\xb5\xd1\x85\xd0\xbd\xd0\xbe\xd0\xbb\xd0\xbe\xd0\xb3",  // технолог
		"\xd0\xbb\xd0\xbe\xd0\xb3\xd0\xb8\xd1\x81\xd1\x82\xd0\xb8\xd0\xba",  // логистик
	};
	const std::string form = normalized_entity_form(word);
	if (words.count(form) > 0) return true;
	for (const char *stem : russianStems)
		if (form.rfind(stem, 0) == 0) return true;
	return false;
}

// Whether a word of a company's name could be the name it is known by alone:
// written with a capital, four letters or more, not a legal form, not a word
// any company's name has, not a word of the language ("Harbor", "Larkspur") and
// not a place ("Texas").
bool distinctive_company_word(const std::string &word) {
	const std::vector<utf8proc_int32_t> points = code_points(word);
	if (points.empty() || !is_capital(points.front())) return false;
	std::size_t letters = 0;
	for (const utf8proc_int32_t point : points) {
		if (is_letter(point)) ++letters;
		else if (point != '-' && point != '\'' && point != 0x2019 && !(point >= '0' && point <= '9')) return false;
	}
	if (letters < 4 || legal_form_word(word) || generic_company_word(word)) return false;
	if (is_common_word(word) || is_public_jurisdiction(word)) return false;
	return true;
}

// The name a company of words of the language is known by: its first two
// words together, each written with a capital and neither a word any
// company's name has - "Kestrel Ridge" of "Kestrel Ridge Properties LLC",
// "Harbor Point" of "Harbor Point Capital LLC". Each word alone names nothing
// ("Harbor", "Ridge"), so both were once left out and "Kestrel Ridge will
// repair the roof" went out readable. Never a street ("Main Street Capital").
std::string common_words_short_name(const std::string &whole, const std::vector<std::pair<std::size_t, std::size_t>> &words) {
	static const std::set<std::string> streets{"street", "avenue", "road", "lane", "boulevard", "drive", "place", "square",
		"st", "ave", "rd", "blvd", "way", "highway", "parkway"};
	if (words.size() < 3) return {};
	const auto word = [&](std::size_t index) { return whole.substr(words[index].first, words[index].second - words[index].first); };
	if (whole.substr(words[0].second, words[1].first - words[0].second) != " ") return {};
	std::size_t letters = 0;
	for (std::size_t index = 0; index < 2; ++index) {
		const std::string each = word(index);
		const std::vector<utf8proc_int32_t> points = code_points(each);
		if (points.empty() || !is_capital(points.front()) || !std::all_of(points.begin(), points.end(), is_letter)) return {};
		if (legal_form_word(each) || generic_company_word(each) || is_public_jurisdiction(each)) return {};
		letters = std::max(letters, points.size());
	}
	if (letters < 4 || streets.count(normalized_entity_form(word(1))) > 0) return {};
	// The words after it only say what kind of company it is.
	const std::string next = word(2);
	if (!generic_company_word(next) && !legal_form_word(next) && !is_public_jurisdiction(next)) return {};
	return whole.substr(words[0].first, words[1].second - words[0].first);
}

// The name a company is known by alone: the name it gives in quotes
// («СтройИнвест» of ООО «СтройИнвест», "Northwind" of TOO "Northwind"), or its
// distinctive words from the first of them on, as long as they run -
// "Meadowbrook" of "Meadowbrook Industrial Partners LLC", "Brightline" of
// "Brightline Foods, Inc.". Empty when it has none, or when that is the whole
// name as written.
std::string short_company_name(const std::string &name) {
	const auto trimmed = [](std::string value) {
		while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
		while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
		return value;
	};
	const std::string whole = trimmed(name);
	static const std::vector<std::pair<std::string, std::string>> quotes{
		{"\xC2\xAB", "\xC2\xBB"}, {"\xE2\x80\x9C", "\xE2\x80\x9D"}, {"\xE2\x80\x9E", "\xE2\x80\x9C"}, {"\"", "\""}};
	for (const auto &[open, close] : quotes) {
		const std::size_t opened = whole.find(open);
		if (opened == std::string::npos) continue;
		const std::size_t closed = whole.find(close, opened + open.size());
		if (closed == std::string::npos) continue;
		const std::string inner = trimmed(whole.substr(opened + open.size(), closed - opened - open.size()));
		const std::vector<utf8proc_int32_t> points = code_points(inner);
		const std::size_t letters = static_cast<std::size_t>(std::count_if(points.begin(), points.end(), is_letter));
		if (letters < 4 || inner == whole || points.empty() || !is_capital(points.front()) || legal_form_word(inner)) return {};
		return inner;
	}
	std::vector<std::pair<std::size_t, std::size_t>> words;
	for (const WordSpan &span : word_spans(whole)) words.emplace_back(span.start, span.end);
	std::size_t first = 0;
	while (first < words.size() && !distinctive_company_word(whole.substr(words[first].first, words[first].second - words[first].first)))
		++first;
	if (first == words.size()) return common_words_short_name(whole, words);
	std::size_t last = first;
	while (last + 1 < words.size()) {
		const std::string between = whole.substr(words[last].second, words[last + 1].first - words[last].second);
		if (between != " " && between != "-") break;
		if (!distinctive_company_word(whole.substr(words[last + 1].first, words[last + 1].second - words[last + 1].first))) break;
		++last;
	}
	// The words after the short name only say what kind of company it is, where
	// and in what form - "Industrial Partners LLC", "Foods, Inc." - or the name
	// runs on to its last word that does not: "Johnson Controls" of "Johnson
	// Controls International plc", "Stanley Black & Decker" of "Stanley Black &
	// Decker, Inc.". "Johnson" and "Stanley" alone are a person's names, and
	// "Johnson signed as witness" once went out as the company's short part.
	for (std::size_t next = last + 1; next < words.size(); ++next) {
		const std::string word = whole.substr(words[next].first, words[next].second - words[next].first);
		if (generic_company_word(word) || legal_form_word(word) || is_public_jurisdiction(word)) continue;
		for (std::size_t joined = last + 1; joined <= next; ++joined) {
			const std::string between = whole.substr(words[joined - 1].second, words[joined].first - words[joined - 1].second);
			if (between != " " && between != "-" && between != " & ") return {};
		}
		last = next;
	}
	const std::string shortName = whole.substr(words[first].first, words[last].second - words[first].first);
	return shortName == whole ? std::string() : shortName;
}

// The short name a text defines for a party right after a writing of its full
// name - 'Corvane Fulfillment Group, Inc. ("Corvane")', 'Brookmere Ward
// Logistics LLC, a Delaware limited liability company (hereinafter
// "Brookmere")', '("Lumenfield" or the "Processor")' - when every word of it is
// a word of the name, in the name's order, one of them more than a word for
// what a company is ("Group", "Bank"), and the text does not define it as a
// role. The text's own definition outranks what the name's words suggest:
// read from them, "Corvane Fulfillment Group, Inc." was "Corvane
// Fulfillment", and "Corvane has not paid" went out readable (round 5). Empty
// when no such definition follows.
std::string defined_short_name_after(const std::string &text, std::size_t end, const std::string &name) {
	std::size_t cursor = end;
	// The full stop of a legal form the writing stops before: "Inc" of "Inc.".
	if (cursor < text.size() && text[cursor] == '.' && end > 0 && std::isalpha(static_cast<unsigned char>(text[end - 1])) != 0)
		++cursor;
	// Over what the party is - ", a Delaware limited liability company" - to the bracket.
	if (text.compare(cursor, 4, ", a ") == 0 || text.compare(cursor, 5, ", an ") == 0) {
		const std::size_t bracket = text.find('(', cursor);
		if (bracket == std::string::npos || bracket - cursor > 120) return {};
		const std::string between = text.substr(cursor, bracket - cursor);
		if (between.find_first_of("\"\n;:)") != std::string::npos || between.find("\xe2\x80\x9c") != std::string::npos)
			return {};
		cursor = bracket;
	}
	while (cursor < text.size() && text[cursor] == ' ') ++cursor;
	if (cursor >= text.size() || text[cursor] != '(') return {};
	++cursor;
	// The words a definition may open with: '(hereinafter "...")', '(referred to as "...")'.
	static const std::set<std::string> opening{"hereinafter", "herein", "referred", "to", "as", "called"};
	for (int words = 0; words < 4; ++words) {
		std::size_t wordEnd = cursor;
		while (wordEnd < text.size() && std::isalpha(static_cast<unsigned char>(text[wordEnd])) != 0) ++wordEnd;
		if (wordEnd == cursor) break;
		std::string word = text.substr(cursor, wordEnd - cursor);
		for (char &character : word) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		if (opening.count(word) == 0) break;
		cursor = wordEnd;
		while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == ',')) ++cursor;
	}
	static const std::pair<std::string, std::string> quotes[] = {{"\"", "\""}, {"\xe2\x80\x9c", "\xe2\x80\x9d"}};
	for (const auto &[open, close] : quotes) {
		if (text.compare(cursor, open.size(), open) != 0) continue;
		const std::size_t from = cursor + open.size();
		const std::size_t to = text.find(close, from);
		if (to == std::string::npos || to - from < 2 || to - from > 60) return {};
		const std::string term = text.substr(from, to - from);
		if (term.find_first_of("\n\t\"()") != std::string::npos || term == name) return {};
		// Then the bracket closes, or another term of the same definition follows.
		const std::size_t after = to + close.size();
		if (after >= text.size() || (text[after] != ')' && text[after] != ',' && text.compare(after, 4, " or ") != 0
				&& text.compare(after, 5, " and ") != 0))
			return {};
		const std::vector<utf8proc_int32_t> points = code_points(term);
		if (points.empty() || !is_capital(points.front())) return {};
		// Every word of it a word of the name, in the name's order.
		const std::vector<WordSpan> termWords = word_spans(term);
		const std::vector<WordSpan> nameWords = word_spans(name);
		if (termWords.empty()) return {};
		std::size_t next = 0;
		bool distinct = false;
		for (const WordSpan &word : termWords) {
			const std::string written = term.substr(word.start, word.end - word.start);
			while (next < nameWords.size()
					&& name.compare(nameWords[next].start, nameWords[next].end - nameWords[next].start, written) != 0)
				++next;
			if (next == nameWords.size()) return {};
			++next;
			std::string lowered = written;
			for (char &character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			if (!generic_company_word(written) && !legal_form_word(written) && !is_public_jurisdiction(written)
					&& !only_institution_kind_words(lowered))
				distinct = true;
		}
		if (!distinct || defined_as_role(text, term)) return {};
		return term;
	}
	return {};
}

// The first word of a company's name that could name it alone
// (distinctive_company_word): "Ferncastle" of "Ferncastle Sensors Holdings
// LLC". Empty when it has none, or when that word is the whole name.
std::string first_distinctive_company_word(const std::string &name) {
	for (const WordSpan &span : word_spans(name)) {
		const std::string word = name.substr(span.start, span.end - span.start);
		if (distinctive_company_word(word)) return word == name ? std::string() : word;
	}
	return {};
}

// Whether the text writes `word` as a word inside a quoted label of a few
// words - a wire's reference "Ferncastle closing", a file's name "Ferncastle
// diligence index v3" - where a company is written by its name, never by a
// person's act.
bool quoted_label_holds(const std::string &text, const std::string &word) {
	static const std::pair<std::string, std::string> quotes[] = {{"\"", "\""}, {"\xe2\x80\x9c", "\xe2\x80\x9d"}};
	for (const auto &[open, close] : quotes) {
		for (std::size_t at = text.find(open); at != std::string::npos; ) {
			const std::size_t from = at + open.size();
			const std::size_t to = text.find(close, from);
			if (to == std::string::npos) break;
			const std::string inner = text.substr(from, to - from);
			if (!inner.empty() && inner.size() <= 60 && inner.find_first_of("\n\t") == std::string::npos
					&& word_spans(inner).size() <= 6)
				for (std::size_t position = inner.find(word); position != std::string::npos;
						position = inner.find(word, position + 1))
					if (stands_as_word(inner, position, word.size())) return true;
			at = text.find(open, to + close.size());
		}
	}
	return false;
}

// A company's name as it stands without its legal form: "Northwind Freight
// Systems" of "Northwind Freight Systems LLC", "Harbor Point Logistics" of
// "Harbor Point Logistics, Inc.". Empty when it ends in no legal form, or is
// nothing else.
std::string name_without_legal_form(const std::string &name) {
	const std::vector<WordSpan> spans = word_spans(name);
	std::size_t kept = spans.size();
	while (kept > 0 && legal_form_word(name.substr(spans[kept - 1].start, spans[kept - 1].end - spans[kept - 1].start)))
		--kept;
	if (kept == spans.size() || kept == 0) return {};
	std::string base = name.substr(0, spans[kept - 1].end);
	while (!base.empty() && (base.back() == ',' || base.back() == ' ')) base.pop_back();
	return base;
}

// Whether capitalised words from text[end] on run into a legal form -
// "Brightline Capital LLC" - so that the word before `end` is the first of
// another company's full name, no writing of the one whose short name it is.
// A legal form right after it ("Brightline, Inc.") names no other company: that
// is the same name written with its form.
bool opens_another_company_name(const std::string &text, std::size_t end) {
	const std::string rest = text.substr(end, 160);
	const std::vector<WordSpan> words = word_spans(rest);
	std::size_t previousEnd = 0;
	for (std::size_t next = 0; next < words.size() && next < 5; ++next) {
		const std::string between = rest.substr(previousEnd, words[next].start - previousEnd);
		if (between != " " && between != ", " && between != " & " && between != "\xC2\xA0") return false;
		const std::string word = rest.substr(words[next].start, words[next].end - words[next].start);
		if (legal_form_word(word)) return next > 0;
		if (!words[next].capitalised) return false;
		previousEnd = words[next].end;
	}
	return false;
}

// Every short name this operation's organisations are known by, with the rows
// that could each be: one row, or two that share it.
std::map<std::string, std::vector<std::size_t>> short_name_owners(const OperationProtectedSymbolTable &symbols) {
	std::map<std::string, std::vector<std::size_t>> owners;
	for (std::size_t row = 0; row < symbols.mapping.size(); ++row) {
		const RequestMappingEntry &entry = symbols.mapping[row];
		if (!organisation_type(entry.type)) continue;
		for (const std::string &writing : entry.every_form()) {
			// Read once per writing in an operation: every passage asks again.
			const std::string key = "\x02" + writing;
			auto cached = symbols.comparison_form_cache.find(key);
			if (cached == symbols.comparison_form_cache.end())
				cached = symbols.comparison_form_cache.emplace(key, short_company_name(writing)).first;
			const std::string &shortName = cached->second;
			if (shortName.empty()) continue;
			std::vector<std::size_t> &rows = owners[shortName];
			if (std::find(rows.begin(), rows.end(), row) == rows.end()) rows.push_back(row);
		}
		// And the short names already recorded on it, a text's own definition
		// among them (defined_short_name_after).
		for (const std::string &form : entry.short_forms) {
			std::vector<std::size_t> &rows = owners[form];
			if (std::find(rows.begin(), rows.end(), row) == rows.end()) rows.push_back(row);
		}
	}
	return owners;
}

// The one organisation row a short name is, when exactly one here is known by it.
std::optional<std::size_t> short_name_owner(const std::map<std::string, std::vector<std::size_t>> &owners,
		const std::string &value) {
	const auto found = owners.find(value);
	if (found == owners.end() || found->second.size() != 1) return std::nullopt;
	return found->second.front();
}

bool written_as_short_name(const RequestMappingEntry &entry, const std::string &value) {
	return organisation_type(entry.type) && value != entry.original
		&& std::find(entry.short_forms.begin(), entry.short_forms.end(), value) != entry.short_forms.end();
}

// Whether `value` is the name a row's organisation is known by alone, read
// from one of its own writings.
bool short_name_of_row(const RequestMappingEntry &entry, const std::string &value) {
	if (!organisation_type(entry.type) || value.empty() || value == entry.original) return false;
	for (const std::string &writing : entry.every_form())
		if (short_company_name(writing) == value) return true;
	return false;
}

// A row's symbol for its short name: "[[E3]]" -> "[[E3.short]]".
std::string short_symbol(const std::string &placeholder) {
	return placeholder.substr(0, placeholder.size() - 2) + ".short]]";
}

// A person's first name or surname written alone, sent as that part of the
// name ([[E5.first]], [[E5.last]]) and written back as it was written, as an
// organisation's short name is: "Hi Evelyn" is not "Hi Evelyn Hart". A model
// shown the whole name's symbol where the text had the first name writes the
// whole name back (a colleague's chat, 2026-10-26). Nothing for any other
// writing, or for a name with no first name and surname to tell apart.
std::optional<std::string> name_part_symbol(const RequestMappingEntry &entry, const std::string &value) {
	if (!person_type(entry.type) || value.empty() || value == entry.original) return std::nullopt;
	const std::optional<PersonNameParts> parts = person_name_parts(entry);
	if (!parts || parts->first.empty() || parts->last.empty()) return std::nullopt;
	const std::string stem = entry.placeholder.substr(0, entry.placeholder.size() - 2);
	if (value == parts->first) return stem + ".first]]";
	if (value == parts->last) return stem + ".last]]";
	return std::nullopt;
}

// The symbol a row is written with where `value` stands for it: its short
// name's part, a name's part, or the whole.
std::string symbol_as_written(const RequestMappingEntry &entry, const std::string &value) {
	if (written_as_short_name(entry, value)) return short_symbol(entry.placeholder);
	if (const std::optional<std::string> part = name_part_symbol(entry, value)) return *part;
	return entry.placeholder;
}

// The next symbol this operation issues, never one the text already writes:
// typed ([[Amount3]], numbered per `prefix` - markers::prefix_for) or compact
// ([[E17]], one numbering for the whole table), as the table writes them.
std::string next_placeholder(OperationProtectedSymbolTable &symbols, std::set<std::string> &issuedSymbols,
		const std::string &text, const std::string &prefix,
		const std::optional<std::string> &project_entity_id = std::nullopt) {
	std::string placeholder;
	if (symbols.marker_style == markers::MarkerStyle::Typed && !prefix.empty()) {
		// A Project identity stands under its own symbol in every request of
		// the Project (D-235), while that symbol is free here.
		if (project_entity_id) {
			const auto own = symbols.project_entity_symbols.find(*project_entity_id);
			if (own != symbols.project_entity_symbols.end() && own->second.rfind("[[" + prefix, 0) == 0
					&& issuedSymbols.find(own->second) == issuedSymbols.end() && text.find(own->second) == std::string::npos) {
				issuedSymbols.insert(own->second);
				return own->second;
			}
		}
		const auto isReserved = [&](const std::string &candidate) {
			for (const auto &[entityId, symbol] : symbols.project_entity_symbols)
				if (symbol == candidate) return true;
			return false;
		};
		std::size_t &number = symbols.numbers_by_prefix[prefix];
		do placeholder = markers::format(prefix, ++number);
		while (issuedSymbols.find(placeholder) != issuedSymbols.end() || isReserved(placeholder));
	} else {
		std::size_t markerIndex = issuedSymbols.size() + 1;
		placeholder = compact_marker(symbols.marker_family, markerIndex);
		while (issuedSymbols.find(placeholder) != issuedSymbols.end())
			placeholder = compact_marker(symbols.marker_family, ++markerIndex);
	}
	if (text.find(placeholder) != std::string::npos)
		throw std::runtime_error("source text collides with a generated protected symbol");
	issuedSymbols.insert(placeholder);
	return placeholder;
}

const char kOperandLabel[] = "statement_operand";
const char kOperandSymbolLabel[] = "statement_operand_symbol";
// An earlier answer's fact anchor (⟪writing|fact:key⟫), read as one value.
const char kFactAnchorLabel[] = "fact_anchor";

// Whether an operand is one of this operation's own symbols: "⟦[[E3]]⟧".
bool operand_is_symbol(const OperationProtectedSymbolTable &symbols, const std::string &inner) {
	return std::any_of(symbols.mapping.begin(), symbols.mapping.end(),
		[&](const RequestMappingEntry &entry) { return entry.placeholder == inner; });
}

// Every ⟦…⟧ of a text as one protected operand, the marks included in its span
// so they never leave: "⟦$2.8 million⟧" is sent as its symbol alone. Typed by
// the value it reads as; a plain decimal below one is the rate of that
// fraction when this operation holds one (a cell's 0.001 for 0,1 %).
std::vector<CanonicalEntity> operand_entities(const std::vector<OperandSpan> &operands,
		const OperationProtectedSymbolTable &symbols) {
	std::vector<CanonicalEntity> entities;
	for (const OperandSpan &operand : operands) {
		CanonicalEntity entity{operand.start, operand.end - operand.start, std::nullopt, 1.0, DetectionSource::Deterministic,
			kOperandLabel, operand.inner, {}};
		if (operand_is_symbol(symbols, operand.inner)) {
			entity.source_label = kOperandSymbolLabel;
			entities.push_back(std::move(entity));
			continue;
		}
		// "⟦$2.8 million|fact:…⟧": an operand an earlier answer anchored to its fact.
		if (const std::optional<AnchoredWriting> anchored = anchored_writing(operand.inner)) {
			entity.value = anchored->writing;
			entity.fact_key = anchored->key;
		}
		if (const std::optional<semantic::Value> value = operand_value(entity.value)) {
			entity.type = type_of_value(*value);
			entity.value_key = semantic::value_key(*value);
			if (value->kind == semantic::Kind::Number && value->amount > semantic::Rational(0)
					&& value->amount < semantic::Rational(1)) {
				const std::string rateKey = semantic::value_key(semantic::make_percentage(value->amount));
				if (std::any_of(symbols.mapping.begin(), symbols.mapping.end(),
						[&](const RequestMappingEntry &entry) { return entry.value_key == rateKey; })) {
					entity.value_key = rateKey;
					entity.type = EntityType::Percentage;
				}
			}
		}
		entities.push_back(std::move(entity));
	}
	return entities;
}

// The value an operand's row stands for, as the operand writes it.
std::optional<semantic::Value> value_of_operand(const CanonicalEntity &operand) {
	std::optional<semantic::Value> value = operand_value(operand.value);
	if (value && value->kind == semantic::Kind::Number && operand.type == EntityType::Percentage)
		value = semantic::make_percentage(value->amount);
	return value;
}

// The row an operand is by its writing rather than its value: a spelling a row
// already has, or one value this operation knows that is the whole operand.
std::optional<std::size_t> operand_row_by_writing(const std::string &inner, const OperationProtectedSymbolTable &symbols,
		const MappingIndex &index) {
	for (std::size_t row = 0; row < symbols.mapping.size(); ++row) {
		const RequestMappingEntry &entry = symbols.mapping[row];
		const std::vector<std::string> forms = entry.every_form();
		if (std::find(forms.begin(), forms.end(), inner) != forms.end()
				|| std::find(entry.surname_forms.begin(), entry.surname_forms.end(), inner) != entry.surname_forms.end())
			return row;
	}
	std::vector<CanonicalEntity> known = known_operation_entities(inner, symbols);
	std::vector<CanonicalEntity> project = known_project_entities(inner, symbols);
	known.insert(known.end(), project.begin(), project.end());
	known = reconcile(inner, std::move(known));
	if (known.size() != 1 || known.front().start != 0 || known.front().length != inner.size()) return std::nullopt;
	return index.row_of(known.front(), project_entity_for_value(symbols, known.front().value));
}

// Replaces every value of `entities` that overlaps an operand with the operands
// themselves, in text order.
void put_operands_in(std::vector<CanonicalEntity> &entities, const std::string &text,
		const OperationProtectedSymbolTable &symbols) {
	const std::vector<OperandSpan> operands = operand_spans(text);
	if (operands.empty()) return;
	entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const CanonicalEntity &entity) {
		return overlaps_operand(operands, entity.start, entity.length);
	}), entities.end());
	for (CanonicalEntity &operand : operand_entities(operands, symbols)) entities.push_back(std::move(operand));
	std::sort(entities.begin(), entities.end(), [](const auto &left, const auto &right) {
		if (left.start != right.start) return left.start < right.start;
		return left.length > right.length;
	});
}

// A quantity's value, read where it was found; a rate the checkpoint tagged as
// money becomes the rate it is.
std::optional<semantic::Value> read_entity_value(const std::string &text, CanonicalEntity &entity) {
	// An operand, or an anchored writing, is read from its own writing - the
	// span it stands in holds its marks too.
	if (entity.source_label == kOperandLabel || entity.source_label == kFactAnchorLabel) return value_of_operand(entity);
	if (entity.source_label == kOperandSymbolLabel) return std::nullopt;
	std::optional<semantic::Value> value = written_value(text, entity.start, entity.length, entity.type);
	// A day the date reading found in a shape no value reading takes - "03.03.26",
	// "3-Mar-2026", "Notice_20260210" - is the day its key names.
	if (entity.type == EntityType::Date && entity.value_key.compare(0, 7, "DATE:D:") == 0 && entity.value_key.size() == 15
			&& (!value || semantic::value_key(*value) != entity.value_key)) {
		const std::string digits = entity.value_key.substr(7);
		value = semantic::parse_value(digits.substr(0, 4) + "-" + digits.substr(4, 2) + "-" + digits.substr(6, 2),
			semantic::ParseHints{semantic::Kind::Date, "", false});
		if (value && semantic::value_key(*value) != entity.value_key) value.reset();
	}
	// And a month so found - "Mar-2026" - the month its key names.
	if (entity.type == EntityType::Date && entity.value_key.compare(0, 7, "DATE:M:") == 0 && entity.value_key.size() == 13
			&& (!value || semantic::value_key(*value) != entity.value_key)) {
		static const char *names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September",
			"October", "November", "December"};
		const int month = std::atoi(entity.value_key.substr(11, 2).c_str());
		value.reset();
		if (month >= 1 && month <= 12) {
			value = semantic::parse_value(std::string(names[month - 1]) + " " + entity.value_key.substr(7, 4),
				semantic::ParseHints{semantic::Kind::Date, "", false});
			if (value && semantic::value_key(*value) != entity.value_key) value.reset();
		}
	}
	// A rate in Russian fraction words («одна десятая процента»), found as a
	// writing of a held rate: the value its key names, read from its own words.
	if (!value && !entity.value_key.empty()) {
		const std::optional<semantic::Value> fraction = russian_fraction_rate(text.substr(entity.start, entity.length));
		if (fraction && semantic::value_key(*fraction) == entity.value_key) value = fraction;
	}
	if (!value) return std::nullopt;
	if (entity.value_key.empty()) entity.value_key = semantic::value_key(*value);
	if (entity.type == EntityType::MoneyAmount && value->kind == semantic::Kind::Percentage) entity.type = EntityType::Percentage;
	if (entity.type == EntityType::MoneyAmount && (value->kind == semantic::Kind::Duration || value->kind == semantic::Kind::Count))
		entity.type = EntityType::Number;
	if (entity.type == EntityType::Percentage && value->kind == semantic::Kind::Money) entity.type = EntityType::MoneyAmount;
	return value;
}

// The honorific written right before a person's name: "Mr." of "Mr. Marcus
// Delgado", written as a salutation writes it. Empty when there is none.
std::string honorific_before(const std::string &text, std::size_t start) {
	static const std::vector<std::pair<const char *, const char *>> honorifics{
		{"Mr. ", "Mr."}, {"Ms. ", "Ms."}, {"Mrs. ", "Mrs."}, {"Dr. ", "Dr."}, {"Mr ", "Mr."}, {"Ms ", "Ms."},
		{"Mrs ", "Mrs."}, {"Dr ", "Dr."},
	};
	for (const auto &[written, honorific] : honorifics) {
		const std::size_t size = std::char_traits<char>::length(written);
		if (start < size || text.compare(start - size, size, written) != 0) continue;
		if (start > size && continues_word_at(text, start - size - 1)) continue;
		return honorific;
	}
	return {};
}

// Gives the people a text names in full their rows before anything else is
// judged, so a surname, initials or a first name written alone is weighed
// against every person the batch names - «Кузнецова» read before the passage
// that names Анна Павловна Кузнецова is never taken for Дмитрий Кузнецов's
// genitive because her row did not exist yet.
void register_full_names(const std::string &text, const std::vector<CanonicalEntity> &candidates,
		OperationProtectedSymbolTable &symbols, bool user_authored) {
	std::vector<CanonicalEntity> entities = reconcile(text, candidates);
	MappingIndex index(symbols);
	std::set<std::string> issuedSymbols;
	for (const auto &entry : symbols.mapping) issuedSymbols.insert(entry.placeholder);
	for (const CanonicalEntity &entity : entities) {
		// And every organisation named in full, so its name written alone in
		// another passage is weighed against it ("Meadowbrook" read before the
		// passage that names Meadowbrook Industrial Partners LLC).
		const bool organisationInFull = organisation_type(entity.type) && !short_company_name(entity.value).empty();
		if (!written_in_full(entity) && !organisationInFull) continue;
		const ProjectEntityResolution projectEntity = project_entity_for_value(symbols, entity.value);
		if (index.row_of(entity, projectEntity)) continue;
		RequestMappingEntry created{next_placeholder(symbols, issuedSymbols, text,
				markers::prefix_for(entity.type, std::nullopt, entity.source), projectEntity.entity_id), entity.value, entity.type, entity.source,
			projectEntity.entity_id};
		created.original_is_user_authored = user_authored;
		symbols.mapping.push_back(std::move(created));
		index.add(symbols.mapping.size() - 1);
	}
}

// Whether text[start, end) is part of a file's name: on its line, before a tab,
// a quotation mark or a table's bar, the text reaches a document's extension -
// "Договор поручительства № ФП-044-П.docx", a document list's "name":"…".
// A file's name writes a code the way a file system lets it ("/" becomes
// "-"), which is never how the document itself writes it.
bool in_file_name(const std::string &text, std::size_t start, std::size_t end) {
	static const std::array<const char *, 12> extensions{".docx", ".xlsx", ".xlsm", ".xls", ".pdf", ".md", ".txt",
		".eml", ".msg", ".doc", ".rtf", ".csv"};
	const std::size_t limit = std::min(text.size(), end + 160);
	for (std::size_t cursor = end; cursor < limit; ++cursor) {
		const char character = text[cursor];
		if (character == '\n' || character == '\t' || character == '"' || character == '|') return false;
		if (text.compare(cursor, 2, "\xC2\xAB") == 0 || text.compare(cursor, 2, "\xC2\xBB") == 0) return false;
		if (character != '.') continue;
		for (const char *extension : extensions) {
			const std::size_t size = std::char_traits<char>::length(extension);
			if (text.compare(cursor, size, extension) != 0) continue;
			const std::size_t after = cursor + size;
			if (after >= text.size() || std::isalnum(static_cast<unsigned char>(text[after])) == 0) return true;
		}
	}
	static_cast<void>(start);
	return false;
}

// Where a writing of a value was met: a document's text, a file's name, the
// lawyer's own request, a model's earlier answer (RequestMappingEntry::
// writing_origins). Which writing is restored depends on it.
unsigned writing_origin(const std::string &text, std::size_t start, std::size_t length, bool user_authored,
		bool model_authored) {
	if (in_file_name(text, start, start + length)) return RequestMappingEntry::FileName;
	if (user_authored) return RequestMappingEntry::LawyerRequest;
	if (model_authored) return RequestMappingEntry::ModelAuthored;
	return RequestMappingEntry::DocumentText;
}

// ---------------------------------------------------------------------------
// The fact registry (DESIGN3 9.3): which fact of the material an amount, a
// rate, a count, a date or a period written at one place is.
//
// A symbol for a quantity or a date names a FACT - the purchase price, the
// liability cap, the July rent of a ledger - not a value. Four facts of $5m
// are four symbols; that their values are equal is a relation Singularity knows
// here and sends only when a task needs it. Several writings are one fact only
// where the material proves it: one clause restating its own figure, a cell
// and a reference to that cell, the same defined term, the same statement
// read twice. A person or an organisation keeps its real-world identity
// (MappingIndex below); only quantities and dates are facts.

// Where a text a request carries comes from, and its digest: what a fact's
// place names (FactProvenance::origin, passage_sha256).
struct FactContext {
	FactProvenance::Origin origin = FactProvenance::Origin::Source;
	// SHA-256 of the text exactly as the request carries it.
	std::string digest;
};

std::string sixteen_hex(const std::string &value) {
	return platform::sha256_hex_digest(value).substr(0, 16);
}

// A fact's key: sixteen hex digits of the digest of where and in what role the
// material states it. Never a value, never readable words.
std::string fact_key_of(const FactProvenance &provenance) {
	return sixteen_hex(std::string(to_string(provenance.origin)) + "|" + provenance.passage_sha256 + "|"
		+ std::to_string(provenance.offset) + "+" + std::to_string(provenance.length) + "|" + provenance.cell + "|"
		+ provenance.role + "|" + provenance.derivation);
}

// Every ⟪writing|fact:key⟫ of a text.
struct AnchorMarkSpan {
	std::size_t start = 0;
	std::size_t end = 0;
	AnchoredWriting parts;
};

std::vector<AnchorMarkSpan> anchor_mark_spans(const std::string &text) {
	std::vector<AnchorMarkSpan> spans;
	const std::string open(fact_anchor_marks::kOpen);
	const std::string close(fact_anchor_marks::kClose);
	for (std::size_t at = text.find(open); at != std::string::npos; at = text.find(open, at + 1)) {
		const std::size_t innerStart = at + open.size();
		const std::size_t end = text.find(close, innerStart);
		if (end == std::string::npos || end - innerStart > 400) continue;
		const std::string inner = text.substr(innerStart, end - innerStart);
		if (inner.find('\n') != std::string::npos || inner.find(open) != std::string::npos) continue;
		const std::optional<AnchoredWriting> parts = anchored_writing(inner);
		if (!parts) continue;
		spans.push_back({at, end + close.size(), *parts});
		at = end;
	}
	return spans;
}

// Every ⟪writing|fact:key⟫ of a text as one value, the marks included in its
// span so they never leave: the writing is read for its value, and the fact
// the key names is the symbol it goes as.
// A symbol-shaped token - an issued fact's [[Amount3]], a protocol handle's
// [[Calendar1]], [[Table2]], [[Schedule1]] - is never a value of the lawyer's:
// nothing found inside one is protected (a fake-provider run: "Calendar1" in
// Singularity's own result line was read as a date and sent as [[[[Date49]]]]).
void drop_inside_symbol_tokens(std::vector<CanonicalEntity> &entities, const std::string &text) {
	std::vector<std::pair<std::size_t, std::size_t>> tokens;
	for (std::size_t at = text.find("[["); at != std::string::npos; at = text.find("[[", at + 2)) {
		std::size_t cursor = at + 2;
		const std::size_t letters = cursor;
		while (cursor < text.size() && std::isalpha(static_cast<unsigned char>(text[cursor])) != 0) ++cursor;
		if (cursor == letters) continue;
		const std::size_t digits = cursor;
		while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) ++cursor;
		if (cursor == digits) continue;
		if (cursor < text.size() && text[cursor] == '.') {
			const std::size_t part = ++cursor;
			while (cursor < text.size() && std::islower(static_cast<unsigned char>(text[cursor])) != 0) ++cursor;
			if (cursor == part) continue;
		}
		if (text.compare(cursor, 2, "]]") != 0) continue;
		tokens.emplace_back(at, cursor + 2);
	}
	if (tokens.empty()) return;
	entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const CanonicalEntity &entity) {
		if (entity.source_label == kOperandSymbolLabel) return false;
		return std::any_of(tokens.begin(), tokens.end(), [&](const std::pair<std::size_t, std::size_t> &token) {
			return entity.start < token.second && token.first < entity.start + entity.length;
		});
	}), entities.end());
}

void put_anchor_marks_in(std::vector<CanonicalEntity> &entities, const std::string &text) {
	const std::vector<AnchorMarkSpan> marks = anchor_mark_spans(text);
	if (marks.empty()) return;
	entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const CanonicalEntity &entity) {
		return std::any_of(marks.begin(), marks.end(), [&](const AnchorMarkSpan &mark) {
			return entity.start < mark.end && mark.start < entity.start + entity.length;
		});
	}), entities.end());
	for (const AnchorMarkSpan &mark : marks) {
		CanonicalEntity entity{mark.start, mark.end - mark.start, std::nullopt, 1.0, DetectionSource::Deterministic,
			kFactAnchorLabel, mark.parts.writing, {}};
		entity.fact_key = mark.parts.key;
		if (const std::optional<semantic::Value> value = operand_value(mark.parts.writing)) {
			entity.type = type_of_value(*value);
			entity.value_key = semantic::value_key(*value);
		}
		entities.push_back(std::move(entity));
	}
	std::sort(entities.begin(), entities.end(), [](const auto &left, const auto &right) {
		if (left.start != right.start) return left.start < right.start;
		return left.length > right.length;
	});
}

// Whether two values could be one fact's: the same kind and amount (a date the
// same day and precision), and money in the same currency - or one of them a
// bare figure whose currency the text does not say.
bool values_agree(const semantic::Value &left, const std::string &leftKey, const semantic::Value &right,
		const std::string &rightKey) {
	if (leftKey == rightKey) return true;
	// A count written as a bare figure is that count: "2" of the two rows Singularity
	// counted. Only a fact's own key reaches a count (no value index holds
	// one), so this names it again and never finds it anywhere else (a
	// fake-provider run: the answer check's anchor of [[Count1]] became a new
	// [[Number3]], then [[Number9]]).
	if ((left.kind == semantic::Kind::Count || right.kind == semantic::Kind::Count)
			&& (left.kind == semantic::Kind::Count || left.kind == semantic::Kind::Number)
			&& (right.kind == semantic::Kind::Count || right.kind == semantic::Kind::Number))
		return amount_of_key(leftKey) == amount_of_key(rightKey);
	const bool leftBare = left.kind == semantic::Kind::Number || currency_unknown(left);
	const bool rightBare = right.kind == semantic::Kind::Number || currency_unknown(right);
	if (left.kind == semantic::Kind::Money || right.kind == semantic::Kind::Money)
		return (leftBare || rightBare) && amount_of_key(leftKey) == amount_of_key(rightKey)
			&& (left.kind == semantic::Kind::Money || leftBare) && (right.kind == semantic::Kind::Money || rightBare);
	return false;
}

bool row_value_agrees(const RequestMappingEntry &entry, const semantic::Value &value, const std::string &key) {
	return entry.value && !entry.value_key.empty() && values_agree(*entry.value, entry.value_key, value, key);
}

// The sentence (or table line) a span stands in: from the end of the previous
// sentence - a full stop, "!", "?" or ";" before a space, or a line break - to
// the end of its own.
std::pair<std::size_t, std::size_t> sentence_around(const std::string &text, std::size_t start, std::size_t end) {
	// A full stop that closes an abbreviation - "i.e.", "No.", "Inc.", «г.»,
	// «т.е.», «руб.» - ends no sentence.
	const auto abbreviation = [&](std::size_t stop) {
		std::size_t from = stop;
		while (from > 0 && text[from - 1] != ' ' && text[from - 1] != '\n' && text[from - 1] != '(') --from;
		std::string word = platform::folded_text(text.substr(from, stop - from));
		if (word.empty()) return false;
		if (word.find('.') != std::string::npos) return true;
		static const std::set<std::string> known{"no", "nos", "inc", "ltd", "co", "corp", "mr", "ms", "mrs", "dr", "st", "vs",
			"art", "sec", "para", "cl", "approx", "\xD0\xB3", "\xD0\xB3\xD0\xB3", "\xD0\xBF", "\xD1\x81\xD1\x82",
			"\xD1\x80\xD1\x83\xD0\xB1", "\xD0\xBA\xD0\xBE\xD0\xBF", "\xD1\x82\xD1\x8B\xD1\x81",
			"\xD0\xBC\xD0\xBB\xD0\xBD", "\xD0\xBC\xD0\xBB\xD1\x80\xD0\xB4", "\xD0\xB4"};
		return known.count(word) > 0 || (word.size() == 1 && std::isalpha(static_cast<unsigned char>(word[0])) != 0);
	};
	const auto ends = [&](std::size_t punctuation) {
		const char character = text[punctuation];
		if (character != '.' && character != '!' && character != '?' && character != ';') return false;
		if (!(punctuation + 1 >= text.size() || text[punctuation + 1] == ' ' || text[punctuation + 1] == '\n')) return false;
		if (character == ';') {
			// Inside a bracket a semicolon parts a list, not sentences:
			// «3 марта 2026 г. (03.03.2026 г.; 2026-03-03)».
			const std::size_t opened = text.rfind('(', punctuation);
			const std::size_t line = text.rfind('\n', punctuation);
			if (opened != std::string::npos && (line == std::string::npos || opened > line)
					&& text.find(')', opened) > punctuation)
				return false;
		}
		return character != '.' || !abbreviation(punctuation);
	};
	std::size_t from = std::min(start, text.size());
	while (from > 0 && text[from - 1] != '\n' && !(from >= 2 && ends(from - 2) && text[from - 1] == ' ')) --from;
	std::size_t to = std::min(end, text.size());
	while (to < text.size() && text[to] != '\n' && !ends(to)) ++to;
	return {from, to};
}

// A word of a role folded as it is compared: lower case, a possessive let go,
// a Russian word's ending cut to its stem («Цены Договора» and «Цена Договора»
// are one role).
std::string role_word(const std::string &word) {
	std::string folded = platform::folded_text(word);
	for (const char *possessive : {"'s", "\xE2\x80\x99s"}) {
		const std::string suffix(possessive);
		if (folded.size() > suffix.size() && folded.compare(folded.size() - suffix.size(), suffix.size(), suffix) == 0)
			folded.erase(folded.size() - suffix.size());
	}
	if (has_cyrillic_letter(folded)) {
		std::vector<utf8proc_int32_t> points = code_points(folded);
		static const std::set<utf8proc_int32_t> endings{0x430, 0x435, 0x438, 0x43E, 0x443, 0x44B, 0x44C, 0x44E, 0x44F,
			0x439, 0x451, 0x43C, 0x445};
		std::size_t cut = 0;
		while (points.size() > 4 && cut < 2 && endings.count(points.back()) > 0) {
			points.pop_back();
			++cut;
		}
		std::string stem;
		for (const utf8proc_int32_t point : points) {
			utf8proc_uint8_t encoded[4] = {0, 0, 0, 0};
			const utf8proc_ssize_t written = utf8proc_encode_char(point, encoded);
			stem.append(reinterpret_cast<const char *>(encoded), static_cast<std::size_t>(written));
		}
		return stem;
	}
	return folded;
}

bool role_filler_word(const std::string &folded) {
	static const std::set<std::string> fillers{"the", "a", "an", "this", "that", "such", "its", "their", "his", "her",
		"our", "your", "any", "each", "all", "of", "in", "on", "for", "to", "by", "and", "or", "with", "at", "as",
		"is", "be", "was", "shall", "will", "may", "must", "per", "total"};
	return fillers.count(folded) > 0;
}

// What an amount or a date is where it stands, in the document's own words
// (DESIGN3 9.3.3): the defined term that governs it, its table column and row,
// or the role noun right before it. `strong` for a defined term or a table
// cell - a place that says what the figure is - and not for a noun, which
// only says so within the passage it stands in. Empty when nothing says.
struct FactRole {
	std::string role;
	bool strong = false;
	// Where the role was read: a term the document defines, a markdown or Word
	// table's column and row, a workbook sheet's column and row, or a noun.
	// Each says the same fact within its own reach only (role_key_of).
	enum class Kind : unsigned char { None, Defined, Table, Workbook, Noun };
	Kind kind = Kind::None;
	// A table's header line, where it starts in the text: two tables of one
	// passage are two tables.
	std::size_t table_at = 0;
};

std::string joined_role(const std::string &text, const std::vector<WordSpan> &words, std::size_t first, std::size_t last) {
	std::string role;
	for (std::size_t index = first; index <= last && index < words.size(); ++index) {
		const std::string word = role_word(text.substr(words[index].start, words[index].end - words[index].start));
		if (word.empty()) continue;
		role += (role.empty() ? "" : " ") + word;
	}
	return role;
}

// A markdown or Word table's line: its cells between bars, or - a Word table
// read as text - between tabs.
std::vector<std::pair<std::size_t, std::size_t>> table_cells(const std::string &text, std::size_t from, std::size_t to) {
	std::vector<std::pair<std::size_t, std::size_t>> cells;
	std::size_t cursor = from;
	while (cursor < to && text[cursor] == ' ') ++cursor;
	if (cursor < to && text[cursor] != '|' && text.find('\t', cursor) < to) {
		for (std::size_t cell = cursor; cell <= to;) {
			std::size_t tab = text.find('\t', cell);
			if (tab == std::string::npos || tab > to) tab = to;
			cells.emplace_back(cell, tab);
			cell = tab + 1;
		}
		return cells;
	}
	if (cursor >= to || text[cursor] != '|') return cells;
	for (std::size_t bar = cursor; bar < to;) {
		const std::size_t next = text.find('|', bar + 1);
		if (next == std::string::npos || next > to) break;
		cells.emplace_back(bar + 1, next);
		bar = next;
	}
	return cells;
}

std::string trimmed_cell(const std::string &text, std::pair<std::size_t, std::size_t> cell) {
	std::size_t from = cell.first;
	std::size_t to = cell.second;
	while (from < to && (text[from] == ' ' || text[from] == '*')) ++from;
	while (to > from && (text[to - 1] == ' ' || text[to - 1] == '*')) --to;
	return text.substr(from, to - from);
}

FactRole table_role(const std::string &text, std::size_t start) {
	const std::size_t lineStart = start == 0 ? 0 : (text.rfind('\n', start - 1) == std::string::npos ? 0 : text.rfind('\n', start - 1) + 1);
	std::size_t lineEnd = text.find('\n', start);
	if (lineEnd == std::string::npos) lineEnd = text.size();
	const auto cells = table_cells(text, lineStart, lineEnd);
	if (cells.size() < 2) return {};
	std::size_t column = cells.size();
	for (std::size_t index = 0; index < cells.size(); ++index)
		if (cells[index].first <= start && start < cells[index].second) column = index;
	if (column == cells.size()) return {};
	// The table's first line is its header; a "|---|" line under it is not a row.
	std::size_t header = lineStart;
	while (header > 0) {
		const std::size_t previousEnd = header - 1;
		const std::size_t previousStart = previousEnd == 0 ? 0
			: (text.rfind('\n', previousEnd - 1) == std::string::npos ? 0 : text.rfind('\n', previousEnd - 1) + 1);
		if (table_cells(text, previousStart, previousEnd).size() < 2) break;
		header = previousStart;
	}
	if (header == lineStart) return {};
	std::size_t headerEnd = text.find('\n', header);
	if (headerEnd == std::string::npos) headerEnd = text.size();
	const auto headers = table_cells(text, header, headerEnd);
	if (column >= headers.size()) return {};
	const std::string heading = platform::folded_text(trimmed_cell(text, headers[column]));
	if (heading.empty() || heading.find_first_not_of("-: ") == std::string::npos) return {};
	// The row's key: its first label (a cell with a word in it) or date, else
	// its first cell - never the row's number when a label names the row.
	// A row's key cell - the month of a ledger's row - is keyed by itself.
	std::string rowKey;
	const auto keyLike = [](const std::string &cell) {
		return has_letter(cell) || semantic::parse_value(cell, semantic::ParseHints{semantic::Kind::Date, "", false}).has_value();
	};
	for (std::size_t index = 0; index < cells.size() && rowKey.empty(); ++index)
		if (index != column && keyLike(trimmed_cell(text, cells[index]))) rowKey = platform::folded_text(trimmed_cell(text, cells[index]));
	if (rowKey.empty() && keyLike(trimmed_cell(text, cells[column]))) rowKey = platform::folded_text(trimmed_cell(text, cells[column]));
	for (std::size_t index = 0; index < cells.size() && rowKey.empty(); ++index)
		if (index != column) rowKey = platform::folded_text(trimmed_cell(text, cells[index]));
	return {heading + "@" + rowKey, true, FactRole::Kind::Table, header};
}

FactRole role_of(const std::string &text, std::size_t start, std::size_t end) {
	if (FactRole table = table_role(text, start); !table.role.empty()) return table;
	const auto [sentenceStart, sentenceEnd] = sentence_around(text, start, end);
	// A term the sentence defines by this figure: "... $5,000,000 (the "Liability Cap")",
	// «... (далее - «Цена»)».
	{
		std::size_t cursor = end;
		while (cursor < sentenceEnd && text[cursor] == ' ') ++cursor;
		if (cursor < sentenceEnd && text[cursor] == '(') {
			const std::size_t close = text.find(')', cursor);
			if (close != std::string::npos && close <= sentenceEnd + 1 && close - cursor < 120) {
				const std::string inside = text.substr(cursor + 1, close - cursor - 1);
				for (const auto &[open, shut] : std::vector<std::pair<std::string, std::string>>{{"\"", "\""},
						{"\xE2\x80\x9C", "\xE2\x80\x9D"}, {"\xC2\xAB", "\xC2\xBB"}}) {
					const std::size_t opened = inside.find(open);
					const std::size_t closed = opened == std::string::npos ? std::string::npos : inside.find(shut, opened + open.size());
					if (closed == std::string::npos) continue;
					const std::string term = inside.substr(opened + open.size(), closed - opened - open.size());
					const std::vector<WordSpan> words = word_spans(term);
					if (!words.empty() && words.size() <= 6) return {joined_role(term, words, 0, words.size() - 1), true, FactRole::Kind::Defined};
				}
			}
		}
	}
	const std::string before = text.substr(sentenceStart, start - sentenceStart);
	const std::vector<WordSpan> words = word_spans(before);
	// '"Base Rent" means $21,350 ...': the term this sentence defines.
	for (const char *means : {"\" means", "\xE2\x80\x9D means", "\" shall mean", "\xE2\x80\x9D shall mean",
			"\xC2\xBB \xD0\xBE\xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB0\xD0\xB5\xD1\x82"}) {
		const std::size_t at = before.find(means);
		if (at == std::string::npos) continue;
		const std::string quote = std::string(means).substr(0, std::string(means).find(' '));
		const std::string opening = quote == "\"" ? "\"" : quote == "\xE2\x80\x9D" ? "\xE2\x80\x9C" : "\xC2\xAB";
		const std::size_t opened = before.rfind(opening, at == 0 ? 0 : at - 1);
		if (opened == std::string::npos || at - opened > 100) continue;
		const std::string term = before.substr(opened + opening.size(), at - opened - opening.size());
		const std::vector<WordSpan> termWords = word_spans(term);
		if (!termWords.empty() && termWords.size() <= 6) return {joined_role(term, termWords, 0, termWords.size() - 1), true, FactRole::Kind::Defined};
	}
	if (words.empty()) return {};
	// A capitalised name right before "shall be", "is", "of", ":", «составляет»:
	// "the Liability Cap of $5,000,000", «Цена Договора составляет ...».
	{
		std::string tail = platform::folded_text(before.substr(words.back().end));
		std::size_t last = words.size();
		static const std::vector<std::vector<std::string>> connectors{{"shall", "be"}, {"will", "be"}, {"is"}, {"of"},
			{"equal", "to"}, {"equals"}, {"amounting", "to"}, {"in", "the", "amount", "of"}, {"\xD1\x81\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x8F\xD0\xB5\xD1\x82"},
			{"\xD0\xB2", "\xD1\x80\xD0\xB0\xD0\xB7\xD0\xBC\xD0\xB5\xD1\x80\xD0\xB5"}};
		const bool colon = tail.find(':') != std::string::npos || tail.find('=') != std::string::npos;
		std::size_t connectorWords = 0;
		if (!colon) {
			for (const auto &connector : connectors) {
				if (connector.size() > words.size()) continue;
				bool matches = true;
				for (std::size_t index = 0; index < connector.size(); ++index) {
					const WordSpan &word = words[words.size() - connector.size() + index];
					if (platform::folded_text(before.substr(word.start, word.end - word.start)) != connector[index]) matches = false;
				}
				if (matches) {
					connectorWords = connector.size();
					break;
				}
			}
		}
		if (colon || connectorWords > 0) {
			last = words.size() - connectorWords;
			std::size_t first = last;
			while (first > 0 && words[first - 1].capitalised && last - first < 5) --first;
			// An article that opens the sentence is no part of the name.
			while (first < last && role_filler_word(platform::folded_text(before.substr(words[first].start,
					words[first].end - words[first].start))))
				++first;
			const bool opensSentence = first == 0;
			if (first < last && (!opensSentence || last - first >= 2)) return {joined_role(before, words, first, last - 1), true, FactRole::Kind::Defined};
		}
	}
	// A role noun among the six words before it: "a limit of", "the monthly rent of".
	static const std::set<std::string> nouns{"price", "cap", "limit", "deductible", "basket", "rent", "fee", "fees",
		"penalty", "claim", "balance", "deposit", "premium", "principal", "loan", "salary", "interest", "escrow",
		"retention", "holdback", "threshold", "date", "deadline", "rate"};
	static const std::vector<std::string> stems{"\xD1\x86\xD0\xB5\xD0\xBD", "\xD0\xBB\xD0\xB8\xD0\xBC\xD0\xB8\xD1\x82",
		"\xD0\xBD\xD0\xB5\xD1\x83\xD1\x81\xD1\x82\xD0\xBE\xD0\xB9\xD0\xBA", "\xD0\xB7\xD0\xB0\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xBD",
		"\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82", "\xD1\x88\xD1\x82\xD1\x80\xD0\xB0\xD1\x84", "\xD0\xB4\xD0\xB0\xD1\x82",
		"\xD1\x81\xD1\x80\xD0\xBE\xD0\xBA"};
	for (std::size_t back = 0; back < 6 && back < words.size(); ++back) {
		const std::size_t index = words.size() - 1 - back;
		const std::string word = role_word(before.substr(words[index].start, words[index].end - words[index].start));
		bool noun = nouns.count(word) > 0;
		for (const std::string &stem : stems)
			if (word.rfind(stem, 0) == 0) noun = true;
		if (!noun) continue;
		std::size_t first = index;
		if (first > 0) {
			const std::string previous = role_word(before.substr(words[first - 1].start, words[first - 1].end - words[first - 1].start));
			const bool figure = std::all_of(previous.begin(), previous.end(), [](char character) {
				return (character >= '0' && character <= '9') || character == ',' || character == '.';
			});
			if (!role_filler_word(previous) && !figure) --first;
		}
		return {joined_role(before, words, first, index), false, FactRole::Kind::Noun};
	}
	return {};
}

// The workbook cell a quantity is the value of, in a reading's own line - "Row
// 16: A16 = … | B16 = 21350 (shown in $)" - named with its sheet when the
// passage names one ("Ledger!B16"); and the cell a pure reference formula
// right after it names ("{=B19}", "{=Ledger!B19}").
struct CellPlace {
	std::string cell;
	std::string referenced;
	std::string bare;
};

std::string cell_reference_at(const std::string &text, std::size_t from, std::size_t to) {
	std::size_t cursor = from;
	while (cursor < to && text[cursor] == '$') ++cursor;
	const std::size_t letters = cursor;
	while (cursor < to && text[cursor] >= 'A' && text[cursor] <= 'Z' && cursor - letters < 3) ++cursor;
	if (cursor == letters) return {};
	std::string reference = text.substr(letters, cursor - letters);
	while (cursor < to && text[cursor] == '$') ++cursor;
	const std::size_t digits = cursor;
	while (cursor < to && text[cursor] >= '0' && text[cursor] <= '9' && cursor - digits < 7) ++cursor;
	if (cursor == digits || cursor != to) return {};
	return reference + text.substr(digits, cursor - digits);
}

CellPlace cell_place(const std::string &text, std::size_t start, std::size_t end) {
	CellPlace place;
	const std::size_t lineStart = start == 0 ? 0 : (text.rfind('\n', start - 1) == std::string::npos ? 0 : text.rfind('\n', start - 1) + 1);
	if (text.compare(lineStart, 4, "Row ") != 0) return place;
	std::size_t lineEnd = text.find('\n', start);
	if (lineEnd == std::string::npos) lineEnd = text.size();
	// The cell's own " = " and its reference before it.
	const std::size_t equals = text.rfind(" = ", start);
	if (equals == std::string::npos || equals < lineStart || equals + 3 > start) return place;
	std::size_t refStart = equals;
	while (refStart > lineStart && text[refStart - 1] != ' ') --refStart;
	const std::string reference = cell_reference_at(text, refStart, equals);
	if (reference.empty()) return place;
	// Nothing but the value's own note between " = " and the value.
	if (text.substr(equals + 3, start - equals - 3).find('|') != std::string::npos) return place;
	std::string sheet;
	for (std::size_t at = text.rfind("\nSheet \"", lineStart); ; at = text.rfind("\nSheet \"", at - 1)) {
		const std::size_t nameStart = at == std::string::npos ? (text.compare(0, 7, "Sheet \"") == 0 ? 7 : std::string::npos) : at + 8;
		if (nameStart != std::string::npos) {
			const std::size_t nameEnd = text.find('"', nameStart);
			if (nameEnd != std::string::npos && nameEnd < lineStart) sheet = text.substr(nameStart, nameEnd - nameStart);
		}
		break;
	}
	place.bare = reference;
	place.cell = sheet.empty() ? reference : sheet + "!" + reference;
	// A formula that is only a reference: "{=B19}", "{=Ledger!B19}".
	const std::size_t pieceEnd = std::min(lineEnd, text.find(" | ", end) == std::string::npos ? lineEnd : text.find(" | ", end));
	const std::size_t formula = text.find(" {=", end);
	if (formula != std::string::npos && formula < pieceEnd) {
		const std::size_t close = text.find('}', formula);
		if (close != std::string::npos && close < pieceEnd) {
			std::string inside = text.substr(formula + 3, close - formula - 3);
			std::string referencedSheet;
			const std::size_t bang = inside.rfind('!');
			if (bang != std::string::npos) {
				referencedSheet = inside.substr(0, bang);
				if (referencedSheet.size() >= 2 && referencedSheet.front() == '\'' && referencedSheet.back() == '\'')
					referencedSheet = referencedSheet.substr(1, referencedSheet.size() - 2);
				inside = inside.substr(bang + 1);
			}
			const std::string target = cell_reference_at(inside, 0, inside.size());
			if (!target.empty()) {
				const std::string targetSheet = referencedSheet.empty() ? sheet : referencedSheet;
				place.referenced = targetSheet.empty() ? target : targetSheet + "!" + target;
			}
		}
	}
	return place;
}

// What a workbook cell's figure is, as its sheet says (DESIGN3 9.3.3, rule 2):
// its column's header and its row's key - "rent@july 2026" for B16 of "Row 1:
// A1 = Month | B1 = Rent" and "Row 16: A16 = July 2026 | B16 = 21350". The
// same role a Word or markdown table's row gives the same figure. Empty when
// the sheet names neither.
FactRole workbook_role(const std::string &text, std::size_t start) {
	const std::size_t lineStart = start == 0 ? 0 : (text.rfind('\n', start - 1) == std::string::npos ? 0 : text.rfind('\n', start - 1) + 1);
	std::size_t lineEnd = text.find('\n', start);
	if (lineEnd == std::string::npos) lineEnd = text.size();
	// "A16 = July 2026 (shown …)" -> {"A", "July 2026"}, each cell of a line.
	const auto cellsOf = [&](std::size_t from, std::size_t to) {
		std::vector<std::tuple<std::string, std::string, std::size_t, std::size_t>> cells;
		std::size_t colon = text.find(": ", from);
		if (colon == std::string::npos || colon >= to) return cells;
		for (std::size_t cell = colon + 2; cell < to;) {
			std::size_t bar = text.find(" | ", cell);
			if (bar == std::string::npos || bar > to) bar = to;
			const std::size_t equals = text.find(" = ", cell);
			if (equals != std::string::npos && equals < bar) {
				std::string letters;
				for (std::size_t at = cell; at < equals && text[at] >= 'A' && text[at] <= 'Z'; ++at) letters += text[at];
				std::string value = text.substr(equals + 3, bar - equals - 3);
				const std::size_t note = value.find(" (shown");
				if (note != std::string::npos) value.erase(note);
				const std::size_t formula = value.find(" {=");
				if (formula != std::string::npos) value.erase(formula);
				if (!letters.empty()) cells.emplace_back(letters, value, equals + 3, bar);
			}
			cell = bar + 3;
		}
		return cells;
	};
	if (text.compare(lineStart, 4, "Row ") != 0) return {};
	const auto row = cellsOf(lineStart, lineEnd);
	std::string column;
	for (const auto &[letters, value, from, to] : row)
		if (from <= start && start < to) column = letters;
	if (column.empty()) return {};
	// The header: the sheet's "Row 1:" line before this one, or the "header row
	// 1:" a later passage of the sheet repeats.
	std::string heading;
	for (const std::string &marker : {std::string("header row 1: "), std::string("\nRow 1: ")}) {
		const std::size_t at = text.rfind(marker, lineStart);
		if (at == std::string::npos || !heading.empty()) continue;
		const std::size_t headerStart = at + (marker[0] == '\n' ? 1 : 0);
		std::size_t headerEnd = text.find('\n', headerStart);
		if (headerEnd == std::string::npos) headerEnd = text.size();
		if (headerStart == lineStart) return {};
		const std::size_t cellsFrom = marker[0] == '\n' ? headerStart : headerStart + marker.size() - 2;
		for (const auto &[letters, value, from, to] : cellsOf(cellsFrom, headerEnd))
			if (letters == column) heading = platform::folded_text(value);
	}
	if (heading.empty()) return {};
	std::string rowKey;
	std::string own;
	const auto keyLike = [](const std::string &value) {
		return has_letter(value) || semantic::parse_value(value, semantic::ParseHints{semantic::Kind::Date, "", false}).has_value();
	};
	for (const auto &[letters, value, from, to] : row) {
		if (letters == column) own = value;
		else if (rowKey.empty() && keyLike(value)) rowKey = platform::folded_text(value);
	}
	if (rowKey.empty() && keyLike(own)) rowKey = platform::folded_text(own);
	if (rowKey.empty()) return {};
	return {heading + "@" + rowKey, true, FactRole::Kind::Workbook};
}

// The one cell a sentence of prose names beside a figure: "cell B19", "Ledger!B19",
// «ячейка B19». Empty when it names none, or more than one.
std::string prose_cell(const std::string &text, std::size_t start, std::size_t end) {
	const auto [from, to] = sentence_around(text, start, end);
	std::string found;
	std::size_t count = 0;
	const std::string sentence = text.substr(from, to - from);
	const std::string folded = platform::folded_text(sentence);
	const auto consider = [&](std::size_t at) {
		std::size_t cursor = at;
		while (cursor < sentence.size() && sentence[cursor] == ' ') ++cursor;
		std::size_t referenceEnd = cursor;
		while (referenceEnd < sentence.size() && (std::isalnum(static_cast<unsigned char>(sentence[referenceEnd])) != 0
				|| sentence[referenceEnd] == '$'))
			++referenceEnd;
		const std::string reference = cell_reference_at(sentence, cursor, referenceEnd);
		if (reference.empty() || reference == found) return;
		found = reference;
		++count;
	};
	for (const char *word : {"cell ", "cells ", "\xD1\x8F\xD1\x87\xD0\xB5\xD0\xB9\xD0\xBA"}) {
		for (std::size_t at = folded.find(word); at != std::string::npos; at = folded.find(word, at + 1)) {
			std::size_t after = at + std::char_traits<char>::length(word);
			// «ячейка», «ячейке», «ячейки»: past the ending.
			while (after < sentence.size() && static_cast<unsigned char>(sentence[after]) >= 0x80) ++after;
			if (after < sentence.size() && sentence[after] != ' ' && std::char_traits<char>::length(word) > 6) continue;
			consider(after);
		}
	}
	// "Ledger!B19": a sheet's name, then the cell.
	for (std::size_t bang = sentence.find('!'); bang != std::string::npos; bang = sentence.find('!', bang + 1)) {
		std::size_t nameStart = bang;
		while (nameStart > 0 && (std::isalnum(static_cast<unsigned char>(sentence[nameStart - 1])) != 0
				|| sentence[nameStart - 1] == '_'))
			--nameStart;
		if (nameStart == bang) continue;
		std::size_t referenceEnd = bang + 1;
		while (referenceEnd < sentence.size() && (std::isalnum(static_cast<unsigned char>(sentence[referenceEnd])) != 0
				|| sentence[referenceEnd] == '$'))
			++referenceEnd;
		const std::string reference = cell_reference_at(sentence, bang + 1, referenceEnd);
		if (reference.empty()) continue;
		const std::string named = sentence.substr(nameStart, bang - nameStart) + "!" + reference;
		if (named == found) continue;
		found = named;
		++count;
	}
	return count == 1 ? found : std::string();
}

// Whether the figure at [start, end) of `text` restates the figure the same
// clause wrote just before it at [before, beforeEnd) - one fact written twice
// (DESIGN3 9.3.2, A2): "$18,400,000 (eighteen million … dollars)", "$18.4
// million, i.e. $18,400,000", «3 марта 2026 г. (03.03.2026 г.; 2026-03-03)».
// Only brackets, punctuation and the words that say so may stand between them,
// and something must say so: a bracket opened between them, a restating word,
// or - inside one bracket that holds both - another writing of the value. A
// list of equal figures written alike ("$1m; $1m; $1m") is several facts.
bool restates(const std::string &text, std::size_t before, std::size_t beforeEnd, std::size_t start, std::size_t end) {
	if (beforeEnd > start) return false;
	std::string rest = platform::folded_text(text.substr(beforeEnd, start - beforeEnd));
	bool said = rest.find('(') != std::string::npos;
	for (const char *words : {"that is", "i.e.", "i.e", "namely", "in words", "\xD1\x82\xD0\xBE \xD0\xB5\xD1\x81\xD1\x82\xD1\x8C",
			"\xD1\x82.\xD0\xB5.", "\xD0\xB8\xD0\xBB\xD0\xB8", "or"}) {
		for (std::size_t at = rest.find(words); at != std::string::npos; at = rest.find(words)) {
			rest.erase(at, std::char_traits<char>::length(words));
			said = true;
		}
	}
	// «г.» of a Russian date, a dash, a no-break space: no word of their own.
	for (const char *mark : {"\xD0\xB3.", "\xE2\x80\x94", "\xE2\x80\x93", "\xC2\xA0"})
		for (std::size_t at = rest.find(mark); at != std::string::npos; at = rest.find(mark))
			rest.erase(at, std::char_traits<char>::length(mark));
	if (rest.size() > 12 || rest.find_first_not_of(" ,;:()-.") != std::string::npos) return false;
	if (said) return true;
	// Inside one bracket that holds both, written another way.
	const std::size_t opened = text.rfind('(', start);
	if (opened == std::string::npos || opened > before) return false;
	if (text.find(')', opened) < start) return false;
	return text.compare(before, beforeEnd - before, text, start, end - start) != 0;
}

// One place a quantity or a date is written, and the fact it was found to be.
struct FactDecision {
	std::optional<std::size_t> row;
	FactProvenance provenance;
	// A new fact takes this key instead of its place's: the anchor that named it.
	std::string anchored_key;
	// The further proofs this place gives, registered with the fact it is.
	std::string statement_key;
	std::string role_key;
	// A term a document defines, by its words and value alone: what the lawyer's
	// words and an earlier answer name - the one fact of the material defined
	// so, never one of several.
	std::string named_role_key;
	// A4: a table row's or a workbook row's tie (its role and value, with the
	// passage or the sheet it stands in), and whether it is a table's.
	std::string tie_key;
	bool table_tie = false;
	// The document it stands in (scope_of): a workbook's cells are its own.
	std::string scope;
	std::string cell;
	std::string referenced_cell;
	std::string bare_cell;
};

// The document a text of this operation belongs to (document_of_passage), or
// the text alone when it belongs to none: how far a defined term, a table's
// role or a sentence read twice can say "the same fact".
std::string scope_of(const OperationProtectedSymbolTable &symbols, const std::string &digest) {
	const auto found = symbols.document_of_passage.find(digest);
	if (found != symbols.document_of_passage.end() && !found->second.empty()) return "d:" + found->second;
	return "p:" + digest;
}

// The words of a figure's sentence that say whose or which it is - a name, a
// number, a month - folded and sorted: "Tenant Alpha pays rent of $5,000" and
// "Tenant Beta pays rent of $5,000" are two tenants' rents. The figure itself,
// its role's own words and the little words are not counted.
std::string subject_of(const std::string &text, std::size_t start, std::size_t end, const std::string &role) {
	const auto [from, to] = sentence_around(text, start, end);
	std::set<std::string> roleWords;
	for (std::size_t at = 0; at <= role.size();) {
		const std::size_t space = std::min(role.find(' ', at), role.size());
		if (space > at) roleWords.insert(role.substr(at, space - at));
		at = space + 1;
	}
	std::set<std::string> subject;
	const std::string sentence = text.substr(from, to - from);
	for (const WordSpan &word : word_spans(sentence)) {
		const std::size_t at = from + word.start;
		const std::size_t stop = from + word.end;
		if (at < end && start < stop) continue;
		const std::string raw = sentence.substr(word.start, word.end - word.start);
		const bool digit = std::any_of(raw.begin(), raw.end(), [](char character) { return character >= '0' && character <= '9'; });
		if (!word.capitalised && !digit) continue;
		const std::string folded = role_word(raw);
		if (folded.empty() || role_filler_word(folded) || roleWords.count(folded) > 0) continue;
		subject.insert(folded);
	}
	std::string joined;
	for (const std::string &word : subject) joined += (joined.empty() ? "" : ",") + word;
	return joined;
}

// A workbook cell's key within its document: "cell|<scope>|Ledger!B16".
std::string cell_key_of(const std::string &scope, const std::string &cell) {
	return cell.empty() ? std::string() : "cell|" + scope + "|" + cell;
}

// The sheet a workbook cell stands on ("Ledger" of "Ledger!B16"), folded.
std::string sheet_of(const std::string &cell) {
	const std::size_t bang = cell.rfind('!');
	return bang == std::string::npos ? std::string() : platform::folded_text(cell.substr(0, bang));
}

// A role's key with its reach (DESIGN3 9.3.2 A5, under owner correction 1: a
// symbol names a fact, and two documents' equal figures are two facts). A term
// a document defines says the same fact within that document; a table's
// column and row within that table; a workbook's column and row within that
// sheet of that workbook; a role noun within its passage, for the same subject.
std::string role_key_of(const FactRole &role, const std::string &scope, const std::string &digest,
		const std::string &subject, const std::string &sheet, const std::string &valueKey) {
	if (role.role.empty()) return {};
	switch (role.kind) {
		case FactRole::Kind::Defined: return "role|" + scope + "|" + role.role + "|" + valueKey;
		case FactRole::Kind::Table:
			return "tbl|" + digest + "@" + std::to_string(role.table_at) + "|" + role.role + "|" + valueKey;
		case FactRole::Kind::Workbook: return "wb|" + scope + "|" + sheet + "|" + role.role + "|" + valueKey;
		case FactRole::Kind::Noun: return "noun|" + digest + "|" + subject + "|" + role.role + "|" + valueKey;
		case FactRole::Kind::None: break;
	}
	return {};
}

// A defined term by its words and value alone, across the material: the key
// the lawyer's words and an earlier answer look a defined fact up by. Kept only
// while one fact of the material is defined so (register_fact_place).
std::string named_role_key(const std::string &role, const std::string &valueKey) {
	return role.empty() ? std::string() : "any|" + role + "|" + valueKey;
}

// The statement a figure stands in, as a key: its document, its sentence and
// which writing of that figure in the sentence it is. The same statement read
// twice - a passage in two overlapping chunks of one document - is one fact.
// The same sentence in two documents (a template's clause in two leases) is two
// facts. Only a sentence long enough to say something (40 bytes or more).
std::string statement_key_of(const std::string &text, std::size_t start, std::size_t end, const std::string &scope) {
	const auto [from, to] = sentence_around(text, start, end);
	if (to - from < 40) return {};
	const std::string writing = text.substr(start, end - start);
	std::size_t ordinal = 0;
	for (std::size_t at = text.find(writing, from); at != std::string::npos && at < start; at = text.find(writing, at + 1)) ++ordinal;
	return "stmt|" + scope + "|" + sixteen_hex(text.substr(from, to - from)) + "|" + std::to_string(start - from) + "|"
		+ std::to_string(ordinal);
}

// Whether folded `text` names `name` as a whole word or words ("the Ledger
// sheet", "Schedule 2 (Rent Ledger)"): what ties a document's table to a sheet.
bool names_word(const std::string &text, const std::string &name) {
	if (name.size() < 3) return false;
	const auto wordByte = [](char character) {
		return std::isalnum(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0x80;
	};
	for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
		const bool opens = at == 0 || !wordByte(text[at - 1]);
		const bool closes = at + name.size() >= text.size() || !wordByte(text[at + name.size()]);
		if (opens && closes) return true;
	}
	return false;
}

// The fact restoration wrote at [start, start + length) of `text` earlier in
// this operation: the answer's own sentence stands around it again (a draft
// under review, a correction), which proves the fact. Nothing when no such
// sentence stands there, or when two facts were written so.
std::optional<std::string> restored_fact_at(const std::string &text, std::size_t start, std::size_t length,
		const OperationProtectedSymbolTable &symbols) {
	const auto found = symbols.restored_writings.find(text.substr(start, length));
	if (found == symbols.restored_writings.end()) return std::nullopt;
	std::set<std::string> keys;
	for (const OperationProtectedSymbolTable::RestoredPlace &place : found->second) {
		if (place.offset > start) continue;
		if (text.compare(start - place.offset, place.sentence.size(), place.sentence) == 0) keys.insert(place.key);
	}
	if (keys.size() != 1) return std::nullopt;
	return *keys.begin();
}

// Where in a text an amount or a date stands, as far as its fact goes. A part
// detection reads on its own - one passage, one history record, the request -
// is what its FactContext says. A whole prompt protected as one text
// (PromptAssembler's sections read at once) is read by its own structure: the
// section a figure stands in says whose words it is, and a SOURCE block which
// document's passage - the block's own text is the passage it was met in.
struct TextPlace {
	FactProvenance::Origin origin = FactProvenance::Origin::Source;
	// The passage by its digest, and where it starts in the text.
	std::string passage;
	std::size_t passage_start = 0;
	// Its document (scope_of), and whether it is a document's text at all.
	std::string scope;
	bool document = false;
};

// A SOURCE block of a text around [start, end): the block's text, where it
// starts, and its document's handle ("DOC_2"). Nothing when it stands in none.
struct SourceBlock {
	std::size_t from = 0;
	std::size_t to = 0;
	std::string document;
};

std::optional<SourceBlock> source_block_around(const std::string &text, std::size_t start, std::size_t end) {
	static const std::string kMarker = "[SOURCE:DOC_";
	static const std::string kClosing = "\n[/SOURCE]";
	const std::size_t marker = start == 0 ? std::string::npos : text.rfind(kMarker, start - 1);
	if (marker == std::string::npos || (marker > 0 && text[marker - 1] != '\n')) return std::nullopt;
	const std::size_t lineEnd = text.find('\n', marker);
	if (lineEnd == std::string::npos || lineEnd >= start || text[lineEnd - 1] != ']') return std::nullopt;
	const std::size_t closing = text.find(kClosing, lineEnd);
	if (closing == std::string::npos || closing < end) return std::nullopt;
	const std::size_t tokenStart = marker + 8;
	const std::size_t colon = text.find(':', tokenStart);
	if (colon == std::string::npos || colon > lineEnd) return std::nullopt;
	return SourceBlock{lineEnd + 1, closing, text.substr(tokenStart, colon - tokenStart)};
}

// The title of a document header around [start, end) ("[DOCUMENT:DOC_2]\n
// Board Minutes 2026-08-14.txt\n[/DOCUMENT]", SourceMaterialSpans): its one line,
// where it starts, and its document's handle. A file's name is the same bytes
// on every turn, so a date in it is one place - read against the whole prompt
// instead, it was a new fact, and a new symbol, each time the prompt grew.
std::optional<SourceBlock> document_header_around(const std::string &text, std::size_t start, std::size_t end) {
	static const std::string kMarker = "[DOCUMENT:DOC_";
	static const std::string kClosing = "\n[/DOCUMENT]";
	const std::size_t marker = start == 0 ? std::string::npos : text.rfind(kMarker, start - 1);
	if (marker == std::string::npos || (marker > 0 && text[marker - 1] != '\n')) return std::nullopt;
	const std::size_t lineEnd = text.find('\n', marker);
	if (lineEnd == std::string::npos || lineEnd >= start || text[lineEnd - 1] != ']') return std::nullopt;
	const std::size_t closing = text.find(kClosing, lineEnd);
	if (closing == std::string::npos || closing < end) return std::nullopt;
	// A title is one line: a closing further on belongs to another header.
	if (text.find('\n', lineEnd + 1) < closing) return std::nullopt;
	const std::size_t tokenStart = marker + 10;
	return SourceBlock{lineEnd + 1, closing, text.substr(tokenStart, lineEnd - 1 - tokenStart)};
}

TextPlace place_in(const std::string &text, std::size_t start, std::size_t end, const OperationProtectedSymbolTable &symbols,
		const FactContext &context) {
	TextPlace place;
	place.origin = context.origin;
	place.passage = context.digest;
	const auto registered = symbols.document_of_passage.find(context.digest);
	place.document = registered != symbols.document_of_passage.end() && !registered->second.empty();
	static const std::string kSections = "<<<LM_SECTION:";
	if (text.compare(0, kSections.size() + 9, kSections + "SYSTEM>>>") == 0) {
		const std::size_t opened = text.rfind(kSections, start);
		if (opened != std::string::npos) {
			const std::size_t nameStart = opened + kSections.size();
			const std::size_t nameEnd = text.find(">>>", nameStart);
			const std::string name = nameEnd == std::string::npos ? std::string() : text.substr(nameStart, nameEnd - nameStart);
			const std::size_t closed = name.empty() ? std::string::npos : text.find("<<<LM_END_SECTION:" + name + ">>>", nameStart);
			if (closed != std::string::npos && closed >= end) {
				if (name == "CURRENT_REQUEST" || name == "USER_DEFAULT_INSTRUCTIONS" || name == "MATTER_INSTRUCTIONS")
					place.origin = FactProvenance::Origin::Request;
				else if (name == "HISTORY") place.origin = FactProvenance::Origin::History;
				else if (name == "PROJECT_CONTEXT") place.origin = FactProvenance::Origin::ProjectContext;
				else if (name == "SOURCE_MATERIAL") place.origin = FactProvenance::Origin::Source;
			}
		}
	}
	if (const std::optional<SourceBlock> block = source_block_around(text, start, end)) {
		place.passage = platform::sha256_hex_digest(text.substr(block->from, block->to - block->from));
		place.passage_start = block->from;
		place.document = true;
	} else if (const std::optional<SourceBlock> title = document_header_around(text, start, end)) {
		place.passage = platform::sha256_hex_digest(text.substr(title->from, title->to - title->from));
		place.passage_start = title->from;
	}
	place.scope = scope_of(symbols, place.passage);
	return place;
}

// Which fact the quantity or date `entity` of `text` is (DESIGN3 9.3.2, rules
// A0-A6, the first that holds): the fact an anchor names; the fact already
// found at this very place; the fact an answer of this operation wrote in this
// very sentence; the figure the same clause just wrote, restated; the cell it
// is, or refers to, or the cell its sentence names; the table row and the
// workbook row its document ties together; the same statement of the same
// document read before; the same role within its reach, with the same value;
// for the lawyer's words and an earlier answer, the one fact the material
// defines by that term and value; otherwise a fact of its own. Never a fact
// only because its value is equal. Reads the table; `register_fact_place`
// records it.
FactDecision attribute_fact(const std::string &text, const CanonicalEntity &entity, const semantic::Value &value,
		const std::string &valueKey, const OperationProtectedSymbolTable &symbols, const FactContext &context) {
	FactDecision decision;
	const std::size_t start = entity.start;
	const std::size_t end = entity.start + entity.length;
	const auto agrees = [&](std::size_t row) {
		return row < symbols.mapping.size() && row_value_agrees(symbols.mapping[row], value, valueKey);
	};
	const auto lookup = [&](const std::map<std::string, std::size_t> &index, const std::string &key) -> std::optional<std::size_t> {
		if (key.empty()) return std::nullopt;
		const auto found = index.find(key);
		if (found != index.end() && agrees(found->second)) return found->second;
		return std::nullopt;
	};
	// Where it stands: the place its fact key names.
	// A workbook cell is its own place: its reading's line has no role words.
	const CellPlace cell = cell_place(text, start, end);
	const FactRole role = !entity.fact_key.empty() ? FactRole{}
		: cell.cell.empty() ? role_of(text, start, end) : workbook_role(text, start);
	const TextPlace place = place_in(text, start, end, symbols, context);
	const FactProvenance::Origin origin = place.origin;
	const std::string &scope = place.scope;
	const std::string sheet = sheet_of(cell.cell);
	decision.scope = scope;
	decision.provenance.origin = origin;
	if (origin == FactProvenance::Origin::History && role.role.empty() && cell.cell.empty())
		decision.provenance.origin = FactProvenance::Origin::Occurrence;
	decision.provenance.passage_sha256 = place.passage;
	decision.provenance.offset = start - place.passage_start;
	decision.provenance.length = entity.length;
	decision.provenance.cell = cell.cell;
	decision.provenance.role = role.role;
	decision.cell = cell.cell;
	decision.bare_cell = cell.bare;
	decision.referenced_cell = cell.referenced;
	// A workbook cell is its cell, never its line's words: two sheets' equal
	// lines are two sheets' facts. The lawyer's own sentence, and an earlier
	// answer's, sent again later in the operation - a review quoting the
	// request - is the same statement wherever it stands.
	// Text that is no document's (Singularity's framing, a typed field) is one
	// statement wherever it is sent again, as before.
	const std::string statementScope = origin == FactProvenance::Origin::Request ? std::string("request")
		: origin == FactProvenance::Origin::History ? std::string("history") : place.document ? scope : std::string("free");
	if (cell.cell.empty()) decision.statement_key = statement_key_of(text, start, end, statementScope);
	FactRole reach = role;
	if (reach.kind == FactRole::Kind::Table && reach.table_at >= place.passage_start) reach.table_at -= place.passage_start;
	decision.role_key = role_key_of(reach, scope, place.passage,
		role.kind == FactRole::Kind::Noun ? subject_of(text, start, end, role.role) : std::string(), sheet, valueKey);
	// Only a document's own definition is a fact the lawyer's words can name.
	const bool material = origin == FactProvenance::Origin::Source || origin == FactProvenance::Origin::ProjectContext;
	if (material && role.kind == FactRole::Kind::Defined) decision.named_role_key = named_role_key(role.role, valueKey);
	if (role.kind == FactRole::Kind::Table) {
		decision.tie_key = "tie-tbl|" + role.role + "|" + valueKey + "|" + place.passage;
		decision.table_tie = true;
	} else if (role.kind == FactRole::Kind::Workbook && !sheet.empty()) {
		decision.tie_key = "tie-wb|" + role.role + "|" + valueKey + "|" + sheet;
	}
	// A0: an earlier answer's anchor.
	if (!entity.fact_key.empty()) {
		if (const auto row = lookup(symbols.fact_by_key, entity.fact_key)) {
			decision.row = row;
			return decision;
		}
		decision.anchored_key = entity.fact_key;
		decision.provenance.origin = FactProvenance::Origin::History;
		decision.provenance.anchored = true;
		return decision;
	}
	// A1: this very place, read before - in this text (another section, the
	// settling pass), or under its key: the same passage of a later read, or the
	// place an earlier answer's anchor named before the passage was read.
	if (const auto row = lookup(symbols.fact_by_key, fact_key_of(decision.provenance))) {
		decision.row = row;
		return decision;
	}
	WrittenText &written = written_text(symbols, text);
	for (const FactPlace &place : written.facts)
		if (place.start == start && place.length == entity.length && agrees(place.row)) {
			decision.row = place.row;
			return decision;
		}
	// An answer of this operation sent again: the very sentence restoration
	// wrote the fact in stands around it (restored_writings) - the answer's own
	// text, not a figure of equal value.
	// A statement's operand is found by the writing inside its marks.
	std::optional<std::string> restoredKey = restored_fact_at(text, start, entity.length, symbols);
	if (!restoredKey && entity.source_label == kOperandLabel && !entity.value.empty()) {
		const std::size_t inner = text.find(entity.value, start);
		if (inner != std::string::npos && inner + entity.value.size() <= end)
			restoredKey = restored_fact_at(text, inner, entity.value.size(), symbols);
	}
	if (restoredKey)
		if (const auto row = lookup(symbols.fact_by_key, *restoredKey)) {
			decision.row = row;
			return decision;
		}
	// A2: the figure the same clause wrote just before, restated.
	{
		const auto [sentenceStart, sentenceEnd] = sentence_around(text, start, end);
		static_cast<void>(sentenceEnd);
		const FactPlace *previous = nullptr;
		for (const FactPlace &place : written.facts)
			if (place.start + place.length <= start && place.start >= sentenceStart
					&& (previous == nullptr || place.start > previous->start))
				previous = &place;
		if (previous != nullptr && agrees(previous->row)
				&& restates(text, previous->start, previous->start + previous->length, start, end)) {
			decision.row = previous->row;
			return decision;
		}
	}
	// A3: the cell it is, the cell its formula refers to - both within this
	// workbook: two workbooks' Ledger!A16 are two cells - and the cell its
	// sentence names.
	if (const auto row = lookup(symbols.fact_by_cell, cell_key_of(scope, cell.cell))) {
		decision.row = row;
		return decision;
	}
	if (const auto row = lookup(symbols.fact_by_cell, cell_key_of(scope, cell.referenced))) {
		decision.row = row;
		return decision;
	}
	// A cell prose named without its sheet ("cell B19"), read before the sheet -
	// unless another sheet's B19 was read first: then it names neither. The
	// first sheet cell that takes it keeps it (register_fact_place).
	if (!cell.bare.empty()) {
		const auto named = lookup(symbols.fact_by_cell, "named:" + cell.bare);
		if (named && symbols.fact_by_cell.find("bare:" + cell.bare) == symbols.fact_by_cell.end()) {
			decision.row = named;
			return decision;
		}
	}
	if (cell.cell.empty()) {
		const std::string named = prose_cell(text, start, end);
		if (!named.empty()) {
			decision.referenced_cell = named;
			if (const auto row = lookup(symbols.fact_by_cell, named)) {
				decision.row = row;
				return decision;
			}
			// "cell B19" with no sheet: the one sheet's B19 this request reads.
			if (named.find('!') == std::string::npos)
				if (const auto row = lookup(symbols.fact_by_cell, "bare:" + named)) {
					decision.row = row;
					return decision;
				}
		}
	}
	// A4: a document's table row and the workbook row it is tied to - the
	// document names the sheet, the two rows' keys and columns agree, and so do
	// their values. Only the one such row; several prove nothing.
	if (!decision.tie_key.empty()) {
		const std::string prefix = (decision.table_tie ? "tie-wb|" : "tie-tbl|") + role.role + "|" + valueKey + "|";
		const std::optional<SourceBlock> block = source_block_around(text, start, end);
		const std::string folded = !decision.table_tie ? std::string()
			: platform::folded_text(block ? text.substr(block->from, block->to - block->from) : text);
		std::set<std::size_t> tied;
		for (auto found = symbols.fact_by_role.lower_bound(prefix);
				found != symbols.fact_by_role.end() && found->first.compare(0, prefix.size(), prefix) == 0; ++found) {
			if (!agrees(found->second)) continue;
			const std::string other = found->first.substr(prefix.size());
			if (decision.table_tie) {
				if (names_word(folded, other)) tied.insert(found->second);
			} else {
				const auto passage = symbols.table_passages.find(other);
				if (passage != symbols.table_passages.end() && names_word(passage->second, sheet)) tied.insert(found->second);
			}
		}
		if (tied.size() == 1) {
			decision.row = *tied.begin();
			return decision;
		}
	}
	// The same statement of the same document, read before - never a sentence
	// this very text writes twice (a ledger's two equal lines are two payments).
	if (const auto row = lookup(symbols.fact_by_key, decision.statement_key)) {
		const bool repeated = std::any_of(written.facts.begin(), written.facts.end(), [&](const FactPlace &place) {
			return place.row == *row && place.start != start;
		});
		if (!repeated) {
			decision.row = row;
			return decision;
		}
	}
	// A5: the same role within its reach, with the same value.
	if (const auto row = lookup(symbols.fact_by_role, decision.role_key)) {
		decision.row = row;
		return decision;
	}
	// The lawyer's words and an earlier answer may name a term the material
	// defines ("the Liability Cap of $5m"): the one fact defined so.
	if ((origin == FactProvenance::Origin::Request || origin == FactProvenance::Origin::History)
			&& (role.kind == FactRole::Kind::Defined || role.kind == FactRole::Kind::Noun))
		if (const auto row = lookup(symbols.fact_by_role, named_role_key(role.role, valueKey))) {
			decision.row = row;
			return decision;
		}
	// A6: a fact of its own.
	return decision;
}

// Records that the quantity or date at [start, start + length) of `text` is the
// fact of row `row`, with every proof its place gives: what the transform
// replaces it with, and what the next writing of the same fact is found by.
void register_fact_place(OperationProtectedSymbolTable &symbols, const std::string &text, std::size_t start,
		std::size_t length, std::size_t row, const FactDecision &decision) {
	WrittenText &written = written_text(symbols, text);
	const auto same = std::find_if(written.facts.begin(), written.facts.end(), [&](const FactPlace &place) {
		return place.start == start && place.length == length;
	});
	if (same != written.facts.end()) same->row = row;
	else written.facts.push_back(FactPlace{start, length, row});
	RequestMappingEntry &entry = symbols.mapping[row];
	const std::string placeKey = fact_key_of(decision.provenance);
	if (entry.fact_key.empty()) {
		entry.fact_key = decision.anchored_key.empty() ? placeKey : decision.anchored_key;
		entry.provenance = decision.provenance;
		symbols.fact_by_key.emplace(entry.fact_key, row);
	} else if (!entry.derived && entry.provenance.derivation.empty() && entry.provenance.origin != FactProvenance::Origin::Derived
			&& entry.provenance.passage_sha256.empty() && entry.provenance.cell.empty() && !entry.provenance.anchored
			&& (!decision.provenance.passage_sha256.empty() || !decision.provenance.cell.empty())) {
		// A fact first met outside the documents - the Matter's memory, an
		// earlier answer, the lawyer's request - and then found in a document's
		// own passage stands on that passage: the answer that cites it is
		// sourced. Kept as first met, it read as untraced and cost the lawyer a
		// repair turn and the sentence (live, 11 Oct 2026: five turns for one
		// price summary, the survival periods cut).
		entry.provenance = decision.provenance;
	}
	for (const std::string &key : {placeKey, decision.anchored_key}) {
		if (key.empty() || key == entry.fact_key) continue;
		if (symbols.fact_by_key.emplace(key, row).second) entry.unified_keys.push_back(key);
	}
	if (!decision.statement_key.empty()) symbols.fact_by_key.emplace(decision.statement_key, row);
	if (!decision.role_key.empty()) symbols.fact_by_role.emplace(decision.role_key, row);
	// A key two facts could answer to names neither.
	const auto unique = [&](const std::string &key) {
		if (key.empty()) return;
		const auto [found, added] = symbols.fact_by_role.emplace(key, row);
		if (!added && found->second != row) found->second = static_cast<std::size_t>(-1);
	};
	unique(decision.named_role_key);
	unique(decision.tie_key);
	if (decision.table_tie && !decision.tie_key.empty()) {
		const std::optional<SourceBlock> block = source_block_around(text, start, start + length);
		symbols.table_passages.emplace(decision.provenance.passage_sha256,
			platform::folded_text(block ? text.substr(block->from, block->to - block->from) : text));
	}
	// A key two cells could answer to names neither.
	const auto uniqueCell = [&](const std::string &key) {
		if (key.empty()) return;
		const auto [found, added] = symbols.fact_by_cell.emplace(key, row);
		if (!added && found->second != row) found->second = static_cast<std::size_t>(-1);
	};
	// A workbook's cell within its workbook; and by its sheet and cell alone
	// ("Ledger!B19") while only one workbook read has it - what prose names.
	if (!decision.cell.empty()) {
		symbols.fact_by_cell.emplace(cell_key_of(decision.scope, decision.cell), row);
		uniqueCell(decision.cell);
		// The cell prose named before any sheet was read is this one now; another
		// sheet's cell of that name is not.
		const auto named = symbols.fact_by_cell.find("named:" + decision.bare_cell);
		if (named != symbols.fact_by_cell.end() && named->second == row) named->second = static_cast<std::size_t>(-1);
	}
	// The cell a formula refers to is in its own workbook. A cell prose names:
	// by its sheet and cell as written; one with no sheet ("cell B19") also as
	// named, for the sheet read after it.
	if (!decision.referenced_cell.empty()) {
		if (!decision.cell.empty()) {
			symbols.fact_by_cell.emplace(cell_key_of(decision.scope, decision.referenced_cell), row);
		} else {
			uniqueCell(decision.referenced_cell);
			if (decision.referenced_cell.find('!') == std::string::npos) uniqueCell("named:" + decision.referenced_cell);
		}
	}
	// A cell by its bare reference ("B19") only while one sheet has it: two
	// sheets' B19 are two cells, and prose naming "B19" then names neither.
	if (!decision.bare_cell.empty()) {
		const auto [found, added] = symbols.fact_by_cell.emplace("bare:" + decision.bare_cell, row);
		if (!added && found->second != row) found->second = static_cast<std::size_t>(-1);
	}
}

// Keeps `facts_by_value` - every fact by its value, for finding writings of a
// value and for local equality, never identity - in step with row `row`.
void index_fact_value(OperationProtectedSymbolTable &symbols, std::size_t row, const std::string &previousKey) {
	if (!previousKey.empty())
		for (auto found = symbols.facts_by_value.lower_bound(previousKey);
				found != symbols.facts_by_value.end() && found->first == previousKey; ++found)
			if (found->second == row) {
				symbols.facts_by_value.erase(found);
				break;
			}
	const RequestMappingEntry &entry = symbols.mapping[row];
	if (entry.value_key.empty()) return;
	for (auto found = symbols.facts_by_value.lower_bound(entry.value_key);
			found != symbols.facts_by_value.end() && found->first == entry.value_key; ++found)
		if (found->second == row) return;
	symbols.facts_by_value.emplace(entry.value_key, row);
}

// Whether a detected span is a quantity or a date whose value was read, and so
// a fact rather than a name or a code.
bool is_fact(const CanonicalEntity &entity, const std::optional<semantic::Value> &value) {
	if (!value || entity.source_label == kOperandSymbolLabel) return false;
	if (value->kind == semantic::Kind::Boolean) return false;
	return quantity_type(entity.type) || entity.source_label == kOperandLabel || entity.source_label == kFactAnchorLabel;
}

// The row the transform writes for a quantity or date the collection read at
// this very place; else an anchor's fact; else the one fact of that value.
// Without an attribution and with several facts of the value, the first of
// them: the value is masked either way, and the repair after the transform
// never lets a writing go.
std::optional<std::size_t> frozen_fact_row(const std::string &text, const CanonicalEntity &entity,
		const std::optional<semantic::Value> &value, const OperationProtectedSymbolTable &symbols) {
	const WrittenText &written = written_text(symbols, text);
	for (const FactPlace &place : written.facts)
		if (place.start == entity.start && place.length == entity.length && place.row < symbols.mapping.size())
			return place.row;
	if (!entity.fact_key.empty()) {
		const auto found = symbols.fact_by_key.find(entity.fact_key);
		if (found != symbols.fact_by_key.end()) return found->second;
	}
	for (const FactPlace &place : written.facts)
		if (place.start <= entity.start && entity.start + entity.length <= place.start + place.length
				&& place.row < symbols.mapping.size() && value && row_value_agrees(symbols.mapping[place.row], *value, entity.value_key))
			return place.row;
	if (!value) return std::nullopt;
	const std::string key = entity.value_key.empty() ? semantic::value_key(*value) : entity.value_key;
	std::optional<std::size_t> first;
	for (std::size_t row = 0; row < symbols.mapping.size(); ++row)
		if (!symbols.mapping[row].derived && row_value_agrees(symbols.mapping[row], *value, key)) {
			first = row;
			break;
		}
	if (!first)
		for (std::size_t row = 0; row < symbols.mapping.size(); ++row)
			if (row_value_agrees(symbols.mapping[row], *value, key)) return row;
	return first;
}

// Every SOURCE block of a text protected as one (a whole prompt), filed under
// its document by the block's handle (file_passages_under_documents).
void file_source_blocks_under_documents(const std::string &text, OperationProtectedSymbolTable &symbols) {
	static const std::string kMarker = "[SOURCE:DOC_";
	if (text.find(kMarker) == std::string::npos) return;
	std::vector<std::pair<std::string, std::string>> passages;
	for (std::size_t at = text.find(kMarker); at != std::string::npos; at = text.find(kMarker, at + 1)) {
		const std::size_t lineEnd = text.find('\n', at);
		if (lineEnd == std::string::npos) break;
		const std::optional<SourceBlock> block = source_block_around(text, lineEnd + 1, lineEnd + 1);
		if (!block || block->from != lineEnd + 1) continue;
		passages.emplace_back(block->document, platform::sha256_hex_digest(text.substr(block->from, block->to - block->from)));
	}
	file_passages_under_documents(symbols, passages);
}

// A name built on an organisation's name and written on to a legal form -
// "Corvane Growth Fund II, L.P." beside the sponsor "Corvane" - is a name of
// its own (#245): the model saw only the sponsor's word there, and the rest of
// the fund's name went out beside the sponsor's symbol. Only capitalised words
// on one line, at least one of them, up to the legal form; a capitalised word
// that ends no name ("Corvane Board members") leaves the value as it was.
// `entities` is reconcile()'s result: in order, none overlapping another.
void extend_to_legal_form(const std::string &text, std::vector<CanonicalEntity> &entities) {
	const auto joiningWord = [](const std::string &word) { return word == "of" || word == "and" || word == "&"; };
	const auto nameWord = [&](const std::string &word) {
		if (joiningWord(word)) return true;
		if (word.front() < 'A' || word.front() > 'Z' || word.back() == '.') return false;
		return std::all_of(word.begin(), word.end(), [](char character) {
			return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '&' || character == '-'
				|| character == '\'';
		});
	};
	const auto ends = [](char character) {
		return character == ' ' || character == ',' || character == '\n' || character == '\r' || character == '\t'
			|| character == '(' || character == ')' || character == ';' || character == ':' || character == '"';
	};
	for (std::size_t item = 0; item < entities.size(); ++item) {
		CanonicalEntity &entity = entities[item];
		if (!organisation_type(entity.type) || entity.source == DetectionSource::CustomTerm) continue;
		std::size_t cursor = entity.start + entity.length;
		std::size_t end = std::string::npos;
		std::size_t words = 0;
		bool joining = false;
		for (int step = 0; step < 8 && cursor < text.size(); ++step) {
			std::size_t next = cursor;
			const bool comma = text[next] == ',';
			if (comma) ++next;
			if (next >= text.size() || text[next] != ' ') break;
			++next;
			std::size_t wordEnd = next;
			while (wordEnd < text.size() && !ends(text[wordEnd])) ++wordEnd;
			if (wordEnd == next) break;
			const std::string word = text.substr(next, wordEnd - next);
			std::string undotted;
			for (const char character : word)
				if (character != '.') undotted.push_back(character);
			if (words > 0 && !joining && !undotted.empty() && legal_form_word(undotted)) {
				// "L.P." and "Inc." keep their own full stops; one after "LLC" ends the sentence.
				const bool sentenceStop = word.back() == '.' && word.find('.') == word.size() - 1
					&& word != "Inc." && word != "Corp." && word != "Co." && word != "Ltd.";
				end = sentenceStop ? wordEnd - 1 : wordEnd;
				break;
			}
			if (comma || !nameWord(word)) break;
			joining = joiningWord(word);
			++words;
			cursor = wordEnd;
		}
		if (end == std::string::npos) continue;
		if (item + 1 < entities.size() && entities[item + 1].start < end) continue;
		entity.length = end - entity.start;
		entity.value = text.substr(entity.start, entity.length);
		entity.value_key.clear();
	}
}

std::vector<CanonicalEntity> reconcile_into_mapping(
    const std::string &text, std::vector<CanonicalEntity> candidates,
    OperationProtectedSymbolTable &symbols, bool user_authored = false, bool model_authored = false,
    const FactContext &facts = {}) {
  obs::emit({obs::OperationEventKind::PapinianReconcile,
      "Resolving overlapping and repeated sensitive values",
      {{"candidates", std::to_string(candidates.size())}}, std::nullopt});
  // A prompt read as one text: each SOURCE block filed under its document.
  file_source_blocks_under_documents(text, symbols);
  auto entities = expand_detected_values_to_all_occurrences(text, candidates);
  auto projectEntities = known_project_entities(text, symbols);
  entities.insert(entities.end(), projectEntities.begin(), projectEntities.end());
  auto knownEntities = known_operation_entities(text, symbols, true);
  entities.insert(entities.end(), knownEntities.begin(), knownEntities.end());
  let_values_take_their_writings(entities);
  entities = reconcile(text, std::move(entities));
  extend_to_legal_form(text, entities);
  // What a statement's ⟦…⟧ holds is one operand, whatever else was found in it.
  put_operands_in(entities, text, symbols);
  // And what an earlier answer's anchor marks is the fact it names.
  put_anchor_marks_in(entities, text);
  drop_inside_symbol_tokens(entities, text);
  std::vector<std::optional<semantic::Value>> values(entities.size());
  for (std::size_t item = 0; item < entities.size(); ++item) values[item] = read_entity_value(text, entities[item]);
  std::size_t processed = 0;
  obs::emit({obs::OperationEventKind::PapinianMappingBuild,
      "Building request-only protection mapping",
      {{"processed", "0"}, {"total", std::to_string(entities.size())}}, std::nullopt});
  MappingIndex index(symbols);
  std::set<std::string> issuedSymbols;
  for (const auto &entry : symbols.mapping) issuedSymbols.insert(entry.placeholder);
  // A person's surname written alone ("Hale", "Ms. Morgan", "Громову"), with
  // initials ("M. Delgado", «Лебедеву А. И.») or a first name alone ("Marcus")
  // is the person this operation already names in full. They are judged after
  // every other value of this text has its row - initials before a surname
  // alone - so the full name may stand anywhere in the text.
  std::map<std::string, std::size_t> rowBySurname;
  for (std::size_t row = 0; row < symbols.mapping.size(); ++row)
    for (const std::string &form : symbols.mapping[row].surname_forms) rowBySurname.emplace(form, row);
  std::vector<bool> surnameAlone(entities.size(), false);
  std::vector<bool> withInitials(entities.size(), false);
  // «Уважаемый Андрей Игоревич!»: a first name and a patronymic alone, weighed
  // once every full name of the text has its row.
  std::vector<bool> givenAndPatronymic(entities.size(), false);
  // The surnames this text names in full, in every form: initials written with
  // one of them wait for that full name; any others are read where they stand.
  std::set<std::string> namedInFull;
  for (const CanonicalEntity &entity : entities) {
    if (!written_in_full(entity)) continue;
    for (const std::string &form : surname_writings(person_name(entity.value).surname, person_gender(entity.value)))
      namedInFull.insert(form);
  }
  // An organisation's name written alone ("Meadowbrook") waits for the
  // organisation this text names in full, wherever that stands.
  std::set<std::string> shortNamesInText;
  for (const CanonicalEntity &entity : entities)
    if (organisation_type(entity.type))
      if (const std::string shortName = short_company_name(entity.value); !shortName.empty())
        shortNamesInText.insert(shortName);
  std::vector<bool> organisationShort(entities.size(), false);
  std::vector<bool> deferred(entities.size(), false);
  std::vector<std::size_t> order;
  order.reserve(entities.size());
  for (std::size_t item = 0; item < entities.size(); ++item) {
    surnameAlone[item] = written_as_surname_alone(entities[item]);
    withInitials[item] = !surnameAlone[item] && written_with_initials(entities[item]);
    givenAndPatronymic[item] = person_type(entities[item].type) && given_and_patronymic(entities[item].value).has_value();
    organisationShort[item] = organisation_type(entities[item].type) && shortNamesInText.count(entities[item].value) > 0;
    deferred[item] = surnameAlone[item] || givenAndPatronymic[item] || organisationShort[item]
        || (withInitials[item] && namedInFull.count(person_name(entities[item].value).surname) > 0);
    if (!deferred[item]) order.push_back(item);
  }
  for (std::size_t item = 0; item < entities.size(); ++item)
    if (organisationShort[item]) order.push_back(item);
  for (std::size_t item = 0; item < entities.size(); ++item)
    if (givenAndPatronymic[item]) order.push_back(item);
  for (std::size_t item = 0; item < entities.size(); ++item)
    if (deferred[item] && withInitials[item]) order.push_back(item);
  for (std::size_t item = 0; item < entities.size(); ++item)
    if (surnameAlone[item]) order.push_back(item);
  // The surname lookup is brought up to date only when the table has changed
  // since it was last read: one read serves every surname of the text.
  std::size_t changes = 0;
  std::size_t surnamesReadAt = static_cast<std::size_t>(-1);
  for (const std::size_t item : order) {
    const CanonicalEntity &entity = entities[item];
    // An operand that is already a symbol of this operation stays that symbol.
    if (entity.source_label == kOperandSymbolLabel) continue;
    const bool operand = entity.source_label == kOperandLabel;
    // An amount, a rate, a count, a date or a period is a fact of the material,
    // found by where and in what role it stands - never by its value alone.
    const bool fact = is_fact(entity, values[item]);
    const ProjectEntityResolution projectEntity = fact ? ProjectEntityResolution{}
        : project_entity_for_value(symbols, entity.value);
    std::optional<std::size_t> row;
    FactDecision factDecision;
    if (fact) {
      factDecision = attribute_fact(text, entity, *values[item], semantic::value_key(*values[item]), symbols, facts);
      row = factDecision.row;
    }
    // Initials and a surname name the one person here whose name they agree
    // with, before a case form of another writing is weighed: «Кузнецовой
    // А. П.» is only Анна Павловна, never the symbol «Кузнецова А. П.» took
    // while it could be either of two people (a verifier's run, 25 Sep 2026).
    if (!fact && withInitials[item] && !projectEntity.entity_id && !projectEntity.ambiguous) {
      if (surnamesReadAt != changes) refresh_surname_index(symbols);
      surnamesReadAt = changes;
      const PersonName name = person_name(entity.value);
      row = surname_owner(symbols, name.surname, entity.type, &name).row;
    }
    if (!fact && !row) row = index.row_of(entity, projectEntity);
    if (!fact && !row) {
      const auto recorded = rowBySurname.find(entity.value);
      if (recorded != rowBySurname.end()) row = recorded->second;
    }
    if (!fact && !row && operand) row = operand_row_by_writing(entity.value, symbols, index);
    // An organisation's name written alone is the one organisation here known by it.
    if (!fact && !row && organisation_type(entity.type) && !projectEntity.entity_id && !projectEntity.ambiguous)
      row = short_name_owner(short_name_owners(symbols), entity.value);
    if (!fact && !row && givenAndPatronymic[item] && !projectEntity.entity_id && !projectEntity.ambiguous)
      row = given_patronymic_owner(symbols, entity.value);
    // Only where the Project's registry has nothing to say: a name it knows is
    // the entity it records, never whoever else carries that surname.
    if (!fact && !row && (surnameAlone[item] || withInitials[item]) && !projectEntity.entity_id && !projectEntity.ambiguous) {
      if (surnamesReadAt != changes) refresh_surname_index(symbols);
      surnamesReadAt = changes;
      const PersonName name = person_name(entity.value);
      if (withInitials[item]) {
        // The same person written another way: masked and restored as one.
        row = surname_owner(symbols, name.surname, entity.type, &name).row;
      } else {
        SurnameOwner owner = surname_owner(symbols, name.surname, entity.type);
        if (!owner.row && !owner.shared) owner.row = given_name_owner(symbols, entity.value, entity.type);
        if (owner.row) {
          symbols.mapping[*owner.row].surname_forms.push_back(entity.value);
          rowBySurname.emplace(entity.value, *owner.row);
          row = owner.row;
          ++changes;
        }
      }
    }
    auto found = row ? symbols.mapping.begin() + static_cast<std::ptrdiff_t>(*row) : symbols.mapping.end();
    if (found != symbols.mapping.end()) {
      // A surname alone is not a writing of the full name: it is replaced only
      // as a word, and never becomes the spelling an answer is written in.
      const bool surnameOfRow = entity.value != found->original
          && std::find(found->surname_forms.begin(), found->surname_forms.end(), entity.value)
              != found->surname_forms.end();
      // Nor is an organisation's short name: it is written back as it was
      // written ([[E3.short]]), and the full name stays the row's spelling.
      const bool shortOfRow = !surnameOfRow && !operand
          && (written_as_short_name(*found, entity.value) || short_name_of_row(*found, entity.value));
      if (shortOfRow && std::find(found->short_forms.begin(), found->short_forms.end(), entity.value) == found->short_forms.end())
        found->short_forms.push_back(entity.value);
      if (!surnameOfRow && !shortOfRow) {
        found->count_writing(entity.value);
        if (!operand)
          found->writing_origins[entity.value] |=
              writing_origin(text, entity.start, entity.length, user_authored, model_authored);
      }
      if (person_type(found->type) && found->honorific.empty()) found->honorific = honorific_before(text, entity.start);
      if (person_type(found->type) && !operand && row_person_gender(*found) == russian_names::Gender::Unknown)
        found->gender = gender_in_sentence(text, entity.start, entity.length);
      // A value this row did not know yet, or knew without its currency: the
      // bare cell "18400000" is rubles once "18 400 000 рублей" joins it.
      if (values[item] && quantity_type(found->type)
          && (!found->value || (currency_unknown(*found->value) && values[item]->kind == semantic::Kind::Money
              && !currency_unknown(*values[item])))) {
        const std::string previousKey = found->value_key;
        found->value = values[item];
        found->value_key = semantic::value_key(*values[item]);
        index_fact_value(symbols, *row, previousKey);
      }
      // A rate first met with no period, and now with one: "18%" and "18% per annum".
      else if (values[item] && found->value && found->value->kind == semantic::Kind::Percentage
          && found->value->basis == semantic::RateBasis::None && values[item]->kind == semantic::Kind::Percentage
          && found->value_key == semantic::value_key(*values[item]))
        found->value->basis = values[item]->basis;
      if (fact) register_fact_place(symbols, text, entity.start, entity.length, *row, factDecision);
      // The operation table owns identities, not spellings: one legal identity
      // is one row and one symbol, and its first spelling stays the value
      // restoration writes. But a spelling judged to BE that identity must be
      // replaced like it - the name in capitals, without its comma, in another
      // case is found by neither the first spelling nor a recorded alias, and
      // was sent to the provider readable while the leak check, looking only
      // for the first spelling, passed. The row keeps every such spelling.
      if (entity.value != found->original && !entity.value.empty() && !surnameOfRow && !shortOfRow) {
        // An identity is one row and one symbol whichever way it is spelled.
        // Which spelling the answer comes back in is a separate question, and
        // the right answer is the person's own: a request that says "Bluebonnet
        // Health Plan" must not come back saying "BLUEBONNET HEALTH PLAN, INC."
        // because a signature block in a document was read first. The person's
        // spelling takes the row's `original` once, and a document met later
        // never displaces it; the spelling it replaces stays a surface form, so
        // masking and the leak checks still cover every writing.
        // The person's words win; the record's capitals do. A request typed in
        // a hurry - "the ashgrove recapture thing", "change of control" - is
        // the same identity, but it is not how a name or a defined term is
        // written, and an answer that takes its spelling from the chat window
        // carries lower-case party names into draft agreement language. A
        // casual writing is kept as a surface form (so masking and the leak
        // checks still cover it) and leaves the row's spelling alone. An
        // operand is Singularity's own restored writing, never the person's.
        ++changes;
        const bool casualWriting = !has_ascii_upper(entity.value) && has_ascii_upper(found->original);
        if (user_authored && !operand && !found->original_is_user_authored && !casualWriting) {
          const std::string displaced = found->original;
          found->original = entity.value;
          found->original_is_user_authored = true;
          found->surface_forms.erase(
              std::remove(found->surface_forms.begin(), found->surface_forms.end(), entity.value),
              found->surface_forms.end());
          if (!displaced.empty()
              && std::find(found->surface_forms.begin(), found->surface_forms.end(), displaced)
                  == found->surface_forms.end())
            found->surface_forms.push_back(displaced);
        } else if (std::find(found->surface_forms.begin(), found->surface_forms.end(), entity.value)
              == found->surface_forms.end()) {
          found->surface_forms.push_back(entity.value);
        }
      }
    } else {
      RequestMappingEntry created{next_placeholder(symbols, issuedSymbols, text,
              markers::prefix_for(entity.type, values[item], entity.source), projectEntity.entity_id), entity.value, entity.type,
          entity.source, projectEntity.entity_id};
      created.original_is_user_authored = user_authored && !operand;
      created.count_writing(entity.value);
      if (!operand)
        created.writing_origins[entity.value] |=
            writing_origin(text, entity.start, entity.length, user_authored, model_authored);
      if (person_type(created.type)) created.honorific = honorific_before(text, entity.start);
      if (person_type(created.type) && !operand && row_person_gender(created) == russian_names::Gender::Unknown)
        created.gender = gender_in_sentence(text, entity.start, entity.length);
      if (values[item]) {
        created.value = values[item];
        created.value_key = semantic::value_key(*values[item]);
      }
      symbols.mapping.push_back(std::move(created));
      index.add(symbols.mapping.size() - 1);
      if (fact) {
        register_fact_place(symbols, text, entity.start, entity.length, symbols.mapping.size() - 1, factDecision);
        index_fact_value(symbols, symbols.mapping.size() - 1, std::string());
      }
      ++changes;
    }
    ++processed;
    if (processed % 256 == 0 || processed == entities.size())
      obs::emit({obs::OperationEventKind::PapinianMappingBuild,
          "Building request-only protection mapping",
          {{"processed", std::to_string(processed)}, {"total", std::to_string(entities.size())}},
          std::nullopt});
  }

  // A known person's surname written alone where nothing above found it: the
  // checkpoint takes "Victor Hale" and misses "Hale confirmed", takes "И.
  // Громов" and misses "Громову" (review, 23 Sep 2026). Only a capitalised
  // whole word equal to a writing of a surname already in this table, and
  // only where no detected value covers it - "brown" or "hale" in lowercase is
  // never one. Where a sentence begins, "Hale and hearty" cannot be told from
  // "Hale confirmed", and it is masked: a word hidden needlessly costs less
  // than a name sent.
  //
  // A surname followed by a first name, a patronymic or initials is a full
  // name: that person's when they agree with the name, otherwise another
  // person's, masked whole with a symbol of its own - «Кузнецова Анна
  // Павловна» is never Дмитрий Кузнецов's «Кузнецова» with «Анна Павловна»
  // left readable after it.
  //
  // What is found here is recorded on the person's row, which is what the
  // frozen pass replaces and the leak check looks for. It is deliberately not
  // returned as a detection: Project preprocessing records detections as
  // registry aliases, which match in any case, and a bare "Rich" learnt that
  // way would mask the word "rich" in every later request of the Project.
  refresh_surname_index(symbols);
  if (!symbols.surname_rows_by_form.empty()) {
    const std::vector<WordSpan> words = word_spans(text);
    const std::vector<OperandSpan> operands = operand_spans(text);
    for (std::size_t word = 0; word < words.size(); ++word) {
      if (!words[word].capitalised) continue;
      const std::size_t start = words[word].start;
      std::size_t end = words[word].end;
      // A surname in two parts is looked for whole first: "Hale-Smith", "O'Neil".
      if (word + 1 < words.size() && joined_by_one_mark(text, words[word], words[word + 1])
          && symbols.surname_rows_by_form.count(text.substr(start, words[word + 1].end - start)) > 0)
        end = words[++word].end;
      const std::string form = text.substr(start, end - start);
      const bool covered = std::any_of(entities.begin(), entities.end(), [&](const CanonicalEntity &entity) {
        return ranges_overlap(start, end - start, entity.start, entity.length);
      });
      if (symbols.surname_rows_by_form.count(form) == 0) {
        if (covered || overlaps_operand(operands, start, end - start)) continue;
        // A first name and a patronymic - «Анна Павловна», the way Singularity
        // itself writes a salutation back - are the one person here with
        // both. Sent again in history, «Уважаемая Анна Павловна!» went out
        // readable beside the person's symbol (a verifier's run, 25 Sep 2026).
        if (word + 1 < words.size()) {
          const WordSpan &next = words[word + 1];
          const std::string between = text.substr(end, next.start - end);
          const std::string pair = text.substr(start, next.end - start);
          const bool nextCovered = std::any_of(entities.begin(), entities.end(), [&](const CanonicalEntity &entity) {
            return ranges_overlap(next.start, next.end - next.start, entity.start, entity.length);
          });
          if ((between == " " || between == "\xC2\xA0") && !nextCovered && given_and_patronymic(pair)) {
            if (const std::optional<std::size_t> owner = given_patronymic_owner(symbols, pair)) {
              RequestMappingEntry &person = symbols.mapping[*owner];
              const std::vector<std::string> writings = person.every_form();
              if (std::find(writings.begin(), writings.end(), pair) == writings.end()) person.surface_forms.push_back(pair);
              ++word;
              continue;
            }
          }
        }
        // A first name alone - "Call Marcus at …" - is the one person here
        // with that first name, found the way a surname alone is: the
        // checkpoint that missed it once misses it wherever it stands.
        if (!stands_as_given_name(text, words, word)) continue;
        bool shared = false;
        std::optional<std::size_t> owner = given_name_owner(symbols, form, std::nullopt, &shared);
        // Two people here are called so: the first name alone keeps a symbol
        // of its own, as a surname two people share does - never readable.
        if (!owner && shared) {
          const CanonicalEntity given{start, end - start, EntityType::Person, 1.0, DetectionSource::Deterministic,
              "person_given_name", form, {}};
          owner = index.row_of(given, project_entity_for_value(symbols, form));
          if (!owner) {
            RequestMappingEntry created{next_placeholder(symbols, issuedSymbols, text, "Person"), form, EntityType::Person,
                DetectionSource::Deterministic, std::nullopt};
            created.original_is_user_authored = user_authored;
            created.count_writing(form);
            // Found as a whole word only, as a surname alone is.
            created.surname_forms.push_back(form);
            symbols.mapping.push_back(std::move(created));
            index.add(symbols.mapping.size() - 1);
            owner = symbols.mapping.size() - 1;
          }
        }
        if (!owner) continue;
        RequestMappingEntry &person = symbols.mapping[*owner];
        const std::vector<std::string> writings = person.every_form();
        if (std::find(writings.begin(), writings.end(), form) == writings.end()
            && std::find(person.surname_forms.begin(), person.surname_forms.end(), form) == person.surname_forms.end())
          person.surname_forms.push_back(form);
        continue;
      }
      if (covered || overlaps_operand(operands, start, end - start)) continue;
      if (const std::optional<FollowingName> following = name_words_after(text, end)) {
        const std::string whole = text.substr(start, following->end - start);
        PersonName written = following->name;
        std::optional<std::size_t> row = surname_owner(symbols, form, std::nullopt, &written).row;
        const CanonicalEntity named{start, whole.size(), EntityType::Person, 1.0, DetectionSource::Deterministic,
            "person_name", whole, {}};
        if (!row) row = index.row_of(named, project_entity_for_value(symbols, whole));
        if (!row) {
          RequestMappingEntry created{next_placeholder(symbols, issuedSymbols, text, "Person"), whole, EntityType::Person,
              DetectionSource::Deterministic, std::nullopt};
          created.original_is_user_authored = user_authored;
          created.count_writing(whole);
          symbols.mapping.push_back(std::move(created));
          index.add(symbols.mapping.size() - 1);
          refresh_surname_index(symbols);
        } else {
          RequestMappingEntry &owner = symbols.mapping[*row];
          if (whole != owner.original
              && std::find(owner.surface_forms.begin(), owner.surface_forms.end(), whole) == owner.surface_forms.end())
            owner.surface_forms.push_back(whole);
        }
        while (word + 1 < words.size() && words[word + 1].start < following->end) ++word;
        continue;
      }
      std::optional<std::size_t> row = surname_owner(symbols, form).row;
      if (!row) {
        // Shared by two people: the surname alone keeps a symbol of its own.
        const CanonicalEntity shared{start, end - start, EntityType::Person, 1.0, DetectionSource::Deterministic,
            "person_surname", form, {}};
        row = index.row_of(shared, project_entity_for_value(symbols, form));
      }
      if (!row) {
        RequestMappingEntry created;
        created.placeholder = next_placeholder(symbols, issuedSymbols, text, "Person");
        created.original = form;
        created.type = EntityType::Person;
        created.source = DetectionSource::Deterministic;
        created.original_is_user_authored = user_authored;
        created.count_writing(form);
        symbols.mapping.push_back(std::move(created));
        index.add(symbols.mapping.size() - 1);
        row = symbols.mapping.size() - 1;
      }
      RequestMappingEntry &owner = symbols.mapping[*row];
      if (std::find(owner.surname_forms.begin(), owner.surname_forms.end(), form) == owner.surname_forms.end())
        owner.surname_forms.push_back(form);
    }
  }

  // The short name the text itself defines for an organisation it names in
  // full, whatever words the full name is made of (defined_short_name_after):
  // recorded on its row, so the search below finds every writing of it.
  for (RequestMappingEntry &entry : symbols.mapping) {
    if (!organisation_type(entry.type)) continue;
    for (const std::string &writing : entry.every_form()) {
      if (writing.empty()) continue;
      for (std::size_t at = text.find(writing); at != std::string::npos; at = text.find(writing, at + writing.size())) {
        const std::string defined = defined_short_name_after(text, at + writing.size(), writing);
        if (!defined.empty() && defined != entry.original
            && std::find(entry.short_forms.begin(), entry.short_forms.end(), defined) == entry.short_forms.end())
          entry.short_forms.push_back(defined);
      }
    }
    // Its first word inside a quoted label names it too, however its full name
    // runs on: "Ferncastle" of the reference "Ferncastle closing", where its
    // words give "Ferncastle Sensors" (round 6). Never a word a person here
    // bears, and never outside quotes, where "Johnson signed as witness" stays
    // as the model read it.
    const std::string first = first_distinctive_company_word(entry.original);
    if (!first.empty() && symbols.surname_rows_by_form.count(first) == 0 && quoted_label_holds(text, first)
        && std::find(entry.short_forms.begin(), entry.short_forms.end(), first) == entry.short_forms.end())
      entry.short_forms.push_back(first);
  }

  // An organisation's short name written alone where nothing above found it:
  // the checkpoint takes "Meadowbrook Industrial Partners LLC" and misses
  // "Meadowbrook demands payment" and the file "Lease - Meadowbrook Cold
  // Storage (executed June 2, 2025).docx" (a live run, 25 Sep 2026). Only a
  // capitalised whole word (or the quoted name) that one organisation here is
  // known by, where no detected value covers it and no person here bears it as
  // a surname. Two organisations known by it leave it a symbol of its own, and
  // neither keeps it. Recorded on the row, as a surname alone is, and not
  // returned as a detection.
  {
    const std::map<std::string, std::vector<std::size_t>> owners = short_name_owners(symbols);
    for (const auto &[shortName, rows] : owners) {
      if (rows.size() < 2) continue;
      for (RequestMappingEntry &entry : symbols.mapping)
        entry.short_forms.erase(std::remove(entry.short_forms.begin(), entry.short_forms.end(), shortName),
            entry.short_forms.end());
    }
    if (!owners.empty()) {
      const std::vector<WordSpan> words = word_spans(text);
      const std::vector<OperandSpan> operands = operand_spans(text);
      for (std::size_t word = 0; word < words.size(); ++word) {
        if (!words[word].capitalised) continue;
        const std::size_t start = words[word].start;
        // Only the names that begin with this word's first byte.
        for (auto candidate = owners.lower_bound(std::string(1, text[start]));
             candidate != owners.end() && candidate->first[0] == text[start]; ++candidate) {
          const std::vector<std::size_t> &rows = candidate->second;
          // As the name is written, or in capitals ("MEADOWBROOK shall notify").
          const std::string &name = candidate->first;
          const std::string shouted = upper_case(name);
          const bool same = text.compare(start, name.size(), name) == 0;
          if ((!same && (shouted == name || text.compare(start, shouted.size(), shouted) != 0))
              || !stands_as_short_name(text, start, name.size()))
            continue;
          const std::string shortName = text.substr(start, name.size());
          const std::size_t end = start + shortName.size();
          // "Brightline Capital LLC": the short name opens another company's
          // full name, which is no writing of this one.
          if (opens_another_company_name(text, end)) {
            while (word + 1 < words.size() && words[word + 1].start < end) ++word;
            break;
          }
          // "Northwind Freight Systems" of "Northwind Freight Systems LLC": the
          // name a text writes without its legal form is the whole name, not
          // its short part with an ordinary word after it. Sent as the short
          // part, a live draft wrote the party back as "Northwind Freight
          // Systems" in its preamble and signature block (25 Sep 2026).
          std::string wholeName;
          if (rows.size() == 1)
            for (const std::string &writing : symbols.mapping[rows.front()].every_form()) {
              const std::string base = name_without_legal_form(writing);
              if (base.size() > shortName.size() && base.size() > wholeName.size()
                  && text.compare(start, base.size(), base) == 0 && stands_as_short_name(text, start, base.size()))
                wholeName = base;
            }
          const std::size_t writtenLength = wholeName.empty() ? shortName.size() : wholeName.size();
          const bool covered = overlaps_operand(operands, start, writtenLength)
              || std::any_of(entities.begin(), entities.end(), [&](const CanonicalEntity &entity) {
                   return ranges_overlap(start, writtenLength, entity.start, entity.length);
                 });
          if (!covered && !wholeName.empty()) {
            RequestMappingEntry &owner = symbols.mapping[rows.front()];
            if (wholeName != owner.original
                && std::find(owner.surface_forms.begin(), owner.surface_forms.end(), wholeName) == owner.surface_forms.end())
              owner.surface_forms.push_back(wholeName);
          } else if (!covered && symbols.surname_rows_by_form.count(shortName) == 0) {
            if (rows.size() == 1) {
              RequestMappingEntry &owner = symbols.mapping[rows.front()];
              if (shortName != owner.original
                  && std::find(owner.short_forms.begin(), owner.short_forms.end(), shortName) == owner.short_forms.end())
                owner.short_forms.push_back(shortName);
            } else {
              const CanonicalEntity shared{start, shortName.size(), EntityType::Organisation, 1.0,
                  DetectionSource::Deterministic, "organisation_short_name", shortName, {}};
              if (!index.row_of(shared, project_entity_for_value(symbols, shortName))) {
                RequestMappingEntry created{next_placeholder(symbols, issuedSymbols, text, "Org"), shortName,
                    EntityType::Organisation, DetectionSource::Deterministic, std::nullopt};
                created.original_is_user_authored = user_authored;
                created.count_writing(shortName);
                symbols.mapping.push_back(std::move(created));
                index.add(symbols.mapping.size() - 1);
              }
            }
          }
          while (word + 1 < words.size() && words[word + 1].start < start + std::max(writtenLength, end - start)) ++word;
          break;
        }
      }
    }
  }

  // What the frozen pass will ask this table for, asked now while it can still
  // answer. Applying a frozen mapping reconciles the values this table already
  // knows, and reconcile() grows every overlap to the union of the spans that
  // claimed it - so two known values that touch become one longer span whose
  // exact text is nobody's spelling. Collecting is the only place a mapping may
  // grow; a table that has never seen that span can only fail the request
  // closed later. Run the same reconciliation here and give each span it forms
  // a row, until nothing new forms (a handful of rounds at most - each round
  // only ever lengthens spans, and a text has a finite number of them).
  for (int round = 0; round < 4; ++round) {
    auto formed = known_operation_entities(text, symbols, true);
    auto formedProject = known_project_entities(text, symbols);
    formed.insert(formed.end(), formedProject.begin(), formedProject.end());
    let_values_take_their_writings(formed);
    formed = reconcile(text, std::move(formed));
    const std::vector<OperandSpan> operands = operand_spans(text);
    const std::vector<AnchorMarkSpan> anchorMarks = anchor_mark_spans(text);
    MappingIndex formedIndex(symbols);
    std::map<std::string, std::size_t> rowBySpelling;
    for (std::size_t row = 0; row < symbols.mapping.size(); ++row) {
      for (const std::string &form : symbols.mapping[row].surface_forms) rowBySpelling.emplace(form, row);
      for (const std::string &form : symbols.mapping[row].surname_forms) rowBySpelling.emplace(form, row);
      for (const std::string &form : symbols.mapping[row].short_forms) rowBySpelling.emplace(form, row);
    }
    bool added = false;
    for (auto &entity : formed) {
      if (overlaps_operand(operands, entity.start, entity.length)) continue;
      if (std::any_of(anchorMarks.begin(), anchorMarks.end(), [&](const AnchorMarkSpan &mark) {
            return entity.start < mark.end && mark.start < entity.start + entity.length;
          }))
        continue;
      const std::optional<semantic::Value> value = read_entity_value(text, entity);
      // A quantity or date this text writes is attributed like any other: a span
      // formed of two that touch is the fact its place proves, or a new one.
      if (is_fact(entity, value)) {
        const WrittenText &written = written_text(symbols, text);
        if (std::any_of(written.facts.begin(), written.facts.end(), [&](const FactPlace &place) {
              return place.start == entity.start && place.length == entity.length;
            }))
          continue;
        const std::string key = semantic::value_key(*value);
        const FactDecision decision = attribute_fact(text, entity, *value, key, symbols, facts);
        if (decision.row) {
          register_fact_place(symbols, text, entity.start, entity.length, *decision.row, decision);
          continue;
        }
        RequestMappingEntry created{next_placeholder(symbols, issuedSymbols, text,
                markers::prefix_for(entity.type, value, entity.source)), entity.value, entity.type,
            entity.source, std::nullopt};
        created.value = value;
        created.value_key = key;
        symbols.mapping.push_back(std::move(created));
        formedIndex.add(symbols.mapping.size() - 1);
        register_fact_place(symbols, text, entity.start, entity.length, symbols.mapping.size() - 1, decision);
        index_fact_value(symbols, symbols.mapping.size() - 1, std::string());
        added = true;
        continue;
      }
      const ProjectEntityResolution projectEntity = project_entity_for_value(symbols, entity.value);
      if (formedIndex.row_of(entity, projectEntity)) continue;
      if (rowBySpelling.find(entity.value) != rowBySpelling.end()) continue;
      RequestMappingEntry created{next_placeholder(symbols, issuedSymbols, text,
              markers::prefix_for(entity.type, value, entity.source), projectEntity.entity_id), entity.value, entity.type,
          entity.source, projectEntity.entity_id};
      if (value) {
        created.value = value;
        created.value_key = semantic::value_key(*value);
      }
      symbols.mapping.push_back(std::move(created));
      formedIndex.add(symbols.mapping.size() - 1);
      added = true;
    }
    if (!added) break;
  }
  return entities;
}

} // namespace

bool detection_may_be_recorded(EntityType type, const std::string &value) {
  // What is no name or value of its kind by its shape alone: a span cut out of
  // the sentence around a name, an address with no mailbox, a case number with
  // no figure (REGISTRY-3). A value the lawyer adds is never judged here.
  if (named_by_shape_alone(type, value)) return false;
  if (type == EntityType::Person)
    return !(is_generic_role_term(type, value) || is_officer_title(value) || is_public_office(value));
  if (type == EntityType::Judge)
    return !(is_officer_title(value) || is_generic_role_term(EntityType::Person, value) || is_public_office(value)
        || written_as_lowercase_prose(value));
  if (!prose_can_be_mistaken_for(type)) return true;
  if (written_as_lowercase_prose(value) || is_public_legal_vocabulary(type, value, ValueOrigin::Detected)) return false;
  // An identifier is a code. A title made only of ordinary words, with no
  // figure, is the kind of paper it is; a party's name in it is that party's.
  if (type == EntityType::ContractIdentifier && !contains_digit(value)) {
    std::string plain = plain_phrase(bare_term(value));
    std::replace(plain.begin(), plain.end(), '\'', ' ');
    if (!plain.empty() && ordinary_lowercase_words(plain)) return false;
  }
  return true;
}

bool recorded_value_names_identity(EntityType type, const std::string &value) {
  return detection_may_be_recorded(type, value) && !is_public_legal_vocabulary(type, value, ValueOrigin::RegistryAlias);
}

double ConfidenceThresholds::for_type(EntityType type) const {
  const auto found = per_type.find(type);
  return found == per_type.end() ? default_semantic : found->second;
}

double ConfidenceThresholds::lowest() const {
  double lowest = default_semantic;
  for (const auto &[type, threshold] : per_type) {
    static_cast<void>(type);
    lowest = std::min(lowest, threshold);
  }
  return lowest;
}

PapinianGuard::PapinianGuard(std::shared_ptr<const ISemanticModelAdapter> semantic_model,
                             GuardConfig config)
    : semantic_model_(std::move(semantic_model)), config_(std::move(config)) {
  if (config_.thresholds.default_semantic < 0.0 || config_.thresholds.default_semantic > 1.0) {
    throw std::invalid_argument("default semantic threshold must be in [0, 1]");
  }
  for (const auto &[type, threshold] : config_.thresholds.per_type) {
    static_cast<void>(type);
    if (threshold < 0.0 || threshold > 1.0) {
      throw std::invalid_argument("semantic threshold must be in [0, 1]");
    }
  }
}

ProtectionResult PapinianGuard::protect(const std::string &utf8_text, const Policy &policy,
                                        const std::string &request_namespace,
										OperationProtectedSymbolTable *operation_symbols) const {
  if (!valid_namespace(request_namespace)) {
    throw std::invalid_argument("request namespace must be 8-32 ASCII alphanumeric characters");
  }

  ProtectionResult result;
  result.protected_text = utf8_text;
  result.request_namespace = request_namespace;
  if (policy.mode == Mode::Off || utf8_text.empty()) return result;
	OperationProtectedSymbolTable localSymbols;
	OperationProtectedSymbolTable &symbols = operation_symbols == nullptr ? localSymbols : *operation_symbols;
	result.entities = collect_sensitive_values(utf8_text, policy, request_namespace, symbols);
	const OperationProtectedSymbolTable frozenSymbols = symbols;
	result.protected_text = apply_frozen_mapping(utf8_text, frozenSymbols);
	result.mapping = frozenSymbols.mapping;
  result.validation = validate_masking(result.protected_text, result.mapping);
  obs::emit({obs::OperationEventKind::PapinianPayloadReady, "Protected text validated",
      {{"outputBytes", std::to_string(result.protected_text.size())},
       {"maskingComplete", result.validation.masking_complete ? "true" : "false"}}, std::nullopt});
  return result;
}

std::vector<CanonicalEntity> PapinianGuard::collect_sensitive_values(
		const std::string &utf8_text, const Policy &policy,
		const std::string &request_namespace,
		OperationProtectedSymbolTable &symbols) const {
  if (!valid_namespace(request_namespace)) {
    throw std::invalid_argument("request namespace must be 8-32 ASCII alphanumeric characters");
  }
  if (policy.mode == Mode::Off || utf8_text.empty()) return {};
	if (symbols.operation_id.empty()) symbols.operation_id = request_namespace;
	if (symbols.marker_family.empty()) {
		symbols.marker_family = choose_marker_family(utf8_text);
		if (config_.marker_style && symbols.mapping.empty()) symbols.marker_style = *config_.marker_style;
		settle_marker_style(symbols, {utf8_text});
	} else reject_unissued_operation_markers(utf8_text, symbols);
	// Everything is judged on the text as it reads (reading_view).
	const std::string view = reading_view(utf8_text);

  obs::emit({obs::OperationEventKind::PapinianProtectStart, "Detecting protected values locally",
      {{"inputBytes", std::to_string(utf8_text.size())}}, std::nullopt});
  auto candidates = deterministic_entities(view, policy);
  if (config_.semantic_detection_enabled && semantic_model_) {
    std::vector<CanonicalEntity> semantic;
    try {
      obs::emit({obs::OperationEventKind::PapinianModelInference, "Running local semantic detection", {}, std::nullopt});
      semantic = semantic_entities(view, *semantic_model_, config_);
      obs::emit({obs::OperationEventKind::PapinianProgress, "Local semantic detection completed",
          {{"entities", std::to_string(semantic.size())}}, std::nullopt});
    } catch (const std::exception &error) {
      // PapinianGuard is model-neutral by design (see README) and never
      // depends on or names its pluggable semantic model adapter's backing
      // runtime to callers - the adapter's own failure detail (which model,
      // which artifact, which runtime) is a local debugging concern, logged
      // here, not something that should ever reach product/marketing-facing
      // text.
      std::cerr << "Papinian Guard: semantic model unavailable: " << error.what() << '\n';
      throw std::runtime_error("Papinian Guard's local model isn't available right now.");
    }
    candidates.insert(candidates.end(), semantic.begin(), semantic.end());
  }
  return reconcile_into_mapping(view, std::move(candidates), symbols, false, false,
      FactContext{FactProvenance::Origin::Source, platform::sha256_hex_digest(utf8_text)});
}

std::vector<CanonicalEntity> PapinianGuard::collect_known_values(
		const std::string &utf8_text, const Policy &policy,
		const std::string &request_namespace,
		OperationProtectedSymbolTable &symbols, FactProvenance::Origin origin) const {
	if (!valid_namespace(request_namespace)) {
		throw std::invalid_argument("request namespace must be 8-32 ASCII alphanumeric characters");
	}
	if (policy.mode == Mode::Off || utf8_text.empty()) return {};
	if (symbols.operation_id.empty()) symbols.operation_id = request_namespace;
	if (symbols.marker_family.empty()) {
		symbols.marker_family = choose_marker_family(utf8_text);
		if (config_.marker_style && symbols.mapping.empty()) symbols.marker_style = *config_.marker_style;
		settle_marker_style(symbols, {utf8_text});
	} else reject_unissued_operation_markers(utf8_text, symbols);
	// No semantic model, by construction: reconcile_into_mapping() adds this
	// Project's registered entities and this operation's own known values, and
	// deterministic_entities() the kinds that are recognised by their shape.
	const std::string view = reading_view(utf8_text);
	return reconcile_into_mapping(view, deterministic_entities(view, policy), symbols,
		origin == FactProvenance::Origin::Request, origin == FactProvenance::Origin::History,
		FactContext{origin, platform::sha256_hex_digest(utf8_text)});
}

namespace {

// An organisation met first by its short name and then by its full one is one
// row with two writings: the full name the row's own, the short name its short
// part ([[E3.short]]). Met in the other order - the registry's "Harlan" before
// the DPA's preamble - "Harlan" had become the row's spelling, its full name
// another writing of the same symbol, and every place the DPA wrote "Harlan"
// came back "Harlan Retail Group, Inc." (P0, 30 Oct 2026). A name written
// without only its legal form ("Northwind Freight Systems") stays a writing of
// the name itself. Settled only before the table first sends: what an already
// sent symbol stands for never changes.
void settle_organisation_rows(OperationProtectedSymbolTable &symbols) {
	if (!symbols.outbound_scope.empty()) return;
	const auto withForm = [](const std::string &writing) {
		for (const WordSpan &span : word_spans(writing))
			if (legal_form_word(writing.substr(span.start, span.end - span.start))) return true;
		return false;
	};
	for (RequestMappingEntry &row : symbols.mapping) {
		if (!organisation_type(row.type) || row.value || row.derived || withForm(row.original)) continue;
		// Its full writings: only those this operation's own material wrote - a
		// document's text or the lawyer's words. A spelling the Matter's registry
		// knows but no document of the request writes is never promoted: a stale
		// "Coastline Logistics Corp." once named a party the pleadings call
		// "Coastline Logistics, Inc." throughout a reply (P0, 30 Oct 2026).
		std::vector<std::string> candidates;
		for (const std::string &writing : row.surface_forms) {
			const auto origin = row.writing_origins.find(writing);
			if (origin != row.writing_origins.end()
					&& (origin->second & (RequestMappingEntry::DocumentText | RequestMappingEntry::LawyerRequest)) != 0)
				candidates.push_back(writing);
		}
		const std::string *full = nullptr;
		std::size_t fullTimes = 0;
		for (const std::string &writing : candidates) {
			if (!withForm(writing) || upper_case(writing) == writing || reads_as_cut_of_text(writing)) continue;
			const std::string base = name_without_legal_form(writing);
			if (base.size() <= row.original.size() + 1 || base.compare(0, row.original.size(), row.original) != 0
					|| base[row.original.size()] != ' ')
				continue;
			const std::size_t times = row.times_written(writing);
			if (full == nullptr || times > fullTimes) {
				full = &writing;
				fullTimes = times;
			}
		}
		if (full == nullptr) continue;
		const std::string promoted = *full;
		const std::string shortName = row.original;
		row.surface_forms.erase(std::remove(row.surface_forms.begin(), row.surface_forms.end(), promoted),
			row.surface_forms.end());
		row.original = promoted;
		// The short name, and its other writings ("HARLAN"), are the short part.
		std::vector<std::string> kept;
		for (const std::string &writing : row.surface_forms) {
			if (ascii_lower(writing) == ascii_lower(shortName)) {
				if (std::find(row.short_forms.begin(), row.short_forms.end(), writing) == row.short_forms.end())
					row.short_forms.push_back(writing);
			} else {
				kept.push_back(writing);
			}
		}
		row.surface_forms = std::move(kept);
		if (std::find(row.short_forms.begin(), row.short_forms.end(), shortName) == row.short_forms.end())
			row.short_forms.insert(row.short_forms.begin(), shortName);
	}
}

} // namespace

std::vector<CanonicalLeafDetection> PapinianGuard::collect_sensitive_values_batch(
    const std::vector<SemanticLeaf> &written_leaves, const Policy &policy,
    const std::string &request_namespace,
    OperationProtectedSymbolTable &symbols, ProtectionBatchMetrics *metrics) const {
  if (!valid_namespace(request_namespace)) {
    throw std::invalid_argument("request namespace must be 8-32 ASCII alphanumeric characters");
  }
  std::vector<CanonicalLeafDetection> results;
  results.reserve(written_leaves.size());
  if (policy.mode == Mode::Off || written_leaves.empty()) return results;
  if (symbols.operation_id.empty()) symbols.operation_id = request_namespace;
  if (symbols.marker_family.empty()) {
    symbols.marker_family = choose_marker_family(written_leaves);
    if (config_.marker_style && symbols.mapping.empty()) symbols.marker_style = *config_.marker_style;
    std::vector<std::string_view> texts;
    for (const auto &leaf : written_leaves) texts.emplace_back(leaf.text);
    settle_marker_style(symbols, texts);
  }
  for (const auto &leaf : written_leaves) reject_unissued_operation_markers(leaf.text, symbols);
  // Each passage as it reads (reading_view), for the model and for every
  // judgment after it; a passage without an escape is scored as it always was.
  std::vector<SemanticLeaf> leaves = written_leaves;
  for (SemanticLeaf &leaf : leaves) leaf.text = reading_view(leaf.text);

  std::vector<SemanticLeafDetection> semantic;
  if (config_.semantic_detection_enabled && semantic_model_) {
    const auto semanticStarted = std::chrono::steady_clock::now();
    // Reuse this operation's own prior raw detections for any leaf whose
    // text is byte-identical to one already scored this same operation
    // (exact string match only - see OperationProtectedSymbolTable::
    // semantic_detection_cache's own comment on why this is safe). Static or
    // slowly-growing prompt content (system/task instructions, already-
    // inspected evidence carried forward turn to turn) is the common case in
    // practice; only genuinely new/changed text ever reaches the model.
    std::vector<SemanticLeaf> uncachedLeaves;
    std::vector<std::size_t> uncachedPositions;
    // Leaf positions whose text is already pending under another position, with
    // the pending position they take their detections from.
    std::vector<std::pair<std::size_t, std::size_t>> repeatedPositions;
    semantic.resize(leaves.size());
    // Durable reuse across operations: the same exact text scored by the same
    // pinned model (SemanticDetectionStore's own comment on why this is safe).
    std::shared_ptr<const SemanticDetectionStore> store = process_semantic_detection_store();
    if (store && !store->serves(semantic_model_->identity())) store.reset();
    std::size_t reusedFromStore = 0;
    const bool traced = platform::trace::enabled();
    // Diagnostic trace only: which text was scored and where its detections
    // came from, as a size and a fingerprint - never the text itself.
    const auto traceLeaf = [&](const SemanticLeaf &leaf, const char *source) {
      if (!traced) return;
      platform::trace::record("ner", "leaf",
          {{"identity", leaf.identity}, {"bytes", leaf.text.size()},
           {"fingerprint", platform::trace::fingerprint(leaf.text)}, {"source", source},
           {"namespace", request_namespace}});
    };
    for (std::size_t index = 0; index < leaves.size(); ++index) {
      const auto cached = symbols.semantic_detection_cache.find(leaves[index].text);
      if (cached != symbols.semantic_detection_cache.end()) {
        semantic[index] = {leaves[index].identity, cached->second};
        if (metrics != nullptr) ++metrics->detections_from_operation;
        traceLeaf(leaves[index], "operationCache");
        continue;
      }
      if (store) {
        if (auto stored = store->load(leaves[index].text, config_.thresholds.lowest())) {
          symbols.semantic_detection_cache[leaves[index].text] = *stored;
          semantic[index] = {leaves[index].identity, std::move(*stored)};
          ++reusedFromStore;
          if (metrics != nullptr) ++metrics->detections_reused;
          traceLeaf(leaves[index], "store");
          continue;
        }
      }
      // The same text twice in one request - a passage carried in two sections,
      // a record repeated - is one thing to score. The operation's own cache is
      // filled only after the batch returns, so identical leaves inside one
      // batch would otherwise each go to the model.
      const auto alreadyPending = std::find_if(uncachedLeaves.begin(), uncachedLeaves.end(),
          [&](const SemanticLeaf &pending) { return pending.text == leaves[index].text; });
      if (alreadyPending != uncachedLeaves.end()) {
        if (metrics != nullptr) ++metrics->detections_from_operation;
        traceLeaf(leaves[index], "operationCache");
        repeatedPositions.emplace_back(index,
            static_cast<std::size_t>(alreadyPending - uncachedLeaves.begin()));
        continue;
      }
      traceLeaf(leaves[index], "model");
      if (metrics != nullptr) ++metrics->detections_computed;
      uncachedLeaves.push_back(leaves[index]);
      uncachedPositions.push_back(index);
    }
    if (reusedFromStore > 0) {
      obs::emit({obs::OperationEventKind::PapinianProgress,
          "Reused local semantic detections prepared earlier",
          {{"samples", std::to_string(reusedFromStore)}}, std::nullopt});
    }
    if (!uncachedLeaves.empty()) {
      std::vector<SemanticLeafDetection> freshlyDetected;
      try {
        obs::emit({obs::OperationEventKind::PapinianModelInference,
            "Running batched local semantic detection",
            {{"samples", std::to_string(uncachedLeaves.size())},
             {"reusedFromOperationCache", std::to_string(leaves.size() - uncachedLeaves.size())}}, std::nullopt});
        platform::trace::Span modelSpan("ner", "model.batch", {{"leaves", uncachedLeaves.size()}});
        if (modelSpan.active()) {
          std::size_t bytes = 0;
          for (const SemanticLeaf &leaf : uncachedLeaves) bytes += leaf.text.size();
          modelSpan.add("bytes", bytes);
          modelSpan.add("namespace", request_namespace);
        }
        freshlyDetected = semantic_model_->detect_batch(uncachedLeaves);
      } catch (const std::exception &error) {
        std::cerr << "Papinian Guard: semantic model unavailable: " << error.what() << '\n';
        throw std::runtime_error("Papinian Guard's local model isn't available right now.");
      }
      if (freshlyDetected.size() != uncachedLeaves.size())
        throw std::runtime_error("Papinian semantic batch returned the wrong leaf count");
      for (std::size_t uncachedIndex = 0; uncachedIndex < uncachedLeaves.size(); ++uncachedIndex) {
        if (freshlyDetected[uncachedIndex].identity != uncachedLeaves[uncachedIndex].identity)
          throw std::runtime_error("Papinian semantic batch changed canonical leaf identity");
        symbols.semantic_detection_cache[uncachedLeaves[uncachedIndex].text] = freshlyDetected[uncachedIndex].entities;
        if (store) store->save(uncachedLeaves[uncachedIndex].text, freshlyDetected[uncachedIndex].entities,
            config_.thresholds.lowest());
        semantic[uncachedPositions[uncachedIndex]] = std::move(freshlyDetected[uncachedIndex]);
      }
      for (const auto &[position, pending] : repeatedPositions) {
        const auto &detections = symbols.semantic_detection_cache[uncachedLeaves[pending].text];
        semantic[position] = {leaves[position].identity, detections};
      }
    }
    if (metrics) metrics->semantic_detection_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - semanticStarted).count();
  } else {
    for (const auto &leaf : leaves) semantic.push_back({leaf.identity, {}});
  }

  const auto mappingStarted = std::chrono::steady_clock::now();
  platform::trace::Span mappingSpan("ner", "mapping", {{"leaves", leaves.size()}, {"namespace", request_namespace}});
  std::vector<std::vector<CanonicalEntity>> leafCandidates(leaves.size());
  // What the documents of this request define is known in every part of it:
  // the lawyer's "basket" is the agreement's '(the "Basket")'.
  const std::set<std::string> definedTerms = terms_defined_in(leaves);
  for (std::size_t index = 0; index < leaves.size(); ++index) {
    if (semantic[index].identity != leaves[index].identity)
      throw std::runtime_error("Papinian semantic batch changed canonical leaf identity");
    const auto deterministicStarted = std::chrono::steady_clock::now();
    auto candidates = deterministic_entities(leaves[index].text, policy);
    const auto semanticFromStarted = std::chrono::steady_clock::now();
    auto semanticEntities = semantic_entities_from(leaves[index].text, semantic[index].entities, config_, &definedTerms,
        !leaves[index].user_authored && !leaves[index].model_authored);
    candidates.insert(candidates.end(), semanticEntities.begin(), semanticEntities.end());
    leafCandidates[index] = std::move(candidates);
    if (metrics != nullptr) {
      const auto now = std::chrono::steady_clock::now();
      metrics->deterministic_ms += std::chrono::duration<double, std::milli>(
          semanticFromStarted - deterministicStarted).count();
      metrics->project_match_ms += std::chrono::duration<double, std::milli>(now - semanticFromStarted).count();
    }
  }
  // Every person the batch names in full has a row before any surname, initials
  // or first name written alone is weighed: which passage is read first never
  // decides whose symbol «Кузнецова» takes.
  for (std::size_t index = 0; index < leaves.size(); ++index)
    register_full_names(leaves[index].text, leafCandidates[index], symbols, leaves[index].user_authored);
  for (std::size_t index = 0; index < leaves.size(); ++index) {
    const auto reconcileStarted = std::chrono::steady_clock::now();
    results.push_back({leaves[index].identity,
        reconcile_into_mapping(leaves[index].text, std::move(leafCandidates[index]), symbols,
            leaves[index].user_authored, leaves[index].model_authored,
            // Where each amount and date of this passage is stated: the digest of
            // the passage's own text, as the outbound record digests it.
            FactContext{leaves[index].user_authored ? FactProvenance::Origin::Request
                    : leaves[index].model_authored ? FactProvenance::Origin::History : FactProvenance::Origin::Source,
                platform::sha256_hex_digest(written_leaves[index].text)})});
    if (metrics != nullptr)
      metrics->reconcile_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - reconcileStarted).count();
  }
  if (metrics) metrics->mapping_build_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - mappingStarted).count();
  settle_organisation_rows(symbols);
  obs::emit({obs::OperationEventKind::PapinianProgress,
      "Batched local semantic detection completed",
      {{"samples", std::to_string(leaves.size())}}, std::nullopt});
  return results;
}

std::size_t PapinianGuard::warm_semantic_detections(const std::vector<std::string> &utf8_texts) const {
  if (!config_.semantic_detection_enabled || !semantic_model_) return 0;
  const std::shared_ptr<const SemanticDetectionStore> store = process_semantic_detection_store();
  if (!store || !store->serves(semantic_model_->identity())) return 0;
  constexpr std::size_t kBatch = 16;
  std::size_t scored = 0;
  std::vector<SemanticLeaf> pending;
  const auto flush = [&] {
    if (pending.empty()) return;
    platform::trace::Span modelSpan("ner", "model.batch", {{"leaves", pending.size()}, {"namespace", "warmup"}});
    if (modelSpan.active()) {
      std::size_t bytes = 0;
      for (const SemanticLeaf &leaf : pending) {
        bytes += leaf.text.size();
        platform::trace::record("ner", "leaf",
            {{"identity", leaf.identity}, {"bytes", leaf.text.size()},
             {"fingerprint", platform::trace::fingerprint(leaf.text)}, {"source", "model"},
             {"namespace", "warmup"}});
      }
      modelSpan.add("bytes", bytes);
    }
    const auto detected = semantic_model_->detect_batch(pending);
    if (detected.size() != pending.size())
      throw std::runtime_error("Papinian semantic batch returned the wrong leaf count");
    for (std::size_t index = 0; index < pending.size(); ++index) {
      store->save(pending[index].text, detected[index].entities, config_.thresholds.lowest());
    }
    scored += pending.size();
    pending.clear();
  };
  for (const std::string &written : utf8_texts) {
    // Scored as a send will score it: as the text reads (reading_view).
    const std::string text = reading_view(written);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
    if (store->contains(text, config_.thresholds.lowest())) continue;
    pending.push_back({"warm#" + std::to_string(scored + pending.size()), text});
    if (pending.size() == kBatch) flush();
  }
  flush();
  return scored;
}

// The table-wide part of masking with a frozen table: its index of rows and
// every spelling recorded for a row.
struct PapinianGuard::FrozenMapper::Prepared {
	MappingIndex index;
	// A spelling recorded for a row is that row, whatever its length - a
	// surname written alone included.
	std::map<std::string, std::size_t> row_by_spelling;

	explicit Prepared(const OperationProtectedSymbolTable &frozen_symbols) : index(frozen_symbols) {
		for (std::size_t row = 0; row < frozen_symbols.mapping.size(); ++row) {
			for (const std::string &form : frozen_symbols.mapping[row].surface_forms) row_by_spelling.emplace(form, row);
			for (const std::string &form : frozen_symbols.mapping[row].surname_forms) row_by_spelling.emplace(form, row);
			for (const std::string &form : frozen_symbols.mapping[row].short_forms) row_by_spelling.emplace(form, row);
		}
	}
};

PapinianGuard::FrozenMapper::FrozenMapper(const PapinianGuard &guard, const OperationProtectedSymbolTable &frozen_symbols)
	: guard_(&guard), symbols_(&frozen_symbols), prepared_(std::make_unique<Prepared>(frozen_symbols)) {}

PapinianGuard::FrozenMapper::FrozenMapper(FrozenMapper &&) noexcept = default;
PapinianGuard::FrozenMapper::~FrozenMapper() = default;

std::string PapinianGuard::FrozenMapper::apply(const std::string &utf8_text) const {
	if (utf8_text.empty() || symbols_->mapping.empty()) return utf8_text;
	return guard_->apply_frozen_mapping_with(utf8_text, *symbols_, *prepared_);
}

PapinianGuard::FrozenMapper PapinianGuard::frozen_mapper(const OperationProtectedSymbolTable &frozen_symbols) const {
	return FrozenMapper(*this, frozen_symbols);
}

std::string PapinianGuard::apply_frozen_mapping(
		const std::string &utf8_text,
		const OperationProtectedSymbolTable &frozen_symbols) const {
	if (utf8_text.empty() || frozen_symbols.mapping.empty()) return utf8_text;
	const FrozenMapper::Prepared prepared(frozen_symbols);
	return apply_frozen_mapping_with(utf8_text, frozen_symbols, prepared);
}

std::string PapinianGuard::apply_frozen_mapping_with(const std::string &utf8_text,
		const OperationProtectedSymbolTable &frozen_symbols, const FrozenMapper::Prepared &prepared) const {
	reject_unissued_operation_markers(utf8_text, frozen_symbols);
	// Found in the text as it reads (reading_view), replaced in the text as it
	// is written: the two have the same offsets.
	const std::string view = reading_view(utf8_text);
	auto entities = known_operation_entities(view, frozen_symbols);
	auto projectEntities = known_project_entities(view, frozen_symbols);
	entities.insert(entities.end(), projectEntities.begin(), projectEntities.end());
	let_values_take_their_writings(entities);
	entities = reconcile(view, std::move(entities));
	// A statement's ⟦…⟧ leaves as its symbol alone, the marks with it; so does
	// an earlier answer's anchor mark.
	put_operands_in(entities, view, frozen_symbols);
	put_anchor_marks_in(entities, view);
	drop_inside_symbol_tokens(entities, view);
	std::vector<std::optional<semantic::Value>> values(entities.size());
	for (std::size_t item = 0; item < entities.size(); ++item) values[item] = read_entity_value(view, entities[item]);

	std::string output;
	output.reserve(utf8_text.size());
	std::size_t cursor = 0;
	const MappingIndex &index = prepared.index;
	const std::map<std::string, std::size_t> &rowBySpelling = prepared.row_by_spelling;
	for (std::size_t item = 0; item < entities.size(); ++item) {
		const CanonicalEntity &entity = entities[item];
		output.append(utf8_text, cursor, entity.start - cursor);
		if (entity.source_label == kOperandSymbolLabel) {
			output += entity.value;
			cursor = entity.start + entity.length;
			continue;
		}
		// An amount or a date goes as the fact the collection found it to be,
		// read at these very bytes.
		if (is_fact(entity, values[item])) {
			const std::optional<std::size_t> row = frozen_fact_row(view, entity, values[item], frozen_symbols);
			if (row) {
				output += frozen_symbols.mapping[*row].placeholder;
				cursor = entity.start + entity.length;
				continue;
			}
		}
		const ProjectEntityResolution projectEntity = project_entity_for_value(frozen_symbols, entity.value);
		std::optional<std::size_t> row = index.row_of(entity, projectEntity);
		if (!row) {
			const auto spelled = rowBySpelling.find(entity.value);
			if (spelled != rowBySpelling.end()) row = spelled->second;
		}
		if (!row && entity.source_label == kOperandLabel) row = operand_row_by_writing(entity.value, frozen_symbols, index);
		if (!row) {
			// Fails closed, and says what could not be represented - its kind and
			// where it was found, never the value itself.
			throw std::runtime_error(std::string("frozen Papinian mapping cannot represent a collected sensitive value")
				+ " (kind " + (entity.type ? to_string(*entity.type) : "unknown")
				+ ", source " + (entity.source == DetectionSource::CustomTerm ? "custom term"
					: entity.source == DetectionSource::Deterministic ? "deterministic" : "detected")
				+ (entity.source_label.empty() ? "" : "/" + entity.source_label)
				+ ", " + std::to_string(entity.length) + " bytes"
				+ ", mapping of " + std::to_string(frozen_symbols.mapping.size()) + ")");
		}
		// An organisation's name written alone goes as its short part, a person's
		// first name or surname alone as that part of the name, and each comes
		// back as it was written.
		const RequestMappingEntry &target = frozen_symbols.mapping[*row];
		output += symbol_as_written(target, entity.value);
		cursor = entity.start + entity.length;
	}
	output.append(utf8_text, cursor, utf8_text.size() - cursor);
	// A ⟦ or ⟧ with no operand to close carries no value; it is not sent either.
	for (const std::string &mark : {kOperandOpen, kOperandClose})
		for (std::size_t position = output.find(mark); position != std::string::npos; position = output.find(mark, position))
			output.erase(position, mark.size());
	return output;
}

GuardValidation PapinianGuard::validate_outbound_payload(
    const std::string &exact_payload, const ProtectionResult &request) const {
  return validate_masking(exact_payload, request.mapping);
}

std::string PapinianGuard::mask_leak_occurrences(const std::string &protected_text,
		const GuardValidation &validation, const std::vector<RequestMappingEntry> &mapping) const {
	std::string repaired = protected_text;
	// The places are taken left to right, the longest first where two begin
	// together, and a place inside one already taken is part of it.
	std::vector<LeakOccurrence> places;
	for (const LeakOccurrence &occurrence : validation.occurrences) {
		if (occurrence.row >= mapping.size() || occurrence.length == 0) continue;
		if (occurrence.start + occurrence.length > repaired.size()) continue;
		if (!places.empty() && occurrence.start < places.back().start + places.back().length) continue;
		places.push_back(occurrence);
	}
	// Masked right to left, so every earlier place keeps its offset.
	for (auto occurrence = places.rbegin(); occurrence != places.rend(); ++occurrence) {
		if (repaired.compare(occurrence->start, occurrence->length, occurrence->form) != 0) continue;
		const RequestMappingEntry &entry = mapping[occurrence->row];
		const bool shortName = occurrence->kind == LeakOccurrence::Kind::SurnameOrShort
			&& std::find(entry.short_forms.begin(), entry.short_forms.end(), occurrence->form) != entry.short_forms.end();
		const std::optional<std::string> namePart = occurrence->kind == LeakOccurrence::Kind::SurnameOrShort
			? name_part_symbol(entry, occurrence->form) : std::nullopt;
		repaired.replace(occurrence->start, occurrence->length,
			shortName ? short_symbol(entry.placeholder) : namePart ? *namePart : entry.placeholder);
	}
	return repaired;
}

std::string PapinianGuard::mask_leak_occurrences(const std::string &protected_text,
		const GuardValidation &validation, OperationProtectedSymbolTable &operation_symbols) const {
	// A place an amount, a rate, a count or a date still stands is a fact to
	// attribute, not a value to look up (DESIGN3 9.3.5 under owner correction
	// 1): the one fact the defined term governing it names; else a fact of its
	// own, met at that place. Equal values are never one identity - the claim
	// written where the transform did not reach is not the price, even when the
	// price is the only fact of that value.
	GuardValidation attributed = validation;
	std::string digest;
	for (LeakOccurrence &occurrence : attributed.occurrences) {
		if (occurrence.kind == LeakOccurrence::Kind::OperandMark || occurrence.kind == LeakOccurrence::Kind::AnchorMark
				|| occurrence.kind == LeakOccurrence::Kind::SurnameOrShort)
			continue;
		const bool named = occurrence.row < operation_symbols.mapping.size();
		const RequestMappingEntry *entry = named ? &operation_symbols.mapping[occurrence.row] : nullptr;
		const bool quantity = occurrence.kind != LeakOccurrence::Kind::Writing
			|| (entry != nullptr && entry->value && !entry->value_key.empty() && quantity_type(entry->type));
		if (!quantity) continue;
		std::optional<semantic::Value> value = semantic::parse_value(occurrence.form);
		if (value && value->kind == semantic::Kind::Boolean) value.reset();
		if (entry != nullptr && entry->value && (!value || !row_value_agrees(*entry, *value, semantic::value_key(*value))))
			value = entry->value;
		if (!value) continue;
		const std::string key = semantic::value_key(*value);
		std::optional<std::size_t> row;
		const FactRole role = role_of(protected_text, occurrence.start, occurrence.start + occurrence.length);
		// The one fact the material defines by the term that governs this place.
		if (role.kind == FactRole::Kind::Defined) {
			const auto found = operation_symbols.fact_by_role.find(named_role_key(role.role, key));
			if (found != operation_symbols.fact_by_role.end() && found->second < operation_symbols.mapping.size()
					&& row_value_agrees(operation_symbols.mapping[found->second], *value, key))
				row = found->second;
		}
		// A row with no fact of its own (a table built before facts, a name-like
		// value) is the value's one row, as validation named it. A fact's value
		// alone never names the fact: the claim of $5,000,000 left where the
		// transform did not reach is not the price of $5,000,000.
		if (!row && entry != nullptr && entry->fact_key.empty()
				&& operation_symbols.facts_by_value.find(key) == operation_symbols.facts_by_value.end())
			row = occurrence.row;
		if (!row) {
			if (digest.empty()) digest = platform::sha256_hex_digest(protected_text);
			FactProvenance place;
			place.passage_sha256 = digest;
			place.offset = occurrence.start;
			place.length = occurrence.length;
			place.role = role.role;
			const std::string placeholder = issue_occurrence_fact(operation_symbols, *value,
				type_of_value(*value).value_or(EntityType::Number), place);
			for (std::size_t index = 0; index < operation_symbols.mapping.size(); ++index)
				if (operation_symbols.mapping[index].placeholder == placeholder) row = index;
		}
		if (row) occurrence.row = *row;
	}
	return mask_leak_occurrences(protected_text, attributed, operation_symbols.mapping);
}

namespace {

bool has_ascii_upper(const std::string &value) {
  return std::any_of(value.begin(), value.end(),
      [](char character) { return character >= 'A' && character <= 'Z'; });
}

std::string ascii_lower(std::string value) {
  for (char &character : value) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  return value;
}

// The spelling a symbol is written back as.
//
// The words are the identity's own; the capitals are the record's. A lawyer
// types in a hurry ("the ashgrove recapture thing", "change of control") and a
// contract shouts its parties in its preamble ("BLUEBONNET HEALTH PLAN,
// INC."), and neither belongs in an answer or in draft language: a live day on
// a data room produced "without Meridia Clinical Systems support's prior
// written consent" and a whole partner-confirmation point invented out of
// "LARKSPUR CLINICS OF TEXAS, LLC" reading as a different company from
// "Larkspur Clinics of Texas, LLC".
//
// So a writing is used as it was met, unless it is casual (no capital anywhere)
// or shouted (no lower-case letter anywhere). Then the identity's own other
// writings are asked, in order: the same words properly written, else the
// fullest properly written name known for it. Only writings this identity was
// actually met in are ever used - restoration invents nothing.
// Whether this request's own material wrote a writing - a document's text, a
// file's name or the lawyer's words - where the row says where its writings
// came from (D-172). A model's writing, Singularity's own restored one and a
// spelling only the Matter's registry knows never stand in for a party's name.
bool written_by_the_material(const RequestMappingEntry &entry, const std::string &writing) {
  if (entry.writing_origins.empty()) return true;
  const auto origin = entry.writing_origins.find(writing);
  return origin != entry.writing_origins.end()
      && (origin->second & (RequestMappingEntry::DocumentText | RequestMappingEntry::FileName
          | RequestMappingEntry::LawyerRequest)) != 0;
}

std::string restoration_spelling(const RequestMappingEntry &entry) {
  const auto ascii_letters = [](const std::string &value) {
    return std::count_if(value.begin(), value.end(), [](char character) {
      return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    });
  };
  const auto shouted = [&](const std::string &value) {
    return ascii_letters(value) >= 2
        && std::none_of(value.begin(), value.end(),
            [](char character) { return character >= 'a' && character <= 'z'; });
  };
  const std::string lowered = ascii_lower(entry.original);

  // The same words, written differently in the same request: the record's own
  // predominant writing is the one an answer is written in. Ties keep the
  // spelling this request met first, and a shouted writing never wins a tie -
  // a preamble in capitals is styling, not a different name.
  const std::string *majority = nullptr;
  std::size_t majorityTimes = 0;
  for (const auto &counted : entry.writing_counts) {
    if (ascii_lower(counted.first) != lowered) continue;
    bool better = majority == nullptr;
    if (!better && shouted(*majority) != shouted(counted.first))
      better = shouted(*majority);
    else if (!better)
      better = counted.second > majorityTimes;
    if (better) { majority = &counted.first; majorityTimes = counted.second; }
  }
  if (majority != nullptr && *majority != entry.original
      && (majorityTimes > entry.times_written(entry.original)
          || (!has_ascii_upper(entry.original) && has_ascii_upper(*majority))
          || (shouted(entry.original) && !shouted(*majority))))
    return *majority;

  const bool casual = !has_ascii_upper(entry.original);
  if (!casual && !shouted(entry.original)) return entry.original;

  for (const std::string &form : entry.surface_forms) {
    if (ascii_lower(form) == lowered && has_ascii_upper(form) && !shouted(form)) return form;
  }
  const auto words = [](const std::string &value) {
    std::size_t count = 0;
    bool inside = false;
    for (char character : value) {
      const bool space = character == ' ' || character == '\t' || character == '\n' || character == '\r';
      if (!space && !inside) ++count;
      inside = !space;
    }
    return count;
  };
  const std::string *best = nullptr;
  for (const std::string &form : entry.surface_forms) {
    if (form.empty() || !has_ascii_upper(form) || shouted(form) || !written_by_the_material(entry, form)) continue;
    if (best == nullptr || words(form) > words(*best)
        || (words(form) == words(*best) && form.size() > best->size()))
      best = &form;
  }
  if (best != nullptr) return *best;
  for (const std::string &form : entry.surface_forms) {
    if (ascii_lower(form) == lowered && has_ascii_upper(form)) return form;
  }
  return entry.original;
}

// Whether a writing is shouted or casual, so never the one an answer is written in.
bool properly_capitalised(const std::string &value) {
	return has_ascii_upper(value) && std::any_of(value.begin(), value.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

// Whether a writing of a quantity reads as the answer's language writes one:
// its words' language, a date's style, and for figures alone the separators -
// "1 183 452,05 ₽" is Russian, "$61,300.00" English, "3%" either.
bool fits_language(const std::string &writing, const semantic::Value &value, semantic::Language language) {
	const bool russian = language == semantic::Language::Russian;
	// A currency's code is no language's word: "75000 RUB" reads in either.
	std::string words = writing;
	for (std::size_t index = 0; index + 3 <= words.size(); ++index) {
		const bool code = std::all_of(words.begin() + static_cast<std::ptrdiff_t>(index),
			words.begin() + static_cast<std::ptrdiff_t>(index + 3), [](char c) { return c >= 'A' && c <= 'Z'; });
		const bool alone = (index == 0 || !std::isalpha(static_cast<unsigned char>(words[index - 1])))
			&& (index + 3 == words.size() || !std::isalpha(static_cast<unsigned char>(words[index + 3])));
		if (code && alone) words.replace(index, 3, "   ");
	}
	if (has_letter(words)) return semantic::language_of(words) == language;
	// A date in figures: "10.02.2026" is written the Russian way, "02/10/2026"
	// the American one, "2026-02-10" either.
	if (value.kind == semantic::Kind::Date) {
		if (writing.find('.') != std::string::npos) return russian;
		if (writing.find('/') != std::string::npos) return !russian;
		return true;
	}
	bool russianMark = false;
	bool englishMark = false;
	for (std::size_t index = 1; index + 1 < writing.size(); ++index) {
		const bool digitBefore = writing[index - 1] >= '0' && writing[index - 1] <= '9';
		if (!digitBefore) continue;
		std::size_t digitsAfter = 0;
		while (index + 1 + digitsAfter < writing.size() && writing[index + 1 + digitsAfter] >= '0'
				&& writing[index + 1 + digitsAfter] <= '9')
			++digitsAfter;
		if (writing[index] == ',') (digitsAfter == 3 ? englishMark : russianMark) = true;
		else if (writing[index] == '.' && digitsAfter > 0) englishMark = true;
		else if ((writing[index] == ' ' || static_cast<unsigned char>(writing[index]) == 0xC2) && digitsAfter > 0)
			russianMark = true;
	}
	if (russianMark == englishMark) return true;
	return russianMark == russian;
}

// Whether a writing of a row reads as the row's value - as its type says a
// bare figure is ("14 million" in a money row is money) when `typed` is false,
// and on its own words alone when it must read back without the row's help.
bool reads_as_row_value(const std::string &writing, const RequestMappingEntry &entry, bool typed,
		const std::string &note = std::string()) {
	semantic::ParseHints hints;
	if (!typed) {
		if (entry.type == EntityType::MoneyAmount) hints.expect = semantic::Kind::Money;
		else if (entry.type == EntityType::Percentage) hints.expect = semantic::Kind::Percentage;
		else if (entry.type == EntityType::Date) hints.expect = semantic::Kind::Date;
	}
	const std::optional<semantic::Value> value = semantic::parse_value(writing + note, hints);
	return value && semantic::value_key(*value) == entry.value_key;
}

// Whether a symbol stands as a workbook cell's value in the reading of a
// sheet: "B6 = [[E5]]", "B12: [[E3]]".
bool stands_as_cell_value(const std::string &text, std::size_t position) {
	std::size_t cursor = position;
	if (cursor > 0 && text[cursor - 1] == ' ') --cursor;
	if (cursor == 0 || (text[cursor - 1] != '=' && text[cursor - 1] != ':')) return false;
	--cursor;
	if (cursor > 0 && text[cursor - 1] == ' ') --cursor;
	std::size_t digits = 0;
	while (cursor > 0 && text[cursor - 1] >= '0' && text[cursor - 1] <= '9') { --cursor; ++digits; }
	if (cursor > 0 && text[cursor - 1] == '$') --cursor;
	std::size_t letters = 0;
	while (cursor > 0 && text[cursor - 1] >= 'A' && text[cursor - 1] <= 'Z') { --cursor; ++letters; }
	if (cursor > 0 && text[cursor - 1] == '$') --cursor;
	const bool standsAlone = cursor == 0 || text[cursor - 1] == ' ' || text[cursor - 1] == '\n' || text[cursor - 1] == '|';
	if (!(digits >= 1 && digits <= 7 && letters >= 1 && letters <= 3 && standsAlone)) return false;
	// "B6 = " is a cell wherever it stands; "B12: " only on a sheet's reading
	// ("Row 5: A5: ... | B5: 0"). In prose, "CY2: " or "Q3: " is a label, and
	// its figure is written as prose writes it - never as a bare cell value
	// (live, 26 Sep 2026: "CY2: 6032" in a Word edit, re-read unmasked).
	std::size_t separator = position;
	if (separator > 0 && text[separator - 1] == ' ') --separator;
	if (text[separator - 1] == '=') return true;
	const std::size_t lineStart = text.rfind('\n', cursor == 0 ? 0 : cursor - 1);
	const std::size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
	std::size_t lineEnd = text.find('\n', position);
	if (lineEnd == std::string::npos) lineEnd = text.size();
	const std::string line = text.substr(from, lineEnd - from);
	return line.rfind("Row ", 0) == 0 || line.find(" | ") != std::string::npos;
}

// A quantity inside a statement's ⟦…⟧: a writing that reads back as the same
// value, so Singularity computes with the value itself - never a bare workbook
// number that would read as a plain number with no currency.
std::string typed_writing(const RequestMappingEntry &entry, semantic::Language language) {
	for (const std::string &writing : entry.every_form()) {
		const std::optional<semantic::Value> value = semantic::parse_value(writing);
		if (value && semantic::value_key(*value) == entry.value_key) return writing;
	}
	for (const semantic::Language each : {language, semantic::Language::English, semantic::Language::Russian}) {
		try {
			const std::string shown = semantic::display(*entry.value, {}, each);
			const std::optional<semantic::Value> value = semantic::parse_value(shown);
			if (value && semantic::value_key(*value) == entry.value_key) return shown;
		} catch (const semantic::AlgebraError &) {
		}
	}
	if (entry.value->kind == semantic::Kind::Money && !entry.value->currency.empty()) {
		try {
			return entry.value->currency + " " + entry.value->amount.to_decimal(entry.value->amount.is_integer() ? 0 : 2);
		} catch (const semantic::AlgebraError &) {
		}
	}
	return entry.original;
}

// Whether a writing of a value writes every figure of it: "$12,35" is not
// $12.3456, and two amounts that differ in their fourth decimal must never be
// written back alike - the lawyer would read one figure for two, and sent
// again, both would take one symbol.
bool writes_exactly(const std::string &writing, const semantic::Value &value) {
	if (value.kind == semantic::Kind::Date || value.kind == semantic::Kind::DayOfYear || value.kind == semantic::Kind::Boolean)
		return true;
	// An amount in words («пятьсот тысяч рублей») writes a whole number, all of
	// it: whether it is this value is what reading it says.
	const std::string written = significant_figures(std::string_view(writing));
	if (written.empty() && !contains_digit(writing)) return true;
	// Zero has no significant figure: "$0.00" and «0,00 ₽» write it exactly.
	if (value.amount.is_zero()) return written.empty() && contains_digit(writing);
	const std::string figures = significant_figures(value);
	return !figures.empty() && written == figures;
}

// Whether a source's writing of a row reads as the row's value by its own
// words - "$61,300.00", "0,1 %", "14 million" in an amount whose currency no
// writing names - rather than only as the cell it came from: a workbook's bare
// "0.18" is a rate only beside "(shown as a percentage)", and "4800000" is
// the workbook's form, never prose.
bool says_what_it_is(const std::string &writing, const RequestMappingEntry &entry) {
	if (writing.find("(shown") != std::string::npos) return false;
	if (reads_as_row_value(writing, entry, true)) return true;
	if (!currency_unknown(*entry.value)) return false;
	const std::size_t digits = static_cast<std::size_t>(std::count_if(writing.begin(), writing.end(),
		[](char character) { return character >= '0' && character <= '9'; }));
	const bool workbookNumber = writing.find_first_not_of("0123456789.-") == std::string::npos && digits >= 5;
	return !workbookNumber && reads_as_row_value(writing, entry, false);
}

// An amount written with every decimal it has, in the language's style and with
// its currency where the language puts it: "$12.3456", "1 183 452,0512 ₽".
std::string exact_money_writing(const semantic::Value &value, semantic::Language language, bool withCurrency) {
	semantic::Format number;
	number.style = semantic::FormatStyle::Number;
	std::string digits = semantic::display(semantic::make_number(value.amount), number, language);
	// Cents are written as cents: "61,300.50", never "61,300.5".
	const std::size_t mark = digits.find(language == semantic::Language::Russian ? ',' : '.');
	if (mark != std::string::npos && digits.size() - mark - 1 == 1) digits += "0";
	if (!withCurrency || value.currency.empty()) return digits;
	std::string shape = semantic::display(semantic::make_money(semantic::Rational(1), value.currency), {}, language);
	const std::size_t one = shape.find('1');
	if (one == std::string::npos || shape.find('1', one + 1) != std::string::npos) return value.currency + " " + digits;
	shape.replace(one, 1, digits);
	return shape;
}

// A quantity in prose: a writing of it in the answer's language that writes it
// exactly, else Singularity's own display - never the raw workbook form ("0.18"
// reads as a rate only with its cell's note) - and without the currency or the
// percent the text already writes right beside it ("[[E3]] рублей") when that
// is the value's own. A date takes the answer's form: "July 2026" in Russian
// prose is «июль 2026 г.», never the oblique «июле 2026» it was once met in,
// nor a workbook's "2026-03-15".
std::string prose_writing(const RequestMappingEntry &entry, semantic::Language language, const std::string &currencyBeside,
		bool percentBeside, bool yearBeside) {
	const semantic::Value &value = *entry.value;
	const auto exact = [&](const std::string &writing) { return writes_exactly(writing, value); };
	try {
		// Another currency beside it is not this amount's: the amount keeps its own.
		if (!currencyBeside.empty() && value.kind == semantic::Kind::Money
				&& (value.currency.empty() || value.currency == currencyBeside)) {
			semantic::Value bare = value;
			bare.currency.clear();
			bare.basis = semantic::RateBasis::None;
			const std::string shown = semantic::display(bare, {}, language);
			return exact(shown) ? shown : exact_money_writing(value, language, false);
		}
		// A figure the answer writes as money ("[[Amount75]] ₽") is written as
		// money is, to the kopeck or cent - never a plain number's four places
		// (live R2, 27 Sep 2026: «25 461,3102 ₽» beside «21 529,87 ₽»), when the
		// value itself came without a currency (a ledger column read as numbers).
		if (!currencyBeside.empty() && (value.kind == semantic::Kind::Number || value.kind == semantic::Kind::Count)) {
			semantic::Value money = semantic::make_money(value.amount, currencyBeside, 2);
			money.currency.clear();
			return semantic::display(money, {}, language);
		}
		if (percentBeside && value.kind == semantic::Kind::Percentage) {
			semantic::Format number;
			number.style = semantic::FormatStyle::Number;
			return semantic::display(semantic::make_number(value.amount * semantic::Rational(100)), number, language);
		}
		if (value.kind == semantic::Kind::Date) {
			semantic::Value styled = value;
			if (language == semantic::Language::Russian) styled.date_style = value.date_style == "dotted" ? "dotted" : "russian";
			else styled.date_style = value.date_style == "slashed" ? "slashed" : "english";
			std::string shown = semantic::display(styled, {}, language);
			// «до [[E1]] года», «[[E1]] г.»: the text writes the year's word itself.
			if (yearBeside) {
				const std::string mark = "\xd0\xb3.";
				if (shown.size() > mark.size() && shown.compare(shown.size() - mark.size(), mark.size(), mark) == 0) {
					shown.erase(shown.size() - mark.size());
					while (!shown.empty() && shown.back() == ' ') shown.pop_back();
					if (shown.size() >= 2 && shown.compare(shown.size() - 2, 2, "\xC2\xA0") == 0) shown.erase(shown.size() - 2);
				}
			}
			return shown;
		}
		// A day of the year as the answer's language writes one: "July 1", «1 июля».
		if (value.kind == semantic::Kind::DayOfYear) return semantic::display(value, {}, language);
		// A ratio a workbook works out ("0.054849622377264616 (shown as a
		// percentage)") is read in prose as a percentage shows it - to two
		// places, "5.48%" - never as its sixteen stored decimals (ab2 EN1, 25 Sep
		// 2026). A rate a contract writes ("0.1 %") has few places and is exact.
		if (value.kind == semantic::Kind::Percentage && !(value.amount * semantic::Rational(100)).exact_decimal(4)) {
			semantic::Value shown = value;
			shown.basis = semantic::RateBasis::None;
			shown.amount = (value.amount * semantic::Rational(100)).rounded(2) / semantic::Rational(100);
			return semantic::display(shown, {}, language);
		}
	} catch (const semantic::AlgebraError &) {
	}
	// The source's own writing, when it says what it is in its own words: a
	// cell's bare "0.18" does not, and is never prose. An amount the sources
	// write in figures too is written in figures: its words ("Fifteen Thousand
	// Dollars") are a contract's legal form beside them, which an answer took
	// 15 times in ab4 (25 Sep 2026). Words only where no figures were written.
	for (const bool figures : {true, false})
		for (const std::string &writing : entry.every_form()) {
			if (figures && !contains_digit(writing)) continue;
			if (!says_what_it_is(writing, entry) || !exact(writing)) continue;
			if (fits_language(writing, value, language)) return writing;
		}
	try {
		// A rate's period is the sentence's to write ("[[E1]] per annum"): the
		// figure alone.
		semantic::Value figure = value;
		if (figure.kind == semantic::Kind::Percentage) figure.basis = semantic::RateBasis::None;
		const std::string shown = semantic::display(figure, {}, language);
		if (exact(shown)) return shown;
		if (value.kind == semantic::Kind::Money) return exact_money_writing(value, language, true);
	} catch (const semantic::AlgebraError &) {
	}
	for (const std::string &writing : entry.every_form())
		if (says_what_it_is(writing, entry) && exact(writing)) return writing;
	return entry.original;
}

// A quantity in a quotation of a document's own text: a source's writing, in
// the language the quotation is in, that says what it is - how a document
// writes it - never Singularity's display of it.
std::string quoted_writing(const RequestMappingEntry &entry, semantic::Language language) {
	for (const std::string &writing : entry.every_form())
		if (says_what_it_is(writing, entry) && fits_language(writing, *entry.value, language)) return writing;
	return entry.original;
}

// A quantity in a workbook's reading, before its note: the source's own cell
// writing, the one that reads with the note as this value. A cell holds a plain
// number - figures and their separators, no sign and no words - so that is
// asked for first, and without a note a bare amount is read in the row's own
// currency: "4800000" is the cell of a row "$4,800,000" and a workbook's
// "4800000 (shown in $)" became. Read without it, a cell's whole value came
// back "$4,800,000", which a number cell refuses (a verifier's run, 25 Sep 2026).
std::string workbook_writing(const RequestMappingEntry &entry, const std::string &note) {
	const auto plain = [](const std::string &writing) {
		return contains_digit(writing) && writing.find_first_not_of("0123456789.,- \xC2\xA0") == std::string::npos;
	};
	const auto readsAsCell = [&](const std::string &writing) {
		if (reads_as_row_value(writing, entry, false, note)) return true;
		if (!note.empty() || entry.value->kind != semantic::Kind::Money || entry.value->currency.empty()) return false;
		const std::optional<semantic::Value> value
			= semantic::parse_value(writing, semantic::ParseHints{semantic::Kind::Money, entry.value->currency, false});
		return value && semantic::value_key(*value) == entry.value_key;
	};
	for (const std::string &writing : entry.every_form())
		if (plain(writing) && readsAsCell(writing)) return writing;
	// A fact the material states only in prose ("$4,800,000") has no writing a
	// cell takes: a number cell gets the value's own plain number, never text.
	if (note.empty() && entry.value
			&& (entry.value->kind == semantic::Kind::Money || entry.value->kind == semantic::Kind::Number
				|| entry.value->kind == semantic::Kind::Percentage))
		if (const std::optional<std::string> number = entry.value->amount.exact_decimal(8)) return *number;
	for (const std::string &writing : entry.every_form())
		if (readsAsCell(writing)) return writing;
	return entry.original;
}

bool ends_with_any(const std::string &text, std::size_t end, std::initializer_list<const char *> marks) {
	for (const char *mark : marks) {
		const std::size_t size = std::char_traits<char>::length(mark);
		if (end >= size && text.compare(end - size, size, mark) == 0) return true;
	}
	return false;
}

// The currency the text writes right beside a symbol at [start, end), as its
// code: "$[[E3]]" USD, "[[E3]] рублей" RUB, "[[E3]] US dollars" USD; "?" for a
// currency Singularity does not name; empty when none is written there.
std::string currency_beside(const std::string &text, std::size_t start, std::size_t end) {
	std::size_t before = start;
	if (before > 0 && text[before - 1] == ' ') --before;
	else if (before >= 2 && text.compare(before - 2, 2, "\xC2\xA0") == 0) before -= 2;
	static const std::vector<std::pair<const char *, const char *>> signs{
		{"$", "USD"}, {"\xe2\x82\xac", "EUR"}, {"\xc2\xa3", "GBP"}, {"\xc2\xa5", "JPY"}, {"\xe2\x82\xbd", "RUB"},
		{"USD", "USD"}, {"EUR", "EUR"}, {"GBP", "GBP"}, {"RUB", "RUB"}, {"JPY", "JPY"},
	};
	for (const auto &[sign, code] : signs)
		if (ends_with_any(text, before, {sign})) return code;
	std::size_t after = end;
	if (after < text.size() && text[after] == ' ') ++after;
	else if (text.compare(after, 2, "\xC2\xA0") == 0) after += 2;
	std::string next = text.substr(after, 16);
	for (char &character : next) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	static const std::vector<std::pair<const char *, const char *>> words{
		{"\xe2\x82\xbd", "RUB"}, {"\xe2\x82\xac", "EUR"}, {"$", "USD"}, {"\xd1\x80\xd1\x83\xd0\xb1", "RUB"},
		{"\xd0\xa0\xd1\x83\xd0\xb1", "RUB"}, {"rub", "RUB"}, {"usd", "USD"}, {"eur", "EUR"}, {"gbp", "GBP"},
		{"jpy", "JPY"}, {"dollar", "USD"}, {"us dollar", "USD"}, {"u.s. dollar", "USD"}, {"euro", "EUR"},
		{"pound", "GBP"}, {"yen", "JPY"}, {"\xd0\xb4\xd0\xbe\xd0\xbb\xd0\xbb", "USD"},
		{"\xd0\x94\xd0\xbe\xd0\xbb\xd0\xbb", "USD"}, {"\xd0\xb5\xd0\xb2\xd1\x80\xd0\xbe", "EUR"},
		{"\xd0\x95\xd0\xb2\xd1\x80\xd0\xbe", "EUR"}, {"\xd1\x84\xd1\x83\xd0\xbd\xd1\x82", "GBP"},
		{"\xd1\x8e\xd0\xb0\xd0\xbd", "?"}, {"yuan", "?"}, {"franc", "?"},
	};
	for (const auto &[word, code] : words)
		if (next.compare(0, std::char_traits<char>::length(word), word) == 0) return code;
	return {};
}

// Whether the text writes a percent right after a symbol: "[[E3]]%", "[[E3]]
// per cent", "[[E3]] процента".
bool percent_beside(const std::string &text, std::size_t end) {
	std::size_t after = end;
	if (after < text.size() && text[after] == ' ') ++after;
	else if (text.compare(after, 2, "\xC2\xA0") == 0) after += 2;
	if (after < text.size() && text[after] == '%') return true;
	for (const char *word : {"percent", "per cent", "pct", "\xd0\xbf\xd1\x80\xd0\xbe\xd1\x86\xd0\xb5\xd0\xbd\xd1\x82"})
		if (text.compare(after, std::char_traits<char>::length(word), word) == 0) return true;
	return false;
}

// Whether the text writes a Russian year's word right after a symbol: "[[E1]]
// года", "[[E1]] г.", "[[E1]] году". A date restored there is written without
// its own «г.», never «3 марта 2026 г. года» (a verifier's run, 25 Sep 2026).
bool year_beside(const std::string &text, std::size_t end) {
	std::size_t after = end;
	if (after < text.size() && text[after] == ' ') ++after;
	else if (text.compare(after, 2, "\xC2\xA0") == 0) after += 2;
	// «г» and what follows it: a full stop, «од…», or no letter at all.
	if (text.compare(after, 2, "\xd0\xb3") != 0) return false;
	after += 2;
	if (after >= text.size() || text[after] == '.') return true;
	if (text.compare(after, 4, "\xd0\xbe\xd0\xb4") == 0) return true;
	return !continues_word_at(text, after);
}

// "Dear [[E5]]": the honorific a source wrote for this person and the
// surname - "Dear Ms. Morgan" - or nothing when no source wrote one.
std::optional<std::string> salutation_writing(const RequestMappingEntry &entry) {
	static const std::vector<std::string> honorifics{"Mr.", "Ms.", "Mrs.", "Dr.", "Mr", "Ms", "Mrs", "Dr"};
	std::vector<std::string> writings = entry.every_form();
	writings.insert(writings.end(), entry.surname_forms.begin(), entry.surname_forms.end());
	const PersonName name = person_name(fullest_person_writing(entry));
	if (name.surname.empty()) return std::nullopt;
	for (const std::string &writing : writings)
		for (const std::string &honorific : honorifics)
			if (writing.size() > honorific.size() + 1 && writing.compare(0, honorific.size(), honorific) == 0
					&& writing[honorific.size()] == ' ')
				return (honorific.back() == '.' ? honorific : honorific + ".") + " " + name.surname;
	// Written before the name the checkpoint returned: "Mr. [Marcus Delgado]".
	if (!entry.honorific.empty()) return entry.honorific + " " + name.surname;
	return std::nullopt;
}

// A Russian salutation the model wrote around a person's symbol - «Уважаемый
// [[E4]]», «Уважаемый(ая) господин [[E4]] А. И.» - and how it is written back:
// the adjective agreeing with the person's gender, then the given name and the
// patronymic when the sources give them («Уважаемый Андрей Игоревич»), else the
// name as the sources write it. Never «господин»: a Russian letter does not
// write it before a name. Nothing when the symbol does not follow «Уважаем…».
struct RussianSalutation {
	std::size_t start = 0;
	std::size_t end = 0;
	std::string text;
};

std::optional<RussianSalutation> russian_salutation(const RequestMappingEntry &entry, const std::string &text,
		std::size_t position, std::size_t end, russian_names::Gender gender) {
	const std::string adjective = "\xD0\xA3\xD0\xB2\xD0\xB0\xD0\xB6\xD0\xB0\xD0\xB5\xD0\xBC";  // Уважаем
	const std::size_t lineStart = text.rfind('\n', position == 0 ? 0 : position - 1);
	const std::size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
	if (position - from > 96) return std::nullopt;
	const std::size_t found = text.rfind(adjective, position);
	if (found == std::string::npos || found < from) return std::nullopt;
	// Between the adjective and the symbol only its ending and a form of address.
	std::string between = text.substr(found + adjective.size(), position - found - adjective.size());
	for (const char *word : {"\xD1\x8B\xD0\xB9(\xD0\xB0\xD1\x8F)", "\xD0\xB0\xD1\x8F(\xD1\x8B\xD0\xB9)", "\xD1\x8B\xD0\xB9/\xD0\xB0\xD1\x8F",
			"\xD1\x8B\xD0\xB9", "\xD0\xB0\xD1\x8F", " \xD0\xB3\xD0\xBE\xD1\x81\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xB8\xD0\xBD",
			" \xD0\xB3\xD0\xBE\xD1\x81\xD0\xBF\xD0\xBE\xD0\xB6\xD0\xB0", " \xD0\xB3-\xD0\xBD", " \xD0\xB3-\xD0\xB6\xD0\xB0"}) {
		const std::string piece(word);
		std::size_t at;
		while ((at = between.find(piece)) != std::string::npos) between.erase(at, piece.size());
	}
	if (between.find_first_not_of(' ') != std::string::npos) return std::nullopt;
	RussianSalutation salutation;
	salutation.start = found;
	salutation.end = end;
	// Initials the model wrote after the symbol ("[[E4]] А. И.") are the name's own.
	std::size_t after = end;
	for (int initial = 0; initial < 2; ++initial) {
		std::size_t at = after;
		if (at < text.size() && text[at] == ' ') ++at;
		if (at + 2 >= text.size() || static_cast<unsigned char>(text[at]) != 0xD0) break;
		const unsigned char letter = static_cast<unsigned char>(text[at + 1]);
		if (!((letter >= 0x90 && letter <= 0xAF) || letter == 0x81) || text[at + 2] != '.') break;
		after = at + 3;
	}
	salutation.end = after;
	const std::optional<PersonNameParts> parts = person_name_parts(entry);
	std::string name;
	if (parts && !parts->first.empty() && !parts->patronymic.empty()) {
		name = parts->first + " " + parts->patronymic;
		if (gender == russian_names::Gender::Unknown) gender = russian_names::gender_of(name);
	} else {
		name = russian_names::declined(fullest_person_writing(entry), russian_names::Case::Nominative, gender);
	}
	const std::string masculine = adjective + "\xD1\x8B\xD0\xB9";  // Уважаемый
	const std::string feminine = adjective + "\xD0\xB0\xD1\x8F";   // Уважаемая
	salutation.text = (gender == russian_names::Gender::Feminine ? feminine
		: gender == russian_names::Gender::Masculine ? masculine : masculine + "(\xD0\xB0\xD1\x8F)") + " " + name;
	return salutation;
}

// A figure in a document table's cell, written as the table writes it: the
// column's header names the currency and the scale ("Сумма, руб.", "Revenue
// ($m)"), so the cell holds the bare figure - "25 000", "12.5" - never "25 000 ₽"
// or "$12,500,000". Restoring the lawyer's own table once wrote a currency mark
// into every cell, and a model's quotation of a cell no longer matched the
// document. The source's own cell writing first; nothing outside such a column.
std::optional<std::string> table_cell_writing(const RequestMappingEntry &entry, const std::string &text, std::size_t position,
		semantic::Language language) {
	if (!entry.value || entry.value->kind != semantic::Kind::Money) return std::nullopt;
	const ColumnUnit unit = column_unit(text, position);
	// A column that names no currency ("Base Rent") holds its figures as the
	// table writes them too: "61300" stays "61300" in the lawyer's own table.
	if (unit.currency.empty() && entry.value->currency.empty() && table_column_header(text, position)) {
		for (const std::string &writing : entry.every_form()) {
			if (!contains_digit(writing) || writing.find_first_not_of("0123456789.,- \xC2\xA0") != std::string::npos) continue;
			const std::optional<semantic::Value> value =
				semantic::parse_value(writing, semantic::ParseHints{semantic::Kind::Money, "", false});
			if (value && value->kind == semantic::Kind::Money && value->amount == entry.value->amount) return writing;
		}
		return std::nullopt;
	}
	if (unit.currency.empty() || (!entry.value->currency.empty() && entry.value->currency != unit.currency)) return std::nullopt;
	for (const std::string &writing : entry.every_form()) {
		if (!contains_digit(writing) || writing.find_first_not_of("0123456789.,- \xC2\xA0") != std::string::npos) continue;
		const std::optional<semantic::Value> value = money_in_column(writing, unit);
		if (value && value->amount == entry.value->amount) return writing;
	}
	try {
		semantic::Value figure = *entry.value;
		figure.amount = figure.amount / unit.scale;
		return exact_money_writing(figure, language, false);
	} catch (const semantic::AlgebraError &) {
		return std::nullopt;
	}
}

// Whether a symbol at [start, end) is glued to what stands beside it: a file
// name's words ("Notice_[[E1]].pdf", "Minutes [[E1]]-final.docx"), a
// timestamp's time ("[[E1]]T10:30:00Z") or another value of a range
// ("[[E1]]/[[E2]]", "[[E1]]-[[E2]]").
bool glued_symbol(const std::string &text, std::size_t start, std::size_t end) {
	const auto joins = [&](std::size_t index) {
		return index < text.size() && (std::isalnum(static_cast<unsigned char>(text[index])) != 0 || text[index] == '_');
	};
	if (start > 0 && joins(start - 1)) return true;
	if (start >= 2 && (text[start - 1] == '-' || text[start - 1] == '/') && (joins(start - 2) || text[start - 2] == ']'))
		return true;
	if (joins(end)) return true;
	return end + 1 < text.size() && (text[end] == '-' || text[end] == '/' || text[end] == '.')
		&& (joins(end + 1) || text[end + 1] == '[');
}

// A date or a day of the year written back as the source wrote it, where the
// source's writing is the one that belongs there. Glued to a file name's words,
// a time or a range, only the writing itself fits ("Notice_20260210.pdf" is
// found by its name; "Notice_February 10, 2026.pdf" is no file). A day of the
// year keeps the lawyer's own words in the answer's language ("30 June", "the
// 1st of July", «1 июля») - no fixed style rewrites them. After "the", a date an
// execution clause wrote by its ordinal day ("the 1st day of July, 2026") keeps
// it: "the July 1, 2026" is no English. Nothing: Singularity's own display.
std::optional<std::string> own_date_writing(const RequestMappingEntry &entry, const std::string &text, std::size_t start,
		std::size_t end, semantic::Language language) {
	if (!entry.value || (entry.value->kind != semantic::Kind::Date && entry.value->kind != semantic::Kind::DayOfYear))
		return std::nullopt;
	const std::vector<std::string> forms = entry.every_form();
	const auto readsAsRow = [&](const std::string &writing) {
		if (writing == entry.original) return true;
		const std::optional<semantic::Value> value = semantic::parse_value(writing);
		return value && semantic::value_key(*value) == entry.value_key;
	};
	if (glued_symbol(text, start, end)) {
		if (entry.original.find(' ') == std::string::npos) return entry.original;
		for (const std::string &writing : forms)
			if (writing.find(' ') == std::string::npos && readsAsRow(writing)) return writing;
		return std::nullopt;
	}
	const auto inLanguage = [&](const std::string &writing) {
		return has_cyrillic_letter(writing) == (language == semantic::Language::Russian);
	};
	if (entry.value->kind == semantic::Kind::DayOfYear) {
		for (const std::string &writing : forms)
			if (inLanguage(writing) && readsAsRow(writing)) return writing;
		return std::nullopt;
	}
	if (language == semantic::Language::Russian || start < 4) return std::nullopt;
	std::string before = text.substr(start - 4, 4);
	for (char &character : before) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	if (before != "the " || (start > 4 && std::isalpha(static_cast<unsigned char>(text[start - 5])) != 0)) return std::nullopt;
	for (const std::string &writing : forms)
		if (!writing.empty() && writing.front() >= '0' && writing.front() <= '9' && inLanguage(writing)
				&& writing.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos && readsAsRow(writing))
			return writing;
	return std::nullopt;
}

// One end of a range written back as the source wrote it, where the model
// writes the range as the source did - "[[E1]]-[[E2]]", "[[E1]] – [[E2]]" -
// and one end's writing says only its day ("10" of «10-12 марта 2026 г.»,
// "March 3" and "5, 2026" of "March 3-5, 2026"): the other end says the rest,
// so it must be the same range's - of the same year, and of the same month
// when the writing names none - and then both ends keep the source's writing
// ("1-3 July 2026", never "1-July 3, 2026"). Anywhere else such a writing says
// too little, and the day is written whole. Nothing: another writing belongs here.
std::optional<std::string> range_end_writing(const RequestMappingEntry &entry, const std::string &text, std::size_t start,
		std::size_t end, const std::unordered_map<std::string, const RequestMappingEntry *> &mapping) {
	if (!entry.value || entry.value->kind != semantic::Kind::Date || !day_of_key(entry.value_key)) return std::nullopt;
	const auto dashLength = [&](std::size_t at) -> std::size_t {
		if (at < text.size() && text[at] == '-') return 1;
		if (text.compare(at, 3, "\xE2\x80\x93") == 0 || text.compare(at, 3, "\xE2\x80\x94") == 0) return 3;
		return 0;
	};
	// The symbol across the dash, on either side.
	const RequestMappingEntry *other = nullptr;
	std::size_t cursor = end;
	while (cursor < text.size() && text[cursor] == ' ') ++cursor;
	if (const std::size_t dash = dashLength(cursor); dash != 0) {
		cursor += dash;
		while (cursor < text.size() && text[cursor] == ' ') ++cursor;
		const std::size_t closed = text.compare(cursor, 2, "[[") == 0 ? text.find("]]", cursor) : std::string::npos;
		if (closed != std::string::npos && closed - cursor < 24) {
			const auto found = mapping.find(text.substr(cursor, closed + 2 - cursor));
			if (found != mapping.end()) other = found->second;
		}
	}
	if (other == nullptr) {
		cursor = start;
		while (cursor > 0 && text[cursor - 1] == ' ') --cursor;
		std::size_t dash = 0;
		if (cursor >= 1 && text[cursor - 1] == '-') dash = 1;
		else if (cursor >= 3 && dashLength(cursor - 3) == 3) dash = 3;
		if (dash != 0) {
			cursor -= dash;
			while (cursor > 0 && text[cursor - 1] == ' ') --cursor;
			const std::size_t opened = cursor >= 2 && text.compare(cursor - 2, 2, "]]") == 0 ? text.rfind("[[", cursor - 2)
				: std::string::npos;
			if (opened != std::string::npos && cursor - opened < 24) {
				const auto found = mapping.find(text.substr(opened, cursor - opened));
				if (found != mapping.end()) other = found->second;
			}
		}
	}
	if (other == nullptr || other == &entry || !other->value || other->value->kind != semantic::Kind::Date) return std::nullopt;
	// Whether a row's writing says only a part of its day, which `whole` says the rest of.
	const auto partOf = [](const RequestMappingEntry &part, const RequestMappingEntry &whole) {
		if (writes_its_own_date(part, part.original)) return false;
		const std::optional<std::array<int, 3>> own = day_of_key(part.value_key);
		const std::optional<std::array<int, 3>> theirs = day_of_key(whole.value_key);
		if (!own || !theirs || own->at(0) == 0 || theirs->at(0) == 0) return false;
		// What the writing names itself: a month by its word or «01.07»'s figures, a year by four figures.
		const std::string &writing = part.original;
		bool month = false;
		bool year = false;
		std::size_t figures = 0;
		for (std::size_t index = 0; index < writing.size(); ++index) {
			const unsigned char character = static_cast<unsigned char>(writing[index]);
			if (std::isalpha(character) != 0 || character >= 0x80) month = true;
			if (std::isdigit(character) != 0) {
				if (++figures == 4) year = true;
			} else {
				if (figures > 0 && (character == '.' || character == '/') && index + 1 < writing.size()
						&& std::isdigit(static_cast<unsigned char>(writing[index + 1])) != 0)
					month = true;
				figures = 0;
			}
		}
		const int years = own->at(0) - theirs->at(0);
		if (!year && years != 0 && !(years == -1 && month)) return false;
		return month || (own->at(1) == theirs->at(1) && years == 0);
	};
	if (partOf(entry, *other)) return entry.original;
	if (writes_its_own_date(entry, entry.original) && partOf(*other, entry)) return entry.original;
	return std::nullopt;
}

} // namespace

std::optional<PersonNameParts> person_name_parts(const RequestMappingEntry &entry) {
	if (!person_type(entry.type)) return std::nullopt;
	const std::string fullest = fullest_person_writing(entry);
	PersonNameParts parts;
	if (has_cyrillic_letter(fullest)) {
		const std::string nominative = russian_names::declined(fullest, russian_names::Case::Nominative, row_person_gender(entry));
		if (const auto named = russian_names::name_parts(nominative)) {
			parts.first = named->first;
			parts.last = named->surname;
			parts.patronymic = named->patronymic;
		}
	} else {
		const PersonName name = person_name(fullest);
		parts.first = name.given;
		parts.last = name.surname;
	}
	if (parts.first.empty() && parts.last.empty()) return std::nullopt;
	return parts;
}

std::optional<std::string> organisation_short_name(const RequestMappingEntry &entry) {
	if (!organisation_type(entry.type) || entry.short_forms.empty()) return std::nullopt;
	// Written back as the company writes it, never as a heading shouts it.
	for (const std::string &form : entry.short_forms)
		if (upper_case(form) != form) return form;
	for (const std::string &writing : entry.every_form()) {
		const std::string shortName = short_company_name(writing);
		if (!shortName.empty() && upper_case(shortName) == upper_case(entry.short_forms.front())) return shortName;
	}
	return entry.short_forms.front();
}

// A company's full registered name among the ways this operation met it: a
// writing with its legal form ("Northwind Freight Systems LLC", «ООО «Северный
// ветер»»), written as the company writes it rather than as a heading shouts
// it - the row's own spelling when that is one, else the longest. Nothing when
// no writing carries a legal form.
std::optional<std::string> full_legal_name(const RequestMappingEntry &entry) {
	if (!organisation_type(entry.type)) return std::nullopt;
	const auto withForm = [](const std::string &writing) {
		for (const WordSpan &span : word_spans(writing))
			if (legal_form_word(writing.substr(span.start, span.end - span.start))) return true;
		return false;
	};
	const auto shouted = [](const std::string &writing) { return upper_case(writing) == writing; };
	// Only a writing the material itself wrote, whole: never a span that took
	// the text around a name with it (P0, 30 Oct 2026: "Brightwater Foods,
	// LLC]\"}", the longest, had been the one written into a caption).
	// And one this request's material itself wrote, where the row says where
	// its writings came from: never a spelling only a model or the registry had.
	const auto clean = [&entry](const std::string &writing) {
		return !reads_as_cut_of_text(writing) && written_by_the_material(entry, writing);
	};
	if (withForm(entry.original) && !shouted(entry.original) && clean(entry.original)) return entry.original;
	// Of the others, the one the material writes most; the first met on a tie.
	std::optional<std::string> best;
	std::size_t bestTimes = 0;
	for (const std::string &writing : entry.every_form()) {
		if (!withForm(writing) || shouted(writing) || !clean(writing)) continue;
		const std::size_t times = entry.times_written(writing);
		if (!best || times > bestTimes) {
			best = writing;
			bestTimes = times;
		}
	}
	return best;
}

// The writing of an identifier or an organisation a symbol is written back as
// where it stands, byte for byte one this operation met - never rewritten. A
// file's name takes the writing file names were met in ("Договор № ФП-044-П
// .docx": a model finds a file only by its own name); everywhere else a code
// takes the document's own writing, the one its text uses most, then the
// lawyer's, then a model's earlier answer's - a file name's writing only when
// nothing else wrote it: "ФП-044/П" in a letter, never "ФП-044-П" (11 times
// in one run, 26 Sep 2026). An organisation is named by its full registered
// name ("Corvane Audio, Inc.", not the "Corvane" met first); its short part
// ([[E3.short]]) says the short name. Any other row keeps its spelling.
std::string identity_writing(const RequestMappingEntry &entry, const std::string &text, std::size_t position,
		std::size_t end, bool framed_by_model) {
	const bool identifier = entry.type == EntityType::ContractIdentifier || entry.type == EntityType::CaseNumber
		|| entry.type == EntityType::TaxId || entry.type == EntityType::BankAccount;
	const bool organisation = organisation_type(entry.type);
	if (!identifier && !organisation) return restoration_spelling(entry);
	const auto most = [&](unsigned origin) -> const std::string * {
		const std::string *best = nullptr;
		std::size_t times = 0;
		for (const auto &[writing, count] : entry.writing_counts) {
			const auto found = entry.writing_origins.find(writing);
			if (found == entry.writing_origins.end() || (found->second & origin) == 0) continue;
			if (best == nullptr || count > times) {
				best = &writing;
				times = count;
			}
		}
		return best;
	};
	if (in_file_name(text, position, end))
		if (const std::string *written = most(RequestMappingEntry::FileName)) return *written;
	if (organisation) {
		// In prose a symbol comes back as the writing it was sent as (D-172): the
		// document is authoritative, and "Harlan" is never rewritten "Harlan
		// Retail Group, Inc.". Where a draft names a party in full - its
		// definition, its signature block - restore() writes the full name the
		// material itself wrote.
		static_cast<void>(framed_by_model);
		return restoration_spelling(entry);
	}
	for (const unsigned origin : {static_cast<unsigned>(RequestMappingEntry::DocumentText),
			static_cast<unsigned>(RequestMappingEntry::LawyerRequest), static_cast<unsigned>(RequestMappingEntry::ModelAuthored)})
		if (const std::string *written = most(origin)) return *written;
	return restoration_spelling(entry);
}

// Whether a symbol stands where a draft names a party in full: its
// definition ('[[E3]], a Texas limited liability company ("Vendor")',
// «[[E3]] (далее — «Поставщик»)»), its signature block ("Vendor: [[E3]]" or
// the name alone on a line above "By:") or a notice's addressee ("To:",
// "Attention:"). A contract names a party there by its full registered name.
bool stands_where_a_party_is_named(const std::string &text, std::size_t position, std::size_t end) {
	const std::size_t lineStart = text.rfind('\n', position) == std::string::npos ? 0 : text.rfind('\n', position) + 1;
	std::size_t lineEnd = text.find('\n', end);
	if (lineEnd == std::string::npos) lineEnd = text.size();
	const std::string before = text.substr(lineStart, position - lineStart);
	std::string after = text.substr(end, std::min<std::size_t>(lineEnd - end, 120));
	// A definition right after the name.
	std::string lowered = ascii_lower(after);
	if (lowered.rfind(", a ", 0) == 0 || lowered.rfind(", an ", 0) == 0) {
		for (const char *kind : {"corporation", "company", "partnership", "trust", "association", "bank"})
			if (lowered.find(kind) != std::string::npos && lowered.find(kind) < 70) return true;
	}
	for (const char *opening : {" (\"", " (the \"", " (\xe2\x80\x9c", " (the \xe2\x80\x9c", " (\xc2\xab",
			" (hereinafter", " (\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xb5\xd0\xb5", ", \xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x83\xd0\xb5\xd0\xbc",
			", \xd0\xb2 \xd0\xbb\xd0\xb8\xd1\x86\xd0\xb5"})  // (далее / , именуем / , в лице
		if (after.rfind(opening, 0) == 0) return true;
	// A label for the party or the addressee before it on its line: "Vendor:",
	// "If to Customer:", "Attention:", «Поставщик:».
	std::string label = before;
	while (!label.empty() && (label.back() == ' ' || label.back() == '\t')) label.pop_back();
	if (!label.empty() && label.back() == ':' && label.size() <= 40 && label.find('[') == std::string::npos
			&& label.find('.') == std::string::npos)
		return true;
	// The name alone on its line, a signature line under it.
	std::string rest = after;
	while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t' || rest.back() == ':' || rest.back() == ',')) rest.pop_back();
	const bool alone = before.find_first_not_of(" \t") == std::string::npos && rest.find_first_not_of(" \t") == std::string::npos;
	if (!alone || lineEnd >= text.size()) return false;
	std::size_t next = lineEnd + 1;
	while (next < text.size() && (text[next] == '\n' || text[next] == ' ' || text[next] == '\t')) ++next;
	const std::string nextLine = ascii_lower(text.substr(next, 24));
	for (const char *signature : {"by:", "by ", "name:", "title:", "signature", "_", "/s/",
			"\xd0\xbf\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb8\xd1\x81\xd1\x8c", "\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb8\xd1\x81\xd1\x8c",
			"\xd0\xa4\xd0\x98\xd0\x9e"})  // подпись, Подпись, ФИО
		if (nextLine.rfind(signature, 0) == 0) return true;
	return false;
}

namespace {

// A piece of a restored text and what the model wrote there: its own words,
// or a symbol restoration wrote as the value it stands for.
struct RestoredPiece {
	std::size_t start = 0;
	std::size_t end = 0;
	std::string source;
	bool restored = false;
	// Part of a value's writing was cut away by an earlier repair.
	bool trimmed = false;
};

// What the model itself wrote for a range of a restored text: its own words
// as a person reads them, and each value by the symbol it wrote - a value
// only partly inside the range by which part.
std::string provider_writing(const std::string &output, const std::vector<RestoredPiece> &pieces, std::size_t from,
		std::size_t to) {
	std::string writing;
	for (const RestoredPiece &piece : pieces) {
		if (piece.end <= from || piece.start >= to) continue;
		const std::size_t start = std::max(from, piece.start);
		const std::size_t end = std::min(to, piece.end);
		if (!piece.restored) {
			writing += platform::folded_text(output.substr(start, end - start));
			continue;
		}
		writing += "\x01" + piece.source;
		if (piece.trimmed || start != piece.start || end != piece.end)
			writing += "@" + std::to_string(start - piece.start) + ":" + std::to_string(end - piece.start);
		writing += "\x01";
	}
	return writing;
}

// The restored value a range lies wholly inside, if any.
const RestoredPiece *value_holding(const std::vector<RestoredPiece> &pieces, std::size_t from, std::size_t to) {
	for (const RestoredPiece &piece : pieces)
		if (piece.restored && piece.start <= from && to <= piece.end) return &piece;
	return nullptr;
}

bool touches_value(const std::vector<RestoredPiece> &pieces, std::size_t from, std::size_t to) {
	return std::any_of(pieces.begin(), pieces.end(), [&](const RestoredPiece &piece) {
		return piece.restored && piece.start < to && from < piece.end;
	});
}

// Whether cutting a range would leave part of a restored value behind.
bool cuts_into_value(const std::vector<RestoredPiece> &pieces, std::size_t from, std::size_t to) {
	return std::any_of(pieces.begin(), pieces.end(), [&](const RestoredPiece &piece) {
		return piece.restored && piece.start < to && from < piece.end && !(from <= piece.start && piece.end <= to);
	});
}

// What stands between two writings of one phrase in a list or side by side.
bool repetition_gap(const std::string &gap) {
	static const std::set<std::string> gaps{", ", " and ", " or ", " and/or ", ", and ", ", or ", ", and/or ", "; ",
		" \xD0\xB8 ", " \xD0\xB8\xD0\xBB\xD0\xB8 ", " \xD0\xBB\xD0\xB8\xD0\xB1\xD0\xBE ",  // и, или, либо
		", \xD0\xB0 \xD1\x82\xD0\xB0\xD0\xBA\xD0\xB6\xD0\xB5 ", " "};                     // , а также
	return gaps.count(gap) > 0;
}

// Where restoring a text wrote one phrase twice side by side that the model
// never wrote twice - one value under two symbols ("[[E1]], [[E2]]" both
// "injunctive relief"), a value's words beside the same words the model wrote
// itself ("[[E1]] relief, injunctive relief", "[[E3]] LLC" reading "LLC LLC"),
// a short name before the whole name - the range to cut: the separator and one
// writing. A repetition the model wrote itself - its own words twice ("New
// York, New York", "had had") or one symbol twice - is the model's text and
// is never touched, and neither is a figure or anything inside one value.
std::optional<std::pair<std::size_t, std::size_t>> restoration_repetition(const std::string &output,
		const std::vector<WordSpan> &words, const std::vector<RestoredPiece> &pieces, std::size_t first,
		std::size_t length) {
	if (first + 2 * length > words.size()) return std::nullopt;
	const std::size_t firstStart = words[first].start;
	const std::size_t firstEnd = words[first + length - 1].end;
	const std::size_t secondStart = words[first + length].start;
	const std::size_t secondEnd = words[first + 2 * length - 1].end;
	const std::string gap = output.substr(firstEnd, secondStart - firstEnd);
	if (!repetition_gap(gap)) return std::nullopt;
	const std::string phrase = output.substr(firstStart, firstEnd - firstStart);
	const std::string again = output.substr(secondStart, secondEnd - secondStart);
	if (phrase.find_first_of(".;:\n\t0123456789") != std::string::npos
			|| again.find_first_of("0123456789") != std::string::npos)
		return std::nullopt;
	if (platform::folded_text(phrase) != platform::folded_text(again)) return std::nullopt;
	if (length == 1) {
		// One word twice: only a name or a legal form side by side, or a word of
		// four letters or more listed twice between commas.
		if (gap == " ") {
			if (!legal_form_word(phrase) && !words[first].capitalised) return std::nullopt;
		} else if (gap != ", " || phrase.size() < 4) {
			return std::nullopt;
		}
	}
	if (!touches_value(pieces, firstStart, firstEnd) && !touches_value(pieces, secondStart, secondEnd))
		return std::nullopt;
	const RestoredPiece *holder = value_holding(pieces, firstStart, secondEnd);
	if (holder != nullptr) return std::nullopt;
	if (provider_writing(output, pieces, firstStart, firstEnd) == provider_writing(output, pieces, secondStart, secondEnd))
		return std::nullopt;
	// The separator and the second writing go, or the first writing and the
	// separator where that keeps every value whole ("[[E3.short]] [[E3]]").
	if (!cuts_into_value(pieces, firstEnd, secondEnd)) return std::make_pair(firstEnd, secondEnd);
	if (!cuts_into_value(pieces, firstStart, secondStart)) return std::make_pair(firstStart, secondStart);
	return std::make_pair(firstEnd, secondEnd);
}

// Every restoration repetition in a restored text written once; the pieces and
// the Russian names' spans follow the text.
// The unit a word counts in, when it is one: a period, a scale, a currency, a
// percent. Empty for any other word. What "45 дней дней" repeats.
std::string unit_class(std::string word) {
	while (!word.empty() && (word.back() == ',' || word.back() == ';' || word.back() == ':' || word.back() == ')'
			|| word.back() == '!' || word.back() == '?'))
		word.pop_back();
	const std::string folded = normalized_entity_form(word);
	static const std::map<std::string, std::string> classes{
		{"day", "day"}, {"days", "day"}, {"день", "day"}, {"дня", "day"}, {"дней", "day"}, {"дню", "day"}, {"днями", "day"},
		{"week", "week"}, {"weeks", "week"}, {"неделя", "week"}, {"недели", "week"}, {"недель", "week"}, {"неделю", "week"},
		{"month", "month"}, {"months", "month"}, {"месяц", "month"}, {"месяца", "month"}, {"месяцев", "month"},
		{"year", "year"}, {"years", "year"}, {"год", "year"}, {"года", "year"}, {"лет", "year"},
		{"hour", "hour"}, {"hours", "hour"}, {"час", "hour"}, {"часа", "hour"}, {"часов", "hour"},
		{"million", "million"}, {"millions", "million"}, {"млн", "million"}, {"миллион", "million"},
		{"миллиона", "million"}, {"миллионов", "million"}, {"billion", "billion"}, {"billions", "billion"},
		{"млрд", "billion"}, {"миллиард", "billion"}, {"миллиарда", "billion"}, {"миллиардов", "billion"},
		{"thousand", "thousand"}, {"тыс", "thousand"}, {"тысяч", "thousand"}, {"тысячи", "thousand"},
		{"dollar", "usd"}, {"dollars", "usd"}, {"usd", "usd"}, {"рубль", "rub"}, {"рубля", "rub"},
		{"рублей", "rub"}, {"руб", "rub"}, {"rub", "rub"}, {"euro", "eur"}, {"euros", "eur"}, {"eur", "eur"},
		{"percent", "percent"}, {"процент", "percent"}, {"процента", "percent"}, {"процентов", "percent"},
	};
	const auto found = classes.find(folded);
	if (found != classes.end()) return found->second;
	if (word == "\xE2\x82\xBD") return "rub";
	if (word == "%") return "percent";
	return {};
}

// A word that says what kind of a period it is: "calendar", «рабочих».
bool unit_qualifier(const std::string &word) {
	static const std::set<std::string> qualifiers{"calendar", "business", "working", "banking", "full", "consecutive",
		"календарных", "календарный", "календарные", "рабочих", "рабочий", "рабочие", "банковских", "банковский",
		"полных", "полный"};
	return qualifiers.count(normalized_entity_form(word)) > 0;
}

// A restored value already carries its unit ("45 дней", "$30 million"), and a
// model that writes the unit after the symbol as well doubled it: "45 дней
// дней" 17 times in one run, "45 дней календарных дней", "$30 million
// million" (26 Sep 2026). Only right after a restored value, never in the
// model's own words ("had had"): a unit repeated as it stands is written once
// - the value's own writing agrees with its number - and a unit the model
// qualified ("календарных дней") keeps the model's words, the value its
// number alone.
void cut_restored_unit_repeats(std::string &output, std::vector<RestoredPiece> &pieces,
		std::vector<restoration_grammar::RestoredSpan> &spans) {
	const auto isSpace = [&](std::size_t at) {
		return at < output.size() && (output[at] == ' ' || output.compare(at, 2, "\xC2\xA0") == 0);
	};
	const auto spaceLength = [&](std::size_t at) { return output[at] == ' ' ? std::size_t{1} : std::size_t{2}; };
	const auto wordAt = [&](std::size_t at) {
		std::size_t end = at;
		while (end < output.size() && !isSpace(end) && output[end] != '\n' && output[end] != '\t') ++end;
		return std::make_pair(at, end);
	};
	for (std::size_t index = pieces.size(); index-- > 0;) {
		const RestoredPiece &piece = pieces[index];
		if (!piece.restored || piece.end <= piece.start) continue;
		// The last word of the restored value, and the space before it.
		std::size_t unitStart = piece.end;
		while (unitStart > piece.start && !isSpace(unitStart - 1)
				&& !(unitStart >= piece.start + 2 && output.compare(unitStart - 2, 2, "\xC2\xA0") == 0))
			--unitStart;
		if (unitStart <= piece.start) continue;
		const std::string unitWord = output.substr(unitStart, piece.end - unitStart);
		const std::string unit = unit_class(unitWord);
		if (unit.empty()) continue;
		std::size_t gapStart = unitStart;
		while (gapStart > piece.start && (output[gapStart - 1] == ' ' || (gapStart >= piece.start + 2
				&& output.compare(gapStart - 2, 2, "\xC2\xA0") == 0)))
			gapStart -= output[gapStart - 1] == ' ' ? 1 : 2;
		// The value's own qualifier, when it has one: "10 business days".
		std::string ownQualifier;
		if (gapStart > piece.start) {
			std::size_t qualifierStart = gapStart;
			while (qualifierStart > piece.start && !isSpace(qualifierStart - 1)) --qualifierStart;
			const std::string candidate = output.substr(qualifierStart, gapStart - qualifierStart);
			if (unit_qualifier(candidate)) ownQualifier = normalized_entity_form(candidate);
		}
		// What the model wrote right after the value.
		std::size_t cursor = piece.end;
		if (!isSpace(cursor)) continue;
		while (isSpace(cursor)) cursor += spaceLength(cursor);
		auto [firstStart, firstEnd] = wordAt(cursor);
		std::string first = output.substr(firstStart, firstEnd - firstStart);
		std::string qualifier;
		std::size_t modelUnitStart = firstStart;
		std::size_t modelUnitEnd = firstEnd;
		if (unit_qualifier(first)) {
			qualifier = normalized_entity_form(first);
			std::size_t next = firstEnd;
			if (!isSpace(next)) continue;
			while (isSpace(next)) next += spaceLength(next);
			const auto [secondStart, secondEnd] = wordAt(next);
			modelUnitStart = secondStart;
			modelUnitEnd = secondEnd;
		}
		std::string modelUnit = output.substr(modelUnitStart, modelUnitEnd - modelUnitStart);
		if (unit_class(modelUnit) != unit) continue;
		// The unit's own trailing mark stays: "45 days." keeps its stop.
		std::size_t cutEnd = modelUnitEnd;
		while (cutEnd > modelUnitStart && (output[cutEnd - 1] == ',' || output[cutEnd - 1] == ';' || output[cutEnd - 1] == ':'
				|| output[cutEnd - 1] == ')' || output[cutEnd - 1] == '!' || output[cutEnd - 1] == '?'
				|| (output[cutEnd - 1] == '.' && unit != "rub")))
			--cutEnd;
		std::size_t from = 0;
		std::size_t to = 0;
		if (qualifier.empty() || qualifier == ownQualifier) {
			// The same unit written again: the model's copy goes.
			from = piece.end;
			to = cutEnd;
		} else {
			// The model qualified the unit: the value keeps its number alone.
			from = gapStart;
			to = piece.end;
		}
		if (to <= from) continue;
		const std::size_t removed = to - from;
		output.erase(from, removed);
		const auto moved = [&](std::size_t offset) {
			if (offset <= from) return offset;
			if (offset >= to) return offset - removed;
			return from;
		};
		for (RestoredPiece &other : pieces) {
			const std::size_t start = moved(other.start);
			const std::size_t end = moved(other.end);
			if (end - start != other.end - other.start) other.trimmed = true;
			other.start = start;
			other.end = end;
		}
		std::vector<restoration_grammar::RestoredSpan> keptSpans;
		for (restoration_grammar::RestoredSpan span : spans) {
			const std::size_t start = moved(span.offset);
			const std::size_t end = moved(span.offset + span.length);
			if (end - start != span.length) continue;
			span.offset = start;
			keptSpans.push_back(std::move(span));
		}
		spans = std::move(keptSpans);
	}
}

void write_restoration_repetitions_once(std::string &output, std::vector<RestoredPiece> &pieces,
		std::vector<restoration_grammar::RestoredSpan> &spans, std::vector<std::string> &written_once) {
	for (std::size_t round = 0; round < 64; ++round) {
		const std::vector<WordSpan> words = word_spans(output);
		std::optional<std::pair<std::size_t, std::size_t>> cut;
		std::string phrase;
		for (std::size_t first = 0; first < words.size() && !cut; ++first) {
			// The longest phrase first: "injunctive relief, injunctive relief" is one
			// repetition, never "relief" twice.
			for (std::size_t length = std::min<std::size_t>(8, (words.size() - first) / 2); length >= 1 && !cut;
					--length) {
				cut = restoration_repetition(output, words, pieces, first, length);
				if (cut) phrase = output.substr(words[first].start, words[first + length - 1].end - words[first].start);
			}
		}
		if (!cut) return;
		const std::size_t from = cut->first;
		const std::size_t to = cut->second;
		const std::size_t removed = to - from;
		const auto moved = [&](std::size_t offset) {
			if (offset <= from) return offset;
			if (offset >= to) return offset - removed;
			return from;
		};
		output.erase(from, removed);
		std::vector<RestoredPiece> kept;
		for (RestoredPiece piece : pieces) {
			const std::size_t start = moved(piece.start);
			const std::size_t end = moved(piece.end);
			if (start == end) continue;
			if (end - start != piece.end - piece.start) piece.trimmed = true;
			piece.start = start;
			piece.end = end;
			kept.push_back(std::move(piece));
		}
		pieces = std::move(kept);
		std::vector<restoration_grammar::RestoredSpan> keptSpans;
		for (restoration_grammar::RestoredSpan span : spans) {
			const std::size_t start = moved(span.offset);
			const std::size_t end = moved(span.offset + span.length);
			// A name cut into is left as it now reads, never declined.
			if (end - start != span.length) continue;
			span.offset = start;
			keptSpans.push_back(std::move(span));
		}
		spans = std::move(keptSpans);
		if (std::find(written_once.begin(), written_once.end(), phrase) == written_once.end())
			written_once.push_back(phrase);
	}
}

// A count Singularity worked out ([[Count1]], DESIGN4 §4.4), as a whole number,
// and the noun the model wrote right after it in the form that number takes:
// «3 счёта», «5 счетов», «21 день»; "1 invoice", "3 invoices". Only the nouns
// of a closed, legal-general list are agreed; any other word is the model's
// and stays as written. `end` moves past a noun it rewrote.
// The case a Russian count's noun is in, read from the word before the count:
// «в течение 3 дней», «не более 1 месяца» - a genitive governor; «по 3 дня»,
// «с 5 днями» - another case, where Singularity leaves the model's noun alone;
// anything else - the counting (nominative or accusative) position.
enum class CountCase { Counting, Genitive, Other };

CountCase russian_count_case(const std::string &text, std::size_t position) {
	const std::size_t from = position > 48 ? position - 48 : 0;
	const std::vector<std::string> words = roles::lexicon_detail::folded_words(std::string_view(text).substr(from, position - from));
	if (words.empty()) return CountCase::Counting;
	const std::string &last = words.back();
	const std::string pair = words.size() >= 2 ? words[words.size() - 2] + " " + last : std::string();
	static const std::set<std::string> genitivePairs{
		"\xD0\xB2 \xD1\x82\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",                    // в течение
		"\xD0\xBD\xD0\xB5 \xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5",                              // не более
		"\xD0\xBD\xD0\xB5 \xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5",                              // не менее
		"\xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB7\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB5",          // не позднее
		"\xD0\xBD\xD0\xB5 \xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xB5",                              // не ранее
		"\xD0\xBF\xD0\xBE \xD0\xB8\xD1\x81\xD1\x82\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB8",  // по истечении
		"\xD0\xB2 \xD0\xBA\xD0\xBE\xD0\xBB\xD0\xB8\xD1\x87\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2\xD0\xB5",  // в количестве
		"\xD0\xB2 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB0\xD1\x85"};        // в пределах
	if (!pair.empty() && genitivePairs.count(pair) > 0) return CountCase::Genitive;
	static const std::set<std::string> genitive{
		"\xD0\xB8\xD0\xB7", "\xD0\xB4\xD0\xBE", "\xD0\xBE\xD1\x82",                                        // из до от
		"\xD0\xBF\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5", "\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xBB\xD0\xBE",      // после около
		"\xD0\xB4\xD0\xBB\xD1\x8F", "\xD0\xB1\xD0\xB5\xD0\xB7", "\xD1\x81\xD0\xB2\xD1\x8B\xD1\x88\xD0\xB5",  // для без свыше
		"\xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5", "\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5",      // более менее
		"\xD0\xBA\xD1\x80\xD0\xBE\xD0\xBC\xD0\xB5", "\xD0\xB2\xD0\xBC\xD0\xB5\xD1\x81\xD1\x82\xD0\xBE",  // кроме вместо
		"\xD1\x81\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB8", "\xD1\x83", "\xD0\xB2\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4\xD1\x81\xD1\x82\xD0\xB2\xD0\xB8\xD0\xB5",  // среди у вследствие
		"\xD0\xBF\xD0\xBE\xD1\x81\xD1\x80\xD0\xB5\xD0\xB4\xD1\x81\xD1\x82\xD0\xB2\xD0\xBE\xD0\xBC"};  // посредством
	static const std::set<std::string> other{
		"\xD0\xBF\xD0\xBE", "\xD0\xBA", "\xD0\xBA\xD0\xBE", "\xD1\x81", "\xD1\x81\xD0\xBE",              // по к ко с со
		"\xD0\xBC\xD0\xB5\xD0\xB6\xD0\xB4\xD1\x83", "\xD0\xBE", "\xD0\xBE\xD0\xB1", "\xD0\xBF\xD1\x80\xD0\xB8",  // между о об при
		"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB4", "\xD0\xBD\xD0\xB0\xD0\xB4", "\xD0\xBF\xD0\xBE\xD0\xB4",  // перед над под
		"\xD0\xB2", "\xD0\xB2\xD0\xBE"};                                                                  // в во (accusative or prepositional)
	if (genitive.count(last) > 0) return CountCase::Genitive;
	if (other.count(last) > 0) return CountCase::Other;
	return CountCase::Counting;
}

std::string counted_writing(const semantic::Value &value, const std::string &text, std::size_t position, std::size_t &end,
		semantic::Language language) {
	const semantic::Rational whole = value.amount.rounded(0);
	semantic::Format plain;
	plain.style = semantic::FormatStyle::Number;
	std::string digits;
	try {
		digits = semantic::display(semantic::make_number(whole), plain, language);
	} catch (const semantic::AlgebraError &) {
		digits = whole.to_string();
	}
	if (end >= text.size() || text[end] != ' ') return digits;
	const std::size_t wordStart = end + 1;
	std::size_t wordEnd = wordStart;
	while (wordEnd < text.size() && std::string_view(" ,.;:!?)\n\t").find(text[wordEnd]) == std::string_view::npos) ++wordEnd;
	const std::string word = text.substr(wordStart, wordEnd - wordStart);
	if (word.empty()) return digits;
	struct Forms {
		const char *one;
		const char *few;
		const char *many;
	};
	static const Forms russian[] = {{"день", "дня", "дней"}, {"месяц", "месяца", "месяцев"}, {"год", "года", "лет"},
		{"неделя", "недели", "недель"}, {"строка", "строки", "строк"}, {"счёт", "счёта", "счетов"}, {"счет", "счета", "счетов"},
		{"платёж", "платежа", "платежей"}, {"платеж", "платежа", "платежей"}, {"раз", "раза", "раз"},
		{"документ", "документа", "документов"}, {"позиция", "позиции", "позиций"}, {"период", "периода", "периодов"},
		{"случай", "случая", "случаев"}, {"требование", "требования", "требований"}, {"договор", "договора", "договоров"},
		{"акт", "акта", "актов"}, {"квартал", "квартала", "кварталов"}, {"поставка", "поставки", "поставок"}};
	static const Forms english[] = {{"day", "days", "days"}, {"month", "months", "months"}, {"year", "years", "years"},
		{"week", "weeks", "weeks"}, {"row", "rows", "rows"}, {"invoice", "invoices", "invoices"}, {"payment", "payments", "payments"},
		{"time", "times", "times"}, {"document", "documents", "documents"}, {"item", "items", "items"},
		{"period", "periods", "periods"}, {"claim", "claims", "claims"}, {"contract", "contracts", "contracts"},
		{"entry", "entries", "entries"}, {"quarter", "quarters", "quarters"}, {"delivery", "deliveries", "deliveries"},
		{"shipment", "shipments", "shipments"}};
	const bool russian_text = language == semantic::Language::Russian;
	const Forms *first = russian_text ? std::begin(russian) : std::begin(english);
	const Forms *last = russian_text ? std::end(russian) : std::end(english);
	const CountCase grammaticalCase = russian_text ? russian_count_case(text, position) : CountCase::Counting;
	// Another case than these two: the model's noun is its own grammar.
	if (grammaticalCase == CountCase::Other) return digits;
	for (const Forms *forms = first; forms != last; ++forms) {
		if (word != forms->one && word != forms->few && word != forms->many) continue;
		const semantic::Int128 count = whole.numerator();
		std::string agreed;
		if (!russian_text) {
			agreed = count == 1 || count == -1 ? std::string(forms->one) : std::string(forms->many);
		} else if (grammaticalCase == CountCase::Genitive) {
			// «1 дня», «21 месяца», «3 дней», «11 лет»: the genitive singular after
			// a count ending in one (never eleven) is the table's second form.
			const semantic::Int128 magnitude = count < 0 ? -count : count;
			agreed = magnitude % 10 == 1 && magnitude % 100 != 11 ? std::string(forms->few) : std::string(forms->many);
		} else {
			agreed = semantic::russian_plural(count, forms->one, forms->few, forms->many);
		}
		end = wordEnd;
		return digits + " " + agreed;
	}
	return digits;
}

// Whether `length` characters at `at` stand as a word of their own: nothing of
// a word, a number or a symbol's brackets touches them, and no ".part" follows.
bool stands_as_symbol_word(const std::string &text, std::size_t at, std::size_t length) {
	const auto joins = [](char character) {
		return markers::is_ascii_letter(character) || markers::is_ascii_digit(character) || character == '_'
			|| character == '[' || character == ']';
	};
	if (at > 0 && joins(text[at - 1])) return false;
	const std::size_t end = at + length;
	if (end < text.size() && joins(text[end])) return false;
	if (end + 1 < text.size() && text[end] == '.' && markers::is_ascii_letter(text[end + 1])) return false;
	return true;
}

// A typed symbol a model wrote without its brackets - "Person4" for
// "[[Person4]]" - written whole again, so it is restored like any other. Only
// a typed symbol this request issued (a compact "E17" may be a document's own
// word), standing as a word, and only where the text sent never wrote that
// word itself; each is said among the normalizations.
std::string with_unbracketed_symbols_whole(const std::string &text, const ProtectionResult &request,
		std::vector<std::string> &normalized) {
	std::vector<std::tuple<std::size_t, std::size_t, std::string>> found;
	for (const auto &entry : request.mapping) {
		if (entry.placeholder.size() < 5 || entry.placeholder.rfind("[[", 0) != 0) continue;
		const std::string identity = entry.placeholder.substr(2, entry.placeholder.size() - 4);
		if (marker_letters(identity).size() < 2) continue;
		bool isSentAsWord = false;
		for (std::size_t at = request.protected_text.find(identity); at != std::string::npos && isSentAsWord == false;
				at = request.protected_text.find(identity, at + 1))
			isSentAsWord = stands_as_symbol_word(request.protected_text, at, identity.size());
		if (isSentAsWord) continue;
		bool isSaid = false;
		for (std::size_t at = text.find(identity); at != std::string::npos; at = text.find(identity, at + 1)) {
			// A ".part" after it: a part a symbol is written with ("Org2.short",
			// "Person4.first", "Amount3.words") is that symbol's part, written whole
			// with it; any other word after the dot (".docx") follows the symbol
			// itself (a live talk, 26 Oct 2026: "under Org2.short's wording",
			// "Org3.docx").
			const std::size_t end = at + identity.size();
			std::size_t partEnd = end;
			std::string part;
			if (end + 1 < text.size() && text[end] == '.' && markers::is_ascii_letter(text[end + 1])) {
				partEnd = end + 1;
				while (partEnd < text.size() && markers::is_ascii_letter(text[partEnd])) ++partEnd;
				part = text.substr(end + 1, partEnd - end - 1);
				std::string compared = part;
				for (char &character : compared) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
				static const std::set<std::string> parts{"short", "first", "last", "patronymic", "words", "digitsandwords"};
				if (parts.count(compared) == 0) part.clear();
			}
			const bool joinsBefore = at > 0 && (markers::is_ascii_letter(text[at - 1]) || markers::is_ascii_digit(text[at - 1])
				|| text[at - 1] == '_' || text[at - 1] == '[' || text[at - 1] == ']');
			const bool joinsAfter = end < text.size() && (markers::is_ascii_letter(text[end]) || markers::is_ascii_digit(text[end])
				|| text[end] == '_' || text[end] == '[' || text[end] == ']');
			if (joinsBefore == true || joinsAfter == true) continue;
			if (part.empty() == false) {
				const std::string withPart = entry.placeholder.substr(0, entry.placeholder.size() - 2) + "." + part + "]]";
				found.emplace_back(at, partEnd - at, withPart);
			} else {
				found.emplace_back(at, identity.size(), entry.placeholder);
			}
			if (isSaid == false) normalized.push_back(identity + " -> " + entry.placeholder);
			isSaid = true;
		}
	}
	if (found.empty()) return text;
	// Right to left, so each earlier place is still where it was found.
	std::sort(found.begin(), found.end(), [](const auto &left, const auto &right) { return std::get<0>(left) > std::get<0>(right); });
	std::string whole = text;
	for (const auto &[at, length, placeholder] : found) whole.replace(at, length, placeholder);
	return whole;
}

} // namespace

RestorationResult PapinianGuard::restore(const std::string &provider_text_as_written,
                                         const ProtectionResult &request,
                                         RestorationField field) const {
  RestorationResult result;
  const std::string provider_text = with_unbracketed_symbols_whole(provider_text_as_written, request,
      result.normalized_placeholders);
  result.text = provider_text;
  std::unordered_map<std::string, const RequestMappingEntry *> mapping;
  for (const auto &entry : request.mapping) mapping.emplace(entry.placeholder, &entry);
  // The part a symbol asks for, when the row has it: [[E5.first]].
  const auto partOf = [](const RequestMappingEntry &entry, const std::string &part) -> std::string {
    if (part == "short") {
      const std::optional<std::string> shortName = organisation_short_name(entry);
      return shortName ? *shortName : std::string();
    }
    const std::optional<PersonNameParts> parts = person_name_parts(entry);
    if (!parts) return {};
    if (part == "first") return parts->first;
    if (part == "last") return parts->last;
    if (part == "patronymic") return parts->patronymic;
    return {};
  };
	// A display format a quantity's or a date's symbol asks for (DESIGN4 §4.4,
	// owner 26 Sep 2026): [[Amount3.words]], [[Amount3.digitsandwords]],
	// [[Date4.words]] - written below from the value, never by the model.
	const auto formatOf = [](const RequestMappingEntry &entry, const std::string &part)
			-> std::optional<restoration_grammar::DisplayFormat> {
		if (!entry.value || entry.value_key.empty()) return std::nullopt;
		return restoration_grammar::display_format_named(part);
	};

	// Every symbol the table issued, by its identity compared case-blind
	// ("amount3", "e17"), and the letters its symbols begin with: a table writes
	// typed symbols ([[Amount3]], [[Org1]]) or compact ones ([[E17]]) - a stored
	// operation's, or a text that already wrote a typed symbol's shape - and a
	// response is read in whichever it was sent.
	const auto lowered = [](std::string value) {
		for (char &character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		return value;
	};
	std::unordered_map<std::string, const RequestMappingEntry *> byIdentity;
	std::set<std::string> issuedLetters;
	for (const auto &entry : request.mapping) {
		if (entry.placeholder.size() < 5) continue;
		const std::string identity = entry.placeholder.substr(2, entry.placeholder.size() - 4);
		byIdentity.emplace(lowered(identity), &entry);
		issuedLetters.insert(lowered(marker_letters(identity)));
	}
	// One symbol shape at `open`: "[[" or "[", spaces, the letters, spaces, the
	// digits, a ".part" (in double brackets), spaces, and the matching close.
	struct Candidate {
		std::size_t length = 0;
		std::string letters;
		std::string digits;
		std::string part;
	};
	const auto candidateAt = [&](const std::string &value, std::size_t open, bool doubled) -> std::optional<Candidate> {
		std::size_t at = open + (doubled ? 2 : 1);
		const auto spaces = [&]() { while (at < value.size() && std::isspace(static_cast<unsigned char>(value[at])) != 0) ++at; };
		spaces();
		const std::size_t lettersStart = at;
		while (at < value.size() && markers::is_ascii_letter(value[at]) && at - lettersStart < 12) ++at;
		if (at == lettersStart) return std::nullopt;
		Candidate candidate;
		candidate.letters = value.substr(lettersStart, at - lettersStart);
		if (issuedLetters.count(lowered(candidate.letters)) == 0) return std::nullopt;
		spaces();
		const std::size_t digitsStart = at;
		while (at < value.size() && markers::is_ascii_digit(value[at]) && at - digitsStart < 7) ++at;
		if (at == digitsStart) return std::nullopt;
		candidate.digits = value.substr(digitsStart, at - digitsStart);
		spaces();
		if (doubled && at < value.size() && value[at] == '.') {
			++at;
			spaces();
			const std::size_t partStart = at;
			while (at < value.size() && markers::is_ascii_letter(value[at])) ++at;
			if (at == partStart) return std::nullopt;
			candidate.part = lowered(value.substr(partStart, at - partStart));
			spaces();
		}
		const std::string close = doubled ? "]]" : "]";
		if (value.compare(at, close.size(), close) != 0) return std::nullopt;
		candidate.length = at + close.size() - open;
		return candidate;
	};
	if (!request.mapping.empty()) {
		std::string unmatched = provider_text;
		std::map<std::string, std::string> normalizations;
		// Where each symbol written another way stands: it is rewritten there
		// and nowhere else. "[E5]" is also the middle of a correct "[[E5]]";
		// rewriting it everywhere made that one "[[[E5]]]", restored as
		// "[Victor Hale]".
		std::vector<std::tuple<std::size_t, std::size_t, std::string>> rewrites;
		for (std::size_t open = provider_text.find('['); open != std::string::npos; open = provider_text.find('[', open + 1)) {
			std::optional<Candidate> found;
			if (open + 1 < provider_text.size() && provider_text[open + 1] == '[') found = candidateAt(provider_text, open, true);
			if (!found) found = candidateAt(provider_text, open, false);
			if (!found) continue;
			const std::string candidate = provider_text.substr(open, found->length);
			unmatched.replace(open, candidate.size(), candidate.size(), ' ');
			const auto known = byIdentity.find(lowered(found->letters + found->digits));
			// A part the row's name does not show is as unknown as a symbol never issued.
			if (known == byIdentity.end() || (!found->part.empty() && partOf(*known->second, found->part).empty()
					&& !formatOf(*known->second, found->part))) {
				result.integrity_ok = false;
				result.unknown_or_malformed_placeholders.push_back(candidate);
			} else {
				const std::string &base = known->second->placeholder;
				const std::string canonical = found->part.empty() ? base
					: base.substr(0, base.size() - 2) + "." + found->part + "]]";
				if (candidate != canonical) {
					rewrites.emplace_back(open, candidate.size(), canonical);
					if (normalizations.emplace(candidate, canonical).second)
						result.normalized_placeholders.push_back(candidate + " -> " + canonical);
				}
			}
			open += found->length - 1;
		}
		// Right to left, so each earlier place is still where it was found.
		for (auto rewrite = rewrites.rbegin(); rewrite != rewrites.rend(); ++rewrite)
			result.text.replace(std::get<0>(*rewrite), std::get<1>(*rewrite), std::get<2>(*rewrite));
		// What is left that opens like a symbol and never closes as one.
		for (std::size_t open = unmatched.find('['); open != std::string::npos; open = unmatched.find('[', open + 1)) {
			std::size_t at = open;
			while (at < unmatched.size() && unmatched[at] == '[') ++at;
			while (at < unmatched.size() && std::isspace(static_cast<unsigned char>(unmatched[at])) != 0) ++at;
			const std::size_t lettersStart = at;
			while (at < unmatched.size() && markers::is_ascii_letter(unmatched[at]) && at - lettersStart < 12) ++at;
			if (at == lettersStart || issuedLetters.count(lowered(unmatched.substr(lettersStart, at - lettersStart))) == 0) continue;
			while (at < unmatched.size() && std::isspace(static_cast<unsigned char>(unmatched[at])) != 0) ++at;
			const std::size_t digitsStart = at;
			while (at < unmatched.size() && markers::is_ascii_digit(unmatched[at])) ++at;
			if (at == digitsStart) continue;
			result.integrity_ok = false;
			result.unknown_or_malformed_placeholders.push_back(unmatched.substr(open, at - open));
			open = at - 1;
		}
	}
  if (request.mapping.empty()) return result;

  // One pass, left to right, so every name written back is known where it
  // stands: a Russian name then takes the case its place in the sentence asks
  // for (RestorationGrammar), and a quantity the form the answer's language
  // writes it in.
  obs::emit({obs::OperationEventKind::PapinianRestoreProgress, "Applying request-only restoration entries",
      {{"processed", "0"}, {"total", std::to_string(request.mapping.size())}}, std::nullopt});
  const std::string &text = result.text;
  // The answer's language, read from its words: a JSON envelope's keys and
  // literals ("type", "final", null) are no language of the answer's.
  const semantic::Language language = [&text]() {
    static const std::regex syntax(R"("[A-Za-z_]+"\s*:|\bnull\b|\btrue\b|\bfalse\b)");
    if (text.find("\":") == std::string::npos) return semantic::language_of(text);
    return semantic::language_of(std::regex_replace(text, syntax, " "));
  }();
  std::string output;
  output.reserve(text.size());
  std::vector<restoration_grammar::RestoredSpan> spans;
  // Whose name each span is, and whether it is a part of one ([[E5.first]]).
  std::vector<std::pair<const RequestMappingEntry *, bool>> spanRows;
  // What the model wrote at each piece of the output: its own words, or a
  // symbol written as its value.
  std::vector<RestoredPiece> pieces;
  const auto ownWords = [&](std::size_t from, std::size_t to) {
    if (to <= from) return;
    pieces.push_back({output.size(), output.size() + (to - from), std::string(), false, false});
    output.append(text, from, to - from);
  };
  std::size_t cursor = 0;
  std::size_t restoredSymbols = 0;
  // The pieces restoration writes an amount or a date as, with the fact each
  // is, in the order they stand: the anchors a stored answer keeps.
  std::vector<std::pair<std::string, std::string>> anchoredWritings;
  for (std::size_t position = text.find("[["); position != std::string::npos; position = text.find("[[", position + 1)) {
    if (position < cursor) continue;
    std::size_t at = position + 2;
    const std::size_t lettersStart = at;
    while (at < text.size() && markers::is_ascii_letter(text[at]) && at - lettersStart < 12) ++at;
    if (at == lettersStart) continue;
    const std::size_t digitsStart = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
    if (at == digitsStart) continue;
    const std::size_t digitsEnd = at;
    std::string part;
    if (at < text.size() && text[at] == '.') {
      std::size_t partEnd = at + 1;
      while (partEnd < text.size() && text[partEnd] >= 'a' && text[partEnd] <= 'z') ++partEnd;
      part = text.substr(at + 1, partEnd - at - 1);
      at = partEnd;
    }
    if (text.compare(at, 2, "]]") != 0) continue;
    const auto known = mapping.find("[[" + text.substr(lettersStart, digitsEnd - lettersStart) + "]]");
    if (known == mapping.end()) continue;
    const RequestMappingEntry &entry = *known->second;
    std::size_t end = at + 2;
    // "[[Amount1]] ([[Amount1]])": the material wrote the figure and its words
    // ("$21,350.00 (twenty-one thousand three hundred fifty dollars)") and the
    // model kept both - written as the figure and its words, from the value, so
    // the words never become a second figure and a changed figure its words.
    if (part.empty() && entry.value && !entry.value_key.empty() && field != RestorationField::Cell) {
      const std::string again = " (" + entry.placeholder + ")";
      if (text.compare(end, again.size(), again) == 0) {
        part = "digitsandwords";
        end += again.size();
      }
    }
    const bool operand = position >= kOperandOpen.size()
        && text.compare(position - kOperandOpen.size(), kOperandOpen.size(), kOperandOpen) == 0
        && text.compare(end, kOperandClose.size(), kOperandClose) == 0;
    std::string written;
    std::optional<restoration_grammar::SpanKind> kind;
    std::string canonical;
    russian_names::Gender gender = russian_names::Gender::Unknown;
    // Where the model's own words end before what this symbol is written as:
    // the symbol itself, or a Russian salutation written whole.
    std::size_t salutationStart = position;
    bool salutationWritten = false;
    // Where a draft names a party - its definition, its signature block, a
    // notice's addressee - a company is written by its full registered name,
    // whichever of its symbols the model wrote there, and the words of that
    // name the model wrote after its short part are that same name.
    // A name the model framed itself - «[[E1]]», "[[E1]]", ООО [[E1]] - keeps
    // the frame it was given, never a second legal form inside it.
    const auto framedByModel = [&]() {
      const std::string before = text.substr(position >= 16 ? position - 16 : 0, position >= 16 ? 16 : position);
      for (const char *quote : {"\xC2\xAB", "\xE2\x80\x9C", "\"", "\xE2\x80\x9E", "'"})
        if (before.size() >= std::char_traits<char>::length(quote)
            && before.compare(before.size() - std::char_traits<char>::length(quote), std::string::npos, quote) == 0)
          return true;
      const std::vector<WordSpan> words = word_spans(before);
      return !words.empty() && words.back().end + 1 >= before.size()
          && legal_form_word(before.substr(words.back().start, words.back().end - words.back().start));
    };
    std::optional<std::string> partyName;
    if (!operand && organisation_type(entry.type) && (part.empty() || part == "short") && !framedByModel()) {
      const std::optional<std::string> full = full_legal_name(entry);
      if (full) {
        const std::string base = name_without_legal_form(*full);
        std::size_t nameEnd = end;
        if (part == "short") {
          const std::optional<std::string> shortName = organisation_short_name(entry);
          if (shortName && !base.empty() && base.rfind(*shortName, 0) == 0) {
            const std::string continued = base.substr(shortName->size());
            if (!continued.empty() && text.compare(nameEnd, continued.size(), continued) == 0) nameEnd += continued.size();
          }
        }
        // The legal form the model wrote after the symbol is the one the name
        // already carries: "[[E3]] LLC" never reads "LLC LLC".
        if (!base.empty() && full->size() > base.size()) {
          const std::string suffix = full->substr(base.size());
          if (text.compare(nameEnd, suffix.size(), suffix) == 0) nameEnd += suffix.size();
        }
        if (stands_where_a_party_is_named(text, position, nameEnd)) {
          partyName = full;
          end = nameEnd;
        }
      }
    }
    if (partyName) {
      written = *partyName;
      kind = restoration_grammar::SpanKind::Organisation;
      canonical = written;
    } else if (const std::optional<restoration_grammar::DisplayFormat> format = part.empty() || operand
        ? std::nullopt : formatOf(entry, part)) {
      // The figure in the form the model asked for, from the value: its words
      // are Singularity's, in the answer's language; a value with no such form (a
      // month, a quarter) is written as prose writes it.
      if (const std::optional<std::string> shown = restoration_grammar::spelled(*entry.value, *format, language))
        written = *shown;
      else
        written = prose_writing(entry, language, std::string(), false, false);
      // «[[Amount1.digitsandwords]] 00 копеек»: the material's kopecks stood
      // outside the figure's span, and the rendering writes them itself.
      if (entry.value->kind == semantic::Kind::Money && *format != restoration_grammar::DisplayFormat::Digits
          && restoration_grammar::writes_minor_units(written))
        if (const std::optional<std::size_t> minor = restoration_grammar::minor_units_after(text, end)) end += *minor;
      // The words the model left after it spell the figure that stood there
      // before: the figure's own replace them.
      if (field == RestorationField::Prose && *format != restoration_grammar::DisplayFormat::Words)
        if (const std::optional<std::size_t> stale = restoration_grammar::spelling_after(text, end, *entry.value)) {
          if (const std::optional<std::string> both = restoration_grammar::spelled(*entry.value,
                  restoration_grammar::DisplayFormat::DigitsAndWords, language)) {
            written = *both;
            end += *stale;
          }
        }
    } else if (!part.empty()) {
      written = partOf(entry, part);
      if (written.empty()) continue;
      // A part of a Russian name takes its case like the whole name does:
      // «Уважаемая Ольга Николаевна», «направлено Лебедеву», «с Андреем Игоревичем».
      if (has_cyrillic_letter(written) && person_type(entry.type)) {
        kind = restoration_grammar::SpanKind::Person;
        canonical = written;
        gender = row_person_gender(entry);
      }
    } else if (entry.value && !entry.value_key.empty()) {
      const std::string note = shown_note_after(text, end);
      const bool wholeField = position == 0 && end == text.size();
      if (operand) written = typed_writing(entry, language);
      // A workbook's own reading ("B5: [[E3]] (shown in $)") and a cell's whole
      // value keep the cell's writing - a number cell takes nothing else; an
      // exact quotation of a document keeps the source's; prose takes the
      // answer's form, and a field that is the value alone is prose too unless
      // the source's writing already says what it is in its own words.
      else if (!note.empty()) written = workbook_writing(entry, note);
      else if (stands_as_cell_value(text, position)) written = workbook_writing(entry, std::string());
      else if (wholeField && field == RestorationField::Cell) written = workbook_writing(entry, std::string());
      else if (field == RestorationField::Quote) written = quoted_writing(entry, language);
      else if (wholeField && says_what_it_is(entry.original, entry) && writes_exactly(entry.original, *entry.value))
        written = entry.original;
      else if (const std::optional<std::string> cell = table_cell_writing(entry, text, position, language)) written = *cell;
      else if (const std::optional<std::string> range = range_end_writing(entry, text, position, end, mapping)) written = *range;
      else if (const std::optional<std::string> own = own_date_writing(entry, text, position, end, language)) written = *own;
      // A count Singularity worked out ([[Count1]]) is a whole number, and the noun
      // the model wrote after it takes the count's number (DESIGN4 §4.4).
      else if (entry.value->kind == semantic::Kind::Count) written = counted_writing(*entry.value, text, position, end, language);
      else written = prose_writing(entry, language, currency_beside(text, position, end), percent_beside(text, end),
          year_beside(text, end));
      // Words the model left after a figure in prose - the words of whatever
      // figure stood there before - are written again from this value, so a
      // changed figure never stands beside stale words (owner, 26 Sep 2026).
      if (!operand && note.empty() && field == RestorationField::Prose && !stands_as_cell_value(text, position))
        if (const std::optional<std::size_t> stale = restoration_grammar::spelling_after(text, end, *entry.value))
          if (const std::optional<std::string> both = restoration_grammar::spelled(*entry.value,
                  restoration_grammar::DisplayFormat::DigitsAndWords, language)) {
            written = *both;
            end += *stale;
          }
    } else if (person_type(entry.type)) {
      const std::string fullest = fullest_person_writing(entry);
      gender = row_person_gender(entry);
      if (has_cyrillic_letter(fullest)) {
        // The fullest writing, in the nominative; its place in the sentence
        // declines it below.
        written = russian_names::declined(fullest, russian_names::Case::Nominative, gender);
        // «Уважаемый [[E4]]!» - a Russian letter addresses a person by the given
        // name and patronymic, «Уважаемый Андрей Игоревич!», never «Уважаемый
        // господин Лебедев А. И.» (ab4 R1, 25 Sep 2026): the whole salutation
        // is written here, its adjective agreeing with the person's gender.
        if (const std::optional<RussianSalutation> salutation = russian_salutation(entry, text, position, end, gender)) {
          salutationStart = salutation->start;
          end = salutation->end;
          written = salutation->text;
          salutationWritten = true;
        }
      } else {
        written = restoration_spelling(entry);
        if (person_name(fullest).parts > person_name(written).parts && properly_capitalised(fullest)) written = fullest;
        std::string before = text.substr(position >= 5 ? position - 5 : 0, position >= 5 ? 5 : position);
        for (char &character : before) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (before == "dear ")
          if (const std::optional<std::string> salutation = salutation_writing(entry)) written = *salutation;
      }
      kind = restoration_grammar::SpanKind::Person;
      canonical = written;
      // A salutation is written whole, in the form it takes: nothing declines it.
      if (salutationWritten) kind.reset();
    } else if (const std::optional<std::string> rows = entry.derived
        ? restoration_grammar::row_labels(entry.original, language) : std::nullopt) {
      // A row set Singularity worked out: its rows' labels, as the answer lists them.
      written = *rows;
    } else {
      written = identity_writing(entry, text, position, end, organisation_type(entry.type) && framedByModel());
      if (entry.type == EntityType::Organisation || entry.type == EntityType::LegalEntity) {
        kind = restoration_grammar::SpanKind::Organisation;
        canonical = written;
      }
    }
    // A value that ends in a full stop ("Larkspur Health Partners, Inc.") and
    // closes a sentence would otherwise read "Inc..": the model, which never
    // saw the value, put the sentence's own stop after the symbol.
    if (!written.empty() && written.back() == '.' && end < text.size() && text[end] == '.'
        && (end + 1 >= text.size() || text[end + 1] != '.'))
      ++end;
    ownWords(cursor, salutationStart);
    if (kind && !operand) {
      restoration_grammar::RestoredSpan span;
      span.offset = output.size();
      span.length = written.size();
      span.kind = *kind;
      span.canonical = canonical;
      span.gender = *kind == restoration_grammar::SpanKind::Person ? gender : russian_names::gender_of(canonical);
      // The parts of one name written side by side are that name, declined
      // as one: "[[E5.first]] [[E5.patronymic]]".
      if (!spans.empty() && !part.empty() && spanRows.back().first == &entry && spanRows.back().second
          && spans.back().offset + spans.back().length + 1 == span.offset && output[span.offset - 1] == ' ') {
        spans.back().length = span.offset + span.length - spans.back().offset;
        spans.back().canonical = output.substr(spans.back().offset, spans.back().length - written.size()) + written;
      } else {
        spans.push_back(std::move(span));
        spanRows.emplace_back(&entry, !part.empty());
      }
    }
    // Every restored span of a fact is anchored - a count, a yes/no, a row set a
    // derived row writes, as much as an amount (DESIGN4 §4.4) - so text read
    // again is re-protected from its anchors, never re-detected.
    if (!entry.fact_key.empty() && (entry.value || entry.derived) && !written.empty())
      anchoredWritings.emplace_back(written, entry.fact_key);
    pieces.push_back({output.size(), output.size() + written.size(), text.substr(salutationStart, end - salutationStart), true, false});
    output += written;
    cursor = end;
    ++restoredSymbols;
  }
  ownWords(cursor, text.size());
  // A phrase restoration wrote twice side by side that the model wrote once -
  // a live amendment read "seek injunctive relief, injunctive relief or
  // specific performance" (25 Sep 2026) - is written once, in prose only: an
  // exact quotation and a cell keep what they are.
  if (field != RestorationField::Cell && field != RestorationField::Quote && restoredSymbols > 0)
    cut_restored_unit_repeats(output, pieces, spans);
  if (field == RestorationField::Prose && restoredSymbols > 0) {
    write_restoration_repetitions_once(output, pieces, spans, result.written_once);
    if (!result.written_once.empty())
      platform::trace::record("papinian", "restore.written_once", {{"phrases", result.written_once.size()}});
  }
  // Russian names in the case their place asks for, and the words that agree
  // with them in their gender. Nothing else in the text is changed.
  if (!spans.empty() && has_cyrillic_letter(output)) output = restoration_grammar::agree(output, spans);
  result.text = std::move(output);
  result.fact_anchors = anchors_in_order(result.text, anchoredWritings);
  obs::emit({obs::OperationEventKind::PapinianRestoreProgress, "Applied request-only restoration entries",
      {{"processed", std::to_string(restoredSymbols)}, {"total", std::to_string(request.mapping.size())}}, std::nullopt});
  return result;
}

std::optional<SemanticModelIdentity> PapinianGuard::semantic_model_identity() const {
  return semantic_model_ ? std::optional<SemanticModelIdentity>(semantic_model_->identity())
                         : std::nullopt;
}

// ---------------------------------------------------------------------------
// Lane P2 (Papinian round 3): values Singularity worked out itself.

std::string PapinianGuard::issue_derived_value(OperationProtectedSymbolTable &operation_symbols,
		const semantic::Value &value, EntityType type, const std::string &derivation) const {
	// A value Singularity worked out is a fact of its own, named by how it was worked
	// out - the operation over its operands' facts - and never by its value: an
	// equal amount the material writes, or one worked out another way, is
	// another fact, and that they are equal is a relation Singularity sends only
	// when a task needs it. The same derivation keeps its symbol across the
	// calls of one operation while it still comes to the same value.
	const std::string key = semantic::value_key(value);
	const auto known = operation_symbols.derived_by_derivation.find(derivation);
	if (known != operation_symbols.derived_by_derivation.end() && known->second < operation_symbols.mapping.size()
			&& operation_symbols.mapping[known->second].value_key == key)
		return operation_symbols.mapping[known->second].placeholder;
	std::set<std::string> issuedSymbols;
	for (const RequestMappingEntry &entry : operation_symbols.mapping) issuedSymbols.insert(entry.placeholder);
	if (operation_symbols.marker_family.empty()) {
		operation_symbols.marker_family = "E";
		if (config_.marker_style && operation_symbols.mapping.empty()) operation_symbols.marker_style = *config_.marker_style;
	}
	RequestMappingEntry created;
	created.placeholder = next_placeholder(operation_symbols, issuedSymbols, std::string(),
		markers::prefix_for(type, value, DetectionSource::Deterministic));
	try {
		created.original = semantic::display(value, {}, semantic::Language::English);
	} catch (const semantic::AlgebraError &) {
		created.original = key;
	}
	created.type = type;
	created.source = DetectionSource::Deterministic;
	created.value = value;
	created.value_key = key;
	created.derived = true;
	created.provenance.origin = FactProvenance::Origin::Derived;
	created.provenance.derivation = derivation;
	created.fact_key = fact_key_of(created.provenance);
	operation_symbols.mapping.push_back(std::move(created));
	const std::size_t row = operation_symbols.mapping.size() - 1;
	operation_symbols.derived_by_derivation[derivation] = row;
	operation_symbols.fact_by_key[operation_symbols.mapping[row].fact_key] = row;
	// A count or a period Singularity counted is its own number: no figure of a text
	// is ever read as it, so it is no value a leak is looked for by either.
	if (!counted_by_singularity(operation_symbols.mapping[row])) index_fact_value(operation_symbols, row, std::string());
	return operation_symbols.mapping[row].placeholder;
}

std::string PapinianGuard::issue_occurrence_fact(OperationProtectedSymbolTable &operation_symbols,
		const semantic::Value &value, EntityType type, const FactProvenance &provenance) const {
	// One place a validated text still writes a value no fact of the table can be
	// proven to be: masked as a fact of its own, met there. Never left readable,
	// never given another fact's identity because their values are equal.
	FactProvenance place = provenance;
	place.origin = FactProvenance::Origin::Occurrence;
	const std::string factKey = fact_key_of(place);
	const auto known = operation_symbols.fact_by_key.find(factKey);
	if (known != operation_symbols.fact_by_key.end() && known->second < operation_symbols.mapping.size())
		return operation_symbols.mapping[known->second].placeholder;
	std::set<std::string> issuedSymbols;
	for (const RequestMappingEntry &entry : operation_symbols.mapping) issuedSymbols.insert(entry.placeholder);
	if (operation_symbols.marker_family.empty()) operation_symbols.marker_family = "E";
	RequestMappingEntry created;
	created.placeholder = next_placeholder(operation_symbols, issuedSymbols, std::string(),
		markers::prefix_for(type, value, DetectionSource::Deterministic));
	try {
		created.original = semantic::display(value, {}, semantic::Language::English);
	} catch (const semantic::AlgebraError &) {
		created.original = semantic::value_key(value);
	}
	created.type = type;
	created.source = DetectionSource::Deterministic;
	created.value = value;
	created.value_key = semantic::value_key(value);
	created.provenance = place;
	created.fact_key = factKey;
	operation_symbols.mapping.push_back(std::move(created));
	const std::size_t row = operation_symbols.mapping.size() - 1;
	operation_symbols.fact_by_key[factKey] = row;
	index_fact_value(operation_symbols, row, std::string());
	++operation_symbols.occurrence_facts_issued;
	return operation_symbols.mapping[row].placeholder;
}

// DESIGN4 §4.2: a value worked out at the model's request is a fact of its
// own, named by its canonical AST over its operands' fact keys - never by its
// value. The same AST keeps its symbol while it comes to the same value; a row
// set, a group set or a truth value kept as a symbol takes the protocol's own
// prefix ([[Rows1]], [[Groups1]], [[Flag1]]).
std::string PapinianGuard::issue_derived_fact(OperationProtectedSymbolTable &operation_symbols,
		compute::DerivedFact &fact) const {
	const std::string derivation = "compute:" + fact.ast;
	const std::string head = fact.type.substr(0, fact.type.find('('));
	const bool structural = head == "ROW_SET" || head == "GROUP_SET";
	const std::string key = structural ? head + ":" + fact.ast : semantic::value_key(fact.value);
	const auto known = operation_symbols.derived_by_derivation.find(derivation);
	if (known != operation_symbols.derived_by_derivation.end() && known->second < operation_symbols.mapping.size()
			&& operation_symbols.mapping[known->second].value_key == key) {
		// The fact it already is keeps its first name and record.
		const RequestMappingEntry &row = operation_symbols.mapping[known->second];
		fact.placeholder = row.placeholder;
		fact.fact_key = row.fact_key;
		operation_symbols.derived_facts.emplace(fact.placeholder, fact);
		return fact.placeholder;
	}
	// The same fact an earlier answer named: its anchor's key is this very
	// derivation's, so the history's symbol is this fact - one fact, one
	// symbol, never a second row that takes the key from it (a fake-provider
	// run on E1: the history's [[Duration1]] and the recomputed [[Duration2]]
	// shared a key, and everything worked out from the first lost its trace).
	if (!structural) {
		FactProvenance worked;
		worked.origin = FactProvenance::Origin::Derived;
		worked.derivation = derivation;
		const auto named = operation_symbols.fact_by_key.find(fact_key_of(worked));
		if (named != operation_symbols.fact_by_key.end() && named->second < operation_symbols.mapping.size()
				&& row_value_agrees(operation_symbols.mapping[named->second], fact.value, key)) {
			const RequestMappingEntry &row = operation_symbols.mapping[named->second];
			operation_symbols.derived_by_derivation[derivation] = named->second;
			fact.placeholder = row.placeholder;
			fact.fact_key = row.fact_key;
			operation_symbols.derived_facts[fact.placeholder] = fact;
			return fact.placeholder;
		}
	}
	std::set<std::string> issuedSymbols;
	for (const RequestMappingEntry &entry : operation_symbols.mapping) issuedSymbols.insert(entry.placeholder);
	if (operation_symbols.marker_family.empty()) {
		operation_symbols.marker_family = "E";
		if (config_.marker_style && operation_symbols.mapping.empty()) operation_symbols.marker_style = *config_.marker_style;
	}
	const EntityType type = fact.value.kind == semantic::Kind::Money ? EntityType::MoneyAmount
		: fact.value.kind == semantic::Kind::Percentage ? EntityType::Percentage
		: fact.value.kind == semantic::Kind::Date || fact.value.kind == semantic::Kind::DayOfYear ? EntityType::Date
		: EntityType::Number;
	const std::string prefix = head == "ROW_SET" ? std::string("Rows")
		: head == "GROUP_SET" ? std::string("Groups")
		: head == "BOOLEAN" || fact.value.kind == semantic::Kind::Boolean ? std::string("Flag")
		: head == "COUNT" ? std::string("Count")
		: markers::prefix_for(type, fact.value, DetectionSource::Deterministic);
	RequestMappingEntry created;
	created.placeholder = next_placeholder(operation_symbols, issuedSymbols, std::string(), prefix);
	if (!structural) {
		try {
			created.original = semantic::display(fact.value, {}, semantic::Language::English);
		} catch (const semantic::AlgebraError &) {
			created.original = key;
		}
		created.value = fact.value;
	}
	created.type = type;
	created.source = DetectionSource::Deterministic;
	created.value_key = key;
	created.derived = true;
	created.open_value = fact.stated_openly;
	created.provenance.origin = FactProvenance::Origin::Derived;
	created.provenance.derivation = derivation;
	created.fact_key = fact_key_of(created.provenance);
	operation_symbols.mapping.push_back(std::move(created));
	const std::size_t row = operation_symbols.mapping.size() - 1;
	operation_symbols.derived_by_derivation[derivation] = row;
	operation_symbols.fact_by_key[operation_symbols.mapping[row].fact_key] = row;
	// A set, a truth value, a count or a period Singularity counted is its own
	// answer: no figure of a text is ever read as it.
	// A value the material writes openly is masked nowhere on its account.
	if (!structural && prefix != "Flag" && !fact.stated_openly && !counted_by_singularity(operation_symbols.mapping[row]))
		index_fact_value(operation_symbols, row, std::string());
	fact.placeholder = operation_symbols.mapping[row].placeholder;
	fact.fact_key = operation_symbols.mapping[row].fact_key;
	operation_symbols.derived_facts[fact.placeholder] = fact;
	return fact.placeholder;
}

// DESIGN4 §6.2: a workbook cell no prompt carried yet, as a Source fact of
// that cell. A cell a prompt already carried is that fact; nothing is scored.
std::string PapinianGuard::issue_cell_fact(OperationProtectedSymbolTable &operation_symbols, const semantic::Value &value,
		EntityType type, const FactProvenance &provenance) const {
	FactProvenance place = provenance;
	place.origin = FactProvenance::Origin::Source;
	if (!place.cell.empty()) {
		const auto byCell = operation_symbols.fact_by_cell.find(place.cell);
		if (byCell != operation_symbols.fact_by_cell.end() && byCell->second < operation_symbols.mapping.size()
				&& operation_symbols.mapping[byCell->second].value_key == semantic::value_key(value))
			return operation_symbols.mapping[byCell->second].placeholder;
	}
	const std::string factKey = fact_key_of(place);
	const auto known = operation_symbols.fact_by_key.find(factKey);
	if (known != operation_symbols.fact_by_key.end() && known->second < operation_symbols.mapping.size())
		return operation_symbols.mapping[known->second].placeholder;
	std::set<std::string> issuedSymbols;
	for (const RequestMappingEntry &entry : operation_symbols.mapping) issuedSymbols.insert(entry.placeholder);
	if (operation_symbols.marker_family.empty()) operation_symbols.marker_family = "E";
	RequestMappingEntry created;
	created.placeholder = next_placeholder(operation_symbols, issuedSymbols, std::string(),
		markers::prefix_for(type, value, DetectionSource::Deterministic));
	try {
		created.original = semantic::display(value, {}, semantic::Language::English);
	} catch (const semantic::AlgebraError &) {
		created.original = semantic::value_key(value);
	}
	created.type = type;
	created.source = DetectionSource::Deterministic;
	created.value = value;
	created.value_key = semantic::value_key(value);
	created.provenance = place;
	created.fact_key = factKey;
	operation_symbols.mapping.push_back(std::move(created));
	const std::size_t row = operation_symbols.mapping.size() - 1;
	operation_symbols.fact_by_key[factKey] = row;
	if (!place.cell.empty()) operation_symbols.fact_by_cell[place.cell] = row;
	index_fact_value(operation_symbols, row, std::string());
	return operation_symbols.mapping[row].placeholder;
}

void file_passages_under_documents(OperationProtectedSymbolTable &symbols,
		const std::vector<std::pair<std::string, std::string>> &passages) {
	std::map<std::string, std::vector<std::string>> passagesOfDocument;
	std::map<std::string, std::set<std::string>> documentsOfPassage;
	for (const auto &[document, digest] : passages) {
		if (document.empty() || digest.empty()) continue;
		std::vector<std::string> &digests = passagesOfDocument[document];
		if (std::find(digests.begin(), digests.end(), digest) == digests.end()) digests.push_back(digest);
		documentsOfPassage[digest].insert(document);
	}
	for (const auto &[document, digests] : passagesOfDocument) {
		std::set<std::string> earlier;
		std::string joined;
		for (const std::string &digest : digests) {
			joined += digest;
			if (documentsOfPassage[digest].size() > 1) continue;
			const auto known = symbols.document_of_passage.find(digest);
			if (known != symbols.document_of_passage.end() && !known->second.empty()) earlier.insert(known->second);
		}
		const std::string scope = earlier.size() == 1 ? *earlier.begin() : platform::sha256_hex_digest(joined).substr(0, 16);
		for (const std::string &digest : digests)
			symbols.document_of_passage[digest] = documentsOfPassage[digest].size() > 1 ? std::string() : scope;
	}
}

void record_restored_facts(OperationProtectedSymbolTable &symbols, const std::string &restored_text,
		const std::vector<FactAnchor> &anchors) {
	for (const FactAnchor &anchor : anchors) {
		if (anchor.key.empty() || anchor.length == 0 || anchor.start + anchor.length > restored_text.size()) continue;
		const auto [from, to] = sentence_around(restored_text, anchor.start, anchor.start + anchor.length);
		// The sentence must say more than the figure: a field that is only the
		// figure ("$5,000,000") proves nothing about where it stands next time.
		if (to < from || to - from < anchor.length + 12) continue;
		const std::string sentence = restored_text.substr(from, to - from);
		const std::size_t offset = anchor.start - from;
		if (!has_letter(sentence.substr(0, offset) + " " + sentence.substr(offset + anchor.length))) continue;
		std::vector<OperationProtectedSymbolTable::RestoredPlace> &places =
			symbols.restored_writings[restored_text.substr(anchor.start, anchor.length)];
		const bool known = std::any_of(places.begin(), places.end(), [&](const OperationProtectedSymbolTable::RestoredPlace &place) {
			return place.offset == offset && place.key == anchor.key && place.sentence == sentence;
		});
		if (!known) places.push_back({sentence, offset, anchor.key});
	}
}

const char *to_string(FactProvenance::Origin origin) {
	switch (origin) {
		case FactProvenance::Origin::Source: return "source";
		case FactProvenance::Origin::ProjectContext: return "context";
		case FactProvenance::Origin::History: return "history";
		case FactProvenance::Origin::Request: return "request";
		case FactProvenance::Origin::Instruction: return "instruction";
		case FactProvenance::Origin::Derived: return "derived";
		case FactProvenance::Origin::Occurrence: return "occurrence";
		case FactProvenance::Origin::External: return "external";
	}
	return "occurrence";
}

const RequestMappingEntry *row_of_marker(const OperationProtectedSymbolTable &symbols, std::string_view marker) {
	// "[[ amount3 ]]" is [[Amount3]]: the letters compared case-blind, the digits
	// exactly, a name part set aside.
	std::string wanted;
	bool inPart = false;
	for (const char character : marker) {
		if (character == '.') inPart = true;
		if (inPart) continue;
		if (std::isalnum(static_cast<unsigned char>(character)) != 0)
			wanted += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	if (wanted.empty()) return nullptr;
	for (const RequestMappingEntry &entry : symbols.mapping) {
		std::string issued;
		for (const char character : entry.placeholder)
			if (std::isalnum(static_cast<unsigned char>(character)) != 0)
				issued += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		if (issued == wanted) return &entry;
	}
	return nullptr;
}

} // namespace singularity::protected_api::papinian
