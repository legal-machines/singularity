#pragma once

#include "ExternalResearch/Types/ExternalResearchTypes.hpp"
#include "Platform/HttpClient/HttpClient.hpp"

#include <functional>
#include <optional>
#include <string>

namespace singularity::research {

using HostedResearchPost = std::function<platform::HttpResponse(const std::string &,
	const std::vector<platform::HttpHeader> &, const std::string &, int)>;

bool is_safe_public_research_url(const std::string &url) noexcept;

class IExternalResearchAdapter {
public:
	virtual ~IExternalResearchAdapter() = default;
	virtual ResearchResponse search(const ResearchRequest &request) = 0;
	virtual std::optional<ResearchOpenResult> open(const std::string &sourceId) = 0;
};

class OpenAiHostedResearchAdapter final : public IExternalResearchAdapter {
public:
	OpenAiHostedResearchAdapter(std::string apiKey, std::string modelId = "gpt-5.5",
		HostedResearchPost post = {});

	ResearchResponse search(const ResearchRequest &request) override;
	std::optional<ResearchOpenResult> open(const std::string &sourceId) override;

private:
	std::string api_key_;
	std::string model_id_;
	HostedResearchPost post_;
};

// A second, independently selectable ExternalResearch backend - Phase 24.1D's
// answer to research belonging to Singularity rather than one vendor: a Project
// using an OpenRouter-routed primary model (or any other) must not lose web
// research merely because the OTHER, separately-configured OpenAI hosted
// research backend is quota-limited. Implements the exact same
// IExternalResearchAdapter contract as OpenAiHostedResearchAdapter, so
// ResearchTools.cpp's production_adapter() is the only place that decides
// which one runs - ResearchToolLoop, ToolBroker and every canonical
// web.search/web.open identity are unchanged either way.
//
// Uses OpenRouter's current server-side tool mechanism
// (tools:[{"type":"openrouter:web_search"/"openrouter:web_fetch",...}]),
// never the deprecated `:online` model suffix or `plugins:[{"id":"web"}]`
// shape. This is a wholly separate request from the primary reasoning
// model's own native `lm_web_search`/`lm_web_open` function-tool turn (see
// ProviderRegistry.hpp's GenerationToolControl) - OpenRouter's server tools
// are never exposed to the primary legal reasoning loop, only used here, as
// this ONE adapter's own private implementation of "how do I actually search
// the web for the canonical query ToolBroker already authorized."
class OpenRouterExternalResearchAdapter final : public IExternalResearchAdapter {
public:
	OpenRouterExternalResearchAdapter(std::string apiKey,
		std::string workerModelId = "google/gemini-2.5-flash", HostedResearchPost post = {});

	ResearchResponse search(const ResearchRequest &request) override;
	std::optional<ResearchOpenResult> open(const std::string &sourceId) override;

private:
	std::string api_key_;
	std::string worker_model_id_;
	HostedResearchPost post_;
};

} // namespace singularity::research
