import { useEffect, useRef, useState } from "react";

import { Button } from "../../components/Button/Button.jsx";
import { Dialog } from "../../components/Dialog/Dialog.jsx";
import { Field, Input } from "../../components/Field/Field.jsx";
import { toast } from "../../components/Toast/toast.js";
import { viewerMay } from "../../data/workspace.js";
import { PROVIDERS, isConnected, reasonOf } from "../../onboarding/tour/providers.js";
import { protectionDetail, protectionWords, setupOf } from "../../onboarding/tour/setup.js";

import { Group, Setting } from "./rows.jsx";
import styles from "./SettingsPage.module.css";

// Settings' API keys (the owner, 2026-11-02: "write about API keys, and lead
// to a tab inside Settings"): the model accounts the firm's AI colleagues
// think with, kept here for good (D-134 asks for one on its second slide;
// 2026-10-29: a lawyer who answered "Later" there had no way to connect one).
// A key goes to the native side, which checks it with its service before it
// keeps it, and never comes back to the page (AG-SEC-1). Client data
// protection's state stands under them: client data is protected before any
// of it reaches an external model. Only the firm's administrator connects an
// account (D-184): a colleague who joined reads what is connected here.
export function ApiKeys() {
	const setup = useRef(setupOf(window.nativeApp)).current;
	const [connected, setConnected] = useState([]);
	const [choosing, setChoosing] = useState(null);
	const [protection, setProtection] = useState(null);

	useEffect(() => {
		let current = true;
		setup.providers().then(list => current && setConnected(list ?? []), () => {});
		return () => {
			current = false;
		};
	}, [setup]);
	// The protection model's download, read every two seconds while the tab
	// shows it (the backend sends no event for it).
	useEffect(() => {
		let current = true;
		let timer = null;
		const read = () => {
			setup.protection().then(status => current && setProtection(status), () => {}).finally(() => {
				if (current) timer = setTimeout(read, 2000);
			});
		};
		read();
		return () => {
			current = false;
			clearTimeout(timer);
		};
	}, [setup]);

	const may = viewerMay.connectModels == true;
	const shown = may ? PROVIDERS : PROVIDERS.filter(provider => isConnected(provider, connected));
	const words = protectionWords(protection);
	const detail = protectionDetail(protection);
	// Nothing downloads until someone asks (D-198): a model not here yet is
	// downloaded from here, unless signing in comes first.
	const protectionAction = protection?.status == "error" || (protection?.status == "available" && protection?.recovery != "sign_in")
		? <Button onClick={() => setup.downloadProtection().catch(() => {})}>{protection.status == "error" ? "Try again" : "Download"}</Button>
		: null;

	return (
		<>
			<Group
				title="Model accounts"
				line={may
					? "The accounts your AI colleagues think with. Add a key, and they can take the work you give them."
					: "The accounts the firm's AI colleagues think with. The firm's administrator connects them, on the computer where the colleagues work."}
			>
				{shown.length == 0 ? <li className={styles.none}>No account is connected on this computer, and none needs to be: the firm's AI colleagues think with the accounts the administrator connected.</li> : null}
				{shown.map(provider => {
					const isOn = isConnected(provider, connected);
					return (
						<Setting
							key={provider.key}
							title={provider.name}
							line={provider.line}
							action={isOn
								? <span className={styles.connected}>Connected</span>
								: may ? <Button onClick={() => setChoosing(provider)}>Add key</Button> : null}
						/>
					);
				})}
			</Group>
			{words ? (
				<Group title="Client data protection">
					<Setting
						title="Before anything reaches an external model"
						line={<span aria-live="polite">{words}{detail != "" ? <span className={styles.line_more}>{detail}</span> : null}</span>}
						action={protectionAction}
					/>
				</Group>
			) : null}
			{choosing != null && may ? (
				<ConnectDialog
					provider={choosing}
					setup={setup}
					onClose={() => setChoosing(null)}
					onConnected={list => {
						setConnected(list);
						toast(`${choosing.name} is connected. Your AI colleagues can use it now.`);
						setChoosing(null);
					}}
				/>
			) : null}
		</>
	);
}

// The key of one model account, checked with its service before it is kept.
function ConnectDialog({ provider, setup, onClose, onConnected }) {
	const [key, setKey] = useState("");
	const [error, setError] = useState(null);
	const [isChecking, setChecking] = useState(false);

	async function connect() {
		if (isChecking) return;
		if (key.trim() == "") {
			setError("Paste the API key.");
			return;
		}
		setChecking(true);
		setError(null);
		try {
			const list = await setup.connect(provider, key.trim());
			onConnected(Array.isArray(list) ? list : await setup.providers());
		} catch (failure) {
			setError(reasonOf(failure));
			setChecking(false);
		}
	}

	return (
		<Dialog
			title={`Connect ${provider.name}`}
			description={`Paste an API key of your firm's ${provider.name} account. Singularity checks it with ${provider.name}, keeps it on this computer only, and never shows it again.`}
			initialFocus="input"
			onClose={onClose}
			actions={(
				<>
					<Button onClick={onClose}>Cancel</Button>
					<Button variant="primary" busy={isChecking} disabled={key.trim() == ""} onClick={connect}>{isChecking ? "Checking" : "Connect"}</Button>
				</>
			)}
		>
			<Field label="API key" error={error ?? undefined}>
				<Input
					type="password"
					value={key}
					spellCheck={false}
					autoComplete="off"
					onChange={event => {
						setKey(event.target.value);
						setError(null);
					}}
					onKeyDown={event => {
						if (event.key == "Enter") connect();
					}}
				/>
			</Field>
		</Dialog>
	);
}
