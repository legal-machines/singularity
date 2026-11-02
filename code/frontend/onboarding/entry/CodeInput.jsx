import { useState } from "react";
import styles from "./CodeInput.module.css";

// One semantic input preserves OTP autofill, paste, selection and keyboard
// editing. The six visual cells are decoration, never six separate tab stops.
export function CodeInput({ value, onChange, inputRef, invalid, busy, describedBy }) {
	const [focused, setFocused] = useState(false);
	const [cursor, setCursor] = useState(0);
	const active = Math.min(cursor, 5);
	const update = (text) => onChange(text.replace(/\D/g, "").slice(0, 6));
	return (
		<div className={styles.control} data-invalid={invalid || undefined}>
			<input
				ref={inputRef} id="entry-code" name="code" className={styles.input}
				type="text" inputMode="numeric" autoComplete="one-time-code" maxLength={6}
				value={value} readOnly={busy} spellCheck={false} autoCapitalize="none"
				aria-invalid={invalid} aria-describedby={describedBy}
				onFocus={() => setFocused(true)} onBlur={() => setFocused(false)}
				onSelect={(event) => setCursor(event.currentTarget.selectionStart ?? value.length)}
				onChange={(event) => update(event.target.value)}
				onPaste={(event) => {
					const digits = event.clipboardData.getData("text").replace(/\D/g, "").slice(0, 6);
					if (!digits || busy) return;
					event.preventDefault();
					update(digits);
					setCursor(digits.length);
				}}
				onPointerDown={(event) => {
					// Clicking a filled cell selects that digit so typing replaces it.
					event.preventDefault();
					const input = event.currentTarget;
					const box = input.getBoundingClientRect();
					const at = Math.min(value.length, Math.max(0, Math.min(5, Math.floor((event.clientX - box.left) / (box.width / 6)))));
					input.focus();
					input.setSelectionRange(at, Math.min(at + 1, value.length));
					setCursor(at);
				}}
			/>
			<div className={styles.cells} aria-hidden="true">
				{Array.from({ length: 6 }, (_, at) => (
					<span key={at} className={styles.cell} data-active={focused && at === active || undefined}>{value[at] ?? ""}</span>
				))}
			</div>
		</div>
	);
}
