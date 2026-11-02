import { useEffect, useRef, useState } from "react";

import { Icon } from "../Icon/Icon.jsx";
import { ArchiveMatterDialog } from "../Manage/ArchiveMatterDialog.jsx";
import { Menu, useMenu } from "../Menu/Menu.jsx";
import { useNavigation } from "../Navigation/navigation.js";
import { NewMatterDialog } from "../NewMatter/NewMatterDialog.jsx";
import { MarkPicker, ProjectMark } from "../ProjectMark/ProjectMark.jsx";
import { hasNative, realLatestOf, shownMatters, shownThreads, useRealWork } from "../../data/native.js";
import { viewerId } from "../../data/workspace.js";
import { eventWords, lastEvent } from "../../pages/threadState.js";
import { findMember, firstName, plainWords } from "../../pages/words.js";
import { Badges } from "./Badges.jsx";
import { describePlace } from "./places.js";

import styles from "./Matters.module.css";
import recent from "./Recent.module.css";
import sidebar from "./Sidebar.module.css";

// The firm's Matters, as Grok Bot lists its bots (the owner, 2026-11-02):
// each by its mark, which a member sets, its name, and what it said last -
// who wrote it and their words. They stand in the order the member gives them
// by dragging (the owner, 2026-11-02: "let the Matters' order be changed by
// dragging them by hand"), kept on this computer; until then, and for a
// Matter new since, by name, so the hand finds each where it was (D-047).
// Closed ones are on the Matters page. A new Matter begins from the place's
// row pinned under them (D-200).
export function Matters({ state, unread = new Map() }) {
	const { place, open } = useNavigation();
	// Read again as the backend says a Matter changed.
	useRealWork();
	const [isCreating, setCreating] = useState(false);
	const [order, setOrder] = useState(readOrder);
	const matters = inOrder(shownMatters().filter(matter => matter.isClosed != true), order);
	const here = place.kind == "matter" ? place.matterId : describePlace(place)?.matterId ?? null;
	const listRef = useRef(null);
	const drag = useReorder(listRef, matters.map(matter => matter.id), ids => {
		setOrder(ids);
		keepOrder(ids);
	});

	function move(matterId, step) {
		const ids = matters.map(matter => matter.id);
		const from = ids.indexOf(matterId);
		const to = Math.max(0, Math.min(ids.length - 1, from + step));
		if (from < 0 || from == to) return;
		ids.splice(to, 0, ...ids.splice(from, 1));
		setOrder(ids);
		keepOrder(ids);
	}

	return (
		<section className={styles.matters} aria-label="Matters">
			{/* Headed as Recent is (the owner, 2026-11-02). */}
			<div className={`${recent.heading} ${styles.heading}`}><h2>Matters</h2></div>
			<ul ref={listRef} className={drag.isDragging ? `${styles.list} ${styles.list_dragging}` : styles.list}>
				{matters.map((matter, index) => (
					<MatterRow key={matter.id} matter={matter} said={saidIn(matter, state)} isHere={here == matter.id} unread={unread.get(matter.id)}
						isFirst={index == 0} isLast={index == matters.length - 1}
						dragging={drag.styleOf(matter.id)} onPointerDown={event => drag.begin(event, matter.id)}
						onOpen={() => {
							if (drag.wasDragged()) return;
							open({ kind: "matter", matterId: matter.id, tab: "overview" });
						}}
						onMove={step => move(matter.id, step)} />
				))}
			</ul>
			{/* A place's row like Inbox's or Team's (the owner, 2026-11-02: "rebuild
			    this New matter so that it fits the sidebar's design"). */}
			<button className={`${sidebar.tab} ${styles.new}`} type="button" onClick={() => (hasNative() ? setCreating(true) : open({ kind: "matters" }))}>
				<span className={sidebar.tab_lead}><Icon name="plus" size={18} /></span>
				<span className={sidebar.tab_text}><span className={sidebar.tab_label}>New matter</span></span>
			</button>
			{isCreating ? (
				<NewMatterDialog onClose={() => setCreating(false)} onCreated={matterId => { setCreating(false); open({ kind: "matter", matterId, tab: "overview" }); }} />
			) : null}
		</section>
	);
}

