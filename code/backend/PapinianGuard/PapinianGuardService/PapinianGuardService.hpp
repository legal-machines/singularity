#pragma once

#include "Context/RussianNames/RussianNames.hpp"
#include "LocalModel/Models/PapinianNER/SemanticModel/SemanticModel.hpp"
#include "PapinianGuard/PatternScanner/PatternScanner.hpp"
#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"
#include "PapinianGuard/Taxonomy/Taxonomy.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "PapinianGuard/ProtectedMarkers/MarkerStyle.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeTypes.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTableTypes.hpp"
#include "PapinianGuard/Types/FactAnchor.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace singularity::protected_api::papinian {

enum class DetectionSource {
  CustomTerm,
  Deterministic,
  SemanticModel,
};

struct CanonicalEntity {
  std::size_t start = 0;
  std::size_t length = 0;
  std::optional<EntityType> type;
  double confidence = 0.0;
  DetectionSource source = DetectionSource::SemanticModel;
  std::string source_label;
  std::string value;
  // The identity of the value this span writes (semantic::value_key), when it
  // reads as one where it stands - "18400000" before "(shown in ₽)" is rubles.
  // Empty for a name, a code, or a quantity nothing could read.
  std::string value_key;
  // The fact an earlier answer's anchor names for this writing (FactAnchor:
  // ⟪writing|fact:key⟫, ⟦writing|fact:key⟧); empty for any other span.
  std::string fact_key = {};
};

struct CanonicalLeafDetection {
  std::string identity;
  std::vector<CanonicalEntity> entities;
};

struct ProtectionBatchMetrics {
  double semantic_detection_ms = 0.0;
  double mapping_build_ms = 0.0;
  // Where this operation's semantic detections came from, so a send can say
  // what it saved: detections prepared earlier and found in the durable store,
  // detections the model ran for now, and text this same operation had already
  // scored (the same passage carried in more than one section).
  std::size_t detections_reused = 0;
  std::size_t detections_computed = 0;
  std::size_t detections_from_operation = 0;
  // What building the mapping is actually spent on, so the next thing worth
  // making faster is chosen from measurement rather than from a guess.
  double deterministic_ms = 0.0;
  double project_match_ms = 0.0;
  double reconcile_ms = 0.0;
};

struct ConfidenceThresholds {
  double default_semantic = 0.5;
  std::map<EntityType, double> per_type;

  double for_type(EntityType type) const;
  // The lowest confidence any type accepts: a detection below it is never
  // used, whatever its label. What the durable detection store keeps.
  double lowest() const;
};

struct GuardConfig {
  ConfidenceThresholds thresholds;
  bool semantic_detection_enabled = true;
  // A style every fresh operation table this guard fills writes its symbols
  // in, whatever the table's own default (markers::kMarkerStyle): a test that
  // asserts compact literals ([[E1]]) pins Compact here. Nothing: the table's.
  std::optional<markers::MarkerStyle> marker_style;
};

// Where a quantity or date FACT of the material is stated, and what it is
// there. For an amount, a rate, a count, a date or a period a symbol names a
// fact, not a value: the purchase price, the liability cap, the insurance limit
// and the claim are four facts, and four symbols, however equal their amounts.
// Their equal values are a relation Singularity knows locally (derived), never
// identity. Local only; never serialized, except `fact_key` - a digest - in a
// stored answer's anchors (FactAnchor) and in outbound provenance.
struct FactProvenance {
	// External: a fact first met in a tool result the model itself asked for (a
	// web search or a page it opened, DESIGN4 §8.1 R11) - never the lawyer's.
	enum class Origin : unsigned char { Source, ProjectContext, History, Request, Instruction, Derived, Occurrence, External };
	Origin origin = Origin::Occurrence;
	// SHA-256 of the local text of the passage, record or part it was met in.
	std::string passage_sha256;
	// Its bytes in that local text.
	std::size_t offset = 0;
	std::size_t length = 0;
	// "Ledger!B16" when a workbook reading states it; otherwise empty.
	std::string cell;
	// Its folded role label ("purchase price", "rent@2026-07"): the document's
	// own words, local; the digest never carries them readable. Empty when none.
	std::string role;
	// A derived fact only: the canonical operation over its operands' fact keys.
	std::string derivation;
	// Met through an earlier answer's anchor (⟪writing|fact:key⟫ in the
	// history): a figure restoration wrote for the lawyer and the answer check
	// verified then - traced, though no passage of this operation holds it. Never
	// part of the fact's key.
	bool anchored = false;
};

