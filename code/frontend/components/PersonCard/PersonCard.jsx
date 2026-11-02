import { useCallback, useState } from "react";

import { personFacts } from "../../data/profiles.js";
import { colleagueState, memberAway } from "../../pages/threadState.js";
import { findMember, isViewerMember, positionOf } from "../../pages/words.js";
import { Avatar } from "../Avatar/Avatar.jsx";
import { Button } from "../Button/Button.jsx";
import { useNavigation } from "../Navigation/navigation.js";
import { Popover } from "../Popover/Popover.jsx";
import { ShowPerson, useShowPerson } from "./showPerson.js";

import styles from "./PersonCard.module.css";

// A member's card, opened by a press on its face, its name or an @mention of
// it, wherever they stand (the owner, 2026-10-24: "when I press on people and
// people's tags, a window with the person must open"). One card for a person
// and an AI colleague alike (D-087): the face, the name, the position with its
// practice (D-058), what the member handles, what it is doing now, and the
// two ways on: its chat and its profile.

// The one card of the window, over everything; a second press on what opened
// it puts it away.
export function PersonCardLayer({ state, children }) {
	const [shown, setShown] = useState(null);
	const show = useCallback((memberId, anchor) => {
		setShown(previous => previous?.memberId == memberId && previous.anchor == anchor ? null : { memberId, anchor });
	}, []);
	const close = useCallback(() => setShown(null), []);
	const member = shown != null ? findMember(shown.memberId) : null;

	return (
		<ShowPerson.Provider value={show}>
			{children}
			{member != null ? (
				<Popover anchor={shown.anchor} width={320} label={member.name} onClose={close}>
					<PersonCard member={member} state={state} onLeave={close} />
				</Popover>
			) : null}
		</ShowPerson.Provider>
	);
}

// A face or a name that opens its member's card.
export function PersonButton({ memberId, label, className, children }) {
	const show = useShowPerson();
	return (
		<button
			className={[styles.opener, className].filter(Boolean).join(" ")}
			type="button"
			aria-label={label}
			aria-haspopup="dialog"
			onClick={event => {
				event.stopPropagation();
				show(memberId, event.currentTarget);
			}}
		>
			{children}
		</button>
	);
}

function PersonCard({ member, state, onLeave }) {
	const { open } = useNavigation();
	const isViewer = isViewerMember(member.id);
	const now = member.kind == "ai_colleague" ? colleagueState(member.id, state) : null;
	const away = memberAway(member, state);
	const office = personFacts[member.id]?.office;
	const status = now?.text ?? away ?? (office != null ? `${office} office` : null);

	function go(place) {
		onLeave();
		open(place);
	}

	return (
		<div className={styles.card}>
			<div className={styles.who}>
				<Avatar member={member} size={56} presence={now?.presence} />
				<div className={styles.names}>
					<p className={styles.name}>{isViewer ? `${member.name} (you)` : member.name}</p>
					<p className={styles.role}>
						{positionOf(member)}
						{member.practice != null ? <span className={styles.practice}> ({member.practice})</span> : null}
					</p>
				</div>
			</div>
			{member.area != null ? <p className={styles.area}>{member.area}</p> : null}
			{status != null ? <p className={styles.status}>{status}</p> : null}
			<div className={styles.actions}>
				{isViewer ? null : <Button variant="primary" icon="messages" onClick={() => go({ kind: "messages", memberId: member.id })}>Message</Button>}
				<Button onClick={() => go({ kind: "profile", memberId: member.id })}>{isViewer ? "Your profile" : "Profile"}</Button>
			</div>
		</div>
	);
}
