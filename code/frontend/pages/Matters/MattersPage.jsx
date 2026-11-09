import { useEffect, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { AskedToJoin, AskingToJoin, GivenToYou } from "../../components/FirmComputers/FirmComputers.jsx";
import { ArchiveMatterDialog } from "../../components/Manage/ArchiveMatterDialog.jsx";
import { ConfirmDialog } from "../../components/Manage/Manage.jsx";
import { NewMatterDialog } from "../../components/NewMatter/NewMatterDialog.jsx";
import { toast } from "../../components/Toast/toast.js";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Page, PageHead } from "../../components/Page/Page.jsx";
import { ProjectMark } from "../../components/ProjectMark/ProjectMark.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { Views } from "../../components/Views/Views.jsx";
import { deleteMatterFromTrash, hasNative, lookForArrivedMatters, matterTrash, openArrivedMatter, openMatterFolder, restoreMatterFromTrash, setMatterArchived, shownMatters, useRealWork } from "../../data/native.js";
import { organisation, viewerId } from "../../data/workspace.js";
import { findMember } from "../words.js";

import { Figures } from "./Figures.jsx";
import { useMatterFigures } from "./figures.js";
import styles from "./MattersPage.module.css";

// Every Matter the member can see, by client, set as Team is: which Matters -
// the open or the archived - over the list, then each client's Matters under
// the client's name with their count, on the page itself, no frame, no
// capitals, no lines between rows. Each row: the Matter's own mark, its name
// with what it holds and what its work came to beside it, what it is, and who
// works on it; under the pointer, what can be done with it, as an Inbox
// row's.
export function MattersPage({ state }) {
	const { place, open } = useNavigation();
	// Opened from the desk's New Matter, the page asks for the Matter at once.
	const [isCreating, setCreating] = useState(place.create == true);
	const [view, setView] = useState("open");
	const real = useRealWork();
	// What arrived is looked for as the page opens, so one gone since leaves it.
	useEffect(() => {
		if (hasNative()) lookForArrivedMatters();
	}, []);
	// The archived ones stand apart, under their own view, in the app.
	const archived = hasNative() ? shownMatters().filter(matter => matter.isClosed == true) : [];
	const matters = shownMatters().filter(matter => archived.includes(matter) == false);
	// Matters the firm's computers brought that are not open here yet.
	const arrivals = real.arrivals ?? [];
	const clients = [...new Set(matters.map(matter => matter.client))];
	// A colleague's Matter comes to this computer as their offer, accepted here: no
	// invitation to type. A lawyer opens the firm's Matters here.
	const create = hasNative() ? <Button variant="primary" icon="plus" onClick={() => setCreating(true)}>New Matter</Button> : null;
	const openMatter = matterId => open({ kind: "matter", matterId, tab: "overview" });
	// This computer's Trash: what was deleted here in the last 30 days.
	const trash = hasNative() ? matterTrash() : [];
	const views = [{ id: "open", label: "Open" }, { id: "archived", label: hasNative() ? "Archived" : "Closed" },
		...(hasNative() ? [{ id: "trash", label: "Trash" }] : [])];
	const figuresOf = useMatterFigures(matters);

	return (
		<Page asInbox bar={<SheetBar trail={[{ label: organisation.name }, { label: "Matters" }]} />}>
			<PageHead asInbox title="Matters" line={matters.length + archived.length == 0 ? "No Matter is open here yet." : `${matters.filter(matter => !matter.isClosed).length} open and ${matters.filter(matter => matter.isClosed).length + archived.length} ${hasNative() ? "archived" : "closed"}, for ${clients.length} ${clients.length == 1 ? "client" : "clients"}.`} actions={create} />
			{isCreating ? <NewMatterDialog onClose={() => setCreating(false)} onCreated={matterId => { setCreating(false); openMatter(matterId); }} /> : null}
			<div className={styles.toolbar}><Views label="Which Matters" views={views} view={view} onChange={setView} controls="matters-list" /></div>
			<div className={styles.groups} id="matters-list" role="tabpanel">
				{view == "open" ? (
					<>
						<GivenToYou />
						<AskingToJoin />
						<AskedToJoin />
						{arrivals.length > 0 ? <Arrived arrivals={arrivals} onOpened={openMatter} /> : null}
						{hasNative() && matters.length == 0 && arrivals.length == 0 ? (
							<p className={styles.empty}>Open the firm's first Matter with New Matter, or join a colleague's with the invitation they send you.</p>
						) : null}
						{clients.map(client => {
							const list = matters.filter(matter => matter.client == client).sort((first, second) => Number(first.isClosed == true) - Number(second.isClosed == true));
							return (
								<Group key={client} title={client} count={list.length}>
									{list.map(matter => {
										const people = matter.memberIds.map(findMember).filter(Boolean);
										return (
											<MatterRow
												key={matter.id}
												matter={matter}
												line={matter.description.replace(/^For [^:]+: /, "").replace(/^./, letter => letter.toUpperCase())}
												figures={figuresOf(matter)}
												onOpen={() => openMatter(matter.id)}
												actions={matter.isClosed ? null : <MatterActions matter={matter} />}
												aside={
													<>
														{matter.isClosed ? <span className={styles.word}>Closed</span> : null}
														<span className={styles.faces}>
															{people.map(member => <span key={member.id} className={styles.face} data-tooltip={member.id == viewerId ? "You" : member.name}><Avatar member={member} size={24} /></span>)}
														</span>
													</>
												}
											/>
										);
									})}
								</Group>
							);
						})}
					</>
				) : view == "trash" ? (trash.length > 0 ? <Trash matters={trash} />
					: <p className={styles.empty}>The Trash is empty. A Matter deleted here stays in it for 30 days.</p>)
				: archived.length > 0 ? <Archived matters={archived} onOpen={openMatter} />
					: <p className={styles.empty}>No Matter is archived.</p>}
			</div>
		</Page>
	);
}

