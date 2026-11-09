import { report } from "../components/Avatar/life.js";
import { now } from "../pages/threadState.js";
import { FROM_EMULATION, FROM_ONBOARDING } from "./emulation.js";
import { SCENES } from "./scenes.js";
import { applyProfile, prepareWorld } from "./world.js";

// Thrown into a scene's script when another scene starts or the app leaves.
const STOPPED = Symbol("stopped");
// How long a scene holds its end before it plays again.
const REPLAY_AFTER_MS = 12000;

let director = null;

// The one director of this frame's app: it plays the scene onboarding asks
// for, by the app's own functions and on its own elements, so what shows is
// the app at work, not a picture of it.
export function directorOfThisFrame() {
	director ??= createDirector();
	return director;
}

function createDirector() {
	prepareWorld();
	let app = null;
	let wanted = null;
	const sceneListeners = new Set();
	let playing = 0;
	let paused = false;
	let holdAtEnd = false;
	const sleepers = new Set();
	const wake = () => { for (const resume of [...sleepers]) resume(); };
	document.addEventListener("visibilitychange", wake);
	let serial = 0;
	let pointer = null;
	const at = { x: 0, y: 0 };
	const calm = window.matchMedia("(prefers-reduced-motion: reduce)");

	const tell = (message) => window.parent?.postMessage({ from: FROM_EMULATION, ...message }, "*");

	window.addEventListener("message", (event) => {
		if (event.source !== window.parent || event.data?.from !== FROM_ONBOARDING) return;
		if (event.data.type === "playback") {
			const wasHolding = holdAtEnd;
			paused = event.data.paused === true;
			holdAtEnd = event.data.settle === true;
			if (holdAtEnd && pointer) delete pointer.dataset.shown;
			if (app && wanted && (holdAtEnd || wasHolding && !paused)) play(wanted, holdAtEnd);
			for (const animation of pointer?.getAnimations() ?? []) paused ? animation.pause() : animation.play();
			wake();
		}
		// The member introducing themselves on onboarding's first page, as they
		// type: the app draws them at once, and on the team's page the row they
		// join in comes into view.
		if (event.data.type === "profile") {
			applyProfile(event.data.profile);
			app?.()?.setState?.((previous) => ({ ...previous }));
			if (wanted == "team") requestAnimationFrame(() => requestAnimationFrame(() => document.querySelector("main li[data-joining]")?.scrollIntoView({ block: "nearest" })));
		}
		if (event.data.type == "scene" && (event.data.scene != wanted || event.data.replay === true)) {
			wanted = event.data.scene;
			for (const listener of sceneListeners) listener(wanted);
			if (app != null) play(wanted, holdAtEnd);
		}
	});

	// A scene plays its one action, holds it while the member reads, and plays
	// it again, as long as its slide shows. Under reduced motion it shows its
	// end once and stays.
	async function play(id, finish = false) {
		const run = ++playing;
		wake();
		const script = SCENES[id];
		if (script == null) return;
		try {
			for (;;) {
				const cue = cueFor(run, finish);
				await script(cue);
				if (run == playing) tell({ type: "done", scene: id });
				if (calm.matches || finish) return;
				await cue.wait(REPLAY_AFTER_MS);
			}
		} catch (error) {
			if (error !== STOPPED) console.error(error);
		}
	}

	// What a scene's script can do: the steps a member's hand takes, and what
	// the other members do meanwhile.
	function cueFor(run, finish) {
		const alive = () => {
			if (run != playing || app == null) throw STOPPED;
		};
		const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
		const frames = () => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
		const own = () => {
			alive();
			return app();
		};

		// Time passes only while the window shows the page; under reduced
		// motion it does not pass at all, and the scene shows its end.
		async function whenActive() {
			alive();
			while (document.hidden || paused && !finish) {
				await new Promise(resolve => {
					const resume = () => { sleepers.delete(resume); resolve(); };
					sleepers.add(resume);
				});
				alive();
			}
		}
		async function wait(ms) {
			await whenActive();
			let left = calm.matches || finish ? 0 : ms;
			while (left > 0) {
				await whenActive();
				if (calm.matches || finish) break;
				const step = Math.min(left, 200);
				await sleep(step);
				left -= step;
				alive();
			}
			await whenActive();
			await frames();
			alive();
		}

		function find(selector, text) {
			const all = [...document.querySelectorAll(selector)];
			return all.find((element) => text == null || element.textContent.includes(text)) ?? null;
		}

		// The innermost element that says these words: every element that says
		// them holds it, and it comes last in the page's order.
		function words(text) {
			const all = [...document.querySelectorAll("main *")].filter((element) => element.textContent.includes(text));
			return all.at(-1) ?? null;
		}

		async function until(selector, text, timeout = 4000) {
			for (let waited = 0; waited <= timeout; waited += 50) {
				const found = find(selector, text);
				if (found != null) return found;
				await sleep(50);
				alive();
			}
			throw new Error(`The emulation found no ${selector}${text ? ` with “${text}”` : ""}.`);
		}

		const centre = (element) => {
			const rect = element.getBoundingClientRect();
			return { x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
		};

		// The pointer travels as a hand does: farther takes longer, and it
		// eases in and out of the move.
		async function point(element) {
			alive();
			if (pointer == null || calm.matches || finish) return;
			const to = centre(element);
			const distance = Math.hypot(to.x - at.x, to.y - at.y);
			const duration = Math.min(1100, 260 + distance * 0.7);
			pointer.dataset.shown = "";
			// It rests where it arrives; the move only animates the way there.
			pointer.style.transform = `translate(${to.x}px, ${to.y}px)`;
			pointer.animate(
				[{ transform: `translate(${at.x}px, ${at.y}px)` }, { transform: `translate(${to.x}px, ${to.y}px)` }],
				{ duration, easing: "cubic-bezier(0.45, 0, 0.2, 1)" },
			);
			at.x = to.x;
			at.y = to.y;
			await wait(duration + 60);
		}

		// A press: the pointer goes there and presses. With `click: false` the
		// script itself does what the press does (an attached file, which no
		// script can pick from a dialog).
		async function press(element, { click = true } = {}) {
			await point(element);
			if (pointer != null && !calm.matches && !finish) {
				pointer.firstElementChild?.animate(
					[{ transform: "scale(1)" }, { transform: "scale(0.86)" }, { transform: "scale(1)" }],
					{ duration: 220, easing: "ease-out" },
				);
				await wait(110);
			}
			if (click) element.click();
			await wait(160);
		}

		return {
			calm: calm.matches || finish,
			wait,
			find,
			until,
			async says(text, timeout = 4000) {
				for (let waited = 0; waited <= timeout; waited += 50) {
					const found = words(text);
					if (found != null) return found;
					await sleep(50);
					alive();
				}
				throw new Error(`The emulation found no “${text}”.`);
			},
			point,
			press,
			hidePointer() {
				if (pointer != null) delete pointer.dataset.shown;
			},
			// A colleague pointed at reacts as it does under a hand.
			async hover(element) {
				await point(element);
				element.dispatchEvent(new MouseEvent("mouseover", { bubbles: true }));
				element.dispatchEvent(new MouseEvent("mouseenter", { bubbles: false }));
			},
			leave(element) {
				element.dispatchEvent(new MouseEvent("mouseout", { bubbles: true }));
				element.dispatchEvent(new MouseEvent("mouseleave", { bubbles: false }));
			},
			async open(place) {
				own().open(place);
				await frames();
				alive();
			},
			// Words typed into a field, a character at a time, as React reads
			// typing.
			async type(element, text) {
				await press(element);
				element.focus();
				const prototype = element instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
				const set = Object.getOwnPropertyDescriptor(prototype, "value").set;
				for (let length = calm.matches || finish ? text.length : 1; length <= text.length; length += 1) {
					set.call(element, text.slice(0, length));
					element.dispatchEvent(new Event("input", { bubbles: true }));
					if (length < text.length) await wait(38 + (text.charCodeAt(length) % 5) * 9);
				}
				await wait(120);
			},
			// A thread as the scene begins it: none of what an earlier showing
			// added, nobody typing.
			clear(threadId) {
				own().setState(previous => {
					const reactions = { ...previous.reactions };
					const answers = { ...previous.answers };
					for (const event of previous.extraEvents[threadId] ?? []) { delete reactions[event.id]; delete answers[event.id]; }
					return { ...previous, reactions, answers, extraEvents: { ...previous.extraEvents, [threadId]: [] }, typing: { ...previous.typing, [threadId]: [] } };
				});
			},
			// The member at the keyboard posts, by the app's own reply, with
			// the files the screenplay gives.
			reply(threadId, text, files) {
				own().reply(threadId, text, null, { files });
			},
			// Another member's post in a thread.
			post(threadId, post) {
				const event = { id: `emulated_${++serial}`, kind: "post", at: now(), quote: null, files: null, notChecked: null, ask: null, ...post };
				own().setState((previous) => ({
					...previous,
					extraEvents: { ...previous.extraEvents, [threadId]: [...(previous.extraEvents[threadId] ?? []), event] },
				}));
				return event.id;
			},
			reveal(threadId, postId) {
				own().setState(previous => ({
					...previous,
					extraEvents: { ...previous.extraEvents, [threadId]: (previous.extraEvents[threadId] ?? []).map(event => event.id === postId ? { ...event, demoHidden: false } : event) },
				}));
			},
			react(postId, emoji, memberId) {
				own().setState(previous => {
					const reactions = previous.reactions[postId] ?? {};
					return { ...previous, reactions: { ...previous.reactions, [postId]: { ...reactions, [emoji]: [...new Set([...(reactions[emoji] ?? []), memberId])] } } };
				});
			},
			// Another member's message in its personal chat with the viewer.
			message(memberId, message) {
				const item = { id: `emulated_${++serial}`, authorId: memberId, at: now(), matterId: null, text: "", files: null, notChecked: null, ask: null, ...message };
				own().setState((previous) => ({
					...previous,
					extraMessages: { ...previous.extraMessages, [memberId]: [...(previous.extraMessages[memberId] ?? []), item] },
				}));
				return item.id;
			},
			// Who writes an answer now: in a thread, by its id, or in a chat,
			// by the member's id.
			typing(where, memberIds) {
				own().setState((previous) => ({ ...previous, typing: { ...previous.typing, [where]: memberIds } }));
			},
			presence(memberId, presence) {
				alive();
				report(memberId, presence);
			},
			// Where onboarding's lens should look, in this frame's pixels: around
			// one element, or all of several. A marked moment stands out: the
			// rest stands back under a veil.
			focus(elements, { pad = 24, mark = false } = {}) {
				alive();
				const rects = [elements].flat().filter(Boolean).map((element) => element.getBoundingClientRect());
				if (rects.length == 0) return;
				const left = Math.min(...rects.map((rect) => rect.left)) - pad;
				const top = Math.min(...rects.map((rect) => rect.top)) - pad;
				const right = Math.max(...rects.map((rect) => rect.right)) + pad;
				const bottom = Math.max(...rects.map((rect) => rect.bottom)) + pad;
				tell({ type: "focus", rect: { x: left, y: top, width: right - left, height: bottom - top }, spot: mark });
			},
			scroll(element) {
				element.scrollIntoView({ block: "nearest", behavior: calm.matches ? "auto" : "smooth" });
			},
		};
	}

	return {
		useScene(listener) { sceneListeners.add(listener); if (wanted) listener(wanted); return () => sceneListeners.delete(listener); },
		attach(getApp) {
			app = getApp;
			tell({ type: "ready" });
			if (wanted != null) play(wanted, holdAtEnd);
			return () => {
				if (app === getApp) { app = null; ++playing; wake(); }
			};
		},
		usePointer(element) {
			pointer = element;
			at.x = window.innerWidth * 0.62;
			at.y = window.innerHeight * 0.72;
			element.style.transform = `translate(${at.x}px, ${at.y}px)`;
			return () => {
				if (pointer === element) pointer = null;
			};
		},
	};
}
