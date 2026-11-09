import { useEffect, useSyncExternalStore } from "react";

import { sourceIcons } from "../../data/native.js";

// The icons of the sites web sources come from, as the window keeps them
// while it is open: each site's icon as a picture's data, or null where the
// site has none. The sources a message shows together are asked for
// together, in one call, and each site once.
const MAX_SITES_PER_ASK = 24;

const known = new Map();
const wanted = new Set();
const asked = new Set();
const listeners = new Set();
let isAsking = false;
let isQueued = false;
let askOf = sourceIcons;

function tell() {
	for (const listener of listeners) listener();
}

async function askWanted() {
	if (isAsking || wanted.size == 0) return;
	isAsking = true;
	const sites = [...wanted].slice(0, MAX_SITES_PER_ASK);
	for (const site of sites) {
		wanted.delete(site);
		asked.add(site);
	}
	let icons = {};
	try {
		icons = (await askOf(sites)) ?? {};
	} catch {
		icons = {};
	}
	for (const site of sites) {
		known.set(site, typeof icons[site] == "string" ? icons[site] : null);
		asked.delete(site);
	}
	isAsking = false;
	tell();
	if (wanted.size > 0) askWanted();
}

// Asks for `site`'s icon, unless the window has it or has asked for it
// already; every site wanted in the same moment goes in the same call.
export function wantSiteIcon(site) {
	if (site == "" || known.has(site) || wanted.has(site) || asked.has(site)) return;
	wanted.add(site);
	if (isQueued) return;
	isQueued = true;
	setTimeout(() => {
		isQueued = false;
		askWanted();
	}, 0);
}

function subscribe(listener) {
	listeners.add(listener);
	return () => listeners.delete(listener);
}

// `site`'s icon: its data once it has come, null where the site has none,
// undefined while it is on its way.
export function siteIconOf(site) {
	return known.get(site);
}

export function useSiteIcon(site) {
	const icon = useSyncExternalStore(subscribe, () => known.get(site));
	useEffect(() => {
		wantSiteIcon(site);
	}, [site]);
	return icon;
}

// A source's site as its address names it, for asking its icon: the whole
// host, "www." and all, as the site answers there.
export function hostOf(source) {
	try {
		return new URL(source.url).hostname.toLowerCase();
	} catch {
		return (source.site ?? "").toLowerCase();
	}
}

// For tests: what asks the native side, and a window that has asked nothing.
export function askSiteIconsWith(ask) {
	askOf = ask ?? sourceIcons;
	known.clear();
	wanted.clear();
	asked.clear();
	isAsking = false;
	isQueued = false;
}
