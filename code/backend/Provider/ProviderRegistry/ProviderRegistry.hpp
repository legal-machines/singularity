#pragma once

#include "Platform/HttpClient/HttpClient.hpp"
#include "Provider/ModelClassification/ModelClassification.hpp"
#include "Provider/Types/ReasoningEffort.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Provider/Types/UsageMetadata.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace singularity::providers {

// The model providers Singularity can actually connect to. Kept narrow and
// hand-written (not data-driven) because each provider's request shape -
// headers, auth scheme, response body - genuinely differs; see
// provider_registry.cpp. code/frontend/src/entities/model-provider mirrors
// this list for display metadata (name, key placeholder) only - it never
// makes the connection itself, so a mismatch here is a bug, not a
// security boundary.
//
// "openai_compatible" is the one legacy, singular slot for an endpoint that
// speaks the OpenAI chat-completions wire format (self-hosted gateways,
// OpenRouter, Groq, ...) - a base URL is required for it and rejected for
// every other known provider; see is_valid_base_url(). Universal custom
// connections (is_valid_custom_provider_id() below) generalize this same
// wire dialect to any number of independently named endpoints.
bool is_known_provider(const std::string &provider_id);

// A universal "connect any provider" custom connection id - a caller-
// generated slug (e.g. "openrouter", "together-ai"), never a raw display
// name, never colliding with a fixed preset id. Real validation, not merely
// a frontend convenience: the backend must never store or route a
// connection under an unsafe or ambiguous id. Lowercase ascii letters,
// digits and hyphens only, 1-40 characters, and not one of the reserved ids
// ("openai", "anthropic", "openai_compatible", "google", "local").
bool is_valid_custom_provider_id(const std::string &provider_id);

// Whether `provider_id` requires (and accepts) a base URL - false only for
// the two providers with a fixed, well-known API root ("openai",
// "anthropic"); true for "openai_compatible" and for every custom
// connection (is_valid_custom_provider_id()), which is the universal
// mechanism this function's own name already promised.
bool provider_requires_base_url(const std::string &provider_id);

// https://, or http:// to this computer's own server for same-machine
// endpoints: the host exactly localhost (any case), 127.0.0.1 or [::1], no
// userinfo, at most a port 1-65535 in digits. Anything else - a bare http://
// to a remote host in particular, however much its address looks local
// ("localhost.evil.net", "localhost@evil.net", "127.0.0.1.nip.io", "127.1") -
// is rejected, so test_connection() and generate_with_transport() build no
// request for it and a provider key is never sent in the clear over the
// network.
bool is_valid_base_url(const std::string &base_url);

struct ProviderTestResult {
  bool ok = false;
  // Every model the provider reported back, unfiltered and untruncated -
  // only meaningful when `ok`. Task selection must filter to
  // `text_capable` models itself; the full list is kept here so discovery
  // stays auditable (Master Spec invariant 5) rather than silently hiding
  // what the provider actually returned.
  std::vector<ModelDescriptor> models;
  // User-facing message, already in the app's plain error-copy style. Only meaningful when `!ok`.
  std::string error;
};

// Verifies `api_key` against the named provider's own API - a real network
// call, not a local format check. Never throws for a bad key or an
// unreachable provider (that is reported via ProviderTestResult::error);
// only a genuinely unexpected failure propagates as an exception.
// `base_url` is required for "openai_compatible" and ignored otherwise.
ProviderTestResult test_connection(const std::string &provider_id, const std::string &api_key,
                                   const std::optional<std::string> &base_url = std::nullopt);

// Pure parsing of an already-decoded OpenAI-compatible GET /models response
// body (id, and - when the provider reports a real, positive integer
// `context_length` for that exact model - the model's real context-window
// capability). No network; exposed purely so this parsing, including real
// provider-reported capability capture, is directly unit-testable against a
// captured fixture. Returns std::nullopt for a malformed/unexpected shape.
std::optional<std::vector<ModelDescriptor>> parse_openai_style_models_response(
    const nlohmann::json &payload);

