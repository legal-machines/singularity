import { useEffect, useRef, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { Menu, useMenu } from "../../components/Menu/Menu.jsx";
import { MovingHead } from "../../components/MovingHead/MovingHead.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { ProjectBand } from "../../components/ProjectMark/ProjectMark.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { toast } from "../../components/Toast/toast.js";
import { useFirmComputers } from "../../data/coordination.js";
import { findMail } from "../../data/mail.js";
import { hasNative, realWork, shownChats, shownInbox, shownMail, shownMatters } from "../../data/native.js";
import { chats, decisions, members, mockNow, viewerId } from "../../data/workspace.js";
import { eventWords, filingOf, findThread, isThreadUnread, lastEvent, threadStatus } from "../threadState.js";
import { findMatter, findMember, firstName } from "../words.js";
import { inboxTime, inboxView } from "./inboxView.js";
import { useStableGutter } from "../../components/Page/stableGutter.js";

import styles from "./InboxPage.module.css";

const views = [{ id: "active", label: "Active" }, { id: "done", label: "Done" }, { id: "saved", label: "Saved" }];

// One personal queue across the firm (D-164). What calls for this member
// comes first; everyone else's work continues in its own place. The view is
// disposable UI state; the existing app handlers own read, saved and done.
export function InboxPage({ state, onDone, onSave, onFile, display, onDisplayChange }) {
	const { place, open } = useNavigation();
	const firm = useFirmComputers();
	const [localDisplay, setLocalDisplay] = useState({ view: "active", unreadOnly: false });
	const { view, unreadOnly } = display ?? localDisplay;
	const updateDisplay = onDisplayChange ?? setLocalDisplay;
	const [cursorId, setCursorId] = useState(place.threadId ?? null);
	const scrollRef = useRef(null);
	useStableGutter(scrollRef);
	const headRef = useRef(null);
	const allEntries = entriesOf(state, firm).map(entry => ({ ...entry, content: contentOf(entry, state) }));
	const groups = inboxView(allEntries, state, view, unreadOnly);
	const entries = groups.flatMap(group => group.entries);
	const position = entries.findIndex(entry => entry.id == cursorId);

	function openEntry(entry) {
		if (entry.kind == "mail") open({ kind: "mail", mailId: entry.id, from: "inbox" });
		else if (entry.kind == "dm") open({ kind: "messages", memberId: entry.memberId });
		else if (entry.kind == "computer") open(entry.matterId != null ? { kind: "matter", matterId: entry.matterId, tab: "overview" } : { kind: "matters" });
		else open({ kind: "thread", threadId: entry.id, from: "inbox" });
	}

	function changeView(next) {
		updateDisplay(previous => ({ ...previous, view: next }));
		setCursorId(null);
		scrollRef.current?.scrollTo({ top: 0 });
	}

	function step(direction) {
		const next = entries[position < 0 ? (direction > 0 ? 0 : entries.length - 1) : position + direction];
		if (next == null) return;
		setCursorId(next.id);
		const row = [...scrollRef.current.querySelectorAll("[data-entry]")].find(element => element.dataset.entry == next.id);
		row?.scrollIntoView({ block: "nearest" });
	}

	function toggleSaved(id) {
		const wasSaved = state.savedIds.includes(id);
		onSave(id);
		toast(wasSaved ? `Removed “${titleOf(id)}” from Saved.` : `Saved “${titleOf(id)}”.`, { action: { label: "Undo", run: () => onSave(id) } });
	}

	function markDone(id) {
		const wasDone = state.doneIds.includes(id);
		onDone(id, !wasDone);
		toast(wasDone ? `“${titleOf(id)}” is back in your inbox.` : `Marked “${titleOf(id)}” done for you.`, { action: { label: "Undo", run: () => onDone(id, wasDone) } });
		if (id == cursorId) {
			const rest = entries.filter(entry => entry.id != id);
			setCursorId(rest[Math.min(Math.max(position, 0), rest.length - 1)]?.id ?? null);
		}
	}

	function fileTo(id, matterId) {
		const before = filingOf(findDirect(id).message, state);
		onFile(id, matterId);
		toast(`Filed “${titleOf(id)}” to ${findMatter(matterId).name}.`, { action: { label: "Undo", run: () => onFile(id, before) } });
	}

	useEffect(() => {
		function keys(event) {
			if (event.ctrlKey || event.metaKey || event.altKey) return;
			if (event.target.closest?.("input, textarea, select, [contenteditable], [role='menu'], [role='dialog'], [role='tablist']")) return;
			const key = event.key.toLowerCase();
			const current = entries[position];
			if (key == "arrowdown" || key == "j") step(1);
			else if (key == "arrowup" || key == "k") step(-1);
			else if (key == "enter" && current != null && event.target.closest?.("button") == null) openEntry(current);
			else if (key == "e" && current != null) markDone(current.id);
			else if (key == "s" && current != null) toggleSaved(current.id);
			else return;
			event.preventDefault();
		}
		window.addEventListener("keydown", keys);
		return () => window.removeEventListener("keydown", keys);
	});

	return (
		<div className={styles.sheet}>
			<SheetBar />
			<div ref={scrollRef} className={styles.feed_scroll} data-fade-edge="">
				<div className={styles.feed}>
					<header ref={headRef} className={styles.head}>
						<p className={`${styles.title} ${styles.title_room}`} data-part="title" aria-hidden="true">Inbox</p>
						<p className={styles.summary}>{today()}</p>
					</header>
					<div className={styles.toolbar}>
						<div className={styles.tabs} role="tablist" aria-label="Inbox views">
							{views.map((item, index) => (
								<button key={item.id} id={`inbox-tab-${item.id}`} type="button" role="tab" aria-selected={view == item.id} aria-controls="inbox-items" tabIndex={view == item.id ? 0 : -1} onClick={() => changeView(item.id)} onKeyDown={event => {
									const next = event.key == "ArrowRight" ? (index + 1) % views.length : event.key == "ArrowLeft" ? (index + views.length - 1) % views.length : event.key == "Home" ? 0 : event.key == "End" ? views.length - 1 : null;
									if (next == null) return;
									event.preventDefault();
									changeView(views[next].id);
									event.currentTarget.parentElement.children[next].focus();
								}}>{item.label}</button>
							))}
						</div>
						<button className={styles.unread_filter} type="button" aria-pressed={unreadOnly} onClick={() => { updateDisplay(previous => ({ ...previous, unreadOnly: !previous.unreadOnly })); setCursorId(null); }}>
							<Icon name="filter" size={16} />Unread
						</button>
					</div>
					<div id="inbox-items" role="tabpanel" aria-labelledby={`inbox-tab-${view}`}>
						{entries.length == 0 ? <Empty view={view} unreadOnly={unreadOnly} /> : null}
						{groups.map(group => (
							<section key={group.id} className={styles.group} aria-labelledby={`inbox-section-${group.id}`}>
								<h2 id={`inbox-section-${group.id}`} className={styles.divider}>{group.title}</h2>
								<ul className={styles.lines}>
									{group.entries.map(entry => <Line key={entry.id} entry={entry} isSelected={entry.id == cursorId} isDone={state.doneIds.includes(entry.id)} isSaved={state.savedIds.includes(entry.id)} onOpen={() => openEntry(entry)} onDone={() => markDone(entry.id)} onSave={() => toggleSaved(entry.id)} onFile={matterId => fileTo(entry.id, matterId)} onOpenMatter={entry.matterId != null ? () => open({ kind: "matter", matterId: entry.matterId, tab: "overview" }) : null} />)}
								</ul>
							</section>
						))}
					</div>
				</div>
			</div>
			<MovingHead scrollRef={scrollRef} placeholderRef={headRef} title="Inbox" onTop={() => scrollRef.current?.scrollTo({ top: 0, behavior: "smooth" })} />
		</div>
	);
}

// The whole row opens the work; its project and personal actions have their
// own buttons. There are no nested controls or changes to the shared work.
function Line({ entry, isSelected, isDone, isSaved, onOpen, onDone, onSave, onFile, onOpenMatter }) {
	const { content } = entry;
	const time = inboxTime(content.at, hasNative() ? new Date() : new Date(mockNow));
	const cardRef = useRef(null);
	const menu = useMenu();
	const status = content.status;
	const givenWords = entry.isAsk ? (status?.words ?? (entry.kind == "computer" ? "Requests access" : null)) : status != null && status.kind != "waiting" ? status.words : null;
	// A thread's bare "Asks you" says no more than the section it stands in
	// ("Needs you"), so its row says nothing there (the owner, 2026-11-02:
	// "remove this Asks you from the threads in the Inbox"); a sharper word -
	// "Answer today", "Your review" - stays.
	const statusWords = entry.kind == "thread" && givenWords == "Asks you" ? null : givenWords;
	const shortStatus = /Ready for review$/.test(statusWords ?? "") ? "Your review"
		: { "Waits for your answer today": "Answer today", "Waits for your answer": "Your answer", "Waits for your acceptance": "Your review" }[statusWords] ?? statusWords;
	const author = content.face != null && entry.kind == "thread" ? firstName(content.face) : null;
	function onContextMenu(event) {
		event.preventDefault();
		const { clientX: x, clientY: y } = event;
		menu.open({ getBoundingClientRect: () => ({ top: y, bottom: y, left: x, right: x, width: 0, height: 0 }), contains: () => false, focus: () => cardRef.current?.focus() });
	}
	const menuItems = [
		{ key: "open", label: "Open", icon: "open", onSelect: onOpen },
		entry.kind == "dm" ? { key: "file", label: "File to project", icon: "matter", submenu: shownMatters().map(matter => ({ key: matter.id, label: matter.name, checked: entry.matterId == matter.id, onSelect: () => onFile(matter.id) })) } : null,
		{ key: "done", label: isDone ? "Move to Active" : "Mark done", icon: isDone ? "return" : "check", onSelect: onDone },
		{ key: "save", label: isSaved ? "Remove from Saved" : "Save", icon: isSaved ? "saved_filled" : "saved", onSelect: onSave },
		onOpenMatter != null ? { separator: true } : null,
		onOpenMatter != null ? { key: "matter", label: `Open ${findMatter(entry.matterId).name}`, icon: "matter", onSelect: onOpenMatter } : null,
	];
	return (
		<li className={[styles.item, isSelected ? styles.item_selected : null, content.isUnread ? styles.item_unread : null].filter(Boolean).join(" ")} data-entry={entry.id}>
			<button ref={cardRef} className={styles.line} type="button" aria-current={isSelected ? "true" : undefined} onClick={onOpen} onContextMenu={onContextMenu}>
				<span className={styles.face}>
					{content.face != null ? <Avatar member={content.face} size={40} /> : <span className={styles.sign_face}><Icon name={content.sign} size={20} /></span>}
					{entry.kind == "mail" ? <span className={styles.mail_sign} role="img" aria-label="Email">@</span> : null}
				</span>
				<span className={styles.line_text}>
					<span className={styles.line_head}>
						<span className={styles.line_title}>{content.title}</span>
						<time className={styles.line_time} dateTime={content.at}>{time}</time>
					</span>
					<span className={styles.line_said}>
						{author != null ? <span className={styles.line_author}>{author}: </span> : null}
						{content.subject != null ? <>{content.subject} — </> : null}{content.said}
					</span>
				</span>
			</button>
			<div className={styles.context}>
				{onOpenMatter != null ? <ProjectBand matter={findMatter(entry.matterId)} onClick={onOpenMatter} /> : <span className={styles.unfiled}>{entry.kind == "dm" ? "Direct message" : "Firm"}</span>}
				{statusWords != null ? <span className={styles.status} data-kind={entry.isAsk ? "yours" : status?.kind}>{shortStatus}</span> : null}
			</div>
			<div className={styles.actions}>
				<button className={isSaved ? styles.action_saved : undefined} type="button" aria-label={`${isSaved ? "Unsave" : "Save"}: ${content.title}`} aria-pressed={isSaved} title={isSaved ? "Remove from Saved (S)" : "Save (S)"} onClick={onSave}><Icon name={isSaved ? "saved_filled" : "saved"} size={17} /></button>
				<button type="button" aria-label={`${isDone ? "Move to Active" : "Mark done"}: ${content.title}`} title={isDone ? "Move to Active (E)" : "Mark done for you (E)"} onClick={onDone}><Icon name={isDone ? "return" : "closed"} size={19} /></button>
				<button type="button" aria-label={`More: ${content.title}`} aria-haspopup="menu" aria-expanded={menu.isOpen} title="More" onClick={event => menu.open(event.currentTarget)}><Icon name="more" size={18} /></button>
			</div>
			{menu.isOpen ? <Menu anchor={menu.anchor} label={content.title} minWidth={220} onClose={menu.close} items={menuItems} /> : null}
		</li>
	);
}

function Empty({ view, unreadOnly }) {
	const words = unreadOnly ? ["You’re up to date", "No unread items in this view."] : view == "done" ? ["Nothing marked done", "Items you mark done will appear here."] : view == "saved" ? ["Keep something for later", "Save a conversation to find it here."] : ["Nothing waits for you", "New messages and results will appear here."];
	return <div className={styles.empty} role="status"><span className={styles.empty_sign}><Icon name={view == "saved" ? "saved" : view == "done" ? "closed" : "inbox"} size={30} /></span><p className={styles.empty_title}>{words[0]}</p><p className={styles.empty_text}>{words[1]}</p></div>;
}

// What a line shows of a thread, a message from Outlook or a direct message.
function contentOf(entry, state) {
	if (entry.kind == "computer") {
		const { computer } = entry;
		const isNamed = computer.isYou == false && computer.name != "";
		const first = computer.name.split(/\s+/)[0] ?? "";
		// Its member by the seat it signed in with, never by a name that matches.
		const known = isNamed && (computer.actorId ?? "") != ""
			? members.find(member => member.kind != "ai_colleague" && member.actorId == computer.actorId) ?? null : null;
		// Whose it is, as far as the firm's records say: the member's face; a
		// person no face is kept for, their initials; a computer no record names,
		// the sign of the key it asks for.
		const initials = computer.name.split(/\s+/).filter(Boolean).slice(0, 2).map(word => word[0].toUpperCase()).join("");
		const face = computer.isYou ? findMember(viewerId)
			: known != null ? known
			: isNamed ? { id: `computer_${computer.deviceId}`, kind: "person", name: computer.name, initials, color: "#676c7e" }
			: null;
		return {
			title: computer.isYou ? "Your other computer" : isNamed ? computer.name : "A computer",
			said: computer.isYou ? `Asks to join ${computer.matterName}. Compare its code, then let it in.`
				: isNamed ? `Asks to join ${computer.matterName} from their computer. Compare its code with ${first}, then let it in.`
				: `Asks to join ${computer.matterName}. Compare its code with your colleague, then let it in.`,
			at: entry.at,
			face,
			sign: "key",
			isUnread: true,
		};
	}
	if (entry.kind == "mail") {
		const message = findMail(entry.id);
		return {
			title: message.contact.name,
			subject: message.subject,
			said: message.paragraphs[0] ?? "",
			at: message.at,
			face: message.contact,
			isUnread: message.isUnread && state.readIds.includes(message.id) == false,
		};
	}
	if (entry.kind == "dm") {
		const { chat, message } = findDirect(entry.id);
		const author = findMember(message.authorId);
		return {
			title: author.name,
			said: message.text,
			at: message.at,
			face: author,
			isUnread: chat.isUnread && state.readChatIds.includes(chat.memberId) == false,
		};
	}
	const thread = findThread(entry.id);
	const event = lastEvent(thread, state);
	const decision = thread.decisionId != null && state.choices[thread.decisionId] == null ? decisions[thread.decisionId] : null;
	const isAsk = threadStatus(thread, state).kind == "yours";
	const asker = decision != null ? findMember(decision.colleagueId) : entry.ask != null && isAsk && thread.posts.length > 0 ? findMember(thread.posts[0].authorId) : null;
	return {
		title: thread.title,
		said: decision != null
			? decision.question
			: entry.ask != null && isAsk
				? entry.ask
				: eventWords(event),
		at: decision != null ? thread.posts.find(post => post.decisionId == thread.decisionId)?.at ?? event.at : event.at,
		face: asker ?? findMember(event.byId ?? event.authorId),
		isUnread: isThreadUnread(entry.id, state),
		status: threadStatus(thread, state),
	};
}

// A direct message that named no project, with its chat.
function findDirect(messageId) {
	for (const chat of chats) {
		const message = chat.messages.find(item => item.id == messageId);
		if (message != null) return { chat, message };
	}
	return null;
}

function titleOf(id) {
	if (id.startsWith("computer:")) return "The computer's request";
	const direct = findDirect(id);
	if (direct != null) return `${firstName(findMember(direct.message.authorId))}'s message`;
	return findThread(id)?.title ?? findMail(id)?.subject ?? "";
}

// The feed's entries: the inbox's threads, the mail filed to projects and the
// direct messages that named no project (filed since, or not), each with its
// project, its moment and whether it asks something of the member.
function entriesOf(state, firm) {
	const threadEntries = shownInbox().map(entry => {
		const thread = findThread(entry.threadId);
		return { ...entry, id: entry.threadId, kind: "thread", matterId: thread.matterId, at: lastEvent(thread, state).at, isAsk: threadStatus(thread, state).kind == "yours" };
	});
	const mailEntries = shownMail()
		.map(message => ({ id: message.id, kind: "mail", matterId: message.matterId, at: message.at, isAsk: false }));
	const directEntries = shownChats(state)
		.flatMap(chat => chat.messages.filter(message => message.isUnsorted == true && message.authorId != viewerId).map(message => ({ chat, message })))
		.map(({ chat, message }) => ({ id: message.id, kind: "dm", matterId: filingOf(message, state), at: message.at, isAsk: true, memberId: chat.memberId }));
	// A computer that asks to join a Matter open here: a decision the member
	// holds (D-123), decided on the Matter's page.
	// One for the Matter the window shows stands with that Matter; one for any
	// other Matter carried here stands with the unsorted, and is decided on the
	// Matters page.
	const shownMatterId = realWork().matterId ?? null;
	const uiMatterId = realWork().uiMatterId ?? null;
	const computerEntries = (firm?.asking ?? [])
		.map(computer => ({ id: `computer:${computer.deviceId}`, kind: "computer", matterId: computer.matterId == shownMatterId ? uiMatterId : null,
			at: computer.at || new Date().toISOString(), isAsk: true, computer }));
	return [...computerEntries, ...threadEntries, ...mailEntries, ...directEntries];
}

// The member's day, by the computer's own clock in the real app and the
// prototype's moment in its scene. No greeting by the time of day (DESIGN.md,
// UI-17: the window said "Good morning" at three in the morning, 2026-10-29).
function today() {
	const now = hasNative() == true ? new Date() : new Date(mockNow);
	return new Intl.DateTimeFormat("en-US", { weekday: "long", month: "long", day: "numeric" }).format(now);
}
