import { useEffect, useRef, useState } from "react";

import { Button } from "../../components/Button/Button.jsx";
import { Dialog } from "../../components/Dialog/Dialog.jsx";
import { Field, Input, PasswordInput } from "../../components/Field/Field.jsx";
import { toast } from "../../components/Toast/toast.js";
import { hasNative, nativeApp } from "../../data/native.js";
import { twoStepSetUp, twoStepSetupPutOff, useTwoStepSetup } from "../../data/twoStep.js";
import { Dots } from "../../onboarding/Dots.jsx";
import { CodeInput } from "../../onboarding/entry/CodeInput.jsx";
import { QrCode } from "../../onboarding/entry/QrCode.jsx";

import styles from "./SettingsPage.module.css";

// A seat's own sign-in security: whether two-step sign-in is on, read from
// Legal Machines; null while it is read or where it cannot be.
export function useOwnSeat(again) {
	const [seat, setSeat] = useState(null);
	useEffect(() => {
		if (hasNative() == false || typeof nativeApp().getOwnSeat != "function") return undefined;
		let isCurrent = true;
		nativeApp().getOwnSeat().then(found => isCurrent && setSeat(found), () => isCurrent && setSeat(null));
		return () => {
			isCurrent = false;
		};
	}, [again]);
	return seat;
}

function wordsOf(reason, fallback) {
	return reason?.detail ?? reason?.userMessage ?? reason?.message ?? fallback;
}

// The seat's own password, changed with the one it has now and, with two-step
// sign-in on, the code the authenticator app shows or a recovery code.
export function ChangePasswordDialog({ hasTwoFactor, onClose }) {
	const [current, setCurrent] = useState("");
	const [wanted, setWanted] = useState("");
	const [repeat, setRepeat] = useState("");
	const [code, setCode] = useState("");
	const [isBusy, setBusy] = useState(false);
	const [error, setError] = useState(null);
	const isShort = wanted.length > 0 && wanted.length < 12;
	const isDifferent = repeat.length > 0 && repeat != wanted;
	const canSave = current != "" && wanted.length >= 12 && repeat == wanted && (hasTwoFactor == false || code.trim() != "");

	async function save() {
		if (isBusy || canSave == false) return;
		setBusy(true);
		setError(null);
		try {
			await nativeApp().changeOwnPassword(current, wanted, hasTwoFactor ? code.trim() : undefined);
			toast("Your password is changed. Sign in with it on your other computers.");
			onClose();
		} catch (reason) {
			setError(wordsOf(reason, "The password could not be changed. Try again."));
			setBusy(false);
		}
	}

	return (
		<Dialog
			layout="spacious"
			size="small"
			title="Change your password"
			description="This computer stays signed in."
			initialFocus="input"
			onClose={onClose}
			actions={(
				<>
					<Button disabled={isBusy} onClick={onClose}>Cancel</Button>
					<Button variant="primary" busy={isBusy} disabled={canSave == false} onClick={save}>Change the password</Button>
				</>
			)}
		>
			<form className={styles.dialog_fields} onSubmit={event => { event.preventDefault(); save(); }}>
				<Field label="Current password">
					<PasswordInput autoComplete="current-password" maxLength={200} value={current} onChange={event => { setCurrent(event.target.value); setError(null); }} />
				</Field>
				<Field label="New password" error={isShort ? "At least 12 characters" : undefined}>
					<PasswordInput autoComplete="new-password" maxLength={200} value={wanted} onChange={event => { setWanted(event.target.value); setError(null); }} />
				</Field>
				<Field label="Repeat the new password" error={isDifferent ? "The two passwords differ" : undefined}>
					<PasswordInput autoComplete="new-password" maxLength={200} value={repeat} onChange={event => { setRepeat(event.target.value); setError(null); }} />
				</Field>
				{hasTwoFactor ? (
					<Field label="Code from your authenticator app, or a recovery code">
						<Input autoComplete="one-time-code" spellCheck={false} maxLength={12} value={code} onChange={event => { setCode(event.target.value); setError(null); }} />
					</Field>
				) : null}
				{error != null ? <p className={styles.dialog_error} role="alert">{error}</p> : null}
			</form>
		</Dialog>
	);
}