// An LM-owned machine-readable output contract a caller (ResearchToolLoop
// today) wants a generation turn's response constrained to - the caller
// expresses only the contract itself ("this turn must produce this exact
// JSON shape"); this layer decides whether and how the selected
// provider/model actually enforces it. `json_schema` is a real JSON Schema
// object (the caller's own protocol shape - never invented here);
// `schema_name` is a short stable identifier some providers require
// alongside it. Presence of a contract on a call does not by itself force
// enforcement - see execute_protected_generation()'s and
// InferenceExecutor.cpp's own capability gating, which only actually
// attaches it when the resolved model's own discovered
// ModelDescriptor::supports_structured_output is true; otherwise the turn
// falls back to today's ordinary prompt-only behavior, unchanged.
struct GenerationOutputContract {
  std::string schema_name;
  nlohmann::json json_schema;
};

// One LM-owned, provider-neutral tool a caller wants offered to the model on
// this generation turn. `wire_name` is transport syntax only (must satisfy
// the ordinary OpenAI-compatible function-name contract, e.g. "lm_web_search")
// - never a caller's canonical capability id; the caller (ResearchTools.hpp
// today) owns the bidirectional mapping between the two and is the one
// source of truth for `parameters_schema`, never duplicated ad hoc here.
struct ModelToolDefinition {
  std::string wire_name;
  std::string description;
  nlohmann::json parameters_schema;
  // Typed projection of response argument leaves that may contain protected
  // professional data. Mirrors the parameter shape, but omits protocol,
  // handles and other immutable machine fields.
  nlohmann::json response_data_plane_shape = nlohmann::json::object();
};

// Generic, semantics-free JSON Schema shape normalization for the one wire
// slot every OpenAI-compatible native tool-calling API actually exposes: a
// plain object ("type":"object", "properties", "required", ...) - never a
// root-level composition keyword. A caller's `parameters_schema` (e.g. a
// per-item oneOf/anyOf built from finite capability handles) may not be
// expressible there as-is. This function knows nothing about what any
// schema means, only its shape:
//   - no root-level oneOf/anyOf: returned unchanged (already representable).
//   - oneOf/anyOf with exactly one alternative: that alternative is returned
//     - a composition of one option is semantically identical to the option
//     itself, so this is always lossless.
//   - oneOf/anyOf with two or more alternatives: std::nullopt - flattening
//     distinct alternatives (e.g. per-item field ownership) into one
//     permissive object would make illegal cross-branch combinations
//     representable, so this is reported as not safely representable rather
//     than silently narrowed.
// A caller that gets std::nullopt for any tool it needs to expose must not
// send that schema over native tool calling - use an existing schema-
// faithful execution path instead, or fail closed before the request is
// ever built.
[[nodiscard]] std::optional<nlohmann::json> lower_tool_schema_for_native_calling(
    const nlohmann::json &schema);

// How this generation turn should be allowed to select among the offered
// tools. ForceNamed requires `forced_tool_wire_name` to be set - used for the
// first turn of a Required research loop on a model whose own discovered
// ModelDescriptor::supports_tool_choice is true; Required is the same intent
// for a model that accepts `tools` but not a named `tool_choice`; Auto lets
// the model choose freely (including returning ordinary content instead of a
// tool call), used for every continuation once usable evidence exists.
enum class ToolChoicePolicy { Auto, Required, ForceNamed };

// Everything one generation turn needs to offer native provider tool calling
// - deliberately mirrors GenerationOutputContract's shape (a caller-owned
// contract this layer only mechanically serializes) rather than anything
// provider- or model-specific. Never combined with a GenerationOutputContract
// on the same call - see generate_openai_style()'s own invariant check: a
// tool-decision turn and a response_format-constrained turn are different
// concepts and must never be requested together.
struct GenerationToolControl {
  std::vector<ModelToolDefinition> tools;
  ToolChoicePolicy tool_choice = ToolChoicePolicy::Auto;
  std::optional<std::string> forced_tool_wire_name;
  // Phase 24.1D deliberately runs one deterministic tool call at a time (see
  // ResearchToolLoop.hpp) - approval is exact-intent-bound, web.open depends
  // on a prior web.search result, and ExternalEvidence ids are minted
  // sequentially. Always false for every call this codebase makes today;
  // kept as an explicit field (not a hardcoded false in the serializer) so a
  // caller's intent is visible in one place rather than assumed by the wire
  // layer.
  bool parallel_tool_calls = false;
};

