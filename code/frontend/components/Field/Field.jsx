import { cloneElement, forwardRef, isValidElement, useId, useLayoutEffect, useRef } from "react";

import { SecretInput } from "./SecretInput.jsx";

import styles from "./Field.module.css";

// A field: its label above, its control, and one line below that either
// helps or, when something is wrong, says what and how to put it right.
// The control learns its id and what describes it.
export function Field({ label, hint, error, optional = false, className, children }) {
	const id = useId();
	const noteId = useId();
	const control = isValidElement(children)
		? cloneElement(children, {
			id: children.props.id ?? id,
			"aria-describedby": hint != null || error != null ? noteId : undefined,
			"aria-invalid": error != null ? true : undefined,
		})
		: children;

	return (
		<div className={[styles.field, className].filter(Boolean).join(" ")}>
			{label != null ? (
				<label className={styles.label} htmlFor={children?.props?.id ?? id}>
					{label}
					{optional ? <span className={styles.optional}>Optional</span> : null}
				</label>
			) : null}
			{control}
			{error != null ? <p id={noteId} className={styles.error}>{error}</p> : hint != null ? <p id={noteId} className={styles.hint}>{hint}</p> : null}
		</div>
	);
}

export const Input = forwardRef(function Input({ className, size = "medium", ...props }, ref) {
	return <input ref={ref} className={[styles.control, size == "small" ? styles.control_small : null, className].filter(Boolean).join(" ")} {...props} />;
});

// A password (kind "password") or an API key (kind "key") in a field's look,
// with its eye (SecretInput.jsx).
export const PasswordInput = forwardRef(function PasswordInput({ className, size = "medium", kind = "password", ...props }, ref) {
	return <SecretInput ref={ref} kind={kind} className={[styles.control, size == "small" ? styles.control_small : null, className].filter(Boolean).join(" ")} {...props} />;
});

// A text area grows with what is written in it, up to `maxRows`.
export const TextArea = forwardRef(function TextArea({ className, minRows = 2, maxRows = 10, value, ...props }, ref) {
	const own = useRef(null);
	const element = ref ?? own;

	useLayoutEffect(() => {
		const area = typeof element == "function" ? null : element.current;
		if (area == null) return;
		const line = parseFloat(getComputedStyle(area).lineHeight) || 21;
		const padding = parseFloat(getComputedStyle(area).paddingTop) + parseFloat(getComputedStyle(area).paddingBottom);
		area.style.height = "auto";
		const height = Math.min(Math.max(area.scrollHeight, minRows * line + padding), maxRows * line + padding);
		area.style.height = `${height + 2}px`;
	}, [value, minRows, maxRows, element]);

	return <textarea ref={element} className={[styles.control, styles.area, className].filter(Boolean).join(" ")} value={value} rows={minRows} {...props} />;
});

export const Select = forwardRef(function Select({ className, size = "medium", children, ...props }, ref) {
	return (
		<select ref={ref} className={[styles.control, styles.select, size == "small" ? styles.control_small : null, className].filter(Boolean).join(" ")} {...props}>
			{children}
		</select>
	);
});

// A check box with its words beside it; the whole line can be pressed.
export function Checkbox({ label, description, checked, onChange, disabled, tone }) {
	return (
		<label className={[styles.check, disabled ? styles.check_disabled : null].filter(Boolean).join(" ")}>
			<input
				className={styles.check_box}
				type="checkbox"
				checked={checked}
				disabled={disabled}
				onChange={event => onChange?.(event.target.checked)}
				style={tone != null ? { "--check-tone": tone } : undefined}
			/>
			<span className={styles.check_text}>
				<span>{label}</span>
				{description != null ? <span className={styles.check_description}>{description}</span> : null}
			</span>
		</label>
	);
}