const char *to_string(FactProvenance::Origin origin);

struct RequestMappingEntry {
  std::string placeholder;
  std::string original;
  std::optional<EntityType> type;
  DetectionSource source = DetectionSource::SemanticModel;
	// Optional local Project identity link. It is never included in the
	// placeholder and never crosses the provider boundary.
	std::optional<std::string> project_entity_id;
	// Every other spelling this operation has met for the same identity: the
	// name in capitals in a signature block, without its comma, in another
	// grammatical case. Deciding that a spelling IS this identity is only half
	// of protecting it - the symbol must also replace that spelling wherever it
	// stands, and the leak checks must look for it. Restoration still writes
	// `original`, which is the person's own spelling when this request carried
	// one. Request-scoped like everything here; never serialized.
	std::vector<std::string> surface_forms;

	// This person's surname written on its own, met in this operation after the
	// full name: "Hale" after "Victor Hale", "Ms. Morgan" after "Dana Morgan",
	// "…" after "…". The same person, so the same symbol - but
	// kept apart from surface_forms, and out of every_form(), because it counts
	// only as a whole word: "Hale" is not inside "Haley", nor "Rich" inside
	// "Richard". It is replaced, and the leak check looks for it, only there.
	std::vector<std::string> surname_forms;

	// An organisation's own name written alone, met in this operation: the name
	// without its legal form and the words any company's name has -
	// "Meadowbrook" of "Meadowbrook Industrial Partners LLC", in prose, in a file
	// name ("Lease - Meadowbrook Cold Storage.docx") or in a journal record - or
	// the name it gives in quotes, «…» of …. The same
	// organisation, so the same symbol, sent as its short part ([[E3.short]]) and
	// written back as it was written, so a file named with it is still found by
	// that name. Like a surname alone, kept out of every_form() and matched only
	// as a whole word; never when two organisations here share it.
	std::vector<std::string> short_forms;

	// How many times each spelling was met in this request, in the order they
	// were first met. A data room writes the same defined term both ways -
	// "Change of Control" eighteen times, "Change of control" five - and the
	// spelling an answer is written in must be the record's own, not whichever
	// chunk happened to be read first.
	std::vector<std::pair<std::string, std::size_t>> writing_counts;

	// Whether `original` is the spelling the person used in their own request.
	// Once it is, a spelling met later in a document never displaces it.
	bool original_is_user_authored = false;

	// The value this row stands for, when its writing reads as one: an amount,
	// a rate, a date, a plain quantity. One value is one row however it is
	// written - "…",
	// "…" and a workbook's "18400000 (shown in ₽)" are one
	// symbol in prose, Word and Excel alike. `value_key` is its
	// semantic::value_key; both empty for a name or a code. Request-scoped.
	std::optional<semantic::Value> value;
	std::string value_key;

	// The honorific a source wrote right before this person, outside the name
	// the checkpoint returned: "Mr." of "Mr. Marcus Delgado". What "Dear
	// [[E5]]" is written with. Request-scoped.
	std::string honorific;

	// A Russian person's gender as the sources' sentences show it where the
	// name alone does not: «…» is a man -
	// a woman there would be «…» - though «…» alone could
	// be either. Unknown when nothing shows it. Request-scoped.
	context::russian_names::Gender gender = context::russian_names::Gender::Unknown;

	// A value Singularity worked out itself on the lawyer's computer - a total, a
	// difference, a deadline, the days between two dates - and never met in any
	// text: no source writes it, so it has no writing but its display. Issued
	// by PapinianGuard::issue_derived_value. Request-scoped.
	bool derived = false;
	// A derived value the lawyer's material writes openly (a band's rate a
	// lookup chose): Singularity's own like a count - its bare writing elsewhere is
	// never looked for as its leak, nor read as it (counted_by_singularity).
	bool open_value = false;

