import { useEffect, useRef, useState } from "react";
import { isEmulation } from "../../emulation/emulation.js";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Reactions } from "../../components/Reactions/Reactions.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { ComposerAction, ComposerBox } from "../../components/Composer/ComposerBox.jsx";
import { Dialog } from "../../components/Dialog/Dialog.jsx";
import { TextArea } from "../../components/Field/Field.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { MessageText } from "../../components/MessageText/MessageText.jsx";
import { MaskedDraft, MaskedText } from "../../components/ProviderView/ProviderView.jsx";
import { HOLD_AFTER_MS, holdingLine } from "./holding.js";
import { useProviderView } from "../../components/ProviderView/providerView.js";
import { plainOf } from "../../components/MessageText/markdown.js";
import { Menu, useMenu } from "../../components/Menu/Menu.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Popover } from "../../components/Popover/Popover.jsx";
import { Answers as AskAnswers, Result } from "../../components/Result/Result.jsx";
import { AddButton, DraftTray, useResultDraft } from "../../components/Result/ResultDraft.jsx";
import { Faces } from "../../components/Faces/Faces.jsx";
import { PersonButton } from "../../components/PersonCard/PersonCard.jsx";
import { AssignmentBlock, AssignmentDetails, DecisionList, PostReview, RealFiles, resultDocumentIdsOf } from "../../components/Review/Review.jsx";
import { ProjectBand } from "../../components/ProjectMark/ProjectMark.jsx";
import { toast } from "../../components/Toast/toast.js";
import { addDocumentFromComputer, protectionProjectOfChat, assignmentOfPost, closeRealThread, colleaguesTypingIn, confirmMemberComputer, decisionsOfPost, decideReview, decideWritten, deleteRealPostWithUndo, editRealPost, postToThread, reactToRealPost, realDocuments, reopenRealThread, showPlace, unconfirmedComputersOf, watchRealChat } from "../../data/native.js";
import { decisions, viewerId, workProducts } from "../../data/workspace.js";
import { findThread, isThreadClosed, threadEvents } from "../threadState.js";
import { findMatter, findMember, firstName, formatMoment } from "../words.js";

import styles from "./ThreadView.module.css";

