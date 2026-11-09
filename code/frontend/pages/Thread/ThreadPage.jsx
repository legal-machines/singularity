import { useRef, useState } from "react";
import { isEmulation } from "../../emulation/emulation.js";

import { Button } from "../../components/Button/Button.jsx";
import { Faces } from "../../components/Faces/Faces.jsx";
import { ConfirmDialog, MoreButton, RenameThreadDialog } from "../../components/Manage/Manage.jsx";
import { toast } from "../../components/Toast/toast.js";
import { MovingHead } from "../../components/MovingHead/MovingHead.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Page, PageHead } from "../../components/Page/Page.jsx";
import { ProjectBand } from "../../components/ProjectMark/ProjectMark.jsx";
import { ProviderViewButton } from "../../components/ProviderView/ProviderView.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { ShareWorkLogDialog, useCanShareWorkLog } from "../../components/WorkLog/ShareWorkLog.jsx";
import { deleteThread, renameThread, setThreadArchived, useRealWork } from "../../data/native.js";
import { findThread } from "../threadState.js";
import { findMatter, findMember } from "../words.js";
import { ThreadView } from "./ThreadView.jsx";

import styles from "./ThreadPage.module.css";

// A thread on a page of its own. Its title, its project's band and its faces
// travel into the window's band as it scrolls, and come back down at its top;
// a press on the docked title brings the thread back to its top.
export function ThreadPage({ state, onDecide, onReply, onReact, onEditPost, onDeletePost, onMarkUnread, onAnswer, onCloseThread, onReopenThread }) {
	const { place, open } = useNavigation();
	const real = useRealWork();
	const scrollRef = useRef(null);
	const placeholderRef = useRef(null);
	const pageRef = useRef(null);
	const thread = findThread(place.threadId);
	// A thread that is not here: a real one still being read from this
	// computer, or one that was removed. The page says which, and the way on.
	if (thread == null) {
		const isReading = ["connecting"].includes(real.status);
		return (
			<Page bar={<SheetBar />}>
				<PageHead
					title={isReading ? "Opening the thread" : "This thread is not here"}
					line={isReading
						? "Singularity is reading the Matter's threads on this computer."
						: "It may have been removed, or it belongs to a Matter this computer has not opened. Your other threads are in the Inbox."}
					actions={isReading ? null : <Button variant="primary" onClick={() => open({ kind: "inbox" })}>Go to the Inbox</Button>}
				/>
			</Page>
		);
	}
	const matter = findMatter(thread.matterId);
	// A real thread is renamed where its title stands; one just begun opens
	// with its name to write.
	const isReal = thread.isReal == true;
	const isUnnamed = isReal && (thread.posts ?? []).length == 0 && ["New thread", "New chat"].includes(thread.title);
	const hasColleague = isReal && thread.real?.chatId != null && (thread.participantIds ?? []).some(id => findMember(id)?.kind == "ai_colleague");
	async function rename(title) {
		try {
			await renameThread(thread.id, title);
		} catch (error) {
			toast(error?.message ?? "The thread could not be renamed. Try again.", { tone: "seal" });
		}
	}

	return (
		<div ref={pageRef} className={styles.page}>
			{!isEmulation ? <SheetBar actions={thread.isReal == true ? (
				<>
					{/* With an AI colleague in it, the thread can be read as an AI provider receives it. */}
					{hasColleague ? <ProviderViewButton chatId={thread.real?.chatId} /> : null}
					<ThreadMenu thread={thread} matter={matter} />
				</>
			) : null} /> : null}
			<ThreadView key={thread.id} threadId={thread.id} state={state} onDecide={onDecide} onReply={onReply} onReact={onReact} onEditPost={onEditPost} onDeletePost={onDeletePost} onMarkUnread={onMarkUnread} onAnswer={onAnswer} onCloseThread={onCloseThread} onReopenThread={onReopenThread} scrollRef={scrollRef} placeholderRef={placeholderRef} />
			{!isEmulation ? <MovingHead
				key={`head-${thread.id}`}
				scrollRef={scrollRef}
				placeholderRef={placeholderRef}
				title={thread.title}
				band={<ProjectBand matter={matter} onClick={() => open({ kind: "matter", matterId: matter.id, tab: "overview" })} />}
				faces={<Faces people={thread.participantIds.map(findMember)} size={22} />}
				onTop={() => scrollRef.current?.scrollTo({ top: 0, behavior: "smooth" })}
				onRename={isReal ? rename : null}
				isNaming={isUnnamed}
				// Named, the member writes on: the reply box.
				onNamed={() => pageRef.current?.querySelector("textarea")?.focus()}
			/> : null}
		</div>
	);
}

