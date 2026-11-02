import { useState } from "react";

import { Avatar } from "../Avatar/Avatar.jsx";
import { characters } from "../Avatar/characters.js";
import { Field, Input, Select, TextArea } from "../Field/Field.jsx";
import { readPicture, squarePhoto } from "../../data/photos.js";
import { normalizePractice } from "../../data/practices.js";
import { hasNative, nativeApp } from "../../data/native.js";
import { POSITIONS } from "../../onboarding/tour/pages.js";

import styles from "./ProfileFields.module.css";

// Who sits in a seat, as the firm says it (D-209): their photo, their name,
// their role on the firm's scale, their own words for their position, their
// practice, and a few lines about them (the owner, 2026-11-02). Kept in the
// firm's own record, never at Legal Machines. The value is { name, rank,
// title, practice, about, photo }; every change goes to onChange whole.
export function emptyProfile() {
	return { name: "", rank: "", title: "", practice: "", about: "", photo: null };
}

export function ProfileFields({ value, onChange, disabled = false, autoFocus = false }) {
	const [photoProblem, setPhotoProblem] = useState(null);
	const set = (key, next) => onChange({ ...value, [key]: next });

	async function takePicture(picture) {
		if (picture == null) return;
		try {
			set("photo", await squarePhoto(picture));
			setPhotoProblem(null);
		} catch {
			setPhotoProblem("This picture can’t be used. Choose a JPEG, PNG, GIF or WebP picture.");
		}
	}
	async function choosePhoto() {
		setPhotoProblem(null);
		if (hasNative() == false || typeof nativeApp().chooseOwnPhoto != "function") return;
		try {
			await takePicture((await nativeApp().chooseOwnPhoto())?.picture ?? null);
		} catch (reason) {
			setPhotoProblem(reason?.detail ?? reason?.message ?? "The picture couldn’t be opened. Choose another.");
		}
	}
	async function dropPhoto(event) {
		event.preventDefault();
		if (disabled) return;
		try {
			await takePicture(await readPicture(event.dataTransfer?.files?.[0]));
		} catch (reason) {
			setPhotoProblem(reason.message);
		}
	}

	const face = { id: "profile-fields", kind: "person", name: value.name.trim(), color: "#5b6170", photo: value.photo };
	return (
		<div className={styles.fields}>
			<div className={styles.photo} onDragOver={event => event.preventDefault()} onDrop={dropPhoto}>
				<button className={styles.face} type="button" disabled={disabled} aria-label={value.photo ? "Change the photo" : "Add a photo"} onClick={choosePhoto}>
					<Avatar member={face} size={56} />
				</button>
				<span className={styles.photoText}>
					<span className={styles.photoLabel}>Photo</span>
					<span className={styles.photoActions}>
						<button className={styles.inline} type="button" disabled={disabled} onClick={choosePhoto}>{value.photo ? "Change" : "Choose a photo"}</button>
						{value.photo ? <button className={styles.inline} type="button" disabled={disabled} onClick={() => set("photo", null)}>Remove</button> : null}
					</span>
					{photoProblem != null ? <span className={styles.problem}>{photoProblem}</span> : null}
				</span>
			</div>
			<Field label="Name">
				<Input value={value.name} maxLength={120} autoComplete="off" disabled={disabled} autoFocus={autoFocus}
					onChange={event => set("name", event.target.value)} />
			</Field>
			<div className={styles.pair}>
				<Field label="Role">
					<Select value={value.rank} disabled={disabled} onChange={event => set("rank", event.target.value)}>
						<option value="">Choose a role</option>
						{POSITIONS.map(position => <option key={position} value={position}>{position}</option>)}
					</Select>
				</Field>
				<Field label="Position" optional>
					<Input value={value.title} maxLength={160} autoComplete="off" disabled={disabled} placeholder="Head of Litigation"
						onChange={event => set("title", event.target.value)} />
				</Field>
			</div>
			<Field label="Practice" optional>
				<Input value={value.practice} maxLength={160} autoComplete="off" disabled={disabled} placeholder="Mergers & Acquisitions"
					onChange={event => set("practice", event.target.value)}
					onBlur={() => set("practice", normalizePractice(value.practice))} />
			</Field>
		</div>
	);
}

// A few lines about the person, a profile's second step (the owner,
// 2026-11-02: "the colleague's description must be edited too"): what they
// would have the firm know of them, at most 600 characters, their line breaks
// kept.
export function AboutFields({ value, onChange, disabled = false, placeholder = "" }) {
	return (
		<div className={styles.fields}>
			<Field label="In a few lines">
				<TextArea value={value.about ?? ""} minRows={6} maxRows={10} maxLength={600} disabled={disabled} autoFocus
					placeholder={placeholder} onChange={event => onChange({ ...value, about: event.target.value })} />
			</Field>
		</div>
	);
}

