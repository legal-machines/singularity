import { isEmulation } from "../emulation/emulation.js";
import { mockNow } from "./workspace.js";

// Now, as the window reads it: the computer's own clock in the app, and the
// prototype's fixed morning (data/workspace.js) only where it plays its
// scripted day - onboarding's emulation, or a window with no native side. A
// real Matter's letters, calendar and time are never dated from that morning
// (2026-10-31: a letter of 23 October read "today", and the calendar opened on
// the prototype's day).
export function nowDate() {
	const isReal = isEmulation == false && typeof window != "undefined" && window.nativeApp != null;
	return isReal ? new Date() : new Date(mockNow);
}

// The same moment written as the prototype writes its own, in local time,
// to the second: "2026-10-31T09:41:00".
export function nowLocal() {
	const date = nowDate();
	const two = value => String(value).padStart(2, "0");
	return `${date.getFullYear()}-${two(date.getMonth() + 1)}-${two(date.getDate())}T${two(date.getHours())}:${two(date.getMinutes())}:${two(date.getSeconds())}`;
}
