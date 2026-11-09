#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace singularity::protected_api::papinian {

// One protected value as it left this device inside a request: the marker the
// provider received in its place ("[[E3]]"), the kind of thing it stands for,
// and the Project identity it belongs to when the Entity Registry knows one.
// The marker is the request's own text, never the value behind it.
struct OutboundMarker {
	std::string marker;
	std::string category;
	std::string project_entity_id;
};

// One passage of a protected model request exactly as it left this device: a
// document passage or the lawyer's own instruction, cut from the request that
// was sent, markers and all. What it stood for is never kept: the passage is
// found again on this device by the digest of its own local text, and what
// each marker stood for is read from that local text when a lawyer asks
// (Privacy/SentPassages). Held in memory until the operation's entry is
// written; then kept once per operation beside the Routing Log
// (RoutingLog.cpp), never in the log itself, a trace or a log line.
struct OutboundPassage {
	// "source" (a passage of a Project document), "instruction", or "context"
	// (one record of what the Project remembers that carried a marker).
	std::string kind;
	// The passage's stable identity when the request named it (document id,
	// the document version's fingerprint and the passage's place in it).
	std::string chunk_id;
	// SHA-256 of the passage's local text as the request was built from it.
	std::string content_sha256;
	// Exactly the text that left.
	std::string sent;
};

// One operand of a relation Singularity sent (OutboundRelation): the fact of the
// lawyer's material the marker stood for, by WHERE it is stated - never by its
// value. The passage is found again on this device by the digest of its own
// local text (the lookup SentPassages uses), and the value is read there.
struct RelationSource {
	// The marker as it left ("[[Amount7]]").
	std::string marker;
	// "source", "context", "history", "request", "derived", "occurrence" or
	// "external" (a page the model itself asked for).
	std::string origin;
	// SHA-256 of the local text of the passage the fact is stated in.
	std::string passage_sha256;
	std::size_t offset = 0;
	std::size_t length = 0;
	// "Ledger!B16" when a workbook reading states it; otherwise empty.
	std::string cell;
};

// One relation Singularity worked out on this device over protected facts and sent
// (or held back) with a model request: "[[Amount7]] > [[Amount8]]",
// "[[Amount9]] = [[Amount7]] - [[Amount8]]". Local provenance only - kept in
// the operation's outbound record beside the call, never sent to a provider,
// and never holding a raw value: what an operand or a result stands for is
// re-derived on this device when the lawyer asks (PrivacyBridge).
struct OutboundRelation {
	// "R<n>", unique within its call.
	std::string id;
	// Closed vocabulary: a relation kind (column_total, row_balance, compare...)
	// or an operation's name.
	std::string kind;
	// The line exactly as it left: symbols and closed words.
	std::string sent;
	// The markers it relates, and the markers it defines (derived facts).
	std::vector<std::string> operands;
	std::vector<std::string> results;
	// One per operand that is a fact of the material (not a derived one).
	std::vector<RelationSource> sources;
	// How a result was worked out, over markers: "[[Amount7]] - [[Amount8]]",
	// "business_days_between([[Date3]], [[Date4]], rule)".
	std::string derivation;
	// The business-day rule a calendar relation followed: "document_definition",
	// "incorporated_definition", "matter_rule", "jurisdiction:us_federal",
	// "jurisdiction:us_federal_reserve", "jurisdiction:ru_production",
	// "weekday_fallback"; empty when no calendar was used.
	std::string business_day_rule;
	// Where that rule is defined, when a document defines it.
	RelationSource business_day_rule_source;
	// The most sensitive category among its operands ("MONEY_AMOUNT").
	std::string value_sensitivity;
	// "ordinary" or "sensitive" (Policy.hpp: RelationSensitivity).
	std::string relation_sensitivity = "ordinary";
	// "sent" or "withheld".
	std::string decision = "sent";
	// Closed: "question_names_operand", "question_intent:<intent>",
	// "model_operation", "lawyer_value_in_request", "sheet_total_check".
	std::string justification;
	// "planner" or "model".
	std::string requested_by = "planner";
	// A closed intent word or "operation:<n>"; never free text.
	std::string subproblem;
};