// One tool call a provider proposed, normalized only as far as this
// transport-neutral layer can: the provider's own wire function name (not
// yet mapped to any caller's canonical tool id - see ResearchTools.hpp's
// wire_name_for_canonical_tool_id()/canonical_tool_id_for_wire_name()) and
// its arguments already parsed from the provider's raw JSON-text arguments
// string. `arguments` is a discarded nlohmann::json (is_discarded() == true)
// when the provider's own arguments string was not valid JSON - the caller
// must check this before treating `arguments` as real content; this layer
// never executes or trusts anything about the call, it only parses what the
// provider sent.
struct ModelToolCall {
  std::string call_id;
  std::string wire_name;
  nlohmann::json arguments;
};


// A response arrived but cannot be consumed. Carries billing evidence only,
// never the rejected plaintext or a restoration map.
class UnusableGeneration final : public TaskError {
public:
  UnusableGeneration(TaskErrorCode code, const std::string &message, UsageMetadata usage,
      std::optional<std::string> finishReason,
      std::optional<SafeResponseDiagnostics> diagnostics = std::nullopt)
      : TaskError(code, message, std::move(diagnostics)), usage(std::move(usage)),
        finish_reason(std::move(finishReason)) {}
  UsageMetadata usage;
  std::optional<std::string> finish_reason;
};

// The model declined to answer (an OpenAI-style message.refusal with no
// content). Fails closed, keeps billing evidence, and by construction never
// carries the refusal text - that is pre-restoration provider output.
[[nodiscard]] inline UnusableGeneration model_refusal_error(const std::string &provider_display_name,
    UsageMetadata usage, std::optional<std::string> finish_reason) {
  return UnusableGeneration(TaskErrorCode::Unknown,
      provider_display_name + " declined to answer this request: the model returned a refusal instead of a response.",
      std::move(usage), std::move(finish_reason));
}

// Whether a failure is a model's refusal (model_refusal_error): the one unusable
// generation that carries no code of its own. Asking again only repeats it, at
// a cost, where an answer that could not be read is worth another try.
[[nodiscard]] inline bool is_model_refusal(const TaskError &error) noexcept {
  return error.code() == TaskErrorCode::Unknown && dynamic_cast<const UnusableGeneration *>(&error) != nullptr;
}

struct GenerationResult {
  std::string text;
  UsageMetadata usage;
  // The provider's own reason the generation stopped ("stop"/"length"/...
  // for OpenAI, "end_turn"/"max_tokens"/... for Anthropic) - unset only when
  // the provider's response omitted it, never guessed.
  std::optional<std::string> finish_reason;
  // Real provider-native tool calls parsed from this turn's response (see
  // generate_openai_style()'s own choices[].message.tool_calls parsing) -
  // always empty when the call was not made with a GenerationToolControl, or
  // when the provider returned ordinary content instead of a tool call.
  // Never executed by this layer; the caller (ResearchToolLoop.hpp, via
  // ProtectedInference/InferenceExecutor) normalizes and revalidates every
  // entry before it ever reaches ToolBroker.
  std::vector<ModelToolCall> tool_calls;
  // The provider's own id for this response ("gen-..." from OpenRouter,
  // "chatcmpl-..." from OpenAI, "msg_..." from Anthropic), by which its own
  // records name the request and what it charged. Empty when the response
  // carried none.
  std::string response_id;
  // SHA-256 of the response body exactly as Singularity read it (a streamed
  // answer as its events were put together): it proves which answer a cost
  // belongs to without keeping or showing the answer.
  std::string response_sha256;
};

