import { forwardRef } from "react";

import { Icon } from "../Icon/Icon.jsx";

import styles from "./Button.module.css";

// Folio's buttons (DESIGN.md, FOLIO-20). One per meaning:
// - primary, in ink: the one action a view leads with;
// - seal, in brick: an act that gives a person's key and cannot be undone;
// - secondary, a white button with a frame: every other action;
// - quiet, with no ground until the pointer is on it: actions in bars and rows.
// A busy button keeps its size and says it is working; a disabled one says
// nothing it cannot do.
export const Button = forwardRef(function Button(
	{ variant = "secondary", size = "medium", icon, iconEnd, busy = false, wide = false, className, children, type = "button", disabled, ...props },
	ref,
) {
	const classes = [
		styles.button,
		styles[variant],
		styles[size],
		icon != null && children != null ? styles.with_icon : null,
		iconEnd != null && children != null ? styles.with_icon_end : null,
		wide ? styles.wide : null,
		busy ? styles.busy : null,
		className,
	].filter(Boolean).join(" ");
	const iconSize = size == "small" ? 16 : 18;

	return (
		<button ref={ref} className={classes} type={type} disabled={disabled || busy} aria-busy={busy || undefined} {...props}>
			{busy ? <span className={styles.spinner} aria-hidden="true" /> : icon != null ? <Icon name={icon} size={iconSize} /> : null}
			{children != null ? <span className={styles.label}>{children}</span> : null}
			{iconEnd != null ? <Icon name={iconEnd} size={iconSize} /> : null}
		</button>
	);
});

// A button that is only an icon. Its label names it for a screen reader and
// shows under the pointer, with its key when it has one.
export const IconButton = forwardRef(function IconButton(
	{ icon, label, variant = "quiet", size = "medium", pressed, className, type = "button", ...props },
	ref,
) {
	const classes = [styles.button, styles[variant], styles[size], styles.icon_only, className].filter(Boolean).join(" ");

	return (
		<button
			ref={ref}
			className={classes}
			type={type}
			aria-label={label}
			aria-pressed={pressed}
			data-tooltip={label}
			{...props}
		>
			<Icon name={icon} size={size == "small" ? 16 : 20} />
		</button>
	);
});

// A key the member can press, drawn as a key cap.
export function Kbd({ children }) {
	return <kbd className={styles.kbd}>{children}</kbd>;
}
