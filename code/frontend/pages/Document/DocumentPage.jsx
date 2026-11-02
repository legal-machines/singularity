import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { Chip } from "../../components/Chip/Chip.jsx";
import { FileKindIcon } from "../../components/FileKind/FileKind.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Page, PageHead } from "../../components/Page/Page.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { Row, Rows, Tray } from "../../components/Tray/Tray.jsx";
import { instruments } from "../../data/instruments.js";
import { decisions, threads, workProducts } from "../../data/workspace.js";
import { findMatter, findMember, formatWhen } from "../words.js";

import styles from "./DocumentPage.module.css";

// A document as the lawyer reads it (PRODUCT.md, "Documents" and "Work
// products"): its text on a page of its own, each change in place, struck
// and inserted, with the colleague's reason beside it; the versions and the
// sources on the right.
export function DocumentPage({ state }) {
	const { place, open } = useNavigation();
	const instrument = instruments[place.documentId];
	const workProduct = workProducts[instrument.workProductId];
	const thread = threads.find(item => item.posts.some(post => post.workProductId == instrument.workProductId));
	const matter = findMatter(thread.matterId);
	const decision = thread.decisionId != null ? decisions[thread.decisionId] : null;
	const answered = decision != null ? state.choices[thread.decisionId] : null;
	const notes = Object.entries(instrument.notes);

	return (
		<Page
			bar={<SheetBar trail={[{ label: matter.name, place: { kind: "matter", matterId: matter.id, tab: "overview" } }, { label: "Documents", place: { kind: "matter", matterId: matter.id, tab: "documents" } }, { label: instrument.file }]} />}
		>
			<PageHead
				lead={<span className={styles.file_icon}><FileKindIcon name={instrument.file} /></span>}
				title={`${workProduct.title}, version ${workProduct.version}`}
				line={`${instrument.title} between ${instrument.parties}`}
				actions={
					<>
						<Button icon="return" onClick={() => open({ kind: "thread", threadId: thread.id, from: "matter" })}>Open the thread</Button>
						{decision != null && answered == null ? <Button variant="primary" onClick={() => open({ kind: "thread", threadId: thread.id, from: "matter" })}>Answer Unicorn</Button> : null}
					</>
				}
			/>

			<div className={styles.layout}>
				<article className={styles.paper} aria-label={instrument.title}>
					<h2 className={styles.paper_title}>{instrument.title}</h2>
					<p className={styles.paper_parties}>{instrument.parties}</p>
					{instrument.sections.map(section => (
						<section key={section.number} className={styles.section}>
							<h3 className={styles.section_heading}>
								<span className={styles.number}>{section.number}.</span>
								{section.heading}
							</h3>
							{section.clauses.map(clause => (
								<p key={clause.number} className={styles.clause} data-selectable="">
									<span className={styles.number}>{clause.number}</span>
									<span className={styles.clause_text}>
										{clause.runs.map((run, index) => <Run key={index} run={run} />)}
									</span>
								</p>
							))}
						</section>
					))}
				</article>

				<aside className={styles.side}>
					<Tray icon="edit" label="Changes" count={notes.filter(([, note]) => note.kind == "change").length}>
						<Rows>
							{notes.map(([id, note]) => (
								<Row
									key={id}
									lead={<Avatar member={findMember(note.authorId)} size={28} />}
									title={<span className={note.kind == "unchecked" ? styles.note_open : undefined}>{note.text}</span>}
									line={note.source}
								/>
							))}
						</Rows>
					</Tray>
					<Tray icon="document" label="Versions">
						<Rows>
							{Object.entries(workProducts)
								.filter(([, item]) => item.title == workProduct.title)
								.sort(([, first], [, second]) => second.version - first.version)
								.map(([id, item]) => (
									<Row
										key={id}
										lead={<span className={styles.version}>v{item.version}</span>}
										title={item.version == workProduct.version ? "This version" : item.state}
										line={`By ${findMember(item.authorId).name} ${formatWhen(item.delivered)}`}
										aside={item.version == workProduct.version ? (answered == "A" ? <Chip tone="done">Sent</Chip> : <Chip tone="yours">Asks you</Chip>) : null}
									/>
								))}
						</Rows>
					</Tray>
					{workProduct.sources != null ? (
						<Tray icon="link" label="Sources">
							<Rows>
								{workProduct.sources.map(source => <Row key={source.name} title={source.name} line={source.note} />)}
							</Rows>
						</Tray>
					) : null}
				</aside>
			</div>
		</Page>
	);
}

function Run({ run }) {
	if (typeof run == "string") return run;
	if (run.cite != null) return <span className={styles.cite}>{run.cite}</span>;
	if (run.mark != null) return null;
	return (
		<>
			{run.del != null ? <del className={styles.deleted}>{run.del}</del> : null}
			{run.del != null && run.ins != null ? " " : null}
			{run.ins != null ? <ins className={styles.inserted}>{run.ins}</ins> : null}
		</>
	);
}
