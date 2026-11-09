import { characters } from "./characters.js";

// An AI colleague's face lives on one clock for the whole window, at 24 frames
// a second. Each colleague is in one of three states - at rest, thinking,
// working - and plays that state's loop again and again. When its presence
// changes, it finishes the loop under way and goes by the state's own route:
// the loop's next first frame is the route's first frame, and the route's last
// frame is the new loop's first, which then goes on from its second, so no
// frame shows twice. A route already begun is never cut: a later change waits
// for it and is routed on from where it ends, the latest one kept. At rest,
// now and then and right after it is pointed at, it makes one of its two
// gestures, each from rest and back to it.
//
// Every face of one colleague shows the same frame at the same moment, drawn
// from the atlas of its own resolution; a face that joins later joins the
// frame under way. Under reduced motion there is no clock: each face holds
// its state's still.
export const FPS = 24;
const BASE = "characters-v4/";
const QUIET = 5000;
const SPREAD = 8000;
const FIRST = 2000;
// A window left alone longer than this, then back, goes on one frame at a
// time from where it was, never through the middle of a prop.
const MOST_STEP_MS = 250;
// A colleague no face showed for this long starts again in its state's loop.
const AWAY_MS = 2000;
// An atlas no face uses is let go after this long.
const KEEP_MS = 30000;
const REACTIONS = ["react-1", "react-2"];

// Presences with a loop of their own; every other rests (the package's
// presence_resolution, the same for every species).
const STATE_OF = { think: "think", work: "work" };

export function stateOfPresence(presence) {
	return STATE_OF[presence] ?? "rest";
}

// 112 pixels while the face is drawn at no more than 112 device pixels,
// otherwise 160.
export function resolutionFor(size, ratio = 1) {
	return size * ratio <= 112 ? "112" : "160";
}

export function stillOf(species, presence, px) {
	return `${BASE}stills/${species}/${stateOfPresence(presence)}-${px}.webp`;
}

let manifest = null;
let manifestLoad = null;

// The motion manifest comes as a classic script (deploy/frontend's
// character-manifest-script plugin), never by fetch: the packaged window
// opens the page from disk, where WebView2 on Windows refuses fetch of a
// file, but a script runs. characters-v4 itself stays exactly as the
// motion package ships it.
const MANIFEST_SCRIPT = "characters-v4-manifest.js";

function readManifest() {
	return new Promise(resolve => {
		if (typeof document == "undefined") {
			resolve(null);
			return;
		}
		if (window.__singularityCharacterMotion != null) {
			resolve(window.__singularityCharacterMotion);
			return;
		}
		const script = document.createElement("script");
		script.src = MANIFEST_SCRIPT;
		script.async = true;
		script.onload = () => resolve(window.__singularityCharacterMotion ?? null);
		script.onerror = () => resolve(null);
		document.head.append(script);
	});
}

function loadManifest() {
	if (manifestLoad == null) {
		manifestLoad = readManifest()
			.then(value => {
				manifest = value;
				repaintAll();
				wake();
				return value;
			})
			.catch(() => null);
	}
	return manifestLoad;
}

// For tests: the manifest as given, and nothing fetched.
export function setMotionForTests(value) {
	manifest = value;
	manifestLoad = Promise.resolve(value);
}

const lives = new Map();
const reported = new Map();
const pictures = new Map();
const uses = new Map();

function stillness() {
	return typeof window != "undefined" && typeof window.matchMedia == "function"
		&& window.matchMedia("(prefers-reduced-motion: reduce)").matches;
}

function motionOf(species) {
	return manifest?.species?.[species] ?? null;
}

// One picture by its address, decoded once and kept while a face uses it.
function picture(address) {
	let entry = pictures.get(address);
	if (entry == null) {
		entry = { image: null, ready: false, failed: false };
		pictures.set(address, entry);
		if (typeof Image == "function") {
			const image = new Image();
			image.decoding = "async";
			entry.image = image;
			const loaded = typeof image.decode == "function"
				? (image.src = address, image.decode())
				: new Promise((resolve, reject) => {
					image.onload = resolve;
					image.onerror = reject;
					image.src = address;
				});
			loaded.then(() => {
				entry.ready = true;
				repaintAll();
			}, () => {
				entry.failed = true;
			});
		}
	}
	return entry;
}

