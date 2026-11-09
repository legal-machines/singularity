#pragma once

#include "Context/TemporalSemantics/TemporalSemantics.hpp"

#include <optional>
#include <string>
#include <vector>

// The order of the documents a request reads, worked out on the lawyer's
// computer and given to the model as relations, never as dates.
//
// Papinian Guard keeps a Project's dates from a provider's model, and a model
// that cannot tell the February draft from the March signed agreement reads the
// draft as the newer position (live runs, 24 Sep 2026). So Singularity reads each
// supplied document's own standing - a draft, the signed agreement, an
// amendment, a board record - and its own date, from its folder, name, title
// and text, and states only what follows: "DOC_2 is an earlier draft of DOC_5",
// "DOC_5 precedes DOC_1". The provider receives the relations; the dates, their
// years, months and the time between them stay here.
//
// Only the supplied documents' own dates are read - the date that dates each
// document - so a passport's issue date, an invoice date or a birthday inside a
// document never becomes a relation. Where precision does not settle an order
// ("March 2026" against "3 March 2026") or a document's name and text disagree
// on its date, the context says so and orders nothing.
namespace singularity::context::temporal_context {

// What a document is, as its folder, name, title and text show it. Signed only
// where the text shows a signature given ("/s/ Dana Morgan", "By: Dana Morgan",
// «…»), never from a line left to sign on, which
// a draft has too. Final: no draft's marks and no blank anywhere, and every
// line to sign on names its signatory («…») - the form an
// executed agreement is kept in as text. One that shows none of these is
// Agreement or Amendment - no draft relation rests on it.
enum class Standing { Draft, Signed, Final, Agreement, DraftAmendment, SignedAmendment, FinalAmendment, Amendment, BoardRecord,
	Correspondence, Note, Register, Unknown };

// One supplied document, as the request knows it.
struct SuppliedDocument {
	std::string handle;        // "DOC_2" - the only way the model knows it
	std::string document_id;
	std::string relative_path; // "Previous Drafts/Northwind MSA - Draft of February 10, 2026.docx"
	std::string text;          // its reading (the extraction's text)
};

// Where a document's date came from, for provenance.
struct DatingEvidence {
	std::string source;        // "text" or "name"
	std::string written;       // the date as written there
};

struct DocumentDating {
	Standing standing = Standing::Unknown;
	std::optional<temporal::TemporalValue> date;
	std::vector<DatingEvidence> evidence;
	// The name and the text date the document differently: its order is not known.
	bool conflicting = false;
	// What the document is an instance of, for "an earlier draft of": its
	// title with its status and dates taken away ("master services agreement").
	std::string family;
};

[[nodiscard]] DocumentDating date_document(const SuppliedDocument &document);

// One relation the model is given, with what it rests on - kept on the
// operation, never sent.
struct TemporalRelation {
	std::string statement;                  // exactly as sent: "DOC_2 precedes DOC_5."
	std::vector<std::string> document_ids;  // the documents it relates
	std::vector<DatingEvidence> evidence;   // the dates it was worked out from
};

struct TemporalContextResult {
	// The section as the provider receives it; empty when fewer than two
	// supplied documents can be placed.
	std::string text;
	std::vector<TemporalRelation> relations;
};

// The relations between `documents` - the ones this request reads - in an
// order that never depends on the order they were supplied in.
[[nodiscard]] TemporalContextResult derive(const std::vector<SuppliedDocument> &documents);

// The same from documents already dated, for a caller that relates different
// subsets of one request's documents (the prompt's budget tries several).
struct DatedDocument {
	SuppliedDocument document;
	DocumentDating dating;
};
[[nodiscard]] std::vector<DatedDocument> date_documents(const std::vector<SuppliedDocument> &documents);
[[nodiscard]] TemporalContextResult derive(const std::vector<DatedDocument> &documents);

} // namespace singularity::context::temporal_context
