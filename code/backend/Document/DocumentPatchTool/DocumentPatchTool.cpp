#include "Document/DocumentPatchTool/DocumentPatchTool.hpp"

#include "Document/CsvExtractor/CsvExtractor.hpp"
#include "Document/DocumentPatchValidator/DocumentPatchValidator.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Document/DocumentStructuralReader/DocumentStructuralReader.hpp"
#include "Document/DocumentVersionStore/DocumentVersionStore.hpp"
#include "Document/DocxExtractor/DocxExtractor.hpp"
#include "Document/Types/DocumentActionSerialization.hpp"
#include "Document/Types/DocumentChangeDescription.hpp"
#include "Document/Types/DocumentLocator.hpp"
#include "Document/Types/DocumentSemantics.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"
#include "Routing/Observability/OperationEvent.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Provider/Types/TaskError.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <tuple>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace singularity::documents {
namespace {

using Json = nlohmann::json;

std::mutex &registration_mutex() {
	static std::mutex value;
	return value;
}

bool &is_registered() {
	static bool value = false;
	return value;
}

std::optional<DocumentPatch> patch_from_json(const Json &arguments) {
	if (!arguments.is_object() || !arguments.contains("documentId") || !arguments.at("documentId").is_string()
			|| !arguments.contains("expectedVersion") || !arguments.at("expectedVersion").is_number_integer()
			|| !arguments.contains("expectedFingerprint") || !arguments.at("expectedFingerprint").is_string()
			|| !arguments.contains("operations") || !arguments.at("operations").is_array()
			|| arguments.at("operations").empty()) {
		return std::nullopt;
	}
	DocumentPatch patch;
	patch.document_id = arguments.at("documentId").get<std::string>();
	// Whatever writes this patch writes it as a revision under the name the
	// lawyer chose in Settings.
	patch.revision_author = platform::tracked_change_author(platform::load_app_settings());
	patch.expected_version = arguments.at("expectedVersion").get<int>();
	patch.expected_fingerprint = arguments.at("expectedFingerprint").get<std::string>();
	for (const Json &operationJson : arguments.at("operations")) {
		const std::optional<DocumentPatchOperation> operation = document_patch_operation_from_json(operationJson);
		if (!operation.has_value()) return std::nullopt;
		patch.operations.push_back(*operation);
	}
	return patch;
}

Json operation_outcome_to_json(const DocumentPatchOperationOutcome &outcome) {
	Json value{{"ok", outcome.ok}, {"blocker", to_string(outcome.blocker)}, {"detail", outcome.detail}};
	if (outcome.resolved_row_index.has_value()) value["resolvedRowIndex"] = *outcome.resolved_row_index;
	if (outcome.resolved_column_index.has_value()) value["resolvedColumnIndex"] = *outcome.resolved_column_index;
	return value;
}

Json validation_result_to_json(const DocumentPatchValidationResult &result) {
	Json operations = Json::array();
	for (const DocumentPatchOperationOutcome &outcome : result.operations) operations.push_back(operation_outcome_to_json(outcome));
	return Json{{"ok", result.ok}, {"blocker", to_string(result.blocker)}, {"detail", result.detail}, {"operations", operations}};
}

// Re-resolves the live target: the real Matter, the real MatterDocument by
// id, and its real absolute filesystem path - re-derived on every call
// (never cached across propose/apply) so a patch can never be applied
// against a stale in-memory path. Throws with a clear message on any
// resolution failure; ToolBroker's execute_definition() turns that into a
// Failed tool outcome with this message as error_message.
struct ResolvedTarget {
	matters::MatterDocument document;
	std::filesystem::path absolutePath;
	std::optional<std::string> projectDirectory;
};

ResolvedTarget resolve_target(const std::string &projectId, const std::string &documentId) {
	// Resolve from the canonical registry without starting a new filesystem
	// watcher in the middle of the commit protocol. The patch validator below
	// reads the actual current bytes and binds them to the persisted
	// version/fingerprint; after mutation, the explicit targeted reconcile is
	// the sole writer that advances that same Document identity.
	const matters::Matter project = matter::read_project_documents(projectId);
	const auto found = std::find_if(project.documents.begin(), project.documents.end(),
			[&](const matters::MatterDocument &candidate) { return candidate.id == documentId; });
	if (found == project.documents.end()) {
		throw std::runtime_error("The target document could not be found in the active Project.");
	}
	if (found->source_mode != matters::DocumentSourceMode::Referenced) {
		throw std::runtime_error("Only a real Project-folder document can be the target of a DocumentPatch.");
	}
	if (!project.directory_path.has_value()
			|| documents::registry::is_relative_path_safe(*project.directory_path, found->relative_path) == false) {
		throw std::runtime_error("The target document's Project-relative path failed the Project boundary check.");
	}
	ResolvedTarget resolved;
	resolved.document = *found;
	resolved.absolutePath = std::filesystem::path(*project.directory_path) / found->relative_path;
	resolved.projectDirectory = project.directory_path;
	return resolved;
}

// The conflict the last apply on this thread stopped at, in its plain words
// (take_document_patch_conflict()).
thread_local std::optional<std::string> lastPatchConflict;

// Ends an apply as a conflict: nothing was written, and the reason is the
// document's own state, never the change's.
[[noreturn]] void refuse_as_conflict(const platform::DocumentWriteConflict &conflict) {
	lastPatchConflict = std::string(conflict.what());
	throw conflict;
}

platform::DocumentWriteConflict changed_since_prepared(const ResolvedTarget &target) {
	return platform::DocumentWriteConflict(platform::DocumentWriteConflictKind::ChangedOnDisk,
		platform::document_changed_since_prepared_message(target.absolutePath.filename().u8string()));
}

// Whether the file on disk no longer holds the bytes the change was prepared
// for (the registry's fingerprint is not proof of them).
bool document_changed_on_disk(const ResolvedTarget &target, const std::string &expectedFingerprint) {
	try {
		std::error_code ignored;
		return std::filesystem::is_regular_file(target.absolutePath, ignored) == false
			|| platform::sha256_hex_digest_of_file(target.absolutePath) != expectedFingerprint;
	} catch (const std::exception &) {
		return true;
	}
}

// Whether the target document itself stands in the way of a write: open in the
// program that edits it, marked read-only or locked, or no longer holding the
// bytes the change was prepared for. Asked before the write and again when one
// could not be made, so the lawyer is told which - never a generic failure.
std::optional<platform::DocumentWriteConflict> document_write_conflict(const ResolvedTarget &target,
		const std::string &expectedFingerprint) {
	const std::string name = target.absolutePath.filename().u8string();
	if (platform::office_document_is_open(target.absolutePath))
		return platform::DocumentWriteConflict(platform::DocumentWriteConflictKind::OpenElsewhere,
			platform::office_document_open_message(name) + " Nothing was written.");
	if (platform::document_is_write_protected(target.absolutePath))
		return platform::DocumentWriteConflict(platform::DocumentWriteConflictKind::ReadOnly,
			platform::document_write_protected_message(name));
	if (document_changed_on_disk(target, expectedFingerprint) == true) return changed_since_prepared(target);
	return std::nullopt;
}

// A format writer, and the version store before it, report every failure of
// their own as a change they could not make - a full disk among them. That one
// is said as itself (platform::DiskFull), in the lawyer's words.
void refuse_if_disk_full(const std::string &failure) {
	if (failure == platform::DiskFull().what()) throw platform::DiskFull();
}

DocumentPatchValidationResult validate_against_live_state(
		const ResolvedTarget &target, const DocumentPatch &patch) {
	DocumentExtractionRequest request;
	request.path = target.absolutePath;
	request.format_hint = target.document.file_type;
	const DocumentSemanticModel model = read_document_structure(request);
	return validate_document_patch(target.document, model, patch);
}

// A table cell's text as the structural reader compares it: paragraphs and
// line breaks inside one cell read as single spaces.
std::string normalized_cell_text(const std::string &text) {
	std::string result;
	bool pendingSpace = false;
	for (unsigned char character : text) {
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
			pendingSpace = !result.empty();
			continue;
		}
		if (pendingSpace) result.push_back(' ');
		pendingSpace = false;
		result.push_back(static_cast<char>(character));
	}
	return result;
}

