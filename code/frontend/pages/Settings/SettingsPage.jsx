import { useEffect, useRef, useState } from "react";

import { Button } from "../../components/Button/Button.jsx";
import { MovingHead } from "../../components/MovingHead/MovingHead.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { APPLICATION, OfficeGuide } from "../../components/OfficeConnection/OfficeGuide.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { toast } from "../../components/Toast/toast.js";
import { Views } from "../../components/Views/Views.jsx";
import { useFirmComputers, watchFirmComputers } from "../../data/coordination.js";
import { hasNative, nativeApp } from "../../data/native.js";
import { recoveryCodesLine, takeChangeAsked, useTwoStepSetup } from "../../data/twoStep.js";
import { checkForUpdates, downloadUpdate, restartToUpdate, showUpdate, updateLine, useUpdates, watchUpdates } from "../../data/updates.js";

import { ApiKeys } from "./ApiKeys.jsx";
import { Group, Setting } from "./rows.jsx";
import { ChangePasswordDialog, TwoStepDialog, useOwnSeat } from "./SignInSecurity.jsx";
import { useStableGutter } from "../../components/Page/stableGutter.js";

import styles from "./SettingsPage.module.css";

// Singularity's settings on this computer, laid out as the Inbox is: its
// title on Inbox's baseline, a line under it, and its rows in groups, each a
// sign, what it is and its state, and the one thing to do with it. Opened
// from the member's menu, and with Cmd+, or Ctrl+, (App). Its API keys are a
// tab of their own, opened from the firm's menu too.
const TABS = [{ id: "general", label: "General" }, { id: "keys", label: "API keys" }];

