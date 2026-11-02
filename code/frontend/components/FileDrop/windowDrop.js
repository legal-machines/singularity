import { useEffect, useRef } from "react";

// The window takes files dragged onto it itself, and lets them fall only on
// the places the page names (Platform/FileDrop/FileDrop.hpp): every element
// marked data-file-drop, asked for as a drag goes. A web view alone would
// give the page the files' names, never where they are.
if (typeof window != "undefined") {
	window.singularityFileDropZones = () => [...document.querySelectorAll("[data-file-drop]")].map(element => {
		const box = element.getBoundingClientRect();
		return { x: box.left, y: box.top, width: box.width, height: box.height };
	});
}

// Files dragged over the element `ref` holds, and let go on it: `onOver` says
// whether they are over it as they come and go, `onDrop` has the files where
// they fell, each as { path, isFolder }.
export function useWindowFileDrop(ref, isOn, onOver, onDrop) {
	const said = useRef({ onOver, onDrop });
	said.current = { onOver, onDrop };
	useEffect(() => {
		if (isOn == false || typeof window == "undefined") return undefined;
		let isOver = false;
		const tell = value => {
			if (value == isOver) return;
			isOver = value;
			said.current.onOver?.(value);
		};
		const inside = detail => {
			const box = ref.current?.getBoundingClientRect();
			if (box == null || detail == null) return false;
			return detail.x >= box.left && detail.x <= box.right && detail.y >= box.top && detail.y <= box.bottom;
		};
		const dragging = event => tell(inside(event.detail));
		const ended = () => tell(false);
		const dropped = event => {
			if (inside(event.detail) == false) return;
			event.preventDefault();
			tell(false);
			said.current.onDrop?.(event.detail?.files ?? []);
		};
		window.addEventListener("singularity:files-dragging", dragging);
		window.addEventListener("singularity:files-drag-ended", ended);
		window.addEventListener("singularity:files-dropped", dropped);
		return () => {
			window.removeEventListener("singularity:files-dragging", dragging);
			window.removeEventListener("singularity:files-drag-ended", ended);
			window.removeEventListener("singularity:files-dropped", dropped);
			tell(false);
		};
	}, [isOn, ref]);
}
