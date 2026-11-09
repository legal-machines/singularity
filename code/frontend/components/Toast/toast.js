import { useSyncExternalStore } from "react";

// What the interface tells the member after an act it took: one line from the
// window's top, with an Undo where the act can be undone, gone after a few
// seconds or at once when dismissed. An irreversible act gets no Undo, and its
// toast says what happened, never that it "can". A notice stays instead, while
// what it tells goes on.
let toasts = [];
const listeners = new Set();
let counter = 0;

function emit() {
	for (const listener of listeners) listener();
}

// At most three stand at once. A notice that tells what is still going on
// stays among them; a passing toast makes room by the oldest passing one.
const MOST_SHOWN = 3;

function withRoomFor(list) {
	let kept = list;
	while (kept.length >= MOST_SHOWN) {
		const oldest = kept.findIndex(item => item.key == null);
		if (oldest < 0) break;
		kept = kept.filter((item, index) => index != oldest);
	}
	return kept;
}

export function toast(text, { action, tone, duration = 5200 } = {}) {
	counter += 1;
	const id = counter;
	toasts = [...withRoomFor(toasts), { id, text, action, tone }];
	emit();
	if (duration > 0) setTimeout(() => dismiss(id), duration);
	return id;
}

// What is still going on between the firm's computers: a Matter coming or
// going, with how much of it is there - `progress` from 0 to 1 -, or this
// computer waiting for another's answer, with no progress; or what waits for
// the member's answer, with the one act that answers it. It stays until it is
// ended; said again under its key, it changes in place.
export function notice(key, { text, progress = null, tone = "working", action }) {
	const shown = toasts.find(item => item.key == key);
	if (shown != null) {
		if (shown.text == text && shown.progress == progress && shown.tone == tone && shown.action?.label == action?.label) return shown.id;
		toasts = toasts.map(item => (item.key == key ? { ...item, text, progress, tone, action } : item));
		emit();
		return shown.id;
	}
	counter += 1;
	const id = counter;
	toasts = [...withRoomFor(toasts), { id, key, text, progress, tone, action }];
	emit();
	return id;
}

// The notice's end, said in place - "Project Harbor has arrived", or what went
// wrong -, with the act it leads to, if any, and gone after a few seconds like
// any toast.
export function endNotice(key, { text, tone = "done", duration = 4200, action } = {}) {
	const shown = toasts.find(item => item.key == key);
	if (shown == null) return;
	toasts = toasts.map(item => (item.key == key
		? { ...item, key: null, text: text ?? item.text, tone, progress: tone == "done" && item.progress != null ? 1 : null,
			action: action ?? item.action }
		: item));
	emit();
	setTimeout(() => dismiss(shown.id), duration);
}

export function hasNotice(key) {
	return toasts.some(item => item.key == key);
}

// A notice no longer true, gone without a word: what it waited for came, and
// what comes next says itself.
export function dropNotice(key) {
	const shown = toasts.find(item => item.key == key);
	if (shown != null) dismiss(shown.id);
}

export function dismiss(id) {
	toasts = toasts.filter(item => item.id != id);
	emit();
}

export function useToasts() {
	return useSyncExternalStore(
		listener => {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},
		() => toasts,
	);
}

// This module keeps the window's state. While the window runs it is never
// swapped for a fresh copy, which would start empty: a change to it reloads
// the window.
if (import.meta.hot) import.meta.hot.decline();
