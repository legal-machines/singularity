import { useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { FileKindIcon } from "../../components/FileKind/FileKind.jsx";
import { Input } from "../../components/Field/Field.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Page, PageHead } from "../../components/Page/Page.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { Row, Rows, Tray } from "../../components/Tray/Tray.jsx";
import { realLetters, realMatterDocuments, realWork, shownMatters, shownThreads, useRealWork } from "../../data/native.js";
import { members, viewerId } from "../../data/workspace.js";
import { findMatter, findMember, positionOf } from "../words.js";

import styles from "./SearchPage.module.css";

// Search across what the member can see: Matters, members and threads, by
// their names and what was said in them; and, of the Matter open in the app,
// its documents by name and its letters by their subject, people and words.
export function SearchPage({ query: initial }) {
	const { open } = useNavigation();
	useRealWork();
	const [query, setQuery] = useState(initial ?? "");
	const words = query.trim().toLowerCase();
	const has = text => text.toLowerCase().includes(words);
	const foundMatters = words == "" ? [] : shownMatters().filter(matter => has(`${matter.name} ${matter.client} ${matter.description}`));
	const foundMembers = words == "" ? [] : members.filter(member => has(`${member.name} ${positionOf(member)} ${member.practice ?? ""}`));
	const foundThreads = words == "" ? [] : shownThreads().filter(thread => has([thread.title, findMatter(thread.matterId).name, ...thread.posts.flatMap(post => post.paragraphs)].join(" ")));
	const openMatter = realWork().uiMatterId != null ? findMatter(realWork().uiMatterId) : null;
	const foundDocuments = words == "" || openMatter == null ? [] : realMatterDocuments().filter(document => document.documentId != null && has(document.name));
	const foundLetters = words == "" || openMatter == null ? [] : realLetters().filter(letter => has([
		letter.subject, letter.from?.name, letter.from?.address, ...(letter.to ?? []).map(party => `${party.name} ${party.address}`), letter.text,
	].filter(Boolean).join(" ")));
	const total = foundMatters.length + foundMembers.length + foundThreads.length + foundDocuments.length + foundLetters.length;

	return (
		<Page bar={<SheetBar trail={[{ label: "Search" }]} />}>
			<PageHead title="Search" />
			<label className={styles.field}>
				<Icon name="search" size={18} />
				<Input autoFocus value={query} placeholder="A Matter, a member, a thread, a word said" aria-label="Search" onChange={event => setQuery(event.target.value)} />
			</label>
			{words != "" && total == 0 ? <p className={styles.none}>Nothing found for “{query.trim()}”.</p> : null}
			<div className={styles.results}>
				{foundMatters.length > 0 ? (
					<Tray icon="matters" label="Matters" count={foundMatters.length}>
						<Rows>{foundMatters.map(matter => <Row key={matter.id} title={matter.name} line={matter.client} onOpen={() => open({ kind: "matter", matterId: matter.id, tab: "overview" })} />)}</Rows>
					</Tray>
				) : null}
				{foundMembers.length > 0 ? (
					<Tray icon="team" label="Members" count={foundMembers.length}>
						<Rows>{foundMembers.map(member => <Row key={member.id} lead={<Avatar member={member} size={32} />} title={member.name} line={positionOf(member)} onOpen={() => open({ kind: "profile", memberId: member.id })} />)}</Rows>
					</Tray>
				) : null}
				{foundDocuments.length > 0 ? (
					<Tray icon="document" label={`Documents in ${openMatter.name}`} count={foundDocuments.length}>
						<Rows>{foundDocuments.map(document => <Row key={document.documentId} lead={<FileKindIcon name={document.name} size="row" />} title={document.name} onOpen={() => open({ kind: "document", documentId: document.documentId })} />)}</Rows>
					</Tray>
				) : null}
				{foundLetters.length > 0 ? (
					<Tray icon="mail" label={`Letters in ${openMatter.name}`} count={foundLetters.length}>
						<Rows>
							{foundLetters.map(letter => {
								const other = letter.direction == "outgoing" ? letter.to?.[0] : letter.from;
								return <Row key={letter.id} title={letter.subject || "A letter with no subject"} line={`${letter.direction == "outgoing" ? "To" : "From"} ${other?.name || other?.address || "someone"}`} onOpen={() => open({ kind: "letter", letterId: letter.id })} />;
							})}
						</Rows>
					</Tray>
				) : null}
				{foundThreads.length > 0 ? (
					<Tray icon="messages" label="Threads" count={foundThreads.length}>
						<Rows>
							{foundThreads.map(thread => {
								const author = findMember(thread.posts.at(-1)?.authorId ?? viewerId);
								return <Row key={thread.id} lead={<Avatar member={author} size={32} />} title={thread.title} line={findMatter(thread.matterId).name} onOpen={() => open({ kind: "thread", threadId: thread.id, from: "search", query })} />;
							})}
						</Rows>
					</Tray>
				) : null}
			</div>
		</Page>
	);
}
