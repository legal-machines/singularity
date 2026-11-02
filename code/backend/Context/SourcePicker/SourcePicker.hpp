#pragma once

#include "Matter/Types/Matter.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Which of the Project's documents a request needs - decided by the model
// from a LIST of them, before any document is read for it.
//
// A request that does not name its documents used to be served by relevance
// retrieval over the whole data room: up to ninety-six passages from thirty
// documents, sixty thousand tokens, most of them beside the point - sent so
// the model could find the two documents the request was about. The list is
// a thousand tokens. The model says which documents it needs; only those are
// read, and the ranking that follows runs inside them.
//
// Singularity owns the list and the bounds (what is readable, how many may be
// chosen); the model owns the judgment of which documents the request rests
// on. A request that names its documents needs no judgment at all: the named
// documents are the pick. When the model cannot be asked, or picks nothing,
// the request is served as before - nothing is refused for want of a pick.
namespace singularity::context::source_picker {

// One document as the list shows it: a short handle the model answers with
// (never the registry id), its name and folder, and what Singularity knows of it
// in a line - the lead of the memory keeper's digest, when there is one.
struct ListedDocument {
	std::string handle;
	std::string document_id;
	std::string name;
	std::string folder;
	std::string format;
	std::string lead;
};

// At most this many documents may be picked: a request that rests on more
// than this is a data-room-wide one, served by relevance over everything.
inline constexpr std::size_t kMaximumPicked = 8;
// A data room this small is read as before: the list would cost about as much
// as the documents.
inline constexpr std::size_t kMinimumDocumentsToPick = 3;
// The same by length: documents that together run to fewer characters than
// this - a few pages - are read by relevance over all of them, which supplies
// every passage a pick could, so the pick's model call (seconds of a lawyer's
// wait, measured 6-19 s) buys nothing.
inline constexpr long long kMinimumCharactersToPick = 16000;
// The most of a digest that goes on the list, per document.
inline constexpr std::size_t kLeadCharacters = 160;

// The Project's readable documents as a list, in the registry's order, with
// the lead of each document's digest where the memory keeper has one.
[[nodiscard]] std::vector<ListedDocument> list_documents(const matters::Matter &matter);

// Whether a pick among `listed` can change what the request reads: enough
// documents, and enough of their text (kMinimumDocumentsToPick,
// kMinimumCharactersToPick).
[[nodiscard]] bool worth_picking(const matters::Matter &matter, const std::vector<ListedDocument> &listed);

// A room whose documents together fit comfortably in the model's window is
// read whole: every document, every passage, and no pick to pay for. A live
// run (24 Sep 2026) had a six-document matter picked down to four and read by
// relevance, and "What is this Project about?" left out the Board minutes and
// the register that decide it. Comfortably: at most two fifths of the window
// (about four characters to a token), and never more than
// kWholeRoomMaximumCharacters, so a large room still costs a pick, not its
// whole text on every request. An unknown window never reads a room whole.
inline constexpr long long kWholeRoomMaximumCharacters = 120000;
[[nodiscard]] bool fits_whole(const matters::Matter &matter, const std::vector<ListedDocument> &listed,
	std::optional<std::size_t> window_tokens);

// The documents whose file names (with or without the extension) the request
// quotes - named documents need no judgment.
[[nodiscard]] std::vector<std::string> documents_named_in(const std::string &request,
	const std::vector<ListedDocument> &documents);

// The prompt that asks for the pick: the request, the list, and the rule.
[[nodiscard]] std::string pick_prompt(const std::string &request, const std::vector<ListedDocument> &documents,
	const std::string &memory_brief);

// The strict JSON the pick is answered in: {"documents": [handles], "needsNone": bool}.
[[nodiscard]] providers::GenerationOutputContract pick_output_contract();

// The document ids the reply picks, in the reply's order: unknown handles are
// ignored, duplicates dropped, at most kMaximumPicked kept. Empty when the
// reply says no document is needed or cannot be read.
[[nodiscard]] std::vector<std::string> picked_document_ids(const std::string &reply,
	const std::vector<ListedDocument> &documents);

} // namespace singularity::context::source_picker
