import { useEffect, useRef, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Reactions } from "../../components/Reactions/Reactions.jsx";
import { Button, IconButton } from "../../components/Button/Button.jsx";
import { ComposerAction, ComposerBox } from "../../components/Composer/ComposerBox.jsx";
import { useWindowFileDrop } from "../../components/FileDrop/windowDrop.js";
import { FileKindIcon } from "../../components/FileKind/FileKind.jsx";
import { FileTile, kindOf, wordOf } from "../../components/FileTile/FileTile.jsx";
import { DropOverlay } from "../../components/FileDrop/DropOverlay.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { MessageText } from "../../components/MessageText/MessageText.jsx";
import { MaskedDraft, MaskedText, ProviderViewButton } from "../../components/ProviderView/ProviderView.jsx";
import { useProviderView } from "../../components/ProviderView/providerView.js";
import { isProtectionOn } from "../../data/masking.js";
import { plainOf } from "../../components/MessageText/markdown.js";
import { Menu, useMenu } from "../../components/Menu/Menu.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Views } from "../../components/Views/Views.jsx";
import { PersonButton } from "../../components/PersonCard/PersonCard.jsx";
import { Popover } from "../../components/Popover/Popover.jsx";
import { Answers, Result } from "../../components/Result/Result.jsx";
import { sizeWords } from "../../components/Review/Review.jsx";
import { Count } from "../../components/Status/Status.jsx";
import { toast } from "../../components/Toast/toast.js";
import { Typing } from "../../components/Typing/Typing.jsx";
import { ShareWorkLogDialog, useCanShareWorkLog } from "../../components/WorkLog/ShareWorkLog.jsx";
import {
	actorOfPerson, attachDropped, attachFromComputer, clearConversation, attachmentFirstLines, attachmentState, attachPaths, draftLetterReply, hasNative, MOST_ATTACHMENT_BYTES,
	MOST_ATTACHMENTS, openPersonalChat,
	isPictureFile, openPersonalFile, personalPicture, pictureType, postToThread, protectionProjectOfChat, reactInConversation, showPersonalFile, useRealWork, watchRealChat,
} from "../../data/native.js";
import { viewerId } from "../../data/workspace.js";
import { findMatter, findMember, firstName, formatClock, formatDay, officeLine, positionOf } from "../words.js";
import { conversationsFor, correspondentFace, idOf, keyOf, previewOf } from "./conversations.js";

import styles from "./MessagesPage.module.css";

// Messages: a messenger of the firm (D-042; the owner, 2026-10-26: "as an
// ordinary messenger"). On the left every conversation, the latest first: one
// with each member - people and AI colleagues side by side (UI-5) - and one
// with each correspondent whose letters came through Outlook. On the right the
// conversation open, its words in bubbles, the member's own on the right, and
// the reply under them; who the other is opens beside it on request.
export function MessagesPage({ place, state, onSend, onAnswer }) {
	const { open } = useNavigation();
	useRealWork();
	const conversations = conversationsFor(state);
	const [view, setView] = useState("everyone");
	const [isAbout, setAbout] = useState(false);
	const wanted = keyOf(place.memberId);
	const selected = conversations.find(entry => entry.key == wanted)
		?? (wanted != null && wanted.startsWith("member:") ? strangerOf(idOf(wanted)) : null)
		?? conversations.find(entry => entry.items.length > 0)
		?? null;
	const seen = useSeen(conversations, selected?.key ?? null);

	// The open chat is what the window reads first when the Matter changes.
	const chatId = selected?.send?.kind == "chat" ? selected.send.chatId : null;
	useEffect(() => {
		watchRealChat(chatId);
		return () => watchRealChat(null);
	}, [chatId]);

	// Only conversations with something in them (the owner, 2026-11-02: "why
	// are those I have not written to yet shown here?"); one opened from a
	// profile to begin stands while it is open.
	const shown = conversations
		.filter(entry => entry.items.length > 0 || entry.key == selected?.key)
		.filter(entry => view == "everyone" || (view == "colleagues") == isColleagueConversation(entry));

	return (
		<div className={isAbout && selected != null ? `${styles.page} ${styles.page_about}` : styles.page}>
			<aside className={styles.list} aria-label="Conversations">
				{/* The title, and under it which conversations are listed, as Team
				    and Inbox choose their own (the owner, 2026-11-02: "bring this
				    switch back to Messages instead of the three dots"); no search
				    and no pencil: a conversation begins from a colleague's page. */}
				<div className={styles.list_head}>
					<div className={styles.list_title_row}>
						{/* The page names itself in full; the sidebar keeps "Messages"
						    (the owner, 2026-11-02). */}
						<h1 className={styles.list_title}>Direct Messages</h1>
					</div>
					<Views label="Which conversations" views={MESSAGE_VIEWS} view={view} onChange={setView} controls="message-rows" stretch />
				</div>
				<div className={styles.rows} id="message-rows" role="tabpanel">
					{shown.map(entry => (
						<ConversationRow key={entry.key} entry={entry} isOpen={entry.key == selected?.key} unread={unreadOf(entry, seen)} onOpen={() => open({ kind: "messages", memberId: idOf(entry.key) })} />
					))}
					{shown.length == 0 ? <p className={styles.rows_empty}>{view == "colleagues" ? "No conversation with an AI colleague yet." : "No conversation yet."}</p> : null}
				</div>
			</aside>
			{selected != null ? (
				<Chat
					key={selected.key}
					entry={selected}
					state={state}
					isAbout={isAbout}
					onAbout={() => setAbout(value => !value)}
					onSend={onSend}
					onAnswer={onAnswer}
					onOpen={open}
				/>
			) : (
				<section className={styles.chat_none}>
					<h2 className={styles.chat_none_title}>Messages or threads?</h2>
					<p className={styles.chat_none_line}>A message is between you and one colleague, a person or an AI colleague. Work on a Matter goes to its threads, where its team sees it.</p>
				</section>
			)}
			{isAbout && selected != null ? <About entry={selected} onClose={() => setAbout(false)} onOpen={open} /> : null}
		</div>
	);
}

// Which conversations the list shows: everyone, the people - the firm's and
// those whose letters came - or the AI colleagues.
const MESSAGE_VIEWS = [
	{ id: "everyone", label: "Everyone" },
	{ id: "people", label: "People" },
	{ id: "colleagues", label: "AI colleagues" },
];

function isColleagueConversation(entry) {
	return entry.kind == "member" && findMember(entry.memberId)?.kind == "ai_colleague";
}

