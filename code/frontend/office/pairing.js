// How a pane asks to be connected: by itself, as it opens. Singularity's
// window shows the same code and one button, and the person allows it there.
// A request lasts three minutes: one that ends unanswered is asked again, a
// few times; one Singularity was told to refuse is never asked again by
// itself. While another pane's request stands, or Singularity does not
// answer, the pane asks again in a moment, for a few minutes. A pane allowed
// meanwhile in another window of the same application connects this one too:
// their token is one (the connection's transport).

// An unanswered request is asked again four times: about fifteen minutes in all.
export const RENEWALS = 4;
// While another pane's request stands or Singularity does not answer: asked
// again every four seconds, for three minutes.
export const RETRY_MS = 4000;
export const RETRIES = 45;

const pauseFor = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

// Asks until the person answers in Singularity, and says how it ended:
// "allowed", "declined", "expired" (no one answered), "busy" (another pane's
// request kept standing), "unreachable" (Singularity did not answer), "refused"
// (it did not take the request) or "stopped". `onStage` hears what the pane
// shows meanwhile: {kind: "asking"}, {kind: "waiting", code}, {kind: "busy"} or
// {kind: "unreachable"}.
export async function pairByItself({ transport, client, label, onStage = () => {}, isStopped = () => false, pause = pauseFor }) {
	let renewals = 0;
	let retries = 0;
	onStage({ kind: "asking" });
	for (;;) {
		if (isStopped()) return "stopped";
		if (transport.isPaired()) return "allowed";
		let started;
		try {
			started = await transport.pair(client, label);
		} catch {
			if (++retries > RETRIES) return "unreachable";
			onStage({ kind: "unreachable" });
			await pause(RETRY_MS);
			continue;
		}
		if (isStopped()) return "stopped";
		if (started?.state == "busy") {
			if (++retries > RETRIES) return "busy";
			onStage({ kind: "busy" });
			await pause(RETRY_MS);
			continue;
		}
		if (started?.state != "started") return "refused";
		retries = 0;
		onStage({ kind: "waiting", code: started.code });
		let outcome;
		try {
			outcome = await started.wait();
		} catch {
			if (++retries > RETRIES) return "unreachable";
			onStage({ kind: "unreachable" });
			await pause(RETRY_MS);
			continue;
		}
		if (outcome == "allowed") return "allowed";
		if (outcome == "declined") return "declined";
		// Ended unanswered: "expired", or "unknown" once Singularity let it go.
		if (++renewals > RENEWALS) return "expired";
		onStage({ kind: "asking" });
	}
}

// What the pane says when asking ended without the pane being allowed, and
// the one button that asks again.
export function endingWords(ending, app) {
	switch (ending) {
		case "declined": return { words: "It was not allowed in Singularity.", action: "Ask again" };
		case "expired": return { words: "No one answered in Singularity. Ask again when you are ready.", action: "Ask again" };
		case "busy": return { words: "Singularity is still answering another pane. Answer it there, then ask again.", action: "Ask again" };
		case "unreachable": return { words: "Singularity on this computer did not answer. Open it, then try again.", action: "Try again" };
		default: return { words: `Singularity did not take ${app}'s request.`, action: "Ask again" };
	}
}
