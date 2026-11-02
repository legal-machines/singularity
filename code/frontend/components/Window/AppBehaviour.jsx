import { useEffect, useState } from "react";

import { Menu } from "../Menu/Menu.jsx";

// Where the system's own editing behaviour stays: a member types here.
const EDITABLE = "input, textarea, select, [contenteditable=''], [contenteditable='true']";

// Keys a web view acts on by itself: find, reload, caret browsing, full
// screen, developer tools, and the browser keys of some keyboards.
const BROWSER_KEYS = new Set(["F3", "F5", "F6", "F7", "F11", "F12", "BrowserBack", "BrowserForward", "BrowserRefresh", "BrowserSearch", "BrowserHome", "BrowserFavorites", "BrowserStop"]);

// Keys are read by where they stand on the keyboard, so a Russian or any
// other layout is held the same.
// With the command key alone: print, save, open, find, reload, page source,
// downloads, bookmarks, the address bar, and zoom.
const COMMAND_KEYS = new Set(["KeyP", "KeyS", "KeyO", "KeyF", "KeyG", "KeyR", "KeyU", "KeyJ", "KeyD", "KeyL", "Equal", "Minus", "Digit0", "NumpadAdd", "NumpadSubtract", "Numpad0"]);

// The same, where the system's own commands do not use them: on macOS the
// command key with H, N, T and W belongs to the application's menu
// (Platform/MacOSChrome).
const COMMAND_KEYS_ELSEWHERE = new Set(["KeyH", "KeyN", "KeyT", "KeyW"]);

// With Shift: developer tools, print, reload, save, new and reopened windows,
// zoom, and clearing what the web view keeps.
const SHIFT_COMMAND_KEYS = new Set(["KeyI", "KeyJ", "KeyC", "KeyP", "KeyR", "KeyS", "KeyN", "KeyT", "KeyB", "KeyO", "Delete", "Equal", "Minus"]);

// With Option on macOS: Safari's developer keys.
const OPTION_COMMAND_KEYS = new Set(["KeyI", "KeyJ", "KeyC", "KeyU"]);

