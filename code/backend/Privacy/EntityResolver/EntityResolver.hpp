#pragma once

#include "Matter/Types/Matter.hpp"
#include "Privacy/Types/ProjectEntity.hpp"

#include <optional>
#include <string>

namespace singularity::privacy {

// One local candidate mention ProjectPreprocessing.cpp detected for a
// Document, ready to resolve against the Project's Entity Registry.
struct DetectedMention {
	std::string value;
	protected_api::papinian::EntityType category;
	std::string document_id;
	// A normalized nearby identifier that belongs to a party (an email,
	// phone, tax id or bank account detected close to this mention in the
	// same passage) - ProjectPreprocessing.cpp's own disambiguating signal for
	// people who share a name. Empty when none was found nearby, and always
	// empty for anything that is not a person.
	std::string context_key;
};

enum class ResolutionKind { AttachedToExisting, CreatedNew, AmbiguityRaised };

struct ResolutionOutcome {
	ResolutionKind kind = ResolutionKind::CreatedNew;
	// The entity this mention's occurrence should be recorded against -
	// always set, including on AmbiguityRaised (the mention still gets its
	// own consistent local identity while the ambiguity awaits review; see
	// PRIVACY_CONTRACT.md - preprocessing never blocks on unresolved
	// ambiguity).
	std::string entity_id;
	std::optional<std::string> ambiguity_id;
};

// A name's surface form for matching: Unicode case folding (utf8proc),
// quotation marks and commas dropped, whitespace collapsed, outer punctuation
// trimmed. "ООО «Ромашка»" and "ооо ромашка" share one surface key; "Acme Ltd"
// and "Acme Limited" do not.
[[nodiscard]] std::string surface_alias_key(const std::string &value);

// The surface key without its legal form (LLC/Ltd/plc/ООО/АО/...): the name
// base two aliases must share to be the same entity. Exposed for tests and
// context keys; production resolution uses resolve_mention().
[[nodiscard]] std::string normalize_alias(const std::string &value);

// The class of the legal form a name carries ("ltd" for both "Ltd" and
// "Limited", "ooo" for "ООО" and "Общество с ограниченной ответственностью"),
// empty when it carries none. Names sharing a base but carrying different
// classes are different legal entities until a person says otherwise.
[[nodiscard]] std::string legal_form_class(const std::string &value);

// normalize_alias() further reduced to a cross-script comparable key: diacritics
// removed, a simple explicit Cyrillic-to-Latin transliteration table and the
// same legal-form removal applied to the transliterated form. This is what lets
// "Romashka LLC" and "OOO Romashka" (Cyrillic) be recognized as a CANDIDATE
// match - never an automatic merge, only grounds for EntityAmbiguity review.
[[nodiscard]] std::string comparable_key(const std::string &normalized_value);

// Resolves one DetectedMention against `entities`/`ambiguities`, mutating
// both in place (a new entity may be appended; a new EntityAmbiguity may be
// appended), and returns which entity id the caller should record this
// mention's occurrence against.
//
// Resolution order:
//  1. The same surface form an entity already holds attaches to that entity
//     (the one where a person locked it, when several hold it), and otherwise
//     the same name base with a compatible legal form ("Ltd"/"Limited", or a
//     form on one side only) attaches when exactly one entity qualifies - never
//     to an entity that rejects the value (a person split or removed it), and
//     never when this mention's context_key conflicts with every context_key
//     recorded for that entity.
//  2. Otherwise, a comparable_key() match against a different entity in the
//     same category never merges automatically: a brand-new entity is created
//     for this mention and an EntityAmbiguity links the two for human review,
//     unless a prior KeptSeparate resolution already covers exactly this pair.
//     Names differing only in legal form ("plc"/"Limited", "ООО"/"АО") and a
//     formless name matching several entities land here.
//  3. Otherwise, a brand-new entity is created with a freshly assigned
//     pseudonym (EntityRegistry.hpp's make_new_entity()).
[[nodiscard]] ResolutionOutcome resolve_mention(matters::Matter &matter, const DetectedMention &mention,
		const std::string &timestamp);

} // namespace singularity::privacy
