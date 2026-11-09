// A length a CSS variable gives, in pixels, where an element reads it. The
// browser hands an unregistered variable back as the text it was given
// ("calc(24px * 2 + 14px)"), which parseFloat reads as nothing, so a probe of
// that width measures it. Zero where the variable is not set, and where
// nothing is laid out (a test's document).
export function pixelsOf(element, name) {
	const probe = document.createElement("span");
	probe.style.position = "absolute";
	probe.style.visibility = "hidden";
	probe.style.pointerEvents = "none";
	probe.style.height = "0";
	probe.style.width = `var(${name}, 0px)`;
	element.appendChild(probe);
	const width = probe.getBoundingClientRect().width;
	probe.remove();
	return width;
}