// One Matter: its mark, its name and what it said last. Its menu opens it,
// changes its mark, or moves it up or down - what dragging does, by hand.
function MatterRow({ matter, said, isHere, unread, isFirst, isLast, dragging, onPointerDown, onOpen, onMove }) {
	const rowRef = useRef(null);
	const markRef = useRef(null);
	// The open Matter stays in view among many (the list scrolls in itself).
	useEffect(() => {
		if (isHere != true || typeof rowRef.current?.scrollIntoView != "function") return;
		const isCalm = typeof window.matchMedia == "function" && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
		rowRef.current.scrollIntoView({ block: "nearest", behavior: isCalm ? "auto" : "smooth" });
	}, [isHere]);
	const menu = useMenu();
	const [markAnchor, setMarkAnchor] = useState(null);
	const [isArchiving, setArchiving] = useState(false);

	function onContextMenu(event) {
		event.preventDefault();
		const { clientX: x, clientY: y } = event;
		menu.open({ getBoundingClientRect: () => ({ top: y, bottom: y, left: x, right: x, width: 0, height: 0 }), contains: () => false, focus: () => {} });
	}

	const classes = [styles.row, isHere ? styles.row_here : null, dragging?.isHeld ? styles.row_held : null].filter(Boolean).join(" ");
	return (
		<li className={dragging?.isHeld ? styles.item_held : undefined} style={dragging?.style}>
			<button
				ref={rowRef}
				className={classes}
				type="button"
				aria-current={isHere ? "page" : undefined}
				onClick={onOpen}
				onPointerDown={onPointerDown}
				onContextMenu={onContextMenu}
			>
				<span ref={markRef} className={styles.mark}><ProjectMark matter={matter} size={36} /></span>
				<span className={styles.text}>
					<span className={styles.name}>{matter.name}</span>
					<span className={said == null ? `${styles.said} ${styles.said_none}` : styles.said}>
						{said == null ? "No messages yet" : <>{said.author != null ? <span className={styles.author}>{said.author}: </span> : null}{said.words}</>}
					</span>
				</span>
				<Badges count={unread?.count ?? 0} namesViewer={unread?.namesViewer == true} label={matter.name} />
			</button>
			{menu.isOpen ? (
				<Menu anchor={menu.anchor} label={matter.name} minWidth={200} onClose={menu.close} items={[
					{ key: "open", label: "Open", icon: "open", onSelect: onOpen },
					{ key: "mark", label: "Change icon", icon: "edit", onSelect: () => setMarkAnchor(markRef.current) },
					isFirst ? null : { key: "up", label: "Move up", icon: "up", onSelect: () => onMove(-1) },
					isLast ? null : { key: "down", label: "Move down", icon: "down", onSelect: () => onMove(1) },
					// Archived apart from the open Matters, for everyone on it (D-214).
					matter.isReal == true ? { key: "archive", label: "Archive", icon: "closed", onSelect: () => setArchiving(true) } : null,
				].filter(Boolean)} />
			) : null}
			{markAnchor != null ? <MarkPicker matter={matter} anchor={markAnchor} onClose={() => setMarkAnchor(null)} /> : null}
			{isArchiving ? <ArchiveMatterDialog matter={matter} onClose={() => setArchiving(false)} /> : null}
		</li>
	);
}

// The order the member gave the Matters, by their ids, kept on this computer.
const ORDER_KEY = "singularity.matters.order";

function readOrder() {
	try {
		const kept = JSON.parse(window.localStorage.getItem(ORDER_KEY) ?? "[]");
		return Array.isArray(kept) ? kept.filter(id => typeof id == "string") : [];
	} catch {
		return [];
	}
}

function keepOrder(ids) {
	try {
		window.localStorage.setItem(ORDER_KEY, JSON.stringify(ids));
	} catch {
		// Not kept: the order holds in this window.
	}
}

// The Matters in the member's order; one the order does not name yet comes
// after them, by name.
function inOrder(matters, order) {
	const position = new Map(order.map((id, index) => [id, index]));
	const placed = matters.filter(matter => position.has(matter.id)).sort((first, second) => position.get(first.id) - position.get(second.id));
	const rest = matters.filter(matter => position.has(matter.id) == false).sort((first, second) => first.name.localeCompare(second.name));
	return [...placed, ...rest];
}

// Dragging a row to its place, as a hand moves a card (apple-design: the row
// stays under the pointer where it was taken, the others step aside as it
// passes their middle, and it settles into its place on letting go). It
// begins only past a few points, so a press still opens the Matter; near the
// list's edges the list scrolls under it.
const THRESHOLD = 4;