// A thread, as Twist keeps one: its title, its project's band and who takes
// part, the posts in the order they were written, and the reply at the foot.
// A colleague's result comes as a file on its post; what it asks of the
// member, as the two answers under its words, the way a junior lawyer asks
// what to do next. Its head in the page is the unseen room (`placeholderRef`)
// over which the page's MovingHead draws the title, the band and the faces
// and carries them into the window's band as the thread scrolls.
export function ThreadView({ threadId, state, onDecide, onReply, onReact, onEditPost, onDeletePost, onMarkUnread, onAnswer, onCloseThread, onReopenThread, scrollRef: givenScrollRef, placeholderRef }) {
	const { open } = useNavigation();
	const thread = findThread(threadId);
	const matter = findMatter(thread.matterId);
	const events = threadEvents(thread, state);
	const isClosed = isThreadClosed(thread, state);
	const people = thread.participantIds.map(findMember);
	// Whom a message here can name: the Matter's members, people and AI
	// colleagues together, the member at this computer apart (UI-5).
	const mentionables = (matter?.memberIds ?? thread.participantIds).map(findMember)
		.filter(member => member != null && member.id != viewerId);
	const ownScrollRef = useRef(null);
	const scrollRef = givenScrollRef ?? ownScrollRef;
	// The message a reply answers, quoted over the reply box.
	const [quote, setQuote] = useState(null);
	// A real thread (data/native.js): the backend's, and what it supports.
	const isReal = thread.isReal == true;
	// What the lawyer is returning with comments: a review's version, or a
	// document a colleague wrote (D-143); whom it goes back to, and how.
	const [returning, setReturning] = useState(null);
	const actions = {
		onReact,
		onEditPost,
		onDeletePost,
		onMarkUnread,
		onAnswer,
		onReopen: () => {
			if (isReal == false) {
				onReopenThread(thread.id);
				return;
			}
			reopenRealThread(thread.real.chatId).catch(error => toast(error?.message ?? "The thread could not be reopened.", { tone: "seal" }));
		},
		onQuote: event => setQuote({ postId: event.id, authorId: event.authorId, text: event.paragraphs.join(" ") }),
		onReturnReview: (returned, version) => setReturning({
			agentName: returned.agentName,
			what: `version ${version.number}`,
			decide: comment => decideReview(returned.id, version.number, "return", comment),
		}),
		onReturnWritten: (assignment, step) => setReturning({
			agentName: assignment.agentName,
			what: step.label || "the document",
			decide: comment => decideWritten(assignment.id, step.number, "return", comment),
		}),
	};

	// The open thread is what the window reads first when the Matter changes.
	const openChatId = isReal == true ? thread.real?.chatId ?? null : null;
	useEffect(() => {
		watchRealChat(openChatId);
		return () => watchRealChat(null);
	}, [openChatId]);

	// A thread opens at its latest words.
	useEffect(() => {
		if (isEmulation) return;
		const scroller = scrollRef.current;
		if (scroller != null) scroller.scrollTop = scroller.scrollHeight;
	}, [threadId]);

	// A new message is shown as it comes - from another computer, a colleague or
	// the member - to a reader at the thread's latest words, and always after the
	// member's own; one who scrolled up to read stays where they are (the
	// owner, 2026-10-26: a message from the Mac stood below the window's edge).
	const isAtEnd = useRef(true);
	const latest = events.at(-1);
	useEffect(() => {
		if (isEmulation) return undefined;
		const scroller = scrollRef.current;
		if (scroller == null) return undefined;
		const onScroll = () => {
			isAtEnd.current = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 96;
		};
		scroller.addEventListener("scroll", onScroll, { passive: true });
		return () => scroller.removeEventListener("scroll", onScroll);
	}, [threadId]);
	useEffect(() => {
		if (isEmulation) return;
		const scroller = scrollRef.current;
		if (scroller == null || latest == null) return;
		if (isAtEnd.current == false && latest.authorId != viewerId) return;
		const isCalm = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches == true;
		scroller.scrollTo({ top: scroller.scrollHeight, behavior: isCalm ? "auto" : "smooth" });
		isAtEnd.current = true;
	}, [latest?.id, events.length]);

	// The reply box stands over the thread's foot, as a personal chat's does
	// (the owner, 2026-11-02: "make the logic of hiding by the chat as in DMs"):
	// what scrolls goes under it, and the room it takes is kept at the thread's
	// end so the last message scrolls clear of it; a reader at the end stays
	// there as the box grows.
	const viewRef = useRef(null);
	const [replyRoom, setReplyRoom] = useState(0);
	useEffect(() => {
		const reply = viewRef.current?.querySelector("[data-reply-box]");
		if (reply == null || typeof ResizeObserver != "function") {
			setReplyRoom(0);
			return undefined;
		}
		const observer = new ResizeObserver(() => setReplyRoom(Math.ceil(reply.getBoundingClientRect().height)));
		observer.observe(reply);
		return () => observer.disconnect();
	}, [threadId]);
	useEffect(() => {
		const scroller = scrollRef.current;
		if (scroller != null && isAtEnd.current) scroller.scrollTop = scroller.scrollHeight;
	}, [replyRoom]);

	return (
		<div ref={viewRef} className={styles.view} style={{ "--reply-room": `${replyRoom}px` }} data-demonstration={isEmulation || undefined}>
			{/* No fade at its top or foot, as a personal chat has none (the owner,
			    2026-11-02: "remove these clouds at the top and bottom in threads,
			    as it is done in DMs"). */}
			<div ref={scrollRef} className={styles.scroll}>
				<article className={styles.thread}>
					{!isEmulation ? <header ref={placeholderRef} className={`${styles.head} ${styles.head_room}`} aria-hidden="true">
						<p className={styles.title} data-part="title">{thread.title}</p>
						<div className={styles.where}>
							<span data-part="band"><ProjectBand matter={matter} /></span>
							<span data-part="faces"><Faces people={people} size={22} /></span>
						</div>
					</header> : null}
					<ol className={styles.posts}>
						{events.map((event, index) => (
							<Event key={event.id} event={event} previous={events[index - 1]} thread={thread} state={state} onDecide={onDecide} actions={actions} />
						))}
					</ol>
					<HoldingPosts memberIds={isReal == true ? colleaguesTypingIn(thread.real?.chatId) : state.typing?.[thread.id] ?? []} thread={thread} />
				</article>
			</div>
			{!isEmulation ? <Composer
				key={threadId}
				isClosed={isClosed}
				isReal={isReal}
				isEmpty={events.length == 0}
				names={people.filter(member => member.id != viewerId).map(firstName)}
				mentionables={mentionables}
				matterId={thread.matterId}
				chatId={thread.real?.chatId}
				quote={quote}
				onClearQuote={() => setQuote(null)}
				returning={returning}
				onStopReturning={() => setReturning(null)}
				onReturn={async comment => {
					const { agentName, what, decide } = returning;
					try {
						await decide(comment);
						setReturning(null);
						toast(`${what.charAt(0).toUpperCase()}${what.slice(1)} went back to ${agentName} with your comments.`);
						return true;
					} catch (error) {
						toast(error?.message ?? "It could not be returned.", { tone: "seal" });
						return false;
					}
				}}
				onSend={async (text, extras) => {
					if (isReal) {
						// The backend keeps the post, with the message it quotes and the
						// files it hands over; the thread shows it once it is kept.
						try {
							await postToThread(thread.real.chatId, text, {
								allowsAll: mentionables.length > 1,
								quoteMessageId: quote?.postId ?? null,
								attachmentDocumentIds: (extras?.files ?? []).map(file => file.documentId).filter(Boolean),
								notChecked: extras?.notChecked ?? null,
								askOptions: extras?.ask?.options ?? [],
							});
							setQuote(null);
							return true;
						} catch (error) {
							toast(error?.message ?? "The message could not be posted.", { tone: "seal" });
							return false;
						}
					}
					onReply(thread.id, text, quote, extras);
					setQuote(null);
					return true;
				}}
				onClose={async conclusion => {
					if (isReal) {
						try {
							await closeRealThread(thread.real.chatId, conclusion);
						} catch (error) {
							toast(error?.message ?? "The thread could not be closed.", { tone: "seal" });
							return;
						}
					} else {
						onCloseThread(thread.id, conclusion);
					}
					toast("The thread is closed.");
				}}
			/> : null}
		</div>
	);
}

