import { useEffect, useLayoutEffect, useRef, useState } from "react";

import { FileKindIcon } from "../../components/FileKind/FileKind.jsx";
import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { characters } from "../../components/Avatar/characters.js";
import { members } from "../../data/workspace.js";
import { ANSWER_MS, BEAT_MS, DURATION_MS, createPlayback, frameAt, scenes } from "./conversation.js";
import { placeDialogue } from "./bubblePlacement.js";
import { fileFlight } from "./fileFlight.js";
import styles from "./ColleagueOrbit.module.css";

// Decorative, scripted design study using Team's fictional people and its
// exact Avatar component and animation clips, with scoped studio grounds. These exchanges
// are an illustration, not real messages, files or reported work.
// People and AI share the same size scale: 36, 40, 44, 48 and 56 pixels.
const places = {
	m_evelyn: [-110, -140, 56, -3],
	m_iris: [-15, -185, 44, 2],
	m_noah: [-167, -200, 36, -2],
	m_themis: [105, -157, 56, 3],
	m_gabriel: [14, -102, 48, -2],
	m_lena: [182, -108, 44, 3],
	m_teodor: [-209, -105, 48, -3],
	m_juno: [101, -100, 36, 2],
	m_amina: [-52, -100, 40, 2],
	entry_phoenix: [148, -212, 40, -3],
};
// The welcome uses the same ten colleagues and one conversation timeline,
// split into two groups. No extra faces or independent playback are loaded.
const welcomePlaces = {
	m_evelyn: [-1, -26, -115, 56, -3],
	m_iris: [-1, 62, -45, 48, 3],
	m_noah: [-1, -74, -18, 40, -2],
	m_teodor: [-1, 12, 92, 56, 3],
	m_amina: [-1, -64, 145, 44, -2],
	m_themis: [1, -24, -120, 64, 3],
	m_gabriel: [1, 65, -36, 48, -2],
	m_lena: [1, 3, 124, 44, -3],
	m_juno: [1, 66, 30, 44, 2],
	entry_phoenix: [1, 67, -169, 40, -3],
};
function positionsForWelcome(width) {
	if (width < 820) return places;
	const distance = Math.min(430, width * 0.345);
	return Object.fromEntries(Object.entries(welcomePlaces).map(([id, [side, x, y, size, angle]]) =>
		[id, [side * distance + x, y, size, angle]]));
}

// Every balloon grows below the author's bottom-left corner, inset eight
// pixels into the portrait, and shares its tilt. Width follows the copy.
const bubbleInset = 8;
// The stage includes the tilted portraits' natural bounds in the composition.
const verticalEdges = Object.values(places).map(([, y, size, angle]) => {
	const radians = angle * Math.PI / 180;
	const half = size / 2 * (Math.abs(Math.cos(radians)) + Math.abs(Math.sin(radians)));
	return [y - half, y + half];
});
const castTop = Math.min(...verticalEdges.map(([top]) => top));
const castBottom = Math.max(...verticalEdges.map(([, bottom]) => bottom));
// Phoenix is from the app's existing Face catalogue; this illustrative role
// does not hire it into the user's firm or change any backend membership.
const cataloguePhoenix = { id: "entry_phoenix", name: "Phoenix", kind: "ai_colleague", species: "phoenix" };
const cast = Object.keys(places).map(id => id === cataloguePhoenix.id ? cataloguePhoenix : members.find(member => member.id === id));
// Corner samples from the same five human portrait files, not a new palette.
// These overrides belong to this illustration; Team keeps its own grounds.
const studio = {
	kitsune: { ground: "#AAAEB4", transform: "translateY(2%) scale(0.98)" },
	dragon: { ground: "#B87977", transform: "translateY(3%) scale(0.96)" },
	griffin: { ground: "#50739B", transform: "translateY(4%) scale(0.96)" },
	unicorn: { ground: "#8EC2EF", transform: "translateY(1%) scale(1)" },
	phoenix: { ground: "#90968C", transform: "translateY(4%) scale(0.98)" },
};
const firstName = member => member.name.split(" ")[0];

function useConversation() {
	const [frame, setFrame] = useState(() => ({ ...frameAt(0), paused: document.hidden }));
	useEffect(() => {
		const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
		const playback = createPlayback({
			now: () => performance.now(), schedule: (fn, delay) => setTimeout(fn, delay),
			cancel: clearTimeout, onFrame: setFrame,
		});
		const sync = () => document.hidden || reduced.matches ? playback.pause() : playback.resume();
		reduced.addEventListener("change", sync);
		document.addEventListener("visibilitychange", sync);
		sync();
		return () => {
			playback.dispose();
			reduced.removeEventListener("change", sync);
			document.removeEventListener("visibilitychange", sync);
		};
	}, []);
	return { ...scenes[frame.step], ...frame, serial: frame.step };
}

