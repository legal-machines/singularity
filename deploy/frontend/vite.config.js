import { readdirSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// Singularity's window.
const deploymentRoot = dirname(fileURLToPath(import.meta.url));
const frontendRoot = resolve(deploymentRoot, "../../code/frontend");
// The generated Native API contract (deploy/scripts/generate_native_contract.py).
const nativeApiContract = resolve(deploymentRoot, "../../docs/contracts/generated/NativeApi.json");

// The packaged window opens the page from disk (file://), where a module
// script does not run: the bundle is one classic, deferred script.
function desktopClassicScript() {
	return {
		name: "desktop-classic-script",
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

// The AI colleagues' motion manifest as a classic script beside the page, which
// sets window.__singularityCharacterMotion: the packaged window opens the page
// from disk, where WebView2 refuses fetch of a file, but a script runs
// (components/Avatar/life.js). The motion package's own files stay as shipped.
const charactersManifest = resolve(frontendRoot, "public/characters-v4/manifest.json");
const CHARACTER_MANIFEST_SCRIPT = "characters-v4-manifest.js";
function characterManifestScript() {
	const source = () => `window.__singularityCharacterMotion = ${readFileSync(charactersManifest, "utf8").trim()};\n`;
	return {
		name: "character-manifest-script",
		configureServer(server) {
			server.middlewares.use(`/${CHARACTER_MANIFEST_SCRIPT}`, (_request, response) => {
				response.setHeader("Content-Type", "text/javascript; charset=utf-8");
				response.end(source());
			});
		},
		generateBundle() {
			this.emitFile({ type: "asset", fileName: CHARACTER_MANIFEST_SCRIPT, source: source() });
		},
	};
}

// Onboarding's living painting's parts (code/frontend/onboarding/painting/parts)
// as a classic script beside the page, painting/parts.js, which sets
// window.__singularityPaintingParts to each part's WebP bytes in base64. On disk
// (file://) WebView2 counts each file as an origin of its own, so a part the
// page drew from its file tainted the canvas and WebGL refused it: on Windows
// the painting stood still as its poster. Bytes the page holds itself never
// taint (onboarding/painting/artwork.js). The parts' files are not copied beside
// the page as well.
const paintingParts = resolve(frontendRoot, "onboarding/painting/parts");
const PAINTING_PARTS_SCRIPT = "painting/parts.js";
function paintingPartsScript() {
	const source = () => {
		const parts = {};
		for (const name of readdirSync(paintingParts).filter((file) => file.endsWith(".webp")).sort()) {
			parts[name.slice(0, -".webp".length)] = readFileSync(resolve(paintingParts, name)).toString("base64");
		}
		return `window.__singularityPaintingParts = ${JSON.stringify(parts)};\n`;
	};
	return {
		name: "painting-parts-script",
		configureServer(server) {
			server.middlewares.use(`/${PAINTING_PARTS_SCRIPT}`, (_request, response) => {
				response.setHeader("Content-Type", "text/javascript; charset=utf-8");
				response.end(source());
			});
		},
		generateBundle() {
			this.emitFile({ type: "asset", fileName: PAINTING_PARTS_SCRIPT, source: source() });
		},
	};
}

// Onboarding's layout and its moving layers reload together in the window
// under development: an edit to its JSX or CSS reloads the whole page rather
// than keeping a half-updated tree and its running animations.
const onboardingRoot = resolve(frontendRoot, "onboarding");
function reloadOnboarding() {
	return {
		name: "onboarding-reload",
		apply: "serve",
		hotUpdate({ file, modules, timestamp }) {
			if (!file.startsWith(`${onboardingRoot}/`) || !/\.(jsx?|css)$/.test(file)) return;
			const invalidated = new Set();
			for (const module of modules) this.environment.moduleGraph.invalidateModule(module, invalidated, timestamp, true);
			this.environment.hot.send({ type: "full-reload", path: "*" });
			return [];
		},
	};
}

export default defineConfig({
	root: frontendRoot,
	cacheDir: resolve(deploymentRoot, "node_modules/.vite"),
	publicDir: resolve(frontendRoot, "public"),
	plugins: [reloadOnboarding(), react(), desktopClassicScript(), characterManifestScript(), paintingPartsScript()],
	base: "./",
	resolve: {
		// The source root is code/frontend while the packages are installed
		// here, so each is named exactly: one React instance.
		alias: [
			{ find: /^@native-api$/, replacement: nativeApiContract },
			{
				find: "react-dom",
				replacement: resolve(deploymentRoot, "node_modules/react-dom"),
			},
			{
				find: "react",
				replacement: resolve(deploymentRoot, "node_modules/react"),
			},
			// Roboto, the client's one typeface, bundled so it is the same on
			// every system.
			{
				find: "@fontsource-variable/roboto",
				replacement: resolve(deploymentRoot, "node_modules/@fontsource-variable/roboto"),
			},
			// IBM Plex Mono, for codes a member reads out or compares.
			{
				find: "@fontsource/ibm-plex-mono",
				replacement: resolve(deploymentRoot, "node_modules/@fontsource/ibm-plex-mono"),
			},
			// The QR code an authenticator app scans at registration.
			{
				find: "qr",
				replacement: resolve(deploymentRoot, "node_modules/qr"),
			},
		],
	},
	server: {
		host: "127.0.0.1",
		port: 5173,
		strictPort: true,
		fs: {
			allow: [frontendRoot, resolve(deploymentRoot, "node_modules"), nativeApiContract],
		},
	},
	build: {
		outDir: resolve(deploymentRoot, "dist"),
		emptyOutDir: true,
		target: "es2020",
		sourcemap: process.env.NATIVE_APP_SOURCEMAP == "1" ? "hidden" : false,
		modulePreload: false,
		rolldownOptions: {
			transform: {
				define: {
					"import.meta":
						"({ hot: false, resolve: undefined, url: document.currentScript?.src ?? location.href })",
				},
			},
			output: {
				format: "iife",
			},
		},
	},
});
