import { OBJECTS_ONBOARDING } from "./composition.js";

// The parts come as a classic script beside the page (deploy/frontend's
// painting-parts-script plugin), each part's WebP bytes in base64, never as
// files the page draws. The packaged window opens the page from disk
// (file://), where WebView2 counts each file as an origin of its own: a part
// drawn from its file tainted its canvas, WebGL refused it, and on Windows the
// painting stood still as its poster. An image decoded from bytes the page
// holds is never another origin's, on any system. A script runs from disk
// where fetch of a file is refused, as the colleagues' motion manifest does
// (components/Avatar/life.js).
const PARTS_SCRIPT = "painting/parts.js";

let partsLoad = null;

// The parts' bytes, read once by every painting that asks while they load,
// then let go, so the page does not keep them after the painting has its
// layers.
function readParts() {
	if (partsLoad == null) {
		partsLoad = new Promise((resolve, reject) => {
			const take = () => {
				const parts = window.__singularityPaintingParts;
				delete window.__singularityPaintingParts;
				return parts;
			};
			if (window.__singularityPaintingParts != null) {
				resolve(take());
				return;
			}
			const script = document.createElement("script");
			script.src = PARTS_SCRIPT;
			script.async = true;
			script.onload = () => {
				script.remove();
				const parts = take();
				if (parts == null) reject(new Error("The painting's parts are missing from their script."));
				else resolve(parts);
			};
			script.onerror = () => {
				script.remove();
				reject(new Error("The painting's parts did not load."));
			};
			document.head.append(script);
		}).finally(() => {
			partsLoad = null;
		});
	}
	return partsLoad;
}

// A part's picture from its bytes.
async function decodePart(encoded) {
	const text = atob(encoded);
	const bytes = new Uint8Array(text.length);
	for (let index = 0; index < text.length; index += 1) bytes[index] = text.charCodeAt(index);
	return createImageBitmap(new Blob([bytes], { type: "image/webp" }));
}

const cancelled = () => new DOMException("Painting load cancelled", "AbortError");

// Each layer of the painting, cut and sized for the renderer.
export async function prepareArtwork(signal) {
	const ids = [...new Set(OBJECTS_ONBOARDING.map((a) => a.asset ?? a.id))];
	const parts = await readParts();
	if (signal.aborted) throw cancelled();
	const images = Object.fromEntries(
		await Promise.all(
			ids.map(async (id) => {
				if (typeof parts[id] != "string") throw new Error(`The painting has no part named ${id}.`);
				const image = await decodePart(parts[id]);
				if (signal.aborted) {
					image.close();
					throw cancelled();
				}
				return [id, image];
			}),
		),
	);
	const layers = OBJECTS_ONBOARDING.map((definition) => {
		const base = definition.overlay ? OBJECTS_ONBOARDING.find((a) => a.id === definition.overlay) : definition;
		const source = images[definition.asset ?? definition.id];
		const image = document.createElement("canvas");
		image.width = source.width;
		image.height = source.height;
		const context = image.getContext("2d");
		if (definition.clip) {
			context.beginPath();
			definition.clip.forEach(([u, v], i) => context[i ? "lineTo" : "moveTo"](u * image.width, v * image.height));
			context.closePath();
			context.clip();
		}
		context.drawImage(source, 0, 0);
		const width = base.box?.[2] ?? base.width ?? (base.height * image.width) / image.height;
		const height = base.box?.[3] ?? base.height ?? (base.width * image.height) / image.width;
		return {
			...base,
			...definition,
			image,
			width,
			height,
			x: base.box?.[0] ?? base.x,
			y: base.box?.[1] ?? base.y,
			poseId: definition.overlay ?? definition.id,
		};
	});
	// Each part is in its layers' canvases now; its decoded picture goes.
	for (const image of Object.values(images)) image.close();
	return { layers };
}
