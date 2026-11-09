#include "Tool/ToolBroker/ToolBroker.hpp"

#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace singularity::tools {
namespace {

OrganizationToolPolicy &policy_override() {
  static OrganizationToolPolicy policy;
  return policy;
}

bool &has_policy_override() {
  static bool hasOverride = false;
  return hasOverride;
}

int mode_rank(ProjectActionMode mode) {
  switch (mode) {
    case ProjectActionMode::AskForApproval: return 0;
    case ProjectActionMode::ApproveSafeActions: return 1;
    case ProjectActionMode::FullProjectAccess: return 2;
  }
  return 0;
}

// An approval's id is what the lawyer's answer names, so it is unguessable: 16
// digits that are 64 bits from the system's secure source
// (Platform/SecureRandom), not the next value of a generator seeded once.
std::string random_id(const std::string &prefix) {
  return prefix + platform::secure_random_hex(8);
}

std::string current_timestamp() {
  return workspaces::current_timestamp();
}

// An approval filed outside a run begun through begin_operation() (a loop driven
// directly) cannot be told apart from one whose run filed it and ended (a change
// parked on its checkpoint). Its filer calls wait_for_approval() the moment
// request() returns, so one that has not come to wait within this long never
// will, and once answered nothing needs the entry any more.
std::chrono::milliseconds &waiter_arrival_grace() {
  static std::chrono::milliseconds grace(60000);
  return grace;
}

bool is_project_mutation(ToolEffectClass effect) {
  return effect == ToolEffectClass::RoutineProjectMutation
    || effect == ToolEffectClass::ElevatedProjectMutation;
}

bool is_canonical_capability(const std::string &capability) {
  return capability == "project.read" || capability == "web.search" || capability == "web.open"
    || capability == "document.patch.propose" || capability == "document.patch.apply"
		|| capability == "artifact.action.apply" || capability == "lawyer.ask";
}

// The effect of this one call: what the tool says of it, where it says
// anything a Project change can be, else the effect it is registered with.
ToolEffectClass effect_of_call(const ToolExecutionContext &context, const ToolDefinition &definition,
		const ToolIntent &intent) {
	if (!definition.effect_of || !is_project_mutation(definition.effect)) return definition.effect;
	try {
		const ToolEffectClass effect = definition.effect_of(context, intent.arguments);
		return is_project_mutation(effect) ? effect : definition.effect;
	} catch (const std::exception &) {
		return definition.effect;
	}
}

/* - - - - - - */

// The lawyer's words, as they are compared. Case, width and accents are folded
// by utf8proc - never a hand-made table - so "Don’t", "DON'T" and "don't" read
// alike, as do "…" and "…". An apostrophe joins ("don't" is "dont"). A
// comma, a dash or a bracket ends a clause, written as a "," word; a full
// stop, a question or an exclamation mark, a semicolon, a colon or a line
// break ends a sentence. Each sentence is its own list of words.
const std::string kClauseBreak = ",";

std::string folded(const std::string &text) {
	utf8proc_uint8_t *mapped = nullptr;
	const utf8proc_ssize_t length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(text.data()),
		static_cast<utf8proc_ssize_t>(text.size()), &mapped, static_cast<utf8proc_option_t>(UTF8PROC_STABLE
			| UTF8PROC_COMPAT | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD | UTF8PROC_STRIPMARK | UTF8PROC_IGNORE));
	if (length < 0 || mapped == nullptr) {
		std::free(mapped);
		return text;
	}
	std::string result(reinterpret_cast<const char *>(mapped), static_cast<std::size_t>(length));
	std::free(mapped);
	return result;
}

bool is_word_character(utf8proc_int32_t point) {
	switch (utf8proc_category(point)) {
		case UTF8PROC_CATEGORY_LU: case UTF8PROC_CATEGORY_LL: case UTF8PROC_CATEGORY_LT:
		case UTF8PROC_CATEGORY_LM: case UTF8PROC_CATEGORY_LO: case UTF8PROC_CATEGORY_MN:
		case UTF8PROC_CATEGORY_MC: case UTF8PROC_CATEGORY_ME: case UTF8PROC_CATEGORY_ND:
		case UTF8PROC_CATEGORY_NL: case UTF8PROC_CATEGORY_NO:
			return true;
		default:
			return false;
	}
}

// `questions`, when given, receives one flag per sentence: true for a sentence
// that ends in a question mark.
std::vector<std::vector<std::string>> sentences_of(const std::string &text, std::vector<bool> *questions = nullptr) {
	const std::string value = folded(text);
	std::vector<std::vector<std::string>> sentences(1);
	std::vector<bool> asked(1, false);
	std::string word;
	const auto endWord = [&] {
		if (!word.empty()) sentences.back().push_back(word);
		word.clear();
	};
	const auto endClause = [&] {
		endWord();
		if (!sentences.back().empty() && sentences.back().back() != kClauseBreak) sentences.back().push_back(kClauseBreak);
	};
	const auto endSentence = [&](bool isQuestion) {
		endWord();
		while (!sentences.back().empty() && sentences.back().back() == kClauseBreak) sentences.back().pop_back();
		if (sentences.back().empty()) return;
		asked.back() = isQuestion;
		sentences.emplace_back();
		asked.push_back(false);
	};
	const auto *bytes = reinterpret_cast<const utf8proc_uint8_t *>(value.data());
	const utf8proc_ssize_t size = static_cast<utf8proc_ssize_t>(value.size());
	utf8proc_ssize_t at = 0;
	while (at < size) {
		utf8proc_int32_t point = 0;
		const utf8proc_ssize_t length = utf8proc_iterate(bytes + at, size - at, &point);
		if (length <= 0) {
			endWord();
			++at;
			continue;
		}
		const utf8proc_ssize_t start = at;
		at += length;
		if (is_word_character(point)) {
			word.append(value, static_cast<std::size_t>(start), static_cast<std::size_t>(length));
			continue;
		}
		if (point == '\'' || point == 0x2019 || point == 0x2018 || point == 0x02BC || point == '`') continue;
		// A full stop inside a number or a short form ("9.1", "e.g.") ends no
		// sentence.
		if (point == '.' && at < size && value[static_cast<std::size_t>(at)] != ' '
				&& value[static_cast<std::size_t>(at)] != '\n' && value[static_cast<std::size_t>(at)] != '\r') {
			endWord();
			continue;
		}
		if (point == '.' || point == '!' || point == '?' || point == ';' || point == ':' || point == '\n'
				|| point == '\r' || point == 0x2026) {
			endSentence(point == '?');
			continue;
		}
		if (point == ',' || point == 0x2013 || point == 0x2014 || point == '(' || point == ')') {
			endClause();
			continue;
		}
		endWord();
	}
	endSentence(false);
	if (sentences.back().empty()) {
		sentences.pop_back();
		asked.pop_back();
	}
	if (questions != nullptr) *questions = std::move(asked);
	return sentences;
}

// Every word of the text, in order, its clause and sentence breaks left out.
std::vector<std::string> words_of(const std::string &text) {
	std::vector<std::string> words;
	for (const std::vector<std::string> &sentence : sentences_of(text))
		for (const std::string &word : sentence)
			if (word != kClauseBreak) words.push_back(word);
	return words;
}

bool starts_with_words(const std::vector<std::string> &words, std::size_t at, const std::vector<std::string> &part);

// Each written phrase as the words it is compared as.
std::vector<std::vector<std::string>> phrases_of(std::initializer_list<const char *> written) {
	std::vector<std::vector<std::string>> phrases;
	for (const char *phrase : written) {
		std::vector<std::string> words = words_of(phrase);
		if (!words.empty()) phrases.push_back(std::move(words));
	}
	// The longest first, so "would you mind" is read before "would you".
	std::stable_sort(phrases.begin(), phrases.end(), [](const auto &left, const auto &right) {
		return left.size() > right.size(); });
	return phrases;
}

std::set<std::string> word_set_of(std::initializer_list<const char *> written) {
	std::set<std::string> words;
	for (const char *word : written)
		for (std::string &folded : words_of(word)) words.insert(std::move(folded));
	return words;
}

// How many words of `phrases` begin at `at`: the first that does, or none.
std::size_t phrase_at(const std::vector<std::string> &sentence, std::size_t at,
		const std::vector<std::vector<std::string>> &phrases) {
	for (const std::vector<std::string> &phrase : phrases)
		if (starts_with_words(sentence, at, phrase)) return phrase.size();
	return 0;
}

// The words a lawyer asks for work on a document with. A fixed, bounded list:
// a change asked for in words it does not hold waits for one approval more
// than it needed, and nothing is ever written for words it wrongly holds that
// the lawyer did not write.
struct RequestWords {
	// Asking for nothing themselves ("And…", "Please…", "…").
	std::set<std::string> leads;
	// A request put politely: "Could you…", "Do you mind…", "…".
	std::vector<std::vector<std::string>> frames;
	// A request put impersonally: "Is it possible to…", "…". Only a
	// question with no word of law or advice in it is one: "Is it possible to
	// extend the term under clause 3?" asks what clause 3 allows.
	std::vector<std::vector<std::string>> impersonal_frames;
	// A question whether something may be done, never a request: "…
	// … 14?", as "…".
	std::vector<std::vector<std::string>> question_openers;
	// The words that begin a question in English, "?" or not: "Can we delete
	// clause 14", "Is the cap market". "Do", "have" and "has" only before a
	// pronoun ("Do you…"), since "Do the redline." and "Have it reworded." ask.
	std::set<std::string> auxiliaries;
	std::set<std::string> pronoun_auxiliaries;
	std::set<std::string> pronouns;
	// A question that has any of these in it asks for advice, however it is put:
	// "Can you delete clause 14 without breaching the SPA?", "Would you change
	// anything in clause 9?", "…or would that be unenforceable anyway?".
	std::vector<std::vector<std::string>> advice_words;
	// A want, stated: "I'd like clause 9 updated…", "Need the cap changed…", "I
	// want a new NDA…", "… 60 …". What is wanted may be
	// named anywhere in the clause, and a new document by name alone.
	std::vector<std::vector<std::string>> desires;
	std::set<std::string> new_things;
	std::set<std::string> russian_states;
	// What the document must say, stated: "The governing law needs to be New
	// York.", "… 60 …". Never "shall be" or "must be", the
	// words of a clause pasted in.
	std::vector<std::vector<std::string>> obligations;
	// A request for information: "Tell me…", "Review… and tell me…", "…".
	std::vector<std::vector<std::string>> information_leads;
	// "Yes." "Go ahead." "…"
	std::set<std::string> affirmations;
	// English verbs of work on a document, every form ("update", "updates",
	// "updated", "updating"). English writes them as nouns as well ("the change",
	// "a draft"), so one counts as asking only where a request puts a verb.
	std::set<std::string> english_verbs;
	std::set<std::string> english_verb_predecessors;
	// Russian verbs of work on a document, form by form, never as a stem.
	std::set<std::string> russian_verbs;
	std::set<std::string> negations;
	// What adds a change to a sentence that began as a question: "…and restore it".
	std::set<std::string> coordinators;
};

