#include "Privacy/ProjectPreprocessing/ProjectPreprocessing.hpp"

#include "PapinianGuard/Policy/Policy.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Privacy/EntityResolver/EntityResolver.hpp"

#include <algorithm>
#include <stdexcept>

namespace singularity::privacy {
namespace {

using protected_api::papinian::CanonicalEntity;
using protected_api::papinian::EntityType;

// Identifiers that belong to one party and so can tell two people with the
// same name apart - EntityResolver.hpp's own comment: "identical strings do
// not automatically imply same entity". A case or contract number names a
// proceeding or an agreement, which many parties share, so it is not one.
bool isContextIdentifierCategory(EntityType type) {
	switch (type) {
		case EntityType::Email:
		case EntityType::Phone:
		case EntityType::BankAccount:
		case EntityType::TaxId:
			return true;
		default:
			return false;
	}
}

bool isPersonCategory(EntityType type) {
	return type == EntityType::Person || type == EntityType::Judge;
}

const std::size_t kContextWindowBytes = 250;

std::string nearestContextKey(const std::vector<CanonicalEntity> &entities, const CanonicalEntity &mention) {
	std::string bestValue;
	std::size_t bestDistance = kContextWindowBytes + 1;
	const std::size_t mentionMid = mention.start + mention.length / 2;
	for (const CanonicalEntity &candidate : entities) {
		if (!candidate.type || isContextIdentifierCategory(*candidate.type) == false) continue;
		const std::size_t candidateMid = candidate.start + candidate.length / 2;
		const std::size_t distance =
				candidateMid > mentionMid ? candidateMid - mentionMid : mentionMid - candidateMid;
		if (distance > kContextWindowBytes || distance >= bestDistance) continue;
		bestDistance = distance;
		bestValue = normalize_alias(candidate.value);
	}
	return bestValue;
}

void upsertDocumentProtection(matters::Matter &matter, const DocumentProtectionStatus &status) {
	const auto slot = std::find_if(matter.document_protection.begin(), matter.document_protection.end(),
			[&](const DocumentProtectionStatus &entry) { return entry.document_id == status.document_id; });
	if (slot == matter.document_protection.end()) {
		matter.document_protection.push_back(status);
		return;
	}
	*slot = status;
}

} // namespace

bool needs_protection_pass(const matters::Matter &matter, const matters::MatterDocument &document) {
	if (matters::document_lifecycle_state(document) != matters::DocumentLifecycleState::Ready) return false;
	const auto status = find_document_protection(matter, document.id);
	// Only a current successful run is terminal. Unsupported is durable
	// failure truth, not a permanent poison pill: once Papinian becomes
	// available again the normal production preprocessing path must retry
	// this same registered Document without requiring a fingerprint change.
	return !(status && status->state == DocumentProtectionState::Ready && is_protection_current(*status, document));
}

PassageEntities detect_passage_entities(const std::vector<std::string> &passages,
		const protected_api::papinian::PapinianGuard &guard) {
	protected_api::Policy detection_policy;
	detection_policy.mode = protected_api::Mode::Standard;
	std::vector<protected_api::papinian::SemanticLeaf> leaves;
	std::vector<std::size_t> positions;
	for (std::size_t index = 0; index < passages.size(); ++index) {
		if (passages[index].find_first_not_of(" \t\r\n") == std::string::npos) continue;
		leaves.push_back({"passage" + std::to_string(index), passages[index]});
		positions.push_back(index);
	}
	PassageEntities detections(passages.size());
	if (leaves.empty()) return detections;
	// PapinianGuard validates its request namespace as 8-32 ASCII
	// alphanumeric characters (it becomes part of a request-scoped placeholder
	// token). This local pass has no outbound request of its own, so it uses
	// one fixed, valid, clearly internal value. Its symbol table is discarded:
	// only what was detected, and where, is kept.
	protected_api::papinian::OperationProtectedSymbolTable symbols;
	const auto results = guard.collect_sensitive_values_batch(leaves, detection_policy, "privacypreprocessing", symbols);
	if (results.size() != leaves.size()) throw std::runtime_error("Local detection returned the wrong passage count.");
	for (std::size_t index = 0; index < results.size(); ++index) {
		detections[positions[index]] = results[index].entities;
	}
	return detections;
}

void record_document_detection(matters::Matter &matter, const matters::MatterDocument &document,
		const PassageEntities &detections, const std::string &ner_model_version, const std::string &timestamp) {
	// Detection itself always succeeds here regardless of ambiguity: whether
	// this Document currently "needs review" is a property of the Project's
	// entity_ambiguities right now (EntityRegistry.hpp's
	// document_has_pending_review()), computed fresh on every read - never
	// frozen into this status, which would otherwise go stale the moment a
	// person resolves the ambiguity elsewhere (EntityCuration.cpp) without
	// this exact Document ever being reprocessed again.
	for (const auto &passage : detections) {
		for (const CanonicalEntity &canonical : passage) {
			if (!canonical.type) continue;
			// Recorded, a value is masked in every later request of the Project:
			// what names nobody wherever it stands - prose the document wrote in
			// lower case, public vocabulary, a role or an office, a title made of
			// ordinary words - stays out of the registry.
			if (!protected_api::papinian::detection_may_be_recorded(*canonical.type, canonical.value)) continue;
			DetectedMention mention;
			mention.value = canonical.value;
			mention.category = *canonical.type;
			mention.document_id = document.id;
			mention.context_key = isPersonCategory(*canonical.type)
					? nearestContextKey(passage, canonical)
					: std::string();
			const auto outcome = resolve_mention(matter, mention, timestamp);
			record_occurrence(matter, outcome.entity_id, document.id, mention.value);
		}
	}

	DocumentProtectionStatus status;
	status.document_id = document.id;
	status.state = DocumentProtectionState::Ready;
	status.processed_fingerprint = document.fingerprint;
	status.processed_extractor_version = document.extractor_version;
	status.ner_model_version = ner_model_version;
	status.detail = "";
	status.updated_at = timestamp;
	upsertDocumentProtection(matter, status);
}

void record_document_detection_failure(matters::Matter &matter, const matters::MatterDocument &document,
		const std::string &ner_model_version, const std::string &timestamp, const std::string &reason) {
	DocumentProtectionStatus failed;
	failed.document_id = document.id;
	failed.state = DocumentProtectionState::Unsupported;
	failed.processed_fingerprint = document.fingerprint;
	failed.processed_extractor_version = document.extractor_version;
	failed.ner_model_version = ner_model_version;
	failed.detail = std::string("Protection unavailable: ") + reason;
	failed.updated_at = timestamp;
	upsertDocumentProtection(matter, failed);
}

bool preprocess_document(matters::Matter &matter, const matters::MatterDocument &document,
		const std::string &extracted_text, const protected_api::papinian::PapinianGuard &guard,
		const std::string &ner_model_version, const std::string &timestamp) {
	record_document_detection(matter, document, detect_passage_entities({extracted_text}, guard),
			ner_model_version, timestamp);
	return true;
}

bool preprocess_pending_documents(matters::Matter &matter, const protected_api::papinian::PapinianGuard &guard,
		const std::string &ner_model_version, const std::string &timestamp, const ExtractedTextLoader &load_text) {
	bool changed = false;
	// Snapshot the document list first: the loop body mutates
	// matter.entities/entity_ambiguities/document_protection (through
	// preprocess_document()), never matter.documents itself, but iterating
	// one member of `matter` while a sibling member of the SAME object is
	// mutated through the SAME reference is worth making obviously safe
	// rather than relying on today's mutation pattern staying that way.
	const std::vector<matters::MatterDocument> documents = matter.documents;
	for (const matters::MatterDocument &document : documents) {
		if (needs_protection_pass(matter, document) == false) continue;
		const auto text = load_text(document);
		if (!text) continue;

		try {
			if (preprocess_document(matter, document, *text, guard, ner_model_version, timestamp) == true) {
				changed = true;
			}
		} catch (const std::exception &error) {
			record_document_detection_failure(matter, document, ner_model_version, timestamp, error.what());
			changed = true;
		}
	}
	return changed;
}

} // namespace singularity::privacy