// One symbol's role line as it left (DESIGN4 §5.4, §11.1): the material's own
// words beside a symbol ("[[Amount3]] MONEY(USD/month) PRICE \"Base Rent\" ·
// Lease §4.1"). Symbols and words the payload already carried; never a value.
struct OutboundRole {
	std::string symbol;
	// The line exactly as sent.
	std::string line;
	std::string source;
	std::string role_class;
	// Policy classify_relation for a line that joins a fact and a party.
	std::string value_sensitivity;
	std::string relation_sensitivity = "ordinary";
	bool withheld_by_policy = false;
};

// One table block as it left (DESIGN4 §6.4, §11.1).
struct OutboundTable {
	std::string handle;
	// The block exactly as sent.
	std::string block;
	// Headers, titles, sheet names and row labels from regions the retrieved
	// evidence did not hold, with their cells.
	std::vector<RelationSource> disclosed_cells;
	// The op_id of the describe_tables that asked for it, and its why; empty
	// when the evidence itself touched the table.
	std::string requested_by_op;
	std::string justification;
};

// One local operation the model asked for and what it was told (DESIGN4
// §2.3, §11.1): requested in one call's response, returned in a later call's
// body. Symbols, codes and closed words only.
struct OutboundOperation {
	// "c2.r1.q3".
	std::string op_id;
	// The op as the model asked it, symbol plane.
	std::string requested;
	// payload_sha256 of the call whose response carried the request.
	std::string asked_in_reply_to;
	// ok | bounded | unknown | error | refused | not_served, and its code.
	std::string status;
	std::string code;
	// The result line exactly as sent back.
	std::string returned;
	std::vector<std::string> results;
	// A relation the model learned.
	bool observation = false;
	// Transitive roots (passages, cells).
	std::vector<RelationSource> sources;
	std::string justification;
	std::string value_sensitivity;
	std::string relation_sensitivity = "ordinary";
};

// A picture a model call carried as it is, unmasked: the member's own
// attachment, named as the Matter names it, with its size and SHA-256 - never
// its bytes.
struct OutboundPicture {
	std::string name;
	std::size_t bytes = 0;
	std::string sha256;
};

// What the provider's own record says a call cost, where it keeps one:
// OpenRouter's generation record, asked by the call's id with the firm's key
// after the call. Kept beside the cost the call reported, never in its place.
struct CostConfirmation {
	// "openrouter".
	std::string source;
	std::optional<long long> cost_micros;
	std::string currency;
	std::optional<long long> input_tokens;
	std::optional<long long> output_tokens;
	// Who served it and which model, as the record names them.
	std::string provider;
	std::string model;
	// The provider's time for the call, and when Singularity asked.
	std::string created_at;
	std::string confirmed_at;
	// The SHA-256 of the record exactly as it came.
	std::string record_sha256;
};

