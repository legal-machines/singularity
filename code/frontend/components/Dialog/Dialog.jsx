import { useEffect, useId, useLayoutEffect, useRef } from "react";
import { createPortal } from "react-dom";

import { focusables } from "../Floating/position.js";
import { remeasureTitlebarControls } from "../Window/titlebar.js";

import styles from "./Dialog.module.css";

// A dialog stops the page for one decision or one short task. It says what it
// is in its title, what follows from it in its lines, and holds its actions at
// its foot, the leading one last. It stands in the middle of the window, and
// the page behind is not dimmed: white washes out from the middle over the
// whole window as it opens and draws back into the middle as it closes. It is
// part of that white, with no frame, shadow or close button: "only the dialog".
// Its own buttons and Escape leave it, and the focus returns to what opened it.
// A press anywhere beside it never does: the white is the dialog's, and a stray
// click never throws work away. While a dialog stands, the window's own
// controls over the white - the sidebar's button - are put away. A dialog laid
// out "spacious" is a short task of its own with room to breathe: a larger
// title and its line, what it holds under them, and its actions at its foot as
// every dialog's. What its leading action waits on, such as a consent to tick,
// and what went wrong when it was tried, stand in its footnote: over the
// actions and under what scrolls, so they are always in sight.
export function Dialog({ title, description, size = "medium", tone, layout, fits = false, lead = null, onClose, actions, footnote, children, initialFocus }) {
	const paperRef = useRef(null);
	const scrimRef = useRef(null);
	const opener = useRef(document.activeElement);

	// It leaves as it came: a still copy of it draws the white back into the
	// middle while the dialog goes, then is gone. React takes the dialog away at
	// once; the copy is only seen, never pressed.
	// Only a dialog truly gone leaves a copy: one React only tried taking away
	// (development's double mount) is still on the page a moment later.
	useLayoutEffect(() => () => {
		const scrim = scrimRef.current;
		if (scrim == null) return;
		queueMicrotask(() => {
			if (scrim.isConnected == true) return;
			const ghost = scrim.cloneNode(true);
			ghost.dataset.leaving = "";
			ghost.setAttribute("aria-hidden", "true");
			document.body.append(ghost);
			setTimeout(() => ghost.remove(), 460);
		});
	}, []);
	const titleId = useId();
	const descriptionId = useId();

	// Every dialog open, counted on the page, so the window's controls over the
	// white stay away until the last one closes.
	useEffect(() => {
		const root = document.documentElement;
		root.dataset.dialogOpen = String(Number(root.dataset.dialogOpen ?? 0) + 1);
		remeasureTitlebarControls();
		return () => {
			const left = Number(root.dataset.dialogOpen ?? 1) - 1;
			if (left > 0) root.dataset.dialogOpen = String(left);
			else delete root.dataset.dialogOpen;
			remeasureTitlebarControls();
		};
	}, []);

	useEffect(() => {
		const paper = paperRef.current;
		const target = initialFocus != null ? paper.querySelector(initialFocus) : null;
		// The task's own first control, never the way out above the title (lead).
		(target ?? focusables(paper).find(element => element.dataset.dialogClose == null && element.closest("[data-dialog-lead]") == null) ?? paper).focus();
		const returnTo = opener.current;

		function keys(event) {
			if (event.key == "Escape") {
				event.preventDefault();
				event.stopPropagation();
				onClose();
			}
			if (event.key == "Tab") {
				const list = focusables(paper);
				if (list.length == 0) return;
				const first = list[0];
				const last = list[list.length - 1];
				if (event.shiftKey && document.activeElement == first) {
					event.preventDefault();
					last.focus();
				} else if (!event.shiftKey && document.activeElement == last) {
					event.preventDefault();
					first.focus();
				}
			}
		}

		paper.addEventListener("keydown", keys);
		return () => {
			paper.removeEventListener("keydown", keys);
			returnTo?.focus?.();
		};
	}, []);

	return createPortal(
		<div ref={scrimRef} className={styles.scrim}>
			<div className={styles.wash} aria-hidden="true" />
			<div
				ref={paperRef}
				className={[styles.dialog, styles[size], tone == "seal" ? styles.seal : null].filter(Boolean).join(" ")}
				data-layout={layout ?? undefined}
				data-fits={fits || undefined}
				role="dialog"
				aria-modal="true"
				aria-labelledby={titleId}
				aria-describedby={description != null ? descriptionId : undefined}
				tabIndex={-1}
			>
				{/* An act above the title, at the dialog's top left: a way out
				    that is not the task's own end, such as "Finish later". */}
				{lead != null ? <div className={styles.lead} data-dialog-lead="">{lead}</div> : null}
				<header className={styles.head}>
					<h2 id={titleId} className={styles.title}>{title}</h2>
				</header>
				{description != null ? <p id={descriptionId} className={styles.description}>{description}</p> : null}
				{children != null ? <div className={styles.body}>{children}</div> : null}
				{footnote != null ? <div className={styles.footnote}>{footnote}</div> : null}
				{actions != null ? <footer className={styles.foot}>{actions}</footer> : null}
			</div>
		</div>,
		document.body,
	);
}