// The authenticator's key in groups of four, on two lines of equal length, as
// onboarding shows it.
function keyLines(secret) {
	const groups = secret.match(/.{1,4}/g) ?? [];
	const half = Math.ceil(groups.length / 2);
	return [groups.slice(0, half).join(" "), groups.slice(half).join(" ")].filter(Boolean);
}

// Copies text where the window allows it - the clipboard, or else an input's
// own copy - as onboarding does; true when it did.
async function copyText(text) {
	try {
		await navigator.clipboard.writeText(text);
		return true;
	} catch {
		const field = Object.assign(document.createElement("input"), { value: text, readOnly: true });
		Object.assign(field.style, { position: "fixed", opacity: "0", pointerEvents: "none" });
		document.body.append(field);
		field.select();
		const copied = document.execCommand?.("copy") == true;
		field.remove();
		return copied;
	}
}

// The steps of each way two-step sign-in is set, as onboarding sets it: one
// step a page, with where the member is in the dots at the foot.
const STEP_NAMES = {
	confirm: "Confirm it’s you",
	key: "Authenticator app",
	code: "Code from the app",
	codes: "Recovery codes",
};
const FLOWS = {
	on: { steps: ["key", "code", "codes"], label: "Turning on two-step sign-in" },
	replace: { steps: ["confirm", "key", "code", "codes"], label: "Changing your authenticator app" },
	codes: { steps: ["confirm", "codes"], label: "Making new recovery codes" },
};

