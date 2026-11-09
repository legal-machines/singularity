import { forwardRef, useState } from "react";

import { Icon } from "../Icon/Icon.jsx";

import styles from "./SecretInput.module.css";

// A password or an API key, typed out of sight, with an eye at its end that
// shows what was typed while it is on.
// - A password stays a password field, its autocomplete naming which one it is
//   (current-password, new-password), so the system's password manager and
//   any other know it for what it is. It is shown as text only while the eye
//   is on.
// - An API key is never a password field: a password manager would offer to
//   keep it in a store of its own, beside Singularity's one store of keys. It
//   is plain text with its letters masked, and every password manager is told
//   to leave it alone. The input keeps the look of where it stands:
//   `className` is its own class.
export const SecretInput = forwardRef(function SecretInput({ kind = "password", className, disabled, ...props }, ref) {
	const [isShown, setShown] = useState(false);
	const isKey = kind == "key";
	const what = isKey ? "key" : "password";
	const own = isKey
		? {
			type: "text",
			autoComplete: "off",
			autoCorrect: "off",
			autoCapitalize: "none",
			spellCheck: false,
			"data-1p-ignore": "true",
			"data-lpignore": "true",
			"data-bwignore": "true",
			"data-form-type": "other",
		}
		: { type: isShown ? "text" : "password", spellCheck: false, autoCapitalize: "none", autoCorrect: "off" };
	return (
		<span className={styles.secret}>
			<input
				ref={ref}
				{...props}
				{...own}
				disabled={disabled}
				className={[className, styles.input, isKey && !isShown ? styles.masked : null].filter(Boolean).join(" ")}
			/>
			<button
				type="button"
				className={styles.eye}
				aria-label={isShown ? `Hide the ${what}` : `Show the ${what}`}
				aria-pressed={isShown}
				data-tooltip={isShown ? "Hide" : "Show"}
				disabled={disabled}
				// The caret stays in the field as the eye is pressed.
				onMouseDown={event => event.preventDefault()}
				onClick={() => setShown(value => !value)}
			>
				<Icon name={isShown ? "eye_off" : "eye"} size={18} />
			</button>
		</span>
	);
});