// Who writes an answer here now, as a messenger shows it: "Kitsune is typing"
// and its running dots, under the latest post, in the column of the posts'
// words (the owner, 2026-10-26).
// A colleague at work on its reply says so in words once it has worked ten
// seconds, as a message of its own, in place of "is typing" (the owner,
// 2026-11-02; D-236): a line that fits what was asked, in its language, the
// same on every computer. Its reply takes its place when it comes.
function HoldingPosts({ memberIds, thread }) {
	const since = useRef(new Map());
	const [, setTick] = useState(0);
	const now = Date.now();
	for (const id of memberIds) if (since.current.has(id) == false) since.current.set(id, now);
	for (const id of [...since.current.keys()]) if (memberIds.includes(id) == false) since.current.delete(id);
	const key = memberIds.join(",");
	useEffect(() => {
		const waits = memberIds.map(id => (since.current.get(id) ?? Date.now()) + HOLD_AFTER_MS - Date.now()).filter(wait => wait > 0);
		if (waits.length == 0) return undefined;
		const timer = setTimeout(() => setTick(tick => tick + 1), Math.min(...waits) + 50);
		return () => clearTimeout(timer);
	}, [key]);
	// What was last asked by a person, which the line answers.
	const asked = [...(thread.posts ?? [])].reverse().find(post => findMember(post.authorId)?.kind != "ai_colleague");
	const request = asked != null
		? { id: asked.id, text: (asked.paragraphs ?? []).join("\n\n"), hasFiles: (asked.realFiles ?? asked.files ?? []).length > 0 }
		: { id: thread.id, text: thread.title ?? "" };
	return memberIds
		.filter(id => Date.now() - (since.current.get(id) ?? Date.now()) >= HOLD_AFTER_MS)
		.map(id => findMember(id))
		.filter(Boolean)
		.map(member => {
			const at = new Date((since.current.get(member.id) ?? Date.now()) + HOLD_AFTER_MS).toISOString();
			return (
				<li key={`holding-${member.id}`} className={styles.post} aria-live="polite">
					<span className={styles.face}><Avatar member={member} size={36} /></span>
					<div className={styles.body}>
						<p className={styles.post_head}>
							<span className={styles.name}>{member.name}</span>
							<time className={styles.time} dateTime={at}>{formatMoment(at)}</time>
						</p>
						<div className={styles.text}><p>{holdingLine(request, member.id)}</p></div>
					</div>
				</li>
			);
		});
}

function Event({ event, previous, thread, state, onDecide, actions }) {
	if (event.kind == "closed") {
		const closer = findMember(event.byId);
		// The thread's latest closing, while it holds, can be undone with a press.
		const closings = threadEvents(thread, state).filter(item => item.kind == "closed");
		const isStanding = isThreadClosed(thread, state) && closings[closings.length - 1]?.id == event.id;
		return (
			<li className={styles.closed}>
				<span className={styles.closed_mark}><Icon name="check" size={14} /></span>
				<div className={styles.closed_body}>
					<p className={styles.closed_head}>{event.byId == viewerId ? "You" : closer.name} closed the thread <time className={styles.time} dateTime={event.at}>{formatMoment(event.at)}</time></p>
					<p className={styles.closed_text} data-selectable="">{event.conclusion}</p>
				</div>
				{isStanding ? <Button size="small" onClick={actions.onReopen}>Reopen</Button> : null}
			</li>
		);
	}
	if (event.kind == "reopened") {
		return <li className={styles.reopened}>{event.byId == viewerId ? "You" : firstName(findMember(event.byId))} reopened the thread.</li>;
	}

	return <Post event={event} previous={previous} thread={thread} state={state} onDecide={onDecide} {...actions} />;
}

