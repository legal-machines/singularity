#pragma once

#include "Tool/ToolBroker/ToolBroker.hpp"
#include "Tool/Types/ToolTypes.hpp"

#include <optional>
#include <string>

namespace singularity::documents {

// Registers the "document.patch.propose" and "document.patch.apply" tools
// (Tool/Types/ToolTypes.hpp already reserves both capability strings - Phase
// 24.1A's is_canonical_capability()) with the production ToolBroker. Both
// tools re-resolve the target MatterDocument, re-read its live
// DocumentSemanticModel and re-run DocumentPatchValidator on every call -
// propose never caches a result apply later trusts blindly. Only
// "document.patch.apply" ever calls a format writer
// (DocxExtractor::apply_docx_table_cell_patch, CsvExtractor::apply_csv_table_
// cell_patch); "document.patch.propose" never mutates a file. A successful
// apply also reconciles the real overwrite through the production Document
// Registry path (Matter/MatterActions/MatterActions.hpp's detect_affected_
// paths()+process_project_documents(), the same composition a filesystem
// watcher callback uses) before returning, so its result JSON's
// "newVersion"/"newFingerprint" are already the reconciled canonical
// truth - no caller needs to reconcile the registry itself afterward.
// Idempotent and safe to call repeatedly, matching ExternalResearch/
// ResearchTools.hpp's ensure_production_research_tools() precedent.
void ensure_production_document_patch_tools();

// Test-only: clears the one-time "already registered" latch so a later
// ensure_production_document_patch_tools() call actually re-registers,
// mirroring ToolBroker::clear_registered_tools_for_testing() (which this is
// meant to be paired with) - simulates a real process restart's naturally
// fresh static state, which a single test process cannot otherwise get
// without this. Never called from production code.
void reset_document_patch_tools_for_testing();

// Why the last document.patch.apply on this thread wrote nothing, when the
// document itself was the reason: it changed after the change was prepared, it
// is open in the program that edits it, or it is marked read-only. Its plain
// words, which the lawyer reads beside "prepare it again". The tool broker hands
// a failure on as its words alone, so the caller that applied the patch reads
// this to end the change as the conflict it was - the same outcome the artifact
// path gives (providers::TaskErrorCode::TargetChanged). Taking it clears it;
// forget_document_patch_conflict() clears it before an apply.
[[nodiscard]] std::optional<std::string> take_document_patch_conflict();
void forget_document_patch_conflict();

// Ends a document.patch.apply that did not execute as what stopped it, for
// every caller that applies a patch - one mapping, never a generic failure:
// the document's own state (take_document_patch_conflict()) as
// providers::TaskError(TargetChanged), a full disk as platform::DiskFull, and
// anything else as its own plain words (`fallback` when it has none). Returns
// when the result executed. Call forget_document_patch_conflict() before the
// apply; this takes the conflict either way.
void end_unexecuted_document_patch(const tools::ToolResult &result, const std::string &fallback);

// Reconciles one durable PREPARED mutation after restart. It either applies
// the still-current patch once, recognizes the already-achieved postcondition,
// or fails closed on an external conflict - thrown as
// providers::TaskError(TargetChanged) in plain words, so the change ends as a
// conflict the lawyer can prepare again.
void recover_prepared_document_mutation(const tools::ToolExecutionContext &context,
	const std::string &prepared_record_json);

} // namespace singularity::documents