// A member Messages was opened for who has no conversation here: one to start.
// A person of the firm the member has not written to yet: the first message
// begins the chat of the two (D-042; the owner, 2026-11-02: "why can't I
// write if there were no messages yet - precisely so that there are").
function strangerOf(memberId) {
	const member = findMember(memberId);
	if (member == null) return null;
	const actorId = member.kind == "ai_colleague" ? null : actorOfPerson(memberId);
	return {
		key: `member:${memberId}`, kind: "member", memberId, actorId, items: [],
		send: actorId != null ? { kind: "personal", actorId }
			: { kind: "none", why: `${firstName(member)} can't be written to from this computer yet.` },
	};
}

// What the member has seen of each conversation: its latest item when they
// last had it open. A conversation met for the first time counts as seen, so
// only what comes afterwards is new.
// Whether client data protection is on, asked while `isAsked`: what a picture
// attached to a reply says before it goes (D-256). Unknown until asked, so the
// note never shows and then goes for a computer where protection is off.
function useProtection(isAsked) {
	const [isOn, setOn] = useState(null);
	useEffect(() => {
		if (isAsked == false) return undefined;
		let isLive = true;
		isProtectionOn().then(on => isLive && setOn(on));
		return () => {
			isLive = false;
		};
	}, [isAsked]);
	return isOn;
}

function useSeen(conversations, openKey) {
	const seen = useRef(new Map());
	for (const entry of conversations) {
		const latest = entry.items.at(-1)?.at ?? "";
		if (seen.current.has(entry.key) == false || entry.key == openKey) seen.current.set(entry.key, latest);
	}
	return seen.current;
}

// How many of the other's words came since the member last had it open.
function unreadOf(entry, seen) {
	const since = seen.get(entry.key) ?? "";
	return entry.items.filter(item => item.isOwn != true && (item.at ?? "") > since).length;
}

// A member's first name, or nothing for one this window does not know.
function firstOf(memberId) {
	const member = memberId != null ? findMember(memberId) : null;
	return member != null ? firstName(member) : "";
}

function faceOf(entry) {
	return entry.kind == "mail" ? correspondentFace(entry.contact) : findMember(entry.memberId);
}

function nameOf(entry) {
	return entry.kind == "mail" ? entry.contact.name : findMember(entry.memberId)?.name ?? "";
}

function lineOf(entry) {
	if (entry.kind == "mail") {
		return [entry.contact.organisation, entry.contact.address].filter(Boolean).join(", ");
	}
	return positionOf(findMember(entry.memberId));
}

// A conversation's line: who, the latest words, when, and whether something
// new waits in it.
function ConversationRow({ entry, isOpen, unread, onOpen }) {
	const last = entry.items.at(-1);
	const isNew = unread > 0;
	return (
		<button className={isOpen ? `${styles.row} ${styles.row_open}` : styles.row} type="button" aria-current={isOpen ? "true" : undefined} onClick={onOpen}>
			{/* A letter's conversation says so on its face, as a messenger marks
			    a conversation's channel, and its words line up with every other
			    row's (the owner, 2026-11-02: "redo the mail mark"). */}
			<span className={styles.row_face}>
				<Avatar member={faceOf(entry)} size={40} />
				{entry.kind == "mail" ? <span className={styles.row_mail} aria-label="Mail"><Icon name="at" size={13} /></span> : null}
			</span>
			<span className={styles.row_text}>
				<span className={styles.row_top}>
					<span className={isNew ? `${styles.row_name} ${styles.row_name_new}` : styles.row_name}>{nameOf(entry)}</span>
					{last != null ? <time className={styles.row_time} dateTime={last.at}>{whenOf(last.at)}</time> : null}
				</span>
				<span className={styles.row_bottom}>
					{isTyping(entry) ? <span className={styles.row_preview}><Typing /></span>
						: <span className={styles.row_preview}>{last != null ? previewOf(entry) : lineOf(entry)}</span>}
					{isNew ? <Count label={`${unread} new`}>{unread}</Count> : null}
				</span>
			</span>
		</button>
	);
}

// A colleague writing its answer, as a messenger shows it (the owner,
// 2026-10-26: "add 'typing…' so it still feels like a messenger"; no work
// animation in a chat): the backend says it writes in this conversation now.
// Once it looks things up it is at work, and its face shows that instead.
function isTyping(entry) {
	return entry.kind == "member" && entry.isAnswering == true;
}

// Today's time, or the day for anything older.
function whenOf(at) {
	if (at == null || at == "") return "";
	const day = new Date(at).toDateString();
	return day == new Date().toDateString() ? formatClock(at) : new Date(at).toLocaleDateString("en-US", { month: "short", day: "numeric" });
}

