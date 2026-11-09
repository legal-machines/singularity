import { useEffect, useMemo, useRef, useState } from "react";

import { report } from "./components/Avatar/life.js";
import { IconButton } from "./components/Button/Button.jsx";
import { NavigationContext, addressOf, placeOf } from "./components/Navigation/navigation.js";
import { PageGuard } from "./components/PageGuard/PageGuard.jsx";
import { PersonCardLayer } from "./components/PersonCard/PersonCard.jsx";
import { Sidebar } from "./components/Sidebar/Sidebar.jsx";
import { Toaster } from "./components/Toast/Toaster.jsx";
import { FirmDialogs } from "./components/FirmComputers/FirmComputers.jsx";
import { TooltipLayer } from "./components/Tooltip/TooltipLayer.jsx";
import { AppBehaviour } from "./components/Window/AppBehaviour.jsx";
import { useNativeTitlebarControls } from "./components/Window/titlebar.js";
import { WindowControls } from "./components/WindowControls/WindowControls.jsx";
import { CalendarPage } from "./pages/Calendar/CalendarPage.jsx";
import { DocumentPage } from "./pages/Document/DocumentPage.jsx";
import { RealDocumentPage } from "./pages/Document/RealDocumentPage.jsx";
import { LetterPage } from "./pages/Mail/LetterPage.jsx";
import { HirePage } from "./pages/Hire/HirePage.jsx";
import { InboxPage } from "./pages/Inbox/InboxPage.jsx";
import { InvoicePage } from "./pages/Invoice/InvoicePage.jsx";
import { MatterPage } from "./pages/Matter/MatterPage.jsx";
import { MailPage } from "./pages/Mail/MailPage.jsx";
import { MattersPage } from "./pages/Matters/MattersPage.jsx";
import { MessagesPage } from "./pages/Messages/MessagesPage.jsx";
import { DocumentLensHost } from "./components/DocumentLens/DocumentLens.jsx";
import { ProfilePage } from "./pages/Profile/ProfilePage.jsx";
import { SearchPage } from "./pages/Search/SearchPage.jsx";
import { SettingsPage } from "./pages/Settings/SettingsPage.jsx";
import { TwoStepSetupHost } from "./pages/Settings/SignInSecurity.jsx";
import { TeamPage } from "./pages/Team/TeamPage.jsx";
import { ThreadPage } from "./pages/Thread/ThreadPage.jsx";
import { watchFirmComputers } from "./data/coordination.js";
import { askToChangeApp, recoverySignInLine, takeRecoverySignIn, watchTwoStepSetup } from "./data/twoStep.js";
import { tellUpdated, watchUpdates } from "./data/updates.js";
import { connectRealWork, hasNative, hireColleague, markChatRead, realColleaguePresences, shownMembers, shownThreads, useRealWork } from "./data/native.js";
import { conversationsFor } from "./pages/Messages/conversations.js";
import { toast } from "./components/Toast/toast.js";
import { OfficePairingQuestion } from "./components/OfficeConnection/OfficeConnection.jsx";
import { instruments } from "./data/instruments.js";
import { colleagueFacts } from "./data/profiles.js";
import { decisions, matters, members, viewerId } from "./data/workspace.js";
import { findThread, initialState, isThreadClosed, now } from "./pages/threadState.js";

import styles from "./App.module.css";

function isSamePlace(first, second) {
	return JSON.stringify(first) == JSON.stringify(second);
}

// The window: the desk with its places, and the sheet of the place open.
// Everything runs on mock data. The window's address names the place open
// ("#inbox/t_sablewood", "#matter/x_sablewood",
// "#document/doc_sablewood_markup", "#team", ...), so a reload opens it again.
// In onboarding the app runs as its own emulation: `emulation` drives it
// through the same functions a member's hand does. It shows the one place a
// step is about, so the sidebar and the window's own pieces - its buttons,
// Office's question, the keys it holds - stay out. Whether the member hid the
// sidebar, kept on this computer as a convenience: storage that cannot be read
// leaves it shown.
const SIDEBAR_KEY = "singularity.sidebar";
function readSidebarHidden() {
	try {
		return window.localStorage.getItem(SIDEBAR_KEY) == "hidden";
	} catch {
		return false;
	}
}
function keepSidebarHidden(hidden) {
	try {
		window.localStorage.setItem(SIDEBAR_KEY, hidden ? "hidden" : "shown");
	} catch {
		// Shown again next time; nothing else depends on it.
	}
}

