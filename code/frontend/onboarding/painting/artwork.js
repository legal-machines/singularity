import { OBJECTS_ONBOARDING } from "./composition.js";

// Each layer of the painting, cut and sized for the renderer.
export async function prepareArtwork(signal) {
	const ids = [...new Set(OBJECTS_ONBOARDING.map((a) => a.asset ?? a.id))];
	const images = Object.fromEntries(
		await Promise.all(
			ids.map(async (id) => {
				const image = new Image();
				image.decoding = "async";
				image.src = new URL(`./parts/${id}.webp`, import.meta.url).href;
				await image.decode();
				if (signal.aborted) throw new DOMException("Painting load cancelled", "AbortError");
				return [id, image];
			}),
		),
	);
	return {
		layers: OBJECTS_ONBOARDING.map((definition) => {
			const base = definition.overlay ? OBJECTS_ONBOARDING.find((a) => a.id === definition.overlay) : definition;
			const source = images[definition.asset ?? definition.id];
			const image = document.createElement("canvas");
			image.width = source.naturalWidth;
			image.height = source.naturalHeight;
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
		}),
	};
}
