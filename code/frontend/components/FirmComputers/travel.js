// Whether a Matter travels between the firm's computers (D-137), in words: end
// to end encrypted with whose computers, whether one of them is connected now,
// and when something sealed last came from one. Only what the backend
// established is said (UI-4): the time of this computer's own last round is
// never offered as a sign of the others.

// The Matter's other computers, each with whose it is: every computer its
// members' records state, but this one, one still asking to join and one the
// person here revoked; then any computer connected or heard from that no record
// names yet. Read from the records, so a member's computer is named before it
// is next heard from (2026-10-29: after a restart, Marcus's line named only
// Dana's computer, though Priya's held the Matter too).
export function travelComputers(members, link) {
	const owners = {};
	const devices = [];
	const left = new Set();
	for (const member of members ?? []) {
		for (const computer of member.computers ?? []) {
			if (computer.state == "this_computer" || computer.state == "asking" || computer.state == "revoked") {
				left.add(computer.deviceId);
				continue;
			}
			owners[computer.deviceId] = { name: (member.name ?? "").trim(), isYou: member.isYou == true };
			devices.push(computer.deviceId);
		}
	}
	const live = [...(link?.connected ?? []), ...(link?.heard ?? []).map(entry => entry.deviceId)];
	for (const device of live) if (left.has(device) == false) devices.push(device);
	return { devices: [...new Set(devices)], owners };
}

export function travelWords(link, devices, owners, now = new Date()) {
	if (link == null) return "Only on this computer. Invite a colleague to share it.";
	if (link.lastError != null && link.lastError != "") return link.lastError;
	if (link.state != "carried") return "Waiting for your colleague to let this computer in.";
	if (devices.length == 0) return "End-to-end encrypted. No other computer has reached it yet.";
	const latest = [...(link.heard ?? [])].sort((first, second) => (second.at ?? "").localeCompare(first.at ?? ""))[0];
	const received = latest != null ? `last received ${whenSaid(latest.at, now)}` : "";
	const connected = (link.connected ?? []).length > 0
		? `Connected now${received != "" ? `, ${received}` : ""}.`
		: `Not connected now${received != "" ? `; ${received}` : ""}.`;
	return `End-to-end encrypted with ${computersOf(devices, owners)}. ${connected}`;
}

// "Dana Whitfield's computer", "your other computer", "Dana Whitfield's
// computer and Priya Nair's".
export function computersOf(devices, owners) {
	const phrases = devices.map((device, index) => {
		const owner = owners[device];
		if (owner?.isYou == true) return "your other computer";
		const whose = owner != null && owner.name != "" ? `${owner.name}'s` : "a colleague's";
		return index == 0 ? `${whose} computer` : whose;
	});
	if (phrases.length == 1) return phrases[0];
	return `${phrases.slice(0, -1).join(", ")} and ${phrases[phrases.length - 1]}`;
}

// When, by this computer's own clock: "at 3:42 AM", "yesterday at 6:10 PM",
// "on Monday, October 26".
function whenSaid(iso, now) {
	const date = new Date(iso);
	if (Number.isNaN(date.getTime())) return "";
	const clock = new Intl.DateTimeFormat("en-US", { hour: "numeric", minute: "2-digit" }).format(date);
	const yesterday = new Date(now);
	yesterday.setDate(now.getDate() - 1);
	if (date.toDateString() == now.toDateString()) return `at ${clock}`;
	if (date.toDateString() == yesterday.toDateString()) return `yesterday at ${clock}`;
	return `on ${new Intl.DateTimeFormat("en-US", { weekday: "long", month: "long", day: "numeric" }).format(date)}`;
}
