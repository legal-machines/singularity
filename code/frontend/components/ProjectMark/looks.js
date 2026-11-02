import { useSyncExternalStore } from "react";

import { keepMatterMark } from "../../data/native.js";
import { pigments } from "./pigments.js";

// The colours a project's mark may take: the palette's pigments.
export const projectColors = Object.values(pigments);

// What a member chose for a project's mark: its colour, and an icon or its
// initials. Shown at once from here; a Matter of the firm keeps it with
// itself (D-200), and the prototype's Matters keep it in memory.
const chosen = new Map();
const listeners = new Set();
let version = 0;

export function setProjectLook(matterId, change) {
	chosen.set(matterId, { ...chosen.get(matterId), ...change });
	version += 1;
	for (const listener of listeners) listener();
	keepMatterMark(matterId, change).catch(() => {});
}

function subscribe(listener) {
	listeners.add(listener);
	return () => listeners.delete(listener);
}

function snapshot() {
	return version;
}

// A project's look now: its colour, and its icon or null for its initials.
export function useProjectLook(matter) {
	useSyncExternalStore(subscribe, snapshot);
	const own = chosen.get(matter.id) ?? {};
	return {
		color: own.color ?? matter.color,
		icon: Object.hasOwn(own, "icon") ? own.icon : matter.icon ?? null,
	};
}

// A project's initials: the first letters of its first two words that count;
// a code name of one word gives one.
export function monogram(name) {
	const words = name.split(/\s+/).filter(word => !["of", "the", "in", "for", "and", "v.", "a"].includes(word.toLowerCase()));
	return words.slice(0, 2).map(word => word.charAt(0).toUpperCase()).join("");
}