// The final security boundary for generation requests. `serialized_body` is
// the exact immutable HTTP body handed to transport. `content_segments` are
// every user/model content-bearing string embedded in that body; protocol
// framing such as JSON keys, roles and model identifiers is deliberately not
// content. Adapters must construct both together and may not mutate the body
// after validation. The callback must throw to block transmission.
struct FinalPayloadView {
  std::string serialized_body;
  std::vector<std::string> content_segments;
  // The SHA-256 of each picture the body carries, in the order it does: the
  // final check holds every image in the body to the member's own.
  std::vector<std::string> picture_sha256;
};

using FinalPayloadValidator = std::function<void(const FinalPayloadView &payload)>;

// Injectable only so exact-payload tests can observe what would be sent. The
// production implementation delegates directly to platform::https_post.
class IProviderTransport {
public:
  virtual ~IProviderTransport() = default;
  virtual platform::HttpResponse post(const std::string &url,
                                      const std::vector<platform::HttpHeader> &headers,
                                      const std::string &body, int timeout_seconds) = 0;
  // post(), with a 2xx body told to `receive` as it arrives
  // (platform::https_post_streaming). A transport that cannot stream tells it
  // the whole body once.
  virtual platform::HttpResponse post_streaming(const std::string &url,
                                                const std::vector<platform::HttpHeader> &headers,
                                                const std::string &body, int timeout_seconds,
                                                const platform::HttpBodyReceiver &receive) {
    platform::HttpResponse response = post(url, headers, body, timeout_seconds);
    if (receive && response.status >= 200 && response.status < 300 && !response.body.empty()) receive(response.body);
    return response;
  }
};

// Thrown by OutboundCapturingTransport::post() the instant the exact
// serialized request body is captured. Deliberately NOT derived from
// std::exception so it passes straight through generate_openai_style()'s/
// generate_anthropic()'s own `catch (const std::exception &)` blocks (which
// exist only to reclassify a genuine transport failure as
// TaskErrorCode::NetworkError) instead of being misreported as one.
struct OutboundPayloadCaptured {
  std::string body;
};

// A transport that captures the exact outbound request instead of ever
// sending it - Master Spec Sec.92's own "recording transport" concept,
// reused here for production preview (Phase 08 WP8-B "What leaves this
// device?"), not only tests. post() always throws OutboundPayloadCaptured
// immediately after recording `body`: there is no synthetic response to
// fabricate, and platform::https_post is never reached. Deliberately never
// records `headers` - the only header on any real request is the
// provider's own Authorization/x-api-key credential (see
// generate_openai_style/generate_anthropic), which a preview must never
// see or expose.
class OutboundCapturingTransport final : public IProviderTransport {
public:
  platform::HttpResponse post(const std::string &url,
                              const std::vector<platform::HttpHeader> &headers,
                              const std::string &body, int timeout_seconds) override;
};

// Runs one real generation request against `provider_id`/`model_id`. Never
// returns a partial result on failure - a provider-side error (bad key,
// unknown model, rate limit, unreachable service) is thrown as a TaskError
// carrying a stable providers::TaskErrorCode plus the same plain-language
// message this codebase already surfaced, so callers can classify a failure
// without parsing its text (see Provider/Types/TaskError.hpp).
// `output_contract`, when present, must already have been resolved by the
// caller against the selected model's own discovered
// ModelDescriptor::supports_structured_output - this layer never decides
// that itself, it only mechanically serializes whatever contract it is
// given. Only the OpenAI-compatible ("openai"/"openai_compatible") request
// shape honors it today (as `response_format`/`json_schema`); ignored for
// "anthropic".
// `tool_control` and `output_contract` are mutually exclusive - passing both
// throws std::invalid_argument before any request is built. This is a hard
// portability invariant for every caller (Master Spec: TOOLS PRESENT => NO
// response_format envelope): a tool-decision turn and a final
// structured-output turn are different concepts, and combining them is a
// known compatibility defect across OpenAI-compatible runtimes, not merely
// an aesthetic preference.
// `reasoning_effort` other than Default is sent as the provider's own reasoning
// parameter - OpenRouter's "reasoning": {"effort": ...}, OpenAI's
// "reasoning_effort" - and only by a caller that already asked
// accepts_reasoning_effort() about this model; Anthropic's request has no such
// parameter and is sent without it.
GenerationResult generate(const std::string &provider_id, const std::string &api_key,
                          const std::string &model_id, const std::string &prompt,
                          const FinalPayloadValidator &final_payload_validator,
                          const std::optional<std::string> &base_url = std::nullopt,
						  std::optional<std::uint64_t> maximum_output_tokens = std::nullopt,
						  const std::optional<GenerationOutputContract> &output_contract = std::nullopt,
						  const std::optional<GenerationToolControl> &tool_control = std::nullopt,
						  ReasoningEffort reasoning_effort = ReasoningEffort::Default);

