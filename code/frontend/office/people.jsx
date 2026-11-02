import { Avatar } from "../components/Avatar/Avatar.jsx";
import { characters } from "../components/Avatar/characters.js";

import styles from "./pane.module.css";

// The faces the pane shows, the same as Singularity's window shows them: an
// AI colleague's species of the Agent Family, a person's initials.

const PERSON_COLOURS = ["#2f56b0", "#2f8a6e", "#8b6a4b", "#7a4b97", "#5b6170"];

function initialsOf(name) {
	return (name ?? "").trim().split(/\s+/).map(word => word[0] ?? "").join("").slice(0, 2).toUpperCase() || "?";
}

function colourOf(name) {
	let sum = 0;
	for (const character of name ?? "") sum += character.charCodeAt(0);
	return PERSON_COLOURS[sum % PERSON_COLOURS.length];
}

// A colleague the backend names, as the window's faces draw it.
export function colleagueMember(agent) {
	const species = (agent?.name ?? "").trim().toLowerCase();
	return {
		id: agent?.id ?? species,
		kind: "ai_colleague",
		name: agent?.name ?? "",
		species: characters[species] != null ? species : null,
		initials: initialsOf(agent?.name),
	};
}

export function personMember(name) {
	return { id: `person:${name}`, kind: "person", name, initials: initialsOf(name), color: colourOf(name) };
}

// What a colleague's face acts out now: its work while it works.
export function presenceOf(agent, isWorking) {
	if (isWorking) return "work";
	return agent?.presence || "rest";
}

export function Face({ member, size, presence }) {
	return <Avatar member={member} size={size} presence={presence} />;
}

// The Matter's mark: its short name's initials on its colour.
export function MatterMark({ matter, size = 32 }) {
	const name = matter?.appearance?.shortName || matter?.name || "";
	const colour = matter?.appearance?.color || "#2f56b0";
	return (
		<span className={styles.mark} style={{ width: size, height: size, background: colour, fontSize: Math.round(size * 0.38) }} aria-hidden="true">
			{initialsOf(name)}
		</span>
	);
}
