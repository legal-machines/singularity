// The model accounts onboarding connects (D-134), as the backend takes them
// (Provider/ProviderRegistry): OpenAI and Anthropic by their own ids; every
// other service by an OpenAI-compatible address under an id of its own.
// OpenRouter is `openai_compatible`, the one form web research reads. The key
// goes to the native side and is kept there only (AG-SEC-1); the backend
// checks it with the service before it keeps it.
export const PROVIDERS = [
	{ key: "openrouter", name: "OpenRouter", line: "One key for every major model", id: "openai_compatible", baseUrl: "https://openrouter.ai/api/v1", displayName: "OpenRouter" },
	{ key: "openai", name: "OpenAI", line: "GPT and Codex models", id: "openai" },
	{ key: "anthropic", name: "Anthropic", line: "Claude models", id: "anthropic" },
	{ key: "google", name: "Google", line: "Gemini models", id: "gemini", baseUrl: "https://generativelanguage.googleapis.com/v1beta/openai", displayName: "Google Gemini" },
	{ key: "deepseek", name: "DeepSeek", line: "DeepSeek models", id: "deepseek", baseUrl: "https://api.deepseek.com/v1", displayName: "DeepSeek" },
	{ key: "xai", name: "xAI", line: "Grok models", id: "xai", baseUrl: "https://api.x.ai/v1", displayName: "xAI Grok" },
];

// Whether the backend keeps a connection of this provider already.
export function isConnected(provider, connected) {
	return connected.some((item) => item.id == provider.id && (provider.baseUrl == null || item.baseUrl == null || item.baseUrl == provider.baseUrl));
}

// What a refused key says: the backend's reason, which names what to fix,
// in place of its general sentence.
export function reasonOf(error) {
	const detail = typeof error?.detail == "string" ? error.detail.trim() : "";
	if (detail && detail.length <= 160 && !detail.includes("\n")) return detail;
	return "The key was not accepted. Check it and try again.";
}
