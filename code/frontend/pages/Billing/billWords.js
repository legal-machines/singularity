// The words a real Matter's bill reads and writes (RealBill.jsx).

// The minutes in a time as a lawyer types it: "1.5" or "1,5" hours, "1:30",
// "90m", "1h 30m", "2h". Nothing else, never a day or more.
export function minutesFrom(text) {
	const said = String(text ?? "").trim().toLowerCase().replace(",", ".");
	if (said == "") return null;
	let minutes = null;
	let match;
	if ((match = /^(\d+(?:\.\d+)?)\s*h?$/.exec(said))) minutes = Number(match[1]) * 60;
	else if ((match = /^(\d+):([0-5]\d)$/.exec(said))) minutes = Number(match[1]) * 60 + Number(match[2]);
	else if ((match = /^(\d+)\s*m(?:in)?$/.exec(said))) minutes = Number(match[1]);
	else if ((match = /^(\d+)\s*h\s*(\d+)\s*m?(?:in)?$/.exec(said))) minutes = Number(match[1]) * 60 + Number(match[2]);
	if (minutes == null || Number.isFinite(minutes) == false) return null;
	minutes = Math.round(minutes);
	return minutes >= 1 && minutes < 24 * 60 ? minutes : null;
}

const PROVIDERS = {
	openai: "OpenAI",
	anthropic: "Anthropic",
	google: "Google",
	mistral: "Mistral",
	openrouter: "OpenRouter",
	xai: "xAI",
	deepseek: "DeepSeek",
	meta: "Meta",
};

// A model as a lawyer reads it: "openai/gpt-6.1-sol" is "OpenAI GPT-6.1 Sol",
// "anthropic/claude-opus-5-5" is "Anthropic Claude Opus 5.5".
export function modelWords(id) {
	const [provider, ...rest] = String(id ?? "").split("/");
	const model = rest.join("/");
	if (model == "") return provider;
	const words = [];
	for (const part of model.split("-")) {
		const last = words.at(-1);
		if (/^\d+(\.\d+)?$/.test(part) && last != null && /\d$/.test(last)) {
			words[words.length - 1] = `${last}.${part}`;
		} else if (/^\d/.test(part) && last != null && /^gpt$/i.test(last)) {
			words[words.length - 1] = `GPT-${part}`;
		} else if (/^gpt$/i.test(part)) {
			words.push("GPT");
		} else {
			words.push(/^\d/.test(part) ? part : part.charAt(0).toUpperCase() + part.slice(1));
		}
	}
	const named = PROVIDERS[provider.toLowerCase()] ?? provider.charAt(0).toUpperCase() + provider.slice(1);
	return `${named} ${words.join(" ")}`;
}

// A provider as a lawyer reads it: "openrouter" is "OpenRouter".
export function providerWords(id) {
	const said = String(id ?? "");
	if (said == "") return "";
	if (said == "local") return "this computer";
	return PROVIDERS[said.toLowerCase()] ?? said.charAt(0).toUpperCase() + said.slice(1);
}

// What a request was for, as its bill line says it: the thread it
// answered in, or the kind of work it was.
export function requestWords(request, threadTitle = "") {
	if (request.operationRole == "research") return "Web research";
	switch (request.operationType) {
		// An AI colleague's own talk or work, said by the thread it worked for.
		case "document_review":
			return threadTitle.trim() != "" ? `In “${threadTitle.trim()}”` : "Document review";
		case "agent_run":
			return "Data room watch";
		case "review_table":
			return "Review table";
		case "context_automation":
			return "The Matter’s context";
		case "memory_keeping":
			return "What the Matter remembers";
		default:
			break;
	}
	return threadTitle.trim() != "" ? `In “${threadTitle.trim()}”` : "A request";
}

// The model and who asked, under what a request was for: "OpenAI GPT-6.1 Sol,
// asked by Rufus Hartwell"; an AI colleague's own work "on its own".
export function requestDetailWords(request, isColleague, isAskerYou = false) {
	const model = request.modelId != null && request.modelId != "" ? modelWords(request.modelId) : "No model connected";
	if (isColleague == false) return model;
	if (request.agentInitiated) return `${model}, on its own`;
	if (request.askedBy != null && (request.askedBy.name ?? "") != "") return `${model}, asked by ${isAskerYou ? "you" : request.askedBy.name}`;
	return model;
}

// "Nov 4, 3:12 PM", in this computer's time.
export function requestTimeWords(time) {
	const at = new Date(time);
	if (Number.isNaN(at.getTime())) return "";
	return new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric", hour: "numeric", minute: "2-digit" }).format(at);
}

// A fingerprint shortened for a line, whole on copy: its first 16 of 64.
export function shortPrint(sha256) {
	const said = String(sha256 ?? "");
	return said.length > 16 ? `${said.slice(0, 16)}…` : said;
}

