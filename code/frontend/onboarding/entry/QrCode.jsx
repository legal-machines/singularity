import encodeQR from "qr";
import { useMemo } from "react";

// The quiet margin a scanner needs around the code, in modules, and the size
// the code is drawn at: whole pixels a module, about 168 pixels across.
const QUIET = 4;
const ABOUT = 168;

// The modules of `text` as a QR code, medium error correction, its quiet
// margin included: true is dark.
export function qrModules(text) {
	return encodeQR(text, "raw", { ecc: "medium", border: QUIET });
}

// One path of the dark modules, a row's adjacent ones joined.
export function qrPath(modules) {
	let path = "";
	modules.forEach((row, y) => {
		for (let x = 0; x < row.length; x += 1) {
			if (!row[x]) continue;
			let end = x;
			while (end + 1 < row.length && row[end + 1]) end += 1;
			path += `M${x} ${y}h${end - x + 1}v1h-${end - x + 1}z`;
			x = end;
		}
	});
	return path;
}

// A QR code an authenticator app scans, in the page's ink on white, crisp at
// any scale.
export function QrCode({ text, label }) {
	const modules = useMemo(() => qrModules(text), [text]);
	const view = modules.length;
	const size = view * Math.max(2, Math.floor(ABOUT / view));
	return (
		<svg role="img" aria-label={label} viewBox={`0 0 ${view} ${view}`} width={size} height={size} shapeRendering="crispEdges">
			<rect width={view} height={view} fill="#ffffff" />
			<path d={qrPath(modules)} fill="#10100d" />
		</svg>
	);
}
