import { useLayoutEffect, useRef, useState } from "react";
import { Avatar } from "../Avatar/Avatar.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { Popover } from "../Popover/Popover.jsx";
import styles from "./Reactions.module.css";

// The emoji changes your reaction; the visible faces open everyone who
// reacted. Both use the backend's member ids, with no second reaction state.
export function Reactions({ reactions = {}, viewerId, resolveMember, onReact, onAdd, className }) {
	const groups = Object.entries(reactions).map(([emoji, who]) => [emoji, [...new Set(Array.isArray(who) ? who : [])]]).filter(([, who]) => who.length);
	if (!groups.length) return null;
	return <div className={className ? `${styles.reactions} ${className}` : styles.reactions}>
		{groups.map(([emoji, who]) => <Reaction key={emoji} emoji={emoji} who={who} viewerId={viewerId} resolveMember={resolveMember} onReact={onReact} />)}
		{onAdd ? <button className={styles.add} type="button" aria-label="Add a reaction" onClick={onAdd}><Icon name="smile" size={16} /></button> : null}
	</div>;
}
function Reaction({ emoji, who, viewerId, resolveMember, onReact }) {
	const [anchor, setAnchor] = useState(null);
	const chip = useRef(null);
	const [ready, setReady] = useState(false);
	useLayoutEffect(() => {
		let live = true;
		const cleanups = [];
		const images = [...chip.current.querySelectorAll("img")];
		const loaded = images.map(image => image.complete ? Promise.resolve() : new Promise(resolve => {
			const finish = () => { image.removeEventListener("load", finish); image.removeEventListener("error", finish); resolve(); };
			image.addEventListener("load", finish); image.addEventListener("error", finish);
			cleanups.push(() => { image.removeEventListener("load", finish); image.removeEventListener("error", finish); });
		}));
		Promise.all(loaded).then(() => { if (live) setReady(true); });
		return () => { live = false; cleanups.forEach(cleanup => cleanup()); };
	}, []);
	const people = who.map(id => resolveMember(id) ?? { id, name: "Colleague unavailable", initials: "?", color: "#888884" });
	const names = people.map(member => member.id === viewerId ? `${member.name} (you)` : member.name).join(", ");
	const selected = who.includes(viewerId);
	return <span ref={chip} className={styles.chip} data-ready={ready || undefined} data-mine={selected || undefined} role="group" aria-label={`${emoji} reaction`}>
		<button className={styles.emoji} type="button" aria-label={onReact ? `React with ${emoji}` : `Who reacted with ${emoji}`} aria-pressed={onReact ? selected : undefined} title={onReact ? selected ? "Remove your reaction" : "Add your reaction" : names} onClick={event => onReact ? onReact(emoji) : setAnchor(event.currentTarget)}>{emoji}</button>
		<button className={styles.people} type="button" aria-label={`Who reacted with ${emoji}: ${names}`} title={names} aria-haspopup="dialog" aria-expanded={Boolean(anchor)} onClick={event => setAnchor(event.currentTarget)}>
			<span className={styles.faces} aria-hidden="true">{people.slice(0, 3).map(member => <span key={member.id} className={styles.face}><Avatar member={member} size={22} showsOnline={false} /></span>)}</span>
			{people.length > 3 ? <span className={styles.more}>+{people.length - 3}</span> : null}
		</button>
		{anchor ? <Popover anchor={anchor} onClose={() => setAnchor(null)} width={272} label={`Reactions ${emoji}`}>
			<div className={styles.listHead}><span>{emoji}</span><span>{people.length} {people.length === 1 ? "reaction" : "reactions"}</span></div>
			<ul className={styles.list}>{people.map(member => <li key={member.id}><Avatar member={member} size={28} /><span>{member.name}{member.id === viewerId ? <small>You</small> : null}</span></li>)}</ul>
		</Popover> : null}
	</span>;
}