// The thread's messages as the member sees them, for its work log.
function messagesOf(thread) {
	return (thread.posts ?? []).map(post => ({
		author: findMember(post.authorId ?? post.byId)?.name ?? "",
		at: post.at ?? post.createdAt ?? "",
		text: Array.isArray(post.paragraphs) ? post.paragraphs.join("\n\n") : post.text ?? "",
	}));
}

// What the member may do to the thread: rename it, archive it apart from the
// Matter's threads or bring it back, or delete it, which its notice can
// still undo; its words are never erased. On a design partner's seat it also
// sends the thread's work log to Legal Machines.
function ThreadMenu({ thread, matter }) {
	const { open } = useNavigation();
	const [asking, setAsking] = useState(null);
	// The log is the AI models' record: a thread of people alone has none, so
	// the option is not there.
	const canShareWorkLog = useCanShareWorkLog() && thread.real?.chatId != null
		&& (thread.participantIds ?? []).some(id => findMember(id)?.kind == "ai_colleague");
	const backToMatter = () => open({ kind: "matter", matterId: matter.id, tab: "threads" });

	async function archive(archived) {
		try {
			await setThreadArchived(thread.id, archived);
			if (archived) {
				backToMatter();
				toast(`${thread.title} is archived.`, { action: { label: "Undo", run: () => setThreadArchived(thread.id, false) } });
			} else {
				toast(`${thread.title} is back among the threads.`);
			}
		} catch (error) {
			toast(error?.message ?? "The thread could not be changed. Try again.", { tone: "seal" });
		}
	}

	return (
		<>
			<MoreButton label={`More for ${thread.title}`} items={[
				{ key: "rename", label: "Rename", icon: "edit", onSelect: () => setAsking("rename") },
				thread.isArchived == true
					? { key: "restore", label: "Restore", icon: "return", onSelect: () => archive(false) }
					: { key: "archive", label: "Archive", icon: "closed", onSelect: () => archive(true) },
				{ key: "delete", label: "Delete", icon: "trash", onSelect: () => setAsking("delete") },
				canShareWorkLog ? { key: "workLog", label: "Send work log to Legal Machines", icon: "send", onSelect: () => setAsking("workLog") } : null,
			].filter(Boolean)} />
			{asking == "workLog" ? (
				<ShareWorkLogDialog matterId={thread.real.matterId} chatId={thread.real.chatId} title={thread.title} messages={messagesOf(thread)}
					onClose={() => setAsking(null)} />
			) : null}
			{asking == "rename" ? (
				<RenameThreadDialog title={thread.title} onClose={() => setAsking(null)}
					onSave={async title => {
						await renameThread(thread.id, title);
						toast("The thread is renamed.");
					}} />
			) : null}
			{asking == "delete" ? (
				<ConfirmDialog
					title={`Delete ${thread.title}?`}
					description="It leaves the Matter's threads for everyone on it. What was written in it and the work it led to stay in the Matter's history."
					confirm="Delete"
					onClose={() => setAsking(null)}
					onConfirm={async () => {
						const undo = await deleteThread(thread.id);
						backToMatter();
						toast(`${thread.title} is deleted.`, { action: { label: "Undo", run: undo } });
					}} />
			) : null}
		</>
	);
}