// OpenRouter's own record of one generation: what it charged, the tokens it
// counted and who served it, as its /generation answer gives them, with the
// SHA-256 of that answer as it came.
struct GenerationRecord {
  std::optional<long long> cost_micros;
  std::optional<long long> input_tokens;
  std::optional<long long> output_tokens;
  std::string provider;
  std::string model;
  std::string created_at;
  std::string record_sha256;
};

// Whether an OpenAI-style root is OpenRouter's, which keeps such records.
bool is_openrouter_root(const std::string &root);
// The record in OpenRouter's answer ({"data": {...}}), or nothing for any
// other shape.
std::optional<GenerationRecord> parse_openrouter_generation(const std::string &body);
// Asks OpenRouter for its record of the generation `id` with the firm's key.
// Nothing while OpenRouter does not have it yet (it keeps one moments after
// the answer) or when it cannot be asked; never throws.
std::optional<GenerationRecord> openrouter_generation(const std::string &api_key, const std::string &root,
                                                      const std::string &id);

GenerationResult generate_with_transport(
    const std::string &provider_id, const std::string &api_key,
    const std::string &model_id, const std::string &prompt,
    const FinalPayloadValidator &final_payload_validator,
    IProviderTransport &transport,
    const std::optional<std::string> &base_url = std::nullopt,
	std::optional<std::uint64_t> maximum_output_tokens = std::nullopt,
	const std::optional<GenerationOutputContract> &output_contract = std::nullopt,
	const std::optional<GenerationToolControl> &tool_control = std::nullopt,
	ReasoningEffort reasoning_effort = ReasoningEffort::Default);

// Whether a request to this model may say how long it thinks. A model an
// OpenAI-compatible endpoint lists with "reasoning" or "reasoning_effort"
// among its supported parameters (OpenRouter reports them per model); for
// direct OpenAI, which reports no parameters, its reasoning models by family
// - the o-series and gpt-5, never a "-chat" model. Anthropic (directly or
// through OpenRouter, where asking would turn Claude's thinking on), any
// "-chat" model and every model nothing says of: false, and the call is sent
// as it always was.
[[nodiscard]] bool accepts_reasoning_effort(const std::string &provider_id, const ModelDescriptor &model);

// Whether a provider's refusal of a request is about its reasoning parameter
// ("Unsupported parameter: 'reasoning_effort'"), or OpenRouter finding no one
// endpoint that takes every required parameter ("No endpoints found that can
// handle the requested parameters"): the request is then sent once more
// without it.
[[nodiscard]] bool refuses_reasoning_effort(const std::string &message);
// Whether a provider turned a request away because its endpoint will not
// answer without thinking ("Reasoning is mandatory for this endpoint and
// cannot be disabled"): a request that asked for none is asked again for the
// least (ReasoningEffort::Minimal), never for the endpoint's default.
[[nodiscard]] bool requires_reasoning(const std::string &message);

// How long generate() waits before its one retry of a request a provider
// turned away for a passing reason (429, 5xx, a timeout) - 1.5 s plus up to
// as much again at random in production.
void set_generation_retry_delay_for_testing(std::chrono::milliseconds delay);

// How many seconds from `now` a provider's Retry-After header asks Singularity to
// wait: whole seconds, or an HTTP date (RFC 9110: IMF-fixdate, and the obsolete
// RFC 850 and asctime forms); a date already past is 0. Nothing when the header
// is absent or cannot be read - the ordinary short wait applies then.
[[nodiscard]] std::optional<long long> retry_after_seconds(const std::string &value,
                                                           std::chrono::system_clock::time_point now);

} // namespace singularity::providers
