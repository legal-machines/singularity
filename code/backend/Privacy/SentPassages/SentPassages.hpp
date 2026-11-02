#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "PapinianGuard/Types/PrivacyMetadata.hpp"

// What a protected request really sent, read back beside the Project's own
// text: which words of a passage were hidden, and which marker went in their
// place. Re-derived on this device each time from two things Singularity keeps for
// their own reasons - the passage as it left (Routing/RoutingLog SentPassages)
// and the local document - so what a marker stood for is never itself stored.
// Pure: no file, no Project, no model.
namespace singularity::privacy::sent_passages {

// One marker of an operation's protected requests, as the lawyer reads it.
struct MarkerFact {
	// The protection table the marker was numbered by (OutboundCall::
	// symbol_scope): "[[E1]]" of one table and "[[E1]]" of another are two
	// facts - a source picker's request and the task's own, or a task resumed
	// after a restart, each number afresh.
	std::string scope;
	// "[[E4]]": the marker a request's mapping names the value by.
	std::string marker;
	std::string category;
	// "Person 2", "Company 1", "Amount 1": numbered by kind, in the order the
	// operation's requests first sent them - the same in every view. One
	// value is one label: markers of different tables for the same Project
	// entity share it, and nothing else does.
	std::string label;
	std::string entity_id;
	// The Project's own name for the entity the marker stood for, when the
	// Entity Registry links one.
	std::string entity_name;
	// Spellings of the value known on this device (the entity's name and
	// aliases, what another passage of the operation showed it as): used only
	// to line text up, never shown as what a passage says.
	std::vector<std::string> aliases;
};

// "[[E4.last]]" and "[[E4]]" are both "[[E4]]"; anything else is itself.
std::string base_marker(const std::string &written);

// "Person", "Company", "Amount" - what a category is called in a label.
std::string category_noun(const std::string &category);

// The markers the model calls of `calls` sent, each once per table, labelled
// by kind in the order first sent.
std::vector<MarkerFact> marker_facts(const std::vector<protected_api::papinian::OutboundCall> &calls);

// The facts of one table: the only ones a passage sent under it is read with.
std::vector<MarkerFact> facts_in_scope(const std::vector<MarkerFact> &facts, const std::string &scope);

// The fact that names `marker` (a base marker), or null. `facts` are one
// table's (facts_in_scope).
const MarkerFact *find_fact(const std::vector<MarkerFact> &facts, const std::string &marker);

// One run of a passage. In the sent form a hidden run is a marker, exactly as
// written; beside the local text it is the original words the marker took the
// place of.
struct Segment {
	std::string text;
	bool is_hidden = false;
	// The marker exactly as it left ("[[E4]]", "[[E4.last]]").
	std::string sent_as;
	// Its base marker, which names its MarkerFact.
	std::string marker;
	// The value's readable label and kind ("Person 2", "PERSON").
	std::string label;
	std::string category;
};

// The sent text cut into kept text and markers. Only a marker the operation
// really sent is one: "[[x]]" a document itself holds stays text.
std::vector<Segment> sent_segments(const std::string &sent, const std::vector<MarkerFact> &facts);

enum class Fit {
	// Every word the request kept stands in the local text in order, and one
	// reading of what each marker took the place of is better than any other.
	Aligned,
	// The words line up, but more than one reading of the markers fits, and
	// the best is not settled: a marker whose words differ between readings
	// is read only where a known spelling of its value supports it.
	Ambiguous,
	// The local text is not the text the passage was sent from.
	Mismatch,
};

struct Alignment {
	Fit fit = Fit::Mismatch;
	// The local text, with each hidden original as its own run. Empty unless
	// the fit is Aligned.
	std::vector<Segment> segments;
};

// Lines `sent` up against `local`, the text it was built from.
Alignment align(const std::string &local, const std::string &sent, const std::vector<MarkerFact> &facts);

// Whether `local` is the text a passage was sent from: its digest, or that of
// the text as a rendering escapes it (SourceMaterialSpans' as_prompt_text()),
// is `content_sha256`. The form that matched, when one did.
std::optional<std::string> built_from(const std::string &content_sha256, const std::string &local);

// One value a passage, or an operation, hid.
struct HiddenValue {
	std::string marker;
	std::string category;
	std::string label;
	std::string entity_id;
	std::string entity_name;
	// Every way the marker was written in what left ("[[E4]]", "[[E4.last]]").
	std::vector<std::string> sent_as;
	// The original words, each once, as the local text has them; empty when no
	// local text lines up.
	std::vector<std::string> originals;
	// Why no original is shown, when none is and the Project names no entity
	// for it: "changed" (the text it came from has changed since), "unavailable"
	// (that text cannot be read on this device now), "unmatched" (what left does
	// not line up word for word with it) or "not_kept" (it left in a part of the
	// request Singularity does not keep word for word). Empty otherwise.
	std::string missing;
};

// One value for each fact, in label order, with no originals yet.
std::vector<HiddenValue> fact_values(const std::vector<MarkerFact> &facts);

// The values `sent` hid, in the order they appear, with their originals from
// `aligned` when it lined up (else none).
std::vector<HiddenValue> hidden_values(const std::vector<Segment> &sent, const std::vector<Segment> &aligned,
		const std::vector<MarkerFact> &facts);

// `into` with `more` added: a value already there (the same label) gains the
// ways it was written and its originals; a new one is appended.
void merge_hidden_values(std::vector<HiddenValue> &into, const std::vector<HiddenValue> &more);

// Every original `values` found, added to the spellings of every fact of the
// same value (the same label):
// a passage that lined up shows how a value is written, and helps another
// passage of the same operation line up.
void learn_spellings(std::vector<MarkerFact> &facts, const std::vector<HiddenValue> &values);

// How a recorded passage reads beside the Project's text now.
enum class Reading {
	// Lined up: every hidden value has its original.
	Aligned,
	// The words line up, but which words each marker hid cannot be told apart.
	Ambiguous,
	// The local text is not the text the passage was sent from: the document
	// changed, or the passage is not the one it was.
	Changed,
	// The local text is the text it was sent from, yet the sent form does not
	// line up with it.
	Unaligned,
	// No local text could be read.
	Unavailable,
};

const char *to_string(Reading reading);

struct PassageReading {
	Reading reading = Reading::Unavailable;
	std::vector<Segment> sent;
	// Set only when Aligned.
	std::vector<Segment> local;
	std::vector<HiddenValue> hidden;
};

// One recorded passage read beside `local` (nullopt: no local text).
PassageReading read_passage(const std::string &sent, const std::string &content_sha256,
		const std::optional<std::string> &local, const std::vector<MarkerFact> &facts);

} // namespace singularity::privacy::sent_passages