	// Where each writing was met, as an OR of WritingOrigin: a document's text,
	// a file's name, the lawyer's own request, a model's earlier answer. Which
	// writing an identifier is written back in depends on where it stands.
	// Request-scoped; never serialized.
	enum WritingOrigin : unsigned { DocumentText = 1, FileName = 2, LawyerRequest = 4, ModelAuthored = 8 };
	std::map<std::string, unsigned> writing_origins;

	// A quantity or date row is one FACT (FactProvenance): its key - a digest of
	// where and in what role the material first states it - and that place.
	// `unified_keys` are the keys of other places proven to state this same
	// fact (one clause's figures and words, a cell and the prose naming it, a
	// table row and the workbook row the documents tie together, the same
	// defined role). Empty for a name or a code, which keep their identity by
	// writing. Request-scoped; only the digest ever leaves process memory.
	std::string fact_key;
	FactProvenance provenance;
	std::vector<std::string> unified_keys;

	void count_writing(const std::string &value) {
		for (auto &counted : writing_counts) {
			if (counted.first == value) { ++counted.second; return; }
		}
		writing_counts.emplace_back(value, 1);
	}

	[[nodiscard]] std::size_t times_written(const std::string &value) const {
		for (const auto &counted : writing_counts) {
			if (counted.first == value) return counted.second;
		}
		return 0;
	}

	// `original` and every other spelling, for whoever must find them all.
	[[nodiscard]] std::vector<std::string> every_form() const {
		std::vector<std::string> forms{original};
		forms.insert(forms.end(), surface_forms.begin(), surface_forms.end());
		return forms;
	}
};

struct WrittenValueCache;

