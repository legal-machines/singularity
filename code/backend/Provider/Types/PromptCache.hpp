#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace singularity::providers {

// What a provider may keep of a conversation's prompt between its calls
// (prompt caching): a conversation whose prompt only grows is asked with the
// beginning it already sent, and the provider reads that beginning from its
// cache at a tenth of the input price instead of reading it again. Installed
// for one piece of work (PromptCacheScope) on the thread that does it, as
// GenerationStream is; a provider that keeps no cache, or a prompt not given
// in parts, ignores it.
//
// Nothing of the conversation is in it: the session is a random key that
// keeps the conversation's calls on one endpoint, and the breakpoints are
// positions among the prompt's parts.
struct PromptCache {
	// OpenRouter's session_id: random hex, the same for every call of one
	// conversation while Singularity runs.
	std::string session_id;
	// Where the provider keeps what came before: after the part at `part`
	// (counted from 0 among the prompt's parts), for an hour or five minutes.
	// An hour's breakpoints come before five minutes' (the provider's rule).
	struct Breakpoint {
		std::size_t part = 0;
		bool isHour = false;
	};
	std::vector<Breakpoint> breakpoints;
};

// The cache hint the generations made on this thread carry, or null.
inline std::shared_ptr<const PromptCache> &current_prompt_cache() {
	thread_local std::shared_ptr<const PromptCache> cache;
	return cache;
}

// Gives every generation this thread asks for the hint while the scope lives.
class PromptCacheScope {
public:
	explicit PromptCacheScope(std::shared_ptr<const PromptCache> cache) : previous_(current_prompt_cache()) {
		current_prompt_cache() = std::move(cache);
	}
	~PromptCacheScope() { current_prompt_cache() = previous_; }

	PromptCacheScope(const PromptCacheScope &) = delete;
	PromptCacheScope &operator=(const PromptCacheScope &) = delete;

private:
	std::shared_ptr<const PromptCache> previous_;
};

} // namespace singularity::providers
