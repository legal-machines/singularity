import { useCallback, useEffect, useState } from "react";

import { Button } from "../Button/Button.jsx";
import { Dialog } from "../Dialog/Dialog.jsx";
import { APPLICATION, useOfficeGuideOpen } from "./OfficeGuide.jsx";
import { hasNative, nativeApp } from "../../data/native.js";

import styles from "./OfficeConnection.module.css";

// The question a pane in Word, Excel or Outlook asks before it may read
// anything (D-105), which only Singularity's own window can answer. Connecting
// them, and the panes allowed, are the guide's (OfficeGuide.jsx).

// The question a pane asks: allow it to read the Matters here and work with
// their colleagues. Shown on top of whatever page is open, with the code the
// pane shows, so the person can see it is the pane they just asked from.
export function OfficePairingQuestion() {
	const [pending, setPending] = useState([]);
	// A question the person closed without answering stays unanswered: the pane
	// keeps waiting until it ends, and only "Don't allow" refuses it.
	const [setAside, setSetAside] = useState([]);
	// The guide, when open, asks it itself.
	const isGuideOpen = useOfficeGuideOpen();

	const read = useCallback(() => {
		nativeApp().listOfficeClients().then(found => setPending(found.pending ?? []), () => {});
	}, []);

	useEffect(() => {
		if (!hasNative() || typeof nativeApp().listOfficeClients != "function") return undefined;
		read();
		const off = nativeApp().on("office.pairingRequested", read);
		const timer = setInterval(read, 15000);
		return () => {
			off();
			clearInterval(timer);
		};
	}, [read]);

	const request = pending.find(entry => !setAside.includes(entry.pairingId));
	if (request == null || isGuideOpen) return null;
	const application = APPLICATION[request.client] ?? "A program";
	const answer = async allow => {
		try {
			await nativeApp().decideOfficePairing(request.pairingId, allow);
		} finally {
			read();
		}
	};

	return (
		<Dialog
			title={`Allow ${application} to work with Singularity?`}
			description={`${application} on this computer asks to read your Matters, their threads and documents, and to give work to your colleagues, from its Singularity pane. Allow it only if the pane shows this code:`}
			size="small"
			onClose={() => setSetAside(list => [...list, request.pairingId])}
			actions={(
				<>
					<Button variant="quiet" onClick={() => answer(false)}>Don't allow</Button>
					<Button variant="primary" onClick={() => answer(true)}>Allow</Button>
				</>
			)}
		>
			<p className={styles.code}>{request.code.slice(0, 3)} {request.code.slice(3)}</p>
		</Dialog>
	);
}