// Singularity is an application, not a web page (the owner, 2026-10-24:
// "forbid everything a browser does"). None of the web view's own behaviour
// reaches a member: no browser menu on a right click, no print, save, find,
// reload, zoom, page source or developer tools from the keyboard or the
// wheel, no picture or page dragged away, no autoscroll, no list of saved
// entries under a field. A text field keeps the system's editing menu, as in
// any application, and text a member selected in a post, a chat or a
// document offers Copy.
export function AppBehaviour() {
	const [copy, setCopy] = useState(null);

	useEffect(() => {
		const isMac = document.documentElement.dataset.nativePlatform == "darwin" || navigator.platform.startsWith("Mac");

		function onContextMenu(event) {
			// A place with its own menu, such as an inbox line, has opened it.
			if (event.defaultPrevented) return;
			if (event.target.closest?.(EDITABLE)) return;
			event.preventDefault();
			const selection = window.getSelection();
			if (selection == null || selection.isCollapsed || selection.rangeCount == 0) return;
			const range = selection.getRangeAt(0);
			const text = selection.toString();
			if (text.trim() == "" || isOnRange(range, event.clientX, event.clientY) == false) return;
			setCopy({ range: range.cloneRange(), text, x: event.clientX, y: event.clientY });
		}

		function onKeyDown(event) {
			if (BROWSER_KEYS.has(event.key)) {
				event.preventDefault();
				return;
			}
			const command = isMac ? event.metaKey : event.ctrlKey;
			if (command == false) return;
			// Control-Command on macOS is the system's, such as full screen.
			if (isMac && event.ctrlKey) return;
			const code = event.code;
			if (event.altKey) {
				if (isMac && OPTION_COMMAND_KEYS.has(code)) event.preventDefault();
				return;
			}
			if (event.shiftKey) {
				if (SHIFT_COMMAND_KEYS.has(code)) event.preventDefault();
				return;
			}
			if (COMMAND_KEYS.has(code) || (isMac == false && COMMAND_KEYS_ELSEWHERE.has(code))) event.preventDefault();
		}

		// The wheel or a pinch with the command key zooms a web view.
		function onWheel(event) {
			if (event.ctrlKey || (isMac && event.metaKey)) event.preventDefault();
		}

		function onGesture(event) {
			event.preventDefault();
		}

		// The middle button scrolls a web view by itself; in a field on Linux
		// it pastes, as the system does.
		function onMouseDown(event) {
			if (event.button == 1 && event.target.closest?.(EDITABLE) == null) event.preventDefault();
		}

		function onDragStart(event) {
			if (event.target.closest?.(EDITABLE) || event.target.closest?.("[data-draggable]")) return;
			event.preventDefault();
		}

		// Nothing is dropped on the window unless a place takes it; files are
		// taken by the window itself (Platform/FileDrop).
		function onDragOver(event) {
			if (event.defaultPrevented) return;
			event.preventDefault();
			if (event.dataTransfer != null) event.dataTransfer.dropEffect = "none";
		}

		function onDrop(event) {
			if (event.defaultPrevented == false) event.preventDefault();
		}

		// A scrolled area says so (data-scrolled), so its top edge can fade into
		// the band above it while anything is under it; at its top it is crisp.
		function onScroll(event) {
			const area = event.target;
			if (area instanceof Element) area.toggleAttribute("data-scrolled", area.scrollTop > 0);
		}

		function onFocusIn(event) {
			const field = event.target;
			if ((field instanceof HTMLInputElement || field instanceof HTMLTextAreaElement) && field.hasAttribute("autocomplete") == false) {
				field.setAttribute("autocomplete", "off");
			}
		}

		const options = { passive: false };
		window.addEventListener("contextmenu", onContextMenu);
		window.addEventListener("keydown", onKeyDown, true);
		window.addEventListener("wheel", onWheel, options);
		document.addEventListener("gesturestart", onGesture, options);
		document.addEventListener("gesturechange", onGesture, options);
		window.addEventListener("mousedown", onMouseDown, true);
		window.addEventListener("dragstart", onDragStart);
		window.addEventListener("dragover", onDragOver);
		window.addEventListener("drop", onDrop);
		window.addEventListener("focusin", onFocusIn, true);
		document.addEventListener("scroll", onScroll, true);
		return () => {
			document.removeEventListener("scroll", onScroll, true);
			window.removeEventListener("contextmenu", onContextMenu);
			window.removeEventListener("keydown", onKeyDown, true);
			window.removeEventListener("wheel", onWheel, options);
			document.removeEventListener("gesturestart", onGesture, options);
			document.removeEventListener("gesturechange", onGesture, options);
			window.removeEventListener("mousedown", onMouseDown, true);
			window.removeEventListener("dragstart", onDragStart);
			window.removeEventListener("dragover", onDragOver);
			window.removeEventListener("drop", onDrop);
			window.removeEventListener("focusin", onFocusIn, true);
		};
	}, []);

	if (copy == null) return null;

	const anchor = {
		getBoundingClientRect: () => ({ top: copy.y, bottom: copy.y, left: copy.x, right: copy.x, width: 0, height: 0 }),
		contains: () => false,
		focus: () => {},
	};

	return (
		<Menu
			anchor={anchor}
			label="Selected text"
			minWidth={180}
			onClose={() => setCopy(null)}
			items={[{ key: "copy", label: "Copy", onSelect: () => copyText(copy) }]}
		/>
	);
}

// Whether a point stands on the selected text, as a right click on it does.
function isOnRange(range, x, y) {
	for (const rect of range.getClientRects()) {
		if (x >= rect.left - 2 && x <= rect.right + 2 && y >= rect.top - 2 && y <= rect.bottom + 2) return true;
	}
	return false;
}

// Copies the selected text with its formatting, as the system's Copy does,
// or as plain text where the web view refuses.
function copyText({ range, text }) {
	const selection = window.getSelection();
	selection?.removeAllRanges();
	selection?.addRange(range);
	let isCopied = false;
	try {
		isCopied = document.execCommand("copy");
	} catch {
		isCopied = false;
	}
	if (isCopied == false) navigator.clipboard?.writeText(text).catch(() => {});
}