// The reactions a message takes in one press.
const QUICK_REACTIONS = ["👍", "✅", "👀", "🙏", "❓"];

// A message and what a member can do with it (the owner, 2026-10-24: "I must
// be able to do something with messages and interact with them"). Under the
// pointer it lightens and shows its tools: react, reply with it quoted, and
// more, which a right-click opens as well: copy its text, mark the thread
// unread from here, and for the member's own, edit or delete it. Its
// reactions stand under it; a press on one adds the member's or takes it back.
function Post({ event, previous, thread, state, onDecide, onReact, onQuote, onEditPost, onDeletePost, onMarkUnread, onAnswer, onReturnReview, onReturnWritten }) {
	// Read as an AI provider receives it, from the thread's corner (D-233).
	const isProviderView = useProviderView(thread.real?.chatId);
	const { open } = useNavigation();
	const author = findMember(event.authorId);
	// In onboarding's demonstration a post shows the files the scene gives it to
	// show (D-162), and the deal's two of the first page.
	const shownFiles = isEmulation ? event.files?.filter(file => file.isShownInDemo == true || ["DD workplan.xlsx", "Closing memorandum.docx"].includes(file.name)) : event.files;
	const work = event.workProductId != null ? workProducts[event.workProductId] : null;
	const isOwn = event.authorId == viewerId;
	const isMore = previous?.kind == "post" && previous.authorId == event.authorId && new Date(event.at) - new Date(previous.at) < 10 * 60000 && event.quote == null;
	const decision = event.decisionId != null ? decisions[event.decisionId] : null;
	const [isEditing, setEditing] = useState(false);
	const [isShowingHistory, setShowingHistory] = useState(false);
	const [picker, setPicker] = useState(null);
	const menu = useMenu();
	// The same tools for every message, a person's or a colleague's (the owner,
	// 2026-10-24). In a real thread the backend keeps the reactions, the edit
	// and the deletion, and the rule is its own: only a message's author changes
	// it, on the computer that wrote it.
	const isReal = event.isReal == true;
	const chatId = thread.real?.chatId;
	// The assignment whose card stands under this message, and the decisions
	// it puts to the lawyer (D-120).
	const assignment = isReal ? assignmentOfPost(event) : null;
	const decided = isReal ? decisionsOfPost(event) : [];
	// A draft the assignment's card shows as a result is shown there once, not
	// again as a file of the message (D-174).
	const cardDocuments = assignment != null ? resultDocumentIdsOf(assignment) : new Set();
	const shownRealFiles = (event.realFiles ?? []).filter(file => cardDocuments.has(file.documentId) == false);
	const reactionsOf = isReal ? (event.realReactions ?? {}) : (state.reactions[event.id] ?? {});
	const canChange = isReal ? event.canChange == true : isOwn;
	const isGone = event.isDeleted == true;

	function react(emoji) {
		if (isReal == false) {
			onReact(event.id, emoji);
			return;
		}
		reactToRealPost(chatId, event.id, emoji).catch(error => toast(error?.message ?? "The reaction was not kept.", { tone: "seal" }));
	}

	function copy() {
		const selection = window.getSelection();
		const node = document.getElementById(`post-${event.id}`);
		const selected = selection != null && selection.isCollapsed == false && node?.contains(selection.anchorNode) ? selection.toString() : "";
		navigator.clipboard?.writeText(selected || event.paragraphs.join("\n\n")).then(() => toast("Copied.")).catch(() => {});
	}

	function remove() {
		if (isReal) {
			const undo = deleteRealPostWithUndo(chatId, event.id, error => toast(error?.message ?? "The message was not deleted.", { tone: "seal" }));
			toast("Message deleted.", { action: { label: "Undo", run: undo } });
			return;
		}
		onDeletePost(event.id);
		toast("Message deleted.", { action: { label: "Undo", run: () => onDeletePost(event.id, false) } });
	}

	const items = [
		isGone ? null : { key: "react", label: "React", icon: "smile", submenu: QUICK_REACTIONS.map(emoji => ({ key: emoji, label: emoji, checked: (reactionsOf[emoji] ?? []).includes(viewerId), onSelect: () => react(emoji) })) },
		isGone ? null : { key: "reply", label: "Reply", icon: "reply", onSelect: () => onQuote(event) },
		isGone ? null : { key: "copy", label: "Copy text", icon: "copy", onSelect: copy },
		// How the work was done, kept off the page (D-238).
		isReal && isGone == false && assignment != null ? { key: "history", label: "Work history", icon: "history", onSelect: () => setShowingHistory(true) } : null,
		{ key: "unread", label: "Mark unread from here", icon: "inbox", onSelect: () => { onMarkUnread(thread.id); toast("Marked unread from here."); } },
		canChange && isGone == false ? { separator: true } : null,
		canChange && isGone == false ? { key: "edit", label: "Edit", icon: "edit", onSelect: () => setEditing(true) } : null,
		canChange && isGone == false ? { key: "delete", label: "Delete", icon: "trash", tone: "seal", onSelect: remove } : null,
	];

	function onContextMenu(pointer) {
		if (isEditing || pointer.target.closest?.("button, textarea, input")) return;
		pointer.preventDefault();
		const x = pointer.clientX;
		const y = pointer.clientY;
		menu.open({
			getBoundingClientRect: () => ({ top: y, bottom: y, left: x, right: x, width: 0, height: 0 }),
			contains: () => false,
			focus: () => {},
		});
	}

	return (
		<li id={`post-${event.id}`} data-demo-hidden={isEmulation && event.demoHidden || undefined} className={[styles.post, isMore ? styles.post_more : null, menu.isOpen || picker != null ? styles.post_active : null].filter(Boolean).join(" ")} onContextMenu={onContextMenu}>
			<span className={styles.face}>{isMore ? null : <PersonButton memberId={author.id} label={author.name}><Avatar member={author} size={36} /></PersonButton>}</span>
			<div className={styles.body}>
				{isMore ? null : (
					<p className={styles.post_head}>
						<PersonButton memberId={author.id} className={styles.name_button}><span className={styles.name}>{isOwn && isEmulation == false ? "You" : author.name}</span></PersonButton>
						<time className={styles.time} dateTime={event.at}>{formatMoment(event.at)}</time>
						{event.isEdited ? <span className={styles.edited}>edited</span> : null}
					</p>
				)}
				{event.quote != null ? <Quote quote={event.quote} /> : null}
				{event.place != null && isGone == false ? <PlaceQuote place={event.place} /> : null}
				{isEditing ? (
					<PostEditor
						initial={event.paragraphs.join("\n\n")}
						onCancel={() => setEditing(false)}
						onSave={text => {
							if (isReal) {
								editRealPost(chatId, event.id, text).catch(error => toast(error?.message ?? "The edit was not kept.", { tone: "seal" }));
							} else {
								onEditPost(event.id, text);
							}
							setEditing(false);
						}}
					/>
				) : (
					isGone ? <p className={styles.deleted_note}>This message was deleted.</p> : (
						<div className={styles.text} data-selectable="">
							{isProviderView
								? <MaskedText projectId={protectionProjectOfChat(chatId)} text={event.paragraphs.join("\n\n")} />
								: <MessageText text={event.paragraphs.join("\n\n")} />}
						</div>
					)
				)}
				{isReal && isGone == false && shownRealFiles.length > 0 ? <RealFiles files={shownRealFiles} notChecked={event.notChecked} /> : null}
				{event.failure != null ? <p className={styles.stopped}>{event.failure.next}</p> : null}
				{event.reviewId != null && isGone == false ? <PostReview post={event} thread={thread} onReturn={onReturnReview} /> : null}
				{isReal && isGone == false && assignment != null ? <AssignmentBlock assignment={assignment} onReturn={onReturnReview} onReturnWritten={onReturnWritten} /> : null}
				{isShowingHistory && assignment != null ? (
					<Dialog title="Work history" onClose={() => setShowingHistory(false)}
						actions={<Button variant="primary" onClick={() => setShowingHistory(false)}>Done</Button>}>
						<AssignmentDetails assignment={assignment} />
					</Dialog>
				) : null}
				{isReal && isGone == false && decided.length > 0 ? <DecisionList items={decided} /> : null}
				{isReal && isGone == false && (event.heldFor ?? []).length > 0 ? <HeldNote post={event} author={author} /> : null}
				{work != null ? (
					<Result
						files={work.files ?? []}
						notChecked={work.notChecked}
						replacedWord={work.state.startsWith("Replaced") ? `Superseded by version ${work.version + 1}` : null}
						onOpenFile={work.documentId != null ? () => open({ kind: "document", documentId: work.documentId }) : undefined}
					/>
				) : shownFiles?.length ? <Result files={shownFiles} notChecked={event.notChecked} /> : null}
				{decision != null
					? <Answers thread={thread} decisionId={event.decisionId} decision={decision} choice={state.choices[event.decisionId]} quote={{ postId: event.id, authorId: event.authorId, text: decision.question }} onDecide={onDecide} />
					: event.ask != null && isGone == false ? (
						<AskAnswers
								options={event.ask.options}
								answer={isReal ? event.realAnswer ?? null : state.answers[event.id]}
								isOwn={isOwn}
								onAnswer={option => {
									if (isReal == false) {
										onAnswer(event.id, option, { threadId: thread.id, quote: { postId: event.id, authorId: event.authorId, text: (event.paragraphs ?? []).join(" ") } });
										return;
									}
									postToThread(chatId, option, { quoteMessageId: event.id, answerOption: option })
										.catch(error => toast(error?.message ?? "The answer was not sent.", { tone: "seal" }));
								}}
							/>
						) : null}
				{isEmulation && event.demoReactionSpace ? <div className={styles.demonstrationReaction}><Reactions reactions={isGone ? {} : reactionsOf} viewerId={viewerId} resolveMember={findMember} onReact={react} /></div> : <Reactions reactions={isGone ? {} : reactionsOf} viewerId={viewerId} resolveMember={findMember} onReact={react} onAdd={event => setPicker(event.currentTarget)} />}
			</div>
			{isEditing ? null : (
				<div className={styles.tools} role="toolbar" aria-label="Message">
					{isGone ? null : <button className={styles.tool} type="button" aria-label="React" data-tooltip="React" onClick={pointer => setPicker(pointer.currentTarget)}><Icon name="smile" size={18} /></button>}
					{isGone ? null : <button className={styles.tool} type="button" aria-label="Reply" data-tooltip="Reply" onClick={() => onQuote(event)}><Icon name="reply" size={18} /></button>}
					<button className={styles.tool} type="button" aria-label="More" data-tooltip="More" aria-haspopup="menu" aria-expanded={menu.isOpen} onClick={pointer => menu.toggle(pointer.currentTarget)}><Icon name="more" size={18} /></button>
				</div>
			)}
			{picker != null ? (
				<Popover anchor={picker} onClose={() => setPicker(null)} placement="bottom-end" width={208} label="React">
					<div className={styles.picker}>
						{QUICK_REACTIONS.map(emoji => (
							<button
								key={emoji}
								className={styles.pick}
								type="button"
								aria-label={`React with ${emoji}`}
								onClick={() => {
									react(emoji);
									setPicker(null);
								}}
							>
								{emoji}
							</button>
						))}
					</div>
				</Popover>
			) : null}
			{menu.isOpen ? <Menu anchor={menu.anchor} label="Message" minWidth={232} onClose={menu.close} items={items} /> : null}
		</li>
	);
}