const RequestWords &request_words() {
	static const RequestWords words = [] {
		RequestWords known;
		known.leads = word_set_of({"and", "so", "but", "also", "then", "now", "please", "kindly", "just", "\320\260", "\320\270", "\320\275\320\276", "\321\202\320\260\320\272",
			"\321\202\320\276\320\263\320\264\320\260", "\321\202\320\265\320\277\320\265\321\200\321\214", "\321\202\320\260\320\272\320\266\320\265", "\320\277\320\276\320\266\320\260\320\273\321\203\320\271\321\201\321\202\320\260", "\320\277\321\200\320\276\321\201\321\202\320\276"});
		// Never "can we" or "could we": "Can we delete clause 14?" asks whether
		// they may.
		known.frames = phrases_of({"can you", "could you", "would you", "will you", "can i get", "could i get",
			"can we get", "could we get", "would you mind", "do you mind", "is it possible for you to",
			"why don't you", "why not", "i'd like you to", "i would like you to", "i want you to", "i need you to",
			"you can", "please", "\320\274\320\276\320\266\320\265\321\210\321\214", "\320\274\320\276\320\266\320\265\321\202\320\265", "\320\274\320\276\320\263 \320\261\321\213", "\320\274\320\276\320\263\320\273\320\260 \320\261\321\213", "\320\274\320\276\320\263\320\273\320\270 \320\261\321\213", "\320\275\320\265 \320\274\320\276\320\263 \320\261\321\213", "\320\275\320\265 \320\274\320\276\320\263\320\273\320\260 \320\261\321\213",
			"\320\275\320\265 \320\274\320\276\320\263\320\273\320\270 \320\261\321\213", "\320\274\320\276\320\266\320\265\321\210\321\214 \320\273\320\270", "\320\274\320\276\320\266\320\265\321\202\320\265 \320\273\320\270", "\320\275\320\265 \320\274\320\276\320\266\320\265\321\210\321\214 \320\273\320\270", "\320\275\320\265 \320\274\320\276\320\266\320\265\321\202\320\265 \320\273\320\270", "\321\201\320\274\320\276\320\266\320\265\321\210\321\214", "\321\201\320\274\320\276\320\266\320\265\321\202\320\265",
			"\320\261\321\203\320\264\321\214 \320\264\320\276\320\261\321\200", "\320\261\321\203\320\264\321\214\321\202\320\265 \320\264\320\276\320\261\321\200\321\213", "\321\202\321\213", "\320\262\321\213", "you"});
		known.impersonal_frames = phrases_of({"is it possible to", "would it be possible to", "\320\274\320\276\320\266\320\275\320\276"});
		known.question_openers = phrases_of({"\320\274\320\276\320\266\320\275\320\276 \320\273\320\270", "\320\275\320\265\320\273\321\214\320\267\321\217 \320\273\320\270", "\321\201\321\202\320\276\320\270\321\202 \320\273\320\270", "\321\201\320\273\320\265\320\264\321\203\320\265\321\202 \320\273\320\270", "\320\275\320\260\320\264\320\276 \320\273\320\270", "\320\275\321\203\320\266\320\275\320\276 \320\273\320\270"});
		known.auxiliaries = word_set_of({"can", "could", "would", "will", "should", "shall", "may", "might", "must",
			"is", "are", "was", "were", "does", "did"});
		known.pronoun_auxiliaries = word_set_of({"do", "have", "has"});
		known.pronouns = word_set_of({"you", "we", "i", "they", "it", "he", "she", "there", "this", "that", "these",
			"those", "the", "a", "an", "our", "my", "your", "their", "its", "clause", "section", "anyone", "someone"});
		// Words of a clause a lawyer may well ask to add ("breach", "risk",
		// "something about") are not among them: only a question whether it may
		// be done.
		known.advice_words = phrases_of({"anything", "without breaching", "without breach", "without violating",
			"without infringing", "without being in breach", "be in breach", "put us in breach", "breaching",
			"violating", "legally", "lawfully", "lawful", "unlawful", "illegal", "enforceable", "unenforceable",
			"permitted", "permissible", "allowed to", "advisable", "a good idea", "wise", "risky", "compliant",
			"comply", "consistent with", "or would", "or should", "or is", "or will", "or does", "or do", "or can",
			"or could", "or not", "\320\261\320\265\320\267 \320\275\320\260\321\200\321\203\321\210\320\265\320\275\320\270\321\217", "\320\275\320\265 \320\275\320\260\321\200\321\203\321\210\320\260\321\217", "\320\267\320\260\320\272\320\276\320\275\320\275\320\276", "\320\275\320\265\320\267\320\260\320\272\320\276\320\275\320\275\320\276", "\320\277\321\200\320\260\320\262\320\276\320\274\320\265\321\200\320\275\320\276", "\320\264\320\276\320\277\321\203\321\201\321\202\320\270\320\274\320\276",
			"\320\262\320\277\321\200\320\260\320\262\320\265", "\320\277\320\276 \320\267\320\260\320\272\320\276\320\275\321\203", "\320\270\320\274\320\265\320\265\320\274 \320\277\321\200\320\260\320\262\320\276", "\321\200\320\270\321\201\320\272\320\276\320\262\320\260\320\275\320\275\320\276", "\320\270\320\273\320\270 \320\275\320\265\321\202"});
		known.desires = phrases_of({"i'd like", "i would like", "i want", "i need", "we need", "we want", "we'd like",
			"we would like", "need", "i'd prefer", "i would prefer", "\321\205\320\276\321\207\321\203", "\321\217 \321\205\320\276\321\207\321\203", "\321\205\320\276\321\202\320\265\320\273 \320\261\321\213", "\321\205\320\276\321\202\320\265\320\273\320\260 \320\261\321\213",
			"\321\217 \320\261\321\213 \321\205\320\276\321\202\320\265\320\273", "\321\217 \320\261\321\213 \321\205\320\276\321\202\320\265\320\273\320\260", "\321\205\320\276\321\202\320\270\320\274", "\321\205\320\276\321\202\320\265\320\273\320\276\321\201\321\214 \320\261\321\213", "\320\274\320\275\320\265 \320\275\321\203\320\266\320\275\320\276", "\320\274\320\275\320\265 \320\275\320\260\320\264\320\276", "\320\275\320\260\320\274 \320\275\321\203\320\266\320\275\320\276", "\320\275\320\260\320\274 \320\275\320\260\320\264\320\276",
			"\320\277\321\203\321\201\321\202\321\214"});
		known.new_things = word_set_of({"new", "nda", "msa", "sow", "memo", "memorandum", "letter", "draft",
			"clause", "section", "amendment", "redline", "version", "agreement", "contract", "lease", "deed",
			"schedule", "exhibit", "annex", "template", "register", "tracker", "spreadsheet", "workbook", "table",
			"notice", "email", "document", "file"});
		known.russian_states = word_set_of({"\320\261\321\213\320\273", "\320\261\321\213\320\273\320\260", "\320\261\321\213\320\273\320\276", "\320\261\321\213\320\273\320\270", "\321\201\321\202\320\260\320\273", "\321\201\321\202\320\260\320\273\320\260", "\321\201\321\202\320\260\320\273\320\276", "\321\201\321\202\320\260\320\273\320\270",
			"\321\201\320\276\321\201\321\202\320\260\320\262\320\273\321\217\320\273", "\321\201\320\276\321\201\321\202\320\260\320\262\320\273\321\217\320\273\320\260", "\321\201\320\276\321\201\321\202\320\260\320\262\320\273\321\217\320\273\320\276", "\321\201\320\276\321\201\321\202\320\260\320\262\320\273\321\217\320\273\320\270"});
		known.obligations = phrases_of({"needs to be", "need to be", "has to be", "have to be", "should be",
			"ought to be", "should read", "should say", "needs to read", "needs to say", "\320\264\320\276\320\273\320\266\320\265\320\275 \320\261\321\213\321\202\321\214",
			"\320\264\320\276\320\273\320\266\320\275\320\260 \320\261\321\213\321\202\321\214", "\320\264\320\276\320\273\320\266\320\275\320\276 \320\261\321\213\321\202\321\214", "\320\264\320\276\320\273\320\266\320\275\321\213 \320\261\321\213\321\202\321\214", "\320\264\320\276\320\273\320\266\320\265\320\275 \321\201\320\276\321\201\321\202\320\260\320\262\320\273\321\217\321\202\321\214", "\320\264\320\276\320\273\320\266\320\275\320\260 \321\201\320\276\321\201\321\202\320\260\320\262\320\273\321\217\321\202\321\214",
			"\320\264\320\276\320\273\320\266\320\275\320\276 \321\201\320\276\321\201\321\202\320\260\320\262\320\273\321\217\321\202\321\214", "\320\264\320\276\320\273\320\266\320\275\321\213 \321\201\320\276\321\201\321\202\320\260\320\262\320\273\321\217\321\202\321\214"});
		known.information_leads = phrases_of({"tell", "explain", "describe", "summarise", "summarize", "outline", "list",
			"identify", "confirm", "check", "review", "analyse", "analyze", "assess", "evaluate", "compare", "find",
			"advise", "show", "look", "see", "read", "go through", "walk me through", "give me", "let me know",
			"i'd like to know", "i would like to know", "i want to know", "i need to know", "i wonder", "i'm wondering",
			"i was wondering", "i'd like to understand", "i want to understand", "what", "what's", "which", "who",
			"who's", "when", "where", "where's", "why", "how", "how's", "whether",
			"\321\201\320\272\320\260\320\266\320\270", "\321\201\320\272\320\260\320\266\320\270\321\202\320\265", "\320\277\320\276\320\264\321\201\320\272\320\260\320\266\320\270", "\320\277\320\276\320\264\321\201\320\272\320\260\320\266\320\270\321\202\320\265", "\321\200\320\260\321\201\321\201\320\272\320\260\320\266\320\270", "\321\200\320\260\321\201\321\201\320\272\320\260\320\266\320\270\321\202\320\265", "\320\276\320\261\321\212\321\217\321\201\320\275\320\270", "\320\276\320\261\321\212\321\217\321\201\320\275\320\270\321\202\320\265", "\320\276\320\277\320\270\321\210\320\270",
			"\320\276\320\277\320\270\321\210\320\270\321\202\320\265", "\320\277\320\265\321\200\320\265\321\207\320\270\321\201\320\273\320\270", "\320\277\320\265\321\200\320\265\321\207\320\270\321\201\320\273\320\270\321\202\320\265", "\320\277\321\200\320\276\320\262\320\265\321\200\321\214", "\320\277\321\200\320\276\320\262\320\265\321\200\321\214\321\202\320\265", "\320\277\320\276\321\201\320\274\320\276\321\202\321\200\320\270", "\320\277\320\276\321\201\320\274\320\276\321\202\321\200\320\270\321\202\320\265", "\320\276\321\206\320\265\320\275\320\270",
			"\320\276\321\206\320\265\320\275\320\270\321\202\320\265", "\321\201\321\200\320\260\320\262\320\275\320\270", "\321\201\321\200\320\260\320\262\320\275\320\270\321\202\320\265", "\320\275\320\260\320\271\320\264\320\270", "\320\275\320\260\320\271\320\264\320\270\321\202\320\265", "\321\203\320\267\320\275\320\260\320\271", "\321\203\320\267\320\275\320\260\320\271\321\202\320\265", "\320\262\321\213\321\217\321\201\320\275\320\270", "\320\262\321\213\321\217\321\201\320\275\320\270\321\202\320\265",
			"\320\277\321\200\320\276\320\260\320\275\320\260\320\273\320\270\320\267\320\270\321\200\321\203\320\271", "\320\277\321\200\320\276\320\260\320\275\320\260\320\273\320\270\320\267\320\270\321\200\321\203\320\271\321\202\320\265", "\320\270\320\267\321\203\321\207\320\270", "\320\270\320\267\321\203\321\207\320\270\321\202\320\265", "\320\277\321\200\320\276\321\207\320\270\321\202\320\260\320\271", "\320\277\321\200\320\276\321\207\320\270\321\202\320\260\320\271\321\202\320\265", "\320\277\320\276\320\272\320\260\320\266\320\270", "\320\277\320\276\320\272\320\260\320\266\320\270\321\202\320\265",
			"\320\264\320\260\320\271 \320\276\321\206\320\265\320\275\320\272\321\203", "\320\264\320\260\320\271\321\202\320\265 \320\276\321\206\320\265\320\275\320\272\321\203", "\321\205\320\276\321\207\321\203 \320\267\320\275\320\260\321\202\321\214", "\321\205\320\276\321\207\321\203 \320\277\320\276\320\275\321\217\321\202\321\214", "\320\270\320\275\321\202\320\265\321\200\320\265\321\201\320\275\320\276", "\321\207\321\202\320\276", "\320\272\320\260\320\272\320\276\320\271", "\320\272\320\260\320\272\320\260\321\217", "\320\272\320\260\320\272\320\276\320\265",
			"\320\272\320\260\320\272\320\270\320\265", "\320\272\320\260\320\272\320\276\320\262", "\320\272\320\260\320\272\320\276\320\262\320\260", "\320\272\320\260\320\272\320\276\320\262\320\276", "\320\272\321\202\320\276", "\320\263\320\264\320\265", "\320\272\320\276\320\263\320\264\320\260", "\320\277\320\276\321\207\320\265\320\274\321\203", "\320\267\320\260\321\207\320\265\320\274", "\320\272\320\260\320\272", "\321\201\320\272\320\276\320\273\321\214\320\272\320\276"});
		known.affirmations = word_set_of({"yes", "yeah", "yep", "yup", "ok", "okay", "sure", "agreed", "approved",
			"confirmed", "fine", "perfect", "absolutely", "\320\264\320\260", "\320\264\320\260\320\262\320\260\320\271", "\320\264\320\260\320\262\320\260\320\271\321\202\320\265", "\320\276\320\272", "\320\276\320\272\320\265\320\271", "\321\205\320\276\321\200\320\276\321\210\320\276", "\321\201\320\276\320\263\320\273\320\260\321\201\320\265\320\275",
			"\321\201\320\276\320\263\320\273\320\260\321\201\320\275\320\260", "\321\201\320\276\320\263\320\273\320\260\321\201\320\275\321\213", "\320\272\320\276\320\275\320\265\321\207\320\275\320\276", "\320\262\320\277\320\265\321\200\320\265\320\264", "\320\277\320\276\320\264\321\202\320\262\320\265\321\200\320\266\320\264\320\260\321\216", "\320\276\320\264\320\276\320\261\321\200\321\217\321\216"});
		// Forms made from each verb as English writes them: "add" gives "adds",
		// "added", "adding"; "update" "updating"; "modify" "modified"; "cap"
		// "capped". A form English never writes is harmless: nobody types it.
		for (const char *verb : {"add", "adjust", "align", "amend", "append", "apply", "attach", "bold", "broaden",
				"build", "cap", "change", "clarify", "clean", "complete", "compose", "conform", "convert", "copy",
				"correct", "create", "cross", "cut", "define", "delete", "do", "draft", "drop", "edit", "enter",
				"expand", "extend", "fill", "finalise", "finalize", "fix", "flag", "format", "generate", "harmonise",
				"harmonize", "highlight", "implement", "include", "incorporate", "increase", "indent", "input",
				"insert", "italicise", "italicize", "keep", "lengthen", "limit", "log", "lower", "make", "mark",
				"merge", "modify", "move", "narrow", "number", "paste", "populate", "prepare", "proceed", "produce",
				"put", "raise", "record", "redline", "redo", "redraft", "reduce", "reformat", "reinstate", "remove",
				"rename", "renumber", "reorder", "rephrase", "replace", "restore", "restructure", "reverse", "revert",
				"revise", "rework", "reword", "rewrite", "save", "set", "shorten", "simplify", "soften", "split",
				"standardise", "standardize", "strengthen", "strike", "substitute", "supplement", "swap", "tidy",
				"tighten", "track", "translate", "trim", "type", "underline", "undo", "update", "weaken", "widen",
				"write", "sort", "bump", "turn", "switch", "tweak", "shift", "push", "lift", "delay", "fold",
				"consolidate", "combine", "tone", "firm", "beef"}) {
			const std::string base = verb;
			const char last = base.back();
			for (const std::string &form : {base, base + "s", base + "es", base + "d", base + "ed", base + "ing",
					base + last + "ed", base + last + "ing"})
				known.english_verbs.insert(form);
			if (last == 'e') known.english_verbs.insert(base.substr(0, base.size() - 1) + "ing");
			if (last == 'y') {
				known.english_verbs.insert(base.substr(0, base.size() - 1) + "ies");
				known.english_verbs.insert(base.substr(0, base.size() - 1) + "ied");
			}
		}
		for (const std::string &form : word_set_of({"made", "did", "done", "wrote", "written", "rewrote", "rewritten",
				"redone", "undone", "struck", "stricken", "kept"}))
			known.english_verbs.insert(form);
		known.english_verb_predecessors = word_set_of({"to", "please", "kindly", "just", "also", "then", "and", "now",
			"let's", "you", "we", "i'll", "we'll", "should", "must", "can", "could", "will", "would", "shall", "be",
			"been", "being", "get", "gets", "got", "mind", "simply", "quickly", "go", "or", "first"});
		for (const char *verb : {"\320\262\320\275\320\265\321\201\320\270", "\320\262\320\275\320\276\321\201\320\270", "\320\270\320\267\320\274\320\265\320\275\320\270", "\320\270\320\267\320\274\320\265\320\275\321\217\320\271", "\320\277\320\276\320\274\320\265\320\275\321\217\320\271", "\320\274\320\265\320\275\321\217\320\271", "\320\270\321\201\320\277\321\200\320\260\320\262\321\214", "\320\270\321\201\320\277\321\200\320\260\320\262\320\273\321\217\320\271",
				"\320\277\320\276\320\277\321\200\320\260\320\262\321\214", "\320\277\320\276\320\264\320\277\321\200\320\260\320\262\321\214", "\320\264\320\276\320\261\320\260\320\262\321\214", "\320\264\320\276\320\261\320\260\320\262\320\273\321\217\320\271", "\321\203\320\264\320\260\320\273\320\270", "\321\203\320\264\320\260\320\273\321\217\320\271", "\321\203\320\261\320\265\321\200\320\270", "\320\267\320\260\320\274\320\265\320\275\320\270", "\320\267\320\260\320\274\320\265\320\275\321\217\320\271", "\320\262\320\265\321\200\320\275\320\270",
				"\320\262\320\276\321\201\321\201\321\202\320\260\320\275\320\276\320\262\320\270", "\320\276\320\261\320\275\320\276\320\262\320\270", "\320\264\320\276\320\277\320\276\320\273\320\275\320\270", "\320\277\320\265\321\200\320\265\320\277\320\270\321\210\320\270", "\320\277\320\265\321\200\320\265\321\204\320\276\321\200\320\274\321\203\320\273\320\270\321\200\321\203\320\271", "\320\276\321\202\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\321\203\320\271", "\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\321\203\320\271",
				"\320\277\320\276\320\264\320\263\320\276\321\202\320\276\320\262\321\214", "\321\201\320\276\321\201\321\202\320\260\320\262\321\214", "\320\275\320\260\320\277\320\270\321\210\320\270", "\320\277\320\270\321\210\320\270", "\321\201\320\276\320\267\320\264\320\260\320\271", "\321\201\320\264\320\265\320\273\320\260\320\271", "\320\264\320\265\320\273\320\260\320\271", "\320\276\321\204\320\276\321\200\320\274\320\270", "\320\267\320\260\320\277\320\276\320\273\320\275\320\270", "\320\267\320\260\320\277\320\270\321\210\320\270",
				"\320\262\320\277\320\270\321\210\320\270", "\320\262\321\201\321\202\320\260\320\262\321\214", "\321\201\320\276\320\272\321\200\320\260\321\202\320\270", "\321\203\320\262\320\265\320\273\320\270\321\207\321\214", "\321\203\320\274\320\265\320\275\321\214\321\210\320\270", "\321\201\320\275\320\270\320\267\321\214", "\320\277\320\276\320\262\321\213\321\201\321\214", "\320\277\321\200\320\276\320\264\320\273\320\270", "\320\276\320\263\321\200\320\260\320\275\320\270\321\207\321\214", "\321\203\321\201\320\270\320\273\321\214",
				"\321\201\320\274\321\217\320\263\321\207\320\270", "\320\262\321\213\320\264\320\265\320\273\320\270", "\320\276\321\202\320\274\320\265\321\202\321\214", "\320\277\320\276\320\274\320\265\321\202\321\214", "\320\277\320\265\321\200\320\265\320\275\320\265\321\201\320\270", "\320\277\320\265\321\200\320\265\320\270\320\274\320\265\320\275\321\203\320\271", "\320\277\321\200\320\276\320\275\321\203\320\274\320\265\321\200\321\203\320\271", "\320\277\320\265\321\200\320\265\320\275\321\203\320\274\320\265\321\200\321\203\320\271",
				"\320\277\321\200\320\270\320\274\320\265\320\275\320\270", "\321\201\320\276\321\205\321\200\320\260\320\275\320\270", "\320\267\320\260\321\204\320\270\320\272\321\201\320\270\321\200\321\203\320\271", "\320\277\321\200\320\270\320\262\320\265\320\264\320\270", "\320\270\321\201\320\272\320\273\321\216\321\207\320\270", "\320\262\320\272\320\273\321\216\321\207\320\270", "\320\264\320\276\321\200\320\260\320\261\320\276\321\202\320\260\320\271", "\321\201\320\272\320\276\321\200\321\200\320\265\320\272\321\202\320\270\321\200\321\203\320\271",
				"\320\276\321\202\320\272\320\276\321\200\321\200\320\265\320\272\321\202\320\270\321\200\321\203\320\271", "\320\277\320\265\321\200\320\265\320\264\320\265\320\273\320\260\320\271", "\320\277\320\265\321\200\320\265\320\262\320\265\320\264\320\270", "\320\275\320\260\320\261\321\200\320\276\321\201\320\260\320\271", "\321\200\320\260\321\201\321\210\320\270\321\200\321\214", "\321\201\321\203\320\267\321\214", "\320\262\321\213\321\207\320\265\321\200\320\272\320\275\320\270", "\320\277\321\200\320\276\321\201\321\202\320\260\320\262\321\214",
				"\320\277\320\276\321\201\321\202\320\260\320\262\321\214", "\321\203\321\201\321\202\320\260\320\275\320\276\320\262\320\270", "\320\264\320\265\320\271\321\201\321\202\320\262\321\203\320\271"}) {
			for (const std::string &form : words_of(verb)) {
				known.russian_verbs.insert(form);
				known.russian_verbs.insert(form + "\321\202\320\265");
			}
		}
		for (const std::string &form : word_set_of({"\320\262\320\275\320\265\321\201\321\202\320\270", "\320\262\320\275\320\276\321\201\320\270\321\202\321\214", "\320\270\320\267\320\274\320\265\320\275\320\270\321\202\321\214", "\320\270\320\267\320\274\320\265\320\275\321\217\321\202\321\214", "\320\277\320\276\320\274\320\265\320\275\321\217\321\202\321\214", "\320\274\320\265\320\275\321\217\321\202\321\214",
				"\320\270\321\201\320\277\321\200\320\260\320\262\320\270\321\202\321\214", "\320\270\321\201\320\277\321\200\320\260\320\262\320\273\321\217\321\202\321\214", "\320\277\320\276\320\277\321\200\320\260\320\262\320\270\321\202\321\214", "\320\277\320\276\320\264\320\277\321\200\320\260\320\262\320\270\321\202\321\214", "\320\264\320\276\320\261\320\260\320\262\320\270\321\202\321\214", "\320\264\320\276\320\261\320\260\320\262\320\273\321\217\321\202\321\214", "\321\203\320\264\320\260\320\273\320\270\321\202\321\214", "\321\203\320\264\320\260\320\273\321\217\321\202\321\214",
				"\321\203\320\261\321\200\320\260\321\202\321\214", "\320\267\320\260\320\274\320\265\320\275\320\270\321\202\321\214", "\320\267\320\260\320\274\320\265\320\275\321\217\321\202\321\214", "\320\262\320\265\321\200\320\275\321\203\321\202\321\214", "\320\262\320\276\321\201\321\201\321\202\320\260\320\275\320\276\320\262\320\270\321\202\321\214", "\320\276\320\261\320\275\320\276\320\262\320\270\321\202\321\214", "\320\264\320\276\320\277\320\276\320\273\320\275\320\270\321\202\321\214", "\320\277\320\265\321\200\320\265\320\277\320\270\321\201\320\260\321\202\321\214",
				"\320\277\320\265\321\200\320\265\321\204\320\276\321\200\320\274\321\203\320\273\320\270\321\200\320\276\320\262\320\260\321\202\321\214", "\320\276\321\202\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\320\276\320\262\320\260\321\202\321\214", "\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\320\276\320\262\320\260\321\202\321\214", "\320\277\320\276\320\264\320\263\320\276\321\202\320\276\320\262\320\270\321\202\321\214", "\321\201\320\276\321\201\321\202\320\260\320\262\320\270\321\202\321\214", "\320\275\320\260\320\277\320\270\321\201\320\260\321\202\321\214",
				"\321\201\320\276\320\267\320\264\320\260\321\202\321\214", "\321\201\320\264\320\265\320\273\320\260\321\202\321\214", "\320\276\321\204\320\276\321\200\320\274\320\270\321\202\321\214", "\320\267\320\260\320\277\320\276\320\273\320\275\320\270\321\202\321\214", "\320\267\320\260\320\277\320\270\321\201\320\260\321\202\321\214", "\320\262\320\277\320\270\321\201\320\260\321\202\321\214", "\320\262\321\201\321\202\320\260\320\262\320\270\321\202\321\214", "\321\201\320\276\320\272\321\200\320\260\321\202\320\270\321\202\321\214",
				"\321\203\320\262\320\265\320\273\320\270\321\207\320\270\321\202\321\214", "\321\203\320\274\320\265\320\275\321\214\321\210\320\270\321\202\321\214", "\321\201\320\275\320\270\320\267\320\270\321\202\321\214", "\320\277\320\276\320\262\321\213\321\201\320\270\321\202\321\214", "\320\277\321\200\320\276\320\264\320\273\320\270\321\202\321\214", "\320\276\320\263\321\200\320\260\320\275\320\270\321\207\320\270\321\202\321\214", "\321\203\321\201\320\270\320\273\320\270\321\202\321\214", "\321\201\320\274\321\217\320\263\321\207\320\270\321\202\321\214",
				"\320\262\321\213\320\264\320\265\320\273\320\270\321\202\321\214", "\320\276\321\202\320\274\320\265\321\202\320\270\321\202\321\214", "\320\277\320\276\320\274\320\265\321\202\320\270\321\202\321\214", "\320\277\320\265\321\200\320\265\320\275\320\265\321\201\321\202\320\270", "\320\277\320\265\321\200\320\265\320\270\320\274\320\265\320\275\320\276\320\262\320\260\321\202\321\214", "\320\277\321\200\320\276\320\275\321\203\320\274\320\265\321\200\320\276\320\262\320\260\321\202\321\214", "\320\277\321\200\320\270\320\274\320\265\320\275\320\270\321\202\321\214",
				"\321\201\320\276\321\205\321\200\320\260\320\275\320\270\321\202\321\214", "\320\267\320\260\321\204\320\270\320\272\321\201\320\270\321\200\320\276\320\262\320\260\321\202\321\214", "\320\277\321\200\320\270\320\262\320\265\321\201\321\202\320\270", "\320\270\321\201\320\272\320\273\321\216\321\207\320\270\321\202\321\214", "\320\262\320\272\320\273\321\216\321\207\320\270\321\202\321\214", "\320\264\320\276\321\200\320\260\320\261\320\276\321\202\320\260\321\202\321\214", "\321\201\320\272\320\276\321\200\321\200\320\265\320\272\321\202\320\270\321\200\320\276\320\262\320\260\321\202\321\214",
				"\320\277\320\265\321\200\320\265\320\264\320\265\320\273\320\260\321\202\321\214", "\320\277\320\265\321\200\320\265\320\262\320\265\321\201\321\202\320\270", "\320\262\321\213\321\207\320\265\321\200\320\272\320\275\321\203\321\202\321\214", "\320\277\321\200\320\276\321\201\321\202\320\260\320\262\320\270\321\202\321\214", "\320\277\320\276\321\201\321\202\320\260\320\262\320\270\321\202\321\214", "\321\203\321\201\321\202\320\260\320\275\320\276\320\262\320\270\321\202\321\214", "\320\262\320\275\320\265\321\201\320\265\320\274", "\320\262\320\275\320\265\321\201\321\221\320\274",
				"\320\270\320\267\320\274\320\265\320\275\320\270\320\274", "\320\277\320\276\320\274\320\265\320\275\321\217\320\265\320\274", "\320\270\321\201\320\277\321\200\320\260\320\262\320\270\320\274", "\320\277\320\276\320\277\321\200\320\260\320\262\320\270\320\274", "\320\264\320\276\320\261\320\260\320\262\320\270\320\274", "\321\203\320\264\320\260\320\273\320\270\320\274", "\321\203\320\261\320\265\321\200\320\265\320\274", "\320\267\320\260\320\274\320\265\320\275\320\270\320\274", "\320\262\320\265\321\200\320\275\320\265\320\274",
				"\320\276\320\261\320\275\320\276\320\262\320\270\320\274", "\320\277\320\265\321\200\320\265\320\277\320\270\321\210\320\265\320\274", "\320\277\320\276\320\264\320\263\320\276\321\202\320\276\320\262\320\270\320\274", "\321\201\320\276\321\201\321\202\320\260\320\262\320\270\320\274", "\320\275\320\260\320\277\320\270\321\210\320\265\320\274", "\321\201\320\276\320\267\320\264\320\260\320\264\320\270\320\274", "\321\201\320\264\320\265\320\273\320\260\320\265\320\274", "\320\267\320\260\320\277\320\276\320\273\320\275\320\270\320\274",
				"\320\267\320\260\320\277\320\270\321\210\320\265\320\274", "\320\262\321\201\321\202\320\260\320\262\320\270\320\274", "\321\201\320\276\320\272\321\200\320\260\321\202\320\270\320\274", "\320\277\321\200\320\270\320\274\320\265\320\275\320\270\320\274", "\321\201\320\276\321\205\321\200\320\260\320\275\320\270\320\274"}))
			known.russian_verbs.insert(form);
		known.negations = word_set_of({"don't", "do not", "not", "never", "no", "cannot", "can't", "won't", "shouldn't",
			"needn't", "doesn't", "\320\275\320\265", "\320\275\320\270"});
		known.coordinators = word_set_of({"and", "then", "also", "\320\270", "\320\267\320\260\321\202\320\265\320\274", "\320\277\320\276\321\202\320\276\320\274", "\320\260", "\321\202\320\260\320\272\320\266\320\265"});
		return known;
	}();
	return words;
}