function useReorder(listRef, ids, onReorder) {
	const [state, setState] = useState(null);
	const live = useRef(null);
	const dragged = useRef(false);

	function measure() {
		const rows = [...(listRef.current?.children ?? [])];
		const tops = rows.map(row => row.getBoundingClientRect().top);
		const pitch = tops.length > 1 ? tops[1] - tops[0] : rows[0]?.getBoundingClientRect().height ?? 52;
		return pitch;
	}

	function targetOf(drag, dy) {
		return Math.max(0, Math.min(ids.length - 1, drag.from + Math.round(dy / drag.pitch)));
	}

	function begin(event, id) {
		if (event.button != 0 || event.pointerType == "touch") return;
		const list = listRef.current;
		const from = ids.indexOf(id);
		if (list == null || from < 0) return;
		const pointerId = event.pointerId;
		const target = event.currentTarget;
		const drag = { id, from, startY: event.clientY, startScroll: list.scrollTop, pitch: 0, isMoving: false, y: event.clientY, frame: 0 };
		live.current = drag;
		dragged.current = false;

		// The row goes no further than the first place and the last (the
		// owner, 2026-11-02: "it must simply not move once it reached the
		// limits").
		function offset() {
			const dy = drag.y - drag.startY + (list.scrollTop - drag.startScroll);
			return Math.max(-drag.from * drag.pitch, Math.min((ids.length - 1 - drag.from) * drag.pitch, dy));
		}

		function update() {
			const dy = offset();
			setState({ id: drag.id, from: drag.from, to: targetOf(drag, dy), dy, pitch: drag.pitch });
		}

		// Near the list's top or bottom the list scrolls, faster the closer.
		function autoscroll() {
			const box = list.getBoundingClientRect();
			const reach = 28;
			const speed = drag.y < box.top + reach ? -(box.top + reach - drag.y) / 3 : drag.y > box.bottom - reach ? (drag.y - (box.bottom - reach)) / 3 : 0;
			if (speed != 0) {
				list.scrollTop += Math.max(-12, Math.min(12, speed));
				update();
			}
			drag.frame = requestAnimationFrame(autoscroll);
		}

		function onMove(moveEvent) {
			if (moveEvent.pointerId != pointerId) return;
			drag.y = moveEvent.clientY;
			if (drag.isMoving == false) {
				if (Math.abs(drag.y - drag.startY) < THRESHOLD) return;
				drag.isMoving = true;
				drag.pitch = measure();
				dragged.current = true;
				try {
					target.setPointerCapture(pointerId);
				} catch {
					// Followed on the window instead.
				}
				drag.frame = requestAnimationFrame(autoscroll);
			}
			moveEvent.preventDefault();
			update();
		}

		function end(endEvent) {
			if (endEvent.pointerId != pointerId) return;
			window.removeEventListener("pointermove", onMove);
			window.removeEventListener("pointerup", end);
			window.removeEventListener("pointercancel", end);
			cancelAnimationFrame(drag.frame);
			live.current = null;
			if (drag.isMoving == false) return;
			const dy = offset();
			const to = endEvent.type == "pointercancel" ? drag.from : targetOf(drag, dy);
			const next = [...ids];
			next.splice(to, 0, ...next.splice(drag.from, 1));
			// It settles from where the hand left it into its place.
			const rest = dy - (to - drag.from) * drag.pitch;
			setState({ id: drag.id, settling: true, rest });
			if (to != drag.from) onReorder(next);
			requestAnimationFrame(() => requestAnimationFrame(() => setState(current => (current?.settling && current.id == drag.id ? { ...current, rest: 0 } : current))));
			setTimeout(() => setState(current => (current?.settling && current.id == drag.id ? null : current)), 260);
		}

		window.addEventListener("pointermove", onMove);
		window.addEventListener("pointerup", end);
		window.addEventListener("pointercancel", end);
	}

	function styleOf(id) {
		if (state == null) return null;
		if (state.settling) {
			if (state.id != id) return null;
			return { isHeld: state.rest != 0, style: { transform: `translateY(${state.rest}px)`, transition: state.rest == 0 ? "transform 220ms cubic-bezier(0.2, 0.8, 0.2, 1)" : "none", position: "relative", zIndex: 1 } };
		}
		const index = ids.indexOf(id);
		if (id == state.id) return { isHeld: true, style: { transform: `translateY(${state.dy}px)`, transition: "none", position: "relative", zIndex: 1 } };
		let shift = 0;
		if (state.from < state.to && index > state.from && index <= state.to) shift = -state.pitch;
		if (state.from > state.to && index < state.from && index >= state.to) shift = state.pitch;
		return { isHeld: false, style: { transform: shift != 0 ? `translateY(${shift}px)` : "none", transition: "transform 180ms cubic-bezier(0.2, 0.8, 0.2, 1)" } };
	}

	return {
		isDragging: state != null && state.settling != true,
		begin,
		styleOf,
		// A press that moved a Matter does not also open it.
		wasDragged() {
			if (dragged.current == false) return false;
			dragged.current = false;
			return true;
		},
	};
}

// What a Matter said last, as its second line reads (D-200): its author -
// "You" for the member here, a person or a colleague by first name, no one
// for a model's answer - and the words, or the files it handed over.
function saidIn(matter, state) {
	if (hasNative()) {
		const latest = realLatestOf(matter.id);
		if (latest == null) return null;
		const author = latest.isViewer ? "You" : latest.authorName != "" ? latest.authorName.split(/\s+/)[0] : null;
		return { author, words: wordsOf(plainWords(latest.text), latest.files) };
	}
	// The prototype's Matters: the latest of their threads.
	let found = null;
	for (const thread of shownThreads()) {
		if (thread.matterId != matter.id) continue;
		const event = lastEvent(thread, state);
		if (found == null || (event.at ?? "") > (found.at ?? "")) found = event;
	}
	if (found == null || found.kind == "begun") return null;
	const member = found.authorId != null && found.authorId != viewerId ? findMember(found.authorId) : null;
	const author = found.authorId == viewerId ? "You" : member != null ? firstName(member) : null;
	return { author, words: wordsOf(eventWords(found), (found.files ?? []).length) };
}

function wordsOf(words, files) {
	if ((words ?? "").trim() != "") return words.trim();
	if (files > 0) return files == 1 ? "Sent a file" : `Sent ${files} files`;
	return "";
}