// Under a message that names a colleague running on this computer, sent from
// a computer the person here has not confirmed as its author's: the colleague
// does not take it up until they compare that computer's code with its owner
// and confirm it (D-123, Chat/MessageProvenance). Confirming it once is for
// everything the author asks from it.
function HeldNote({ post, author }) {
	const [computers, setComputers] = useState(null);
	const [isReading, setReading] = useState(false);
	// The computer being confirmed, by its id.
	const [isConfirming, setConfirming] = useState(null);
	const colleagues = namesJoined(post.heldFor.map(colleague => colleague.name));
	const who = firstName(author);

	async function compare() {
		setReading(true);
		try {
			setComputers(await unconfirmedComputersOf(post.authorActorId));
		} catch (error) {
			toast(error?.message ?? `${who}'s computers could not be read.`, { tone: "seal" });
		} finally {
			setReading(false);
		}
	}

	// One computer at a time, by the code its owner reads out: a computer that
	// only claims to be theirs is never confirmed along with theirs.
	async function confirm(computer) {
		setConfirming(computer.deviceId);
		try {
			await confirmMemberComputer(post.authorActorId, computer.deviceId);
			toast(`${colleagues} now takes up what ${who} asks from that computer.`);
			setComputers(null);
		} catch (error) {
			toast(error?.message ?? "The computer was not confirmed.", { tone: "seal" });
		} finally {
			setConfirming(null);
		}
	}

	return (
		<>
			<div className={styles.held}>
				<p>{colleagues} takes this up once you confirm it came from {author.name}'s computer.</p>
				<Button size="small" busy={isReading} onClick={compare}>Compare codes</Button>
			</div>
			{computers != null ? (
				<Dialog
					title={`Is this ${author.name}'s computer?`}
					description={computers.length == 0
						? `Singularity has no computer of ${who}'s to confirm yet. Ask ${who} to open the Matter, then try again.`
						: `Ask ${who} for their computer's code: it is under their name, at the foot of their Singularity window. If it matches, ${colleagues} takes up what ${who} asks from that computer, here and from now on.`}
					onClose={() => setComputers(null)}
					actions={<Button disabled={isConfirming != null} onClick={() => setComputers(null)}>{computers.length == 0 ? "Close" : "Not now"}</Button>}
				>
					{computers.length > 0 ? (
						<ul className={styles.codes}>
							{computers.map(computer => (
								<li key={computer.deviceId} className={styles.code_row}>
									<strong className={styles.code}>{computer.code}</strong>
									<Button variant="primary" size="small" busy={isConfirming == computer.deviceId} disabled={isConfirming != null && isConfirming != computer.deviceId} onClick={() => confirm(computer)}>It matches</Button>
								</li>
							))}
						</ul>
					) : null}
				</Dialog>
			) : null}
		</>
	);
}

