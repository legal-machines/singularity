import { useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

import { Avatar } from "../Avatar/Avatar.jsx";
import { placeNear } from "../Floating/position.js";
import { Icon } from "../Icon/Icon.jsx";

import styles from "./MentionList.module.css";

// Whom a message can name, as the member types @ (the owner, 2026-10-26):
// the conversation's members, people and AI colleagues together, each with
// face, name and place in the firm (UI-5), and "all" first where more than
// two can be named. It stands over the box, beside where the words begin, and
// the words stay in the box: Up and Down move, Enter or Tab names the one
// marked, Escape closes, a press names one.
export function MentionList({ id, anchor, options, active, onPick, onPoint }) {
	const listRef = useRef(null);
	const [place, setPlace] = useState(null);

	useLayoutEffect(() => {
		if (anchor == null || listRef.current == null) return;
		const box = anchor.getBoundingClientRect();
		const size = listRef.current.getBoundingClientRect();
		const start = { left: box.left + 8, right: box.left + 8 + size.width, top: box.top, bottom: box.top, width: size.width, height: 0 };
		setPlace(placeNear(start, { width: size.width, height: size.height }, { placement: "top-start", gap: 6 }));
	}, [anchor, options.length]);

	useLayoutEffect(() => {
		listRef.current?.querySelector(`[data-index="${active}"]`)?.scrollIntoView({ block: "nearest" });
	}, [active]);

	return createPortal(
		<div
			ref={listRef}
			id={id}
			role="listbox"
			aria-label="Name a member"
			className={[styles.list, place != null ? styles.shown : null].filter(Boolean).join(" ")}
			style={place == null ? undefined : { top: place.top, left: place.left }}
			onMouseDown={event => event.preventDefault()}
		>
			{options.map((option, index) => (
				<div
					key={option.key}
					id={`${id}-${index}`}
					data-index={index}
					role="option"
					aria-selected={index == active}
					className={[styles.option, index == active ? styles.option_active : null].filter(Boolean).join(" ")}
					onMouseMove={() => onPoint(index)}
					onClick={() => onPick(option)}
				>
					<span className={styles.face}>
						{option.member != null ? <Avatar member={option.member} size={28} /> : <span className={styles.all}><Icon name="people" size={18} /></span>}
					</span>
					<span className={styles.text}>
						<span className={styles.name}>{option.label}</span>
						{option.line ? <span className={styles.line}>{option.line}</span> : null}
					</span>
				</div>
			))}
		</div>,
		document.body,
	);
}
