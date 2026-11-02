import { StrictMode } from "react";
import { createRoot } from "react-dom/client";

import { Start } from "./Start.jsx";

// Roboto, the one typeface (the owner, 2026-10-24: "strictly Roboto"),
// bundled so every system shows the same type, in every weight it has.
import "@fontsource-variable/roboto/wght.css";
// A code a member reads out or compares - this computer's - is set in IBM
// Plex Mono, letters of one width (the owner, 2026-11-02; UI-27).
import "@fontsource/ibm-plex-mono/latin-400.css";
import "@fontsource/ibm-plex-mono/latin-500.css";
import "./styles/tokens.css";
import "./styles/base.css";
import "./styles/legacy.css";

createRoot(document.getElementById("root")).render(
	<StrictMode>
		<Start />
	</StrictMode>,
);
