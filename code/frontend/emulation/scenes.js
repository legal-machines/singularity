import { scenes as screenplay } from "../onboarding/entry/conversation.js";
import { SCENARIOS, scenarioFor } from "./scenarios.js";
import { DD_THREAD, YOU, fileOf, profileOf, titleWork } from "./world.js";

// MOCK DATA - the scenes onboarding plays in the app, one for each of its
// steps. They expand the screenplay of onboarding's first page: the same
// people and AI colleagues and the same direction of every ask and answer,
// written out as the firm's members write to each other in the app's threads.
// The member at the keyboard gives the work in their own name and field once
// they have introduced themselves; until then it is Gabriel, the partner on
// the deal. What they do goes through the app's own functions.

// The deal's exchanges, by their place in the screenplay (S001 is 0): what
// the asker says, then what the one asked answers.
export const WRITTEN = {
	0: { ask: SCENARIOS.ma.ask, answer: SCENARIOS.ma.answer },
	1: { ask: SCENARIOS.ma.handoff, answer: SCENARIOS.ma.reply },
};

// The second page's two exchanges in the member's field: they give Kitsune
// the work, Kitsune answers with its plan and its one file, and hands
// Evelyn her part.
function delegation() {
	const profile = profileOf();
	const work = scenarioFor(profile.practice);
	return {
		title: work.title,
		given: {
			from: profile.name != "" ? YOU : screenplay[0].from,
			to: screenplay[0].to,
			ask: work.ask,
			answer: work.answer,
			replyFiles: [{ ...fileOf(work.file), isShownInDemo: true }],
		},
		handed: { from: screenplay[1].from, to: screenplay[1].to, ask: work.handoff, answer: work.reply },
	};
}

export const SCENES = {
	// One firm, one team: people and AI colleagues in one list, in the firm's
	// order; the hand rests on Kitsune, who ranks with the partners.
	async team(cue) {
		await cue.open({ kind: "team" });
		cue.hidePointer();
		await cue.wait(1800);
	},

	// AI colleagues take whole matters: the member gives Kitsune the work in
	// their field (S001); Kitsune answers with the plan, and hands Evelyn her
	// part (S002).
	async delegate(cue) {
		const { title, given, handed } = delegation();
		titleWork(title);
		await cue.open({ kind: "thread", threadId: DD_THREAD });
		cue.clear(DD_THREAD);
		cue.hidePointer();
		// Lay out the final exchange once, then reveal its rows in order. Its
		// completed height stays centered without moving earlier messages.
		const request = cue.post(DD_THREAD, { authorId: given.from, paragraphs: [given.ask], files: [], demoHidden: true });
		const plan = cue.post(DD_THREAD, { authorId: given.to, paragraphs: [given.answer], files: given.replyFiles, demoHidden: true });
		const assignment = cue.post(DD_THREAD, { authorId: handed.from, paragraphs: [handed.ask], files: [], demoHidden: true });
		const reply = cue.post(DD_THREAD, { authorId: handed.to, paragraphs: [handed.answer], files: [], quote: { postId: assignment, authorId: handed.from, text: handed.ask }, demoHidden: true, demoReactionSpace: true });
		await cue.wait(350);
		cue.reveal(DD_THREAD, request);
		await cue.wait(1400);
		cue.reveal(DD_THREAD, plan);
		cue.presence(handed.from, "work");
		// Let the current rest pass finish naturally before work begins, so
		// changing clips never cuts a face in mid-pose.
		if (!cue.calm) await cue.until('main [data-species="kitsune"][data-state="work"]', null, 6500);
		await cue.wait(1000);
		cue.reveal(DD_THREAD, assignment);
		await cue.wait(1400);
		cue.reveal(DD_THREAD, reply);
		await cue.wait(900);
		cue.react(reply, "👍", handed.from);
		await cue.wait(2400);
	},

	// Every Matter in one place: Amsterdam, its people and AI colleagues, its
	// threads; the hand opens its documents.
	async matter(cue) {
		await cue.open({ kind: "matter", matterId: "x_sablewood", tab: "threads" });
		const title = await cue.until("main h1", "Amsterdam");
		cue.focus(title.closest("main") ?? title, { pad: 0 });
		await cue.wait(1600);
		const tab = await cue.until("main [role=tab], main button, main a", "Documents");
		await cue.press(tab);
		await cue.wait(500);
		cue.hidePointer();
		cue.focus(title.closest("main") ?? title, { pad: 0 });
		await cue.wait(3200);
	},

};