// Whether a staged rewrite of the target holds exactly the patch: every patched
// cell reads its new value, and every other table cell reads what it read
// before - same tables, rows and cells.
bool staged_rewrite_holds_exactly(const ResolvedTarget &target, const DocumentPatch &patch,
		const DocumentPatchValidationResult &validation, const DocumentSemanticModel &before,
		const std::filesystem::path &staged) {
	try {
		DocumentExtractionRequest request;
		request.path = staged;
		request.format_hint = target.document.file_type;
		const DocumentSemanticModel after = read_document_structure(request);
		// A patched cell is named by its table, row position and column key - the
		// same identity the validator resolved it by.
		std::map<std::tuple<std::string, std::size_t, std::string>, std::string> newValues;
		for (std::size_t index = 0; index < patch.operations.size() && index < validation.operations.size(); ++index) {
			const DocumentPatchOperationOutcome &outcome = validation.operations[index];
			const DocumentLocator &locator = patch.operations[index].locator;
			if (!outcome.resolved_row_index || !locator.table_id || !locator.column_key) return false;
			newValues[std::make_tuple(*locator.table_id, *outcome.resolved_row_index, *locator.column_key)] =
				patch.operations[index].new_value;
		}
		if (after.tables.size() != before.tables.size()) return false;
		for (std::size_t tableIndex = 0; tableIndex < before.tables.size(); ++tableIndex) {
			const DocumentTable &previous = before.tables[tableIndex];
			const DocumentTable &current = after.tables[tableIndex];
			if (previous.table_id != current.table_id || previous.rows.size() != current.rows.size()) return false;
			for (std::size_t rowIndex = 0; rowIndex < previous.rows.size(); ++rowIndex) {
				const DocumentTableRow &previousRow = previous.rows[rowIndex];
				const DocumentTableRow &currentRow = current.rows[rowIndex];
				if (previousRow.cells.size() != currentRow.cells.size()) return false;
				for (std::size_t cellIndex = 0; cellIndex < previousRow.cells.size(); ++cellIndex) {
					const auto patched = newValues.find(std::make_tuple(previous.table_id, previousRow.row_index,
						previousRow.cells[cellIndex].column_key));
					const std::string &expected = patched != newValues.end() ? patched->second
						: previousRow.cells[cellIndex].text;
					if (normalized_cell_text(currentRow.cells[cellIndex].text) != normalized_cell_text(expected)) return false;
				}
			}
		}
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

// Whether the live document already reads exactly what a prepared patch
// intended, checked at the cell positions that patch resolved to before it was
// applied (see execute_apply()'s own comment on why the row KEY cannot answer
// this once the change has landed). A prepared record from before those
// positions were recorded has nothing to check against and is not treated as
// applied - it is reported as a conflict rather than silently accepted.
bool already_applied_exactly(const ResolvedTarget &target, const Json &record) {
	if (!record.contains("resolvedCells") || !record.at("resolvedCells").is_array()
			|| record.at("resolvedCells").empty())
		return false;
	try {
		DocumentExtractionRequest request;
		request.path = target.absolutePath;
		request.format_hint = target.document.file_type;
		const DocumentSemanticModel model = read_document_structure(request);
		for (const Json &entry : record.at("resolvedCells")) {
			const std::string tableId = entry.value("tableId", std::string());
			const std::string columnKey = entry.value("columnKey", std::string());
			const std::size_t rowIndex = entry.value("rowIndex", static_cast<std::size_t>(0));
			const auto table = std::find_if(model.tables.begin(), model.tables.end(),
				[&](const DocumentTable &candidate) { return candidate.table_id == tableId; });
			if (table == model.tables.end()) return false;
			const auto row = std::find_if(table->rows.begin(), table->rows.end(),
				[&](const DocumentTableRow &candidate) { return candidate.row_index == rowIndex; });
			if (row == table->rows.end()) return false;
			const DocumentTableCell *cell = row->cell_for_column(columnKey);
			if (cell == nullptr) return false;
			if (normalized_cell_text(cell->text) != normalized_cell_text(entry.value("newValue", std::string())))
				return false;
		}
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

Json execute_propose(const tools::ToolExecutionContext &context, const Json &arguments) {
	const std::optional<DocumentPatch> patch = patch_from_json(arguments);
	if (!patch.has_value()) throw std::runtime_error("The document patch proposal is malformed.");
	const ResolvedTarget target = resolve_target(context.project_id, patch->document_id);
	const DocumentPatchValidationResult validation = validate_against_live_state(target, *patch);
	return Json{{"documentId", patch->document_id}, {"validation", validation_result_to_json(validation)}};
}

Json execute_apply(const tools::ToolExecutionContext &context, const Json &arguments) {
	lastPatchConflict.reset();
	const std::optional<DocumentPatch> patch = patch_from_json(arguments);
	if (!patch.has_value()) throw std::runtime_error("The document patch is malformed.");
	const ResolvedTarget target = resolve_target(context.project_id, patch->document_id);
	// The computer keeping the previous version, read before any lock is taken.
	const std::string keeperDevice = platform::current_device_id();
	// One writer per document, shared with artifact.action.apply and a restored
	// version: everything below reads and writes the file under it.
	const platform::DocumentWriteLock writeLock(target.absolutePath);
	const DocumentPatchValidationResult validation = validate_against_live_state(target, *patch);
	if (validation.ok == false) {
		// A document that is no longer the version the change was prepared for -
		// reconciled already, or edited on disk and not yet - is a conflict the
		// lawyer can prepare again, whatever the validator tripped on first.
		if (validation.blocker == DocumentPatchBlocker::VersionMismatch
				|| validation.blocker == DocumentPatchBlocker::FingerprintMismatch
				|| document_changed_on_disk(target, patch->expected_fingerprint) == true)
			refuse_as_conflict(changed_since_prepared(target));
		throw std::runtime_error("Document patch validation failed: " + std::string(to_string(validation.blocker))
				+ " - " + validation.detail);
	}

	namespace obs = routing::observability;
	obs::emit({obs::OperationEventKind::DocumentPatchValidated, "Document patch validated against current Project state",
		{{"operations", std::to_string(patch->operations.size())}},
		obs::has_sink() ? std::optional<std::string>(arguments.dump()) : std::nullopt});
	const std::string patchIdentity = platform::sha256_hex_digest(
		context.operation_id + "\n" + arguments.dump());
	if (context.mutation_prepared) {
		// The exact cells this change resolved to, by position. A row is located
		// by its own key, but writing into it can change that key (filling a free
		// template block is exactly that case), so the key alone cannot answer
		// "was this already applied?" after an interrupted commit. The resolved
		// positions can, and they are durable only if they are recorded here,
		// before the file is touched.
		Json resolvedCells = Json::array();
		for (std::size_t index = 0; index < patch->operations.size() && index < validation.operations.size(); ++index) {
			const DocumentPatchOperationOutcome &outcome = validation.operations[index];
			const DocumentLocator &locator = patch->operations[index].locator;
			if (!outcome.resolved_row_index || !locator.table_id || !locator.column_key) continue;
			resolvedCells.push_back(Json{{"tableId", *locator.table_id}, {"rowIndex", *outcome.resolved_row_index},
				{"columnKey", *locator.column_key}, {"newValue", patch->operations[index].new_value}});
		}
		context.mutation_prepared(Json{{"state", "prepared"}, {"patchId", patchIdentity},
			{"documentId", patch->document_id}, {"expectedVersion", patch->expected_version},
			{"expectedFingerprint", patch->expected_fingerprint}, {"arguments", arguments},
			{"resolvedCells", std::move(resolvedCells)}}.dump());
	}
	if (context.operation_is_current && !context.operation_is_current())
		throw std::runtime_error("The document mutation was cancelled before physical commit.");
	obs::emit({obs::OperationEventKind::DocumentMutationStart, "Applying validated document patch", {}, std::nullopt});
	// The registry's fingerprint is not proof of the bytes on disk: an edit the
	// watcher has not reconciled yet must never be overwritten. Nor is a document
	// written while its program holds it open (that program would save its own
	// copy over the change) or while it is marked read-only.
	if (const std::optional<platform::DocumentWriteConflict> conflict =
			document_write_conflict(target, patch->expected_fingerprint))
		refuse_as_conflict(*conflict);
	DocumentExtractionRequest beforeRequest;
	beforeRequest.path = target.absolutePath;
	beforeRequest.format_hint = target.document.file_type;
	const DocumentSemanticModel beforeModel = read_document_structure(beforeRequest);
	const std::filesystem::path versionsRoot = document_versions_root(target.projectDirectory,
		node::require_active_workspace_path(), context.project_id);
	std::string stagedFailure;
	// Verified first, then the previous version is kept, then the writer
	// commits: a rewrite that does not hold exactly the patch, or a previous
	// version that cannot be kept, leaves the document untouched.
	const bool cellReplacementsOnly = std::all_of(patch->operations.begin(), patch->operations.end(),
		[](const DocumentPatchOperation &operation) {
			return operation.kind == DocumentPatchOperationKind::ReplaceCellText;
		});
	const auto verifyAndPreserve = [&](const std::filesystem::path &staged) {
		const bool holds = cellReplacementsOnly
			? staged_rewrite_holds_exactly(target, *patch, validation, beforeModel, staged)
			: [&] {
				DocumentExtractionRequest afterRequest;
				afterRequest.path = staged;
				afterRequest.format_hint = target.document.file_type;
				return patch_change_is_contained(beforeModel, read_document_structure(afterRequest), *patch);
			}();
		if (!holds) return false;
		try {
			(void)preserve_document_version(versionsRoot, patch->document_id, target.document.relative_path,
				patch->expected_version, patch->expected_fingerprint, target.absolutePath, patchIdentity, keeperDevice);
		} catch (const std::exception &error) {
			stagedFailure = error.what();
			return false;
		}
		return true;
	};
	// Room for the whole change is asked for here, before anything is begun,
	// where a full disk can still be said as itself: the format writers below
	// would report it only as a change they could not make. A change holds up
	// to three copies of the document at once - the staged copy, the writer's
	// own rewrite of it before its rename (a Word package is rewritten whole),
	// and the version kept so the change can be undone.
	std::error_code sizeError;
	const std::uintmax_t documentSize = std::filesystem::file_size(target.absolutePath, sizeError);
	const std::uintmax_t changeBytes = sizeError ? 0 : documentSize * 3;
	platform::require_free_space(target.absolutePath.parent_path(), changeBytes);
	// A writer that still could not finish, with the disk now too low for even
	// one copy (another program took the room meanwhile, or the change grew
	// the document), ran out of room: said as that, never as its failure code.
	const auto refuse_if_disk_now_full = [&] {
		if (!sizeError) platform::require_free_space(target.absolutePath.parent_path(), documentSize);
	};
	// The product owns this exact atomic replacement, so suspend the folder
	// watcher across the physical write and synchronous targeted reconcile.
	// Otherwise an OS remove/create event can observe the writer's temporary
	// gap and conservatively split one logical Document into two identities.
	matter::stop_watching_documents(context.project_id);
	matters::Matter reconciled;
	// Set once the file is in place: a failure after it is Singularity's
	// records lagging, never "nothing was written" about a document that changed.
	bool committed = false;
	try {
		if (target.document.file_type == DocumentFormat::docx) {
			const bool cellsOnly = std::all_of(patch->operations.begin(), patch->operations.end(),
				[](const DocumentPatchOperation &operation) {
					return operation.kind == DocumentPatchOperationKind::ReplaceCellText;
				});
			const DocxPatchApplyResult applied = cellsOnly
				? apply_docx_table_cell_patch(target.absolutePath, *patch, verifyAndPreserve)
				: apply_docx_document_patch(target.absolutePath, *patch, verifyAndPreserve);
			if (applied.ok == false) {
				// The document may have been opened, locked or changed while the
				// change was built: said as that, the same as before the write.
				if (const std::optional<platform::DocumentWriteConflict> conflict =
						document_write_conflict(target, patch->expected_fingerprint))
					refuse_as_conflict(*conflict);
				refuse_if_disk_full(stagedFailure);
				refuse_if_disk_full(applied.detail);
				if (applied.blocker == DocumentPatchBlocker::AtomicReplaceFailed) refuse_if_disk_now_full();
				throw std::runtime_error(!stagedFailure.empty() ? stagedFailure
					: "DOCX patch application failed: " + std::string(to_string(applied.blocker)) + " - " + applied.detail);
			}
		} else if (target.document.file_type == DocumentFormat::csv) {
			const CsvPatchApplyResult applied = apply_csv_table_cell_patch(target.absolutePath, *patch, verifyAndPreserve);
			if (applied.ok == false) {
				if (const std::optional<platform::DocumentWriteConflict> conflict =
						document_write_conflict(target, patch->expected_fingerprint))
					refuse_as_conflict(*conflict);
				refuse_if_disk_full(stagedFailure);
				refuse_if_disk_full(applied.detail);
				if (applied.blocker == DocumentPatchBlocker::AtomicReplaceFailed) refuse_if_disk_now_full();
				throw std::runtime_error(!stagedFailure.empty() ? stagedFailure
					: "CSV patch application failed: " + std::string(to_string(applied.blocker)) + " - " + applied.detail);
			}
		} else {
			throw std::runtime_error("This document format has no real mutation writer in Phase 24.1C.");
		}
		committed = true;
		obs::emit({obs::OperationEventKind::DocumentVerificationStart,
			"Verifying the document patch and reconciling Project state",
			{{"format", to_string(target.document.file_type)}}, std::nullopt});

	// Reconcile the real overwrite through the same production Document
	// Registry path a filesystem watcher's own callback uses (Matter/
	// MatterActions/MatterActions.cpp's targeted-detect-then-process
	// composition) - a caller must never need to run this itself after a
	// successful apply, and no second reconciliation implementation lives
	// here: this calls the exact same functions DocumentFileActions.cpp's
	// rename/move/remove/add_external_file already call after their own real
	// filesystem mutations.
		// The file changed because Singularity wrote it: the journal says so.
		const registry::ChangeActorScope writtenBySingularity("singularity", context.operation_id);
		matter::detect_affected_paths(context.project_id, {target.absolutePath.string()});
		reconciled = matter::process_project_documents(context.project_id);
		(void)matter::list_documents(context.project_id); // restart steady-state watching after canonical reconciliation
		obs::emit({obs::OperationEventKind::DocumentVerificationEnd,
			"Document patch verified and Project state reconciled",
			{{"format", to_string(target.document.file_type)}}, std::nullopt});
	} catch (...) {
		try { (void)matter::list_documents(context.project_id); } catch (...) {}
		if (!committed) throw;
		// Written: the records catch up on the next read of the folder.
		obs::emit({obs::OperationEventKind::DocumentVerificationEnd,
			"The document was written; its Project records are brought up to date on the next read",
			{{"format", to_string(target.document.file_type)}, {"outcome", "written_unreconciled"}}, std::nullopt});
		try {
			reconciled = matter::process_project_documents(context.project_id);
		} catch (...) {
		}
	}
	auto reconciledDocument = std::find_if(reconciled.documents.begin(), reconciled.documents.end(),
			[&](const matters::MatterDocument &candidate) { return candidate.id == patch->document_id; });
	if (reconciledDocument == reconciled.documents.end()) {
		// The document as the write left it, until the registry reads it again.
		matters::MatterDocument written = target.document;
		written.version = target.document.version + 1;
		try {
			written.fingerprint = platform::sha256_hex_digest_of_file(target.absolutePath);
		} catch (const std::exception &) {
		}
		reconciled.documents.push_back(written);
		reconciledDocument = std::prev(reconciled.documents.end());
	}
	// The version this change wrote is kept too: should a later save made
	// outside Singularity replace it without its words, the change can be
	// found and put back (Matter/SharedMatter).
	try {
		(void)preserve_document_version(versionsRoot, patch->document_id, reconciledDocument->relative_path,
			reconciledDocument->version, reconciledDocument->fingerprint, target.absolutePath, patchIdentity + "written", keeperDevice);
	} catch (const std::exception &) {
	}
	prune_document_versions(versionsRoot, patch->document_id, keeperDevice);

	obs::emit({obs::OperationEventKind::DocumentMutationEnd, "Document changed and registry reconciled",
		{{"previousVersion", std::to_string(patch->expected_version)},
		 {"newVersion", std::to_string(reconciledDocument->version)},
		 {"newFingerprint", reconciledDocument->fingerprint}}, std::nullopt});
	if (context.mutation_committed) {
		context.mutation_committed(Json{{"state", "committed"}, {"patchId", patchIdentity},
			{"documentId", patch->document_id}, {"newVersion", reconciledDocument->version},
			{"newFingerprint", reconciledDocument->fingerprint}}.dump());
	}
	return Json{{"documentId", patch->document_id}, {"operationsApplied", patch->operations.size()},
			{"newVersion", reconciledDocument->version}, {"newFingerprint", reconciledDocument->fingerprint}};
}

std::optional<std::string> validate_arguments(const tools::ToolExecutionContext &, const Json &arguments) {
	if (!patch_from_json(arguments).has_value()) return "invalid_document_patch";
	return std::nullopt;
}

} // namespace

void recover_prepared_document_mutation(const tools::ToolExecutionContext &context,
		const std::string &prepared_record_json) {
	const Json record = Json::parse(prepared_record_json);
	if (!record.is_object() || record.value("state", std::string()) != "prepared"
			|| !record.contains("arguments") || !record.at("arguments").is_object())
		throw std::runtime_error("The prepared document mutation checkpoint is invalid.");
	const Json arguments = record.at("arguments");
	const auto patch = patch_from_json(arguments);
	if (!patch) throw std::runtime_error("The prepared document mutation is malformed.");

	ResolvedTarget target = resolve_target(context.project_id, patch->document_id);
	const DocumentPatchValidationResult originalValidation = validate_against_live_state(target, *patch);
	if (originalValidation.ok) {
		// Called here directly, not through the broker: a document that is not in
		// a state to be written ends the change as the conflict it is.
		try {
			(void)execute_apply(context, arguments);
		} catch (const platform::DocumentWriteConflict &conflict) {
			lastPatchConflict.reset();
			throw providers::TaskError(providers::TaskErrorCode::TargetChanged, conflict.what());
		}
		return;
	}

	// A crash may have happened after atomic file replacement but before the
	// registry/commit acknowledgement. Reconcile, then prove every intended
	// target already has its new value before declaring the same patch complete.
	// The file changed because Singularity wrote it: the journal says so.
	const registry::ChangeActorScope writtenBySingularity("singularity", context.operation_id);
	matter::detect_affected_paths(context.project_id, {target.absolutePath.string()});
	(void)matter::process_project_documents(context.project_id);
	target = resolve_target(context.project_id, patch->document_id);
	if (!already_applied_exactly(target, record))
		throw providers::TaskError(providers::TaskErrorCode::TargetChanged,
			platform::document_changed_since_prepared_message(target.absolutePath.filename().u8string()));
	if (context.mutation_committed) {
		context.mutation_committed(Json{{"state", "committed"},
			{"patchId", record.value("patchId", std::string())}, {"documentId", patch->document_id},
			{"newVersion", target.document.version}, {"newFingerprint", target.document.fingerprint},
			{"recovered", true}}.dump());
	}
}

void ensure_production_document_patch_tools() {
	const std::lock_guard<std::mutex> lock(registration_mutex());
	if (is_registered() == true) return;

	tools::ToolDefinition propose;
	propose.id = "document.patch.propose";
	propose.capability = "document.patch.propose";
	propose.effect = tools::ToolEffectClass::LocalProposal;
	propose.approval_summary = "Prepare a validated proposal to change this Project document";
	propose.validate = validate_arguments;
	propose.execute = execute_propose;
	tools::production_tool_broker().register_tool(propose);

	tools::ToolDefinition apply;
	apply.id = "document.patch.apply";
	apply.capability = "document.patch.apply";
	apply.effect = tools::ToolEffectClass::RoutineProjectMutation;
	apply.approval_summary = "Apply a validated change to this Project document";
	apply.validate = validate_arguments;
	apply.execute = execute_apply;
	// The artifact tool's own rule (artifact_change_is_significant): adding to
	// a document, entering a value in an empty place or formatting it is
	// routine; taking a passage or a row out, emptying a value, or rewording it
	// so that it says something else - a party, a figure, a period, a "not" -
	// waits for the lawyer under "Apply routine changes" too. A change this
	// cannot read is significant.
	apply.effect_of = [](const tools::ToolExecutionContext &, const Json &arguments) {
		const std::optional<DocumentPatch> patch = patch_from_json(arguments);
		if (!patch) return tools::ToolEffectClass::ElevatedProjectMutation;
		for (const DocumentPatchOperation &operation : patch->operations) {
			switch (operation.kind) {
				case DocumentPatchOperationKind::DeleteContent:
				case DocumentPatchOperationKind::DeleteTableRow:
					return tools::ToolEffectClass::ElevatedProjectMutation;
				case DocumentPatchOperationKind::ReplaceText:
				case DocumentPatchOperationKind::ReplaceCellText: {
					// Filling a template's blank ("[●]", "[TBD]", nothing yet) only adds.
					if (is_template_placeholder(operation.expected_old_value)) break;
					const bool emptied = operation.new_value.find_first_not_of(" \t\r\n") == std::string::npos;
					// A register's cell is its data: entering a new figure or status is
					// routine, clearing or zeroing it is not. A passage reworded so that
					// it says something else is not routine either.
					const bool significant = operation.kind == DocumentPatchOperationKind::ReplaceCellText
						? tools::takes_out_substance(operation.expected_old_value, operation.new_value)
						: tools::rewrites_substance(operation.expected_old_value, operation.new_value);
					if (emptied || significant) return tools::ToolEffectClass::ElevatedProjectMutation;
					break;
				}
				case DocumentPatchOperationKind::InsertParagraph:
				case DocumentPatchOperationKind::InsertTableRow:
				case DocumentPatchOperationKind::SetFormatting:
					break;
			}
		}
		return tools::ToolEffectClass::RoutineProjectMutation;
	};
	apply.describe_approval_target = [](const tools::ToolExecutionContext &context, const Json &arguments,
			tools::ToolApprovalRequest &approval) {
		const std::optional<DocumentPatch> patch = patch_from_json(arguments);
		if (!patch) return;
		approval.target_document_id = patch->document_id;
		approval.target_version = patch->expected_version;
		approval.target_fingerprint = patch->expected_fingerprint;
		// The change itself, step by step, with each cell's current words read
		// from the document as it stands now: a person approves what they can
		// see, and a register cell that is overwritten says what it said.
		std::vector<DocumentChangeStep> steps = describe_patch_operations(patch->operations);
		try {
			const ResolvedTarget target = resolve_target(context.project_id, patch->document_id);
			DocumentExtractionRequest request;
			request.path = target.absolutePath;
			request.format_hint = target.document.file_type;
			complete_change_steps(steps, read_document_structure(request));
		} catch (const std::exception &) {
			// The approval still shows what the change itself says.
		}
		Json operations = Json::array();
		for (const DocumentChangeStep &step : steps) operations.push_back(document_change_step_json(step));
		approval.change_kind = "edit";
		approval.change_detail_json = Json{{"kind", "edit"}, {"operations", std::move(operations)}}.dump();
	};
	tools::production_tool_broker().register_tool(apply);

	is_registered() = true;
}

void reset_document_patch_tools_for_testing() {
	const std::lock_guard<std::mutex> lock(registration_mutex());
	is_registered() = false;
}

std::optional<std::string> take_document_patch_conflict() {
	std::optional<std::string> conflict = std::move(lastPatchConflict);
	lastPatchConflict.reset();
	return conflict;
}

void forget_document_patch_conflict() {
	lastPatchConflict.reset();
}

void end_unexecuted_document_patch(const tools::ToolResult &result, const std::string &fallback) {
	const std::optional<std::string> conflict = take_document_patch_conflict();
	if (result.outcome == tools::ToolOutcome::Executed) return;
	if (conflict) throw providers::TaskError(providers::TaskErrorCode::TargetChanged, *conflict);
	if (result.error_message == platform::DiskFull().what()) throw platform::DiskFull();
	throw std::runtime_error(result.error_message.empty() ? fallback : result.error_message);
}

} // namespace singularity::documents