const workingByStage = {
	scope: ["m_themis", "m_juno", "m_teodor", "m_iris", "entry_phoenix"],
	review: ["m_themis", "m_teodor", "m_iris", "entry_phoenix"],
	risk: ["m_themis", "m_juno", "m_teodor", "m_iris", "entry_phoenix"],
	draft: ["m_themis", "m_iris", "entry_phoenix"],
	signing: ["m_themis", "m_iris", "entry_phoenix"],
	regulatory: ["m_iris", "entry_phoenix"],
	conditions: ["m_teodor", "m_iris", "entry_phoenix"],
	closing: ["m_themis", "m_iris", "entry_phoenix"],
	closed: [],
};
function presenceFor(member, scene) {
	if (!scene || member.kind !== "ai_colleague" || scene.phase === "complete") return "rest";
	if (workingByStage[scene.stage]?.includes(member.id)) return "work";
	if ([scene.from, scene.to].includes(member.id)) {
		const available = characters[member.species].presences;
		return ["answer", "return", "settle"].includes(scene.phase) && available.explain ? "explain" : "work";
	}
	return "rest";
}

// Identical motion and one timeline keep each foreground balloon attached
// to its portrait, although they occupy separate stacking layers.
function Floating({ member, epoch, reduced, children }) {
	const ref = useRef(null);
	const index = cast.findIndex(person => person.id === member.id);
	useLayoutEffect(() => {
		if (reduced) return undefined;
		const duration = 6400 + ((index * 7) % 6) * 430;
		const frames = Array.from({ length: 81 }, (_, frame) => {
			const phase = frame / 80 * Math.PI * 2;
			const x = Math.sin(phase) * (1.5 + index % 3 * 0.4);
			const y = Math.cos(phase) * (3 + index % 4 * 0.5);
			const angle = Math.sin(phase) * (0.55 + index % 3 * 0.12);
			return { offset: frame / 80, transform: `translate(${x}px, ${y}px) rotate(${angle}deg)` };
		});
		const motion = ref.current.animate(frames, { duration, iterations: Infinity, easing: "linear" });
		motion.startTime = epoch - index * 0.137 * duration;
		return () => motion.cancel();
	}, [epoch, index, reduced]);
	return <span ref={ref} className={styles.drift}>{children}</span>;
}

function Bubble({ member, text, serial, reply, pending, last, epoch, reduced, messageRef, positions }) {
	const [x, y, size, angle] = positions[member.id];
	return (
		<div className={styles.bubbleSlot} data-bubble-owner={member.id} style={{
			"--orbit-x": `${x}px`, "--orbit-y": `${y}px`, "--orbit-size": `${size}px`, "--orbit-angle": `${angle}deg`,
		}} key={`${member.id}-${serial}`}>
			<Floating member={member} epoch={epoch} reduced={reduced}>
				<div ref={messageRef} className={styles.message} data-speaker={member.id} data-pending={pending || undefined} data-last={last || undefined} style={{ "--message-lifetime": `${reply ? BEAT_MS - ANSWER_MS : BEAT_MS}ms`, "--bubble-inset": `${bubbleInset}px` }}>
					<span className={styles.speaker}>{firstName(member)}</span>
					<span>{text}</span>
				</div>
			</Floating>
		</div>
	);
}

// One effect-wide easing curve, rather than easing each sampled segment.
// The same HTML animation runs in the native WKWebView and browser preview.
function FilePacket({ files, start, end }) {
	const ref = useRef(null);
	const [x1, y1] = start;
	const [x2, y2] = end;
	useLayoutEffect(() => {
		const field = ref.current.parentElement;
		const scale = new DOMMatrixReadOnly(getComputedStyle(field).transform).a || 1;
		// Files fly no higher than 52 pixels under the top of the page they are on.
		const page = field.closest("section");
		const ceiling = page ? (page.getBoundingClientRect().top + 52 - field.getBoundingClientRect().top) / scale : -Infinity;
		const flight = fileFlight([x1, y1], [x2, y2], ceiling);
		const motion = ref.current.animate(flight.frames, {
			duration: flight.duration,
			easing: "cubic-bezier(0.32, 0.04, 0.2, 1)",
			fill: "both",
		});
		const visibility = () => document.hidden ? motion.pause() : motion.play();
		visibility();
		document.addEventListener("visibilitychange", visibility);
		return () => { document.removeEventListener("visibilitychange", visibility); motion.cancel(); };
	}, [x1, y1, x2, y2]);
	return (
		<div className={styles.packet} ref={ref}>
			{files.map((file, index) => {
				const fan = index - (files.length - 1) / 2;
				return (
					<span key={file} data-file={file} className={styles.fanLeaf} style={{ transform: `translate(${fan * 10}px, ${Math.abs(fan) * 2}px) rotate(${fan * 16}deg)` }}>
						<span className={styles.documentIcon}><FileKindIcon name={file} size="row" /></span>
					</span>
				);
			})}
		</div>
	);
}

