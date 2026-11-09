import { useEffect } from "react";

import { isEmulation } from "../../emulation/emulation.js";
import { pixelsOf } from "./cssLength.js";

// On macOS the window keeps a native strip along its top that drags it
// (MacOSChrome); every control of the page inside that strip is handed to the
// window, so a press on it reaches the page (window.setTitlebarControls).
// They are measured again when the place or the window's size changes, and
// once the bundled typeface has loaded, since it sets the width of every word
// in the strip. Windows and Linux need nothing: there the page marks its drag
// areas itself (CSS app-region). `layout` names anything else that moves the
// strip's controls, such as the sidebar shown or hidden; they are measured
// again once its movement has settled. Measures the strip's controls again
// now and once a notice's arrival has settled: a notice of the window
// (Toaster) comes down into the strip, and its Undo must be pressed, not drag
// the window.
let remeasure = () => {};

export function remeasureTitlebarControls() {
	remeasure();
}

export function useNativeTitlebarControls(place, layout) {
	useEffect(() => {
		if (isEmulation || window.nativeApp?.platform != "darwin" || window.nativeApp?.setTitlebarControls == null) return undefined;

		function report() {
			const band = pixelsOf(document.body, "--titlebar-height") || 60;
			// A control the page only keeps the room of - hidden, its copy
			// travelling in the strip (MovingHead) - is none.
			const controls = [...document.querySelectorAll("button, input, textarea, select, a")]
				.filter(element => element.closest("[aria-hidden='true']") == null && getComputedStyle(element).visibility != "hidden")
				.map(element => element.getBoundingClientRect())
				.filter(rect => rect.width > 0 && rect.height > 0 && rect.top < band)
				.map(rect => ({ x: rect.x, y: rect.y, width: rect.width, height: rect.height }));
			window.nativeApp.setTitlebarControls(controls).catch(() => {});
		}

		let isCurrent = true;
		let frame = requestAnimationFrame(report);
		const settled = setTimeout(report, 320);
		let arrived = 0;
		remeasure = () => {
			cancelAnimationFrame(frame);
			frame = requestAnimationFrame(report);
			clearTimeout(arrived);
			arrived = setTimeout(report, 620);
		};
		document.fonts?.ready.then(() => {
			if (isCurrent) report();
		});
		window.addEventListener("resize", report);
		// What covers the page - a dialog, the Office guide - is put into the
		// body: its controls in the strip are handed to the window as it opens,
		// and taken back as it closes, or a press on them would drag the window.
		const covers = new MutationObserver(() => {
			cancelAnimationFrame(frame);
			frame = requestAnimationFrame(report);
		});
		covers.observe(document.body, { childList: true });

		return () => {
			isCurrent = false;
			remeasure = () => {};
			cancelAnimationFrame(frame);
			clearTimeout(settled);
			clearTimeout(arrived);
			covers.disconnect();
			window.removeEventListener("resize", report);
		};
	}, [place, layout]);
}
