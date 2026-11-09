import { OBJECTS, SIZE } from "./scene.js";

// The site's whole painting, seen in onboarding's right half, 3 by 4. The view
// is the painting's right end, so its right edge is the window's. Every figure
// keeps its place from the site: their distances to each other and to the desk
// are the painting's own.
// The view keeps 8 of the painting's points from its top, right and bottom
// edges: the breathing camera zooms and shifts the scene by up to 5 there
// (renderer.js, u_camera), and without the margin the canvas's black showed
// along those edges.
const EDGE = 8;
const HEIGHT = SIZE[1] - 2 * EDGE;
export const VIEW = [SIZE[0] - EDGE - (HEIGHT * 3) / 4, EDGE, (HEIGHT * 3) / 4, HEIGHT];

// The client at the desk's end stands behind it, as in Brueghel's painting:
// the desk is drawn over him, and his depth is just behind the desk's (the site
// draws him in front of it). The stool behind him stays behind him.
const find = (id) => OBJECTS.find((object) => object.id === id);
const behindDesk = [find("stool"), { ...find("client"), depth: 0.62 }];
// The sheet of paper on the floor at the foot of the frame is left out
// here.
export const REMOVED = new Set(["floorPaperRight"]);
// The scene's layers that never reach this frame, however the camera breathes
// and the figures move (80 of the painting's points to spare): they are left
// out, so they are neither loaded nor drawn. composition.test.js measures
// them from their images, and that no other file is shipped.
export const OFF_FRAME = new Set([
	"entranceBundles", "wallBag", "floorPaperLeft", "floorPaperMiddle", "leftRear", "door", "leftFront",
	"clerk", "leftDesk", "waitingRear", "basketWoman", "floorPail", "floorLeft",
]);
const rest = OBJECTS.filter(
	(object) => !behindDesk.includes(object) && object.id !== "client" && !REMOVED.has(object.id) && !OFF_FRAME.has(object.id),
);
const desk = rest.findIndex((object) => object.id === "rightDesk");
export const OBJECTS_ONBOARDING = [...rest.slice(0, desk), ...behindDesk, ...rest.slice(desk)];
export const ACTORS_ONBOARDING = OBJECTS_ONBOARDING.filter((a) => a.head && !a.overlay);