// True when the word at `at` is a verb of work on a document that the sentence
// asks for - not negated ("don't change", "…"), and, in English, where a
// request puts a verb: at the start of a clause, or after "to", "please",
// "you", "should" and the like - never "the change" or "a draft". Where the
// sentence states a want (`isWanted`: "I'd like clause 9 updated…"), the verb
// may stand anywhere in it.
bool asks_with_verb(const std::vector<std::string> &sentence, std::size_t at, std::size_t start, bool isWanted) {
	const RequestWords &known = request_words();
	const std::string &word = sentence[at];
	const auto negated = [&] {
		if (at > start && known.negations.count(sentence[at - 1]) > 0) return true;
		return at > start + 1 && sentence[at - 1] == "to" && known.negations.count(sentence[at - 2]) > 0;
	};
	if (known.russian_verbs.count(word) > 0) return !negated();
	// "Go ahead." "Go with option B."
	const bool goAhead = (word == "go" || word == "goes") && at + 1 < sentence.size()
		&& (sentence[at + 1] == "ahead" || sentence[at + 1] == "with");
	if (!goAhead && known.english_verbs.count(word) == 0) return false;
	if (negated()) return false;
	if (isWanted || at == start || sentence[at - 1] == kClauseBreak) return true;
	return known.english_verb_predecessors.count(sentence[at - 1]) > 0;
}

// True when one of `phrases` begins anywhere in the sentence from `from`.
bool has_phrase(const std::vector<std::string> &sentence, std::size_t from,
		const std::vector<std::vector<std::string>> &phrases) {
	for (std::size_t at = from; at < sentence.size(); ++at)
		if (phrase_at(sentence, at, phrases) > 0) return true;
	return false;
}

// True for a sentence whose words ask for something to be done to a document:
// "Restore the cap.", "Could you restore the cap?", "Do you mind updating
// clause 5?", "Can clause 9 be tightened?", "I'd like clause 9 updated to a
// 12-month cap.", "The governing law needs to be New York.", "Yes, do it.",
// Never for one that asks for information, whatever words it borrows: a
// question ("What's the cap?", "Should we restore it?", "…
// …?"), a question whether something may or should be done ("Can we delete
// clause 14 without breaching the SPA?", "Would you change anything in clause
// 9?", "… 14?"), a request to be told ("Can you tell me
// what the cap is?", "Review the NDA and tell me whether the cap is market.",
// "…"), or a statement ("The notice period is 30
// days."). A request for information asks for a change as well only where one
// is added to it: "Review the NDA and update the cap."
bool asks_for_change(const std::vector<std::string> &sentence, bool isQuestion) {
	const RequestWords &known = request_words();
	std::size_t start = 0;
	bool isFramed = false;
	bool isImpersonal = false;
	bool isWanted = false;
	bool isOpening = true;
	for (;;) {
		while (start < sentence.size() && sentence[start] == kClauseBreak) ++start;
		if (start >= sentence.size()) return false;
		if (known.leads.count(sentence[start]) > 0) {
			++start;
			continue;
		}
		if (isOpening) {
			isOpening = false;
			if (phrase_at(sentence, start, known.question_openers) > 0) return false;
			// A question in English word order is one whether or not it ends in
			// "?": "Can we delete clause 14 without breaching the SPA".
			const std::string &first = sentence[start];
			const std::string next = start + 1 < sentence.size() ? sentence[start + 1] : std::string();
			static const std::set<std::string> persons = word_set_of({"you", "we", "i", "they", "it", "he", "she"});
			if ((known.auxiliaries.count(first) > 0 && known.pronouns.count(next) > 0)
					|| (known.pronoun_auxiliaries.count(first) > 0 && persons.count(next) > 0))
				isQuestion = true;
			// "Can clause 9 be changed to 12 months?" asks for it, as "Can you
			// change it?" does; "Can we change it?" asks whether they may.
			if (isQuestion && (first == "can" || first == "could") && next != "we" && next != "i" && next != "you"
					&& std::find(sentence.begin() + static_cast<std::ptrdiff_t>(start), sentence.end(), "be")
						!= sentence.end())
				isFramed = true;
		}
		if (const std::size_t frame = phrase_at(sentence, start, known.frames)) {
			start += frame;
			isFramed = true;
			continue;
		}
		if (const std::size_t frame = phrase_at(sentence, start, known.impersonal_frames)) {
			start += frame;
			isFramed = true;
			isImpersonal = true;
			continue;
		}
		if (!isQuestion && !isWanted && phrase_at(sentence, start, known.information_leads) == 0) {
			if (const std::size_t desire = phrase_at(sentence, start, known.desires)) {
				start += desire;
				isWanted = true;
				continue;
			}
		}
		if (isWanted && sentence[start] == "\321\207\321\202\320\276\320\261\321\213") {
			++start;
			continue;
		}
		break;
	}
	// A question asks for a change only as a request put politely, and never
	// when it asks whether the change may or should be made.
	if (isQuestion) {
		if (!isFramed || has_phrase(sentence, start, known.advice_words)) return false;
		if (isImpersonal && std::find(sentence.begin() + static_cast<std::ptrdiff_t>(start), sentence.end(), "under")
				!= sentence.end())
			return false;
	}
	std::size_t words = 0;
	for (std::size_t at = start; at < sentence.size(); ++at)
		if (sentence[at] != kClauseBreak) ++words;
	if (!isQuestion && known.affirmations.count(sentence[start]) > 0 && words <= 4) return true;
	const bool asksToBeTold = phrase_at(sentence, start, known.information_leads) > 0;
	if (isWanted && !asksToBeTold) {
		// "I want a new NDA for Acme."
		std::size_t named = start;
		if (named < sentence.size() && (sentence[named] == "a" || sentence[named] == "an")) ++named;
		if (named < sentence.size() && known.new_things.count(sentence[named]) > 0) return true;
		for (std::size_t at = start; at < sentence.size(); ++at)
			if (known.russian_states.count(sentence[at]) > 0) return true;
	}
	for (std::size_t at = start; at < sentence.size(); ++at) {
		if (sentence[at] == kClauseBreak || !asks_with_verb(sentence, at, start, isWanted && !asksToBeTold)) continue;
		if (!asksToBeTold) return true;
		if (at > start && (sentence[at - 1] == kClauseBreak || known.coordinators.count(sentence[at - 1]) > 0)) return true;
	}
	// What the document must say, stated as such.
	if (!isQuestion && !asksToBeTold)
		for (std::size_t at = start; at < sentence.size(); ++at)
			if (phrase_at(sentence, at, known.obligations) > 0 && (at == 0 || known.negations.count(sentence[at - 1]) == 0))
				return true;
	return false;
}

bool starts_with_words(const std::vector<std::string> &words, std::size_t at, const std::vector<std::string> &part) {
	if (part.empty() || at + part.size() > words.size()) return false;
	return std::equal(part.begin(), part.end(), words.begin() + static_cast<std::ptrdiff_t>(at));
}

// One phrasing of "show me first", as a sequence of steps matched within one
// sentence. Written as text (phrase_from()), so the list reads as the words it
// catches:
//   word or {one|two words}  one of these, word for word ("$" among them: the
//                            clause ends here)
//   ?{...}                   the same, or nothing
//   *                        up to four words of the same clause
//   **                       up to eight words of the same sentence, commas
//                            included ("…")
//   $                        the clause ends here (a comma, or the sentence)
//   ^                        the clause begins here
//   !{...}                   not followed by one of these (past a comma)
//   <!{...}                  not preceded by one of these
//   @NAME                    one of a named set of words (kWordSets)
struct PhraseStep {
	enum class Kind { Words, Gap, ClauseEnd, ClauseStart, NotNext, NotAfter };
	struct Choice {
		std::vector<std::string> words;
		bool is_clause_end = false;
	};
	Kind kind = Kind::Words;
	std::vector<Choice> choices;
	bool is_optional = false;
	bool crosses_clauses = false;
};
using Phrase = std::vector<PhraseStep>;

constexpr std::size_t kLongestGap = 4;
constexpr std::size_t kLongestSentenceGap = 8;

bool is_clause_end(const std::vector<std::string> &words, std::size_t at) {
	return at == words.size() || words[at] == kClauseBreak;
}

bool follows(const Phrase &phrase, std::size_t step, const std::vector<std::string> &words, std::size_t at) {
	if (step == phrase.size()) return true;
	const PhraseStep &current = phrase[step];
	switch (current.kind) {
		case PhraseStep::Kind::Gap: {
			const std::size_t longest = current.crosses_clauses ? kLongestSentenceGap : kLongestGap;
			for (std::size_t skipped = 0; skipped <= longest && at + skipped <= words.size(); ++skipped) {
				if (!current.crosses_clauses && skipped > 0 && words[at + skipped - 1] == kClauseBreak) return false;
				if (follows(phrase, step + 1, words, at + skipped)) return true;
			}
			return false;
		}
		case PhraseStep::Kind::ClauseEnd:
			return is_clause_end(words, at) && follows(phrase, step + 1, words, at);
		case PhraseStep::Kind::ClauseStart:
			return (at == 0 || words[at - 1] == kClauseBreak) && follows(phrase, step + 1, words, at);
		case PhraseStep::Kind::NotNext: {
			const std::size_t next = at < words.size() && words[at] == kClauseBreak ? at + 1 : at;
			for (const PhraseStep::Choice &choice : current.choices)
				if (starts_with_words(words, next, choice.words)) return false;
			return follows(phrase, step + 1, words, at);
		}
		case PhraseStep::Kind::NotAfter:
			for (const PhraseStep::Choice &choice : current.choices)
				if (choice.words.size() <= at && starts_with_words(words, at - choice.words.size(), choice.words))
					return false;
			return follows(phrase, step + 1, words, at);
		case PhraseStep::Kind::Words:
			break;
	}
	for (const PhraseStep::Choice &choice : current.choices) {
		if (choice.is_clause_end) {
			if (is_clause_end(words, at) && follows(phrase, step + 1, words, at)) return true;
			continue;
		}
		if (starts_with_words(words, at, choice.words) && follows(phrase, step + 1, words, at + choice.words.size()))
			return true;
	}
	return current.is_optional && follows(phrase, step + 1, words, at);
}

// The named sets the phrasings share. Russian verbs are listed form by form,
// never as a stem: "…" is a pronoun as well as the start of "…".
const std::map<std::string, std::string> &word_sets() {
	static const std::map<std::string, std::string> sets{
		{"SHOW", "show|display|send me|send it|send them|send over|give me|give it to me|give them to me"},
		{"SEE", "see|review|check|read|look|look at|look over|preview|approve|inspect|vet|verify|go over|go through|"
			"sign off|decide|confirm|okay|ok"},
		{"CHANGE", "change|edit|modify|alter|amend|revise|update|touch|write|apply|save|commit|implement|insert|"
			"replace|redline|mark up|make|do|delete|remove|rewrite|put|add|fix|correct|go ahead|proceed|finalize|"
			"finalise"},
		{"CHANGING", "changing|editing|modifying|altering|amending|revising|updating|touching|writing|applying|saving|"
			"committing|implementing|inserting|replacing|redlining|marking up|making|deleting|removing|rewriting|"
			"putting|adding|fixing|correcting|doing|proceeding|going ahead|finalizing|finalising"},
		{"NEG", "don't|do not|never|not to|no need to|needn't|need not"},
		{"OBJECT", "it|them|this|that|anything|any of it|a thing|a word|the document|the documents|the file|the files|"
			"the original|the contract|the agreement|the draft|the text|the doc|my document|my file|any document|"
			"any documents|any file|any files"},
		{"AFTER", "$|yet|until|till|unless|before|for now|right now|now|without|just|only|please|i|we|let's"},
		// The document itself, as a lawyer names it: "don't modify the NDA" holds,
		// "don't change the numbering" only limits the change.
		{"DET", "the|this|that|these|those|my|our|any|either|its|their"},
		{"DOC", "nda|msa|sow|spa|apa|lease|contract|contracts|agreement|agreements|document|documents|file|files|"
			"original|originals|draft|drafts|text|doc|docs|workbook|workbooks|spreadsheet|spreadsheets|sheet|memo|"
			"letter|template|register|tracker|amendment|deed|redline|word file|word document|excel file|excel|"
			"project|folder"},
		{"EXCEPT", "else|other|besides|except|beyond|apart|outside|but|other than|save|aside"},
		{"SHOW_RU", "\320\277\320\276\320\272\320\260\320\266\320\270|\320\277\320\276\320\272\320\260\320\266\320\270\321\202\320\265|\320\277\320\276\320\272\320\260\320\267\320\260\321\202\321\214|\320\277\321\200\320\270\321\210\320\273\320\270|\320\277\321\200\320\270\321\210\320\273\320\270\321\202\320\265|\320\276\321\202\320\277\321\200\320\260\320\262\321\214|\320\276\321\202\320\277\321\200\320\260\320\262\321\214\321\202\320\265|\320\262\321\213\320\262\320\265\320\264\320\270|\320\262\321\213\320\262\320\265\320\264\320\270\321\202\320\265|\320\264\320\260\320\271 \320\277\320\276\321\201\320\274\320\276\321\202\321\200\320\265\321\202\321\214|"
			"\320\264\320\260\320\271\321\202\320\265 \320\277\320\276\321\201\320\274\320\276\321\202\321\200\320\265\321\202\321\214|\320\264\320\260\320\271 \320\274\320\275\320\265 \320\277\320\276\321\201\320\274\320\276\321\202\321\200\320\265\321\202\321\214|\320\264\320\260\320\271\321\202\320\265 \320\274\320\275\320\265 \320\277\320\276\321\201\320\274\320\276\321\202\321\200\320\265\321\202\321\214|\320\264\320\260\320\271 \320\262\320\267\320\263\320\273\321\217\320\275\321\203\321\202\321\214|\320\264\320\260\320\271\321\202\320\265 \320\262\320\267\320\263\320\273\321\217\320\275\321\203\321\202\321\214|\320\264\320\260\320\271 \320\263\320\273\321\217\320\275\321\203\321\202\321\214|"
			"\320\277\321\200\320\265\320\264\321\201\321\202\320\260\320\262\321\214|\320\277\321\200\320\265\320\264\321\201\321\202\320\260\320\262\321\214\321\202\320\265"},
		{"FIRST_RU", "\321\201\320\275\320\260\321\207\320\260\320\273\320\260|\321\201\320\277\320\265\321\200\320\262\320\260|\320\262\320\275\320\260\321\207\320\260\320\273\320\265|\320\264\320\273\321\217 \320\275\320\260\321\207\320\260\320\273\320\260|\320\277\321\200\320\265\320\266\320\264\320\265 \320\262\321\201\320\265\320\263\320\276|\320\277\321\200\320\265\320\266\320\264\320\265|\320\277\321\200\320\265\320\264\320\262\320\260\321\200\320\270\321\202\320\265\320\273\321\214\320\275\320\276|\320\267\320\260\321\200\320\260\320\275\320\265\320\265"},
		{"SEE_RU", "\320\277\320\276\321\201\320\274\320\276\321\202\321\200\320\265\321\202\321\214|\321\203\320\262\320\270\320\264\320\265\321\202\321\214|\320\277\321\200\320\276\320\262\320\265\321\200\320\270\321\202\321\214|\320\262\320\267\320\263\320\273\321\217\320\275\321\203\321\202\321\214|\320\263\320\273\321\217\320\275\321\203\321\202\321\214|\320\276\320\267\320\275\320\260\320\272\320\276\320\274\320\270\321\202\321\214\321\201\321\217|\320\270\320\267\321\203\321\207\320\270\321\202\321\214|\320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200\320\265\321\202\321\214|\321\201\320\276\320\263\320\273\320\260\321\201\320\276\320\262\320\260\321\202\321\214|"
			"\321\203\321\202\320\262\320\265\321\200\320\264\320\270\321\202\321\214|\320\276\320\264\320\276\320\261\321\200\320\270\321\202\321\214|\320\277\321\200\320\276\321\207\320\270\321\202\320\260\321\202\321\214|\320\277\321\200\320\276\321\207\320\265\321\201\321\202\321\214|\320\276\321\206\320\265\320\275\320\270\321\202\321\214"},
		{"SEEI_RU", "\320\277\320\276\321\201\320\274\320\276\321\202\321\200\321\216|\320\277\321\200\320\276\320\262\320\265\321\200\321\216|\320\263\320\273\321\217\320\275\321\203|\320\262\320\267\320\263\320\273\321\217\320\275\321\203|\320\276\320\267\320\275\320\260\320\272\320\276\320\274\320\273\321\216\321\201\321\214|\320\270\320\267\321\203\321\207\321\203|\320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200\321\216|\321\201\320\276\320\263\320\273\320\260\321\201\321\203\321\216|\321\203\321\202\320\262\320\265\321\200\320\266\321\203|\320\276\320\264\320\276\320\261\321\200\321\216|\320\277\321\200\320\276\321\207\320\270\321\202\320\260\321\216|"
			"\320\277\321\200\320\276\321\207\321\202\321\203|\320\276\321\206\320\265\320\275\321\216|\320\277\320\276\320\264\321\202\320\262\320\265\321\200\320\266\321\203|\321\203\320\262\320\270\320\266\321\203|\321\200\320\265\321\210\321\203"},
		{"CHANGE_RU", "\320\274\320\265\320\275\321\217\320\271|\320\274\320\265\320\275\321\217\320\271\321\202\320\265|\320\270\320\267\320\274\320\265\320\275\321\217\320\271|\320\270\320\267\320\274\320\265\320\275\321\217\320\271\321\202\320\265|\320\270\320\267\320\274\320\265\320\275\320\270|\320\270\320\267\320\274\320\265\320\275\320\270\321\202\320\265|\320\277\320\276\320\274\320\265\320\275\321\217\320\271|\320\277\320\276\320\274\320\265\320\275\321\217\320\271\321\202\320\265|\320\277\321\200\320\260\320\262\321\214|\320\277\321\200\320\260\320\262\321\214\321\202\320\265|\320\277\320\276\320\277\321\200\320\260\320\262\321\214|"
			"\320\277\320\276\320\277\321\200\320\260\320\262\321\214\321\202\320\265|\320\270\321\201\320\277\321\200\320\260\320\262\320\273\321\217\320\271|\320\270\321\201\320\277\321\200\320\260\320\262\320\273\321\217\320\271\321\202\320\265|\320\270\321\201\320\277\321\200\320\260\320\262\321\214|\320\270\321\201\320\277\321\200\320\260\320\262\321\214\321\202\320\265|\320\262\320\275\320\276\321\201\320\270|\320\262\320\275\320\276\321\201\320\270\321\202\320\265|\320\262\320\275\320\265\321\201\320\270|\320\262\320\275\320\265\321\201\320\270\321\202\320\265|\321\202\321\200\320\276\320\263\320\260\320\271|\321\202\321\200\320\276\320\263\320\260\320\271\321\202\320\265|\321\202\321\200\320\276\320\275\321\214|"
			"\321\202\321\200\320\276\320\275\321\214\321\202\320\265|\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\321\203\320\271|\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\321\203\320\271\321\202\320\265|\320\276\321\202\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\321\203\320\271|\320\276\321\202\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\321\203\320\271\321\202\320\265|\320\277\321\200\320\270\320\274\320\265\320\275\321\217\320\271|\320\277\321\200\320\270\320\274\320\265\320\275\321\217\320\271\321\202\320\265|\320\277\321\200\320\270\320\274\320\265\320\275\320\270|\320\277\321\200\320\270\320\274\320\265\320\275\320\270\321\202\320\265|"
			"\321\201\320\276\321\205\321\200\320\260\320\275\321\217\320\271|\321\201\320\276\321\205\321\200\320\260\320\275\321\217\320\271\321\202\320\265|\321\201\320\276\321\205\321\200\320\260\320\275\320\270|\321\201\320\276\321\205\321\200\320\260\320\275\320\270\321\202\320\265|\320\267\320\260\320\277\320\270\321\201\321\213\320\262\320\260\320\271|\320\267\320\260\320\277\320\270\321\201\321\213\320\262\320\260\320\271\321\202\320\265|\320\267\320\260\320\277\320\270\321\210\320\270|\320\267\320\260\320\277\320\270\321\210\320\270\321\202\320\265|\320\277\320\270\321\210\320\270|\320\277\320\270\321\210\320\270\321\202\320\265|\320\277\320\265\321\200\320\265\320\277\320\270\321\201\321\213\320\262\320\260\320\271|"
			"\320\277\320\265\321\200\320\265\320\277\320\270\321\201\321\213\320\262\320\260\320\271\321\202\320\265|\320\277\320\265\321\200\320\265\320\277\320\270\321\210\320\270|\320\277\320\265\321\200\320\265\320\277\320\270\321\210\320\270\321\202\320\265|\320\262\321\201\321\202\320\260\320\262\320\273\321\217\320\271|\320\262\321\201\321\202\320\260\320\262\320\273\321\217\320\271\321\202\320\265|\320\262\321\201\321\202\320\260\320\262\321\214|\320\262\321\201\321\202\320\260\320\262\321\214\321\202\320\265|\321\203\320\264\320\260\320\273\321\217\320\271|\321\203\320\264\320\260\320\273\321\217\320\271\321\202\320\265|\321\203\320\264\320\260\320\273\320\270|\321\203\320\264\320\260\320\273\320\270\321\202\320\265|"
			"\320\264\320\265\320\273\320\260\320\271|\320\264\320\265\320\273\320\260\320\271\321\202\320\265|\321\201\320\264\320\265\320\273\320\260\320\271|\321\201\320\264\320\265\320\273\320\260\320\271\321\202\320\265"},
		{"CHANGE_INF_RU", "\320\274\320\265\320\275\321\217\321\202\321\214|\320\270\320\267\320\274\320\265\320\275\321\217\321\202\321\214|\320\270\320\267\320\274\320\265\320\275\320\270\321\202\321\214|\320\277\320\276\320\274\320\265\320\275\321\217\321\202\321\214|\320\277\321\200\320\260\320\262\320\270\321\202\321\214|\320\277\320\276\320\277\321\200\320\260\320\262\320\273\321\217\321\202\321\214|\320\277\320\276\320\277\321\200\320\260\320\262\320\270\321\202\321\214|\320\270\321\201\320\277\321\200\320\260\320\262\320\273\321\217\321\202\321\214|\320\270\321\201\320\277\321\200\320\260\320\262\320\270\321\202\321\214|"
			"\320\262\320\275\320\276\321\201\320\270\321\202\321\214|\320\262\320\275\320\265\321\201\321\202\320\270|\321\202\321\200\320\276\320\263\320\260\321\202\321\214|\321\202\321\200\320\276\320\275\321\203\321\202\321\214|\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\320\276\320\262\320\260\321\202\321\214|\320\276\321\202\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\320\276\320\262\320\260\321\202\321\214|\320\277\321\200\320\270\320\274\320\265\320\275\321\217\321\202\321\214|\320\277\321\200\320\270\320\274\320\265\320\275\320\270\321\202\321\214|\321\201\320\276\321\205\321\200\320\260\320\275\321\217\321\202\321\214|\321\201\320\276\321\205\321\200\320\260\320\275\320\270\321\202\321\214|"
			"\320\267\320\260\320\277\320\270\321\201\321\213\320\262\320\260\321\202\321\214|\320\267\320\260\320\277\320\270\321\201\320\260\321\202\321\214|\320\277\320\265\321\200\320\265\320\277\320\270\321\201\321\213\320\262\320\260\321\202\321\214|\320\277\320\265\321\200\320\265\320\277\320\270\321\201\320\260\321\202\321\214|\320\262\321\201\321\202\320\260\320\262\320\273\321\217\321\202\321\214|\320\262\321\201\321\202\320\260\320\262\320\270\321\202\321\214|\321\203\320\264\320\260\320\273\321\217\321\202\321\214|\321\203\320\264\320\260\320\273\320\270\321\202\321\214|\320\264\320\265\320\273\320\260\321\202\321\214|\321\201\320\264\320\265\320\273\320\260\321\202\321\214"},
		{"CHANGING_RU", "\320\274\320\265\320\275\321\217\321\217|\320\270\320\267\320\274\320\265\320\275\321\217\321\217|\320\277\320\276\320\274\320\265\320\275\321\217\320\262|\320\277\321\200\320\260\320\262\321\217|\320\270\321\201\320\277\321\200\320\260\320\262\320\273\321\217\321\217|\320\262\320\275\320\276\321\201\321\217|\321\202\321\200\320\276\320\263\320\260\321\217|\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\321\203\321\217|\320\277\321\200\320\270\320\274\320\265\320\275\321\217\321\217|\321\201\320\276\321\205\321\200\320\260\320\275\321\217\321\217|\320\267\320\260\320\277\320\270\321\201\321\213\320\262\320\260\321\217|"
			"\320\277\320\265\321\200\320\265\320\277\320\270\321\201\321\213\320\262\320\260\321\217|\320\262\321\201\321\202\320\260\320\262\320\273\321\217\321\217|\321\203\320\264\320\260\320\273\321\217\321\217|\320\270\320\267\320\274\320\265\320\275\320\270\320\262|\320\262\320\275\320\265\321\201\321\217|\321\202\321\200\320\276\320\275\321\203\320\262"},
		{"AFTER_RU", "$|\320\277\320\276\320\272\320\260|\320\265\321\211\320\265|\321\201\320\265\320\271\321\207\320\260\321\201|\321\201\321\200\320\260\320\267\321\203|\320\264\320\276|\320\261\320\265\320\267|\320\275\320\270\321\207\320\265\320\263\320\276|\320\265\320\263\320\276|\320\265\320\265|\320\270\321\205|\320\264\320\276\320\272\321\203\320\274\320\265\320\275\321\202|\320\264\320\276\320\272\321\203\320\274\320\265\320\275\321\202\321\213|\321\204\320\260\320\271\320\273|\321\204\320\260\320\271\320\273\321\213|\320\264\320\276\320\263\320\276\320\262\320\276\321\200|\320\276\321\200\320\270\320\263\320\270\320\275\320\260\320\273|"
			"\321\202\320\265\320\272\321\201\321\202|\320\262 \320\264\320\276\320\272\321\203\320\274\320\265\320\275\321\202|\320\262 \320\264\320\276\320\272\321\203\320\274\320\265\320\275\321\202\320\265|\320\262 \321\204\320\260\320\271\320\273|\320\262 \321\204\320\260\320\271\320\273\320\265|\320\262 \320\264\320\276\320\263\320\276\320\262\320\276\321\200|\320\262 \320\264\320\276\320\263\320\276\320\262\320\276\321\200\320\265|\321\201\320\260\320\274|\321\201\320\260\320\274\320\260|\321\201\320\260\320\274\320\270|\320\277\320\276\320\266\320\260\320\273\321\203\320\271\321\201\321\202\320\260"},
		{"EXCEPT_RU", "\320\272\321\200\320\276\320\274\320\265|\320\277\320\276\320\274\320\270\320\274\320\276|\320\267\320\260 \320\270\321\201\320\272\320\273\321\216\321\207\320\265\320\275\320\270\320\265\320\274|\320\264\321\200\321\203\320\263\320\276\320\263\320\276|\320\264\321\200\321\203\320\263\320\276\320\265|\320\264\321\200\321\203\320\263\320\270\320\265|\320\261\320\276\320\273\321\214\321\210\320\265|\320\270\320\275\320\276\320\263\320\276|\320\270\320\275\320\276\320\265|\320\273\320\270\321\210\320\275\320\265\320\263\320\276|\320\273\320\270\321\210\320\275\320\265\320\265|\320\276\321\201\321\202\320\260\320\273\321\214\320\275\320\276\320\263\320\276|"
			"\320\276\321\201\321\202\320\260\320\273\321\214\320\275\320\276\320\265|\320\276\321\201\321\202\320\260\320\273\321\214\320\275\321\213\320\265|\320\277\321\200\320\276\321\207\320\265\320\263\320\276|\320\277\321\200\320\276\321\207\320\265\320\265"},
		{"MY_RU", "\320\274\320\276\320\265\320\263\320\276|\320\274\320\276\320\265\320\271|\320\274\320\276\320\270\321\205|\320\274\320\276\320\265|\320\274\320\276\321\216|\320\274\320\276\320\271|\320\275\320\260\321\210\320\265\320\263\320\276|\320\275\320\260\321\210\320\265\320\271"},
	};
	return sets;
}