// Ephemeral protection identity for one complete model operation. The table
// is owned by the execution spine and reused across provider turns. Original
// values never leave process memory and this type is never serialized.
struct OperationProtectedSymbolTable {
	std::string operation_id;
	std::string marker_family;
	// A random id this table's calls are recorded under, given when it first
	// sends (OutboundCall::symbol_scope): markers are numbered per table, so a
	// record reads "[[E1]]" only against the calls of the same table.
	std::string outbound_scope;
	std::map<std::string, std::vector<std::string>> project_entity_aliases;
	std::map<std::string, EntityType> project_entity_types;
	// Of those aliases, the ones the lawyer confirmed (the registry's locked
	// aliases): masked as they are, whatever the Guard's vocabulary says. Every
	// other alias an automatic pass recorded is masked only while it still names
	// an identity (recorded_value_names_identity).
	std::map<std::string, std::set<std::string>> project_entity_curated_aliases;
	// The symbol each Project identity stands under in every request and
	// document of the Project: its row takes it here, and a value of no
	// identity is numbered past them.
	std::map<std::string, std::string> project_entity_symbols;
	std::vector<RequestMappingEntry> mapping;
	// Raw local-model detections, keyed by the EXACT leaf text they were
	// produced from (never a hash - a live production run measured Papinian's
	// own semantic detection re-running unchanged over the SAME growing
	// prompt content, mostly static instruction/system text and already-
	// inspected evidence, on every single turn of one operation - 77.9% of
	// total wall time on that one operation). collect_sensitive_values_batch()
	// is the only writer/reader; a leaf whose text is not byte-identical to
	// a prior turn's own leaf is never a cache hit, so the model only ever
	// runs on text it has not already scored THIS operation. Deliberately
	// caches only the RAW ModelEntity spans (a pure function of leaf text,
	// the local model's own detect_batch()), never anything policy/threshold-
	// scoped - semantic_entities_from()'s own confidence-threshold filtering
	// still runs fresh on every call, cached or not, so a change to that
	// runtime config always takes effect immediately for both.
	std::map<std::string, std::vector<ModelEntity>> semantic_detection_cache;
	// Derived lookups, never identity of their own: the comparison form of every
	// value already compared in this operation, and every Project alias by that
	// form. Without them each detected value re-normalised every alias of every
	// Project entity - for a data room, hundreds of thousands of Unicode
	// normalisations per request. Rebuilt whenever the aliases change.
	mutable std::map<std::string, std::string> comparison_form_cache;
	mutable std::map<std::string, std::vector<std::string>> alias_entities_by_form;
	mutable std::size_t alias_index_signature = static_cast<std::size_t>(-1);
	// One more derived lookup of the same kind: the automaton that finds every
	// spelling this operation knows, in one pass over a passage. Building it
	// costs the length of the spellings, so it is built when they change and
	// not once per passage - the whole point is to stop paying per passage for
	// something that does not change while the passages are read.
	// `spelling_signature` is what it was built from.
	mutable std::shared_ptr<const PatternScanner> identity_scanner;
	mutable std::vector<std::pair<std::size_t, std::string>> identity_scanner_forms;
	mutable std::size_t spelling_signature = static_cast<std::size_t>(-1);
	// The same for the Project's own names, which do not change while a request
	// is read at all: one automaton for names written exactly as recorded, one
	// for plain-Latin names matched whatever their case.
	struct AliasPatternRecord {
		std::string alias;
		std::string needle;
		EntityType type = EntityType::Person;
		// Confirmed by the lawyer: masked wherever it stands.
		bool curated = false;
	};
	mutable std::shared_ptr<const PatternScanner> alias_scanner_exact;
	mutable std::shared_ptr<const PatternScanner> alias_scanner_folded;
	mutable std::vector<AliasPatternRecord> alias_scanner_patterns;
	mutable std::size_t alias_scanner_signature = static_cast<std::size_t>(-1);
	// And one for surnames: every writing of a person row's surname ("Hale";
	// "…") with the rows it can name, and the
	// first letter of each row's given name, which tells Victor Hale from Maria
	// Hale. Kept row by row: reading passage after passage derives again only a
	// row that is new or has been written another way since. A Russian
	// surname's writings are the ones its bearer's gender takes: «…»
	// is never … never …
	struct SurnameRecord {
		std::size_t signature = static_cast<std::size_t>(-1);
		std::string initial;
		std::vector<std::string> forms;
		// The given name written out ("Marcus", "…"), and the first letter
		// of every name before the surname in order - the given name's, then a
		// middle name's or a patronymic's ("…" for …).
		std::string given;
		std::vector<std::string> initials;
	};
	mutable std::vector<SurnameRecord> surname_records;
	mutable std::map<std::string, std::vector<std::size_t>> surname_rows_by_form;
	// Every quantity and date each text writes, found once per text (the same
	// passage is read for values when it is collected, settled, transformed
	// and checked). Derived from the text alone, so a frozen copy of the table
	// shares it. Defined with its reader in PapinianGuardService.cpp.
	mutable std::shared_ptr<WrittenValueCache> written_values;
	// Each value Singularity worked out itself (a total of a column, a deadline),
	// by the canonical description of how it was worked out, to its row: the
	// same derived fact keeps its symbol across the calls of one operation.
	std::map<std::string, std::size_t> derived_by_derivation;
	// The fact registry: every fact key (a row's own and every key unified with
	// it) to its row; and every quantity or date row by its value - for finding
	// writings of a value (the leak checks) and for LOCAL equality only, never
	// identity: two facts of one value are two rows.
	std::map<std::string, std::size_t> fact_by_key;
	std::multimap<std::string, std::size_t> facts_by_value;
	// The same registry by the two other proofs a place can give: the workbook
	// cell it is ("Ledger!B19", and "B19" when only one sheet has that cell),
	// and its role with its value (FactProvenance::role + value key). Local.
	std::map<std::string, std::size_t> fact_by_cell;
	std::map<std::string, std::size_t> fact_by_role;
	// How many facts a repair had to issue for a place no fact could be proven
	// to be (issue_occurrence_fact): counted in PROTECT_END.
	std::size_t occurrence_facts_issued = 0;
	// How this table writes its symbols ([[Amount3]] or [[E3]]), and how many of
	// each typed prefix it has issued. Tests that assert compact literals pin
	// Compact on their table.
	markers::MarkerStyle marker_style = markers::kMarkerStyle;
	std::map<std::string, std::size_t> numbers_by_prefix;
	// Relations the operations executor answered for the model and that wait
	// for the call that carries them: noteOutboundCall attaches each record to
	// the call whose prompt holds its line. Local provenance; never sent.
	std::vector<OutboundRelation> pending_relations;
	// Every relation line this call's prompt carried with what Singularity worked
	// out in it (its derived results): what the completeness gate checks an
	// answer uses. Local; never sent again from here.
	std::vector<OutboundRelation> surfaced_relations;
	// The lawyer's request the relations planner serves - protected, and raw
	// for its intents (local only, process memory) - as the attempt's first
	// call read it: a later call of the same attempt (the review of a draft, a
	// correction) plans for that task, never for the text Singularity assembled
	// around it (live EN1 c22: a reviewer call's request carried the draft's
	// every figure, and each was related to the material as if the lawyer had
	// written it).
	std::string planner_question;
	std::string planner_raw_question;
	// The lawyer's material this attempt has sent the model, call by call - its
	// sources and the lawyer's own answers, as the answer contract reads them
	// (answer_contract::remember_sent_material) - so a figure the material
	// writes openly grounds the answer in a later call whose retrieval left
	// that passage out (live EN1 c25: a redraft call carried two documents,
	// and the agreement's "forty-five (45) days" was refused as a raw figure).
	// Protected text only (what was sent); never the model's words.
	std::string sent_material;
	std::set<std::size_t> sent_material_digests;
	// Where restoration wrote each fact back in this operation: the sentence of
	// the restored text around it, where in that sentence it stands, and the
	// fact it is - by the writing (record_restored_facts). An answer's figure
	// sent again later in the same operation - a draft under review, a
	// correction - is the fact it was written for only where that very sentence
	// of the answer stands around it again: the text is the answer's own, which
	// proves the fact. A figure that merely writes the same value (a passage read
	// later, a payment of the same amount) is never joined to it by its writing.
	struct RestoredPlace {
		std::string sentence;
		std::size_t offset = 0;
		std::string key;
	};
	std::map<std::string, std::vector<RestoredPlace>> restored_writings;
	// Which document each passage of this operation's material belongs to, by the
	// passage's digest (SHA-256 of its text as sent to detection): a local scope
	// digest, never a name. A defined term, a table's role and a sentence read
	// twice say the same fact only within one document - two leases that both
	// define "Monthly Rent" as $5,000 are two facts. A text with no document
	// (a history record, the request) is a scope of its own. Filled by
	// ProtectedInference from the SOURCE_MATERIAL markers before collection.
	std::map<std::string, std::string> document_of_passage;
	// Passages that hold a table, by digest, folded: what A4 reads to see whether
	// a document names the workbook sheet a row of its table is tied to.
	std::map<std::string, std::string> table_passages;

