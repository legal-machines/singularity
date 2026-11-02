import { useRef, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { ComposerBox } from "../../components/Composer/ComposerBox.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { ProjectBand } from "../../components/ProjectMark/ProjectMark.jsx";
import { Result } from "../../components/Result/Result.jsx";
import { AddButton, DraftTray, useResultDraft } from "../../components/Result/ResultDraft.jsx";
import { MovingHead } from "../../components/MovingHead/MovingHead.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { findMail } from "../../data/mail.js";
import { nowLocal } from "../../data/clock.js";
import { viewerId } from "../../data/workspace.js";
import { findMatter, findMember, firstName, formatMoment } from "../words.js";

import thread from "../Thread/ThreadView.module.css";
import styles from "./MailPage.module.css";

// A message from the member's Outlook, filed to a project because its sender
// is a contact recorded on it (the owner, 2026-10-24). It reads as a letter:
// who wrote and to whom, the words and the files. A reply goes back to the
// sender through Outlook.
export function MailPage() {
	const { place, open } = useNavigation();
	const message = findMail(place.mailId);
	const [replies, setReplies] = useState([]);
	const scrollRef = useRef(null);
	const placeholderRef = useRef(null);
	if (message == null) return null;

	const matter = findMatter(message.matterId);
	const contact = message.contact;
	const viewer = findMember(viewerId);
	const copied = message.cc.map(findMember);

	return (
		<div className={styles.page}>
			<SheetBar />
			<div className={thread.view}>
				<div ref={scrollRef} className={thread.scroll} data-fade-edge="both">
					<article className={thread.thread}>
						<header ref={placeholderRef} className={`${thread.head} ${thread.head_room}`} aria-hidden="true">
							<p className={thread.title} data-part="title">{message.subject}</p>
							<div className={styles.where}>
								<span data-part="band"><ProjectBand matter={matter} /></span>
							</div>
						</header>

						<section className={styles.letter} aria-label={`From ${contact.name}`}>
							<div className={styles.from}>
								<Avatar member={contact} size={36} />
								<div className={styles.from_text}>
									<p className={styles.from_head}>
										<span className={styles.name}>{contact.name}</span>
										<span className={styles.address} data-selectable="">{contact.email}</span>
										<time className={styles.time} dateTime={message.at}>{formatMoment(message.at)}</time>
									</p>
									<p className={styles.from_line}>
										{contact.organisation}, {contact.role}. To you{copied.length > 0 ? `, a copy to ${copied.map(member => member.name).join(" and ")}` : ""}.
									</p>
								</div>
							</div>

							<div className={`${thread.text} ${styles.words}`} data-selectable="">
								{message.greeting != "" ? <p>{message.greeting}</p> : null}
								{message.paragraphs.map(paragraph => <p key={paragraph}>{paragraph}</p>)}
								{message.signoff.length > 0 ? <p>{message.signoff.map((line, index) => <span key={line} className={styles.signoff_line}>{line}{index < message.signoff.length - 1 ? <br /> : null}</span>)}</p> : null}
							</div>

							{message.attachments.length > 0 ? <div className={styles.files}><Result files={message.attachments} /></div> : null}
						</section>

						{replies.map(reply => (
							<section key={reply.at + reply.text} className={styles.reply} aria-label="Your reply">
								<Avatar member={viewer} size={36} />
								<div className={styles.from_text}>
									<p className={styles.from_head}>
										<span className={styles.name}>You</span>
										<span className={styles.time}>Sent by email, {formatMoment(reply.at)}</span>
									</p>
									{reply.text != "" ? (
										<div className={thread.text} data-selectable="">
											<p>{reply.text}</p>
										</div>
									) : null}
									{reply.files != null ? <Result files={reply.files} /> : null}
								</div>
							</section>
						))}
					</article>
				</div>
			</div>
			<MovingHead
				key={`head-${message.id}`}
				scrollRef={scrollRef}
				placeholderRef={placeholderRef}
				title={message.subject}
				band={<ProjectBand matter={matter} onClick={() => open({ kind: "matter", matterId: matter.id, tab: "overview" })} />}
				onTop={() => scrollRef.current?.scrollTo({ top: 0, behavior: "smooth" })}
			/>
			<Reply key={message.id} name={firstName(contact)} matterId={message.matterId} onSend={(text, files) => setReplies(previous => [...previous, { text, files, at: nowLocal() }])} />
		</div>
	);
}

// The reply box: it answers the sender by email, with files if need be;
// Ctrl+Enter sends.
function Reply({ name, matterId, onSend }) {
	const [draft, setDraft] = useState("");
	const handIn = useResultDraft();
	function send() {
		const text = draft.trim();
		if (text == "" && !handIn.hasFiles) return;
		onSend(text, handIn.extras().files);
		setDraft("");
		handIn.reset();
	}

	return (
		<div className={thread.composer} data-print="hide">
			<DraftTray draft={handIn} />
			<ComposerBox
				value={draft}
				onChange={setDraft}
				onSubmit={send}
				canSend={draft.trim() != "" || handIn.hasFiles}
				placeholder={`Reply to ${name} by email`}
				label="Reply by email"
				tools={<AddButton matterId={matterId} draft={handIn} />}
			/>
		</div>
	);
}