Phrase phrase_from(const std::string &pattern) {
	Phrase phrase;
	std::size_t at = 0;
	while (at < pattern.size()) {
		if (pattern[at] == ' ') {
			++at;
			continue;
		}
		PhraseStep step;
		if (pattern.compare(at, 2, "<!") == 0) {
			step.kind = PhraseStep::Kind::NotAfter;
			at += 2;
		} else if (pattern[at] == '!') {
			step.kind = PhraseStep::Kind::NotNext;
			++at;
		} else if (pattern[at] == '?') {
			step.is_optional = true;
			++at;
		}
		std::string body;
		if (pattern[at] == '{') {
			const std::size_t close = pattern.find('}', at);
			body = pattern.substr(at + 1, close - at - 1);
			at = close + 1;
		} else {
			const std::size_t end = pattern.find(' ', at);
			body = pattern.substr(at, end == std::string::npos ? std::string::npos : end - at);
			at = end == std::string::npos ? pattern.size() : end;
		}
		if (body == "*" || body == "**") {
			step.kind = PhraseStep::Kind::Gap;
			step.crosses_clauses = body == "**";
		}
		else if (body == "$" && step.kind == PhraseStep::Kind::Words) step.kind = PhraseStep::Kind::ClauseEnd;
		else if (body == "^") step.kind = PhraseStep::Kind::ClauseStart;
		std::vector<std::string> choices;
		std::stringstream split(body);
		for (std::string choice; std::getline(split, choice, '|');) {
			if (choice.size() > 1 && choice[0] == '@') {
				std::stringstream named(word_sets().at(choice.substr(1)));
				for (std::string member; std::getline(named, member, '|');) choices.push_back(member);
			} else {
				choices.push_back(choice);
			}
		}
		if (step.kind == PhraseStep::Kind::Words || step.kind == PhraseStep::Kind::NotNext
				|| step.kind == PhraseStep::Kind::NotAfter)
			for (const std::string &choice : choices) {
				PhraseStep::Choice entry;
				if (choice == "$") entry.is_clause_end = true;
				else if (choice == ",") entry.words = {kClauseBreak};
				else entry.words = words_of(choice);
				if (entry.is_clause_end || !entry.words.empty()) step.choices.push_back(std::move(entry));
			}
		phrase.push_back(std::move(step));
	}
	return phrase;
}

// One phrasing of "show me before you change it". A soft one ("Propose a
// redline…", "…for my review", "just list…", "without changing the text")
// asks to see the change only when nothing else in its sentence asks for it to
// be made: "Propose a redline for clause 9 and apply it" asks for the change,
// as "Draft a new NDA for my review" asks for a new document to review, and
// "Renumber the clauses without changing the text" for a renumbering.
struct ShowFirstPhrase {
	Phrase phrase;
	// The name of what makes a soft phrasing an ordinary request, or empty.
	std::string unless;
};

// What makes a soft phrasing an ordinary request, by the name it is marked
// with: "~APPLY" - the sentence also asks for the change to be made; "~NEW" -
// it asks for a new document.
const std::vector<Phrase> &soft_phrase_exceptions(const std::string &name) {
	static const std::map<std::string, std::vector<Phrase>> exceptions = [] {
		std::map<std::string, std::vector<Phrase>> built;
		for (const char *pattern : {
				"{and|then|and then|also|,} ?{please} {apply|insert|implement|commit|incorporate|make|put|write|save|"
					"enter} {it|them|this|that|the change|the changes|the edit|the edits|the redline|the redlines|"
					"the wording|the text|the clause|the fix|the fixes|in|into|to}",
				"{\320\270|\320\267\320\260\321\202\320\265\320\274|\320\277\320\276\321\202\320\276\320\274|\320\260 \320\267\320\260\321\202\320\265\320\274|\320\260 \320\277\320\276\321\202\320\276\320\274|\320\270 \320\277\320\276\321\202\320\276\320\274|\320\270 \321\201\321\200\320\260\320\267\321\203} {\320\262\320\275\320\265\321\201\320\270|\320\262\320\275\320\265\321\201\320\270\321\202\320\265|\320\277\321\200\320\270\320\274\320\265\320\275\320\270|\320\277\321\200\320\270\320\274\320\265\320\275\320\270\321\202\320\265|\320\262\321\201\321\202\320\260\320\262\321\214|\320\262\321\201\321\202\320\260\320\262\321\214\321\202\320\265|"
					"\320\267\320\260\320\274\320\265\320\275\320\270|\320\267\320\260\320\274\320\265\320\275\320\270\321\202\320\265|\320\267\320\260\320\277\320\270\321\210\320\270|\320\267\320\260\320\277\320\270\321\210\320\270\321\202\320\265|\320\262\320\277\320\270\321\210\320\270|\320\262\320\277\320\270\321\210\320\270\321\202\320\265}"})
			built["APPLY"].push_back(phrase_from(pattern));
		// A redline or a markup is of a document they have, never a new one.
		built["NEW"].push_back(phrase_from("^ ?{please|now|also} {draft|prepare|write|create|produce|generate|"
			"put together} ?{me|us} {a|an|a new|new} !{redline|redlines|markup|mark up|revision|revisions|change|"
			"changes|edit|edits|tracked|revised}"));
		return built;
	}();
	return exceptions.at(name);
}

bool phrase_in(const Phrase &phrase, const std::vector<std::string> &sentence) {
	for (std::size_t start = 0; start < sentence.size(); ++start)
		if (follows(phrase, 0, sentence, start)) return true;
	return false;
}

// True when the sentence opens by asking for a change to be made - "Renumber
// the clauses…", "Update clause 9 and…", "…" - rather than
// for one to be drafted, suggested or explained ("Draft…", "Suggest…",
// "Tell…"). "~ACT": what it goes on to say only limits that change.
bool opens_with_change(const std::vector<std::string> &sentence) {
	const RequestWords &known = request_words();
	static const std::set<std::string> drafting = word_set_of({"draft", "drafts", "prepare", "compose", "write",
		"generate", "produce", "create", "redline", "\320\277\320\276\320\264\320\263\320\276\321\202\320\276\320\262\321\214", "\320\277\320\276\320\264\320\263\320\276\321\202\320\276\320\262\321\214\321\202\320\265", "\321\201\320\276\321\201\321\202\320\260\320\262\321\214",
		"\321\201\320\276\321\201\321\202\320\260\320\262\321\214\321\202\320\265", "\320\275\320\260\320\277\320\270\321\210\320\270", "\320\275\320\260\320\277\320\270\321\210\320\270\321\202\320\265", "\320\275\320\260\320\261\321\200\320\276\321\201\320\260\320\271", "\320\275\320\260\320\261\321\200\320\276\321\201\320\260\320\271\321\202\320\265", "\321\201\320\276\320\267\320\264\320\260\320\271", "\321\201\320\276\320\267\320\264\320\260\320\271\321\202\320\265", "\321\201\320\264\320\265\320\273\320\260\320\271", "\321\201\320\264\320\265\320\273\320\260\320\271\321\202\320\265"});
	std::size_t at = 0;
	while (at < sentence.size() && (sentence[at] == kClauseBreak || known.leads.count(sentence[at]) > 0)) ++at;
	if (at >= sentence.size() || drafting.count(sentence[at]) > 0) return false;
	return known.english_verbs.count(sentence[at]) > 0 || known.russian_verbs.count(sentence[at]) > 0;
}

bool unless_holds(const std::string &name, const std::vector<std::string> &sentence) {
	if (name == "ACT") return opens_with_change(sentence);
	const std::vector<Phrase> &exceptions = soft_phrase_exceptions(name);
	return std::any_of(exceptions.begin(), exceptions.end(),
		[&](const Phrase &exception) { return phrase_in(exception, sentence); });
}

// True when the words just before `at`, in its clause, say there is no need
// for what follows: "no need to show me first", "you don't need to ask me
// first", "…".
bool is_waived(const std::vector<std::string> &sentence, std::size_t at) {
	static const std::vector<std::vector<std::string>> waivers = phrases_of({"no need to", "no need", "don't need to",
		"do not need to", "needn't", "need not", "don't have to", "do not have to", "not necessary to",
		"no reason to", "don't bother to", "without having to", "without needing to", "unnecessary to",
		"\320\275\320\265 \320\275\321\203\320\266\320\275\320\276", "\320\275\320\265 \320\275\320\260\320\264\320\276", "\320\275\320\265 \320\276\320\261\321\217\320\267\320\260\321\202\320\265\320\273\321\214\320\275\320\276", "\320\275\320\265\320\276\320\261\321\217\320\267\320\260\321\202\320\265\320\273\321\214\320\275\320\276", "\320\275\320\265\320\267\320\260\321\207\320\265\320\274", "\320\274\320\276\320\266\320\275\320\276 \320\275\320\265", "\320\275\320\265 \321\202\321\200\320\265\320\261\321\203\320\265\321\202\321\201\321\217"});
	for (const std::vector<std::string> &waiver : waivers)
		if (waiver.size() <= at && starts_with_words(sentence, at - waiver.size(), waiver)) return true;
	return false;
}