// A group of rows under its name in plain words, with its count, as Team's.
function Group({ title, count, children }) {
	return (
		<section className={styles.group} aria-label={title}>
			<h2 className={styles.group_title}>{title}{count != null ? <span className={styles.group_count}>{count}</span> : null}</h2>
			<ul className={styles.list}>{children}</ul>
		</section>
	);
}

// One Matter: its mark, its name with its figures, and the line under it;
// pressed, it opens; at its right what can be done with it, shown under the
// pointer, and what stands beside it, a control staying a control.
// A Matter's row; one that cannot be opened, as in the Trash, is not a button.
function MatterRow({ matter, line, figures = [], onOpen, actions = null, aside }) {
	const Open = onOpen != null ? "button" : "span";
	return (
		<li className={styles.row}>
			<Open className={styles.row_open} {...(onOpen != null ? { type: "button", onClick: onOpen } : { "data-still": "" })}>
				<span className={styles.row_mark}><ProjectMark matter={matter} size={40} /></span>
				<span className={styles.row_text}>
					<span className={styles.row_head}>
						<span className={styles.row_name}>{matter.name}</span>
						<Figures figures={figures} />
					</span>
					{line != null && line != "" ? <span className={styles.row_line}>{line}</span> : null}
				</span>
			</Open>
			{actions != null || aside != null ? <span className={styles.row_aside}>{actions}{aside}</span> : null}
		</li>
	);
}

// What can be done with an open Matter from its row, as an Inbox row's
// buttons: show its folder, archive it. Its icon is changed on its own
// page.
function MatterActions({ matter }) {
	const [isArchiving, setArchiving] = useState(false);
	if (matter.isReal != true) return null;
	const folderWords = folderWordsOf();
	async function showFolder() {
		try {
			await openMatterFolder(matter.id);
		} catch (reason) {
			toast(reason?.message ?? "The Matter's folder could not be shown.", { tone: "seal" });
		}
	}
	return (
		<span className={styles.actions}>
			{matter.directoryPath != null ? (
				<button type="button" aria-label={`${folderWords}: ${matter.name}`} data-tooltip={folderWords} onClick={showFolder}><Icon name="matter" size={18} /></button>
			) : null}
			<button type="button" aria-label={`Archive: ${matter.name}`} data-tooltip="Archive" onClick={() => setArchiving(true)}><Icon name="archive" size={18} /></button>
			{isArchiving ? <ArchiveMatterDialog matter={matter} onClose={() => setArchiving(false)} /> : null}
		</span>
	);
}

