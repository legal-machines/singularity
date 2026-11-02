#pragma once

#include <optional>
#include <string>

namespace singularity::providers {

// Token counts a provider reported for one generation request. Any field
// left unset means the provider did not report that count - never guessed,
// never priced (see Routing/InferenceExecutor/InferenceExecutor.hpp).
struct UsageMetadata {
  std::optional<long long> input_tokens;
  std::optional<long long> output_tokens;
  std::optional<long long> total_tokens;
  // Of the output, what the model spent thinking before it answered, where the
  // provider says (OpenRouter's completion_tokens_details): what tells an
  // answer that came back empty after its thinking from one never written.
  std::optional<long long> reasoning_tokens;
  // Of the input, what the provider read from its prompt cache, and what it
  // wrote to it on this call (OpenRouter's prompt_tokens_details), where it
  // says: a cache read costs a tenth of the input price, a write more than it.
  std::optional<long long> cached_input_tokens;
  std::optional<long long> cache_write_tokens;
  // Monetary evidence is populated only when a provider response supplies
  // a real numeric cost - either its own complete currency/classification/
  // methodology trio, or (OpenRouter's real, documented shape) a bare cost
  // with none of those, honestly assumed USD/actual/provider_reported (see
  // read_cost_evidence() in ProviderRegistry.cpp). Missing cost is never
  // represented as zero.
  std::optional<long long> cost_micros;
  std::optional<std::string> cost_currency;
  std::optional<std::string> cost_classification;
  std::optional<std::string> cost_methodology;
  // The provider that served the request, as a gateway names it ("Amazon
  // Bedrock", "Anthropic"): where a call routed among several went (D-116).
  // Unset where the response named none.
  std::optional<std::string> served_by;
};

// THE ONE RULE for what an operation cost when it paid for more than one call -
// its research turns, a review, a correction, a resumed continuation, or each
// bounded batch of an automatic Context build. Lives with the type itself so
// every caller applies the same arithmetic and the same honesty about what it
// does not know:
//   - token counts add;
//   - a call that produced real usage but no price makes the total PARTIAL, so
//     it is never reported as the actual cost of the operation (USE-6);
//   - two currencies are never summed and no conversion is invented - the
//     amount is dropped and the total is partial;
//   - differing classifications (actual + estimated) are likewise partial.
// A partial total keeps its token counts and its evidence; what changes is the
// claim made about it (see usage::classify_cost, which reports it as usage
// without a stated cost rather than as a smaller bill than the real one).
[[nodiscard]] inline UsageMetadata accumulate_usage(
    const UsageMetadata &accumulated, const UsageMetadata &addition) {
  UsageMetadata merged = accumulated;
  const auto add = [](std::optional<long long> &target, const std::optional<long long> &value) {
    if (!value) return;
    target = target.value_or(0) + *value;
  };
  const auto billed = [](const UsageMetadata &usage) {
    return usage.input_tokens || usage.output_tokens || usage.total_tokens || usage.cost_micros;
  };
  const bool accumulatedBilled = billed(accumulated);
  const bool additionBilled = billed(addition);
  add(merged.input_tokens, addition.input_tokens);
  add(merged.output_tokens, addition.output_tokens);
  add(merged.total_tokens, addition.total_tokens);
  add(merged.reasoning_tokens, addition.reasoning_tokens);
  add(merged.cached_input_tokens, addition.cached_input_tokens);
  add(merged.cache_write_tokens, addition.cache_write_tokens);
  add(merged.cost_micros, addition.cost_micros);
  // A gap only exists when SOME of this operation was priced and some was not.
  // An operation with no price anywhere is not a partial bill - it is work with
  // no cost to report at all (a local model runs on the person's own computer),
  // and calling that "partial" would imply money nobody was ever charged.
  const bool anyCostReported = accumulated.cost_micros.has_value() || addition.cost_micros.has_value();
  const bool costGap = anyCostReported
      && ((accumulatedBilled && !accumulated.cost_micros) || (additionBilled && !addition.cost_micros));
  const bool currencyGap = accumulated.cost_currency && addition.cost_currency
      && *accumulated.cost_currency != *addition.cost_currency;
  if (!merged.cost_currency) merged.cost_currency = addition.cost_currency;
  if (!merged.cost_classification) merged.cost_classification = addition.cost_classification;
  if (!merged.cost_methodology) merged.cost_methodology = addition.cost_methodology;
  // Every provider that served a call of the operation, each named once.
  if (addition.served_by && !addition.served_by->empty()) {
    if (!merged.served_by || merged.served_by->empty()) merged.served_by = addition.served_by;
    else if (("; " + *merged.served_by + ";").find("; " + *addition.served_by + ";") == std::string::npos)
      merged.served_by = *merged.served_by + "; " + *addition.served_by;
  }
  if (accumulated.cost_classification && addition.cost_classification
      && *accumulated.cost_classification != *addition.cost_classification)
    merged.cost_classification = "partial";
  if (currencyGap) {
    merged.cost_micros.reset();
    merged.cost_currency.reset();
  }
  if (costGap || currencyGap) merged.cost_classification = "partial";
  return merged;
}

} // namespace singularity::providers