// "Unicorn", "Unicorn and Kitsune", "Unicorn, Kitsune and Fox".
function namesJoined(names) {
	if (names.length <= 1) return names[0] ?? "";
	return `${names.slice(0, -1).join(", ")} and ${names[names.length - 1]}`;
}

// The message a reply answers, over its words; a press goes to it.
function Quote({ quote }) {
	const author = findMember(quote.authorId);
	return (
		<button className={styles.quote} type="button" onClick={() => document.getElementById(`post-${quote.postId}`)?.scrollIntoView({ behavior: "smooth", block: "center" })}>
			<span className={styles.quote_sign}><Icon name="reply" size={14} /></span>
			<span className={styles.quote_who}>{author.id == viewerId ? "You" : author.name}</span>
			<span className={styles.quote_text}>{plainOf(quote.text)}</span>
		</button>
	);
}

// The place in a document a message is about, over its words, as Word shows
// a comment's: the section and the document, then the words there. A press
// goes to it - in Word, where it is connected here (Office.json).
function PlaceQuote({ place }) {
	const { open } = useNavigation();
	// Cells of a workbook read as cells, with their sheet (D-110); a slide of a
	// deck as that slide, with its title (D-112).
	const isWorkbook = /\.(xlsx|xlsm|xls|csv)$/i.test(place.documentName ?? "");
	const isDeck = /\.(pptx|ppt)$/i.test(place.documentName ?? "");
	const where = isWorkbook && place.section
		? `${place.section.includes(":") ? "Cells" : "Cell"} ${place.section}${place.heading ? `, ${place.heading}` : ""}`
		: isDeck && place.section ? `${place.section}${place.heading ? `, ${place.heading}` : ""}`
		: place.section ? `Section ${place.section}${place.heading ? ` ${place.heading}` : ""}` : "A passage";
	const document = (place.documentName || "the document").replace(/\.[^.]+$/, "");
	return (
		<button
			className={styles.place}
			type="button"
			data-tooltip="Show it in the document"
			onClick={() => showPlace(place, open).catch(error => toast(error?.message ?? "The place could not be shown.", { tone: "seal" }))}
		>
			<span className={styles.quote_sign}><Icon name="document" size={14} /></span>
			<span className={styles.place_head}><span className={styles.place_where}>{where}</span> in {document}</span>
			{place.quote ? <span className={styles.place_text}>“{place.quote}”</span> : null}
		</button>
	);
}

