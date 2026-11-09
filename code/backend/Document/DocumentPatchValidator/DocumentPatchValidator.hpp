#pragma once

#include "Document/Types/DocumentPatchTypes.hpp"
#include "Document/Types/DocumentSemantics.hpp"
#include "Document/Types/MatterDocument.hpp"

namespace singularity::documents {

// Checks, in order:
//   1. document identity - patch.document_id matches targetDocument.id;
//   2. expected version matches targetDocument.version;
//   3. expected fingerprint matches targetDocument.fingerprint;
//   4. the target format's capability supports cell/paragraph mutation
//      (document_format_capability() in Document/Types/DocumentSemantics.hpp);
//   5. currentModel was read successfully (ExtractionStatus::ready or empty);
//   6. per operation - the locator resolves to exactly one table/row/column
//      cell in currentModel (fails closed on both "not found" and "more than
//      one match", the same posture as DocumentTable::find_row());
//   7. per operation - expected_old_value matches that cell's current text.
// Domain-specific value meaning is deliberately outside this generic
// structural/state-safety boundary.
//
// The writer's own re-resolution against a freshly re-read file (see
// DocxExtractor.hpp's own comment) is a defensive final check, not a
// duplicate of this authority - this is still the only place that decides
// WHETHER a patch is valid.
// Whether a rewritten document holds exactly what a patch asked for, and
// nothing else: the same tables, the row counts the patch intended, every
// surviving row holding the same cells, and - for a patch that changes no
// text - every paragraph still reading what it read. This is the check that
// runs against the STAGED document before it is allowed to replace a lawyer's
// file, and the reason a change that quietly damaged a table can never commit.
[[nodiscard]] bool patch_change_is_contained(const DocumentSemanticModel &before,
	const DocumentSemanticModel &after, const DocumentPatch &patch);

// `equivalent_writings` (optional) groups the names that are the same party -
// see Platform/PhraseMatch. A value a model quotes back from a document it only
// ever saw protected often names a party another way; with the groups, that is
// recognised as the same value, and without them nothing changes.
[[nodiscard]] DocumentPatchValidationResult validate_document_patch(
		const matters::MatterDocument &targetDocument, const DocumentSemanticModel &currentModel,
		const DocumentPatch &patch, const std::vector<std::vector<std::string>> &equivalent_writings = {});

// A table operation names its table by the id Singularity gave it ("table-0"). A
// model that read the table only as words - in a passage, never in Singularity's
// structural reading - cannot know that id: it names the table its own way, or
// not at all (Golden C, 25 Sep 2026). Where the id it gives names no table of
// the document, or it gives none, the table is the one the operation's own
// words stand in: the row its first cell names and the column its heading
// names (and, for a cell, the value it quotes), in exactly one table. That
// table's real id is written into the operation, so validation, the approval
// and the writer all act on the same table. An id that names a real table is
// never changed, and nothing is guessed: words that stand in no table, or in
// more than one alike, leave the operation as it was for validation to refuse.
// Returns how many operations were given their table.
std::size_t settle_table_locators(const DocumentSemanticModel &currentModel,
		std::vector<DocumentPatchOperation> &operations,
		const std::vector<std::vector<std::string>> &equivalent_writings = {});

// The document's tables as a change names them: each by its id, what its
// columns are headed and what its rows' first cells say, one table per line.
// What a model is told when a change named a table, row or column the
// document does not have, so its one correction can name a real one. A long
// table lists its first rows, and always every further row that shares a word
// with a row the refused change named, so the row it meant is never cut off.
[[nodiscard]] std::string describe_document_tables(const DocumentSemanticModel &model,
		std::size_t maximumRowsPerTable = 24, const std::vector<std::string> &namedRows = {});

} // namespace singularity::documents
