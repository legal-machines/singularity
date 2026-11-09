import { useEffect, useMemo, useRef, useState } from "react";
import { FROM_EMULATION, FROM_ONBOARDING } from "../../emulation/emulation.js";
import styles from "./Lens.module.css";

// The app's own components, curated to the chapter's function. The frame fits
// its stage exactly: no camera crop, reply box or partly visible chrome. It
// plays by itself and has no controls of its own.
export function Lens({ scene, shown, paused = false, profile = null, onReady }) {
	const frame = useRef(null);
	const [ready, setReady] = useState(false);
	const held = paused || !shown;
	const source = useMemo(() => {
		const address = new URL(window.location.href);
		address.search = "?emulation"; address.hash = "";
		return address.toString();
	}, []);
	useEffect(() => {
		const receive = event => {
			if (event.source !== frame.current?.contentWindow || event.data?.from !== FROM_EMULATION) return;
			if (event.data.type === "ready") setReady(true);
		};
		window.addEventListener("message", receive);
		return () => window.removeEventListener("message", receive);
	}, []);
	useEffect(() => { if (ready) onReady?.(); }, [ready]);
	useEffect(() => {
		if (ready) frame.current?.contentWindow?.postMessage({ from: FROM_ONBOARDING, type: "playback", paused: held, settle: paused }, "*");
	}, [ready, held, paused]);
	// The member as they introduce themselves, before any scene plays with
	// them: the demonstration shows them in the firm as they type.
	useEffect(() => {
		if (ready) frame.current?.contentWindow?.postMessage({ from: FROM_ONBOARDING, type: "profile", profile }, "*");
	}, [ready, profile]);
	useEffect(() => {
		if (ready && shown) frame.current?.contentWindow?.postMessage({ from: FROM_ONBOARDING, type: "scene", scene, replay: true }, "*");
	}, [ready, shown, scene]);
	return <aside className={styles.stage} aria-hidden="true" data-shown={shown || undefined}>
		<iframe ref={frame} className={styles.frame} data-ready={ready || undefined} src={source} title="Singularity at work" tabIndex={-1} inert />
	</aside>;
}
