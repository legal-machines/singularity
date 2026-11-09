import { useState } from "react";
import { Popover } from "../Popover/Popover.jsx";
import { monogram, projectColors, setProjectLook, useProjectLook } from "./looks.js";
import { projectIcons, projectIconById } from "./projectIcons.js";

import styles from "./ProjectMark.module.css";

// A project's mark: a circle in the project's colour, carrying its initials
// or an icon. A member's face is a square with G2 corners; a project's mark
// is a circle, so the two never read as one.
export function ProjectMark({ matter, size = 36 }) {
	const look = useProjectLook(matter);
	return <Mark name={matter.name} color={look.color} icon={look.icon} size={size} />;
}

// A project's band: its mark and its name in its colour, where a list of its
// threads and mail begins. A press opens the project; a band with nothing to
// open, such as Unsorted, is only words, and `sign` draws its mark's sign in
// place of initials or an icon.
export function ProjectBand({ matter, onClick, sign }) {
	const look = useProjectLook(matter);
	const Tag = onClick != null ? "button" : "span";
	return (
		<Tag className={styles.band} type={onClick != null ? "button" : undefined} style={{ "--project": look.color }} onClick={onClick}>
			<Mark name={matter.name} color={look.color} icon={look.icon} sign={sign} size={20} />
			<span className={styles.band_name}>{matter.name}</span>
		</Tag>
	);
}

function Mark({ name, color, icon, sign, size }) {
	if (sign != null) {
		return <span className={styles.mark} style={{ width: size, height: size, background: color }} aria-hidden="true">{sign}</span>;
	}
	const drawing = icon != null ? projectIconById[icon] : null;
	const iconSize = Math.round(size * 0.56);
	const stroke = (Math.max(1.25, iconSize * 0.08) * 24) / iconSize;
	return (
		<span className={styles.mark} style={{ width: size, height: size, background: color }} aria-hidden="true">
			{drawing != null ? (
				<svg className={styles.icon} viewBox="0 0 24 24" width={iconSize} height={iconSize} fill="none" stroke="currentColor" strokeWidth={stroke} strokeLinecap="round" strokeLinejoin="round">
					<path d={drawing.path} />
				</svg>
			) : (
				<span className={styles.initials} style={{ fontSize: Math.round(size * 0.42) }}>{monogram(name)}</span>
			)}
		</span>
	);
}

// Where a member changes a project's mark: its colour, then its initials or
// one of the icons.
export function MarkPicker({ matter, anchor, onClose }) {
	const look = useProjectLook(matter);
	return (
		<Popover anchor={anchor} onClose={onClose} label={`${matter.name}: mark`} width={364} placement="bottom-start">
			<MarkChoices name={matter.name} look={look} onChoose={change => setProjectLook(matter.id, change)} />
		</Popover>
	);
}

// A mark chosen before its Matter exists: the mark as it will be, its
// initials following the name as it is typed; pressed, the same choices as a
// Matter's own.
export function MarkChooser({ name, look, onChange }) {
	const [anchor, setAnchor] = useState(null);
	return (
		<>
			<button className={styles.chooser} type="button" aria-label="Choose its mark" data-tooltip="Choose its mark"
				aria-expanded={anchor != null} onClick={event => setAnchor(anchor == null ? event.currentTarget : null)}>
				<Mark name={name} color={look.color} icon={look.icon} size={44} />
			</button>
			{anchor != null ? (
				<Popover anchor={anchor} onClose={() => setAnchor(null)} label="Its mark" width={364} placement="bottom-start">
					<MarkChoices name={name} look={look} onChoose={change => onChange({ ...look, ...change })} />
				</Popover>
			) : null}
		</>
	);
}

// The choices of a mark: its colour, then its initials or one of the icons.
function MarkChoices({ name, look, onChoose }) {
	return (
		<div className={styles.picker}>
			<div className={styles.colors} role="radiogroup" aria-label="Colour">
				{projectColors.map(color => (
					<button
						key={color}
						className={styles.color}
						type="button"
						role="radio"
						aria-checked={look.color == color}
						aria-label={color}
						style={{ "--swatch": color }}
						onClick={() => onChoose({ color })}
					/>
				))}
			</div>
			<div className={styles.choices} role="radiogroup" aria-label="Initials or an icon">
				<button
					className={styles.choice}
					type="button"
					role="radio"
					aria-checked={look.icon == null}
					aria-label="Initials"
					title="Initials"
					onClick={() => onChoose({ icon: null })}
				>
					<Mark name={name} color={look.color} icon={null} size={32} />
				</button>
				{projectIcons.map(icon => (
					<button
						key={icon.id}
						className={styles.choice}
						type="button"
						role="radio"
						aria-checked={look.icon == icon.id}
						aria-label={icon.label}
						title={icon.label}
						onClick={() => onChoose({ icon: icon.id })}
					>
						<Mark name={name} color={look.color} icon={icon.id} size={32} />
					</button>
				))}
			</div>
		</div>
	);
}
