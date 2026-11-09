// The models the firm's AI colleagues think with by default, once a model
// account is connected: a colleague asked to work answered "No model is set for
// this Matter's reasoning. Choose one in AI Setup" with an OpenRouter account
// connected and no AI Setup in the window to choose in. A role the firm already
// set is never changed; each default is the first of these the connected
// accounts offer, models that run the firm's document requests reliably.
const PREFERRED = {
	reasoning: ["gpt-5.6-sol", "gpt-5.5", "gpt-5.4", "gpt-5.1", "gpt-5", "claude-sonnet-5.5", "claude-sonnet-4.5", "gemini-2.5-pro"],
	drafting: ["gpt-5.6-sol", "gpt-5.5", "gpt-5.4", "gpt-5.1", "gpt-5", "claude-sonnet-5.5", "claude-sonnet-4.5", "gemini-2.5-pro"],
	context: ["gpt-5.4-mini", "gpt-5-mini", "gpt-4.1-mini", "claude-haiku-5.5", "claude-haiku-4.5", "gemini-2.5-flash"],
};

// The default for each role the firm has not set, among the eligible models.
export function defaultsFor(roles, assigned) {
	const set = new Set((assigned ?? []).map(assignment => assignment.role));
	const picks = [];
	for (const role of roles ?? []) {
		const preferred = PREFERRED[role.role];
		if (role.assignable != true || preferred == null || set.has(role.role)) continue;
		const eligible = role.eligibleModels ?? [];
		const pick = preferred
			.map(id => eligible.find(model => model.modelId == id || model.modelId == `openai/${id}` || model.modelId == `anthropic/${id}` || model.modelId == `google/${id}`))
			.find(Boolean);
		if (pick != null) picks.push({ role: role.role, providerId: pick.providerId, modelId: pick.modelId });
	}
	return picks;
}

// Sets the defaults the connected accounts allow, keeping every role already
// set; the roles it set, or none.
export async function ensureDefaultModels(nativeApp) {
	if (typeof nativeApp?.getAiSetupCapabilities != "function" || typeof nativeApp?.saveLocalSetup != "function") return [];
	const [capabilities, status] = await Promise.all([nativeApp.getAiSetupCapabilities(), nativeApp.getOnboardingStatus()]);
	const assigned = status?.localSetup?.roleAssignments ?? [];
	const picks = defaultsFor(capabilities?.roles, assigned);
	if (picks.length == 0) return [];
	await nativeApp.saveLocalSetup({ roleAssignments: [...assigned, ...picks] });
	return picks;
}
