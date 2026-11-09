import { useLayoutEffect, useRef, useState } from "react";

import { pixelsOf } from "../Window/cssLength.js";
import { remeasureTitlebarControls } from "../Window/titlebar.js";

import styles from "./MovingHead.module.css";

// The title as the window's band sets it.
const DOCKED = { size: 15, weight: 500, line: 20 };
const FACES_IN_BAND = 20 / 22;
const BAND_LEFT = 24;
// A face that travels with the title docks this tall, the title beside it.
const LEAD_IN_BAND = 28;

// A page's head that travels into the window's band as the page scrolls, on
// every page that has one. The title, and on a thread or a letter the
// project's band and the faces, are drawn once, over the page; an unseen copy
// in the page's flow (its parts marked data-part="title", "band" and "faces"
// inside `placeholderRef`) keeps their room and says where they rest and in
// what type. As the page scrolls, the title rises with the words and, early,
// slides left and draws in from its own size and weight to the band's 15px
// medium, set as real type at every step so it stays sharp. The band hangs on
// the title: it comes round to the title's right side and rises onto its row
// late, so the two never cross; the faces ride beside it. Scrolled back to the
// top, all of it comes down to rest. Each part follows the scroll frame by
// frame. A member's page carries its face and its buttons too: the face,
// `lead`, draws in to the band's left with the title docking beside it, and
// the buttons, `actions`, rise into the band where they stand, clear of the
// window's own buttons on its right.
//
// A title that can be renamed, `onRename`, is edited where it stands: a press
// on it at rest makes it a field in its own type, Return or leaving it keeps
// the name, Escape keeps the old one; `isNaming` opens it so, its words
// chosen, and `onNamed` follows Return.
export function MovingHead({ scrollRef, placeholderRef, title, band, faces, lead, actions, onTop, onRename, isNaming = false, onNamed }) {
	const [draft, setDraft] = useState(isNaming && onRename != null ? title : null);
	const isEditing = draft != null;
	// Kept once: Return takes the field away, and its going would keep it again.
	const isOpen = useRef(isEditing);
	const edit = () => {
		isOpen.current = true;
		setDraft(title);
	};
	const layerRef = useRef(null);
	const titleRef = useRef(null);
	const bandRef = useRef(null);
	const facesRef = useRef(null);
	const leadRef = useRef(null);
	const actionsRef = useRef(null);

	useLayoutEffect(() => {
		const scroller = scrollRef.current;
		const placeholder = placeholderRef.current;
		const layer = layerRef.current;
		if (scroller == null || placeholder == null || layer == null) return undefined;

		let geometry = null;
		let frame = 0;
		// The fonts' promise cannot be cancelled: a refresh it brings after the
		// head is gone finds no title, and does nothing.
		let live = true;

		// Where each part rests, as at the top of the page, in what type the
		// title rests, and where the title docks.
		function measure() {
			const layerBox = layer.getBoundingClientRect();
			const at = part => {
				const element = placeholder.querySelector(`[data-part="${part}"]`);
				if (element == null) return null;
				const box = element.getBoundingClientRect();
				return { x: box.left - layerBox.left, y: box.top - layerBox.top + scroller.scrollTop, element };
			};
			const rest = {
				title: at("title"),
				band: bandRef.current != null ? at("band") : null,
				faces: facesRef.current != null ? at("faces") : null,
				lead: leadRef.current != null ? at("lead") : null,
				actions: actionsRef.current != null ? at("actions") : null,
			};
			if (rest.title == null) return;
			const type = getComputedStyle(rest.title.element);
			const size = parseFloat(type.fontSize);
			const font = {
				family: type.fontFamily,
				size,
				weight: parseFloat(type.fontWeight),
				tracking: parseFloat(type.letterSpacing) || 0,
				line: (parseFloat(type.lineHeight) || size * 1.25) / size,
			};
			const middle = pixelsOf(layer, "--titlebar-height") / 2 || 30;
			const hasLead = rest.lead != null && leadRef.current != null;
			// Past the sidebar's button while the sidebar is put away (App's
			// --band-lead), never under the traffic lights.
			const bandLeft = Math.max(BAND_LEFT, pixelsOf(layer, "--band-lead"));
			const docked = { x: bandLeft + (hasLead ? LEAD_IN_BAND + 10 : 0), y: middle - DOCKED.line / 2 };
			const reserve = parseFloat(getComputedStyle(layer).getPropertyValue("--caption-reserve")) || 0;
			geometry = {
				rest,
				font,
				docked,
				bandLeft,
				middle,
				lead: hasLead ? { height: leadRef.current.offsetHeight } : null,
				actions: rest.actions != null && actionsRef.current != null ? {
					height: actionsRef.current.offsetHeight,
					dockedX: Math.min(rest.actions.x, layerBox.width - reserve - actionsRef.current.offsetWidth - BAND_LEFT),
				} : null,
				band: bandRef.current != null ? { width: bandRef.current.offsetWidth, height: bandRef.current.offsetHeight } : null,
				faces: facesRef.current != null ? { width: facesRef.current.offsetWidth, height: facesRef.current.offsetHeight } : null,
				distance: Math.max(1, rest.title.y - docked.y),
			};
			titleRef.current.style.fontFamily = font.family;
		}

		function place() {
			frame = 0;
			if (geometry == null) return;
			const { rest, font, docked } = geometry;
			const progress = Math.min(1, Math.max(0, scroller.scrollTop / geometry.distance));
			const between = (from, to, amount = progress) => from + (to - from) * amount;
			const early = 1 - Math.pow(1 - progress, 3);

			// The title: real type at every step, and a move.
			const title = titleRef.current;
			if (title == null) return;
			const size = between(font.size, DOCKED.size, early);
			const line = between(font.size * font.line, DOCKED.line, early);
			title.style.fontSize = `${size}px`;
			title.style.lineHeight = `${line}px`;
			title.style.fontWeight = `${Math.round(between(font.weight, DOCKED.weight, early))}`;
			title.style.letterSpacing = `${between(font.tracking, 0, early)}px`;
			const titleX = between(rest.title.x, docked.x, early);
			const titleY = between(rest.title.y, docked.y);
			title.style.transform = `translate(${titleX}px, ${titleY}px)`;
			// Docked, its title and buttons stand in the window's strip: the window
			// is told where they are, or a press on them would drag it; undocked,
			// it is told they have left.
			const isDocked = progress >= 1;
			if (layer.hasAttribute("data-docked") != isDocked) {
				layer.toggleAttribute("data-docked", isDocked);
				remeasureTitlebarControls();
			}

			// The face: drawn in to the band's left, beside the docked title.
			if (geometry.lead != null) {
				const scale = between(1, LEAD_IN_BAND / geometry.lead.height, early);
				const leadX = between(rest.lead.x, geometry.bandLeft, early);
				const leadY = between(rest.lead.y, geometry.middle - LEAD_IN_BAND / 2, progress);
				leadRef.current.style.transform = `translate(${leadX}px, ${leadY}px) scale(${scale})`;
			}
			// The buttons: up into the band, on its right.
			if (geometry.actions != null) {
				const actionsX = between(rest.actions.x, geometry.actions.dockedX, early);
				const actionsY = between(rest.actions.y, geometry.middle - geometry.actions.height / 2, progress);
				actionsRef.current.style.transform = `translate(${actionsX}px, ${actionsY}px)`;
			}
			if (geometry.band == null) return;

			// The band, on the title's right once it reaches the title's row.
			const width = title.offsetWidth;
			const round = early;
			// On a thread the band rises late, so it never crosses the title; on
			// a page that carries its face, the line under the name rises with
			// everything else and lands with it.
			const rise = geometry.lead != null ? progress : Math.pow(progress, 4);
			const bandX = titleX + between(rest.band.x - rest.title.x, width + 8, round);
			const bandY = titleY + between(rest.band.y - rest.title.y, (line - geometry.band.height) / 2, rise);
			bandRef.current.style.transform = `translate(${bandX}px, ${bandY}px)`;
			if (geometry.faces == null) return;

			// The faces, beside the band.
			const facesScale = between(1, FACES_IN_BAND);
			const gap = between(rest.faces.x - rest.band.x - geometry.band.width, 6);
			const facesX = bandX + geometry.band.width + gap;
			const facesY = bandY + (geometry.band.height - geometry.faces.height * facesScale) / 2;
			facesRef.current.style.transform = `translate(${facesX}px, ${facesY}px) scale(${facesScale})`;
		}

		function onScroll() {
			if (frame == 0) frame = requestAnimationFrame(place);
		}

		function refresh() {
			if (!live || titleRef.current == null) return;
			measure();
			place();
		}

		refresh();
		const resize = new ResizeObserver(refresh);
		resize.observe(scroller);
		scroller.addEventListener("scroll", onScroll, { passive: true });
		document.fonts?.ready.then(refresh);
		return () => {
			live = false;
			resize.disconnect();
			scroller.removeEventListener("scroll", onScroll);
			cancelAnimationFrame(frame);
		};
	}, [scrollRef, placeholderRef]);

	// The name kept: a new one is given to `onRename`; an empty or unchanged
	// one leaves the title as it was.
	function keep(text, isReturn) {
		if (isOpen.current == false) return;
		isOpen.current = false;
		setDraft(null);
		const name = (text ?? "").trim();
		if (name != "" && name != title) onRename(name);
		if (isReturn) onNamed?.();
	}

	return (
		<div ref={layerRef} className={styles.layer}>
			<h1 ref={titleRef} className={styles.title}>
				{isEditing ? (
					<input
						className={styles.title_field}
						value={draft}
						aria-label="Name"
						placeholder="Name it"
						maxLength={80}
						spellCheck={false}
						ref={field => {
							if (field != null && field.dataset.ready == null) {
								field.dataset.ready = "";
								field.focus();
								field.select();
							}
						}}
						onChange={event => setDraft(event.target.value)}
						onKeyDown={event => {
							if (event.key == "Enter") {
								event.preventDefault();
								keep(draft, true);
							} else if (event.key == "Escape") {
								event.preventDefault();
								isOpen.current = false;
								setDraft(null);
							}
						}}
						onBlur={() => keep(draft, false)}
					/>
				) : (
					<button className={styles.title_button} type="button" tabIndex={-1} data-renames={onRename != null || undefined}
						onClick={() => {
							const isDocked = layerRef.current?.hasAttribute("data-docked") == true;
							if (onRename != null && isDocked == false) edit();
							else onTop?.();
						}}>{title}</button>
				)}
			</h1>
			{band != null ? <div ref={bandRef} className={styles.part}>{band}</div> : null}
			{faces != null ? <div ref={facesRef} className={styles.part}>{faces}</div> : null}
			{lead != null ? <div ref={leadRef} className={styles.part}>{lead}</div> : null}
			{actions != null ? <div ref={actionsRef} className={`${styles.part} ${styles.actions}`}>{actions}</div> : null}
		</div>
	);
}
