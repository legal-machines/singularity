import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// Singularity's pane in Word, Excel and Outlook: one page, served over HTTPS
// by Singularity on this computer (code/backend/Office/OfficeConnection), with
// Office's own script and the connection's transport loaded before it as
// classic scripts.
const deploymentRoot = dirname(fileURLToPath(import.meta.url));
const frontendRoot = resolve(deploymentRoot, "../../code/frontend");
const officeRoot = resolve(frontendRoot, "office");

// The bundle is one deferred classic script, so Office's script and the
// connection's transport, above it in the page, run first.
function classicScript() {
	return {
		name: "office-classic-script",
		enforce: "post",
		generateBundle(_options, bundle) {
			const html = bundle["index.html"];
			if (html?.type == "asset" && typeof html.source == "string") {
				html.source = html.source
					.replace(/<script type="module" crossorigin/g, "<script defer")
					.replace(/ crossorigin/g, "");
			}
		},
	};
}

export default defineConfig({
	root: officeRoot,
	cacheDir: resolve(deploymentRoot, "node_modules/.vite-office"),
	publicDir: resolve(officeRoot, "public"),
	plugins: [react(), classicScript()],
	base: "./",
	resolve: {
		alias: [
			{ find: "react-dom", replacement: resolve(deploymentRoot, "node_modules/react-dom") },
			{ find: "react", replacement: resolve(deploymentRoot, "node_modules/react") },
			{ find: "@fontsource-variable/roboto", replacement: resolve(deploymentRoot, "node_modules/@fontsource-variable/roboto") },
		],
	},
	build: {
		outDir: resolve(deploymentRoot, "dist/office"),
		emptyOutDir: true,
		target: "es2020",
		sourcemap: false,
		modulePreload: false,
		rolldownOptions: {
			output: { format: "iife" },
		},
	},
});