// Every phrasing of "show me before you change it" Singularity knows. The list is
// closed on purpose: a request it misses still has the model's own askFirst,
// and one it catches waits for one approval more than it needed.
const std::vector<ShowFirstPhrase> &show_first_phrases() {
	static const std::vector<ShowFirstPhrase> phrases = [] {
		const std::vector<std::string> patterns{
			// "Show me first." "Send me the draft first, then I'll decide."
			"@SHOW * {first|beforehand|in advance|ahead of time} {$|then|and|before|so|please|if|i|we|let's}",
			// "Show me the text before you change anything."
			"@SHOW * before {you|we|it|it's|anything|any|they|this|that|the change|the changes|doing|going|"
				"proceeding|@CHANGING}",
			// "Let me see it first." "I'd like to review the changes before they're made."
			"{let me|let us|let's|i want to|i'd like to|i would like to|i need to|i wish to|i'll|i will|i want|"
				"i must|allow me to|i have to|i should|i get to|can i|could i|may i|can we|could we|may we} * @SEE * {first|beforehand|in advance} "
				"{$|then|and|before|so|please|if|i|we|let's}",
			"{let me|let us|let's|i want to|i'd like to|i would like to|i need to|i wish to|i'll|i will|i want|"
				"i must|allow me to|i have to|i should|i get to|can i|could i|may i|can we|could we|may we} * @SEE * before {you|we|it|it's|anything|any|they|"
				"this|that|the change|the changes|doing|going|proceeding|@CHANGING}",
			// "First, show me what you'd change."
			"{first|beforehand} ?{,} {@SHOW|let me see|let me review|let me check|let me look|i want to see|"
				"i want to review|i'd like to see|i'd like to review|i'll review|i'll look|i will review|"
				"i need to see|i need to review}",
			// "Don't change it yet." "Do not edit until I say." Not "don't change
			// anything else", nor "don't change the numbering".
			"@NEG @CHANGE {$|yet|until|till|unless|before|for now|right now|now|without}",
			"@NEG @CHANGE @OBJECT @AFTER !{@EXCEPT}",
			"@NEG @CHANGE * {yet|until|till|for now|right now|without my|without asking|without showing|"
				"without checking|without me|before i|before we|before you show|before showing|before my|unless i}",
			"@NEG {make|apply|commit|implement|insert} {any changes|changes|the changes|any edits|edits|the edits|"
				"the change|any change|anything} !{@EXCEPT|to|in}",
			"{don't|do not} {apply|commit|implement} {$|it|them|anything|yet|until|the change|the changes|the edits|"
				"the edit}",
			// "Do not modify the NDA." "Don't write to the file." "Do not make any
			// changes to the file." "Make no changes."
			"@NEG @CHANGE @DET @DOC @AFTER !{@EXCEPT}",
			"@NEG {write|save|commit|put anything|add anything|make changes|make any changes|make edits|make any edits|"
				"make a change|make any change|make modifications|make any modifications|make alterations|"
				"make any alterations} {to|in|into} @DET @DOC @AFTER !{@EXCEPT}",
			"@NEG {write|save|commit|put anything|add anything} {to|in|into} {it|them|anything} @AFTER",
			"make no {changes|change|edits|edit|modifications|alterations|amendments} "
				"{$|yet|until|till|for now|without|unless|before|to it|to them|in it|in them}",
			"make no {changes|change|edits|edit|modifications|alterations|amendments} {to|in} @DET @DOC @AFTER "
				"!{@EXCEPT}",
			"^ no {changes|edits} {yet|for now|until|till|unless i|without my|without me|without asking|"
				"without checking|without showing|without approval}",
			// "Before changing anything, show me."
			"before {@CHANGING|any changes|changes are made|anything is changed|anything changes} ** {@SHOW|let me see|"
				"let me review|let me check|i want to see|i'd like to see|i want to review|i'd like to review|ask me|"
				"check with me|run it by me|consult me}",
			"before {you|we|i} @CHANGE ** {@SHOW|let me see|let me review|let me check|i want to see|i'd like to see|"
				"i want to review|i'd like to review|ask me|check with me|run it by me|consult me}",
			// "Draft the change, but I want to approve it."
			"{i want to|i'd like to|i would like to|i need to|i wish to|i must|i'll|i will|let me|i get to|i have to|"
				"i should|i'd rather} {approve|sign off on|okay|ok|authorize|authorise|greenlight|green light} "
				"{it|them|this|that|the change|the changes|the edit|the edits|the redline|the redlines|the draft|"
				"each change|every change|any change|any changes|changes|edits|$}",
			// "Show me, then I'll apply it."
			"@SHOW ** {and then|then} {i'll|i will|we'll|we will|we can|you can|i can} {apply|decide|make|approve|"
				"confirm|put|insert|go ahead|proceed}",
			// "Suggest wording without changing the document."
			"without @CHANGING {@OBJECT|any changes|changes|the changes|any edits|edits} {yet|for now|until}",
			// Not "Renumber the clauses without changing the text".
			"~ACT without @CHANGING {@OBJECT|any changes|changes|the changes|any edits|edits} "
				"{$|please|just|only|and|so|i|we} !{@EXCEPT}",
			// "Hold it for my approval." "Nothing goes in until I've seen it."
			"{pending|subject to|until|till|after|once|with|awaiting|await|wait for|waiting for} my "
				"{approval|review|sign off|signoff|ok|okay|go ahead|consent|confirmation|say so|permission|"
				"green light|input|check|look}",
			"for my {approval|sign off|signoff|ok|okay|go ahead|consent|confirmation|say so|permission|green light}",
			// "…and send it to me for approval." "…before anything is saved."
			"{to me|me|over|back} for {approval|sign off|signoff|confirmation|a look|review|my review|checking}",
			"before {it's|it is|they're|they are|anything is|anything's|anything gets|any change is|any changes are|"
				"changes are|the change is|the changes are|it gets|they get|the document is|the file is} {saved|made|"
				"applied|written|changed|put in|inserted|committed|done|finalized|finalised|touched}",
			// "Get my approval before editing." "Seek my approval first."
			"{get|seek|obtain|request|ask for|require|need|have} my {approval|sign off|signoff|ok|okay|go ahead|consent|"
				"confirmation|permission|green light}",
			"{get|seek|obtain} {approval|sign off|signoff|confirmation|permission|the go ahead|a go ahead|the ok|"
				"the green light} {first|beforehand|from me|before}",
			// "Once I give the go-ahead." "After I've had a chance to look."
			"{until|till|once|after|unless|when} {i|i've|i have} {give|given|gave|send|sent} ?{you} {the|my|a} "
				"{go ahead|green light|ok|okay|sign off|signoff|nod|approval|word}",
			"{until|till|once|after|when} {i've had|i have had|i had|i get|i've got|i have} {a chance|the chance|"
				"time|a look|a moment|the time}",
			// "Revise clause 9 for my review." Not "Draft a new NDA for my review":
			// a new document is the draft they review, and nothing they have is
			// changed by writing it.
			"~NEW for my {review|input|check|look}",
			"{until|till|once|after|unless} {i|i've|i have} {approve|approved|confirm|confirmed|say so|said so|"
				"sign off|signed off|review|reviewed|check|checked|agree|agreed|seen|see|look|looked|let you know|"
				"tell you|decide|decided|ok|okay|okayed|ok'd}",
			"{until|till|once|after|unless} {i'm|i am} {happy|satisfied|sure|ok|okay|done}",
			"when {i|i've|i have} {approve|approved|seen|reviewed|checked|confirm|confirmed|agree|agreed|sign off|"
				"signed off|say so|said so|tell you|told you|let you know|give the word|say go}",
			"ask me {first|before|beforehand}",
			"ask {first|beforehand} {$|and|then|please}",
			"ask before {you|making|changing|editing|writing|applying|doing|going}",
			"{check with me|confirm with me|clear it with me|clear this with me|clear that with me|"
				"clear them with me|run it by me|run this by me|run them by me|run that by me|run the change by me|"
				"run the changes by me|run everything by me}",
			"wait for {me|my|approval|confirmation|the go ahead|a go ahead|the ok|an ok|sign off}",
			// Not "Hold on, change the cap instead."
			"hold off",
			"hold on {with|until|till|for now|a moment|a minute|a sec|a second|making|changing|editing|applying}",
			// "Just tell me what's wrong." "Only suggest."
			// Not "Update clause 9 and just list what you changed."
			"~ACT {just|only|simply|merely} {show|tell|suggest|propose|describe|explain|outline|recommend|advise|list|"
				"point out|flag|identify|answer|comment}",
			// Not "Propose a redline for clause 9 and apply it."
			"~APPLY {suggest|propose|recommend} {changes|edits|revisions|amendments|wording|new wording|a wording|"
				"language|redlines|a redline|an edit|a change|a revision|an amendment|improvements|fixes|"
				"corrections|alternative wording|alternatives|options|an option|a fix|a correction|some text|"
				"a clause|clauses}",
			// "What would you change?" "How should I word it?"
			"{what|how} {you'd|you would|you'll|you will|you're going to|you are going to|you plan to|"
				"you intend to|you propose to|you want to|you'd like to|would you|will you|should i|should we|"
				"could you|do you suggest|do you propose|do you recommend} {change|edit|modify|amend|revise|"
				"rewrite|write|put|add|replace|redline|fix|correct|word|phrase|draft|do|suggest|propose}",
			"{as a|in} preview",
			"a preview {first|before|of the change|of the changes|of the edit|of the edits|of the redline|of the draft}",
			"preview {it|them|first|the change|the changes|the edit|the edits|the redline|the draft|before|of}",
			"{dry run|trial run}",
			"{but|and|just|only} not yet",
			"not yet $",
			"{i'll|i will|let me|i want to|i'd like to|i would like to|i'd rather|i prefer to} {decide|apply it|"
				"apply them|apply the change|apply the changes|make the change|make the changes|make the edits|"
				"make the edit|do it myself|edit it myself|change it myself|insert it myself|put it in myself|"
				"do the edits}",
			"{for me to|so i can|so that i can|so i could|to let me} {@SEE|consider|compare|choose|pick}",
			"@SHOW_RU ** @FIRST_RU",
			"@FIRST_RU ?{,} * @SHOW_RU",
			"@SHOW_RU ** {\320\264\320\276 \321\202\320\276\320\263\320\276 \320\272\320\260\320\272|\320\277\320\265\321\200\320\265\320\264 \321\202\320\265\320\274 \320\272\320\260\320\272|\320\277\321\200\320\265\320\266\320\264\320\265 \321\207\320\265\320\274|\320\264\320\276 \320\262\320\275\320\265\321\201\320\265\320\275\320\270\321\217|\320\277\320\265\321\200\320\265\320\264 \320\262\320\275\320\265\321\201\320\265\320\275\320\270\320\265\320\274|\320\264\320\276 \320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217|"
				"\320\277\320\265\321\200\320\265\320\264 \320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\265\320\274|\320\264\320\276 \320\277\321\200\320\260\320\262\320\272\320\270|\320\277\320\265\321\200\320\265\320\264 \320\277\321\200\320\260\320\262\320\272\320\276\320\271|\320\264\320\276 \320\277\321\200\320\260\320\262\320\276\320\272|\320\277\320\265\321\200\320\265\320\264 \320\277\321\200\320\260\320\262\320\272\320\260\320\274\320\270|\320\264\320\276 \320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\271|\320\277\320\265\321\200\320\265\320\264 \320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217\320\274\320\270|"
				"\320\264\320\276 \321\201\320\276\321\205\321\200\320\260\320\275\320\265\320\275\320\270\321\217|\320\277\320\265\321\200\320\265\320\264 \321\201\320\276\321\205\321\200\320\260\320\275\320\265\320\275\320\270\320\265\320\274|\320\264\320\276 \320\267\320\260\320\277\320\270\321\201\320\270|\320\277\320\265\321\200\320\265\320\264 \320\267\320\260\320\277\320\270\321\201\321\214\321\216|\320\264\320\276 \320\277\321\200\320\270\320\274\320\265\320\275\320\265\320\275\320\270\321\217|\320\277\320\265\321\200\320\265\320\264 \320\277\321\200\320\270\320\274\320\265\320\275\320\265\320\275\320\270\320\265\320\274}",
			"{\321\205\320\276\321\207\321\203|\321\205\320\276\321\202\320\265\320\273 \320\261\321\213|\321\205\320\276\321\202\320\265\320\273\320\260 \320\261\321\213|\321\205\320\276\321\202\320\270\320\274|\321\205\320\276\321\202\320\265\320\273\320\276\321\201\321\214 \320\261\321\213|\320\274\320\275\320\265 \320\275\321\203\320\266\320\275\320\276|\320\274\320\275\320\265 \320\275\320\260\320\264\320\276|\320\264\320\260\320\271 \320\274\320\275\320\265|\320\264\320\260\320\271\321\202\320\265 \320\274\320\275\320\265|\321\217 \320\261\321\213 \321\205\320\276\321\202\320\265\320\273|"
				"\321\217 \320\261\321\213 \321\205\320\276\321\202\320\265\320\273\320\260|\321\217 \321\205\320\276\321\207\321\203} * @SEE_RU * {@FIRST_RU|\320\264\320\276 \321\202\320\276\320\263\320\276|\320\277\320\265\321\200\320\265\320\264 \321\202\320\265\320\274|\320\277\321\200\320\265\320\266\320\264\320\265 \321\207\320\265\320\274|\320\264\320\276 \320\262\320\275\320\265\321\201\320\265\320\275\320\270\321\217|"
				"\320\277\320\265\321\200\320\265\320\264 \320\262\320\275\320\265\321\201\320\265\320\275\320\270\320\265\320\274|\320\264\320\276 \320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217|\320\277\320\265\321\200\320\265\320\264 \320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\265\320\274|\320\264\320\276 \320\277\321\200\320\260\320\262\320\272\320\270|\320\277\320\265\321\200\320\265\320\264 \320\277\321\200\320\260\320\262\320\272\320\276\320\271}",
			"{\321\205\320\276\321\207\321\203|\321\205\320\276\321\202\320\265\320\273 \320\261\321\213|\321\205\320\276\321\202\320\265\320\273\320\260 \320\261\321\213|\321\217 \321\205\320\276\321\207\321\203} @FIRST_RU @SEE_RU",
			"@FIRST_RU ?{\321\217} @SEEI_RU",
			"@SEEI_RU @FIRST_RU",
			"@SEEI_RU {\320\270|\320\260} {\320\277\320\276\321\202\320\276\320\274|\320\267\320\260\321\202\320\265\320\274|\321\202\320\276\320\263\320\264\320\260} {\321\200\320\265\321\210\321\203|\321\201\320\272\320\260\320\266\321\203|\320\262\320\275\320\265\321\201\320\265\320\274|\320\262\320\275\320\265\321\201\321\203|\320\262\320\275\320\265\321\201\320\265\321\210\321\214|\320\262\320\275\320\265\321\201\320\265\321\202\320\265|\320\277\321\200\320\270\320\274\320\265\320\275\320\270\320\274|\320\277\321\200\320\270\320\274\320\265\320\275\320\270\321\210\321\214}",
			// "…" Not "…",
			// nor "…".
			"{\320\277\320\276\320\272\320\260|\320\265\321\211\320\265|\321\201\320\265\320\271\321\207\320\260\321\201|\321\201\321\200\320\260\320\267\321\203} ?{\320\275\320\270\321\207\320\265\320\263\320\276|\320\277\320\276\320\272\320\260|\320\265\321\211\320\265|\321\201\320\265\320\271\321\207\320\260\321\201} \320\275\320\265 @CHANGE_RU",
			"<!{@EXCEPT_RU} \320\275\320\270\321\207\320\265\320\263\320\276 \320\275\320\265 @CHANGE_RU !{\320\272\321\200\320\276\320\274\320\265|\320\277\320\276\320\274\320\270\320\274\320\276|\320\267\320\260 \320\270\321\201\320\272\320\273\321\216\321\207\320\265\320\275\320\270\320\265\320\274}",
			"^ ?{\320\275\320\276|\320\270|\320\260|\321\202\320\276\320\273\321\214\320\272\320\276|\320\277\321\200\320\276\321\201\321\202\320\276|\320\277\320\276\320\266\320\260\320\273\321\203\320\271\321\201\321\202\320\260|\321\202\320\260\320\272 \321\207\321\202\320\276|\320\277\320\276\321\215\321\202\320\276\320\274\321\203|\320\276\320\264\320\275\320\260\320\272\320\276} ?{\320\277\320\276\320\266\320\260\320\273\321\203\320\271\321\201\321\202\320\260} \320\275\320\265 @CHANGE_RU "
				"?{\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\271|\320\277\321\200\320\260\320\262\320\272\320\270|\320\277\321\200\320\260\320\262\320\276\320\272|\320\275\320\270\320\272\320\260\320\272\320\270\321\205 \320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\271|\320\275\320\270\320\272\320\260\320\272\320\270\321\205 \320\277\321\200\320\260\320\262\320\276\320\272} @AFTER_RU !{@EXCEPT_RU}",
			"@SHOW_RU ** {\320\260 \320\277\320\276\321\202\320\276\320\274|\320\277\320\276\321\202\320\276\320\274|\320\267\320\260\321\202\320\265\320\274|\320\260 \320\267\320\260\321\202\320\265\320\274|\320\277\320\276\321\201\320\273\320\265 \321\207\320\265\320\263\320\276|\320\270 \320\277\320\276\321\202\320\276\320\274|\320\270 \321\202\320\276\320\263\320\264\320\260|\320\260 \321\202\320\276\320\263\320\264\320\260|\321\202\320\276\320\263\320\264\320\260} {\320\262\320\275\320\265\321\201\320\265\320\274|\320\262\320\275\320\265\321\201\321\203|"
				"\320\262\320\275\320\265\321\201\320\265\321\210\321\214|\320\262\320\275\320\265\321\201\320\265\321\202\320\265|\320\277\321\200\320\270\320\274\320\265\320\275\320\270\320\274|\320\277\321\200\320\270\320\274\320\265\320\275\321\216|\321\200\320\265\321\210\321\203|\321\200\320\265\321\210\320\270\320\274|\321\201\320\272\320\260\320\266\321\203|\320\277\320\276\320\274\320\265\320\275\321\217\320\265\320\274|\320\270\320\267\320\274\320\265\320\275\320\270\320\274|\320\270\321\201\320\277\321\200\320\260\320\262\320\270\320\274|\320\262\321\201\321\202\320\260\320\262\320\270\320\274|\320\262\320\277\320\270\321\210\320\265\320\274|"
				"\320\277\320\276\320\277\321\200\320\260\320\262\320\270\320\274|\321\201\320\276\320\263\320\273\320\260\321\201\321\203\321\216|\320\276\320\264\320\276\320\261\321\200\321\216}",
			"\320\275\320\265 @CHANGE_RU * {\320\277\320\276\320\272\320\260|\320\264\320\276 \321\202\320\276\320\263\320\276|\320\264\320\276 \321\202\320\265\321\205 \320\277\320\276\321\200|\320\261\320\265\320\267 \321\201\320\276\320\263\320\273\320\260\321\201\320\276\320\262\320\260\320\275\320\270\321\217|\320\261\320\265\320\267 \320\276\320\264\320\276\320\261\321\200\320\265\320\275\320\270\321\217|\320\261\320\265\320\267 \320\277\320\276\320\264\321\202\320\262\320\265\321\200\320\266\320\264\320\265\320\275\320\270\321\217|\320\261\320\265\320\267 \321\201\320\277\321\200\320\276\321\201\320\260|"
				"\320\261\320\265\320\267 \321\200\320\260\320\267\321\200\320\265\321\210\320\265\320\275\320\270\321\217|\320\264\320\276 \321\201\320\276\320\263\320\273\320\260\321\201\320\276\320\262\320\260\320\275\320\270\321\217|\320\264\320\276 \321\203\321\202\320\262\320\265\321\200\320\266\320\264\320\265\320\275\320\270\321\217|\320\264\320\276 \320\276\320\264\320\276\320\261\321\200\320\265\320\275\320\270\321\217}",
			"\320\275\320\265 @CHANGE_RU * {\320\264\320\276|\320\261\320\265\320\267} @MY_RU",
			"^ ?{\320\275\320\276|\320\270|\320\260|\321\202\320\276\320\273\321\214\320\272\320\276|\320\277\321\200\320\276\321\201\321\202\320\276|\320\277\320\276\320\266\320\260\320\273\321\203\320\271\321\201\321\202\320\260|\320\277\320\276\320\272\320\260} {\320\275\320\265 \320\275\320\260\320\264\320\276|\320\275\320\265 \320\275\321\203\320\266\320\275\320\276|\320\275\320\265 \321\201\321\202\320\276\320\270\321\202|\320\275\320\265 \321\201\320\277\320\265\321\210\320\270|\320\275\320\265 \321\201\320\277\320\265\321\210\320\270\321\202\320\265|\320\275\320\265 \321\202\320\276\321\200\320\276\320\277\320\270\321\201\321\214|"
				"\320\275\320\265 \321\202\320\276\321\200\320\276\320\277\320\270\321\202\320\265\321\201\321\214|\320\277\320\276\320\272\320\260 \320\275\320\265 \320\275\320\260\320\264\320\276|\320\277\320\276\320\272\320\260 \320\275\320\265 \320\275\321\203\320\266\320\275\320\276} ?{\320\277\320\276\320\272\320\260|\321\201\321\200\320\260\320\267\321\203|\321\201\320\265\320\271\321\207\320\260\321\201|\320\275\320\270\321\207\320\265\320\263\320\276|\320\265\321\211\320\265} ?{\321\201} "
				"{@CHANGE_INF_RU|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217\320\274\320\270|\320\277\321\200\320\260\320\262\320\272\320\260\320\274\320\270|\320\262\320\275\320\265\321\201\320\265\320\275\320\270\320\265\320\274|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\265\320\274|\320\277\321\200\320\260\320\262\320\272\320\276\320\271|\321\215\321\202\320\270\320\274} "
				"{@AFTER_RU|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217|\320\277\321\200\320\260\320\262\320\272\320\270} !{@EXCEPT_RU}",
			"{\320\264\320\276|\320\277\320\276\321\201\320\273\320\265|\321\201|\320\275\320\260|\320\264\320\273\321\217} @MY_RU {\320\276\320\264\320\276\320\261\321\200\320\265\320\275\320\270\321\217|\321\201\320\276\320\263\320\273\320\260\321\201\320\270\321\217|\320\277\320\276\320\264\321\202\320\262\320\265\321\200\320\266\320\264\320\265\320\275\320\270\321\217|\321\203\321\202\320\262\320\265\321\200\320\266\320\264\320\265\320\275\320\270\321\217|\321\201\320\276\320\263\320\273\320\260\321\201\320\276\320\262\320\260\320\275\320\270\321\217|\320\277\321\200\320\276\320\262\320\265\321\200\320\272\320\270|"
				"\320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200\320\260|\321\200\320\260\320\267\321\200\320\265\321\210\320\265\320\275\320\270\321\217|\321\201\320\260\320\275\320\272\321\206\320\270\320\270|\320\272\320\276\320\274\320\260\320\275\320\264\321\213|\320\276\321\202\320\274\320\260\321\210\320\272\320\270|\320\262\320\270\320\267\321\213|\321\200\320\265\320\262\321\214\321\216|\321\200\320\265\321\210\320\265\320\275\320\270\321\217|\320\276\321\202\320\262\320\265\321\202\320\260|\320\276\320\264\320\276\320\261\321\200\320\265\320\275\320\270\320\265|\321\201\320\276\320\263\320\273\320\260\321\201\320\276\320\262\320\260\320\275\320\270\320\265|"
				"\321\203\321\202\320\262\320\265\321\200\320\266\320\264\320\265\320\275\320\270\320\265|\320\277\321\200\320\276\320\262\320\265\321\200\320\272\321\203|\320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200|\321\200\320\265\321\210\320\265\320\275\320\270\320\265|\320\276\320\272|\320\276\320\272\320\265\320\271}",
			"\320\274\320\275\320\265 \320\275\320\260 {\321\201\320\276\320\263\320\273\320\260\321\201\320\276\320\262\320\260\320\275\320\270\320\265|\321\203\321\202\320\262\320\265\321\200\320\266\320\264\320\265\320\275\320\270\320\265|\320\276\320\264\320\276\320\261\321\200\320\265\320\275\320\270\320\265|\320\277\321\200\320\276\320\262\320\265\321\200\320\272\321\203|\320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200|\321\200\320\265\320\262\321\214\321\216}",
			"\320\275\320\260 {\321\201\320\276\320\263\320\273\320\260\321\201\320\276\320\262\320\260\320\275\320\270\320\265|\321\203\321\202\320\262\320\265\321\200\320\266\320\264\320\265\320\275\320\270\320\265|\320\276\320\264\320\276\320\261\321\200\320\265\320\275\320\270\320\265|\320\277\321\200\320\276\320\262\320\265\321\200\320\272\321\203|\320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200|\321\200\320\265\320\262\321\214\321\216} \320\274\320\275\320\265",
			// "…" Not "…
			"{\320\277\321\200\320\270\321\210\320\273\320\270|\320\277\321\200\320\270\321\210\320\273\320\270\321\202\320\265|\320\276\321\202\320\277\321\200\320\260\320\262\321\214|\320\276\321\202\320\277\321\200\320\260\320\262\321\214\321\202\320\265|\320\275\320\260\320\277\321\200\320\260\320\262\321\214|\320\275\320\260\320\277\321\200\320\260\320\262\321\214\321\202\320\265|\320\277\320\265\321\200\320\265\320\264\320\260\320\271|\320\277\320\265\321\200\320\265\320\264\320\260\320\271\321\202\320\265|\320\277\320\276\320\272\320\260\320\266\320\270|\320\277\320\276\320\272\320\260\320\266\320\270\321\202\320\265|\320\277\321\200\320\265\320\264\321\201\321\202\320\260\320\262\321\214|"
				"\320\277\321\200\320\265\320\264\321\201\321\202\320\260\320\262\321\214\321\202\320\265|\320\264\320\260\320\271|\320\264\320\260\320\271\321\202\320\265} ?{\320\274\320\275\320\265|\320\265\320\263\320\276|\320\265\320\265|\320\270\321\205|\321\215\321\202\320\276|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217|\320\277\321\200\320\260\320\262\320\272\320\270|\321\200\320\265\320\264\320\260\320\272\321\206\320\270\321\216|\320\277\321\200\320\276\320\265\320\272\321\202|\320\262\320\260\321\200\320\270\320\260\320\275\321\202} \320\275\320\260 "
				"{\321\201\320\276\320\263\320\273\320\260\321\201\320\276\320\262\320\260\320\275\320\270\320\265|\321\203\321\202\320\262\320\265\321\200\320\266\320\264\320\265\320\275\320\270\320\265|\320\276\320\264\320\276\320\261\321\200\320\265\320\275\320\270\320\265|\320\277\321\200\320\276\320\262\320\265\321\200\320\272\321\203|\320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200|\321\200\320\265\320\262\321\214\321\216}",
			"{\320\264\320\276|\320\277\320\265\321\200\320\265\320\264|\320\277\321\200\320\265\320\266\320\264\320\265} {\320\262\320\275\320\265\321\201\320\265\320\275\320\270\320\265\320\274|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\265\320\274|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217\320\274\320\270|\320\277\321\200\320\260\320\262\320\272\320\276\320\271|\320\277\321\200\320\260\320\262\320\272\320\260\320\274\320\270|\321\201\320\276\321\205\321\200\320\260\320\275\320\265\320\275\320\270\320\265\320\274|\320\267\320\260\320\277\320\270\321\201\321\214\321\216|\320\262\320\275\320\265\321\201\320\265\320\275\320\270\321\217|"
				"\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\271|\320\277\321\200\320\260\320\262\320\272\320\270|\320\277\321\200\320\260\320\262\320\276\320\272|\321\201\320\276\321\205\321\200\320\260\320\275\320\265\320\275\320\270\321\217} ** @SHOW_RU",
			"{\320\277\320\265\321\200\320\265\320\264 \321\202\320\265\320\274 \320\272\320\260\320\272|\320\277\321\200\320\265\320\266\320\264\320\265 \321\207\320\265\320\274|\320\264\320\276 \321\202\320\276\320\263\320\276 \320\272\320\260\320\272} ** @SHOW_RU",
			"{\321\201\320\277\321\200\320\276\321\201\320\270|\321\201\320\277\321\200\320\276\321\201\320\270\321\202\320\265|\321\203\321\202\320\276\321\207\320\275\320\270|\321\203\321\202\320\276\321\207\320\275\320\270\321\202\320\265|\321\201\320\276\320\263\320\273\320\260\321\201\321\203\320\271|\321\201\320\276\320\263\320\273\320\260\321\201\321\203\320\271\321\202\320\265|\321\201\320\262\320\265\321\200\321\214\321\201\321\217|\321\201\320\262\320\265\321\200\321\214\321\202\320\265\321\201\321\214|\320\277\320\276\321\201\320\276\320\262\320\265\321\202\321\203\320\271\321\201\321\217|\320\277\320\276\321\201\320\276\320\262\320\265\321\202\321\203\320\271\321\202\320\265\321\201\321\214|"
				"\321\201\320\262\321\217\320\266\320\270\321\201\321\214|\321\201\320\262\321\217\320\266\320\270\321\202\320\265\321\201\321\214} {\320\274\320\265\320\275\321\217|\321\203 \320\274\320\265\320\275\321\217|\321\201\320\276 \320\274\320\275\320\276\320\271}",
			"{\320\264\320\276\320\266\320\264\320\270\321\201\321\214|\320\264\320\276\320\266\320\264\320\270\321\202\320\265\321\201\321\214|\320\266\320\264\320\270|\320\266\320\264\320\270\321\202\320\265|\320\277\320\276\320\264\320\276\320\266\320\264\320\270|\320\277\320\276\320\264\320\276\320\266\320\264\320\270\321\202\320\265|\320\264\320\276\320\266\320\270\320\264\320\260\320\271\321\201\321\217} {\320\274\320\265\320\275\321\217|\320\277\320\276\320\272\320\260 \321\217|@MY_RU|\320\276\321\202\320\262\320\265\321\202\320\260|\320\276\320\264\320\276\320\261\321\200\320\265\320\275\320\270\321\217|"
				"\321\201\320\276\320\263\320\273\320\260\321\201\320\270\321\217|\320\277\320\276\320\264\321\202\320\262\320\265\321\200\320\266\320\264\320\265\320\275\320\270\321\217|\320\276\321\202\320\274\320\260\321\210\320\272\320\270|\320\272\320\276\320\274\320\260\320\275\320\264\321\213}",
			"{\320\277\320\276\320\264\320\276\320\266\320\264\320\270|\320\277\320\276\320\264\320\276\320\266\320\264\320\270\321\202\320\265} \321\201 {\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217\320\274\320\270|\320\277\321\200\320\260\320\262\320\272\320\260\320\274\320\270|\321\215\321\202\320\270\320\274|\320\262\320\275\320\265\321\201\320\265\320\275\320\270\320\265\320\274|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\265\320\274|\320\277\321\200\320\260\320\262\320\272\320\276\320\271}",
			"{\320\277\320\276\320\272\320\260|\320\264\320\276 \321\202\320\276\320\263\320\276 \320\272\320\260\320\272|\320\277\321\200\320\265\320\266\320\264\320\265 \321\207\320\265\320\274|\320\277\320\265\321\200\320\265\320\264 \321\202\320\265\320\274 \320\272\320\260\320\272|\320\277\320\276\321\201\320\273\320\265 \321\202\320\276\320\263\320\276 \320\272\320\260\320\272|\320\272\320\260\320\272 \321\202\320\276\320\273\321\214\320\272\320\276} \321\217 ?{\320\275\320\265} {@SEEI_RU|\320\264\320\260\320\274 \320\264\320\276\320\261\321\200\320\276|"
				"\320\264\320\260\320\274 \320\276\321\202\320\274\320\260\321\210\320\272\321\203|\320\264\320\260\320\274 \320\276\320\272|\320\264\320\260\320\274 \321\201\320\276\320\263\320\273\320\260\321\201\320\270\320\265|\321\200\320\260\320\267\321\200\320\265\321\210\321\203|\321\201\320\272\320\260\320\266\321\203}",
			"~ACT {\321\202\320\276\320\273\321\214\320\272\320\276|\320\277\321\200\320\276\321\201\321\202\320\276|\320\273\320\270\321\210\321\214} {\320\277\320\276\320\272\320\260\320\266\320\270|\320\277\320\276\320\272\320\260\320\266\320\270\321\202\320\265|\321\201\320\272\320\260\320\266\320\270|\321\201\320\272\320\260\320\266\320\270\321\202\320\265|\320\277\321\200\320\265\320\264\320\273\320\276\320\266\320\270|\320\277\321\200\320\265\320\264\320\273\320\276\320\266\320\270\321\202\320\265|\320\276\320\277\320\270\321\210\320\270|\320\276\320\277\320\270\321\210\320\270\321\202\320\265|\320\277\320\265\321\200\320\265\321\207\320\270\321\201\320\273\320\270|"
				"\320\277\320\265\321\200\320\265\321\207\320\270\321\201\320\273\320\270\321\202\320\265|\320\276\320\261\321\212\321\217\321\201\320\275\320\270|\320\276\320\261\321\212\321\217\321\201\320\275\320\270\321\202\320\265|\320\277\320\276\320\264\321\201\320\272\320\260\320\266\320\270|\320\277\320\276\320\264\321\201\320\272\320\260\320\266\320\270\321\202\320\265|\320\277\320\276\321\201\320\276\320\262\320\265\321\202\321\203\320\271|\320\277\320\276\321\201\320\276\320\262\320\265\321\202\321\203\320\271\321\202\320\265|\320\276\321\202\320\262\320\265\321\202\321\214|\320\276\321\202\320\262\320\265\321\202\321\214\321\202\320\265|\320\276\321\202\320\274\320\265\321\202\321\214|"
				"\320\276\321\202\320\274\320\265\321\202\321\214\321\202\320\265|\321\203\320\272\320\260\320\266\320\270|\321\203\320\272\320\260\320\266\320\270\321\202\320\265|\320\262\321\213\320\264\320\265\320\273\320\270|\320\262\321\213\320\264\320\265\320\273\320\270\321\202\320\265|\320\277\321\200\320\276\320\272\320\276\320\274\320\274\320\265\320\275\321\202\320\270\321\200\321\203\320\271|\320\277\321\200\320\276\320\272\320\276\320\274\320\274\320\265\320\275\321\202\320\270\321\200\321\203\320\271\321\202\320\265}",
			"~APPLY {\320\277\321\200\320\265\320\264\320\273\320\276\320\266\320\270|\320\277\321\200\320\265\320\264\320\273\320\276\320\266\320\270\321\202\320\265|\320\277\321\200\320\265\320\264\320\273\320\276\320\266\320\270\321\202\321\214} {\320\277\321\200\320\260\320\262\320\272\320\270|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217|\321\200\320\265\320\264\320\260\320\272\321\206\320\270\321\216|\321\204\320\276\321\200\320\274\321\203\320\273\320\270\321\200\320\276\320\262\320\272\321\203|\321\204\320\276\321\200\320\274\321\203\320\273\320\270\321\200\320\276\320\262\320\272\320\270|\320\262\320\260\321\200\320\270\320\260\320\275\321\202|\320\262\320\260\321\200\320\270\320\260\320\275\321\202\321\213|"
				"\320\270\321\201\320\277\321\200\320\260\320\262\320\273\320\265\320\275\320\270\321\217|\320\277\320\276\320\277\321\200\320\260\320\262\320\272\320\270|\321\202\320\265\320\272\321\201\321\202|\320\277\321\200\320\260\320\262\320\272\321\203|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\265|\320\275\320\276\320\262\321\203\321\216 \321\200\320\265\320\264\320\260\320\272\321\206\320\270\321\216|\321\201\320\262\320\276\321\216 \321\200\320\265\320\264\320\260\320\272\321\206\320\270\321\216|\321\201\320\262\320\276\320\271 \320\262\320\260\321\200\320\270\320\260\320\275\321\202|\320\260\320\273\321\214\321\202\320\265\321\200\320\275\320\260\321\202\320\270\320\262\321\203}",
			"{\321\207\321\202\320\276|\320\272\320\260\320\272|\320\263\320\264\320\265|\320\272\320\260\320\272\320\270\320\265|\320\272\320\260\320\272\321\203\321\216|\320\272\320\260\320\272\320\276\320\271} {\320\261\321\213 \321\202\321\213|\320\261\321\213 \320\262\321\213|\321\202\321\213 \320\261\321\213|\320\262\321\213 \320\261\321\213} {\320\270\320\267\320\274\320\265\320\275\320\270\320\273|\320\270\320\267\320\274\320\265\320\275\320\270\320\273\320\260|\320\270\320\267\320\274\320\265\320\275\320\270\320\273\320\270|\320\277\320\276\320\274\320\265\320\275\321\217\320\273|"
				"\320\277\320\276\320\274\320\265\320\275\321\217\320\273\320\260|\320\277\320\276\320\274\320\265\320\275\321\217\320\273\320\270|\320\277\320\276\320\277\321\200\320\260\320\262\320\270\320\273|\320\277\320\276\320\277\321\200\320\260\320\262\320\270\320\273\320\260|\320\277\320\276\320\277\321\200\320\260\320\262\320\270\320\273\320\270|\320\270\321\201\320\277\321\200\320\260\320\262\320\270\320\273|\320\270\321\201\320\277\321\200\320\260\320\262\320\270\320\273\320\260|\320\270\321\201\320\277\321\200\320\260\320\262\320\270\320\273\320\270|\320\277\321\200\320\265\320\264\320\273\320\276\320\266\320\270\320\273|\320\277\321\200\320\265\320\264\320\273\320\276\320\266\320\270\320\273\320\260|"
				"\320\277\321\200\320\265\320\264\320\273\320\276\320\266\320\270\320\273\320\270|\320\262\320\275\320\265\321\201|\320\262\320\275\320\265\321\201\320\273\320\260|\320\262\320\275\320\265\321\201\320\273\320\270|\320\277\320\265\321\200\320\265\320\277\320\270\321\201\320\260\320\273|\320\277\320\265\321\200\320\265\320\277\320\270\321\201\320\260\320\273\320\260|\320\277\320\265\321\200\320\265\320\277\320\270\321\201\320\260\320\273\320\270|\321\201\320\264\320\265\320\273\320\260\320\273|\321\201\320\264\320\265\320\273\320\260\320\273\320\260|\321\201\320\264\320\265\320\273\320\260\320\273\320\270|\320\275\320\260\320\277\320\270\321\201\320\260\320\273|"
				"\320\275\320\260\320\277\320\270\321\201\320\260\320\273\320\260|\320\275\320\260\320\277\320\270\321\201\320\260\320\273\320\270|\320\264\320\276\320\261\320\260\320\262\320\270\320\273|\320\264\320\276\320\261\320\260\320\262\320\270\320\273\320\260|\320\264\320\276\320\261\320\260\320\262\320\270\320\273\320\270|\321\201\321\204\320\276\321\200\320\274\321\203\320\273\320\270\321\200\320\276\320\262\320\260\320\273|\321\201\321\204\320\276\321\200\320\274\321\203\320\273\320\270\321\200\320\276\320\262\320\260\320\273\320\260|\321\201\321\204\320\276\321\200\320\274\321\203\320\273\320\270\321\200\320\276\320\262\320\260\320\273\320\270}",
			"{\321\201\320\260\320\274|\321\201\320\260\320\274\320\260|\321\201\320\260\320\274\320\270} {\320\262\320\275\320\265\321\201\321\203|\320\262\320\275\320\265\321\201\320\265\320\274|\320\277\320\276\320\277\321\200\320\260\320\262\320\273\321\216|\320\277\320\276\320\277\321\200\320\260\320\262\320\270\320\274|\320\270\321\201\320\277\321\200\320\260\320\262\320\273\321\216|\320\270\321\201\320\277\321\200\320\260\320\262\320\270\320\274|\320\270\320\267\320\274\320\265\320\275\321\216|\320\270\320\267\320\274\320\265\320\275\320\270\320\274|\320\277\321\200\320\270\320\274\320\265\320\275\321\216|\320\277\321\200\320\270\320\274\320\265\320\275\320\270\320\274|"
				"\320\276\321\202\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\321\203\321\216|\321\200\320\265\321\210\321\203|\321\200\320\265\321\210\320\270\320\274|\320\262\321\201\321\202\320\260\320\262\320\273\321\216|\320\262\321\201\321\202\320\260\320\262\320\270\320\274|\320\262\320\277\320\270\321\210\321\203|\320\262\320\277\320\270\321\210\320\265\320\274}",
			"{\320\262\320\275\320\265\321\201\321\203|\320\262\320\275\320\265\321\201\320\265\320\274|\320\277\320\276\320\277\321\200\320\260\320\262\320\273\321\216|\320\270\321\201\320\277\321\200\320\260\320\262\320\273\321\216|\320\270\320\267\320\274\320\265\320\275\321\216|\320\277\321\200\320\270\320\274\320\265\320\275\321\216|\320\276\321\202\321\200\320\265\320\264\320\260\320\272\321\202\320\270\321\200\321\203\321\216|\320\262\321\201\321\202\320\260\320\262\320\273\321\216|\320\262\320\277\320\270\321\210\321\203} * {\321\201\320\260\320\274|\321\201\320\260\320\274\320\260|\321\201\320\260\320\274\320\270}",
			"\320\261\320\265\320\267 {\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\271|\320\277\321\200\320\260\320\262\320\272\320\270|\320\277\321\200\320\260\320\262\320\276\320\272|\320\262\320\275\320\265\321\201\320\265\320\275\320\270\321\217 \320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\320\271|\320\262\320\275\320\265\321\201\320\265\320\275\320\270\321\217 \320\277\321\200\320\260\320\262\320\276\320\272} "
				"{\320\264\320\276\320\272\321\203\320\274\320\265\320\275\321\202\320\260|\321\204\320\260\320\271\320\273\320\260|\320\264\320\276\320\263\320\276\320\262\320\276\321\200\320\260|\320\276\321\200\320\270\320\263\320\270\320\275\320\260\320\273\320\260|\321\202\320\265\320\272\321\201\321\202\320\260|$}",
			// Not "…"
			"~ACT \320\275\320\265 @CHANGING_RU {\320\264\320\276\320\272\321\203\320\274\320\265\320\275\321\202|\320\264\320\276\320\272\321\203\320\274\320\265\320\275\321\202\321\213|\321\204\320\260\320\271\320\273|\321\204\320\260\320\271\320\273\321\213|\320\264\320\276\320\263\320\276\320\262\320\276\321\200|\320\276\321\200\320\270\320\263\320\270\320\275\320\260\320\273|\321\202\320\265\320\272\321\201\321\202|\320\265\320\263\320\276|\320\265\320\265|\320\270\321\205|\320\275\320\270\321\207\320\265\320\263\320\276|\320\270\320\267\320\274\320\265\320\275\320\265\320\275\320\270\321\217|"
				"\320\277\321\200\320\260\320\262\320\272\320\270|$} !{@EXCEPT_RU}",
			"<!{@EXCEPT_RU} \320\275\320\270\321\207\320\265\320\263\320\276 \320\275\320\265 @CHANGING_RU !{\320\272\321\200\320\276\320\274\320\265|\320\277\320\276\320\274\320\270\320\274\320\276|\320\267\320\260 \320\270\321\201\320\272\320\273\321\216\321\207\320\265\320\275\320\270\320\265\320\274}",
			"{\320\277\321\200\320\265\320\264\320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200|\320\277\321\200\320\265\320\262\321\214\321\216|\320\277\321\200\320\265\320\264\320\262\320\260\321\200\320\270\321\202\320\265\320\273\321\214\320\275\321\213\320\271 \320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200|\320\277\321\200\320\265\320\264\320\262\320\260\321\200\320\270\321\202\320\265\320\273\321\214\320\275\320\276\320\263\320\276 \320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200\320\260|\320\277\321\200\320\265\320\264\320\262\320\260\321\200\320\270\321\202\320\265\320\273\321\214\320\275\320\276\320\274 \320\277\321\200\320\276\321\201\320\274\320\276\321\202\321\200\320\265}",
			"{\320\275\320\276|\321\202\320\276\320\273\321\214\320\272\320\276} \320\275\320\265 {\321\201\320\265\320\271\321\207\320\260\321\201|\321\201\321\200\320\260\320\267\321\203|\320\277\320\276\320\272\320\260}",
		};
		std::vector<ShowFirstPhrase> built;
		for (const std::string &pattern : patterns) {
			ShowFirstPhrase phrase;
			std::string written = pattern;
			if (!written.empty() && written[0] == '~') {
				const std::size_t space = written.find(' ');
				phrase.unless = written.substr(1, space - 1);
				written = written.substr(space + 1);
			}
			phrase.phrase = phrase_from(written);
			built.push_back(std::move(phrase));
		}
		return built;
	}();
	return phrases;
}