// Where a folder is shown, in the words of the computer's own system.
function folderWordsOf() {
	const platform = window.nativeApp?.platform ?? (navigator.platform ?? "").slice(0, 3).toLowerCase();
	if (platform == "darwin" || platform == "mac") return "Show in Finder";
	if (platform == "win32" || platform == "win") return "Show in File Explorer";
	return "Show in Files";
}

// The archived Matters: each opens to be read, and comes back among the open
// ones with Restore.
function Archived({ matters, onOpen }) {
	const clients = [...new Set(matters.map(matter => matter.client))];
	return clients.map(client => {
		const list = matters.filter(matter => matter.client == client);
		return (
			<Group key={client} title={client} count={list.length}>
				{list.map(matter => (
					<MatterRow
						key={matter.id}
						matter={matter}
						line={matter.description?.replace(/^For [^:]+: /, "").replace(/^./, letter => letter.toUpperCase()) ?? ""}
						onOpen={() => onOpen(matter.id)}
						aside={<Button size="small" onClick={async () => {
							try {
								await setMatterArchived(matter.id, false);
								toast(`${matter.name} is open again.`);
							} catch (reason) {
								toast(reason?.message ?? `${matter.name} could not be restored.`, { tone: "seal" });
							}
						}}>Restore</Button>}
					/>
				))}
			</Group>
		);
	});
}

// This computer's Trash: each Matter deleted here, when it went, and when it
// is deleted for good; Restore brings it back whole, Delete now asks first
// and deletes it from this computer at once.
function Trash({ matters }) {
	const [deleting, setDeleting] = useState(null);
	const day = iso => {
		const date = new Date(iso);
		return Number.isNaN(date.getTime()) ? "" : new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric", year: "numeric" }).format(date);
	};
	return (
		<>
			<Group title="Deleted from this computer" count={matters.length}>
				{matters.map(matter => (
					<MatterRow
						key={matter.id}
						matter={matter.mark ?? { id: matter.id, name: matter.name }}
						line={`Deleted ${day(matter.trashedAt)}. Goes for good on ${day(matter.deletesAt)}.`}
						aside={
							<>
								<Button size="small" onClick={async () => {
									try {
										await restoreMatterFromTrash(matter.id);
										toast(`${matter.name} is back among your Matters.`);
									} catch (reason) {
										toast(reason?.message ?? `${matter.name} could not be restored.`, { tone: "seal" });
									}
								}}>Restore</Button>
								<Button size="small" onClick={() => setDeleting(matter)}>Delete now</Button>
							</>
						}
					/>
				))}
			</Group>
			{deleting != null ? (
				<ConfirmDialog
					title={`Delete ${deleting.name} for good?`}
					description="It can't be restored here. Its folder and documents stay where they are."
					confirm="Delete"
					onClose={() => setDeleting(null)}
					onConfirm={async () => {
						await deleteMatterFromTrash(deleting.id);
						toast(`${deleting.name} is deleted from this computer.`);
					}} />
			) : null}
		</>
	);
}

// The Matters the firm's computers brought here that are not open yet: each
// opens here when the lawyer says so.
function Arrived({ arrivals, onOpened }) {
	const [opening, setOpening] = useState(null);
	return (
		<Group title="Arrived from your firm's computers" count={arrivals.length}>
			{arrivals.map(arrival => (
				<MatterRow
					key={arrival.id}
					matter={{ id: arrival.id, name: arrival.name }}
					line="Its documents and threads are here. Open it to work on it with the team."
					aside={<Button size="small" variant="primary" busy={opening == arrival.id} disabled={opening != null} onClick={async () => {
						setOpening(arrival.id);
						try {
							onOpened(await openArrivedMatter(arrival));
							toast(`${arrival.name} is open on this computer.`);
						} catch (reason) {
							toast(reason?.message ?? `${arrival.name} could not be opened.`, { tone: "seal" });
						}
						setOpening(null);
					}}>Open it here</Button>}
				/>
			))}
		</Group>
	);
}
