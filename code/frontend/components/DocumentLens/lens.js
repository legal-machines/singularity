import { useSyncExternalStore } from "react";

// The one document the window shows over everything, as written and as an AI
// provider receives it (DocumentLens.jsx, D-233): { projectId, documentId,
// name }, or null.
let shown = null;
const listeners = new Set();

function tell() {
	for (const listener of listeners) listener();
}

export function openDocumentLens(document) {
	if (document?.projectId == null || document?.documentId == null) return;
	shown = { projectId: document.projectId, documentId: document.documentId, name: document.name ?? "" };
	tell();
}

export function closeDocumentLens() {
	shown = null;
	tell();
}

export function useDocumentLens() {
	return useSyncExternalStore(listener => {
		listeners.add(listener);
		return () => listeners.delete(listener);
	}, () => shown);
}