	// ---------------------------------------------------------------------
	// The protected computation protocol (DESIGN4 §4.1). Empty until a model
	// asks for an operation; request-scoped like everything here.
	//
	// Every value worked out at the model's request, by its symbol: exact
	// value, type, AST over fact keys, transitive roots, certainty.
	std::map<std::string, compute::DerivedFact> derived_facts;
	// Every operation asked for, in order, as parsed (symbol plane, no value).
	std::vector<compute::OpRecord> op_log;
	// The model's result names (bind labels) of this task, to their symbols.
	std::map<std::string, std::string> compute_labels;
	// Names the model gave a result that came back as a bound: never a value
	// to work with, said so when used (never "no such name").
	std::set<std::string> bounded_labels;
	// Every symbol any call of this operation carried or issued: what an
	// operation may name (anything else is `unknown_symbol`).
	std::set<std::string> sent_symbols;
	// Charged observations: the matter's counts (loaded from the Matter) and
	// this task's.
	compute::ObservationLedger observations;
	// The local table models, schedules and business-day calendars the
	// operation's material holds, by handle ([[Table1]], [[Schedule1]],
	// [[Calendar1]]); built once per operation.
	std::vector<tables::ProtectedTable> tables;
	std::vector<tables::ProtectedSchedule> schedules;
	std::vector<compute::CalendarHandle> calendars;
	bool tables_built = false;
	// Budgets spent by this task (ComputeBudget.hpp).
	std::size_t compute_rounds_used = 0;
	std::size_t ops_used = 0;
	std::size_t repair_rounds_used = 0;
	// The provider and model family whose calls wrote this task's bind labels
	// (DESIGN4 §5.5): a label is re-sent only within that chain.
	std::string provider_chain;
	// What a model wrote, as the provider sent it (protected bytes, never
	// restored), by "<call>:<field>": re-sent to a later call verbatim
	// (DESIGN4 §8.7, D13).
	std::map<std::string, std::string> model_authored;
	// The protected text of a prompt prefix already sent, by the SHA-256 of
	// its local text (DESIGN4 §1.7 suffix protection).
	std::map<std::string, std::string> protected_prefixes;
};