export function App({ emulation = null }) {
	const [history, setHistory] = useState(() => ({ entries: [placeOf(window.location.hash)], index: 0 }));
	const [state, setState] = useState(initialState);
	// A return from a conversation restores the member's Inbox view.
	const [inboxDisplay, setInboxDisplay] = useState({ view: "active", unreadOnly: false });
	const place = history.entries[history.index];
	// The sidebar hides and comes back with its own button, at the top of the
	// window beside the traffic lights.
	const [sidebarHidden, setSidebarHidden] = useState(() => emulation == null && readSidebarHidden());
	useEffect(() => {
		if (emulation == null) keepSidebarHidden(sidebarHidden);
	}, [emulation, sidebarHidden]);
	useNativeTitlebarControls(place, sidebarHidden);
	// The real Matter, where Singularity runs the page (data/native.js): read
	// once here, and again each time the backend says it changed; the window
	// draws again with it.
	const real = useRealWork();
	// When the member opened each conversation in this window - a direct
	// message or a thread - for Recent (components/Sidebar/recent.js).
	const [opened, setOpened] = useState({});
	useEffect(() => {
		const key = place.kind == "thread" || (place.kind == "inbox" && place.threadId != null) ? `#thread/${place.threadId}`
			: place.kind == "messages" && place.memberId != null ? `#messages/${place.memberId}` : null;
		if (key != null) setOpened(previous => ({ ...previous, [key]: new Date().toISOString() }));
	}, [place]);
	// The conversation open is read up to its latest message, and again as
	// more comes while it is open.
	useEffect(() => {
		if (hasNative() == false) return;
		const threadId = place.kind == "thread" || place.kind == "inbox" ? place.threadId : null;
		if (threadId != null) {
			const thread = shownThreads().find(item => item.id == threadId);
			const last = [...(thread?.posts ?? [])].reverse().find(post => (post.kind ?? "post") == "post" && post.isSending != true);
			if (thread?.real?.chatId != null && last?.at != null) markChatRead(thread.real.chatId, last.at);
		}
		if (place.kind == "messages" && place.memberId != null) {
			const conversation = conversationsFor(state).find(entry => entry.kind == "member" && entry.memberId == place.memberId);
			const last = conversation?.items.at(-1);
			if (conversation?.send?.chatId != null && last?.at != null) markChatRead(conversation.send.chatId, last.at);
		}
	}, [place, real, state]);
	useEffect(() => {
		connectRealWork();
		// The firm's computers: who asks to join a Matter here, and what this
		// computer asked for.
		watchFirmComputers();
		// Singularity's new versions, from Legal Machines' site.
		watchUpdates();
	}, []);
	// The first start of a newer version says so once, with what is new in it;
	// onboarding's preview never tells it.
	useEffect(() => {
		if (emulation == null) tellUpdated().catch(() => {});
	}, [emulation]);
	// A sign-in by a recovery code is told once Singularity opens, with the way
	// to a new authenticator app; onboarding's preview never tells it. A seat
	// with no authenticator app is led to set one up wherever Legal Machines
	// asks for it first.
	useEffect(() => (emulation == null ? watchTwoStepSetup() : undefined), [emulation]);
	useEffect(() => {
		if (emulation != null) return;
		const recovered = takeRecoverySignIn();
		if (recovered == null) return;
		toast(recoverySignInLine(recovered), {
			tone: "waiting",
			duration: 15000,
			action: { label: "Change app", run: () => { askToChangeApp(); openRef.current({ kind: "settings" }); } },
		});
	}, [emulation]);
	// Each real colleague's face acts out what it is doing now, on every
	// computer: at work, its work.
	useEffect(() => {
		for (const { memberId, presence } of realColleaguePresences()) report(memberId, presence);
	}, [real]);

	useEffect(() => {
		const address = addressOf(place);
		if (window.location.hash != address) window.history.replaceState(null, "", address);
	}, [place]);

	// A change of the window's address from outside (a link, a typed address)
	// opens the place it names.
	useEffect(() => {
		function onHash() {
			const next = placeOf(window.location.hash);
			openRef.current(next);
		}
		window.addEventListener("hashchange", onHash);
		return () => window.removeEventListener("hashchange", onHash);
	}, []);

	function openPlace(nextPlace) {
		setHistory(previous => {
			const current = previous.entries[previous.index];
			if (isSamePlace(current, nextPlace)) return previous;
			// Moving through the inbox's lines replaces the line, not the place.
			if (current.kind == "inbox" && nextPlace.kind == "inbox") {
				const entries = [...previous.entries];
				entries[previous.index] = nextPlace;
				return { ...previous, entries };
			}
			return { entries: [...previous.entries.slice(0, previous.index + 1), nextPlace], index: previous.index + 1 };
		});
		visit(nextPlace);
	}

	function step(direction) {
		const index = history.index + direction;
		if (index < 0 || index >= history.entries.length) return;
		setHistory(previous => ({ ...previous, index }));
		visit(history.entries[index]);
	}

	const openRef = useRef(openPlace);
	openRef.current = openPlace;
	const stepRef = useRef(step);
	stepRef.current = step;

	useEffect(() => {
		function handleKey(event) {
			if ((event.ctrlKey || event.metaKey) && !event.altKey && !event.shiftKey && event.code == "KeyK") {
				event.preventDefault();
				openRef.current({ kind: "search", query: "" });
				return;
			}
			// Settings, as every Mac application opens its own.
			if ((event.ctrlKey || event.metaKey) && !event.altKey && !event.shiftKey && event.code == "Comma") {
				event.preventDefault();
				openRef.current({ kind: "settings" });
				return;
			}
			if (event.altKey == false || event.ctrlKey || event.shiftKey || event.metaKey) return;
			// In a field Option-arrow moves the caret a word: back and
			// forward never take it there.
			if (event.target instanceof Element && event.target.closest("input, textarea, select, [contenteditable='true'], [contenteditable='']") != null) return;
			if (event.key == "ArrowLeft") {
				event.preventDefault();
				stepRef.current(-1);
			}
			if (event.key == "ArrowRight") {
				event.preventDefault();
				stepRef.current(1);
			}
		}
		function handleMouse(event) {
			if (event.button != 3 && event.button != 4) return;
			event.preventDefault();
			stepRef.current(event.button == 3 ? -1 : 1);
		}
		window.addEventListener("keydown", handleKey);
		window.addEventListener("mouseup", handleMouse);
		return () => {
			window.removeEventListener("keydown", handleKey);
			window.removeEventListener("mouseup", handleMouse);
		};
	}, []);

	// Arriving at a thread or a chat reads it.
	function visit(nextPlace) {
		const threadId = nextPlace.kind == "thread" || nextPlace.kind == "inbox" ? nextPlace.threadId : nextPlace.kind == "mail" ? nextPlace.mailId : null;
		if (threadId != null) {
			setState(previous => ({
				...previous,
				readIds: previous.readIds.includes(threadId) ? previous.readIds : [...previous.readIds, threadId],
				unreadIds: previous.unreadIds.filter(id => id != threadId),
			}));
		}
		if (nextPlace.kind == "messages" && nextPlace.memberId != null) {
			setState(previous => ({
				...previous,
				readChatIds: previous.readChatIds.includes(nextPlace.memberId) ? previous.readChatIds : [...previous.readChatIds, nextPlace.memberId],
			}));
		}
	}

	// A hired AI colleague joins the firm. In the app it is the firm's own,
	// never one Matter's: kept by the backend with the office and the face it
	// was hired with, named in any Matter's thread to take work there; in a
	// plain browser it joins the prototype for this session.
	async function hire(colleague, facts, matterIds) {
		if (hasNative()) {
			// Hired over no one's shoulder: it answers to no member, unless one is
			// named.
			const answersToId = shownMembers().some(member => member.id == facts.answersTo) ? facts.answersTo : null;
			const answersTo = answersToId != null ? members.find(member => member.id == answersToId)?.name ?? "" : "";
			const memberId = await hireColleague({
				name: colleague.name,
				species: colleague.species,
				title: colleague.title ?? "",
				ranksWith: colleague.ranksWith,
				practice: colleague.practice ?? "",
				area: colleague.area,
				answersTo,
				goal: colleague.goal ?? "",
				manner: colleague.manner ?? "",
			});
			colleagueFacts[memberId] = { ...facts, answersTo: answersToId ?? undefined };
			// What the hiring said of it, for its profile in this session.
			const member = members.find(item => item.id == memberId);
			if (member != null) Object.assign(member, { remit: colleague.remit, duties: colleague.duties, model: colleague.model, record: colleague.record });
			toast(`${colleague.name} joined the firm. Message them, or name them in any Matter's thread.`);
			setState(previous => ({ ...previous, hires: previous.hires + 1 }));
			openPlace({ kind: "profile", memberId });
			return;
		}
		members.push(colleague);
		colleagueFacts[colleague.id] = facts;
		for (const matter of matters) {
			if (matterIds.includes(matter.id)) matter.memberIds.push(colleague.id);
		}
		setState(previous => ({ ...previous, hires: previous.hires + 1 }));
		openPlace({ kind: "profile", memberId: colleague.id });
	}

	function setDone(threadId, isDone) {
		setState(previous => ({
			...previous,
			doneIds: isDone ? [...previous.doneIds.filter(id => id != threadId), threadId] : previous.doneIds.filter(id => id != threadId),
		}));
	}

	function toggleSaved(threadId) {
		setState(previous => ({
			...previous,
			savedIds: previous.savedIds.includes(threadId) ? previous.savedIds.filter(id => id != threadId) : [...previous.savedIds, threadId],
		}));
	}

	function addEvents(previous, threadId, events) {
		return { ...previous.extraEvents, [threadId]: [...(previous.extraEvents[threadId] ?? []), ...events] };
	}

	// The member's own post in a thread, as its reply box writes one.
	function postOf(paragraphs, quote, extras = {}) {
		return { id: `reply_${Date.now()}`, kind: "post", authorId: viewerId, at: now(), paragraphs, quote: quote ?? null, files: extras.files ?? null, notChecked: extras.notChecked ?? null, ask: extras.ask ?? null };
	}

	// An answer to what a colleague asks is taken once, by the one it asks,
	// and stands in the thread as that member's reply, quoting the question.
	// The thread stays open until someone closes it.
	function decide(threadId, decisionId, key, quote) {
		const option = decisions[decisionId].options.find(item => item.key == key);
		setState(previous => ({
			...previous,
			choices: { ...previous.choices, [decisionId]: key },
			decidedAt: { ...(previous.decidedAt ?? {}), [decisionId]: now() },
			extraEvents: addEvents(previous, threadId, [postOf([option.text], quote)]),
		}));
	}

	// Reopening a closed thread, as its own act.
	function reopenThread(threadId) {
		const event = { id: `reopen_${Date.now()}`, kind: "reopened", at: now(), byId: viewerId };
		setState(previous => ({ ...previous, extraEvents: addEvents(previous, threadId, [event]) }));
	}

	// Closing a thread is an act of its own, with its conclusion.
	function closeThread(threadId, conclusion) {
		const event = { id: `close_${Date.now()}`, kind: "closed", at: now(), byId: viewerId, conclusion };
		setState(previous => ({ ...previous, extraEvents: addEvents(previous, threadId, [event]) }));
	}

	// A reply to a closed thread reopens it; the reply is its reason. A
	// reply to a thread, with what it hands in: its files, what was not
	// checked and the answers to the question it asks, as an AI colleague's
	// post carries them.
	function reply(threadId, text, quote, extras = {}) {
		const thread = findThread(threadId);
		const post = postOf(text.split(/\n\s*\n/).map(paragraph => paragraph.trim()).filter(Boolean), quote, extras);
		const events = isThreadClosed(thread, state) ? [{ id: `reopen_${Date.now()}`, kind: "reopened", at: now(), byId: viewerId }, post] : [post];
		setState(previous => ({ ...previous, extraEvents: addEvents(previous, threadId, events) }));
	}

	// A reaction on a post: the viewer's is put or taken back.
	function react(postId, emoji) {
		setState(previous => {
			const onPost = previous.reactions[postId] ?? {};
			const who = onPost[emoji] ?? [];
			const next = who.includes(viewerId) ? who.filter(id => id != viewerId) : [...who, viewerId];
			const byEmoji = { ...onPost, [emoji]: next };
			if (next.length == 0) delete byEmoji[emoji];
			return { ...previous, reactions: { ...previous.reactions, [postId]: byEmoji } };
		});
	}

	function editPost(postId, text) {
		const paragraphs = text.split(/\n\s*\n/).map(paragraph => paragraph.trim()).filter(Boolean);
		setState(previous => ({ ...previous, editedPosts: { ...previous.editedPosts, [postId]: paragraphs } }));
	}

	function deletePost(postId, isDeleted = true) {
		setState(previous => ({
			...previous,
			deletedPostIds: isDeleted ? [...previous.deletedPostIds, postId] : previous.deletedPostIds.filter(id => id != postId),
		}));
	}

	// A thread the viewer marks unread again: it reads unread until opened anew.
	function markUnread(threadId) {
		setState(previous => ({
			...previous,
			readIds: previous.readIds.filter(id => id != threadId),
			unreadIds: previous.unreadIds.includes(threadId) ? previous.unreadIds : [...previous.unreadIds, threadId],
		}));
	}

	// The viewer's answer to a question a member asked with a message.
	// An answer pressed under a question, whoever asked it: the member's reply
	// with the answer's words, in the thread quoting the question, or in the
	// chat as the member's next message.
	function answer(id, option, where = {}) {
		setState(previous => {
			const next = { ...previous, answers: { ...previous.answers, [id]: option } };
			if (where.threadId != null) next.extraEvents = addEvents(previous, where.threadId, [postOf([option], where.quote)]);
			if (where.memberId != null) {
				const message = { id: `message_${Date.now()}`, authorId: viewerId, at: now(), matterId: where.matterId ?? null, text: option, files: null, notChecked: null, ask: null };
				next.extraMessages = { ...previous.extraMessages, [where.memberId]: [...(previous.extraMessages[where.memberId] ?? []), message] };
			}
			return next;
		});
	}

	function sendMessage(memberId, text, matterId, extras = {}) {
		const message = { id: `message_${Date.now()}`, authorId: viewerId, at: now(), matterId, text, files: extras.files ?? null, notChecked: extras.notChecked ?? null, ask: extras.ask ?? null };
		setState(previous => ({
			...previous,
			extraMessages: { ...previous.extraMessages, [memberId]: [...(previous.extraMessages[memberId] ?? []), message] },
		}));
	}

	const navigation = useMemo(() => ({
		place,
		open: openPlace,
		back: () => step(-1),
		forward: () => step(1),
		canBack: history.index > 0,
		canForward: history.index < history.entries.length - 1,
	}), [place, history]);

	// A direct message the member files to a project from the Inbox.
	function file(messageId, matterId) {
		setState(previous => ({ ...previous, filed: { ...previous.filed, [messageId]: matterId } }));
	}

	const handlers = { onDone: setDone, onSave: toggleSaved, onDecide: decide, onReply: reply, onFile: file, onReact: react, onEditPost: editPost, onDeletePost: deletePost, onMarkUnread: markUnread, onAnswer: answer, onCloseThread: closeThread, onReopenThread: reopenThread };

	// The app's own functions, as they are now, for its emulation.
	const own = useRef(null);
	own.current = { open: openPlace, setState, reply, sendMessage, answer, decide };
	useEffect(() => emulation?.attach(() => own.current), [emulation]);

	return (
		<NavigationContext.Provider value={navigation}>
			<PersonCardLayer state={state}>
				<div className={styles.window} data-print="flow" data-bare={emulation != null || undefined} data-sidebar={sidebarHidden ? "hidden" : undefined}>
					{emulation == null ? (
						<Sidebar
							hidden={sidebarHidden}
							state={state}
							opened={opened}
						/>
					) : null}
					<main className={styles.sheet} data-print="flow" data-real={real.status}>
						<PageGuard key={addressOf(place)} onHome={() => openPlace({ kind: "inbox" })}>
						{place.kind == "inbox" ? <InboxPage state={state} {...handlers} display={inboxDisplay} onDisplayChange={setInboxDisplay} /> : null}
						{place.kind == "thread" ? <ThreadPage key={place.threadId} state={state} {...handlers} /> : null}
						{place.kind == "mail" ? <MailPage key={place.mailId} /> : null}
						{place.kind == "letter" ? <LetterPage key={place.letterId} /> : null}
						{place.kind == "messages" ? <MessagesPage key={place.memberId} place={place} state={state} onOpen={openPlace} onSend={sendMessage} onAnswer={answer} /> : null}
						{place.kind == "matter" ? <MatterPage key={place.matterId} state={state} {...handlers} /> : null}
						{place.kind == "matters" ? <MattersPage state={state} /> : null}
						{place.kind == "document" && instruments[place.documentId] != null ? <DocumentPage key={place.documentId} state={state} /> : null}
						{place.kind == "document" && instruments[place.documentId] == null ? <RealDocumentPage key={place.documentId} documentId={place.documentId} at={place.at ?? null} /> : null}
						{place.kind == "search" ? <SearchPage query={place.query ?? ""} state={state} onOpen={openPlace} /> : null}
						{place.kind == "team" ? <TeamPage state={state} /> : null}
						{place.kind == "profile" ? <ProfilePage key={place.memberId} memberId={place.memberId} state={state} onOpen={openPlace} /> : null}
						{place.kind == "hire" ? <HirePage onHire={hire} onCancel={() => openPlace({ kind: "team" })} /> : null}
						{place.kind == "settings" ? <SettingsPage tab={place.tab ?? "general"} /> : null}
						{place.kind == "calendar" ? <CalendarPage place={place} onOpen={openPlace} /> : null}
						{place.kind == "invoice" ? <InvoicePage key={place.matterId} matterId={place.matterId} onOpen={openPlace} /> : null}
						</PageGuard>
					</main>
					{/* Hiring fills the window (pages/Hire), the sidebar's toggle with it. */}
					{emulation == null && place.kind != "hire" ? (
						<IconButton
							className={styles.sidebar_toggle}
							icon="sidebar"
							label={sidebarHidden ? "Show sidebar" : "Hide sidebar"}
							data-print="hide"
							onClick={() => setSidebarHidden(hidden => !hidden)}
						/>
					) : null}
					{emulation == null ? (
						<div className={styles.controls} data-print="hide">
							<WindowControls />
						</div>
					) : null}
				</div>
			</PersonCardLayer>
			<DocumentLensHost />
			<TooltipLayer />
			<Toaster />
			{/* A check of one of the firm's computers, or a Matter shared with this member. */}
			<FirmDialogs />
			{emulation == null ? <OfficePairingQuestion /> : null}
			{emulation == null ? <TwoStepSetupHost /> : null}
			{emulation == null ? <AppBehaviour /> : null}
		</NavigationContext.Provider>
	);
}