function pageOf(species, px, index) {
	const file = motionOf(species)?.variants?.[px]?.pages?.[index]?.file;
	return file == null ? null : picture(`${BASE}${file}`);
}

// Whether every page a clip's frames stand on is decoded for each resolution
// the colleague's faces use; asking starts what is not.
function isClipReady(life, clipKey) {
	let isReady = true;
	for (const px of resolutionsOf(life)) {
		const variant = motionOf(life.species)?.variants?.[px];
		const clip = variant?.clips?.[clipKey];
		if (clip == null) return false;
		const needed = new Set(clip.frame_ids.map(id => variant.frames[id].page));
		for (const index of needed) {
			const entry = pageOf(life.species, px, index);
			if (entry == null || entry.ready == false) isReady = false;
		}
	}
	return isReady;
}

function resolutionsOf(life) {
	return new Set([...life.faces].map(face => face.px));
}

// The route from the state the colleague is in to what it should do now, or
// null to play its loop again: its latest state, else a gesture when one is
// due at rest. A route whose pages are not decoded yet waits for the next
// loop's end.
function plan(life, now) {
	const motion = motionOf(life.species);
	if (motion == null) return null;
	const wanted = stateOfPresence(life.presence);
	let key = null;
	let reaction = null;
	if (wanted != life.state) {
		key = `${life.state}-to-${wanted}`;
	} else if (life.state == "rest" && (life.pointed == true || now >= life.gestureAt)) {
		reaction = REACTIONS[Math.floor(Math.random() * REACTIONS.length)];
		if (reaction == REACTIONS[life.lastReaction]) reaction = REACTIONS[(life.lastReaction + 1) % REACTIONS.length];
		key = `rest-to-${reaction}`;
	}
	if (key == null) return null;
	const route = (motion.variant_transition_matrix?.[key]?.route ?? []).filter(step => step.startsWith("finish:") == false);
	if (route.length == 0) return null;
	if (route.every(clipKey => isClipReady(life, clipKey)) == false) return null;
	if (reaction != null) {
		life.lastReaction = REACTIONS.indexOf(reaction);
		life.pointed = false;
		const milliseconds = motion.variants?.["112"]?.clips?.[`reactions/${reaction}`]?.duration_ms ?? 0;
		life.gestureAt = now + milliseconds + QUIET + Math.random() * SPREAD;
	}
	return route;
}

// One frame on. A loop's end gives way to the planned route, whose first
// frame stands in for the loop's next first; a route's clip's end is the next
// clip's first frame, which goes on from its second.
export function advance(life, now = Date.now()) {
	const motion = motionOf(life.species);
	const clip = motion?.variants?.["112"]?.clips?.[life.clip];
	if (clip == null) return;
	life.index += 1;
	if (life.index < clip.frame_ids.length) return;
	if (clip.mode == "once") {
		life.clip = life.route.shift() ?? `loops/${life.state}`;
		life.index = 1;
		if (life.clip.startsWith("loops/")) {
			life.state = life.clip.slice("loops/".length);
			const onward = plan(life, now);
			if (onward != null) {
				life.clip = onward[0];
				life.route = onward.slice(1);
			}
		}
		return;
	}
	const onward = plan(life, now);
	if (onward != null) {
		life.clip = onward[0];
		life.route = onward.slice(1);
	}
	life.index = 0;
}

// The state the colleague is in, or the one its route leads to.
function headingOf(life) {
	const last = life.route.at(-1) ?? life.clip;
	return last.startsWith("loops/") ? last.slice("loops/".length) : life.state;
}