ToolCallEvidence evidence_for(const ToolExecutionContext &context, const ToolDefinition &definition,
    const ToolIntent &intent) {
  ToolCallEvidence evidence;
  evidence.id = random_id("tce_");
  evidence.tool_id = definition.id;
  evidence.capability = definition.capability;
  evidence.effect = definition.effect;
  evidence.effective_mode = context.permission.effective_mode;
  evidence.started_at = current_timestamp();
  evidence.normalized_intent_digest = normalized_intent_digest(definition, intent);
  return evidence;
}

// A handler that called a paid external model service reports it under
// "externalCall" ({providerId, modelId, usage}); the broker moves that report
// onto the call's evidence and removes it from the result, so it never reaches
// a model as tool output.
void record_external_call(nlohmann::json &value, ToolCallEvidence &evidence) {
  if (value.is_object() == false || value.contains("externalCall") == false) return;
  const nlohmann::json call = value.at("externalCall");
  value.erase("externalCall");
  if (call.is_object() == false) return;
  const auto text = [&](const char *key) -> std::optional<std::string> {
    if (call.contains(key) == false || call.at(key).is_string() == false || call.at(key).get_ref<const std::string &>().empty())
      return std::nullopt;
    return call.at(key).get<std::string>();
  };
  evidence.external_provider_id = text("providerId");
  evidence.external_model_id = text("modelId");
  if (call.contains("usage") == false || call.at("usage").is_object() == false) return;
  const nlohmann::json &reported = call.at("usage");
  const auto count = [&](const char *key) -> std::optional<long long> {
    if (reported.contains(key) == false || reported.at(key).is_number_integer() == false) return std::nullopt;
    return reported.at(key).get<long long>();
  };
  const auto label = [&](const char *key) -> std::optional<std::string> {
    if (reported.contains(key) == false || reported.at(key).is_string() == false) return std::nullopt;
    return reported.at(key).get<std::string>();
  };
  providers::UsageMetadata usage;
  usage.input_tokens = count("inputTokens");
  usage.output_tokens = count("outputTokens");
  usage.total_tokens = count("totalTokens");
  usage.cost_micros = count("costMicros");
  usage.cost_currency = label("costCurrency");
  usage.cost_classification = label("costClassification");
  usage.cost_methodology = label("costMethodology");
  evidence.external_usage = std::move(usage);
}

