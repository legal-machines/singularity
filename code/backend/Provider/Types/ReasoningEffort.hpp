#pragma once

namespace singularity::providers {

// How long a reasoning model thinks before it answers, asked per call. Default
// sends nothing and leaves the provider's own default; Low asks for a short
// think, for a call whose work is mechanical - naming which facts about the
// lawyer's values an answer rests on - where a long think buys nothing and
// cost a minute on a live run (25 Sep 2026). A caller asks for it; whether it
// is sent is decided from the selected model (accepts_reasoning_effort()).
//
// None asks for no thinking at all, for a conversational answer (a
// colleague's talk, D-129): on Claude's fifth generation through OpenRouter a
// low effort still thought 156 to 1,758 tokens per answer, where none thought 0
// and cost 40% less (measured 2026-10-26).
//
// Minimal asks for the least thinking the endpoint names: what None becomes
// for a model whose endpoint will not answer without thinking ("Reasoning is
// mandatory for this endpoint and cannot be disabled": Sonnet 5.5 through
// OpenRouter, 2026-10-26), never its default, longer think.
enum class ReasoningEffort {
	Default,
	Low,
	None,
	Minimal,
};

} // namespace singularity::providers