// One real call that sent something off this device: a model request, a web
// search or a page opened through a research provider. Written at the moment
// the call is made, from what the call actually sent - never reconstructed
// later, and never with a value Papinian Guard kept on this device.
struct OutboundCall {
	// "model", "web_search" or "web_open".
	std::string kind;
	std::string time;
	std::string provider_id;
	std::string model_id;
	// The protection the call was sent under: "off" when it left as written.
	std::string protection_mode = "off";
	// Exactly the markers that stand in what was sent, each once.
	std::vector<OutboundMarker> markers;
	std::map<std::string, std::size_t> category_counts;
	// The page address a web.open call sent; public, and already kept with the
	// external evidence it opened.
	std::string url;
	std::string payload_sha256;
	// What came back for a model call, as the bill of its work reads it: the
	// provider's own id for the request, the fingerprint of the response, and
	// this call's own tokens and cost, as the provider reported them. Empty
	// for a call made before Singularity kept them, or one that failed before
	// its answer came.
	std::string provider_request_id;
	std::string response_sha256;
	std::optional<long long> input_tokens;
	std::optional<long long> cached_input_tokens;
	std::optional<long long> output_tokens;
	std::optional<long long> reasoning_tokens;
	std::optional<long long> cost_micros;
	std::string cost_currency;
	// "actual" or "estimated", and how it was known ("provider_reported").
	std::string cost_classification;
	std::string cost_methodology;
	// The provider that served it, where a gateway names it.
	std::string served_by;
	// Which marker numbering the call's markers belong to. Every protection
	// table numbers its markers afresh ("[[E1]]" again), and one operation can
	// hold calls of several tables (a source picker's own request, a task
	// resumed after a restart): a marker names a value only within its table.
	// A random id given to the table when it first sends; it names nothing
	// else and is never a request namespace.
	std::string symbol_scope;
	// Each passage of a protected model request as it left, each once.
	std::vector<OutboundPassage> passages;
	// The pictures it carried as they are, each once.
	std::vector<OutboundPicture> pictures;
	// This computer's record of the model call: the chain it was written into
	// and its place there. Empty for a call made before Singularity kept the
	// record, or on a computer that keeps none.
	std::string record_chain;
	std::optional<long long> record_index;
	// The provider's own record of its cost, once asked (usage.confirmCosts).
	std::optional<CostConfirmation> confirmation;
	// The relations this call carried, with their local provenance, and those
	// Singularity worked out for it but held back (relation sensitivity).
	std::vector<OutboundRelation> relations;
	std::vector<OutboundRelation> withheld_relations;
	// The protected computation protocol (DESIGN4 §11.1): the role lines and
	// table blocks this call's body carried, the operations the model asked
	// for in this call's RESPONSE, and the results this call's BODY carried.
	std::vector<OutboundRole> roles;
	std::vector<OutboundTable> tables;
	std::vector<OutboundOperation> operations_requested;
	std::vector<OutboundOperation> operations_returned;
};

// The complete, deliberately non-sensitive privacy record that may cross the
// request boundary into Matter history/UI. It must never contain namespaces,
// detected text, or restoration-map entries. The only placeholder syntax it
// carries is the markers that left in outbound_calls: what the provider saw,
// never what they stand for.
struct PrivacyMetadata {
  std::string mode = "off";
  // What became of the request at the protection boundary, said without any
  // value: "sent" (validated, then delivered), "blocked" (protection refused
  // it before anything left this computer), "failed" (it failed elsewhere,
  // before or after protection) or "local" (it never left this computer).
  // A blocked request's `decision_reason` is Papinian Guard's own words for
  // it, by kind and place ("could not hide one protected person name in the
  // request"), never a value. Empty on an entry written before it was kept.
  std::string decision;
  std::string decision_reason;
  std::string papinian_guard_version = "papinian-guard/0.1.0-alpha";
  // No Legal Machines fine-tuned Papinian NER exists yet. Keep that fact
  // explicit instead of relabelling the provisional third-party detector.
  std::string papinian_ner_version = "not-trained";
  std::size_t protected_count = 0;
  std::map<std::string, std::size_t> category_counts;
  std::string response_integrity = "not_applicable";
	// Safe proof captured at the last boundary before transport. The body is
	// never logged or persisted, only this hash and validation result are.
	bool final_payload_validated = false;
	std::string final_payload_sha256;
	std::vector<std::string> protected_project_entity_ids;
	// Safe response-integrity artifact. These fields contain only protected
	// marker syntax, a semantic type, a JSON pointer and stable outcome codes;
	// never provider plaintext or a restoration-map value.
	std::string response_issue_code;
	std::string response_marker_candidate;
	std::string response_canonical_marker;
	std::string response_marker_type;
	std::string response_json_pointer;
	std::string response_normalization_outcome;
	std::string response_repair_class;
	std::size_t response_repair_attempts = 0;
	// Every external call the operation has made so far, in order (the outbound
	// record: Routing/RoutingLog/OutboundRecord.hpp).
	std::vector<OutboundCall> outbound_calls;
	// Which operation's calls these are, in this process only: the key the
	// Routing Log collects the operation's whole list by when it is written.
	// It can be a request namespace, so it is never serialized anywhere.
	std::string outbound_operation_key;
};

} // namespace singularity::protected_api::papinian
