import { useSyncExternalStore } from "react";

// The sidebar's place for what the open page adds to it: the calendar puts
// its small month and its calendars there, under the places. The sidebar
// gives the element; a page draws into it with a portal.
let slot = null;
const listeners = new Set();

export function setSidebarSlot(element) {
	if (element == slot) return;
	slot = element;
	for (const listener of listeners) listener();
}

export function useSidebarSlot() {
	return useSyncExternalStore(
		listener => {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},
		() => slot,
	);
}
