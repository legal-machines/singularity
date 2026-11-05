import { useEffect, useState } from "react";

import { App } from "./App.jsx";
import { AppBehaviour } from "./components/Window/AppBehaviour.jsx";
import { Emulation } from "./emulation/Emulation.jsx";
import { isEmulation } from "./emulation/emulation.js";
import { Onboarding } from "./onboarding/Onboarding.jsx";
import { completeOnboarding, firstView, viewOf, waitsForFirm } from "./onboarding/start.js";
import { recordVersionSeen } from "./data/updates.js";

// The window's first choice (D-133): onboarding until it is complete, then the
// app. The app is not mounted while onboarding shows, so none of its work
// (reading the Matter, watching the firm's computers, Office) starts before it.
// The frame onboarding shows the app at work in is the app's emulation, which
// never asks the bridge anything (D-134).
export function Start() {
	return isEmulation ? <Emulation /> : <Window />;
}

function Window() {
	const [view, setView] = useState(() => firstView(window.nativeApp, window.location.search));

	useEffect(() => {
		if (view != "asking") return undefined;
		let current = true;
		window.nativeApp.getOnboardingStatus().then(
			async (status) => {
				const next = viewOf(status);
				// A colleague's new computer that opens still new to the firm waits
				// for it before the app (D-205).
				const isHeld = next == "app" ? await waitsForFirm(window.nativeApp) : false;
				if (current) setView(isHeld ? "firm" : next);
			},
			() => {
				if (!current) return;
				// The window may have opened at onboarding's size: give it
				// the app's before the app shows.
				window.nativeApp.prepareMainWindow?.().catch(() => {});
				setView("app");
			},
		);
		return () => {
			current = false;
		};
	}, [view]);

	if (view == "asking") return null;
	if (view == "firm") {
		return (
			<>
				<Onboarding hold onComplete={() => recordVersionSeen().finally(() => setView("app"))} />
				<AppBehaviour />
			</>
		);
	}
	if (view == "onboarding") {
		// Onboarding is the application too: no find, reload, zoom or other
		// browser behaviour reaches the member there either (the owner,
		// 2026-11-01: "cut Ctrl+F and Cmd+F, the browser's search").
		return (
			<>
				<Onboarding
					onComplete={async () => {
						await completeOnboarding(window.nativeApp);
						// Set up with this version: never told it was updated to it.
						await recordVersionSeen();
						setView("app");
					}}
				/>
				<AppBehaviour />
			</>
		);
	}
	return <App />;
}
