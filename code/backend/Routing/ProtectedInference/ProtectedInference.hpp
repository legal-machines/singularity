#pragma once

#include "PapinianGuard/AnswerContract/AnswerContract.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Routing/Types/ModelVisibleTurn.hpp"

#include <optional>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace singularity::routing {

// The final answer contract of one protected generation (DESIGN4 §9,
// PapinianGuard/AnswerContract): what failed, by closed code, symbol, field and
// offset in the model's protected text - never a value - and what was left out
// or replaced before the lawyer reads it.
struct AnswerContractFindings {
	// The final text's violations, hard and soft (a nudge), in text order.
	std::vector<protected_api::papinian::answer_contract::ContractViolation> violations;
	// Beside each violation: its symbol as a local fact anchor, which protection
	// sends as the symbol again - what a repair message names it by - or "".
	// Local text; never sent as it is, never kept.
	std::vector<std::string> local_symbols;
	std::size_t quantitative_symbols = 0;
	// The final text's hard violations (a nudge is not one).
	std::size_t failures = 0;
	// Figures left out of the final text, and figures a tool call's document
	// fields show as a neutral gap instead (DESIGN4 §9.2).
	std::size_t withheld_figures = 0;
	std::size_t tool_call_withheld = 0;
	// Whether the generation's `text` is the answer with every sentence that
	// carries an unverifiable figure left out (a chat answer) or every such
	// figure replaced by a gap (a document field): what the lawyer may read
	// once no repair is left. The answer as the model wrote it is not restored
	// into it. False when nothing failed, or when leaving out would leave no
	// answer at all.
	bool text_withheld = false;
	// Whether every sentence of the answer's prose would be withheld: no answer
	// is left for the lawyer, and the task fails in plain words (DESIGN4 §9.2).
	// `text` is then the answer as restored, for the caller's repair only -
	// never for the lawyer.
	bool answer_emptied = false;
	// When `text_withheld`: the same answer in the model's protected plane (the
	// provider's bytes with what failed left out or gapped) - what a stored
	// answer's protected template is built from, so a later turn never re-sends
	// what the lawyer never read. Symbols only; never a value.
	std::string withheld_protected_text;
};

struct ProjectEntityProtectionHint {
	std::string entity_id;
	protected_api::papinian::EntityType category = protected_api::papinian::EntityType::Person;
	std::vector<std::string> local_values;
	// Of local_values, the ones the lawyer confirmed (the registry's locked
	// aliases): masked as they are. The rest an automatic pass recorded, and the
	// Guard masks them only while they still name an identity.
	std::vector<std::string> curated_values;
	// The symbol it stands under in every request of the Project (D-235).
	std::string symbol;
};

struct ProtectedGenerationResult {
  std::string text;
  bool protected_api_applied = false;
  bool response_integrity_ok = true;
  std::size_t protected_count = 0;
  protected_api::papinian::PrivacyMetadata privacy_metadata;
  providers::UsageMetadata usage;
  std::optional<std::string> finish_reason;
  // Set (only ever alongside finish_reason=="length"/"max_tokens") when this
  // turn had ModelVisibleTurn::request_semantic_completion set and still
  // reached the real physical allowance execute_protected_generation()
  // resolved for it (the provider/model's own documented maximum completion
  // tokens) - a true physical boundary, never a semantic verdict. False for
  // every turn that did not request semantic completion.
  bool physical_context_exhausted = false;
  // Set, with nothing sent, when the turn asked for computable values
  // (ModelVisibleTurn::needs_computable_values) and protection left no
  // protected amount, date, rate or quantity in what would have been sent.
  bool provider_call_skipped = false;
  // Real provider-native tool calls this turn returned (see
  // providers::GenerationResult::tool_calls), each argument string leaf
  // already restored through the same Papinian Guard as `text` above when
  // protection was active and something was actually masked - never restored
  // when there was nothing to restore, exactly mirroring `text`'s own
  // early-return short circuit below. Still entirely untrusted content: the
  // caller (ResearchToolLoop.hpp) normalizes and re-validates every entry
  // before any of it reaches ToolBroker.
  std::vector<providers::ModelToolCall> tool_calls;
	// Every amount or date restoration wrote into `text`, in `text`'s own
	// coordinates, with the fact it is (PapinianGuard::restore): what the stored
	// answer keeps so the next turn protects it as the same fact. Offsets and
	// digests only; never a symbol or a value.
	std::vector<protected_api::papinian::FactAnchor> fact_anchors;
	// Figures the model wrote in its final protected text that neither a symbol,
	// the sent prompt nor an operation accounts for (ProtectedOperations): a
	// count, and where each stands as a JSON locator - never the figure.
	struct UntracedFigures {
		std::size_t count = 0;
		std::vector<std::string> locators;
	};
	UntracedFigures untraced_figures;
	// The final answer contract (DESIGN4 §9): run on the final text and on every
	// string field a tool call writes into the lawyer's work, before restoration.
	AnswerContractFindings answer_contract;
};

