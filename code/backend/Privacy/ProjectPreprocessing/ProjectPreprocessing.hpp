#pragma once

#include "Matter/Types/Matter.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::privacy {

// Reads one AI-ready Document's already-extracted text (Document/
// DocumentCache/DocumentExtractor's own pipeline output - never re-extracted
// here). Production wiring supplies a real DocumentCache-backed loader;
// tests supply a fixture. Returns nullopt when no cached extraction is
// available yet for this exact fingerprint/extractor_version.
using ExtractedTextLoader = std::function<std::optional<std::string>(const matters::MatterDocument &)>;

// Every detection found in one passage of a Document, positioned within that
// passage's own text. A Document is detected passage by passage, the way a
// send presents it to Papinian Guard, so the same scores serve both.
using PassageEntities = std::vector<std::vector<protected_api::papinian::CanonicalEntity>>;

// Whether `document` is AI-ready and has no current, successful protection
// pass recorded - the one test every preprocessing caller uses.
bool needs_protection_pass(const matters::Matter &matter, const matters::MatterDocument &document);

// The heavy half of a protection pass: runs local detection over one
// Document's passages and reads nothing from the Project, so a caller can run
// it without holding the persistence lock. Throws when the local model fails.
PassageEntities detect_passage_entities(const std::vector<std::string> &passages,
		const protected_api::papinian::PapinianGuard &guard);

// The light half: resolves every detected mention into the Project's Entity
// Registry and records the Document's protection as Ready.
void record_document_detection(matters::Matter &matter, const matters::MatterDocument &document,
		const PassageEntities &detections, const std::string &ner_model_version, const std::string &timestamp);

// Records a Document whose detection failed as Unsupported, with the reason.
void record_document_detection_failure(matters::Matter &matter, const matters::MatterDocument &document,
		const std::string &ner_model_version, const std::string &timestamp, const std::string &reason);

// Runs local entity detection (via `guard`, the same PapinianGuard pipeline
// every real Papinian Guard call uses - PapinianGuard/PapinianGuardService/
// PapinianGuardService.hpp) over one Document's extracted text and resolves
// every detected mention against the Project's Entity Registry
// (EntityResolver.hpp). Only CanonicalEntity type/value/position is read from
// the detection result; protected_text/mapping/validation (the ephemeral,
// request-scoped outbound-Guard shape) are never read or persisted here -
// Phase 11 owns that contract, and this function never touches the network.
// Mutates matter.entities/entity_ambiguities/document_protection in place;
// the caller persists it (see Matter/MatterActions.hpp's own load-modify-save
// convention).
bool preprocess_document(matters::Matter &matter, const matters::MatterDocument &document,
		const std::string &extracted_text, const protected_api::papinian::PapinianGuard &guard,
		const std::string &ner_model_version, const std::string &timestamp);

// Runs preprocess_document() over every Document currently AI-ready
// (Document/Types/MatterDocument.hpp's document_lifecycle_state() == Ready)
// whose protection state is missing or stale (EntityRegistry.hpp's
// is_protection_current()) - a changed Document is naturally reprocessed
// here, since its new fingerprint no longer matches the last recorded one. A
// Document whose detection genuinely fails is recorded Unsupported with a
// truthful detail rather than aborting the rest of the batch. Returns true
// if anything in `matter` changed.
bool preprocess_pending_documents(matters::Matter &matter, const protected_api::papinian::PapinianGuard &guard,
		const std::string &ner_model_version, const std::string &timestamp, const ExtractedTextLoader &load_text);

} // namespace singularity::privacy
