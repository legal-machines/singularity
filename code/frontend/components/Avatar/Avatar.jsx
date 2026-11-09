import { useEffect, useRef, useState } from "react";

import { characters } from "./characters.js";
import { join, point, report, resolutionFor, stillOf } from "./life.js";
import { isOnline, useOnline } from "./online.js";
import { stateOf } from "./presence.js";

import styles from "./Avatar.module.css";

// The base face's ground for a person no colour was given to: the slate the
// app gives a new member (data/native.js).
const BASE_GROUND = "#5b6170";

// One face for every member, in one shape: a square with G2 corners. A
// person's face is their photograph, or without one the base face, a figure on
// their colour. An AI colleague's face is its species of the Agent Family on
// the species' own ground, never a photograph, alive on the window's one clock
// (life.js): its state as the backend reports it, and at rest a gesture now
// and then and one right after it is pointed at (its face, or the row that
// passes `pointedAt`). Under reduced motion it holds its state's still. Where
// its state matters, `withState` adds the dot that says what it is doing now.
export function Avatar({ member, size, presence, withState, pointedAt, showsOnline = true }) {
	useOnline();
	const face = faceOf(member, size, presence, pointedAt == true);
	const working = withState == true && member.kind == "ai_colleague" ? stateOf(presence) : null;
	// A colleague who is there now wears a green dot on every face of theirs,
	// unless the dot of what it does stands there or the place says none
	// (Recent: "don't show the online dots in Recent").
	const state = working ?? (showsOnline && isOnline(member) ? { colour: "var(--online)", words: member.kind == "ai_colleague" ? "Available" : "Online" } : null);
	if (state == null) return face;

	// The dot's centre stands where the face's edge crosses its diagonal:
	// the corner's arc is centred as a circle of its radius would be, 6.5% in.
	const dot = Math.max(8, Math.round(size * 0.27));
	const offset = Math.round(size * 0.065 - dot / 2);

	return (
		<span className={styles.frame} style={{ width: size, height: size }}>
			{face}
			<span
				className={styles.state}
				style={{ width: dot, height: dot, right: offset, bottom: offset, background: state.colour }}
				title={state.words}
				role="img"
				aria-label={state.words}
			/>
		</span>
	);
}

function isStill() {
	return typeof window != "undefined" && typeof window.matchMedia == "function"
		&& window.matchMedia("(prefers-reduced-motion: reduce)").matches;
}

function ratioOf() {
	return typeof window != "undefined" ? window.devicePixelRatio || 1 : 1;
}

// An AI colleague's living face: a canvas its colleague's life draws each
// frame into, from the atlas of the resolution its size takes on this screen.
// Only a face that knows the colleague's presence says it; a face in a
// thread's post or an inbox row leaves it as it is.
function LivingFace({ member, character, size, presence, pointedAt }) {
	const canvas = useRef(null);
	const [isHovered, setHovered] = useState(false);
	const ratio = ratioOf();
	const px = resolutionFor(size, ratio);
	const pixels = Math.max(1, Math.round(size * ratio));

	useEffect(() => {
		const element = canvas.current;
		const context = element?.getContext?.("2d") ?? null;
		const face = {
			px,
			paint(frame) {
				if (element == null) return;
				if (frame?.state != null && element.dataset.state != frame.state) element.dataset.state = frame.state;
				if (context == null) return;
				context.clearRect(0, 0, pixels, pixels);
				if (frame?.image == null) return;
				context.imageSmoothingEnabled = true;
				context.imageSmoothingQuality = "high";
				context.drawImage(frame.image, frame.sx, frame.sy, frame.sw, frame.sh, 0, 0, pixels, pixels);
			},
		};
		return join(member.id, member.species, presence, face);
	}, [member.id, member.species, px, pixels]);

	useEffect(() => {
		if (presence != null) report(member.id, presence);
	}, [presence, member.id]);

	useEffect(() => {
		if (pointedAt || isHovered) point(member.id);
	}, [pointedAt, isHovered, member.id]);

	return (
		<span
			className={`${styles.avatar} ${styles.character}`}
			style={{ width: size, height: size, background: `var(--avatar-ground, ${character.ground})` }}
			aria-hidden="true"
			onMouseEnter={() => setHovered(true)}
			onMouseLeave={() => setHovered(false)}
		>
			<canvas ref={canvas} width={pixels} height={pixels} data-species={member.species} />
		</span>
	);
}

function faceOf(member, size, presence, pointedAt) {
	// A photo cut in several sizes (photoSet) is drawn from the one nearest
	// the face's size on this screen, never shrunk many times over.
	if (member.kind != "ai_colleague" && member.photo != null) {
		return (
			<img
				className={`${styles.avatar} ${styles.person} ${styles.photo}`}
				srcSet={member.photoSet ?? undefined}
				sizes={member.photoSet != null ? `${size}px` : undefined}
				src={member.photo}
				width={size}
				height={size}
				alt=""
				draggable={false}
			/>
		);
	}

	// A person who has not chosen a photo: the base face, a head and shoulders
	// on their colour, the same at every size.
	if (member.kind != "ai_colleague") {
		return (
			<span
				className={`${styles.avatar} ${styles.person} ${styles.base}`}
				style={{ width: size, height: size, background: member.color ?? BASE_GROUND }}
				aria-hidden="true"
			>
				<svg viewBox="0 0 40 40" width={size} height={size} focusable="false" aria-hidden="true">
					<circle cx="20" cy="15.2" r="7.2" />
					<path d="M6.5 40c0-8 6-13.2 13.5-13.2S33.5 32 33.5 40Z" />
				</svg>
			</span>
		);
	}

	const character = characters[member.species];
	if (character == null) {
		return <span className={`${styles.avatar} ${styles.character}`} style={{ width: size, height: size }} aria-hidden="true" />;
	}

	// Under reduced motion, its state's still at the resolution its size takes.
	if (isStill()) {
		return (
			<span
				className={`${styles.avatar} ${styles.character}`}
				style={{ width: size, height: size, background: `var(--avatar-ground, ${character.ground})` }}
				aria-hidden="true"
			>
				<img src={stillOf(member.species, presence ?? "rest", resolutionFor(size, ratioOf()))} width={size} height={size} alt="" draggable={false}
					data-species={member.species} />
			</span>
		);
	}
	return <LivingFace member={member} character={character} size={size} presence={presence} pointedAt={pointedAt} />;
}
