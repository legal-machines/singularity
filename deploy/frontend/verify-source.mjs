import { build } from "vite";
import { fileURLToPath } from "node:url";

try {
	await build({
		configFile: fileURLToPath(new URL("./vite.config.js", import.meta.url)),
		build: {
			write: false,
		},
	});
} catch (error) {
	process.exitCode = 1;
	throw error;
}
