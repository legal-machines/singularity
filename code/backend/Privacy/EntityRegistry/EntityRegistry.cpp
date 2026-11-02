#include "Privacy/EntityRegistry/EntityRegistry.hpp"

#include "PapinianGuard/ProtectedMarkers/ProtectedMarkers.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"

#include <utility>
#include <cctype>
#include <set>
#include <algorithm>

namespace singularity::privacy {
namespace {

using protected_api::papinian::EntityType;

// 24 digits that are 96 bits from the system's secure source
// (Platform/SecureRandom).
std::string generate_id(const char *prefix) {
	return std::string(prefix) + platform::secure_random_hex(12);
}

// A Document only ever blocks protection through one of these two layers:
// its own extraction/lifecycle truth (Document/Types/MatterDocument.hpp),
// checked first, or - once that layer is clear - this Phase's own
// preprocessing readiness. Never both reported at once.
matters::DocumentLifecycleState lifecycle_of(const matters::MatterDocument &document) {
	return matters::document_lifecycle_state(document);
}

// Plain-language fallback for a blocking Document lifecycle state that has no
// specific attention_detail of its own (Document/Types/MatterDocument.hpp
// only ever sets attention_detail for NeedsAttention) - never the raw wire
// enum string, which would read as engineering jargon in Privacy's UI.
std::string plain_language_lifecycle_reason(matters::DocumentLifecycleState lifecycle) {
	switch (lifecycle) {
		case matters::DocumentLifecycleState::Processing: return "Protection processing";
		case matters::DocumentLifecycleState::Changed: return "Content changed, reprocessing";
		case matters::DocumentLifecycleState::Unsupported: return "Unsupported for AI";
		case matters::DocumentLifecycleState::Unavailable: return "Unavailable";
		case matters::DocumentLifecycleState::NeedsAttention: return "Needs attention";
		case matters::DocumentLifecycleState::Ready: return "";
	}
	return "Needs attention";
}

} // namespace

ProjectCoverage compute_coverage(const matters::Matter &matter) {
	ProjectCoverage coverage;
	for (const matters::MatterDocument &document : matter.documents) {
		if (lifecycle_of(document) != matters::DocumentLifecycleState::Ready) continue;
		coverage.total++;
		const auto status = find_document_protection(matter, document.id);
		if (status && status->state == DocumentProtectionState::Ready && is_protection_current(*status, document) &&
				document_has_pending_review(matter, document.id) == false) {
			coverage.ready++;
		}
	}
	return coverage;
}

std::vector<AttentionDocument> list_attention_documents(const matters::Matter &matter) {
	std::vector<AttentionDocument> attention;
	for (const matters::MatterDocument &document : matter.documents) {
		const matters::DocumentLifecycleState lifecycle = lifecycle_of(document);
		if (lifecycle != matters::DocumentLifecycleState::Ready) {
			std::string reason = document.attention_detail ? *document.attention_detail
					: plain_language_lifecycle_reason(lifecycle);
			attention.push_back(AttentionDocument{document.id, document.name, reason});
			continue;
		}
		const auto status = find_document_protection(matter, document.id);
		if (!status || is_protection_current(*status, document) == false ||
				status->state == DocumentProtectionState::NotReady) {
			attention.push_back(AttentionDocument{document.id, document.name, "Protection processing"});
			continue;
		}
		if (status->state == DocumentProtectionState::Unsupported) {
			attention.push_back(AttentionDocument{document.id, document.name,
					status->detail.empty() ? "Protection unavailable" : status->detail});
			continue;
		}
		if (document_has_pending_review(matter, document.id) == true) {
			attention.push_back(AttentionDocument{document.id, document.name, "Needs review"});
		}
	}
	return attention;
}

std::optional<DocumentProtectionStatus> find_document_protection(
		const matters::Matter &matter, const std::string &document_id) {
	const auto found = std::find_if(matter.document_protection.begin(), matter.document_protection.end(),
			[&](const DocumentProtectionStatus &entry) { return entry.document_id == document_id; });
	if (found == matter.document_protection.end()) return std::nullopt;
	return *found;
}

bool is_protection_current(const DocumentProtectionStatus &status, const matters::MatterDocument &document) {
	return status.processed_fingerprint == document.fingerprint &&
			status.processed_extractor_version == document.extractor_version;
}

bool document_has_pending_review(const matters::Matter &matter, const std::string &document_id) {
	for (const EntityAmbiguity &ambiguity : matter.entity_ambiguities) {
		if (ambiguity.status != AmbiguityStatus::Pending) continue;
		for (const std::string &entityId : {ambiguity.entity_a_id, ambiguity.entity_b_id}) {
			const auto entity = find_entity(matter, entityId);
			if (!entity) continue;
			const bool hasOccurrence = std::any_of(entity->occurrences.begin(), entity->occurrences.end(),
					[&](const EntityOccurrence &occurrence) { return occurrence.document_id == document_id; });
			if (hasOccurrence == true) return true;
		}
	}
	return false;
}

std::vector<ProjectEntity> list_active_entities(const matters::Matter &matter) {
	std::vector<ProjectEntity> active;
	for (const ProjectEntity &entity : matter.entities) {
		if (entity.merged_into_entity_id) continue;
		active.push_back(entity);
	}
	return active;
}

std::optional<ProjectEntity> find_entity(const matters::Matter &matter, const std::string &entity_id) {
	const auto found = std::find_if(matter.entities.begin(), matter.entities.end(),
			[&](const ProjectEntity &entity) { return entity.id == entity_id; });
	if (found == matter.entities.end()) return std::nullopt;
	return *found;
}

std::vector<EntityAmbiguity> pending_ambiguities_for(const matters::Matter &matter, const std::string &entity_id) {
	std::vector<EntityAmbiguity> result;
	for (const EntityAmbiguity &ambiguity : matter.entity_ambiguities) {
		if (ambiguity.status != AmbiguityStatus::Pending) continue;
		if (ambiguity.entity_a_id != entity_id && ambiguity.entity_b_id != entity_id) continue;
		result.push_back(ambiguity);
	}
	return result;
}

long long count_pending_ambiguities(const matters::Matter &matter) {
	return std::count_if(matter.entity_ambiguities.begin(), matter.entity_ambiguities.end(),
			[](const EntityAmbiguity &ambiguity) { return ambiguity.status == AmbiguityStatus::Pending; });
}

long long next_category_ordinal(const matters::Matter &matter, EntityType category) {
	long long highest = 0;
	for (const ProjectEntity &entity : matter.entities) {
		if (entity.category != category) continue;
		const std::string prefix = display_category_name(category) + " ";
		if (entity.pseudonym.rfind(prefix, 0) != 0) continue;
		try {
			const long long ordinal = std::stoll(entity.pseudonym.substr(prefix.size()));
			highest = std::max(highest, ordinal);
		} catch (const std::exception &) {
		}
	}
	return highest + 1;
}

ProjectEntity make_new_entity(
		const matters::Matter &matter, EntityType category, const std::string &canonical_value,
		const std::string &timestamp) {
	ProjectEntity entity;
	entity.id = generate_id("ent_");
	entity.category = category;
	entity.pseudonym = display_category_name(category) + " " + std::to_string(next_category_ordinal(matter, category));
	entity.canonical_value = canonical_value;
	entity.aliases.push_back(canonical_value);
	entity.created_at = timestamp;
	entity.updated_at = timestamp;
	return entity;
}

std::string generate_ambiguity_id() { return generate_id("amb_"); }

void record_occurrence(matters::Matter &matter, const std::string &entity_id, const std::string &document_id,
		const std::string &alias) {
	const auto entity = std::find_if(matter.entities.begin(), matter.entities.end(),
			[&](const ProjectEntity &entry) { return entry.id == entity_id; });
	if (entity == matter.entities.end()) return;
	const auto occurrence = std::find_if(entity->occurrences.begin(), entity->occurrences.end(),
			[&](const EntityOccurrence &entry) { return entry.document_id == document_id; });
	if (occurrence != entity->occurrences.end()) {
		occurrence->mention_count++;
		return;
	}
	entity->occurrences.push_back(EntityOccurrence{document_id, alias, 1});
}

const char *to_string(DocumentProtectionState state) noexcept {
	switch (state) {
		case DocumentProtectionState::NotReady: return "not_ready";
		case DocumentProtectionState::Ready: return "ready";
		case DocumentProtectionState::Unsupported: return "unsupported";
	}
	return "not_ready";
}

DocumentProtectionState document_protection_state_from_string(const std::string &value) noexcept {
	if (value == "ready") return DocumentProtectionState::Ready;
	if (value == "unsupported") return DocumentProtectionState::Unsupported;
	return DocumentProtectionState::NotReady;
}

const char *to_string(AmbiguityStatus status) noexcept {
	switch (status) {
		case AmbiguityStatus::Pending: return "pending";
		case AmbiguityStatus::MergedSameEntity: return "merged_same_entity";
		case AmbiguityStatus::KeptSeparate: return "kept_separate";
	}
	return "pending";
}

AmbiguityStatus ambiguity_status_from_string(const std::string &value) noexcept {
	if (value == "merged_same_entity") return AmbiguityStatus::MergedSameEntity;
	if (value == "kept_separate") return AmbiguityStatus::KeptSeparate;
	return AmbiguityStatus::Pending;
}

std::string display_category_name(EntityType category) {
	switch (category) {
		case EntityType::Person: return "Person";
		case EntityType::LegalEntity: return "Company";
		case EntityType::Organisation: return "Organisation";
		case EntityType::Location: return "Location";
		case EntityType::Address: return "Address";
		case EntityType::Email: return "Email";
		case EntityType::Phone: return "Phone";
		case EntityType::BankAccount: return "Bank Account";
		case EntityType::TaxId: return "Tax ID";
		case EntityType::CaseNumber: return "Case Number";
		case EntityType::ContractIdentifier: return "Contract ID";
		case EntityType::Court: return "Court";
		case EntityType::Judge: return "Judge";
		case EntityType::Date: return "Date";
		case EntityType::MoneyAmount: return "Amount";
		case EntityType::Percentage: return "Rate";
		case EntityType::Number: return "Number";
	}
	return "Entity";
}

namespace {

// A person's name as bare lower-case words without titles: "Mr. Holt's" ->
// {"holt"}. ASCII case only - a surname in another script is compared as
// written, which can only make it look unshared and so is decided later as
// ambiguous, never as a wrong match.
std::vector<std::string> bare_person_words(const std::string &value) {
	static const std::set<std::string> honorifics{
		"mr", "mrs", "ms", "miss", "mx", "dr", "prof", "professor", "sir", "madam", "hon", "judge", "justice",
		"atty", "attorney",
	};
	std::vector<std::string> words;
	std::string word;
	const auto flush = [&] {
		if (word.size() > 2 && word.compare(word.size() - 2, 2, "'s") == 0) word.erase(word.size() - 2);
		while (!word.empty() && (word.back() == '.' || word.back() == ',' || word.back() == '\'')) word.pop_back();
		if (!word.empty() && honorifics.count(word) == 0) words.push_back(word);
		word.clear();
	};
	for (const char character : value) {
		if (character == ' ' || character == '\t' || character == '\n') flush();
		else word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}
	flush();
	return words;
}

bool is_full_person_name(const std::vector<std::string> &words) {
	return words.size() >= 2 && !(words.size() == 2 && words[0].size() == 1) && words.back().size() >= 3;
}

bool is_person(protected_api::papinian::EntityType category) {
	return category == protected_api::papinian::EntityType::Person
		|| category == protected_api::papinian::EntityType::Judge;
}

// A company's name without its legal form at the end - "Whitcombe Hale" for
// "Whitcombe Hale LLP", "Larkspur Dental Group" for "Larkspur Dental Group, P.C."
// - or the name itself when it ends in none.
std::string without_legal_form(const std::string &name) {
	static const std::vector<std::string> forms{"l.l.c.", "l.l.p.", "pllc", "llc", "llp", "l.p.", "lp", "inc.", "inc",
		"n.a.", "p.c.", "pc", "ltd.", "ltd", "limited", "corp.", "corp", "corporation", "company", "co.", "plc", "gmbh",
		"s.a.", "ag"};
	std::string trimmed = name;
	while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) trimmed.pop_back();
	std::string lowered = trimmed;
	for (char &character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	for (const std::string &form : forms) {
		if (lowered.size() <= form.size() + 1 || lowered.compare(lowered.size() - form.size(), form.size(), form) != 0)
			continue;
		const char before = lowered[lowered.size() - form.size() - 1];
		if (before != ' ' && before != ',') continue;
		std::string bare = trimmed.substr(0, trimmed.size() - form.size());
		while (!bare.empty() && (bare.back() == ' ' || bare.back() == ',')) bare.pop_back();
		return bare;
	}
	return trimmed;
}

std::size_t word_count(const std::string &value) {
	std::size_t count = 0;
	bool inWord = false;
	for (const char character : value) {
		const bool space = std::isspace(static_cast<unsigned char>(character)) != 0;
		if (!space && !inWord) ++count;
		inWord = !space;
	}
	return count;
}

} // namespace

std::vector<std::string> protection_values(const matters::Matter &matter, const ProjectEntity &entity) {
	std::vector<std::string> values = entity.aliases;
	if (std::find(values.begin(), values.end(), entity.canonical_value) == values.end())
		values.push_back(entity.canonical_value);
	if (!is_person(entity.category)) {
		// A company is written in running text without its legal form - "the
		// Whitcombe Hale register format", "Whitcombe Hale comment" - and that
		// writing is the company's too. Two words at least, so the remainder is
		// still the name and not one ordinary word; never for an entity the lawyer
		// has already corrected by removing a writing.
		const bool organisation = entity.category == protected_api::papinian::EntityType::Organisation
			|| entity.category == protected_api::papinian::EntityType::LegalEntity;
		if (organisation && entity.rejected_aliases.empty()) {
			const std::vector<std::string> recorded = values;
			for (const std::string &alias : recorded) {
				const std::string bare = without_legal_form(alias);
				if (bare != alias && word_count(bare) >= 2 && std::find(values.begin(), values.end(), bare) == values.end())
					values.push_back(bare);
			}
		}
		return values;
	}

	std::vector<std::pair<std::string, std::string>> surnames; // lower-case, as written
	for (const std::string &alias : values) {
		// A list is not a name: "Company, the Evaluation Material or the",
		// recorded as a person, gave "the" as a surname, and every "the" of a
		// Matter's requests was masked (2026-10-29).
		if (alias.find(',') != std::string::npos) continue;
		const std::vector<std::string> words = bare_person_words(alias);
		if (!is_full_person_name(words)) continue;
		const std::size_t lastSpace = alias.find_last_of(" \t");
		std::string written = lastSpace == std::string::npos ? alias : alias.substr(lastSpace + 1);
		if (written.size() > 2 && written.compare(written.size() - 2, 2, "'s") == 0) written.erase(written.size() - 2);
		while (!written.empty() && std::ispunct(static_cast<unsigned char>(written.back())) != 0) written.pop_back();
		// In a writing with capitals, a last word in lower case is a word of the
		// sentence, not a surname. A real surname that is also a word - Bell,
		// Frost - has its capital and stays masked on its own, and a name typed
		// all in lower case by accident keeps its surname as before.
		const bool hasCapital = std::any_of(alias.begin(), alias.end(), [](char character) {
			const unsigned char byte = static_cast<unsigned char>(character);
			return byte < 0x80 && std::isupper(byte) != 0;
		});
		if (written.empty() || (hasCapital && static_cast<unsigned char>(written.front()) < 0x80
			&& std::islower(static_cast<unsigned char>(written.front())) != 0)) continue;
		if (written.size() >= 3) surnames.emplace_back(words.back(), std::move(written));
	}
	for (const auto &[lowered, written] : surnames) {
		const bool shared = std::any_of(matter.entities.begin(), matter.entities.end(), [&](const ProjectEntity &other) {
			if (other.id == entity.id || other.merged_into_entity_id || !is_person(other.category)) return false;
			return std::any_of(other.aliases.begin(), other.aliases.end(), [&](const std::string &alias) {
				const std::vector<std::string> words = bare_person_words(alias);
				return !words.empty() && words.back() == lowered;
			});
		});
		if (shared) continue;
		if (std::find(values.begin(), values.end(), written) == values.end()) values.push_back(written);
	}
	return values;
}

std::map<std::string, std::string> project_entity_symbols(const matters::Matter &matter) {
	struct Ranked {
		const ProjectEntity *entity = nullptr;
		long long ordinal = 0;
	};
	std::vector<Ranked> ranked;
	for (const ProjectEntity &entity : matter.entities) {
		Ranked item{&entity, 0};
		const std::string prefix = display_category_name(entity.category) + " ";
		if (entity.pseudonym.rfind(prefix, 0) == 0) {
			try {
				item.ordinal = std::stoll(entity.pseudonym.substr(prefix.size()));
			} catch (const std::exception &) {
			}
		}
		ranked.push_back(item);
	}
	// The order they came in: when, then their kind and their number in it.
	std::sort(ranked.begin(), ranked.end(), [](const Ranked &first, const Ranked &second) {
		if (first.entity->created_at != second.entity->created_at) return first.entity->created_at < second.entity->created_at;
		if (first.entity->category != second.entity->category)
			return static_cast<int>(first.entity->category) < static_cast<int>(second.entity->category);
		if (first.ordinal != second.ordinal) return first.ordinal < second.ordinal;
		return first.entity->id < second.entity->id;
	});
	std::map<std::string, std::size_t> numbers;
	std::map<std::string, std::string> symbols;
	for (const Ranked &item : ranked) {
		const std::string prefix = protected_api::papinian::markers::prefix_for(item.entity->category, std::nullopt,
				protected_api::papinian::DetectionSource::Deterministic);
		symbols[item.entity->id] = protected_api::papinian::markers::format(prefix, ++numbers[prefix]);
	}
	return symbols;
}

} // namespace singularity::privacy