export function SettingsPage({ tab = "general" }) {
	const { open } = useNavigation();
	const scrollRef = useRef(null);
	useStableGutter(scrollRef);
	const headRef = useRef(null);
	const [isGuideOpen, setGuideOpen] = useState(false);
	// The seat's password and two-step sign-in, its app changed and its
	// recovery codes made anew; opened on a new app when the member asked from
	// the notice of a sign-in by a recovery code.
	const [securing, setSecuring] = useState(() => (takeChangeAsked() ? "replace" : null));
	const [seatRead, setSeatRead] = useState(0);
	// Read again once two-step sign-in is set up where Legal Machines asked for it.
	const { setUps } = useTwoStepSetup();
	const ownSeat = useOwnSeat(seatRead + setUps);
	const office = useOffice(isGuideOpen);
	const version = useVersion();
	const seat = useSeat();
	const firm = useFirmComputers();
	useEffect(() => watchFirmComputers(), []);
	// Singularity's new versions, from Legal Machines' site.
	const update = useUpdates();
	useEffect(() => watchUpdates(), []);

	function copyCode() {
		navigator.clipboard?.writeText(firm.deviceCode).then(() => toast("This computer's code is copied."), () => toast("The code could not be copied. Select it and copy it by hand.", { tone: "seal" }));
	}
	function copySeat() {
		navigator.clipboard?.writeText(seat).then(() => toast("Your seat is copied."), () => toast("The seat could not be copied. Select it and copy it by hand.", { tone: "seal" }));
	}

	return (
		<div className={styles.sheet}>
			<SheetBar />
			<div ref={scrollRef} className={styles.scroll} data-fade-edge="">
				<div className={styles.feed}>
					<header ref={headRef} className={styles.head}>
						<p className={`${styles.title} ${styles.title_room}`} data-part="title" aria-hidden="true">Settings</p>
						<p className={styles.summary}>Singularity on this computer</p>
					</header>
					{hasNative() ? (
						<div className={styles.toolbar}>
							<Views label="Which settings" views={TABS} view={tab} controls="settings-tab" onChange={next => open(next == "keys" ? { kind: "settings", tab: "keys" } : { kind: "settings" })} />
						</div>
					) : null}
					{hasNative() && tab == "keys" ? <div id="settings-tab"><ApiKeys /></div> : null}
					{hasNative() && tab != "keys" ? (
						<div id="settings-tab">
							<Group title="Apps">
								<Setting
									icon="document"
									title="Word, Excel, PowerPoint and Outlook"
									line={office}
									action={<Button onClick={() => setGuideOpen(true)}>{office.startsWith("Connected") ? "Manage" : "Set up"}</Button>}
								/>
							</Group>
							{seat != null ? (
								<Group title="Your sign-in">
									<Setting
										icon="key"
										title="Your seat"
										line={<><span className={styles.code}>{seat}</span>. With your firm's email and your password, it signs you in on any computer</>}
										action={<Button onClick={copySeat}>Copy</Button>}
									/>
									<Setting
										icon="key"
										title="Password"
										line="Changed with the one you have now"
										action={<Button onClick={() => setSecuring("password")}>Change</Button>}
									/>
									{ownSeat != null ? (
										<Setting
											icon="shield"
											title="Two-step sign-in"
											line={ownSeat.twoFactor
												? "On. Each sign-in asks for the code from your authenticator app"
												: "Off. Turn it on so that signing in needs your phone too"}
											action={ownSeat.twoFactor
												? <Button onClick={() => setSecuring("replace")}>Change app</Button>
												: <Button onClick={() => setSecuring("twoStep")}>Turn on</Button>}
										/>
									) : null}
									{ownSeat?.twoFactor == true ? (
										<Setting
											icon="key"
											title="Recovery codes"
											line={recoveryCodesLine(ownSeat.recoveryCodesLeft ?? null)}
											action={<Button onClick={() => setSecuring("codes")}>New codes</Button>}
										/>
									) : null}
								</Group>
							) : null}
							<Group title="This computer">
								{firm.deviceCode != "" ? (
									<Setting
										icon="computer"
										title="This computer's code"
										line={<span className={styles.code}>{firm.deviceCode}</span>}
										action={<Button onClick={copyCode}>Copy</Button>}
									/>
								) : null}
								<Setting icon="download" title="Singularity" line={updateLine(update, version ?? "Reading its version")} action={<UpdateAction update={update} />} />
							</Group>
						</div>
					) : null}
					{hasNative() ? null : (
						<p className={styles.empty}>Settings are those of Singularity on your computer. Open it there to change them.</p>
					)}
				</div>
			</div>
			<MovingHead scrollRef={scrollRef} placeholderRef={headRef} title="Settings" onTop={() => scrollRef.current?.scrollTo({ top: 0, behavior: "smooth" })} />
			{isGuideOpen ? <OfficeGuide onClose={() => setGuideOpen(false)} /> : null}
			{securing == "password" ? <ChangePasswordDialog hasTwoFactor={ownSeat?.twoFactor == true} onClose={() => setSecuring(null)} /> : null}
			{securing == "twoStep" || securing == "replace" || securing == "codes" ? (
				<TwoStepDialog mode={securing == "twoStep" ? "on" : securing} onClose={() => setSecuring(null)} onDone={() => setSeatRead(count => count + 1)} />
			) : null}
		</div>
	);
}

// What can be done about Singularity's version now: check for a new one, or
// download the one found, or show the installer downloaded.
function UpdateAction({ update }) {
	if (hasNative() == false || typeof nativeApp().checkForUpdate != "function") return null;
	if (update.state == "available") return <Button onClick={() => downloadUpdate()}>Download</Button>;
	if (update.state == "downloading") return null;
	if (update.state == "ready") return <Button onClick={() => restartToUpdate()}>Restart now</Button>;
	if (update.state == "downloaded") return <Button onClick={() => showUpdate()}>Show in folder</Button>;
	return <Button disabled={update.state == "checking"} onClick={() => checkForUpdates({ isAsked: true })}>Check for updates</Button>;
}

// Where Word, Excel, PowerPoint and Outlook stand with Singularity, in words:
// read again when the guide closes.
function useOffice(isGuideOpen) {
	const [words, setWords] = useState("Reading the connection");
	useEffect(() => {
		if (isGuideOpen || hasNative() == false || typeof nativeApp().getOfficeStatus != "function") return;
		let isCurrent = true;
		Promise.all([nativeApp().getOfficeStatus(), nativeApp().listOfficeClients()]).then(([status, paired]) => {
			if (isCurrent == false) return;
			const apps = [...new Set((paired?.clients ?? []).map(client => APPLICATION[client.client]).filter(Boolean))];
			if (status?.supported == false) setWords("Not available on this computer");
			else if (apps.length > 0) setWords(`Connected in ${listOf(apps)}`);
			else setWords("Not set up yet: Singularity works in their side panel once they are connected");
		}, () => isCurrent && setWords("Singularity could not read the connection. Open the guide to see where it stands."));
		return () => {
			isCurrent = false;
		};
	}, [isGuideOpen]);
	return words;
}

// The seat signed in here: a login of the firm's with no person in it,
// which its member types with the firm's email to sign in elsewhere.
function useSeat() {
	const [seat, setSeat] = useState(null);
	useEffect(() => {
		if (hasNative() == false || typeof nativeApp().getAccountState != "function") return;
		nativeApp().getAccountState().then(state => setSeat(state?.signedIn ? state.seatId ?? null : null), () => {});
	}, []);
	return seat;
}

function useVersion() {
	const [version, setVersion] = useState(null);
	useEffect(() => {
		if (hasNative() == false || typeof nativeApp().getRuntimeInfo != "function") return;
		nativeApp().getRuntimeInfo().then(info => setVersion(info?.appVersion ? `Version ${info.appVersion}` : null), () => {});
	}, []);
	return version;
}

function listOf(names) {
	if (names.length < 2) return names.join("");
	return `${names.slice(0, -1).join(", ")} and ${names[names.length - 1]}`;
}