function Chat({ entry, state, isAbout, onAbout, onSend, onAnswer, onOpen }) {
	const headMenu = useMenu();
	// On a design partner's seat the conversation's work log goes to Legal
	// Machines on request (D-257).
	const [isSendingLog, setSendingLog] = useState(false);
	const canShareWorkLog = useCanShareWorkLog() && entry.send?.kind == "chat" && (entry.send.chatId ?? "") != "" && (entry.send.projectId ?? "") != "";
	// The conversation cleared for the member, on this computer, with its
	// Undo; the other member keeps theirs.
	async function clearHere(conversation) {
		try {
			const undo = await clearConversation(conversation);
			toast(`Your conversation with ${nameOf(conversation)} is cleared. ${isColleagueConversation(conversation) ? `${firstName(findMember(conversation.memberId))} starts afresh.` : "They keep theirs."}`, {
				action: { label: "Undo", run: () => undo().catch(() => toast("It could not be put back.", { tone: "seal" })) },
			});
		} catch (error) {
			toast(error?.message ?? "The conversation could not be cleared.", { tone: "seal" });
		}
	}
	const scrollRef = useRef(null);
	// The message the member answers, quoted over the reply; and the files put
	// into the conversation for the next message, as a messenger has them
	// (D-212): chosen with the paperclip, dropped on the conversation, or pasted.
	const [quote, setQuote] = useState(null);
	const [pending, setPending] = useState([]);
	const [isAttaching, setAttaching] = useState(false);
	// The files being put in, shown at once as tiles - a picture as itself -
	// each with the mark of its loading over it, never as words alone that read
	// as if nothing came in (the owner, 2026-11-05).
	const [placing, setPlacing] = useState([]);
	function beginPlacing(list) {
		setPlacing(list);
		setAttaching(true);
	}
	function endPlacing() {
		setPlacing(list => {
			for (const file of list) if (file.preview != null) URL.revokeObjectURL(file.preview);
			return [];
		});
		setAttaching(false);
	}
	const [isDropping, setDropping] = useState(false);
	const dragDepth = useRef(0);
	// What each file put in stands at: read, or still being read. The reply
	// goes once each is read (the owner, 2026-11-02).
	const [fileStates, setFileStates] = useState({});
	// Files go with a message to a colleague, and with a reply to a letter,
	// where they become the Matter's documents, attached to the draft (D-115).
	const canAttach = (entry.kind == "member" && ["chat", "personal"].includes(entry.send?.kind))
		|| (entry.send?.kind == "mail" && entry.send.letterId != null && (entry.send.matterId ?? "") != "");
	const first = firstOf(entry.memberId) || nameOf(entry).split(/\s+/)[0];

	// An answer to a colleague's question goes back to it as the member's reply,
	// quoting the question (D-120); the prototype keeps it in its own state.
	function answer(conversation, item, id, option) {
		if (conversation.send?.kind != "chat") {
			onAnswer(id, option, { memberId: conversation.memberId, matterId: item.matterId ?? null });
			return;
		}
		postToThread(conversation.send.chatId, option, { quoteMessageId: id, answerOption: option, mentions: conversation.send.mentions, projectId: conversation.send.projectId })
			.catch(error => toast(error?.message ?? "The answer could not be sent.", { tone: "seal" }));
	}
	const face = faceOf(entry);
	const items = entry.items;
	const latest = items.at(-1);

	// A conversation opens at its latest words, and follows what comes.
	useEffect(() => {
		const scroller = scrollRef.current;
		if (scroller == null) return;
		scroller.scrollTo({ top: scroller.scrollHeight, behavior: window.matchMedia?.("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth" });
	}, [latest?.id, items.length]);

	// While the member is at its foot, it stays there as a picture takes its
	// size once loaded (D-212): a photo never stands cut under the reply.
	const isAtFoot = useRef(true);
	useEffect(() => {
		const scroller = scrollRef.current;
		if (scroller == null) return undefined;
		const onScroll = () => {
			isAtFoot.current = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 64;
		};
		scroller.addEventListener("scroll", onScroll, { passive: true });
		return () => scroller.removeEventListener("scroll", onScroll);
	}, []);
	function keepAtFoot() {
		const scroller = scrollRef.current;
		if (scroller != null && isAtFoot.current) scroller.scrollTop = scroller.scrollHeight;
	}

	// Where the next message goes: the conversation's chat, begun first when it
	// is the member's first word to a person (D-042).
	async function placeOfNext() {
		if (entry.send.kind == "personal") return openPersonalChat(entry.send.actorId);
		if (entry.send.kind == "mail") return { chatId: null, projectId: entry.send.matterId };
		return { chatId: entry.send.chatId, projectId: entry.send.projectId };
	}

	// Each file keeps the place it went to: a person's first message opens
	// their chat, and its place, only as the first file goes in.
	function addPending(added, projectId) {
		setPending(list => {
			const placed = added.map(file => ({ ...file, projectId }));
			const next = [...list, ...placed.filter(file => list.some(known => known.documentId == file.documentId) == false)];
			if (next.length > MOST_ATTACHMENTS) toast(`A message hands over at most ${MOST_ATTACHMENTS} files; the rest wait for the next one.`);
			return next.slice(0, MOST_ATTACHMENTS);
		});
	}

	async function attachFiles(files) {
		if (files.length == 0 || canAttach == false) return;
		beginPlacing(files.map((file, index) => ({
			key: `${index}:${file.name}`, name: file.name,
			preview: pictureType(file.name) != null ? URL.createObjectURL(file) : null,
		})));
		try {
			const place = await placeOfNext();
			const added = await attachDropped(place.projectId, files);
			if (added.length == 0) throw new Error("The files could not be put into the conversation. Try again.");
			addPending(added, place.projectId);
		} catch (error) {
			toast(error?.message ?? "The files could not be put into the conversation.", { tone: "seal" });
		} finally {
			endPlacing();
		}
	}

	// The tiles of putting files in stand only once files were chosen: a
	// dialog closed with none chosen leaves the reply as it was.
	async function attachChosen() {
		if (isAttaching) return;
		try {
			const place = await placeOfNext();
			addPending(await attachFromComputer(place.projectId, () => beginPlacing([{ key: "chosen", name: "", preview: null }])), place.projectId);
		} catch (error) {
			toast(error?.message ?? "The file could not be put into the conversation.", { tone: "seal" });
		} finally {
			endPlacing();
		}
	}

	// Files dragged over the conversation: it says it takes them, and takes
	// them where they are let go. In the app the window takes the drag itself
	// and hands over where the files are (Platform/FileDrop); in a browser the
	// page reads the files it is given. Anything else dragged is left alone.
	const sectionRef = useRef(null);
	const isWindowDrop = canAttach && hasNative();

	// The room the reply takes over the conversation's foot, kept at its end so
	// its last message scrolls clear of the box; a conversation at its foot
	// stays there as the box grows.
	const [replyRoom, setReplyRoom] = useState(0);
	useEffect(() => {
		const reply = sectionRef.current?.querySelector(`[data-reply-for]`);
		if (reply == null || typeof ResizeObserver != "function") {
			setReplyRoom(0);
			return undefined;
		}
		const observer = new ResizeObserver(() => setReplyRoom(Math.ceil(reply.getBoundingClientRect().height)));
		observer.observe(reply);
		return () => observer.disconnect();
	}, [entry.key, entry.send?.kind]);
	useEffect(() => {
		keepAtFoot();
	}, [replyRoom]);
	useWindowFileDrop(sectionRef, isWindowDrop, setDropping, files => attachDroppedPaths(files));

	async function attachDroppedPaths(files) {
		const paths = files.filter(file => file.isFolder != true).map(file => file.path).filter(Boolean);
		if (paths.length < files.length) toast("A folder can't go into a message. Drop the files in it instead.");
		if (paths.length == 0 || canAttach == false) return;
		beginPlacing(paths.map(path => ({ key: path, name: path.split(/[\\/]/).pop() ?? path, preview: null })));
		try {
			const place = await placeOfNext();
			const added = await attachPaths(place.projectId, paths);
			if (added.length == 0) throw new Error("The files could not be put into the conversation. Try again.");
			const tooLarge = added.filter(file => (file.sizeBytes ?? 0) > MOST_ATTACHMENT_BYTES);
			if (tooLarge.length > 0) {
				toast(`${tooLarge.map(file => file.name).join(", ")} ${tooLarge.length == 1 ? "is" : "are"} larger than 48 MB, more than the firm's computers carry in one message.`, { tone: "seal" });
			}
			addPending(added.filter(file => tooLarge.includes(file) == false), place.projectId);
		} catch (error) {
			toast(error?.message ?? "The files could not be put into the conversation.", { tone: "seal" });
		} finally {
			endPlacing();
		}
	}

	const carriesFiles = event => [...(event.dataTransfer?.types ?? [])].includes("Files");
	// A picture or a file pasted into the conversation goes with the next
	// message, as one dropped does.
	const onPaste = event => {
		const files = [...(event.clipboardData?.files ?? [])];
		if (files.length == 0) return;
		event.preventDefault();
		attachFiles(files);
	};
	const drag = canAttach == false ? {} : isWindowDrop ? { onPaste } : {
		onDragEnter: event => {
			if (carriesFiles(event) == false) return;
			event.preventDefault();
			dragDepth.current += 1;
			setDropping(true);
		},
		onDragOver: event => {
			if (carriesFiles(event) == false) return;
			event.preventDefault();
			event.dataTransfer.dropEffect = "copy";
		},
		onDragLeave: event => {
			if (carriesFiles(event) == false) return;
			dragDepth.current = Math.max(0, dragDepth.current - 1);
			if (dragDepth.current == 0) setDropping(false);
		},
		onDrop: event => {
			if (carriesFiles(event) == false) return;
			event.preventDefault();
			dragDepth.current = 0;
			setDropping(false);
			attachFiles([...event.dataTransfer.files]);
		},
		onPaste,
	};

	function reply(item) {
		setQuote({ id: item.id, authorId: item.authorId, text: (item.paragraphs ?? []).join(" ") || filesWords(item.realFiles) });
		document.querySelector(`[data-reply-for="${entry.key}"] textarea`)?.focus();
	}

	return (
		<section ref={sectionRef} className={styles.chat} style={{ "--reply-room": `${replyRoom}px` }} aria-label={`Conversation with ${nameOf(entry)}`} data-file-drop={canAttach ? "" : undefined} {...drag}>
			{/* Who it is with, as Twist's: the face and the name, and its
			    menu (the owner, 2026-11-02). */}
			<header className={styles.chat_head}>
				{/* Until something is written the opening below says who it is
				    with, so the head shows only its menu (the owner, 2026-11-02). */}
				{items.length == 0 ? <span className={styles.chat_who} /> : entry.kind == "member" ? (
					<PersonButton memberId={entry.memberId} label={nameOf(entry)}><Avatar member={face} size={36} /></PersonButton>
				) : <Avatar member={face} size={36} />}
				{/* The name, and under it their office in one phrase - or, for
				    a correspondent, what the letters say of them; while they
				    write, that they are writing. */}
				{items.length == 0 ? null : <span className={styles.chat_who}>
					<span className={styles.chat_name}>{nameOf(entry)}</span>
					{isTyping(entry) ? <span className={styles.chat_line} aria-live="polite"><Typing /></span>
						: (entry.kind == "member" ? officeLine(findMember(entry.memberId)) : lineOf(entry)) != "" ? (
							<span className={styles.chat_line}>{entry.kind == "member" ? officeLine(findMember(entry.memberId)) : lineOf(entry)}</span>
						) : null}
				</span>}
				{/* A DM to an AI colleague can be read as an AI provider receives it (D-233). */}
				{isColleagueConversation(entry) && entry.send?.kind == "chat" && entry.send.chatId != null ? <ProviderViewButton chatId={entry.send.chatId} /> : null}
				<IconButton icon="more" label={`More for ${nameOf(entry)}`} pressed={headMenu.isOpen || isAbout} onClick={event => headMenu.toggle(event.currentTarget)} />
				{headMenu.isOpen ? (
					<Menu anchor={headMenu.anchor} label={nameOf(entry)} minWidth={220} placement="bottom-end" onClose={headMenu.close} items={[
						{ key: "about", label: entry.kind == "mail" ? "Contact" : "Profile", icon: "info", checked: isAbout, onSelect: onAbout },
						entry.kind == "member" && entry.memberId != null
							? { key: "page", label: "Open their page", icon: "team", onSelect: () => onOpen({ kind: "profile", memberId: entry.memberId }) } : null,
						// Cleared for the member, with its Undo (the owner, 2026-11-04).
						entry.kind == "member" && items.length > 0 && hasNative()
							? { key: "clear", label: "Clear conversation", icon: "trash", onSelect: () => clearHere(entry) } : null,
						canShareWorkLog ? { key: "workLog", label: "Send work log to Legal Machines", icon: "send", onSelect: () => setSendingLog(true) } : null,
					].filter(Boolean)} />
				) : null}
				{isSendingLog ? (
					<ShareWorkLogDialog matterId={entry.send.projectId} chatId={entry.send.chatId} title={nameOf(entry)}
						messages={items.map(item => ({ author: findMember(item.authorId)?.name ?? "", at: item.at ?? "", text: (item.paragraphs ?? []).join("\n\n") }))}
						onClose={() => setSendingLog(false)} />
				) : null}
			</header>
			{/* No fade at either edge: the head and the reply box cover what
			    scrolls under them (the owner, 2026-11-02). */}
			<div ref={scrollRef} className={styles.scroll}>
				<div className={styles.items}>
					{items.length == 0 ? <Opening entry={entry} /> : null}
					{items.map((item, index) => (
						entry.kind == "mail" || item.letterId != null
							? <Letter key={item.id} item={item} previous={items[index - 1]} />
							: <Message key={item.id} item={item} previous={items[index - 1]} state={state} send={entry.send} onAnswer={(id, option) => answer(entry, item, id, option)} onOpen={onOpen} onReply={reply} onSized={keepAtFoot} />
					))}
				</div>
			</div>
			<Reply
				entry={entry}
				onSend={onSend}
				quote={quote}
				onClearQuote={() => setQuote(null)}
				pending={pending}
				placeOfFiles={entry.send?.kind == "mail" ? entry.send.matterId : entry.send?.projectId}
				isReading={pending.some(file => (fileStates[file.documentId] ?? "reading") == "reading")}
				onFileState={(documentId, fileState) => setFileStates(states => states[documentId] == fileState ? states : { ...states, [documentId]: fileState })}
				onRemovePending={documentId => setPending(list => list.filter(file => file.documentId != documentId))}
				onSent={() => {
					setQuote(null);
					setPending([]);
				}}
				canAttach={canAttach}
				isAttaching={isAttaching}
				placing={placing}
				onAttach={attachChosen}
			/>
			{isDropping ? <DropOverlay words={`Drop to send to ${first}`} /> : null}
		</section>
	);
}

// What a message hands over, in words, where it has no words of its own.
function filesWords(files) {
	const list = files ?? [];
	if (list.length == 0) return "";
	if (list.length == 1) return list[0].name ?? "A file";
	return `${list.length} files`;
}

// What an empty conversation says: who it is with.
function Opening({ entry }) {
	const name = nameOf(entry);
	return (
		<div className={styles.opening}>
			<Avatar member={faceOf(entry)} size={64} />
			<p className={styles.opening_name}>{name}</p>
			<p className={styles.opening_line}>{entry.kind == "mail"
				? `Letters from ${name} come here once they are filed to a Matter from Outlook.`
				: `This is where you and ${name.split(/\s+/)[0]} talk.`}</p>
		</div>
	);
}

function DayMark({ at, previous }) {
	const day = formatDay(at);
	if (previous != null && formatDay(previous.at) == day) return null;
	return <p className={styles.day}>{day}</p>;
}

// The reactions a message takes in one press, as a thread's (ThreadView).
const QUICK_REACTIONS = ["👍", "✅", "👀", "🙏", "❓"];

// A moment as a message's menu tells it: the day where it is not today, and
// the clock to the second, so a message sent and received in the same minute
// still shows how long it took.
function momentOf(at) {
	if (at == null || at == "") return "";
	const date = new Date(at);
	const clock = date.toLocaleTimeString("en-US", { hour: "numeric", minute: "2-digit", second: "2-digit" });
	const day = formatDay(at);
	if (day == "Today") return clock;
	if (day == "Yesterday") return `Yesterday ${clock}`;
	return `${date.toLocaleDateString("en-US", { month: "short", day: "numeric" })}, ${clock}`;
}

// A message as a messenger shows it (D-212; the owner, 2026-11-02): its words
// in a bubble with no time on it, the photos it hands over as pictures and
// its other files as files, the message it answers over it, and its
// reactions under it. A right-click opens what can be done with it - reply,
// react, copy its words - and when it was sent and when it reached the other.
function Message({ item, previous, state, send, onAnswer, onOpen, onReply, onSized }) {
	// Read as an AI provider receives it, from the conversation's corner (D-233).
	const isProviderView = useProviderView(send?.kind == "chat" ? send.chatId : null);
	const isOwn = item.isOwn == true;
	const author = findMember(item.authorId);
	const isSameAuthor = previous != null && previous.authorId == item.authorId && formatDay(previous.at) == formatDay(item.at) && item.quote == null;
	const matter = item.matterId != null ? findMatter(item.matterId) : null;
	const isNewFiling = matter != null && (previous == null || previous.matterId != item.matterId);
	const isGone = item.isDeleted == true;
	const paragraphs = isGone ? ["This message was deleted."] : item.paragraphs ?? [];
	// Its reactions, a person's and a colleague's alike (D-130), as a messenger
	// shows them under the bubble.
	const reactions = isGone ? {} : item.realReactions ?? {};
	const files = isGone ? [] : item.realFiles ?? [];
	const photos = files.filter(file => pictureType(file.name) != null);
	const others = files.filter(file => pictureType(file.name) == null);
	const canAct = item.isReal == true && send?.kind == "chat" && isGone == false && item.isSending != true;
	const menu = useMenu();
	const [picker, setPicker] = useState(null);

	function react(emoji) {
		reactInConversation(send, item.id, emoji).catch(error => toast(error?.message ?? "The reaction was not kept.", { tone: "seal" }));
	}

	function copy() {
		const selection = window.getSelection();
		const node = document.getElementById(`message-${item.id}`);
		const selected = selection != null && selection.isCollapsed == false && node?.contains(selection.anchorNode) ? selection.toString() : "";
		navigator.clipboard?.writeText(selected || paragraphs.join("\n\n")).then(() => toast("Copied.")).catch(() => {});
	}

	function onContextMenu(pointer) {
		if (item.isReal != true || pointer.target.closest?.("button, textarea, input, a")) return;
		pointer.preventDefault();
		const x = pointer.clientX;
		const y = pointer.clientY;
		menu.open({
			getBoundingClientRect: () => ({ top: y, bottom: y, left: x, right: x, width: 0, height: 0 }),
			contains: () => false,
			focus: () => {},
		});
	}

	const menuItems = [
		canAct ? { key: "reply", label: "Reply", icon: "reply", onSelect: () => onReply(item) } : null,
		canAct ? {
			key: "react", label: "React", icon: "smile",
			submenu: QUICK_REACTIONS.map(emoji => ({ key: emoji, label: emoji, checked: (reactions[emoji] ?? []).includes(viewerId), onSelect: () => react(emoji) })),
		} : null,
		isGone || paragraphs.length == 0 ? null : { key: "copy", label: "Copy text", icon: "copy", onSelect: copy },
		{ key: "sent", info: item.isSending == true ? "Sending" : "Sent", icon: "send", hint: item.isSending == true ? null : momentOf(item.at) },
		(item.receivedAt ?? "") != "" ? { key: "received", info: isOwn ? `Received by ${firstOf(item.receivedBy) || "them"}` : "Received", icon: "check", hint: momentOf(item.receivedAt) } : null,
	];

	return (
		<>
			<DayMark at={item.at} previous={previous} />
			{isNewFiling ? (
				<p className={styles.filing}>
					<Icon name="matter" size={14} />
					Filed to <button className={styles.filing_link} type="button" onClick={() => onOpen({ kind: "matter", matterId: matter.id, tab: "overview" })}>{matter.name}</button>
				</p>
			) : null}
			<div
				id={`message-${item.id}`}
				className={[styles.message, isOwn ? styles.message_own : null, isSameAuthor ? styles.message_more : null, menu.isOpen ? styles.message_menu : null].filter(Boolean).join(" ")}
				onContextMenu={onContextMenu}
			>
				{isOwn ? null : (
					<span className={styles.message_face}>
						{isSameAuthor || author == null ? null : <PersonButton memberId={author.id} label={author.name}><Avatar member={author} size={32} /></PersonButton>}
					</span>
				)}
				<div className={styles.message_line}>
				<div className={styles.message_body}>
					{item.quote != null && isGone == false ? <Quote quote={item.quote} isOwn={isOwn} /> : null}
					{photos.length > 0 ? <Photos files={photos} send={send} onSized={onSized} /> : null}
					{paragraphs.length > 0 ? (
						<div className={isGone ? `${styles.bubble} ${styles.bubble_gone}` : styles.bubble} data-selectable=""
							data-tooltip={item.isSending == true || (item.at ?? "") == "" ? undefined : momentOf(item.at)}>
							{isProviderView && isGone == false
								? <MaskedText projectId={send.projectId ?? protectionProjectOfChat(send.chatId)} text={paragraphs.join("\n\n")} />
								: <MessageText text={paragraphs.join("\n\n")} />}
						</div>
					) : null}
					{others.length > 0 ? <Files files={others} send={send} /> : null}
					{item.isSending == true ? <p className={styles.sending}>Sending</p> : null}
					{/* Just under the message, on the side it is read from, as a
					    messenger shows them (the owner, 2026-11-02); a new one comes
					    from the message's right-click menu. */}
					<Reactions className={styles.message_reactions} reactions={reactions} viewerId={viewerId} resolveMember={findMember}
						onReact={canAct ? react : undefined} />
					{Array.isArray(item.files) && item.files.length > 0 ? <Result files={item.files} notChecked={item.notChecked} /> : null}
					{item.ask != null ? <Answers options={item.ask.options} answer={item.isReal == true ? item.answered ?? null : state.answers[item.id]} isOwn={isOwn} onAnswer={option => onAnswer(item.id, option)} /> : null}
				</div>
				{/* What may be done with it, beside it while it is pointed at, as
				    Twist shows it: react, and the rest of its menu. */}
				{item.isReal == true ? (
					<span className={styles.message_actions}>
						{canAct ? <IconButton icon="smile" size="small" label="React" pressed={picker != null} onClick={event => setPicker(event.currentTarget)} /> : null}
						<IconButton icon="more" size="small" label="More" pressed={menu.isOpen} onClick={event => menu.toggle(event.currentTarget)} />
					</span>
				) : null}
				</div>
			</div>
			{picker != null ? (
				<Popover anchor={picker} onClose={() => setPicker(null)} placement="bottom-start" width={208} label="React">
					<div className={styles.picker}>
						{QUICK_REACTIONS.map(emoji => (
							<button key={emoji} className={styles.pick} type="button" aria-label={`React with ${emoji}`} onClick={() => {
								react(emoji);
								setPicker(null);
							}}>
								{emoji}
							</button>
						))}
					</div>
				</Popover>
			) : null}
			{menu.isOpen ? <Menu anchor={menu.anchor} label="Message" minWidth={248} onClose={menu.close} items={menuItems} /> : null}
		</>
	);
}

// The message a reply answers, over it: who wrote it and its first words,
// quieter than the reply; a press goes to it.
function Quote({ quote, isOwn }) {
	const author = findMember(quote.authorId);
	return (
		<button
			className={isOwn ? `${styles.quote} ${styles.quote_own}` : styles.quote}
			type="button"
			onClick={() => document.getElementById(`message-${quote.postId}`)?.scrollIntoView({ behavior: "smooth", block: "center" })}
		>
			<Icon name="reply" size={14} />
			<span className={styles.quote_who}>{author == null || author.id == viewerId ? "You" : firstName(author)}</span>
			<span className={styles.quote_text}>{plainOf(quote.text)}</span>
		</button>
	);
}

// The photos a message hands over, as pictures: one alone at its own shape,
// several in a grid of squares. A press opens one in its own program.
function Photos({ files, send, onSized }) {
	return (
		<div className={files.length > 1 ? `${styles.photos} ${styles.photos_grid}` : styles.photos}>
			{files.map(file => <Photo key={file.documentId} file={file} send={send} isSquare={files.length > 1} onSized={onSized} />)}
		</div>
	);
}

// A photo whose bytes have not come yet - the message's record can reach this
// computer before its file - is asked again a few times before it stands as a
// file.
const PHOTO_TRIES = 6;
const PHOTO_AGAIN_MS = 1500;

function Photo({ file, send, isSquare, onSized }) {
	const [source, setSource] = useState(null);
	const [isMissing, setMissing] = useState(false);
	useEffect(() => {
		let isLive = true;
		let timer = null;
		const look = tries => personalPicture(send?.projectId, file).then(found => {
			if (isLive == false) return;
			if (found != null) setSource(found);
			else if (tries + 1 < PHOTO_TRIES) timer = setTimeout(() => look(tries + 1), PHOTO_AGAIN_MS);
			else setMissing(true);
		});
		look(0);
		return () => {
			isLive = false;
			clearTimeout(timer);
		};
	}, [send?.projectId, file.documentId, file.name, file.sizeBytes]);
	// A picture this window cannot show stands as the file it is.
	if (isMissing) return <Files files={[file]} send={send} />;
	return (
		<button
			className={isSquare ? `${styles.photo} ${styles.photo_square}` : styles.photo}
			type="button"
			aria-label={`Open ${file.name}`}
			onClick={() => openPersonalFile(send.projectId, file.documentId).catch(error => toast(error?.message ?? "The picture could not be opened.", { tone: "seal" }))}
		>
			{source != null ? <img src={source} alt={file.name} draggable={false} onLoad={() => onSized?.()} /> : <span className={styles.photo_wait} />}
		</button>
	);
}

// The other files a message hands over, each as a file: its kind, its name and
// its size; a press opens it in its own program, and the button beside it
// shows it in its folder.
function Files({ files, send }) {
	return (
		<div className={styles.tiles}>
			{files.map((file, index) => (
				<FileTile
					key={file.documentId ?? file.name ?? index}
					name={file.name ?? "A document"}
					line={wordOf(kindOf(file.name ?? ""))}
					size={file.sizeBytes != null ? sizeWords(file.sizeBytes) : null}
					onOpen={() => openPersonalFile(send.projectId, file.documentId).catch(error => toast(error?.message ?? "The file could not be opened.", { tone: "seal" }))}
					onDownload={() => showPersonalFile(send.projectId, file.documentId).catch(error => toast(error?.message ?? "The file could not be shown.", { tone: "seal" }))}
					downloadLabel="Show in folder"
					downloadIcon="external"
				/>
			))}
		</div>
	);
}

// A letter, as the member's mailbox had it: its subject, its words, what came
// with it.
function Letter({ item, previous }) {
	return (
		<>
			<DayMark at={item.at} previous={previous} />
			<div className={item.isOwn ? `${styles.message} ${styles.message_own}` : `${styles.message} ${styles.message_letter}`}>
				<div className={styles.message_body}>
					<div className={`${styles.bubble} ${styles.bubble_letter}`} data-selectable="">
						{item.subject != "" ? <p className={styles.letter_subject}>{item.subject}</p> : null}
						{(item.paragraphs ?? []).map((paragraph, index) => <p key={index}>{paragraph}</p>)}
						<time className={styles.bubble_time} dateTime={item.at}>{formatClock(item.at)}</time>
					</div>
					{(item.attachments ?? []).length > 0 ? (
						<ul className={styles.files}>
							{item.attachments.map(attachment => (
								<li key={attachment.name} className={styles.file}>
									<Icon name="attach" size={16} />
									<span className={styles.file_name}>{attachment.name}</span>
								</li>
							))}
						</ul>
					) : null}
				</div>
			</div>
		</>
	);
}

// The reply under the conversation. To a member it is a message: Enter sends
// it, Shift+Enter starts a new line; over it the message it answers and the
// files it hands over, and the paperclip beside it chooses more (D-212). To a
// correspondent it is a letter: the draft opens in Outlook, which sends it
// (D-079, D-105).
function Reply({ entry, onSend, quote = null, onClearQuote, pending = [], placeOfFiles = null, isReading = false, onFileState, onRemovePending, onSent, canAttach = false, isAttaching = false, placing = [], onAttach }) {
	const [draft, setDraft] = useState("");
	const [isBusy, setBusy] = useState(false);
	// The draft as it would go out, before it is sent, while the conversation
	// is read as an AI provider receives it (D-233).
	const isProviderView = useProviderView(entry.send?.kind == "chat" ? entry.send.chatId : null);
	// A picture goes to the AI model as it is: protection masks words, not
	// pictures, so while it is on the picture says so before it goes, and Send
	// is the member's consent (D-256). Only an AI colleague's conversation sends
	// it to a model: one between two people, or a reply drafted in Outlook, does not.
	const hasPictures = isColleagueConversation(entry) && pending.some(file => isPictureFile(file.name));
	const isProtected = useProtection(hasPictures);
	const name = nameOf(entry);
	const first = name.split(/\s+/)[0];

	if (entry.send?.kind == "none") {
		return <p className={styles.reply_none}>{entry.send.why}</p>;
	}

	// What the message carries besides its words: the message it answers and
	// the files it hands over.
	const carried = {
		quoteMessageId: quote?.id ?? null,
		attachmentDocumentIds: pending.map(file => file.documentId),
		files: pending,
		quote: quote != null ? { postId: quote.id, authorId: quote.authorId, text: quote.text } : null,
	};
	const hasFiles = pending.length > 0;

	async function send() {
		const text = draft.trim();
		if ((text == "" && hasFiles == false) || isBusy || isAttaching || isReading) return;
		if (entry.kind == "mail") {
			if (entry.send.kind == "sample") {
				toast("In the real app this opens in Outlook as a draft to send.");
				return;
			}
			setBusy(true);
			try {
				const drafted = await draftLetterReply(entry.send.letterId, text, pending.map(file => file.documentId));
				// A reply longer than a mail link carries is never cut: it goes
				// whole to the clipboard, to paste into the draft.
				if (drafted?.carriesText == false) {
					try {
						await navigator.clipboard.writeText(text);
						toast("The reply is longer than a mail link carries: it’s on the clipboard. Paste it into the draft.", { duration: 10000 });
					} catch {
						toast("The reply is longer than a mail link carries. Copy it from here into the draft.", { tone: "seal", duration: 10000 });
						return;
					}
				} else {
					toast(hasFiles ? `The reply to ${first} is open in your mail app with its files. Read it and send it from there.`
						: `The reply to ${first} is open in your mail app. Read it and send it from there.`);
				}
				setDraft("");
				onSent?.();
			} catch (error) {
				toast(error?.message ?? "The draft could not be made. Try again.", { tone: "seal" });
			}
			setBusy(false);
			return;
		}
		if (entry.send.kind == "sample") {
			onSend(entry.memberId, text, null, {});
			setDraft("");
			return;
		}
		// A person's first message begins the personal chat of the two (D-042).
		if (entry.send.kind == "personal") {
			setDraft("");
			setBusy(true);
			try {
				const opened = await openPersonalChat(entry.send.actorId);
				await postToThread(opened.chatId, text, { projectId: opened.projectId, isPersonal: true, ...carried });
				onSent?.();
			} catch (error) {
				setDraft(current => current == "" ? text : current);
				toast(error?.message ?? "The message could not be sent.", { tone: "seal" });
			}
			setBusy(false);
			return;
		}
		// The words leave the box at once and stand in the conversation, with
		// what they carry; if they could not be posted they come back.
		setDraft("");
		onSent?.();
		try {
			await postToThread(entry.send.chatId, text, { mentions: entry.send.mentions, projectId: entry.send.projectId,
				isPersonal: entry.send.isPersonal == true, ...carried });
		} catch (error) {
			setDraft(current => current == "" ? text : current);
			toast(error?.message ?? "The message could not be sent.", { tone: "seal" });
		}
	}

	const isMail = entry.kind == "mail";
	return (
		<div className={styles.reply} data-reply-for={entry.key}>
			{entry.send?.isPersonal == true && entry.send.reaches == false ? (
				<p className={styles.reply_wait}>
					{/* A personal chat travels once a computer of the other has joined the
					    firm and holds its key (MatterBridge, "reaches"), not through a
					    Matter (the owner, 2026-11-02: "one line; it's out of date"). */}
					{`${first}'s computer hasn't joined the firm yet. What you write goes once it does.`}
				</p>
			) : null}
			{quote != null && isMail == false ? (
				<div className={styles.replying}>
					<Icon name="reply" size={16} />
					<span className={styles.replying_text}>
						<span className={styles.replying_who}>{quote.authorId == viewerId ? "Replying to yourself" : `Replying to ${firstOf(quote.authorId) || first}`}</span>
						<span className={styles.replying_words}>{plainOf(quote.text)}</span>
					</span>
					<button className={styles.replying_drop} type="button" aria-label="Do not reply to it" data-tooltip="Do not reply to it" onClick={onClearQuote}>
						<Icon name="close" size={16} />
					</button>
				</div>
			) : null}
			<ComposerBox
				value={draft}
				onChange={setDraft}
				onSubmit={send}
				sendsOnEnter={isMail == false}
				canSend={(draft.trim() != "" || hasFiles) && !isBusy && !isAttaching && !isReading}
				above={hasFiles || isAttaching || isProviderView ? (
					<>
						{isProviderView ? <MaskedDraft projectId={entry.send.projectId ?? protectionProjectOfChat(entry.send.chatId)} text={draft} /> : null}
						{hasFiles || isAttaching ? (
							<div className={styles.attached_list}>
								{pending.map(file => (
									<AttachedFile key={file.documentId} projectId={file.projectId ?? placeOfFiles} file={file} onState={onFileState}
										isUnmasked={hasPictures && isProtected == true && isPictureFile(file.name)}
										onRemove={() => onRemovePending(file.documentId)} />
								))}
								{placing.map(file => <PlacingFile key={file.key} file={file} />)}
							</div>
						) : null}
					</>
				) : null}
				mentionables={isMail || entry.memberId == null ? [] : [findMember(entry.memberId)].filter(Boolean)}
				placeholder={isMail ? `Reply to ${first}` : `Message ${first}`}
				label={isMail ? `Reply to ${name}` : `Message ${name}`}
				tools={canAttach ? (
					<button className={styles.attach} type="button" aria-label="Attach photos or files" data-tooltip="Attach photos or files" disabled={isAttaching} onClick={onAttach}>
						<Icon name="attach" size={18} />
					</button>
				) : null}
				submit={isMail ? <ComposerAction disabled={(draft.trim() == "" && hasFiles == false) || isBusy || isAttaching || isReading} onClick={send}>Draft in Outlook</ComposerAction> : undefined}
			/>
		</div>
	);
}

// The mark of a file's loading, over its picture or its page (the owner,
// 2026-11-05: the loading mark over the image itself, so it never looks as if
// nothing came in).
function Loading({ label }) {
	return (
		<span className={styles.attached_busy} role="status" aria-label={label}>
			<span className={styles.attached_spinner} aria-hidden="true" />
		</span>
	);
}

// A file on its way into the reply: its tile at once - the picture itself,
// where the window holds it - with the mark of its loading over it.
function PlacingFile({ file }) {
	const isNamed = file.name != "";
	return (
		<div className={file.preview != null ? `${styles.attached} ${styles.attached_is_picture}` : styles.attached} data-state="placing">
			<span className={styles.attached_open}>
				{file.preview != null ? <img className={styles.attached_picture} src={file.preview} alt="" /> : (
					<>
						<span className={styles.attached_preview}>
							<span className={styles.attached_kind}>{isNamed ? <FileKindIcon name={file.name} /> : null}</span>
						</span>
						<span className={styles.attached_foot}>
							{isNamed ? <FileKindIcon name={file.name} size="row" /> : null}
							<span className={styles.attached_name}>{isNamed ? file.name : "A file"}</span>
						</span>
					</>
				)}
			</span>
			<Loading label={isNamed ? `Putting ${file.name} in` : "Putting the file in"} />
		</div>
	);
}

// A file put into the reply, as Codex shows one before it goes (the owner,
// 2026-11-02): a picture as itself; a document as its first page in small,
// with its kind and its name under it. While Singularity reads it, the mark
// of its loading stands over it, and the reply waits for it; one it cannot
// read says that.
function AttachedFile({ projectId, file, onState, isUnmasked = false, onRemove }) {
	const [found, setFound] = useState({ state: "reading", detail: null });
	const [picture, setPicture] = useState(null);
	const [lines, setLines] = useState(null);
	const isPicture = pictureType(file.name) != null;
	const tellRef = useRef(onState);
	tellRef.current = onState;

	useEffect(() => {
		let isLive = true;
		let timer = 0;
		const started = Date.now();
		async function look() {
			const now = await attachmentState(projectId, file.documentId).catch(() => ({ state: "reading", detail: null }));
			if (isLive == false) return;
			// Read for two minutes and still reading: it goes as it is, and
			// its colleague reads it when it can.
			const isLong = now.state == "reading" && Date.now() - started > 120000;
			setFound(now);
			tellRef.current?.(file.documentId, isLong ? "slow" : now.state);
			if (now.state == "reading" && isLong == false) timer = setTimeout(look, 700);
		}
		look();
		return () => {
			isLive = false;
			clearTimeout(timer);
		};
	}, [projectId, file.documentId]);

	useEffect(() => {
		let isLive = true;
		if (isPicture) personalPicture(projectId, file).then(url => isLive && setPicture(url));
		else if (found.state == "ready") attachmentFirstLines(projectId, file.documentId).then(read => isLive && setLines(read));
		return () => {
			isLive = false;
		};
	}, [projectId, file.documentId, found.state]);

	// A picture with no words in it is still a picture the AI model sees
	// (D-256): only a document that cannot be read says so.
	const shownState = isPictureFile(file.name) && found.state == "failed" ? "ready" : found.state;
	const words = shownState == "failed" ? "Can't be read" : shownState == "gone" ? "No longer here" : null;
	const open = () => openPersonalFile(projectId, file.documentId).catch(() => {});
	return (
		<div className={isPicture && picture != null ? `${styles.attached} ${styles.attached_is_picture}` : styles.attached} data-state={shownState}>
			<button className={styles.attached_open} type="button" aria-label={`Open ${file.name}`} title={found.detail ?? file.name} onClick={open}>
				{isPicture && picture != null ? (
					<img className={styles.attached_picture} src={picture} alt="" />
				) : (
					<>
						<span className={styles.attached_preview}>
							{lines != null && lines.length > 0 ? (
								<span className={styles.attached_page}>
									{lines.map((line, index) => (
										<span key={index} className={styles.attached_line} data-strong={line.isStrong ? "" : undefined} data-centered={line.isCentered ? "" : undefined}>{line.text}</span>
									))}
								</span>
							) : <span className={styles.attached_kind}><FileKindIcon name={file.name} /></span>}
						</span>
						<span className={styles.attached_foot}>
							<FileKindIcon name={file.name} size="row" />
							<span className={styles.attached_name}>{file.name}</span>
						</span>
					</>
				)}
				{words != null ? <span className={styles.attached_state}>{words}</span> : null}
			</button>
			{found.state == "reading" ? <Loading label={`Reading ${file.name}`} /> : null}
			{/* While protection is on, a picture says in its own corner that the AI
			    model sees it as it is, and why on hover (D-256): a plain fact, no
			    alarm, once it is read. Where the tile shows the picture, at its
			    foot; otherwise at its head, where nothing else is said. */}
			{isUnmasked && found.state != "reading" && (words == null || isPicture && picture != null) ? (
				<span className={styles.attached_badge} data-place={isPicture && picture != null ? "foot" : "head"}
					data-tooltip="Protection masks words, not pictures.">Not masked</span>
			) : null}
			<button className={styles.attached_remove} type="button" aria-label={`Take ${file.name} off`} data-tooltip="Take it off" onClick={onRemove}>
				<Icon name="close" size={14} />
			</button>
		</div>
	);
}

// Who the other is, beside the conversation on request: a member's place in
// the firm and their profile; a correspondent's address and the Matters their
// letters were filed to.
function About({ entry, onClose, onOpen }) {
	const face = faceOf(entry);
	const member = entry.kind == "member" ? findMember(entry.memberId) : null;
	const matterIds = [...new Set(entry.items.map(item => item.matterId).filter(Boolean))];
	return (
		<aside className={styles.about} aria-label={`About ${nameOf(entry)}`}>
			<div className={styles.about_head}>
				<Button size="small" icon="close" aria-label="Close" onClick={onClose} />
			</div>
			<div className={styles.about_body}>
				<Avatar member={face} size={72} withState={entry.kind == "member"} />
				<p className={styles.about_name}>{nameOf(entry)}</p>
				<p className={styles.about_line}>{lineOf(entry)}</p>
				{member?.area != null ? <p className={styles.about_text}>{member.area}</p> : null}
				{entry.kind == "mail" && entry.contact.role != null ? <p className={styles.about_text}>{entry.contact.role}</p> : null}
				{matterIds.length > 0 ? (
					<div className={styles.about_matters}>
						{matterIds.map(id => findMatter(id)).filter(Boolean).map(matter => (
							<Button key={matter.id} size="small" icon="matter" onClick={() => onOpen({ kind: "matter", matterId: matter.id, tab: "overview" })}>{matter.name}</Button>
						))}
					</div>
				) : null}
				{member != null && member.id != viewerId ? <Button onClick={() => onOpen({ kind: "profile", memberId: member.id })}>Open profile</Button> : null}
			</div>
		</aside>
	);
}
