import { useEffect, useRef, useState } from "react";

import { Avatar } from "../Avatar/Avatar.jsx";
import { Button } from "../Button/Button.jsx";
import { Dialog } from "../Dialog/Dialog.jsx";
import { Menu, useMenu } from "../Menu/Menu.jsx";
import { useNavigation } from "../Navigation/navigation.js";
import { toast } from "../Toast/toast.js";
import { useFirmComputers, watchFirmComputers } from "../../data/coordination.js";
import { hasNative, nativeApp } from "../../data/native.js";
import { organisation, viewerId } from "../../data/workspace.js";
import { findMember } from "../../pages/words.js";

import styles from "./AccountMenu.module.css";

// The member at this computer, at the desk's foot. Its menu holds only what
// is the member's own here: Settings, this computer's code, and signing out.
// Time off and pausing notifications are gone, and Word, Excel, PowerPoint
// and Outlook moved to Settings.
export function AccountMenu() {
	const menu = useMenu();
	const { open } = useNavigation();
	const firm = useFirmComputers();
	const [isSigningOut, setSigningOut] = useState(false);
	// This computer's code stands where its name was for a moment after a
	// press, as it is copied.
	const [isCodeShown, setCodeShown] = useState(false);
	const codeTimer = useRef(0);
	useEffect(() => () => clearTimeout(codeTimer.current), []);
	const [isLeaving, setLeaving] = useState(false);
	const viewer = findMember(viewerId);
	// The code is read with the firm's computers; reading begins once.
	useEffect(() => watchFirmComputers(), []);
	const office = viewer.position ?? viewer.role;

	function copyCode(code) {
		setCodeShown(true);
		clearTimeout(codeTimer.current);
		codeTimer.current = setTimeout(() => setCodeShown(false), 2400);
		navigator.clipboard?.writeText(code).then(() => toast("This computer's code is copied."), () => toast("The code could not be copied. Select it and copy it by hand.", { tone: "seal" }));
	}

	async function signOut() {
		setLeaving(true);
		try {
			await nativeApp().signOutOfLegalMachines();
		} catch {
			// Signed out here whatever Legal Machines answered.
		}
		window.location.reload();
	}

	const items = [
		{ key: "settings", label: "Settings", icon: "settings", onSelect: () => open({ kind: "settings" }) },
		// This computer's code, always where its member finds it: a colleague
		// compares it before their computer counts what this one signs as this
		// member's, and before their AI colleagues take up what this member
		// asks.
		hasNative() && firm.deviceCode != "" ? { key: "code", label: isCodeShown ? firm.deviceCode : "This computer's code", isCode: isCodeShown, icon: "computer", keepsOpen: true, onSelect: () => copyCode(firm.deviceCode) } : null,
		// Signing out of the firm on this computer: the window opens at the
		// entry, where this or another member signs in.
		hasNative() ? { key: "sign_out", label: `Sign out of ${organisation.shortName}`, icon: "power", onSelect: () => setSigningOut(true) } : null,
	].filter(Boolean);

	return (
		<div className={styles.account}>
			<button
				className={styles.me}
				type="button"
				aria-haspopup="menu"
				aria-expanded={menu.isOpen}
				onClick={event => menu.toggle(event.currentTarget)}
			>
				<Avatar member={viewer} size={36} />
				<span className={styles.me_text}>
					<span className={styles.me_name}>{viewer.name}</span>
					<span className={styles.me_line}>{office}</span>
				</span>
			</button>
			{menu.isOpen ? <Menu anchor={menu.anchor} items={items} label="Your account" placement="top-start" matchAnchorWidth onClose={menu.close} /> : null}
			{/* Signing out is asked once more. */}
			{isSigningOut ? (
				<Dialog
					title={`Sign out of ${organisation.shortName}?`}
					description="Singularity opens at the sign-in, and you sign in again with your password and your authenticator app. Your Matters and their documents stay on this computer."
					size="small"
					onClose={() => setSigningOut(false)}
					actions={(
						<>
							<Button onClick={() => setSigningOut(false)}>Cancel</Button>
							<Button variant="primary" busy={isLeaving} onClick={signOut}>Sign out</Button>
						</>
					)}
				/>
			) : null}
		</div>
	);
}