// The tool's own check of what it was asked to do, refused in the tool's own
// words when it fails. Run before a person is ever asked to approve a call, as
// well as before it executes: a change Singularity could not make is never put in
// front of a lawyer to approve.
std::optional<ToolAttempt> validation_refusal(const ToolExecutionContext &context, const ToolDefinition &definition,
    const ToolIntent &intent, const ToolCallEvidence &evidence) {
  if (!definition.validate) return std::nullopt;
  const std::optional<std::string> validationError = definition.validate(context, intent.arguments);
  if (!validationError.has_value()) return std::nullopt;
  ToolAttempt attempt;
  attempt.evidence = evidence;
  attempt.result.outcome = ToolOutcome::Failed;
  attempt.result.error_category = *validationError;
  // Every ToolValidator returns a short category slug, never tool arguments or
  // secrets, so it is always safe as the plain-language error_message too -
  // without this, every validation rejection left error_message empty and a
  // Required-research caller (ResearchToolLoop) had no way to tell "the
  // model's tool call failed schema validation" apart from every other
  // non-Executed outcome, collapsing them all into one generic, undiagnosable
  // message.
  const std::string explained = definition.explain_rejection
      ? definition.explain_rejection(*validationError) : std::string();
  attempt.result.error_message = explained.empty()
      ? "Tool argument validation rejected this request: " + *validationError
      : explained;
  attempt.evidence.outcome = ToolOutcome::Failed;
  attempt.evidence.error_category = *validationError;
  attempt.evidence.completed_at = current_timestamp();
  return attempt;
}

ToolAttempt execute_definition(const ToolExecutionContext &context, const ToolDefinition &definition,
    const ToolIntent &intent, ToolCallEvidence evidence) {
  ToolAttempt attempt;
  attempt.evidence = std::move(evidence);
	auto reject_stale = [&]() {
		attempt.result.outcome = ToolOutcome::Denied;
		attempt.result.error_category = "operation_stopped";
		attempt.evidence.outcome = ToolOutcome::Denied;
		attempt.evidence.error_category = attempt.result.error_category;
		attempt.evidence.completed_at = current_timestamp();
	};
	if (context.operation_is_current && !context.operation_is_current()) {
		reject_stale();
		return attempt;
	}
  try {
    if (std::optional<ToolAttempt> refused = validation_refusal(context, definition, intent, attempt.evidence))
      return *refused;
		if (context.operation_is_current && !context.operation_is_current()) {
			reject_stale();
			return attempt;
		}
    attempt.result.value = definition.execute(context, intent.arguments);
    attempt.result.outcome = ToolOutcome::Executed;
    attempt.evidence.outcome = ToolOutcome::Executed;
    record_external_call(attempt.result.value, attempt.evidence);
  } catch (const BilledExternalCallFailure &error) {
    attempt.result.outcome = ToolOutcome::Failed;
    attempt.result.error_category = "handler_execution_failed";
    attempt.result.error_message = error.what();
    attempt.evidence.outcome = ToolOutcome::Failed;
    attempt.evidence.error_category = attempt.result.error_category;
    attempt.evidence.external_provider_id = error.provider_id;
    attempt.evidence.external_model_id = error.model_id;
    attempt.evidence.external_usage = error.usage;
  } catch (const std::exception &error) {
    attempt.result.outcome = ToolOutcome::Failed;
    attempt.result.error_category = "handler_execution_failed";
	attempt.result.error_message = error.what();
    attempt.evidence.outcome = ToolOutcome::Failed;
    attempt.evidence.error_category = attempt.result.error_category;
  }
  attempt.evidence.completed_at = current_timestamp();
  return attempt;
}

} // namespace

OrganizationToolPolicy organization_tool_policy() {
  if (has_policy_override() == true) return policy_override();
  return OrganizationToolPolicy{};
}

void set_organization_tool_policy_for_testing(const OrganizationToolPolicy &policy) {
  policy_override() = policy;
  has_policy_override() = true;
}

void set_waiter_arrival_grace_for_testing(std::chrono::milliseconds grace) {
  waiter_arrival_grace() = grace;
}

void reset_organization_tool_policy_for_testing() {
  policy_override() = OrganizationToolPolicy{};
  has_policy_override() = false;
}

namespace {
std::optional<ProjectActionMode> &thread_action_mode_cap() {
  thread_local std::optional<ProjectActionMode> cap;
  return cap;
}
} // namespace

ScopedActionModeCap::ScopedActionModeCap(ProjectActionMode cap) : previous_(thread_action_mode_cap()) {
  thread_action_mode_cap() = cap;
}

ScopedActionModeCap::~ScopedActionModeCap() {
  thread_action_mode_cap() = previous_;
}

ProjectActionPermissionState project_action_permission(const std::string &projectId) {
  if (projectId.empty()) throw std::runtime_error("This action needs a specific Project.");
  const platform::AppSettings settings = platform::load_app_settings();
  ProjectActionMode requestedMode = ProjectActionMode::AskForApproval;
  for (const platform::ProjectActionPreference &preference : settings.project_action_preferences) {
    if (preference.project_id != projectId) continue;
    requestedMode = project_action_mode_from_string(preference.requested_mode)
      .value_or(ProjectActionMode::AskForApproval);
    break;
  }
  const OrganizationToolPolicy policy = organization_tool_policy();
  ProjectActionPermissionState result;
  result.requested_mode = requestedMode;
  result.effective_mode = mode_rank(requestedMode) <= mode_rank(policy.maximum_action_mode)
    ? requestedMode : policy.maximum_action_mode;
  result.is_capped = result.effective_mode != requestedMode;
  result.cap_reason = result.is_capped == true
    ? (policy.cap_reason.empty() ? "Your organization limits Project actions on this device." : policy.cap_reason)
    : std::string();
  result.external_network_permitted = policy.external_network_permitted;
  result.project_mutation_permitted = policy.project_mutation_permitted;
  const std::optional<ProjectActionMode> &cap = thread_action_mode_cap();
  if (cap.has_value() == true && mode_rank(result.effective_mode) > mode_rank(*cap)) {
    result.effective_mode = *cap;
    result.is_capped = true;
    result.cap_reason = "Agents read and propose; every change they would make waits for your approval.";
  }
  return result;
}

ProjectActionPermissionState save_project_action_permission(const std::string &projectId,
    ProjectActionMode requestedMode) {
  if (projectId.empty()) throw std::runtime_error("This action needs a specific Project.");
  platform::AppSettings settings = platform::load_app_settings();
  auto found = std::find_if(settings.project_action_preferences.begin(),
    settings.project_action_preferences.end(), [&](const platform::ProjectActionPreference &preference) {
      return preference.project_id == projectId;
    });
  if (found == settings.project_action_preferences.end()) {
    settings.project_action_preferences.push_back({projectId, to_string(requestedMode)});
  } else {
    found->requested_mode = to_string(requestedMode);
  }
  platform::save_app_settings(settings);
  return project_action_permission(projectId);
}

PermissionDisposition permission_for(const ProjectActionPermissionState &permission,
    ToolEffectClass effect) {
  if (effect == ToolEffectClass::Unclassified) return PermissionDisposition::Deny;
  if (effect == ToolEffectClass::ExternalNetwork && permission.external_network_permitted == false) {
    return PermissionDisposition::Deny;
  }
  if (is_project_mutation(effect) == true && permission.project_mutation_permitted == false) {
    return PermissionDisposition::Deny;
  }
  if (effect == ToolEffectClass::LocalRead || effect == ToolEffectClass::LocalProposal) {
    return PermissionDisposition::Execute;
  }
  if (permission.effective_mode == ProjectActionMode::AskForApproval) {
    return PermissionDisposition::RequireApproval;
  }
  if (permission.effective_mode == ProjectActionMode::ApproveSafeActions
    && effect == ToolEffectClass::ElevatedProjectMutation) {
    return PermissionDisposition::RequireApproval;
  }
  return PermissionDisposition::Execute;
}

std::string normalized_intent_digest(const ToolDefinition &definition, const ToolIntent &intent) {
  const nlohmann::json normalized{
    {"arguments", intent.arguments},
    {"capability", definition.capability},
    {"effect", to_string(definition.effect)},
    {"toolId", definition.id},
  };
  return platform::sha256_hex_digest(normalized.dump());
}

bool asks_to_see_changes_first(const std::string &words) {
	const std::vector<ShowFirstPhrase> &phrases = show_first_phrases();
	for (const std::vector<std::string> &sentence : sentences_of(words))
		for (const ShowFirstPhrase &phrase : phrases) {
			bool isFound = false;
			for (std::size_t start = 0; start < sentence.size() && !isFound; ++start)
				isFound = follows(phrase.phrase, 0, sentence, start) && !is_waived(sentence, start);
			if (!isFound) continue;
			if (phrase.unless.empty() || !unless_holds(phrase.unless, sentence)) return true;
		}
	return false;
}

bool quotes_lawyer(const std::string &quote, const std::vector<std::string> &messages) {
	// Where the quote leaves words out, each part it keeps is quoted on its own,
	// in order.
	std::vector<std::vector<std::string>> parts;
	std::size_t quotedWords = 0;
	std::string rest = quote;
	for (const std::string &ellipsis : {std::string("\xE2\x80\xA6"), std::string("...")})
		for (std::size_t found = rest.find(ellipsis); found != std::string::npos; found = rest.find(ellipsis))
			rest.replace(found, ellipsis.size(), "\n\x1F\n");
	std::size_t from = 0;
	while (from <= rest.size()) {
		const std::size_t cut = rest.find("\n\x1F\n", from);
		std::vector<std::string> part = words_of(rest.substr(from, cut == std::string::npos ? std::string::npos : cut - from));
		quotedWords += part.size();
		if (!part.empty()) parts.push_back(std::move(part));
		if (cut == std::string::npos) break;
		from = cut + 3;
	}
	if (parts.empty()) return false;
	constexpr std::size_t kShortestQuote = 3;
	// Stands where a sentence that asks for no change was - a question, a
	// request to be told, a statement: no quote runs through it.
	const std::string kAskedForNothing = "\x1F";
	for (const std::string &message : messages) {
		std::vector<bool> questions;
		const std::vector<std::vector<std::string>> sentences = sentences_of(message, &questions);
		std::vector<std::string> said;
		std::size_t saidWords = 0;
		for (std::size_t index = 0; index < sentences.size(); ++index) {
			if (!asks_for_change(sentences[index], questions[index])) {
				said.push_back(kAskedForNothing);
				continue;
			}
			for (const std::string &word : sentences[index]) {
				if (word == kClauseBreak) continue;
				said.push_back(word);
				++saidWords;
			}
		}
		std::size_t at = 0;
		bool isFound = true;
		for (const std::vector<std::string> &part : parts) {
			const auto found = std::search(said.begin() + static_cast<std::ptrdiff_t>(at), said.end(), part.begin(), part.end());
			if (found == said.end()) {
				isFound = false;
				break;
			}
			at = static_cast<std::size_t>(found - said.begin()) + part.size();
		}
		if (!isFound) continue;
		if (quotedWords >= kShortestQuote) return true;
		if (parts.size() == 1 && parts.front().size() == saidWords && said.size() == saidWords) return true;
	}
	return false;
}

bool takes_out_substance(const std::string &before, const std::string &after) {
	const std::vector<std::string> was = words_of(before);
	if (was.empty()) return false;
	const std::vector<std::string> becomes = words_of(after);
	if (becomes.empty()) return true;
	std::map<std::string, int> kept;
	for (const std::string &word : becomes) ++kept[word];
	std::size_t removed = 0;
	for (const std::string &word : was) {
		auto found = kept.find(word);
		if (found != kept.end() && found->second > 0) --found->second;
		else ++removed;
	}
	// Most of what the passage said is gone: a clause rewritten as
	// "[Reserved.]", a sentence as ".".
	constexpr std::size_t kFewestRemovedWords = 4;
	if (removed >= kFewestRemovedWords && removed * 2 > was.size()) return true;
	// What it says instead is only that nothing is there: "0", "nil", "None",
	// "Intentionally omitted", "…". A zero is nothing only in a text
	// with no other figure ("1,000" is a figure).
	static const std::set<std::string> nothing = word_set_of({"nil", "none", "na", "reserved", "intentionally",
		"omitted", "deleted", "removed", "blank", "not", "used", "applicable", "\320\275\320\265\321\202", "\320\270\321\201\320\272\320\273\321\216\321\207\320\265\320\275", "\320\270\321\201\320\272\320\273\321\216\321\207\320\265\320\275\320\276",
		"\320\270\321\201\320\272\320\273\321\216\321\207\320\265\320\275\320\260", "\321\203\320\264\320\260\320\273\320\265\320\275", "\321\203\320\264\320\260\320\273\320\265\320\275\320\276", "\321\203\320\264\320\260\320\273\320\265\320\275\320\260", "\320\267\320\260\321\200\320\265\320\267\320\265\321\200\320\262\320\270\321\200\320\276\320\262\320\260\320\275\320\276", "\320\277\321\203\321\201\321\202\320\276"});
	const auto isZero = [](const std::string &word) { return word.find_first_not_of('0') == std::string::npos; };
	const bool hasFigure = std::any_of(becomes.begin(), becomes.end(), [](const std::string &word) {
		return word.find_first_of("123456789") != std::string::npos; });
	std::map<std::string, int> said;
	for (const std::string &word : was) ++said[word];
	bool saysSomething = false;
	for (const std::string &word : becomes) {
		auto found = said.find(word);
		if (found != said.end() && found->second > 0) {
			--found->second;
			continue;
		}
		if (nothing.count(word) == 0 && !(isZero(word) && !hasFigure)) saysSomething = true;
	}
	if (!saysSomething && removed > 0) return true;
	// A word that limits what the passage says is taken out: "shall not be
	// liable" is not "shall be liable", nor "…".
	static const std::set<std::string> limits = word_set_of({"not", "no", "never", "neither", "nor", "without",
		"except", "unless", "excluding", "cannot", "can't", "won't", "shan't", "shouldn't", "mustn't", "don't",
		"doesn't", "isn't", "aren't", "\320\275\320\265", "\320\275\320\265\321\202", "\320\275\320\270", "\320\261\320\265\320\267", "\320\272\321\200\320\276\320\274\320\265", "\320\270\321\201\320\272\320\273\321\216\321\207\320\265\320\275\320\270\320\265\320\274"});
	std::map<std::string, int> limitsLeft;
	for (const std::string &word : becomes)
		if (limits.count(word) > 0) ++limitsLeft[word];
	for (const std::string &word : was) {
		if (limits.count(word) == 0) continue;
		if (limitsLeft[word] > 0) --limitsLeft[word];
		else return true;
	}
	return false;
}

namespace {

// Letters as code points, so a correction is counted in letters, not bytes.
std::u32string letters_of(const std::string &word) {
	std::u32string result;
	const auto *bytes = reinterpret_cast<const utf8proc_uint8_t *>(word.data());
	const utf8proc_ssize_t size = static_cast<utf8proc_ssize_t>(word.size());
	for (utf8proc_ssize_t at = 0; at < size;) {
		utf8proc_int32_t point = 0;
		const utf8proc_ssize_t length = utf8proc_iterate(bytes + at, size - at, &point);
		if (length <= 0) {
			++at;
			continue;
		}
		result.push_back(static_cast<char32_t>(point));
		at += length;
	}
	return result;
}

// True when `after` is `before` spelt right: one letter wrong, missing, extra
// or two swapped ("recieve", "indemnfy", "teh"), two in a long word. Never a
// figure, a word that limits or a modal ("not", "shall" as "small"), nor a word
// and its opposite ("limited", "unlimited").
bool is_spelling_correction(const std::string &before, const std::string &after) {
	static const std::set<std::string> fixed = word_set_of({"not", "no", "nor", "never", "neither", "without",
		"except", "unless", "shall", "may", "must", "will", "should", "can", "could", "would", "might", "and", "or",
		"any", "all", "each", "every", "none", "only", "\320\275\320\265", "\320\275\320\270", "\320\275\320\265\321\202", "\320\261\320\265\320\267", "\320\270", "\320\270\320\273\320\270", "\320\274\320\276\320\266\320\265\321\202", "\320\264\320\276\320\273\320\266\320\265\320\275",
		"\320\264\320\276\320\273\320\266\320\275\320\260", "\320\264\320\276\320\273\320\266\320\275\320\276", "\320\264\320\276\320\273\320\266\320\275\321\213", "\320\262\320\277\321\200\320\260\320\262\320\265", "\320\276\320\261\321\217\320\267\320\260\320\275", "\320\276\320\261\321\217\320\267\320\260\320\275\320\260", "\320\276\320\261\321\217\320\267\320\260\320\275\320\276", "\320\276\320\261\321\217\320\267\320\260\320\275\321\213"});
	if (before == after) return true;
	if (fixed.count(before) > 0 || fixed.count(after) > 0) return false;
	const auto hasDigit = [](const std::string &word) { return word.find_first_of("0123456789") != std::string::npos; };
	if (hasDigit(before) || hasDigit(after)) return false;
	for (const char *prefix : {"un", "in", "im", "il", "ir", "non", "dis", "\320\275\320\265", "\320\261\320\265\320\267"})
		if (before == prefix + after || after == prefix + before) return false;
	const std::u32string was = letters_of(before);
	const std::u32string becomes = letters_of(after);
	const std::size_t shorter = std::min(was.size(), becomes.size());
	// Optimal string alignment distance: an edit, an insertion, a deletion or
	// two neighbours swapped, each one step.
	std::vector<std::vector<std::size_t>> distance(was.size() + 1, std::vector<std::size_t>(becomes.size() + 1));
	for (std::size_t i = 0; i <= was.size(); ++i) distance[i][0] = i;
	for (std::size_t j = 0; j <= becomes.size(); ++j) distance[0][j] = j;
	bool isSwap = false;
	for (std::size_t i = 1; i <= was.size(); ++i)
		for (std::size_t j = 1; j <= becomes.size(); ++j) {
			const std::size_t cost = was[i - 1] == becomes[j - 1] ? 0 : 1;
			distance[i][j] = std::min({distance[i - 1][j] + 1, distance[i][j - 1] + 1, distance[i - 1][j - 1] + cost});
			if (i > 1 && j > 1 && was[i - 1] == becomes[j - 2] && was[i - 2] == becomes[j - 1]
					&& distance[i - 2][j - 2] + 1 < distance[i][j]) {
				distance[i][j] = distance[i - 2][j - 2] + 1;
				if (i == was.size() && j == becomes.size()) isSwap = true;
			}
		}
	const std::size_t steps = distance[was.size()][becomes.size()];
	if (steps == 0) return true;
	// A short word changed by a letter is another word ("or", "of"); only two
	// of its letters swapped is a slip.
	if (shorter <= 3) return steps == 1 && isSwap && was.size() == becomes.size();
	return steps <= (shorter >= 8 ? 2u : 1u);
}

} // namespace

bool rewrites_substance(const std::string &before, const std::string &after) {
	const std::vector<std::string> was = words_of(before);
	const std::vector<std::string> becomes = words_of(after);
	if (was.empty()) return false;
	if (becomes.empty()) return true;
	static const std::set<std::string> articles = word_set_of({"the", "a", "an"});
	// The fewest changes of substance that turn `was` into `becomes`, where a
	// correction, an article added or dropped and a doubled word dropped are
	// none: it rewrites what the passage says when that is more than none.
	std::vector<std::vector<std::size_t>> cost(was.size() + 1, std::vector<std::size_t>(becomes.size() + 1));
	for (std::size_t i = 0; i <= was.size(); ++i)
		for (std::size_t j = 0; j <= becomes.size(); ++j) {
			if (i == 0 && j == 0) continue;
			std::size_t best = std::numeric_limits<std::size_t>::max();
			if (i > 0) {
				const bool isFree = articles.count(was[i - 1]) > 0 || (i > 1 && was[i - 1] == was[i - 2]);
				best = std::min(best, cost[i - 1][j] + (isFree ? 0 : 1));
			}
			if (j > 0) best = std::min(best, cost[i][j - 1] + (articles.count(becomes[j - 1]) > 0 ? 0 : 1));
			if (i > 0 && j > 0)
				best = std::min(best, cost[i - 1][j - 1] + (is_spelling_correction(was[i - 1], becomes[j - 1]) ? 0 : 1));
			cost[i][j] = best;
		}
	return cost[was.size()][becomes.size()] > 0;
}