// Files each passage under the document it belongs to
// (OperationProtectedSymbolTable::document_of_passage): each entry is a
// document's handle within one prompt ("DOC_2", made unique by the caller) and
// the digest of one of its passages. A document keeps the scope an earlier
// call of the operation gave any of its passages; otherwise its scope is the
// digest of its passages here. A passage two documents both hold word for
// word belongs to neither, and never joins them.
void file_passages_under_documents(OperationProtectedSymbolTable &symbols,
		const std::vector<std::pair<std::string, std::string>> &passages);

// Records every fact restoration wrote into `restored_text` (its anchors), with
// the sentence it stands in, for the rest of this operation: what an answer
// sent again is found to be (OperationProtectedSymbolTable::restored_writings).
// A sentence no longer than the figure by a dozen bytes proves nothing and is
// not recorded.
void record_restored_facts(OperationProtectedSymbolTable &symbols, const std::string &restored_text,
		const std::vector<FactAnchor> &anchors);

// A person's name taken apart, each part in the nominative, for a model that
// writes one part alone: [[E5.first]], [[E5.last]], [[E5.patronymic]]. Read
// from the fullest writing of the row; a part the writings do not show is empty.
struct PersonNameParts {
	std::string first;
	std::string last;
	std::string patronymic;
};

// The parts of a PERSON or JUDGE row's name; nothing for any other row, or a
// name no writing of which shows a first name and a surname.
[[nodiscard]] std::optional<PersonNameParts> person_name_parts(const RequestMappingEntry &entry);

// The name an ORGANISATION or LEGAL_ENTITY row was met by alone in this
// operation ("Meadowbrook"), which [[E3.short]] writes; nothing for any other
// row, or one never written so.
[[nodiscard]] std::optional<std::string> organisation_short_name(const RequestMappingEntry &entry);

// Whether a value an automatic detection found in a document is worth
// recording in the Project's registry, where it is masked in every later
// request: never what it is regardless of the passage it stood in - prose the
// document wrote in lower case, public legal vocabulary, a role, a title or a
// public office, a contract "identifier" with no figure made only of ordinary
// words ("Stock Purchase Agreement" is what kind of paper it is). Persons are
// recorded by name as before; a value the lawyer adds is never judged here.
[[nodiscard]] bool detection_may_be_recorded(EntityType type, const std::string &value);

// Whether a value an automatic pass recorded in the registry - an alias the
// lawyer never confirmed - still names an identity: detection_may_be_recorded()
// and the Guard's public vocabulary, judged on the value alone. An ordinary word
// stays an identity: a client may be called "Target", "Apple" or "State" and be
// known by nothing longer. Where such a word stands inside a public phrase ("the
// State of Delaware") is decided where it stands, not here. A confirmed alias
// and a custom term are masked without asking this.
[[nodiscard]] bool recorded_value_names_identity(EntityType type, const std::string &value);

// One place a validated text still writes a protected value: the row whose
// writing it is, the writing found, and where it stands in the text that was
// checked. What a repair masks with that row's own symbol.
struct LeakOccurrence {
	// AnchorMark: a local fact-anchor mark (⟪…|fact:…⟫) the transform did not
	// replace - never repaired by unmasking, always refused.
	enum class Kind { Writing, SurnameOrShort, Quantity, Date, DayOfYear, OperandMark, AnchorMark };
	std::size_t row = 0;
	std::string form;
	std::size_t start = 0;
	std::size_t length = 0;
	Kind kind = Kind::Writing;
};

struct GuardValidation {
  bool masking_complete = true;
  std::vector<std::string> leaked_values;
  std::vector<LeakOccurrence> occurrences;
};

struct ProtectionResult {
  std::string protected_text;
  std::string request_namespace;
  std::vector<CanonicalEntity> entities;
  // Request-scoped only. Never persist, serialize to ordinary product state,
  // log, or pass to a provider.
  std::vector<RequestMappingEntry> mapping;
  GuardValidation validation;
};