// "$0.0521" from micros: the amount as the provider reported it, to the
// millionth where it has one, never rounded to a cent. A bill's total is in
// cents (costText); each request and call on it is exact.
export function exactCostText(micros, currency) {
	const amount = micros / 1000000;
	try {
		return new Intl.NumberFormat("en-US", { style: "currency", currency, minimumFractionDigits: 2, maximumFractionDigits: 6 }).format(amount);
	} catch {
		return `${currency} ${amount.toFixed(6).replace(/0{1,4}$/, "")}`;
	}
}

// One model call's evidence in a line: the provider's own id for the request,
// who served it, its tokens and its cost as the provider reported it.
export function callWords(call, costText) {
	const parts = [];
	const provider = providerWords(call.providerId);
	if ((call.providerRequestId ?? "") != "") parts.push(`${provider} request ${call.providerRequestId}`);
	else parts.push(`${provider}, no request id kept`);
	if ((call.servedBy ?? "") != "" && call.servedBy.toLowerCase() != String(call.providerId ?? "").toLowerCase()) parts.push(`served by ${call.servedBy}`);
	if (call.inputTokens != null || call.outputTokens != null) {
		parts.push(`${(call.inputTokens ?? 0).toLocaleString("en-US")} tokens in and ${(call.outputTokens ?? 0).toLocaleString("en-US")} out`);
	}
	if (call.costMicros != null && (call.costCurrency ?? "") != "") {
		parts.push(`${costText(call.costMicros, call.costCurrency)} ${call.costClassification == "estimated" ? "estimated from the price list" : "as reported"}`);
	}
	// What the provider's own record says, once asked.
	const confirmed = confirmationWords(call, costText);
	if (confirmed != null) parts.push(confirmed);
	return parts.join(", ");
}

// "confirmed by OpenRouter", or what it confirmed where that differs from the
// cost the call reported; null before it is asked.
export function confirmationWords(call, costText) {
	const confirmation = call.confirmation;
	if (confirmation == null || confirmation.costMicros == null) return null;
	const by = confirmation.source == "openrouter" ? "OpenRouter" : "the provider";
	if (confirmation.costMicros == call.costMicros) return `confirmed by ${by}`;
	return `${by} confirms ${costText(confirmation.costMicros, confirmation.currency ?? call.costCurrency ?? "USD")}`;
}

// A request's evidence as text a lawyer can send a client to confirm the bill
// line: when, on which model, the provider's own id for each call, its tokens
// and cost as reported, and the fingerprints of exactly what was sent and what
// came back. Nothing of what the request said.
export function evidenceText(request, { matterName, what, costText }) {
	const lines = [`${what}, ${matterName}`, `Ran ${request.time}`];
	if (request.calls.length == 0) {
		lines.push(request.callsRecorded ? "No call left this computer." : "Its calls were made before Singularity kept their evidence.");
	}
	request.calls.forEach((call, index) => {
		lines.push("");
		lines.push(request.calls.length > 1 ? `Call ${index + 1} of ${request.calls.length}, ${call.time}` : `Call, ${call.time}`);
		lines.push(`Model: ${modelWords(call.modelId)}, through ${providerWords(call.providerId)}${(call.servedBy ?? "") != "" ? `, served by ${call.servedBy}` : ""}`);
		if ((call.providerRequestId ?? "") != "") lines.push(`Provider's request id: ${call.providerRequestId}`);
		if (call.inputTokens != null || call.outputTokens != null) {
			const cached = call.cachedInputTokens != null && call.cachedInputTokens > 0 ? ` (${call.cachedInputTokens.toLocaleString("en-US")} cached)` : "";
			const reasoning = call.reasoningTokens != null && call.reasoningTokens > 0 ? `, ${call.reasoningTokens.toLocaleString("en-US")} reasoning` : "";
			lines.push(`Tokens: ${(call.inputTokens ?? 0).toLocaleString("en-US")} in${cached}, ${(call.outputTokens ?? 0).toLocaleString("en-US")} out${reasoning}`);
		}
		if (call.costMicros != null && (call.costCurrency ?? "") != "") {
			lines.push(`Cost: ${costText(call.costMicros, call.costCurrency)} ${call.costCurrency}, ${call.costClassification == "estimated" ? "estimated from the provider's price list" : "as the provider reported it"}`);
		}
		if (call.confirmation != null && call.confirmation.costMicros != null) {
			lines.push(`Confirmed by ${call.confirmation.source == "openrouter" ? "OpenRouter" : "the provider"}'s own record: ${costText(call.confirmation.costMicros, call.confirmation.currency ?? "USD")} ${call.confirmation.currency ?? "USD"}`);
		}
		if ((call.payloadSha256 ?? "") != "") lines.push(`Sent, SHA-256: ${call.payloadSha256}`);
		if ((call.responseSha256 ?? "") != "") lines.push(`Received, SHA-256: ${call.responseSha256}`);
	});
	return lines.join("\n");
}