void ToolBroker::register_tool(const ToolDefinition &definition) {
  if (definition.id.empty() || is_canonical_capability(definition.capability) == false
    || definition.effect == ToolEffectClass::Unclassified || definition.execute == nullptr) {
    throw std::invalid_argument("A tool definition is incomplete.");
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  definitions_[definition.id] = definition;
}

void ToolBroker::unregister_tool(const std::string &toolId) {
  const std::lock_guard<std::mutex> lock(mutex_);
  definitions_.erase(toolId);
}

void ToolBroker::clear_registered_tools_for_testing() {
  const std::lock_guard<std::mutex> lock(mutex_);
  definitions_.clear();
  pending_.clear();
	invalidated_operations_.clear();
	operation_grants_.clear();
	prior_decisions_.clear();
	held_changes_.clear();
}

ToolAttempt ToolBroker::request(const ToolExecutionContext &context, const ToolIntent &intent) {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		if (invalidated_operations_.count({context.project_id, context.operation_id}) > 0) {
			ToolAttempt attempt;
			attempt.result.outcome = ToolOutcome::Denied;
			attempt.result.error_category = "operation_stopped";
			attempt.evidence.tool_id = intent.tool_id;
			attempt.evidence.outcome = ToolOutcome::Denied;
			attempt.evidence.error_category = attempt.result.error_category;
			return attempt;
		}
	}
  ToolDefinition definition;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = definitions_.find(intent.tool_id);
    if (found == definitions_.end()) {
      ToolAttempt attempt;
      attempt.result.outcome = ToolOutcome::Failed;
      attempt.result.error_category = "unknown_tool";
      attempt.evidence.id = random_id("tce_");
      attempt.evidence.tool_id = intent.tool_id;
      attempt.evidence.capability = "unknown";
      attempt.evidence.effect = ToolEffectClass::Unclassified;
      attempt.evidence.outcome = ToolOutcome::Failed;
      attempt.evidence.effective_mode = context.permission.effective_mode;
      attempt.evidence.started_at = current_timestamp();
      attempt.evidence.completed_at = attempt.evidence.started_at;
      const nlohmann::json normalized{
        {"arguments", intent.arguments}, {"registered", false}, {"toolId", intent.tool_id}};
      attempt.evidence.normalized_intent_digest = platform::sha256_hex_digest(normalized.dump());
      attempt.evidence.error_category = attempt.result.error_category;
      return attempt;
    }
    definition = found->second;
  }

	const ToolEffectClass effect = effect_of_call(context, definition, intent);
  ToolCallEvidence evidence = evidence_for(context, definition, intent);
	evidence.effect = effect;
  PermissionDisposition disposition = permission_for(context.permission, effect);
  if (disposition == PermissionDisposition::Execute && definition.always_asks
      && definition.always_asks(context, intent.arguments))
    disposition = PermissionDisposition::RequireApproval;
	// The lawyer asked in this request to see its changes first: no mode lets
	// one through without them.
	if (disposition == PermissionDisposition::Execute && is_project_mutation(effect)
			&& changes_held(context.project_id, context.operation_id))
		disposition = PermissionDisposition::RequireApproval;
  if (disposition == PermissionDisposition::Deny) {
    ToolAttempt attempt;
    attempt.result.outcome = ToolOutcome::Denied;
    attempt.result.error_category = "organization_policy_denied";
    attempt.evidence = std::move(evidence);
    attempt.evidence.outcome = ToolOutcome::Denied;
    attempt.evidence.error_category = attempt.result.error_category;
    attempt.evidence.completed_at = current_timestamp();
    return attempt;
  }
  if (disposition == PermissionDisposition::Execute) {
    return execute_definition(context, definition, intent, std::move(evidence));
  }
  // A decision the person already gave on this exact call, while the run that
  // first made it was gone: it is not asked again.
  std::optional<PriorDecision> prior;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto decisions = prior_decisions_.find({context.project_id, context.operation_id});
    if (decisions != prior_decisions_.end()) {
      for (auto entry = decisions->second.begin(); entry != decisions->second.end(); ++entry) {
        if (entry->tool_id != definition.id || entry->intent_digest != evidence.normalized_intent_digest) continue;
        prior = *entry;
        decisions->second.erase(entry);
        break;
      }
      if (decisions->second.empty()) prior_decisions_.erase(decisions);
    }
  }
  if (prior) {
    evidence.explicit_approval_required = true;
    evidence.explicit_approval_granted = prior->is_allowed;
    if (prior->is_allowed == false) {
      ToolAttempt attempt;
      attempt.result.outcome = ToolOutcome::Denied;
      attempt.result.error_category = "approval_denied";
      attempt.evidence = std::move(evidence);
      attempt.evidence.outcome = ToolOutcome::Denied;
      attempt.evidence.error_category = attempt.result.error_category;
      attempt.evidence.completed_at = current_timestamp();
      return attempt;
    }
    ToolIntent decided = intent;
    if (definition.takes_response && decided.arguments.is_object()) decided.arguments["answers"] = prior->response;
    return execute_definition(context, definition, decided, std::move(evidence));
  }
  // "Allow for this request": the person already allowed calls of this effect
  // for the rest of this operation, so this one is not put to them again. It
  // stays an explicitly approved call on its record.
  bool isGrantedForOperation = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto granted = operation_grants_.find({context.project_id, context.operation_id});
    isGrantedForOperation = granted != operation_grants_.end() && granted->second.count(effect) > 0;
  }
  if (isGrantedForOperation == true && effect == ToolEffectClass::ExternalNetwork) {
    evidence.explicit_approval_required = true;
    evidence.explicit_approval_granted = true;
    return execute_definition(context, definition, intent, std::move(evidence));
  }
  // Asked only about a call that could be carried out: one its own tool
  // refuses is refused now, never shown to a person as something to approve.
  try {
    if (std::optional<ToolAttempt> refused = validation_refusal(context, definition, intent, evidence))
      return *refused;
  } catch (const std::exception &) {
    ToolAttempt attempt;
    attempt.evidence = std::move(evidence);
    attempt.result.outcome = ToolOutcome::Failed;
    attempt.result.error_category = "handler_execution_failed";
    attempt.evidence.outcome = ToolOutcome::Failed;
    attempt.evidence.error_category = attempt.result.error_category;
    attempt.evidence.completed_at = current_timestamp();
    return attempt;
  }

  ToolApprovalRequest approval;
  approval.approval_id = random_id("tap_");
  approval.operation_id = context.operation_id;
  approval.project_id = context.project_id;
  approval.summary = definition.approval_summary;
  approval.effect = effect;
  approval.tool_id = definition.id;
  approval.intent_digest = evidence.normalized_intent_digest;
  if (definition.describe_approval_target) {
    try {
      definition.describe_approval_target(context, intent.arguments, approval);
    } catch (const std::exception &) {
      approval.target_document_id.reset();
      approval.target_version.reset();
      approval.target_fingerprint.reset();
      approval.change_kind.clear();
      approval.removed_text.clear();
      approval.added_text.clear();
      approval.change_detail_json.clear();
      approval.question_json.clear();
    }
  }
  evidence.outcome = ToolOutcome::ApprovalRequired;
  evidence.explicit_approval_required = true;
  PendingApproval pending;
  pending.request = approval;
  pending.context = context;
  pending.definition = definition;
  pending.intent = intent;
  pending.evidence = evidence;
  pending.filed_at = std::chrono::steady_clock::now();
  // Checked again where the approval is filed, under the same lock: a Stop
  // that arrived since the check above would otherwise find nothing to wake,
  // and the run would wait on this approval for ever.
  bool stopped = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopped = invalidated_operations_.count({context.project_id, context.operation_id}) > 0;
    pending.is_run_tracked = running_operations_.count({context.project_id, context.operation_id}) > 0;
    erase_answered_locked();
    if (!stopped) pending_[approval.approval_id] = std::move(pending);
  }
  if (stopped) {
    ToolAttempt attempt;
    attempt.result.outcome = ToolOutcome::Denied;
    attempt.result.error_category = "operation_stopped";
    attempt.evidence = std::move(evidence);
    attempt.evidence.outcome = ToolOutcome::Denied;
    attempt.evidence.error_category = attempt.result.error_category;
    return attempt;
  }
	if (context.approval_requested) {
		nlohmann::json saved{{"approvalId", approval.approval_id},
			{"operationId", approval.operation_id}, {"projectId", approval.project_id},
			{"summary", approval.summary}, {"effect", to_string(approval.effect)},
			{"toolId", approval.tool_id}, {"intentDigest", approval.intent_digest}};
		if (approval.target_document_id) saved["targetDocumentId"] = *approval.target_document_id;
		if (approval.target_version) saved["targetVersion"] = *approval.target_version;
		if (approval.target_fingerprint) saved["targetFingerprint"] = *approval.target_fingerprint;
		saved["changeKind"] = approval.change_kind;
		saved["removedText"] = approval.removed_text;
		saved["addedText"] = approval.added_text;
		saved["columns"] = approval.columns;
		saved["rows"] = approval.rows;
		if (!approval.change_detail_json.empty()) {
			const nlohmann::json change = nlohmann::json::parse(approval.change_detail_json, nullptr, false);
			if (change.is_object()) saved["change"] = change;
		}
		if (!approval.question_json.empty()) {
			const nlohmann::json question = nlohmann::json::parse(approval.question_json, nullptr, false);
			if (question.is_object()) saved["question"] = question;
		}
		context.approval_requested(saved.dump());
	}
  ToolAttempt attempt;
  attempt.result.outcome = ToolOutcome::ApprovalRequired;
  attempt.evidence = std::move(evidence);
  attempt.approval = std::move(approval);
  return attempt;
}

std::optional<ToolAttempt> ToolBroker::validate(const ToolExecutionContext &context, const ToolIntent &intent) {
  ToolDefinition definition;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = definitions_.find(intent.tool_id);
    if (found == definitions_.end()) return std::nullopt;
    definition = found->second;
  }
  ToolCallEvidence evidence = evidence_for(context, definition, intent);
  try {
    return validation_refusal(context, definition, intent, evidence);
  } catch (const std::exception &) {
    ToolAttempt attempt;
    attempt.evidence = std::move(evidence);
    attempt.result.outcome = ToolOutcome::Failed;
    attempt.result.error_category = "handler_execution_failed";
    attempt.evidence.outcome = ToolOutcome::Failed;
    attempt.evidence.error_category = attempt.result.error_category;
    attempt.evidence.completed_at = current_timestamp();
    return attempt;
  }
}

ToolAttempt ToolBroker::wait_for_approval(const std::string &approvalId) {
  std::unique_lock<std::mutex> lock(mutex_);
  {
    const auto waited = pending_.find(approvalId);
    if (waited != pending_.end()) ++waited->second.waiters;
  }
  changed_.wait(lock, [&] {
    const auto found = pending_.find(approvalId);
    return found == pending_.end() || found->second.is_decided == true;
  });
  return take_decision(lock, approvalId);
}

std::optional<ToolAttempt> ToolBroker::wait_for_approval_for(const std::string &approvalId,
    std::chrono::milliseconds limit) {
  std::unique_lock<std::mutex> lock(mutex_);
  {
    const auto waited = pending_.find(approvalId);
    if (waited != pending_.end()) ++waited->second.waiters;
  }
  const bool isDecided = changed_.wait_for(lock, limit, [&] {
    const auto found = pending_.find(approvalId);
    return found == pending_.end() || found->second.is_decided == true;
  });
  if (!isDecided) {
    // Withdrawn: no thread waits on it any more.
    pending_.erase(approvalId);
    return std::nullopt;
  }
  return take_decision(lock, approvalId);
}

ToolAttempt ToolBroker::take_decision(std::unique_lock<std::mutex> &lock, const std::string &approvalId) {
  const auto found = pending_.find(approvalId);
  if (found == pending_.end()) {
    ToolAttempt attempt;
    attempt.result.outcome = ToolOutcome::Denied;
    attempt.result.error_category = "approval_invalidated";
    return attempt;
  }
  PendingApproval pending = found->second;
  pending_.erase(found);
  lock.unlock();

  pending.evidence.explicit_approval_granted = pending.is_allowed;
  if (pending.is_allowed == false || pending.is_invalidated == true) {
    ToolAttempt attempt;
    attempt.result.outcome = ToolOutcome::Denied;
    attempt.result.error_category = pending.is_invalidated ? "approval_invalidated" : "approval_denied";
    attempt.evidence = std::move(pending.evidence);
    attempt.evidence.outcome = ToolOutcome::Denied;
    attempt.evidence.error_category = attempt.result.error_category;
    attempt.evidence.completed_at = current_timestamp();
    return attempt;
  }
  // The person's reply is what the tool was waiting for; it reaches the tool
  // beside the arguments it was asked with, and nothing else of them changes.
  if (pending.definition.takes_response && pending.intent.arguments.is_object())
    pending.intent.arguments["answers"] = pending.response;
  return execute_definition(pending.context, pending.definition, pending.intent,
    std::move(pending.evidence));
}

ToolAttempt ToolBroker::execute_preapproved(const ToolExecutionContext &context, const ToolIntent &intent) {
  ToolDefinition definition;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = definitions_.find(intent.tool_id);
    if (found == definitions_.end()) {
      ToolAttempt attempt;
      attempt.result.outcome = ToolOutcome::Failed;
      attempt.result.error_category = "unknown_tool";
      attempt.evidence.tool_id = intent.tool_id;
      attempt.evidence.outcome = ToolOutcome::Failed;
      attempt.evidence.error_category = attempt.result.error_category;
      return attempt;
    }
    definition = found->second;
  }
  ToolCallEvidence evidence = evidence_for(context, definition, intent);
	evidence.effect = effect_of_call(context, definition, intent);
  // The caller already holds durable, canonical proof (MatterTask's own
  // document_operation_checkpoint.draft - never ToolBroker::pending_, which
  // is in-process memory only and does not survive a real restart) that a
  // human explicitly authorized this exact action - see this method's own
  // header comment for why ToolBroker itself is never that proof's source
  // of truth. That approval answers the Project's own "ask first" question,
  // and only that one: what the organization does not permit on this device
  // at all is still refused, however the person answered.
  if (permission_for(context.permission, definition.effect) == PermissionDisposition::Deny) {
    ToolAttempt attempt;
    attempt.result.outcome = ToolOutcome::Denied;
    attempt.result.error_category = "organization_policy_denied";
    attempt.result.error_message = context.permission.cap_reason.empty()
      ? "Your organization does not allow this action on this device." : context.permission.cap_reason;
    attempt.evidence = std::move(evidence);
    attempt.evidence.explicit_approval_granted = true;
    attempt.evidence.outcome = ToolOutcome::Denied;
    attempt.evidence.error_category = attempt.result.error_category;
    attempt.evidence.completed_at = current_timestamp();
    return attempt;
  }
  evidence.explicit_approval_granted = true;
  return execute_definition(context, definition, intent, std::move(evidence));
}

bool ToolBroker::respond_to_approval(const std::string &projectId, const std::string &operationId,
    const std::string &approvalId, bool isAllowed, const nlohmann::json &response, bool isForOperation) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pending_.find(approvalId);
    if (found == pending_.end() || found->second.is_decided == true
      || found->second.context.project_id != projectId
      || found->second.context.operation_id != operationId) {
      return false;
    }
    if (response.is_null() == false && found->second.definition.takes_response == false) return false;
    // Only web research can be allowed for a whole request; a document change
    // or a question is answered one at a time.
    if (isForOperation == true && (isAllowed == false
        || found->second.definition.effect != ToolEffectClass::ExternalNetwork)) return false;
    found->second.is_decided = true;
    found->second.is_allowed = isAllowed;
    found->second.response = response;
    if (isForOperation == true)
      operation_grants_[{projectId, operationId}].insert(found->second.definition.effect);
    // Nothing waits on an approval whose run filed it and ended: once it is
    // answered it is only memory, and it goes now. One a run is about to wait
    // on stays until that run has read its answer - however long the run takes
    // to get there (it may be waiting on a save of another Project).
    if (is_abandoned_locked(found->second, std::chrono::steady_clock::now())) pending_.erase(found);
    erase_answered_locked();
  }
  changed_.notify_all();
  return true;
}

std::set<ToolEffectClass> ToolBroker::operation_grants(const std::string &projectId,
    const std::string &operationId) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto found = operation_grants_.find({projectId, operationId});
  return found == operation_grants_.end() ? std::set<ToolEffectClass>() : found->second;
}

void ToolBroker::record_prior_decision(const std::string &projectId, const std::string &operationId,
    const std::string &toolId, const std::string &intentDigest, bool isAllowed, const nlohmann::json &response,
    bool isForOperation) {
  const std::lock_guard<std::mutex> lock(mutex_);
  prior_decisions_[{projectId, operationId}].push_back(
    PriorDecision{toolId, intentDigest, isAllowed, response, isForOperation});
  // "Allow for this request", given while the run was gone, allows the rest of
  // the resumed run's web research exactly as respond_to_approval() would
  // have. Only web research is ever allowed so.
  const auto definition = definitions_.find(toolId);
  if (isForOperation == true && isAllowed == true && definition != definitions_.end()
      && definition->second.effect == ToolEffectClass::ExternalNetwork)
    operation_grants_[{projectId, operationId}].insert(ToolEffectClass::ExternalNetwork);
}

void ToolBroker::begin_operation(const std::string &projectId, const std::string &operationId) {
  const std::lock_guard<std::mutex> lock(mutex_);
  invalidated_operations_.erase({projectId, operationId});
  running_operations_.insert({projectId, operationId});
}

void ToolBroker::end_operation(const std::string &projectId, const std::string &operationId) {
  const std::lock_guard<std::mutex> lock(mutex_);
  operation_grants_.erase({projectId, operationId});
  prior_decisions_.erase({projectId, operationId});
  running_operations_.erase({projectId, operationId});
	held_changes_.erase({projectId, operationId});
  erase_answered_locked();
}

void ToolBroker::hold_changes(const std::string &projectId, const std::string &operationId) {
	const std::lock_guard<std::mutex> lock(mutex_);
	held_changes_.insert({projectId, operationId});
}

void ToolBroker::release_changes(const std::string &projectId, const std::string &operationId) {
	const std::lock_guard<std::mutex> lock(mutex_);
	held_changes_.erase({projectId, operationId});
}

bool ToolBroker::changes_held(const std::string &projectId, const std::string &operationId) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	return held_changes_.count({projectId, operationId}) > 0;
}

std::size_t ToolBroker::held_approvals_for_testing() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return pending_.size();
}

bool ToolBroker::is_abandoned_locked(const PendingApproval &pending,
    std::chrono::steady_clock::time_point now) const {
  if (pending.is_decided == false || pending.waiters > 0) return false;
  // A run that filed it and is still going may yet come to read it; one that
  // ended never will.
  if (pending.is_run_tracked == true)
    return running_operations_.count({pending.context.project_id, pending.context.operation_id}) == 0;
  return now - pending.filed_at >= waiter_arrival_grace();
}

void ToolBroker::erase_answered_locked() {
  const auto now = std::chrono::steady_clock::now();
  for (auto entry = pending_.begin(); entry != pending_.end();) {
    if (is_abandoned_locked(entry->second, now)) entry = pending_.erase(entry);
    else ++entry;
  }
}

void ToolBroker::invalidate_operation(const std::string &projectId, const std::string &operationId) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
	invalidated_operations_.insert({projectId, operationId});
	operation_grants_.erase({projectId, operationId});
	prior_decisions_.erase({projectId, operationId});
	erase_answered_locked();
    for (auto &entry : pending_) {
      PendingApproval &pending = entry.second;
      if (pending.context.project_id != projectId || pending.context.operation_id != operationId
        || pending.is_decided == true) continue;
      pending.is_decided = true;
      pending.is_allowed = false;
      pending.is_invalidated = true;
    }
  }
  changed_.notify_all();
}

void ToolBroker::reset_operation(const std::string &projectId, const std::string &operationId) {
	const std::lock_guard<std::mutex> lock(mutex_);
	invalidated_operations_.erase({projectId, operationId});
}

bool ToolBroker::operation_invalidated(const std::string &projectId, const std::string &operationId) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	return invalidated_operations_.count({projectId, operationId}) > 0;
}

std::optional<ToolApprovalRequest> ToolBroker::pending_approval(const std::string &projectId,
    const std::string &operationId) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &entry : pending_) {
    const PendingApproval &pending = entry.second;
    if (pending.context.project_id == projectId && pending.context.operation_id == operationId
      && pending.is_decided == false) return pending.request;
  }
  return std::nullopt;
}

bool ToolBroker::is_awaited(const std::string &approvalId) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto found = pending_.find(approvalId);
  return found != pending_.end() && found->second.waiters > 0;
}

ToolBroker &production_tool_broker() {
  static ToolBroker broker;
  return broker;
}

} // namespace singularity::tools