// What a face of the colleague draws now: the frame under way from its own
// atlas once that is decoded, else its state's still, else nothing.
export function frameOf(life, px) {
	const motion = motionOf(life.species);
	const variant = motion?.variants?.[px];
	const id = variant?.clips?.[life.clip]?.frame_ids?.[life.index];
	const frame = id == null ? null : variant.frames[id];
	if (frame != null) {
		const entry = pageOf(life.species, px, frame.page);
		if (entry?.ready == true) {
			return { image: entry.image, sx: frame.x, sy: frame.y, sw: frame.width, sh: frame.height, state: headingOf(life) };
		}
	}
	const still = picture(stillOf(life.species, life.state, px));
	return still.ready == true
		? { image: still.image, sx: 0, sy: 0, sw: still.image.naturalWidth, sh: still.image.naturalHeight, state: headingOf(life) }
		: { image: null, state: headingOf(life) };
}

function paint(life) {
	for (const face of life.faces) face.paint(frameOf(life, face.px));
}

function repaintAll() {
	for (const life of lives.values()) paint(life);
}

// The one clock: every colleague a face shows goes on by the same frames.
let raf = 0;
let last = 0;
let carry = 0;

function wake() {
	if (raf != 0 || typeof requestAnimationFrame != "function" || stillness()) return;
	if ([...lives.values()].some(life => life.faces.size > 0) == false) return;
	last = 0;
	raf = requestAnimationFrame(tick);
}

function tick(now) {
	raf = 0;
	const shown = [...lives.values()].filter(life => life.faces.size > 0);
	if (shown.length == 0 || stillness()) {
		last = 0;
		return;
	}
	const delta = last == 0 ? 0 : Math.min(now - last, MOST_STEP_MS);
	last = now;
	carry += (delta * FPS) / 1000;
	const steps = Math.floor(carry);
	carry -= steps;
	if (steps > 0 && manifest != null) {
		const wall = Date.now();
		for (const life of shown) {
			for (let step = 0; step < steps; step += 1) advance(life, wall);
			paint(life);
		}
	}
	raf = requestAnimationFrame(tick);
}

function letGoLater(species, px) {
	setTimeout(() => {
		if ((uses.get(`${species}/${px}`) ?? 0) > 0) return;
		for (const address of [...pictures.keys()]) {
			if (address.startsWith(`${BASE}atlases/${species}/${px}-`)) pictures.delete(address);
		}
	}, KEEP_MS);
}

function newLife(species, presence) {
	const state = stateOfPresence(presence);
	return {
		species, presence, state, clip: `loops/${state}`, index: 0, route: [], faces: new Set(),
		lastReaction: -1, pointed: false, gestureAt: Date.now() + FIRST + Math.random() * SPREAD, leftAt: 0,
	};
}

// A face of the colleague joins its life: `face.px` is the resolution it
// draws at, `face.paint(frame)` draws a frame. The first face starts the
// colleague in its state's loop; a later one joins the frame under way. The
// last to leave stops it.
export function join(memberId, species, presence, face) {
	if (characters[species] == null) return () => {};
	const known = presence ?? reported.get(memberId) ?? "rest";
	let life = lives.get(memberId);
	if (life == null || life.species != species || (life.faces.size == 0 && Date.now() - life.leftAt > AWAY_MS)) {
		life = newLife(species, known);
		lives.set(memberId, life);
	}
	if (presence != null) life.presence = presence;
	life.faces.add(face);
	const usage = `${species}/${face.px}`;
	uses.set(usage, (uses.get(usage) ?? 0) + 1);
	if (manifest == null) loadManifest();
	isClipReady(life, life.clip);
	face.paint(frameOf(life, face.px));
	wake();
	return () => {
		life.faces.delete(face);
		uses.set(usage, Math.max(0, (uses.get(usage) ?? 1) - 1));
		if (uses.get(usage) == 0) letGoLater(species, face.px);
		if (life.faces.size == 0) life.leftAt = Date.now();
	};
}

// What the backend says the colleague is doing: taken up at the end of the
// loop or route under way.
export function report(memberId, presence) {
	reported.set(memberId, presence);
	const life = lives.get(memberId);
	if (life != null) life.presence = presence;
}

// Pointed at, a resting colleague makes a gesture as its loop ends; points
// while one is due or under way are one.
export function point(memberId) {
	const life = lives.get(memberId);
	if (life != null && stateOfPresence(life.presence) == "rest") life.pointed = true;
}

// For tests: a colleague's life as it is now.
export function lifeOf(memberId) {
	return lives.get(memberId) ?? null;
}