// Thrown out of execute_protected_generation() instead of returning
// normally when it was called with an OutboundCapturingTransport (Phase 08
// WP8-B "What leaves this device?", Master Spec Sec.54/92). Carries no
// response text - none was ever received - and no restoration/response-
// integrity state - nothing to restore. `outbound_text` and
// `outbound_request_body` are exactly what a real send would have
// transmitted: already Papinian-masked when protection is active (the same
// `outbound_prompt` variable generate_openai_style()/generate_anthropic()
// embed verbatim as the request's `content` field), and already validated
// by the same final outbound leak guard a real send uses - the throw only
// happens after that guard has already passed.
struct OutboundPreviewCaptured {
  std::string outbound_text;
  std::string outbound_request_body;
  bool protected_api_applied = false;
  protected_api::papinian::PrivacyMetadata privacy_metadata;
	// The protection this very request was built with: its protected prompt,
	// exactly as it leaves, and the mapping behind its symbols - what "What
	// leaves this device?" presents and reveals from. Native memory only,
	// never serialized or sent; a preview session already holds the same.
	protected_api::papinian::ProtectionResult protection;
};

// Runs the security-sensitive part of the real provider path. The request
// mapping is confined to this stack frame and is neither returned nor passed
// to the provider transport.
// The pictures of the member's own that the exact body carries (D-256): each
// image in it - an OpenAI-style data URL, an Anthropic base64 source - must
// be one the member attached to this request (Provider/Types/InputPictures),
// in the order the provider adapter reported it, or the request is refused
// (ProtectedPayloadLeakDetected) and nothing is sent, whatever the protection.
// The final check of every provider call applies it.
std::vector<protected_api::papinian::OutboundPicture> hold_pictures_to_the_members(
		const providers::FinalPayloadView &payload);

ProtectedGenerationResult execute_protected_generation(
    const std::string &provider_id, const std::string &api_key,
    const std::string &model_id, const ModelVisibleTurn &turn,
    const std::optional<std::string> &base_url,
    const protected_api::Policy &policy,
    const protected_api::papinian::PapinianGuard &guard,
    const std::string &request_namespace,
    providers::IProviderTransport *transport = nullptr,
	const std::vector<ProjectEntityProtectionHint> &project_entity_hints = {},
	std::optional<std::uint64_t> maximum_output_tokens = std::nullopt,
	const std::optional<providers::GenerationOutputContract> &output_contract = std::nullopt,
	const std::optional<providers::GenerationToolControl> &tool_control = std::nullopt,
	protected_api::papinian::OperationProtectedSymbolTable *operation_symbols = nullptr);


// The parts of one planned leaf as detection sees them, with whether each is
// the person's own words (their request, their instructions) rather than a
// document's text or Singularity's own framing. Exposed so the section marking
// that decides which spelling of a name an answer comes back in is pinned by
// a test rather than trusted.
[[nodiscard]] std::vector<std::pair<std::string, bool>>
semantic_detection_parts_for_testing(const std::string &identity, const std::string &value);

} // namespace singularity::routing