// Where a restored text stands, when the caller knows: in prose (anything
// else), as a workbook cell's whole value - which the workbook writer takes
// only as a plain number where the cell holds one - or as an exact quotation
// of a document's own text (a replacement's match text).
enum class RestorationField { Prose, Cell, Quote };

struct RestorationResult {
  std::string text;
  bool integrity_ok = true;
  std::vector<std::string> unknown_or_malformed_placeholders;
	std::vector<std::string> normalized_placeholders;
  // Each phrase restoration had written twice side by side where the model
  // wrote it once - one value under two symbols, a value beside the same words
  // the model wrote itself - and wrote once instead, as the lawyer reads it.
  std::vector<std::string> written_once;
  // Every quantity or date piece restoration wrote, in `text`'s coordinates,
  // with the fact it is: what a stored answer keeps (MatterTask::fact_anchors)
  // so the next turn sends that occurrence as the same fact again.
  std::vector<FactAnchor> fact_anchors;
};

// The row a symbol written in a text names, typed ("[[Amount3]]") or compact
// ("[[E17]]"), exactly as issued or as a model varied it ("[[ amount3 ]]");
// nothing for a symbol the table never issued.
[[nodiscard]] const RequestMappingEntry *row_of_marker(const OperationProtectedSymbolTable &symbols,
		std::string_view marker);

// relocate_fact_anchors, carry_fact_anchors, mark_fact_anchors and strip_fact_anchor_marks - the
// pure anchor helpers a stored answer and the history renderer use - are in
// PapinianGuard/Types/FactAnchor.hpp.

class PapinianGuard {
public:
  explicit PapinianGuard(std::shared_ptr<const ISemanticModelAdapter> semantic_model,
                         GuardConfig config = {});

  ProtectionResult protect(const std::string &utf8_text, const Policy &policy,
                           const std::string &request_namespace,
						   OperationProtectedSymbolTable *operation_symbols = nullptr) const;
  // Typed stateful callers use these as two distinct logical passes: collect
  // every sensitive surface into one operation table, freeze that table, then
  // apply it without invoking detection or granting transformation authority
  // to create another symbol.
  std::vector<CanonicalEntity> collect_sensitive_values(
      const std::string &utf8_text, const Policy &policy,
      const std::string &request_namespace,
      OperationProtectedSymbolTable &operation_symbols) const;
  // The values a text carries that this Project and this firm already know -
  // registered Project entities, custom terms and the deterministic kinds -
  // issued into the operation's table with no semantic scoring at all. For
  // text Singularity authored itself (a capability instruction, a coverage
  // statement): ordinary instruction vocabulary must never become a symbol,
  // yet a Project's own name inside it is still that Project's name, and a
  // frozen table that has never seen it can only fail the request closed.
  // `origin` is where the text comes from, for the facts it states
  // (FactProvenance): Singularity's own instruction text by default; a request's
  // passages read again against the settled table give their own.
  std::vector<CanonicalEntity> collect_known_values(
      const std::string &utf8_text, const Policy &policy,
      const std::string &request_namespace,
      OperationProtectedSymbolTable &operation_symbols,
      FactProvenance::Origin origin = FactProvenance::Origin::Instruction) const;
  std::vector<CanonicalLeafDetection> collect_sensitive_values_batch(
      const std::vector<SemanticLeaf> &leaves, const Policy &policy,
      const std::string &request_namespace,
      OperationProtectedSymbolTable &operation_symbols,
      ProtectionBatchMetrics *metrics = nullptr) const;
  std::string apply_frozen_mapping(
      const std::string &utf8_text,
      const OperationProtectedSymbolTable &frozen_symbols) const;
  // apply_frozen_mapping for many short texts masked with one frozen table -
  // a protection legend's labels: the table's index of rows is built once,
  // not for every text. The table must outlive the mapper and stay as it is
  // while the mapper is used; each text is masked exactly as
  // apply_frozen_mapping masks it.
  class FrozenMapper {
   public:
    FrozenMapper(FrozenMapper &&) noexcept;
    ~FrozenMapper();
    std::string apply(const std::string &utf8_text) const;
    struct Prepared;