// The member's own message, edited where it stands: Ctrl+Enter saves it,
// Escape leaves it as it was.
function PostEditor({ initial, onSave, onCancel }) {
	const [value, setValue] = useState(initial);
	return (
		<div className={styles.editor}>
			<TextArea
				className={styles.editor_field}
				minRows={2}
				maxRows={12}
				autoFocus
				aria-label="Edit the message"
				value={value}
				onChange={change => setValue(change.target.value)}
				onKeyDown={key => {
					if (key.key == "Escape") {
						key.preventDefault();
						onCancel();
					} else if (key.key == "Enter" && (key.ctrlKey || key.metaKey) && value.trim() != "") {
						key.preventDefault();
						onSave(value);
					}
				}}
			/>
			<div className={styles.editor_actions}>
				<Button size="small" onClick={onCancel}>Cancel</Button>
				<Button size="small" variant="primary" disabled={value.trim() == ""} onClick={() => onSave(value)}>Save</Button>
			</div>
		</div>
	);
}

// The answers to what a colleague asks, under its words. Once one is pressed
// they go: the answer stands as the member's reply under them.
function Answers({ thread, decisionId, decision, choice, quote, onDecide }) {
	if (choice != null) return null;
	return (
		<div className={styles.answers}>
			{decision.options.map(option => (
				<Button
					key={option.key}
					variant={option.key == "A" ? "primary" : "secondary"}
					onClick={() => {
						onDecide(thread.id, decisionId, option.key, quote);
						toast(option.result, { tone: option.key == "A" ? "settled" : undefined });
					}}
				>
					{option.text}
				</Button>
			))}
		</div>
	);
}

