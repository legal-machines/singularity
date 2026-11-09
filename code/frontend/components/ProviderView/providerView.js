import { useSyncExternalStore } from "react";

// Which conversations the member reads as an AI provider receives them, by
// chat id, while the window is open.
const shown = new Set();
const listeners = new Set();
let version = 0;

export function toggleProviderView(chatId) {
	if (chatId == null) return;
	if (shown.has(chatId)) shown.delete(chatId);
	else shown.add(chatId);
	version += 1;
	for (const listener of listeners) listener();
}

export function useProviderView(chatId) {
	useSyncExternalStore(listener => {
		listeners.add(listener);
		return () => listeners.delete(listener);
	}, () => version);
	return chatId != null && shown.has(chatId);
}