function Exchange({ scene, epoch, reduced, positions }) {
	const askRef = useRef(null);
	const answerRef = useRef(null);
	useLayoutEffect(() => {
		let disposed = false;
		const fit = () => {
			if (disposed || !askRef.current || !answerRef.current) return;
			placeDialogue(askRef.current.closest(`.${styles.field}`), [askRef.current, answerRef.current], [scene.from, scene.to]);
		};
		fit();
		document.fonts.ready.then(fit);
		window.addEventListener("resize", fit);
		return () => { disposed = true; window.removeEventListener("resize", fit); };
	}, [scene.serial, scene.phase === "entrance", positions]);
	if (scene?.phase === "entrance") return null;
	const returning = scene?.phase === "return";
	const files = returning ? scene.replyFiles : scene.files;
	const moving = ["transfer", "return"].includes(scene?.phase) && files.length > 0;
	const from = scene ? cast.find(member => member.id === scene.from) : null;
	const to = scene ? cast.find(member => member.id === scene.to) : null;
	return (
		<>
			{scene && <Bubble positions={positions} member={from} messageRef={askRef} text={scene.ask} serial={scene.serial} last={scene.serial === scenes.length - 1} epoch={epoch} reduced={reduced} />}
			{scene && <Bubble positions={positions} member={to} messageRef={answerRef} pending={!["answer", "return", "settle", "complete"].includes(scene.phase)} text={scene.answer} serial={scene.serial} reply last={scene.serial === scenes.length - 1} epoch={epoch} reduced={reduced} />}
			{moving && <FilePacket key={`${scene.serial}-${scene.phase}`} files={files} start={positions[returning ? scene.to : scene.from]} end={positions[returning ? scene.from : scene.to]} />}
		</>
	);
}

export function ColleagueOrbit({ layout = "cover" }) {
	const orbitRef = useRef(null);
	const [positions, setPositions] = useState(places);
	useLayoutEffect(() => {
		if (layout !== "welcome") return undefined;
		setPositions(positionsForWelcome(orbitRef.current.clientWidth));
		const observer = new ResizeObserver(([entry]) => setPositions(positionsForWelcome(entry.contentRect.width)));
		observer.observe(orbitRef.current);
		return () => observer.disconnect();
	}, [layout]);
	const scene = useConversation();
	const epoch = useRef(document.timeline.currentTime ?? 0).current;
	const [reduced, setReduced] = useState(() => window.matchMedia("(prefers-reduced-motion: reduce)").matches);
	useEffect(() => {
		const media = window.matchMedia("(prefers-reduced-motion: reduce)");
		const update = () => setReduced(media.matches);
		media.addEventListener("change", update);
		return () => media.removeEventListener("change", update);
	}, []);
	return (
		<div ref={orbitRef} className={styles.orbit} data-layout={layout} aria-hidden="true" data-phase={scene?.phase ?? "entrance"} data-step={scene?.step ?? 0} data-stage={scene.stage} data-playback={scene.paused ? "paused" : "running"} data-duration-ms={DURATION_MS} style={{ "--cast-height": `${castBottom - castTop}px`, "--cast-origin-y": `${-castTop}px` }}>
			<div className={styles.field}>
			<Exchange scene={scene} epoch={epoch} reduced={reduced} positions={positions} />
			{cast.map((member, index) => {
				const [x, y, size, angle] = positions[member.id];
				const portrait = studio[member.species];
				const role = scene?.from === member.id ? "lead" : scene?.to === member.id ? "reply" : null;
				return (
					<span key={member.id} className={styles.slot} data-colleague={member.id} data-presence={presenceFor(member, scene)} style={{
						"--orbit-x": `${x}px`, "--orbit-y": `${y}px`, "--orbit-size": `${size}px`,
						"--orbit-angle": `${angle}deg`, "--orbit-delay": `${180 + index * 55}ms`,
						"--avatar-ground": portrait?.ground,
						"--avatar-image-transform": portrait?.transform,
					}}>
						<Floating member={member} epoch={epoch} reduced={reduced}>
							<Avatar key={reduced ? "still" : "live"} member={member} size={size} presence={presenceFor(member, scene)} pointedAt={role != null} />
						</Floating>
					</span>
				);
			})}
			</div>
		</div>
	);
}
