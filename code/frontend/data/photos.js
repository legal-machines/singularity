// A member's photo as the firm sees it: the middle square of the picture
// they chose, 256 pixels a side, a JPEG small enough to travel with their
// record in every Matter. It is cut here, in the window, so no original
// leaves the member's computer at all.
export const PHOTO_SIDE = 256;

export async function squarePhoto(source) {
	const image = new Image();
	image.decoding = "async";
	image.src = source;
	await image.decode();
	const side = Math.min(image.naturalWidth, image.naturalHeight);
	if (side < 1) throw new Error("This picture has no size.");
	const canvas = document.createElement("canvas");
	canvas.width = PHOTO_SIDE;
	canvas.height = PHOTO_SIDE;
	const context = canvas.getContext("2d");
	context.imageSmoothingQuality = "high";
	context.drawImage(image, (image.naturalWidth - side) / 2, (image.naturalHeight - side) / 2, side, side, 0, 0, PHOTO_SIDE, PHOTO_SIDE);
	return canvas.toDataURL("image/jpeg", 0.86);
}

// A picture the member dropped on their face: read here, never uploaded.
export function readPicture(file) {
	return new Promise((resolve, reject) => {
		if (file == null || /^image\/(jpeg|png|gif|webp)$/.test(file.type) == false) {
			reject(new Error("Choose a JPEG, PNG, GIF or WebP picture."));
			return;
		}
		const reader = new FileReader();
		reader.onload = () => resolve(String(reader.result));
		reader.onerror = () => reject(new Error("This picture could not be read."));
		reader.readAsDataURL(file);
	});
}
