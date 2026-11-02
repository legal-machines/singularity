import { useEffect, useState } from "react";

import styles from "./WindowControls.module.css";

// Minimize, maximize and close, where the page carries the window's caption
// (Windows, D-049), drawn as the app's own (the owner, 2026-10-24: "make
// custom Windows buttons", so the header's search can run its full width):
// three small buttons in the middle of the title band, each a fine line
// drawing on a 16-pixel grid. They run the window's own commands, so they do
// what Windows' buttons do. macOS and Linux keep their native controls, and
// this draws nothing there.
const glyphs = {
	minimize: <path d="M4 8.5H12" />,
	maximize: <rect x="4.25" y="4.25" width="7.5" height="7.5" rx="1.5" />,
	restore: <><rect x="4.25" y="5.75" width="6" height="6" rx="1.25" /><path d="M6.25 4.25H10.25C11.08 4.25 11.75 4.92 11.75 5.75V9.75" /></>,
	close: <path d="M4.75 4.75L11.25 11.25M11.25 4.75L4.75 11.25" />,
};

function Glyph({ children }) {
	return (
		<svg className={styles.glyph} viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" strokeWidth="1.25" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
			{children}
		</svg>
	);
}

function readCaptionInPage() {
	return window.nativeApp?.windowFrame == "page" || document.documentElement.dataset.nativeWindowFrame == "page";
}

// A window of a fixed size (onboarding's) has no Maximize; over a dark ground
// (onboarding's painting) the buttons are drawn light.
export function WindowControls({ canMaximize = true, tone = null }) {
	const [frame, setFrame] = useState(() => ({ captionInPage: readCaptionInPage(), maximized: false }));
	const [isActive, setIsActive] = useState(() => document.hasFocus());

	// The maximize button shows restore while the window is maximized, however
	// it got there: the button, a double-click on the title bar, Snap or
	// Win+Up. Each of them resizes the page.
	useEffect(() => {
		if (window.nativeApp?.getWindowFrame == null) return undefined;

		let isCurrent = true;

		function refresh() {
			window.nativeApp.getWindowFrame()
				.then(next => {
					if (isCurrent == true) setFrame({ captionInPage: next.captionInPage == true, maximized: next.maximized == true });
				})
				.catch(() => {});
		}

		function activate() {
			setIsActive(true);
		}

		function deactivate() {
			setIsActive(false);
		}

		refresh();
		window.addEventListener("resize", refresh);
		window.addEventListener("focus", activate);
		window.addEventListener("blur", deactivate);

		return () => {
			isCurrent = false;
			window.removeEventListener("resize", refresh);
			window.removeEventListener("focus", activate);
			window.removeEventListener("blur", deactivate);
		};
	}, []);

	if (frame.captionInPage != true) return null;

	function run(command) {
		window.nativeApp.runWindowCaptionCommand(command)
			.then(result => setFrame(previous => ({ ...previous, maximized: result.maximized == true })))
			.catch(() => {});
	}

	const resize = frame.maximized == true
		? { command: "restore", glyph: glyphs.restore, label: "Restore Down" }
		: { command: "maximize", glyph: glyphs.maximize, label: "Maximize" };

	return (
		<div className={[styles.controls, isActive == true ? null : styles.controls_inactive, tone == "onDark" ? styles.on_dark : null].filter(Boolean).join(" ")}>
			<button className={styles.button} type="button" title="Minimize" aria-label="Minimize" onClick={() => run("minimize")}>
				<Glyph>{glyphs.minimize}</Glyph>
			</button>
			{canMaximize ? (
				<button className={styles.button} type="button" title={resize.label} aria-label={resize.label} onClick={() => run(resize.command)}>
					<Glyph>{resize.glyph}</Glyph>
				</button>
			) : null}
			<button className={`${styles.button} ${styles.close}`} type="button" title="Close" aria-label="Close" onClick={() => run("close")}>
				<Glyph>{glyphs.close}</Glyph>
			</button>
		</div>
	);
}