// Two-step sign-in set from Settings as onboarding sets it. Turned on ("on"):
// the authenticator's QR code and its key in groups of four that copies itself
// when pressed; then the six digits the app shows, in onboarding's own six
// cells; then the recovery codes, kept by printing, saving or copying them
// before the member may say they did. A new app ("replace") and new recovery
// codes ("codes") first ask the member to confirm it is them - their password
// and a code the app shows or a recovery code, so that a lost phone is no lock
// - and then go on the same way. The current app keeps working until the new
// one's code is right, and the codes the member had stop once new ones are
// made.
//
// `isNeeded`: Legal Machines asked for it before anything else, and the first
// step says so.
export function TwoStepDialog({ mode = "on", isNeeded = false, onClose, onDone }) {
	const flow = FLOWS[mode] ?? FLOWS.on;
	const [at, setAt] = useState(0);
	const step = flow.steps[at];
	const [password, setPassword] = useState("");
	const [proof, setProof] = useState("");
	const [key, setKey] = useState(null);
	const [keyTry, setKeyTry] = useState(0);
	const [isKeyCopied, setKeyCopied] = useState(false);
	const [code, setCode] = useState("");
	const [codes, setCodes] = useState([]);
	const [isKept, setKept] = useState(false);
	const [kept, setKeptWord] = useState(null);
	const [isBusy, setBusy] = useState(false);
	const [error, setError] = useState(null);
	const [organisationId, setOrganisationId] = useState(null);
	const codeRef = useRef(null);
	const isNew = mode != "on";

	// Turning it on asks for the app's key at once; a new app's key comes once
	// the member has confirmed it is them.
	useEffect(() => {
		if (mode != "on") return undefined;
		let isCurrent = true;
		setError(null);
		nativeApp().startOwnTwoFactor().then(found => isCurrent && setKey(found),
			reason => isCurrent && setError(wordsOf(reason, "Two-step sign-in could not be set up. Try again.")));
		return () => {
			isCurrent = false;
		};
	}, [mode, keyTry]);

	useEffect(() => {
		let isCurrent = true;
		nativeApp().getAccountState?.().then(state => isCurrent && setOrganisationId(state?.organisationId ?? null), () => {});
		return () => {
			isCurrent = false;
		};
	}, []);

	// Each step's own field takes the keys as it comes.
	useEffect(() => {
		if (step == "code") codeRef.current?.focus();
	}, [step]);

	function go(next) {
		setError(null);
		setAt(flow.steps.indexOf(next));
	}

	// Once new codes are made, Settings reads the seat again, however the
	// dialog closes.
	function close() {
		if (codes.length > 0) onDone();
		onClose();
	}

	const canProve = password != "" && proof.trim() != "";
	async function prove() {
		if (isBusy || canProve == false) return;
		setBusy(true);
		setError(null);
		try {
			if (mode == "replace") {
				setKey(await nativeApp().replaceOwnTwoFactor(password, proof.trim()));
				go("key");
			} else {
				const made = await nativeApp().renewOwnRecoveryCodes(password, proof.trim());
				setCodes(made?.recoveryCodes ?? []);
				go("codes");
			}
			// What proved it is them is not kept a moment longer than it was needed.
			setPassword("");
			setProof("");
		} catch (reason) {
			setError(wordsOf(reason, mode == "replace" ? "The new app could not be set up. Try again." : "New recovery codes could not be made. Try again."));
		}
		setBusy(false);
	}

	async function confirm() {
		if (isBusy || code.length != 6) return;
		setBusy(true);
		setError(null);
		try {
			const on = mode == "replace" ? await nativeApp().confirmReplacedTwoFactor(code) : await nativeApp().confirmOwnTwoFactor(code);
			setCodes(on?.recoveryCodes ?? []);
			go("codes");
		} catch (reason) {
			const words = wordsOf(reason, "That code doesn't match. Try the one the app shows now.");
			// A new app's key lasts half an hour: past that, it starts again.
			if (mode == "replace" && words.startsWith("Start again")) {
				setKey(null);
				setCode("");
				setAt(0);
			}
			setError(words);
		}
		setBusy(false);
	}

	async function copyKey() {
		if (await copyText(key.secret)) setKeyCopied(true);
		else toast("The key could not be copied. Type it into the app by hand.", { tone: "seal" });
	}

	async function keep(how) {
		setError(null);
		try {
			if (how == "copy") {
				// Never the browser's own words: what to do instead.
				if (await copyText(codes.join("\n")) == false) {
					setError("The codes couldn't be copied. Print them or save them as a file.");
					return;
				}
				setKeptWord("Codes copied");
			} else if (how == "save") {
				const saved = await nativeApp().saveLegalMachinesRecoveryCodes(organisationId);
				if (saved?.saved != true) return;
				setKeptWord("Codes saved to a file");
			} else {
				const printed = await nativeApp().printLegalMachinesRecoveryCodes(organisationId);
				if (printed?.printed != true) return;
				setKeptWord("Codes opened for printing");
			}
			setKept(true);
		} catch (reason) {
			setError(wordsOf(reason, "That did not work. Try another way to keep them."));
		}
	}

	const pages = {
		confirm: {
			title: mode == "replace" ? "Change your authenticator app" : "Make new recovery codes",
			line: mode == "replace" ? "Confirm it’s you to set up a new app" : "Confirm it’s you to replace your codes",
			focus: "input",
			body: (
				// Enter in either field sends the step, as onboarding's do.
				<div className={styles.dialog_fields} onKeyDown={event => { if (event.key == "Enter") { event.preventDefault(); prove(); } }}>
					<Field label="Password">
						<PasswordInput autoComplete="current-password" maxLength={200} value={password} disabled={isBusy}
							onChange={event => { setPassword(event.target.value); setError(null); }} />
					</Field>
					<Field label="Code from your authenticator app, or a recovery code">
						<Input autoComplete="one-time-code" spellCheck={false} maxLength={12} value={proof} disabled={isBusy}
							onChange={event => { setProof(event.target.value); setError(null); }} />
					</Field>
				</div>
			),
			primary: <Button variant="primary" busy={isBusy} disabled={canProve == false} onClick={prove}>{mode == "replace" ? "Continue" : "Make new codes"}</Button>,
		},
		key: {
			title: isNew ? "Set up the new app" : isNeeded ? "Set up two-step sign-in to go on" : "Turn on two-step sign-in",
			line: isNew ? "Scan this with your new authenticator app" : "Scan this with an authenticator app",
			focus: "button",
			body: (
				<>
					<div className={styles.qr}>
						{key != null ? <QrCode text={key.uri} label="QR code for your authenticator app" /> : null}
						{key == null && error != null && mode == "on" ? <button className={styles.text_button} type="button" onClick={() => setKeyTry(count => count + 1)}>Try again</button> : null}
					</div>
					{key != null ? (
						<div className={styles.key}>
							<span id="settings-key-label" aria-live="polite">{isKeyCopied ? "Key copied" : "Or enter this key"}</span>
							<button className={styles.key_value} type="button" title="Copy the key" aria-describedby="settings-key-label" onClick={copyKey}>
								{keyLines(key.secret).map((line, index) => <span key={index}>{line}</span>)}
							</button>
						</div>
					) : null}
				</>
			),
			primary: <Button variant="primary" disabled={key == null} onClick={() => go("code")}>I’ve added the code</Button>,
		},
		code: {
			title: isNew ? "Enter the new app’s code" : "Enter the code from the app",
			line: isNew ? "The new app shows six digits, new every 30 seconds" : "Your authenticator app shows six digits, new every 30 seconds",
			body: (
				<div className={styles.code_cells}>
					<CodeInput value={code} onChange={value => { setCode(value); setError(null); }} inputRef={codeRef} invalid={error != null} busy={isBusy} />
				</div>
			),
			primary: <Button variant="primary" busy={isBusy} disabled={code.length != 6} onClick={confirm}>{isNew ? "Use the new app" : "Turn on two-step sign-in"}</Button>,
		},
		codes: {
			title: isNew ? "Save your new recovery codes" : "Save your recovery codes",
			line: isNew ? "Each signs you in once. Old codes no longer work" : "If you lose your authenticator app, each code signs you in once",
			body: (
				<>
					<ol className={styles.recovery_codes} aria-label="Recovery codes">
						{codes.map(item => <li key={item}>{item}</li>)}
					</ol>
					<div className={styles.keep}>
						{organisationId != null ? <button className={styles.text_button} type="button" onClick={() => keep("print")}>Print</button> : null}
						{organisationId != null ? <button className={styles.text_button} type="button" onClick={() => keep("save")}>Save as file</button> : null}
						<button className={styles.text_button} type="button" onClick={() => keep("copy")}>Copy</button>
					</div>
					<p className={styles.kept} aria-live="polite">{kept ?? ""}</p>
				</>
			),
			primary: <Button variant="primary" disabled={isKept == false} onClick={close}>I’ve saved the codes</Button>,
		},
	};
	const page = pages[step];
	// Before anything changed, the member may leave; going back is only from
	// the app's code to its key.
	const canCancel = step == "confirm" || step == "key";

	function submit(event) {
		event.preventDefault();
		if (step == "code") confirm();
	}

	return (
		<Dialog
			layout="spacious"
			size="small"
			title={page.title}
			description={page.line}
			initialFocus={at == 0 ? page.focus : undefined}
			onClose={close}
			actions={(
				<>
					<Dots
						className={styles.steps}
						steps={flow.steps.map(name => STEP_NAMES[name])}
						current={at}
						label={flow.label}
						canGo={index => step == "code" && flow.steps[index] == "key" && isBusy == false}
						onGo={index => go(flow.steps[index])}
					/>
					{canCancel ? <Button disabled={isBusy} onClick={close}>Cancel</Button> : null}
					{step == "code" ? <Button disabled={isBusy} onClick={() => go("key")}>Back</Button> : null}
					{page.primary}
				</>
			)}
		>
			<form className={step == "confirm" ? `${styles.two_step} ${styles.two_step_form}` : styles.two_step} onSubmit={submit}>
				{page.body}
				{error != null ? <p className={styles.dialog_error} role="alert">{error}</p> : null}
			</form>
		</Dialog>
	);
}

// The setup Legal Machines asks for before anything else, wherever the member
// is when it is met (data/twoStep.js): the same steps as Settings'. Closed
// before two steps are on, it is put off and waits as a notice.
export function TwoStepSetupHost() {
	const setup = useTwoStepSetup();
	const isDone = useRef(false);
	if (setup.isOpen == false) return null;
	return (
		<TwoStepDialog
			isNeeded
			onDone={() => {
				isDone.current = true;
				twoStepSetUp();
			}}
			onClose={() => {
				if (isDone.current == false) twoStepSetupPutOff();
				isDone.current = false;
			}}
		/>
	);
}