   private:
    friend class PapinianGuard;
    FrozenMapper(const PapinianGuard &guard, const OperationProtectedSymbolTable &frozen_symbols);
    const PapinianGuard *guard_;
    const OperationProtectedSymbolTable *symbols_;
    std::unique_ptr<Prepared> prepared_;
  };
  FrozenMapper frozen_mapper(const OperationProtectedSymbolTable &frozen_symbols) const;
  // Runs against the exact final provider content payload. Any mapped raw
  // value still present is a blocking validation failure.
  GuardValidation validate_outbound_payload(const std::string &exact_payload,
                                             const ProtectionResult &request) const;
  // Repair before refusing: every place `validation` found in `protected_text`
  // a writing of a row the frozen table holds is masked with that row's own
  // symbol - a writing the transform did not reach (a name glued to a number,
  // a value next to a symbol it had not stood beside before). No row is ever
  // issued. A place no row is named for, or whose bytes are not the writing
  // found (an escape inside it), is left as it is, and the caller's next
  // validation still refuses the send.
  std::string mask_leak_occurrences(const std::string &protected_text, const GuardValidation &validation,
      const std::vector<RequestMappingEntry> &mapping) const;
  // The same repair with the operation's table, which a place an amount or a
  // date still stands may need a fact of its own in (issue_occurrence_fact):
  // the fact its role names, else the one fact of its value, else a new
  // occurrence fact - never another fact because their values are equal.
  std::string mask_leak_occurrences(const std::string &protected_text, const GuardValidation &validation,
      OperationProtectedSymbolTable &operation_symbols) const;
  RestorationResult restore(const std::string &provider_text,
                            const ProtectionResult &request,
                            RestorationField field = RestorationField::Prose) const;

  std::optional<SemanticModelIdentity> semantic_model_identity() const;

  // The symbol of a value Singularity worked out itself (ProtectedRelations,
  // ProtectedOperations): a fact named by `derivation` alone - the canonical
  // operation over its operands' facts. The same derivation keeps its symbol
  // while it comes to the same value; an equal value the material writes, or
  // one worked out another way, is another fact. Typed by `type`. Never scores
  // text and never changes a row's writings.
  std::string issue_derived_value(OperationProtectedSymbolTable &operation_symbols,
      const semantic::Value &value, EntityType type, const std::string &derivation) const;

  // A symbol of its own for one place a validated text still writes a value
  // that no fact of the table can be proven to be (repair only): a fact of
  // that value met at that place (FactProvenance::Origin::Occurrence). Never
  // scores text; the rows a request was frozen with are untouched.
  std::string issue_occurrence_fact(OperationProtectedSymbolTable &operation_symbols,
      const semantic::Value &value, EntityType type, const FactProvenance &provenance) const;

  // The symbol of a value Singularity worked out at the model's request (DESIGN4
  // §4.2): keyed only by `fact.ast`, so the same operation on the same facts
  // keeps its symbol and an equal value worked out another way is another
  // fact. Fills `fact.placeholder` and `fact.fact_key`, keeps the fact in
  // `derived_facts`, and returns the symbol. issue_derived_value becomes its
  // thin wrapper.
  std::string issue_derived_fact(OperationProtectedSymbolTable &operation_symbols, compute::DerivedFact &fact) const;

  // The symbol of a table cell's fact that no prompt carried yet (a row never
  // retrieved, DESIGN4 §6.2): a Source fact of that cell, under the same
  // category rules as protection. Never scores text.
  std::string issue_cell_fact(OperationProtectedSymbolTable &operation_symbols,
      const semantic::Value &value, EntityType type, const FactProvenance &provenance) const;

  // Scores `utf8_texts` with the local semantic model ahead of time and keeps
  // the raw detections in the process SemanticDetectionStore, so a later send
  // of byte-identical text reuses them. Does nothing without a store that
  // serves this guard's model. Returns how many texts were newly scored.
  std::size_t warm_semantic_detections(const std::vector<std::string> &utf8_texts) const;

private:
  std::string apply_frozen_mapping_with(const std::string &utf8_text, const OperationProtectedSymbolTable &frozen_symbols,
      const FrozenMapper::Prepared &prepared) const;
  std::shared_ptr<const ISemanticModelAdapter> semantic_model_;
  GuardConfig config_;
};

} // namespace singularity::protected_api::papinian
