#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace singularity::document_intelligence {

// The facts the documents of one Matter share, checked against each other on
// the lawyer's computer (D-119): a price in a tracker against the schedule of
// the agreement it tracks, a figure on a slide against the notes of the call
// that settled it, a period an email states against the draft it sends, a
// clause a tracker cites against the agreement's own numbering. The check
// reads the real values, so it holds where Papinian Guard keeps them from the
// model; and it reads every value, so a fact no one looked at is never taken
// as correct: a value the check ties to no source is unverified, and says so.
//
// Nothing here reads a file or calls a model. A document is given as its
// statements - its clauses, its table's rows, its cells, its slides' lines -
// and each value in them is read by SemanticAlgebra, the reading Papinian
// Guard computes with: "$1.85 million" and 1,850,000 under "Annual value
// (USD)" are one value, "sixty (60) days" and "60 days" one period.

// A value a statement gives.
struct StatedValue {
	// "money", "number", "percent", "period", "date" or "reference".
	std::string kind;
	// The same for every writing of one value: "MONEY:USD:412", "NUM:3",
	// "DUR:Day:60", "DATE:D:20261215", "REF:section:16.3",
	// "REF:section:3.12(a):item:3", "MULT:2" (two times).
	std::string key;
	std::string text;
	std::size_t start = 0;
	std::size_t length = 0;
	// The statement sets it aside rather than gives it: "about $1.85 million,
	// not $1.58 million as the deck says".
	bool isSetAside = false;
	// One end of a span of days - December 1 of "December 1 to 8, 2026" -
	// which may stand for one thing the span covers and not another, so a
	// source can bear it out but never be said to differ from it.
	bool isBound = false;
};

// Every value `text` gives, left to right. `label` is what the text stands
// under - a cell's column header, a table's header row: a bare figure under
// "Unit price (USD)" is money, under "Term (years)" a period of years. Where
// there is a label, a bare "12.2" or "(17.3)" is the clause it cites.
std::vector<StatedValue> valuesIn(const std::string& text, const std::string& label = std::string());

// One value however it is written: the same kind and amount - money in one
// currency, or one of the two naming none; a bare figure and an amount alike;
// a period in days and weeks, or months and years, converted - the same day
// or period of days, the same place in a document.
bool isSameValue(const StatedValue& left, const StatedValue& right);

// A place in a document that states something.
struct Statement {
	std::string documentId;
	std::string documentName;
	// Where a person finds it: "Contracts!G2", "Section 5.2", "Schedule 1,
	// CV-220", "Slide 2", "Line 4".
	std::string place;
	// What the document says it is: a cell's column header, a table's header
	// row, a clause's heading.
	std::string label;
	// What it is about: a cell's row, a slide's title, the document's name.
	std::string subject;
	std::string text;
	// "source": what settles a fact - an agreement and its schedules, a letter,
	// a consent, notes of what a party said. "summary": what restates facts - a
	// tracker, a deck, an email. A summary is checked against both.
	std::string standing = "source";
	// For a Word agreement's words, whether they are agreed: "agreed" where no
	// tracked change is open in them, else which open changes they hold or would
	// lose, and whose (D-119). Empty for any other document.
	std::string stand;
	// A cell Excel calculates: what it shows follows from other cells, so it is
	// neither checked nor cited.
	bool isCalculated = false;
	// Its values, read once (readValues) so a document checked many times is
	// read once; read on the spot where they are not.
	std::vector<StatedValue> values;
	bool isRead = false;
};

// Reads a statement's values into it, once.
void readValues(Statement& statement);

// The statements of a text read line by line - a letter, notes, a PDF, an
// email - each line its own, with what the text's first line says it is.
std::vector<Statement> statementsOfLines(const std::string& text, const std::string& documentId,
	const std::string& documentName, const std::string& standing);

// What a source says of a value, word for word.
struct FactEvidence {
	std::string documentId;
	std::string documentName;
	std::string place;
	// The source's words, as it gives them.
	std::string quote;
	// The value it gives, as it writes it.
	std::string value;
	bool isSame = false;
	// Where the source is a Word agreement, whether its words are agreed
	// (Statement::stand).
	std::string stand;
};

struct FactCheck {
	// The value checked, and where it stands.
	std::string documentId;
	std::string documentName;
	std::string place;
	std::string label;
	std::string quote;
	StatedValue value;
	// "agrees": a source gives the same value for the same thing. "differs": the
	// source that names it best gives another. "unclear": the sources that name
	// it give several values, none of them this one. "unsourced": no document
	// names it, so it is unverified - never taken as correct.
	std::string state;
	// Best first; at most three.
	std::vector<FactEvidence> evidence;
	// How closely the best source names the same thing, for ordering.
	double strength = 0.0;
};

// Each value of `targets` checked against `sources` - never against its own
// document. A reference is not checked here: see checkReferences.
std::vector<FactCheck> checkFacts(const std::vector<Statement>& targets, const std::vector<Statement>& sources);

// The places of a document a reference can cite: its numbered clauses, and
// the places its headings name ("Section 3.12(a) - Material Contracts"), each
// with the numbered items under it.
struct FactPlace {
	// "16.3", "3.12(a)", "schedule:1".
	std::string key;
	std::string words;
	std::vector<std::string> items;
};
struct FactDocument {
	std::string documentId;
	std::string documentName;
	std::vector<FactPlace> places;
};

// Each clause a target cites checked against the document it cites: a clause
// the document does not have ("17.3" where the agreement ends at 16), or an
// item that names something else than the statement it is cited for ("Schedule
// 3.12(a), item 3" for the Ardent agreement, where item 3 is another contract).
// The document cited is the one the statement names - a tracker's row by the
// agreement it tracks - or, for a place only one document has, that document.
std::vector<FactCheck> checkReferences(const std::vector<Statement>& targets, const std::vector<FactDocument>& documents);

// Where a value a colleague writes comes from: a source that gives it for the
// same thing, else any text the work was given that gives it (the lawyer's
// words, the Playbook, the clause before the change). Nothing when none does:
// the value is then unsourced.
struct GivenText {
	// "the instruction", "the Playbook", "the clause before the change".
	std::string name;
	std::string text;
};
struct ValueOrigin {
	StatedValue value;
	// "sourced" or "unsourced".
	std::string state;
	FactEvidence evidence;
};
std::vector<ValueOrigin> originsOfNewValues(const std::string& before, const std::string& after, const std::string& label,
	const std::vector<Statement>& sources, const std::vector<GivenText>& given);

// A check as a lawyer, or a colleague, reads it: "Contracts!G2 (Unit price
// CV-220 (USD)) gives 421.00; Halvane Supply Agreement, Schedule 1, CV-220
// gives $412.00: “CV-220 | Servo actuator | $412.00”."
std::string checkWords(const FactCheck& check);

} // namespace singularity::document_intelligence
