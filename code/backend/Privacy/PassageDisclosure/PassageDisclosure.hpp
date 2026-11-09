#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "PapinianGuard/Types/PrivacyMetadata.hpp"
#include "Privacy/SentPassages/SentPassages.hpp"

// What one operation's protected requests showed a model, read for a lawyer:
// the passages as they left beside the Project's own text, and what was hidden
// and what replaced it. Everything here is re-derived when asked, from the
// operation's sent passages (Routing/RoutingLog SentPassages), its outbound
// calls, the Project's documents and its Entity Registry - never from a
// request's restoration mapping, which no longer exists by then and is never
// kept.
namespace singularity::privacy::passage_disclosure {

// One passage of the operation, as it left and as the Project has it.
struct DisclosedPassage {
	// "source" (a Project document's passage) or "instruction" (the lawyer's).
	std::string kind;
	std::string document_id;
	std::string chunk_id;
	std::string document_name;
	sent_passages::Reading reading = sent_passages::Reading::Unavailable;
	// The document has changed since; the text shown is how the passage read
	// when it was sent, kept by Singularity's own reading of that version.
	bool is_document_changed = false;
	std::vector<sent_passages::Segment> sent;
	std::vector<sent_passages::Segment> local;
	std::vector<sent_passages::HiddenValue> hidden;
	// The requests that carried it in this form: when, where and to which model.
	std::vector<protected_api::papinian::OutboundCall> calls;
};

// One relation Singularity worked out on this device over protected facts and sent
// with a request of the operation: "[[Amount7]] > [[Amount8]]". What each
// symbol stood for is read again here, from the operation's own passages and,
// for a value Singularity worked out, by working it out again - never kept.
struct DisclosedRelation {
	std::string id;
	// Closed vocabulary: the relation's kind or an operation's name.
	std::string kind;
	// The line exactly as it left.
	std::string sent;
	// The same line with every symbol's original, read on this device; nothing
	// when a value was not kept or cannot be worked out again.
	std::optional<std::string> local;
	// Why it was sent, in plain words.
	std::string why;
	// "sensitive" when the relation policy classified it so.
	std::string sensitivity;
	// When the request that carried it left, and to which model.
	std::string time;
	std::string provider_id;
	std::string model_id;
};

// One role line a request of the operation carried (DESIGN4 §5.4, §11.3):
// "[[Amount3]] MONEY(USD/month) PRICE "Base Rent" · DOC_1 §4.1" - what the
// model was told a symbol is, in the material's own words.
struct DisclosedRole {
	std::string symbol;
	// The line exactly as it left.
	std::string line;
	// The same line with each symbol's original, read on this device; nothing
	// when one cannot be read again.
	std::optional<std::string> local;
	std::string role_class;
	// "sensitive" when joining the fact to a party it names is so classified.
	std::string sensitivity;
	std::string time;
	std::string provider_id;
	std::string model_id;
};

// One spreadsheet table's block a request carried (DESIGN4 §6.4).
struct DisclosedTable {
	std::string handle;
	std::string block;
	// The model's reason, when it asked for a sheet the evidence did not touch.
	std::string why;
	std::string time;
	std::string provider_id;
	std::string model_id;
};

// One operation the model asked Singularity to work out on this device, and what
// it was sent back (DESIGN4 §2.3, §11.3): refused and failed ones included.
struct DisclosedOperation {
	std::string id;
	// The operation as the model asked it (symbols, @names, closed words).
	std::string requested;
	// ok | bounded | unknown | error | refused | not_served, and its code.
	std::string status;
	std::string code;
	// The line exactly as it was sent back; empty when none was.
	std::string returned;
	// What it worked out, with the originals, read on this device; nothing
	// when there is no result or a value cannot be read again.
	std::optional<std::string> local;
	// Whether the model learned a relation from it (a comparison's answer, a
	// difference's sign).
	bool observation = false;
	std::string sensitivity;
	// The model's own reason for asking.
	std::string why;
	// The request whose answer asked for it, and the one that carried the
	// result back.
	std::string asked_time;
	std::string asked_provider_id;
	std::string asked_model_id;
	std::string time;
	std::string provider_id;
	std::string model_id;
};

struct OperationDisclosure {
	std::string operation_id;
	// False when the operation has no record of its passages: sent before
	// Singularity kept them, not through Papinian Guard, or not at all.
	bool is_recorded = false;
	// Every value the operation's protected requests hid, labelled in the order
	// they were first sent, with the originals its passages line up with.
	std::vector<sent_passages::HiddenValue> hidden;
	// The lawyer's own instruction as it left, when it is among the passages.
	std::optional<DisclosedPassage> instruction;
	// Each document passage as it left, in the order first sent.
	std::vector<DisclosedPassage> passages;
	// Every relation the operation's requests carried, in the order sent, and
	// how many Singularity worked out but kept on this device (only counted).
	std::vector<DisclosedRelation> relations;
	std::size_t withheld_relations = 0;
	// The protected computation protocol (DESIGN4 §11.3): every role line and
	// table block the requests carried, each once, and every operation the
	// model asked for, in the order asked.
	std::vector<DisclosedRole> roles;
	std::vector<DisclosedTable> tables;
	std::vector<DisclosedOperation> operations;
};

// How a relation's line reads with its originals, and why it was sent:
// Privacy/PassageDisclosure/RelationReading.hpp.

struct PassageDisclosure {
	// "recorded": the passage was found among what the operation sent.
	// "not_kept": the operation's passages are kept, but not this one (too
	// long to keep, or never sent through Papinian Guard).
	// "unavailable": the operation's passages are kept, but this passage's
	// text cannot be read on this device now, and a passage kept without its
	// name can be found only by that text.
	// "not_recorded": the operation has no record of its passages.
	std::string state = "not_recorded";
	std::optional<DisclosedPassage> passage;
	// How many different forms of this passage left (each request of an
	// operation protects afresh); the latest is the one given.
	std::size_t form_count = 0;
};

OperationDisclosure disclose_operation(const std::string &project_id, const std::string &operation_id);

PassageDisclosure disclose_passage(const std::string &project_id, const std::string &operation_id,
		const std::string &document_id, const std::string &chunk_id);

} // namespace singularity::privacy::passage_disclosure
