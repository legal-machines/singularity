// Personal triage never changes the shared thread or its work. Read, saved
// and done are independent; every view keeps one row per inbox object.
export function inboxView(entries, state, view, unreadOnly = false) {
	const done = new Set(state.doneIds);
	const saved = new Set(state.savedIds);
	const visible = entries.filter(entry => {
		if (view == "saved" ? !saved.has(entry.id) : done.has(entry.id) != (view == "done")) return false;
		return !unreadOnly || entry.content.isUnread;
	}).sort((a, b) => Date.parse(b.at) - Date.parse(a.at) || a.id.localeCompare(b.id));
	if (view != "active") return [{ id: view, title: view == "done" ? "Marked done" : "Saved for later", entries: visible }].filter(group => group.entries.length > 0);
	return [
		{ id: "needs", title: "Needs you", entries: visible.filter(entry => entry.isAsk) },
		{ id: "updates", title: "Updates", entries: visible.filter(entry => !entry.isAsk) },
	].filter(group => group.entries.length > 0);
}

const clock = new Intl.DateTimeFormat("en-US", { hour: "numeric", minute: "2-digit" });
const day = new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric" });
const year = new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric", year: "numeric" });

// Compare calendar days on this computer. The real app never uses the
// demonstration's clock; old items give their year rather than implying today.
export function inboxTime(iso, now) {
	const date = new Date(iso);
	if (Number.isNaN(date.getTime())) return "Date unavailable";
	if (date.toDateString() == now.toDateString()) return clock.format(date);
	const yesterday = new Date(now);
	yesterday.setDate(now.getDate() - 1);
	if (date.toDateString() == yesterday.toDateString()) return "Yesterday";
	return (date.getFullYear() == now.getFullYear() ? day : year).format(date);
}
