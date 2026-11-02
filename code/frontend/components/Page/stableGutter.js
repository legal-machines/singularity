import { useLayoutEffect } from "react";

// A page's column stands in one place whether the page scrolls or not (the
// owner, 2026-11-02: "the red is shifted by the scroll bar"): its left margin
// is worked out from the scroll area's whole width, the scrollbar's room
// included (`--page-width`, read here), never from what the bar leaves, so a
// bar narrows the column on its right and never moves its left edge. The
// same in every engine: the app's WebKit and a browser alike.
export function useStableGutter(ref) {
	useLayoutEffect(() => {
		const element = ref.current;
		if (element == null) return undefined;
		const apply = () => element.style.setProperty("--page-width", `${element.offsetWidth}px`);
		apply();
		if (typeof ResizeObserver != "function") return undefined;
		const observer = new ResizeObserver(apply);
		observer.observe(element);
		return () => observer.disconnect();
	}, [ref]);
}
