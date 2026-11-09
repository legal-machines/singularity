import { StrictMode } from "react";
import { createRoot } from "react-dom/client";

import { hostInAddress, officeReady } from "./host.js";
import { OfficePane } from "./OfficePane.jsx";

// Singularity's pane in Word, Excel, PowerPoint and Outlook. Roboto and the
// window's tokens, so the pane is Singularity's in every application.
import "@fontsource-variable/roboto/wght.css";
import "../styles/tokens.css";
import "../styles/base.css";

// How long Office may take to start its script before the pane says so.
const START_LIMIT_MS = 20000;

const root = createRoot(document.getElementById("root"));
const show = (host, start) => root.render(
	<StrictMode>
		<OfficePane host={host} start={start} />
	</StrictMode>,
);
const named = hostInAddress();
if (window.Office == null) {
	// Office's script did not come from Microsoft: without the internet the pane
	// cannot start, and says so in the application that opened it.
	show(named || "none", named ? "no_office" : "ready");
} else {
	let started = false;
	const late = setTimeout(() => {
		if (!started) show(named || "none", "late");
	}, START_LIMIT_MS);
	officeReady().then(host => {
		started = true;
		clearTimeout(late);
		show(host, "ready");
	});
}