// An AI colleague's profile, edited as a lawyer's is (the owner, 2026-11-02:
// "editing AI colleagues' profiles must be as easy as editing the lawyers'",
// and "split it into slides, as onboarding was, so that everything fits"), in
// two steps. Its profile first, a lawyer's fields: its face where a person's
// photo stands, changed in place among the faces; its name; the role it
// ranks with and its own words for its position on a line; its practice.
// Then its work: its area, what it does and how it works - its hiring's
// words. The value is { name, character, ranksWith, title, practice, area,
// goal, manner }; every change goes to onChange whole.
const FACES = Object.keys(characters);

export function ColleagueProfileFields({ value, onChange, disabled = false }) {
	const [isChoosingFace, setChoosingFace] = useState(false);
	const set = (key, next) => onChange({ ...value, [key]: next });
	// A face this window does not know shows as the first; saved, it is kept as it was.
	const face = { id: "colleague-fields", kind: "ai_colleague", species: characters[value.character] != null ? value.character : FACES[0], name: value.name.trim() };
	return (
		<div className={styles.fields}>
			{/* The faces stand in the face's own place while one is chosen, so the
			    step keeps its height and nothing scrolls. */}
			{isChoosingFace ? (
				<div className={styles.facesRow}>
					<div className={styles.faces} role="radiogroup" aria-label="Face">
						{FACES.map(name => (
							<button key={name} type="button" role="radio" aria-checked={value.character == name} aria-label={name}
								className={styles.choice} disabled={disabled}
								onClick={() => set("character", name)}>
								<Avatar member={{ id: `colleague-face-${name}`, kind: "ai_colleague", species: name, name }} size={36} />
							</button>
						))}
					</div>
					<button className={styles.inline} type="button" disabled={disabled} onClick={() => setChoosingFace(false)}>Done</button>
				</div>
			) : (
				<div className={styles.photo}>
					<button className={styles.face} type="button" disabled={disabled} aria-label="Change the face" aria-expanded={false}
						onClick={() => setChoosingFace(true)}>
						<Avatar member={face} size={56} />
					</button>
					<span className={styles.photoText}>
						<span className={styles.photoLabel}>Face</span>
						<span className={styles.photoActions}>
							<button className={styles.inline} type="button" disabled={disabled} onClick={() => setChoosingFace(true)}>Change</button>
						</span>
					</span>
				</div>
			)}
			<Field label="Name">
				<Input value={value.name} maxLength={80} autoComplete="off" disabled={disabled}
					onChange={event => set("name", event.target.value)} />
			</Field>
			<div className={styles.pair}>
				<Field label="Role">
					<Select value={value.ranksWith} disabled={disabled} onChange={event => set("ranksWith", event.target.value)}>
						<option value="">Choose a role</option>
						{POSITIONS.map(position => <option key={position} value={position}>{position}</option>)}
					</Select>
				</Field>
				<Field label="Position" optional>
					<Input value={value.title} maxLength={80} autoComplete="off" disabled={disabled} placeholder="Head of Finance"
						onChange={event => set("title", event.target.value)} />
				</Field>
			</div>
			<Field label="Practice" optional>
				<Input value={value.practice} maxLength={80} autoComplete="off" disabled={disabled} placeholder="Mergers & Acquisitions"
					onChange={event => set("practice", event.target.value)}
					onBlur={() => set("practice", normalizePractice(value.practice))} />
			</Field>
		</div>
	);
}

export function ColleagueWorkFields({ value, onChange, disabled = false }) {
	const set = (key, next) => onChange({ ...value, [key]: next });
	return (
		<div className={styles.fields}>
			<Field label="Area" optional>
				<Input value={value.area} maxLength={80} autoComplete="off" disabled={disabled} placeholder="Due diligence reporting"
					onChange={event => set("area", event.target.value)} />
			</Field>
			<Field label="What it does" optional>
				<TextArea value={value.goal} minRows={3} maxRows={6} maxLength={2000} disabled={disabled}
					onChange={event => set("goal", event.target.value)} />
			</Field>
			<Field label="How it works" optional>
				<TextArea value={value.manner} minRows={2} maxRows={4} maxLength={400} disabled={disabled}
					onChange={event => set("manner", event.target.value)} />
			</Field>
		</div>
	);
}
