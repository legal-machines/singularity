#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::providers {

// One model a provider reported back, classified for task selection. The
// classification is a documented id-pattern heuristic over well-known,
// stable provider naming conventions (e.g. "embedding" always means an
// embedding model, never a chat model) - not an exact/frozen allowlist of
// model ids, and not an invented capability rating. `text_capable` is the
// only thing task selection may rely on; `excluded_reason` exists purely
// for transparency about why a discovered model was excluded.
struct ModelDescriptor {
  std::string id;
  // Present only when the provider reported a separate human-readable name
  // (Anthropic's `display_name`); OpenAI-style APIs only return `id`.
  std::optional<std::string> display_name;
  bool text_capable = true;
  // Set only when text_capable is false, e.g. "embedding", "image",
  // "audio", "moderation". Unset when text_capable is true.
  std::optional<std::string> excluded_reason;
  // The provider's own reported context window for this exact model
  // (OpenAI-compatible discovery's `context_length`, e.g. OpenRouter) -
  // real, provider-supplied capability, never guessed or derived from the
  // model id. Unset when the provider's /models response did not report
  // a positive integer for it (direct OpenAI's real API does not today).
  // See ExternalResearch/ContextBudget/ContextBudget.hpp's
  // resolve_model_context_window_tokens(), which prefers this over its
  // own hand-maintained catalog when present.
  std::optional<std::uint64_t> context_window_tokens;
  // The most this model writes in one answer, as the provider reports it
  // (OpenRouter's `top_provider.max_completion_tokens`) - its thinking
  // included, for a model that thinks. Unset when the provider reports none;
  // never guessed. A request asks for no more than this.
  std::optional<std::uint64_t> max_output_tokens;
  // What the provider charges for this model, in US dollars a token, as it
  // reports them (OpenRouter's `pricing.prompt` and `pricing.completion`).
  // Unset when it reports none.
  std::optional<double> input_price_per_token;
  std::optional<double> output_price_per_token;
  // When these figures were read from the provider (UTC, ISO 8601): the age
  // of the capability snapshot an execution relied on.
  std::string discovered_at;
  // The provider's own reported request parameters this exact model accepts
  // (OpenAI-compatible discovery's `supported_parameters`, e.g. OpenRouter -
  // raw passthrough strings such as "response_format", "tools",
  // "structured_outputs"). Kept only for transparency/audit, mirroring
  // `excluded_reason` alongside `text_capable` above; workflow code must
  // never inspect this directly - see `supports_structured_output` below,
  // which is what ResearchToolLoop/InferenceExecutor actually consult.
  // Empty when the provider's /models response reported none (direct
  // OpenAI's real API does not today - identical fallback shape to
  // context_window_tokens above).
  std::vector<std::string> supported_parameters;
  // The narrow semantic capability derived, once, at discovery time from
  // `supported_parameters` (true only when the provider itself listed
  // "response_format" or "structured_outputs" for this exact model) - real,
  // provider-reported capability, never inferred from the model or provider
  // name, never assumed true when the provider reported nothing. This is
  // what a caller wanting an LM-owned structured output contract enforced
  // (see ProviderRegistry.hpp's GenerationOutputContract) should consult.
  bool supports_structured_output = false;
  // Three independent, provider-reported tool-control capabilities - kept
  // separate from `supports_structured_output` and from each other rather
  // than folded into one flag, because they gate distinct wire concepts:
  // whether this model accepts a `tools` array at all (supports_native_tools,
  // from the provider's own "tools" supported_parameters entry) and whether
  // it honors a `tool_choice` directive (supports_tool_choice, from
  // "tool_choice"). ResearchToolLoop.hpp consults supports_native_tools to
  // choose the native-tool-call path over the bounded textual fallback, and
  // supports_tool_choice to decide whether the first Required research turn
  // can force a specific tool rather than only requesting "required". A
  // provider that reports "tools" but not "tool_choice" still gets the
  // native path (with tool_choice "required" instead of a forced named
  // tool), never a silent downgrade to the textual protocol.
  bool supports_native_tools = false;
  bool supports_tool_choice = false;
};

// The classification heuristic itself, exposed so it can be tested and
// reasoned about independently of a live provider call. Returns the
// exclusion reason for `model_id` ("embedding"/"image"/"audio"/
// "moderation"), or std::nullopt when the id does not match any of those
// well-known non-text naming patterns (i.e. it is treated as text-capable).
//
// Deliberately a standalone pure function with no network dependency: it is
// linked into settings persistence (classifying legacy string-only model
// entries on migration) as well as live provider discovery, and must not
// pull platform::http_client into either.
std::optional<std::string> classify_model_exclusion(const std::string &model_id);

} // namespace singularity::providers
