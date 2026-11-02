import { useRef } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { MovingHead } from "../../components/MovingHead/MovingHead.jsx";
import { ProjectBand } from "../../components/ProjectMark/ProjectMark.jsx";
import { Result } from "../../components/Result/Result.jsx";
import { sizeWords } from "../../components/Review/Review.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { toast } from "../../components/Toast/toast.js";
import { realDocuments, realLetter, realWork, useRealWork } from "../../data/native.js";
import { findMatter, formatMoment } from "../words.js";

import thread from "../Thread/ThreadView.module.css";
import styles from "./MailPage.module.css";

// A letter filed to the Matter (D-106), as the prototype's mail reads: who
// wrote and to whom, the words and the files, under the Matter's band - and
// how it came to be filed: by whom, from where, on what evidence. It is the
// Matter's correspondence, apart from its threads; a reply goes from Outlook,
// and Singularity sends nothing.
export function LetterPage() {
	const { place, open } = useNavigation();
	useRealWork();
	const letter = realLetter(place.letterId);
	const scrollRef = useRef(null);
	const placeholderRef = useRef(null);
	if (letter == null) return <div className={styles.page}><SheetBar /><p className={styles.from_line}>This letter is no longer filed in the Matter.</p></div>;

	const matter = findMatter(realWork().uiMatterId ?? "x_sablewood");
	const isOutgoing = letter.direction == "outgoing";
	const author = contactOf(letter.from);
	const named = parties => parties.map(party => party.name || party.address).filter(Boolean);
	const to = named(letter.to);
	const cc = named(letter.cc);
	const files = letter.attachments.map(attachment => ({ ...attachment, size: sizeWords(attachment.size) }));

	return (
		<div className={styles.page}>
			<SheetBar />
			<div className={thread.view}>
				<div ref={scrollRef} className={thread.scroll} data-fade-edge="both">
					<article className={thread.thread}>
						<header ref={placeholderRef} className={`${thread.head} ${thread.head_room}`} aria-hidden="true">
							<p className={thread.title} data-part="title">{letter.subject || "A letter with no subject"}</p>
							<div className={styles.where}>
								<span data-part="band"><ProjectBand matter={matter} /></span>
							</div>
						</header>

						<section className={styles.letter} aria-label={`From ${author.name}`}>
							<div className={styles.from}>
								<Avatar member={author} size={36} />
								<div className={styles.from_text}>
									<p className={styles.from_head}>
										<span className={styles.name}>{isOutgoing ? "You" : author.name}</span>
										<span className={styles.address} data-selectable="">{letter.from.address}</span>
										<time className={styles.time} dateTime={letter.sentAt}>{formatMoment(letter.sentAt)}</time>
									</p>
									<p className={styles.from_line}>
										{`To ${listWords(to) || "no one named"}${cc.length > 0 ? `, a copy to ${listWords(cc)}` : ""}.`}
									</p>
								</div>
							</div>

							<div className={`${thread.text} ${styles.words}`} data-selectable="">
								{paragraphsOf(letter.text).map((paragraph, index) => <p key={index} className={styles.letter_lines}>{paragraph}</p>)}
							</div>

							{files.length > 0 ? (
								<div className={styles.files}>
									<Result
										files={files}
										onOpenFile={file => {
											const document = documentAt(file.relativePath);
											if (document != null) open({ kind: "document", documentId: document.id });
											else toast(letter.filing.from == "folder"
												? "It is in the email in the Matter's folder, and not saved to the Matter on its own yet."
												: "It is not in the Matter yet. Save it from the letter in Outlook.");
										}}
									/>
								</div>
							) : null}

							<div className={`${styles.words} ${styles.filed}`}>
								<p className={styles.from_line}>
									{letter.filing.from == "folder"
										? `Filed in ${matter?.name ?? "the Matter"} from its folder, ${formatMoment(letter.filing.at)}.`
										: `Filed in ${matter?.name ?? "the Matter"} by ${letter.filing.actorName || "a member"}${letter.filing.from == "outlook" ? " from Outlook" : ""}, ${formatMoment(letter.filing.at)}.`}
								</p>
								{letter.filing.evidence.map(line => <p key={line} className={styles.filed_line}>{line}</p>)}
							</div>
						</section>
					</article>
				</div>
			</div>
			<MovingHead
				key={`head-${letter.id}`}
				scrollRef={scrollRef}
				placeholderRef={placeholderRef}
				title={letter.subject || "A letter with no subject"}
				band={<ProjectBand matter={matter} onClick={() => open({ kind: "matter", matterId: matter.id, tab: "correspondence" })} />}
				onTop={() => scrollRef.current?.scrollTo({ top: 0, behavior: "smooth" })}
			/>
		</div>
	);
}

// Someone outside the firm, as a face: initials on a calm colour of their own.
export function contactOf(party) {
	const name = party?.name || party?.address || "Someone";
	const initials = name.split(/[\s@.]+/).filter(Boolean).slice(0, 2).map(word => word[0].toUpperCase()).join("");
	const colours = ["#8b6a4b", "#2a7d8c", "#4a4f61", "#6b5b95", "#5b7f4a"];
	const sum = [...(party?.address ?? name)].reduce((total, character) => total + character.charCodeAt(0), 0);
	return { id: `contact:${party?.address ?? name}`, name, initials, color: colours[sum % colours.length] };
}

function documentAt(relativePath) {
	if (!relativePath) return null;
	const wanted = relativePath.replaceAll("\\", "/").toLowerCase();
	return realDocuments().find(document => (document.relativePath ?? "").replaceAll("\\", "/").toLowerCase() == wanted) ?? null;
}

// The letter's words as paragraphs: its blank lines part them.
function paragraphsOf(text) {
	return (text ?? "").replaceAll("\r\n", "\n").split(/\n\s*\n/).map(paragraph => paragraph.trim()).filter(Boolean);
}

function listWords(items) {
	if (items.length <= 1) return items.join("");
	return `${items.slice(0, -1).join(", ")} and ${items[items.length - 1]}`;
}
