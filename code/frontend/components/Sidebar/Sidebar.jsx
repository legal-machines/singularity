import { AccountMenu } from "../AccountMenu/AccountMenu.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { Menu, useMenu } from "../Menu/Menu.jsx";
import { useNavigation } from "../Navigation/navigation.js";
import { useFirmComputers } from "../../data/coordination.js";
import { hasNative, shownChats } from "../../data/native.js";
import { organisation, viewerId } from "../../data/workspace.js";
import { lastChatMoment } from "../../pages/threadState.js";
import { unreadCounts } from "./unread.js";
import { Badges } from "./Badges.jsx";
import { Matters } from "./Matters.jsx";
import { Recent } from "./Recent.jsx";
import { setSidebarSlot } from "./slot.js";

import styles from "./Sidebar.module.css";

// Navigation (D-165): the firm's places, stable Matter folders and shortcuts
// to places actually visited. Calendar replaces the folders with its own
// controls (D-081). The account and global navigation never scroll away.
export function Sidebar({ state, opened = {}, hidden = false }) {
	const { place, open } = useNavigation();
	// What waits unread (D-202): a computer asking to join a Matter here counts
	// on the Inbox too (D-123).
	const unread = unreadCounts(state);
	const firm = useFirmComputers();
	const asking = firm.asking.filter(computer => state.doneIds.includes(`computer:${computer.deviceId}`) == false).length;
	const talked = shownChats(state).filter(chat => chat.memberId != viewerId).sort((first, second) => lastChatMoment(second, state).localeCompare(lastChatMoment(first, state)));
	const firmMenu = useMenu();
	const inCalendar = place.kind == "calendar" || place.kind == "invoice";

	return (
		<aside className={styles.sidebar} data-print="hide" inert={hidden || undefined}>
			<header className={styles.head}>
				<button
					className={styles.firm}
					type="button"
					aria-haspopup="menu"
					aria-expanded={firmMenu.isOpen}
					onClick={event => firmMenu.toggle(event.currentTarget)}
				>
					{organisation.logo != null
						? <img className={styles.firm_logo} src={organisation.logo} width="24" height="24" alt="" draggable={false} />
						: <span className={`${styles.firm_logo} ${styles.firm_initial}`} aria-hidden="true">{organisation.initial}</span>}
					<span className={styles.firm_name}>{organisation.name}</span>
				</button>
				{/* The firm's name alone heads the sidebar, with no chevron or search
				    button beside it (the owner, 2026-11-02): search opens with ⌘K or
				    Ctrl+K (App.jsx), and the name still opens the firm's menu. */}
				{firmMenu.isOpen ? (
					<Menu
						anchor={firmMenu.anchor}
						label={organisation.name}
						minWidth={232}
						onClose={firmMenu.close}
						items={[
							{ key: "team", label: "Team", icon: "team", onSelect: () => open({ kind: "team" }) },
							{ key: "matters", label: "Matters", icon: "matters", onSelect: () => open({ kind: "matters" }) },
							// The model accounts' keys, in Settings (the owner, 2026-11-02);
							// hiring is Team's own.
							hasNative() ? { key: "keys", label: "API keys", icon: "plug", onSelect: () => open({ kind: "settings", tab: "keys" }) } : null,
						]}
					/>
				) : null}
			</header>

			<nav className={styles.nav} aria-label="Places">
				<Tab icon="inbox" label="Inbox" isOpen={place.kind == "inbox"} onClick={() => open({ kind: "inbox" })}>
					<Badges count={unread.inbox.count + asking} namesViewer={unread.inbox.namesViewer} label="Inbox" />
				</Tab>
				<Tab
					icon="messages"
					label="Messages"
					isOpen={place.kind == "messages"}
					onClick={() => open({ kind: "messages", memberId: place.kind == "messages" ? place.memberId : talked[0]?.memberId })}
				>
					<Badges count={unread.messages.count} label="Messages" />
				</Tab>
				<Tab icon="calendar" label="Calendar" isOpen={place.kind == "calendar"} onClick={() => open({ kind: "calendar" })} />
				<Tab icon="team" label="Team" isOpen={place.kind == "team"} onClick={() => open({ kind: "team" })} />
			</nav>

			<div className={styles.scroll}>
				<div ref={setSidebarSlot} className={styles.context} />
				<div className={styles.work} hidden={inCalendar}>
					<Recent state={state} opened={opened} />
					<Matters state={state} unread={unread.matters} />
				</div>
			</div>

			<AccountMenu />
		</aside>
	);
}

// A tab of the desk. Open, it is joined to the sheet.
export function Tab({ icon, lead, label, detail, isOpen, isNew, onClick, children }) {
	return (
		<button
			className={[styles.tab, isOpen ? styles.tab_open : null].filter(Boolean).join(" ")}
			type="button"
			aria-current={isOpen ? "page" : undefined}
			aria-label={isNew ? `${label}, unread` : undefined}
			onClick={onClick}
		>
			<span className={styles.tab_lead}>{lead ?? (icon != null ? <Icon name={icon} size={18} /> : null)}</span>
			<span className={styles.tab_text}>
				<span className={isNew ? `${styles.tab_label} ${styles.tab_label_new}` : styles.tab_label}>{label}</span>
				{detail != null ? <span className={styles.tab_detail}>{detail}</span> : null}
			</span>
			{children != null ? <span className={styles.tab_end}>{children}</span> : null}
		</button>
	);
}

// A heading for what the open page adds to the desk.
export function DeskHeading({ children, aside }) {
	return (
		<div className={styles.desk_heading}>
			<span className={styles.desk_heading_text}>{children}</span>
			{aside}
		</div>
	);
}