// The reply. Its words name whom it reaches; Enter sends it, Shift+Enter starts
// a new line (the owner, 2026-11-02).
// Closing the thread is a mode of its own: what is written becomes the
// conclusion, and the send closes the thread.
function Composer({ isClosed, isReal, isEmpty, names, mentionables, matterId, chatId, quote, onClearQuote, onSend, onClose, returning, onStopReturning, onReturn }) {
	// Read as an AI provider receives it: the draft too, as it would go out,
	// before it is sent (D-233).
	const isProviderView = useProviderView(chatId);
	const [draft, setDraft] = useState("");
	const fieldRef = useRef(null);
	const handIn = useResultDraft();
	const [isClosing, setClosing] = useState(false);
	const [isSending, setSending] = useState(false);
	const isReturning = returning != null;

	// Replying to a message, or returning a version, takes the member to the
	// reply box.
	useEffect(() => {
		if (quote != null || returning != null) fieldRef.current?.focus();
	}, [quote, returning]);
	async function send() {
		const text = draft.trim();
		if (isSending) return;
		if (isReturning) {
			if (text == "") return;
			setSending(true);
			const isDone = await onReturn(text);
			setSending(false);
			if (isDone) setDraft("");
			return;
		}
		if (isClosing) {
			if (text == "") return;
			onClose(text);
			setDraft("");
			setClosing(false);
			return;
		}
		if (text == "" && !handIn.hasFiles) return;
		setSending(true);
		// Words alone leave the box at once, as they show in the thread at once
		// (data/native.js); if they could not be posted they come back to it.
		const isWordsAlone = isReal == true && handIn.hasFiles == false;
		const kept = draft;
		if (isWordsAlone) setDraft("");
		const isDone = await onSend(text, handIn.extras());
		setSending(false);
		if (isDone) {
			setDraft("");
			handIn.reset();
		} else if (isWordsAlone) {
			setDraft(current => current == "" ? kept : current);
		}
	}

	// No one in the thread yet but the member: they write to the Matter's
	// people, whom "@" names (a new thread read "Write to " with no name).
	const to = names.length > 1 ? `${names.slice(0, -1).join(", ")} and ${names[names.length - 1]}`
		: names.length == 1 ? names[0] : "the Matter's team: @ names who it is for";

	return (
		<div className={styles.composer} data-print="hide" data-reply-box="">
			{quote != null ? (
				<div className={styles.composer_quote}>
					<span className={styles.composer_quote_text}>
						<span className={styles.composer_quote_who}>Replying to {quote.authorId == viewerId ? "yourself" : findMember(quote.authorId).name}</span>
						{" "}{plainOf(quote.text)}
					</span>
					<button className={styles.composer_quote_close} type="button" aria-label="Stop replying to it" onClick={onClearQuote}><Icon name="close" size={14} /></button>
				</div>
			) : null}
			{isClosing ? (
				<div className={styles.composer_quote}>
					<span className={styles.composer_quote_text}><span className={styles.composer_quote_who}>Closing the thread.</span> What you write is its conclusion.</span>
					<button className={styles.composer_quote_close} type="button" aria-label="Keep the thread open" onClick={() => setClosing(false)}><Icon name="close" size={14} /></button>
				</div>
			) : null}
			{isReturning ? (
				<div className={styles.composer_quote}>
					<span className={styles.composer_quote_text}><span className={styles.composer_quote_who}>Returning {returning.what} to {returning.agentName}.</span> What you write goes back with it.</span>
					<button className={styles.composer_quote_close} type="button" aria-label="Keep it as it is" onClick={onStopReturning}><Icon name="close" size={14} /></button>
				</div>
			) : null}
			{isClosing ? null : <DraftTray draft={handIn} />}
			<ComposerBox
				ref={fieldRef}
				value={draft}
				onChange={setDraft}
				onSubmit={send}
				above={isProviderView && isReal ? <MaskedDraft projectId={protectionProjectOfChat(chatId)} text={draft} /> : undefined}
				canSend={isSending ? false : isClosing || isReturning ? draft.trim() != "" : draft.trim() != "" || handIn.hasFiles}
				sendsOnEnter
				mentionables={mentionables}
				allowsAll={mentionables.length > 1}
				allLine="Everyone in this thread"
				placeholder={isReturning ? `What should ${returning.agentName} change?` : isClosing ? "Write the conclusion" : isClosed ? "Reply to reopen the thread" : isEmpty ? `Write to ${to}` : `Reply to ${to}`}
				label="Reply"
				tools={isReturning ? null : (
					<AddButton
						matterId={matterId}
						draft={handIn}
						real={isReal ? { documents: realDocuments(), onFromComputer: addDocumentFromComputer } : null}
						extra={isClosed || isClosing ? [] : [{ key: "close", label: "Close the thread", icon: "check", onSelect: () => { setClosing(true); fieldRef.current?.focus(); } }]}
					/>
				)}
				submit={isReturning ? <ComposerAction disabled={draft.trim() == "" || isSending} tooltip={`Return ${returning.what}`} onClick={send}>Return</ComposerAction>
					: isClosing ? <ComposerAction disabled={draft.trim() == ""} tooltip="Close the thread" onClick={send}>Close thread</ComposerAction> : undefined}
			/>
		</div>
	);
}
