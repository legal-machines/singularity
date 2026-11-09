// What goes on between this computer and the firm's others, said while it goes
// on: a Matter coming here or going to another computer, by its bytes - how
// much there is, known before any of it opens, and how much has come -, and
// this computer waiting for another's answer. Pure: the window's coordination
// (coordination.js) reads the backend and shows what this says.

const KIB = 1024;
const MIB = 1024 * KIB;
const GIB = 1024 * MIB;

// A size as the app writes one elsewhere: whole kilobytes, then megabytes to a
// tenth and gigabytes to a hundredth.
export function sizeWords(bytes) {
	const value = Math.max(0, Number(bytes) || 0);
	if (value == 0) return "0 KB";
	if (value < MIB) return `${Math.max(1, Math.round(value / KIB))} KB`;
	if (value < GIB) return `${(value / MIB).toFixed(1)} MB`;
	return `${(value / GIB).toFixed(2)} GB`;
}

// A transfer worth a notice: big enough to watch, or still going since the
// last reading. A message of a few kilobytes comes and goes between two
// readings, unsaid; and a few kilobytes of records held for a computer that is
// away - one that may never come back - wait unsaid too: only a real amount
// held back is worth the lawyer's attention.
export const WORTH_WATCHING_BYTES = 256 * KIB;

function isWorthSaying(transfer, isAway, wasGoing) {
	const bytes = transfer.bytes ?? 0;
	if (isAway == true) return bytes - (transfer.bytesHere ?? 0) >= WORTH_WATCHING_BYTES;
	return bytes >= WORTH_WATCHING_BYTES || wasGoing == true;
}

function whoseComputer(deviceId, names, code) {
	const known = names?.get(deviceId);
	if (known?.isYou == true) return "your other computer";
	if ((known?.name ?? "") != "") return `${known.name}’s computer`;
	if ((code ?? "") != "") return `the computer ${code}`;
	return "the other computer";
}

// A Matter taken here is whole once its key is here, nothing of it is still
// coming, and something sealed of it came: the computer that gives a Matter
// says first what it holds, so before anything came, "nothing coming" only
// means nothing was said yet.
export function isWhole(link) {
	return link?.state == "carried" && (link.receiving ?? []).length == 0 && (link.heard ?? []).length > 0;
}

// A computer that has not answered a moment after the Matter was taken is
// away; before that it may only be connecting.
const CONNECTING_MILLISECONDS = 15 * 1000;

function largest(transfers) {
	return [...(transfers ?? [])].sort((first, second) => (second.bytes ?? 0) - (first.bytes ?? 0))[0] ?? null;
}

// The notices the coordination's state calls for, each by a key it keeps while
// it holds: `kind` "wait", "in" or "out", its words, its progress from 0 to 1
// (none for a wait, or where the size is not known) and the words it ends
// with. `names` holds, by Matter, each device's member; `watched` the
// transfers still going at the last reading; `askedHere` the Matters this
// window asked to join lately; `accepted`, by Matter, what the offer the
// member accepted here said of it - its name, its bytes and files, the
// computer it comes from and its member's name, and when; `givenHere` the
// Matters given from this window whose first transfer is not said yet. What it
// returns as `watched` is passed back at the next reading. A notice marked
// `isDone` tells of a transfer that went whole between two readings: it is
// said once, ended as it shows.
export function transferNotices(matters, { deviceCode = "", names = new Map(), watched = new Set(), askedHere = new Set(),
	accepted = new Map(), givenHere = new Set(), now = Date.now() } = {}) {
	const notices = [];
	const stillGoing = new Set();
	// A Matter the member accepted here is said from the moment they accept it
	// until it is whole here, however small it is and however fast it comes,
	// with how much of what the offer said has come: the member waits for it.
	for (const [matterId, taken] of accepted) {
		const link = (matters ?? []).find(entry => entry.matterId == matterId) ?? null;
		if (isWhole(link)) continue;
		const key = `in:${matterId}`;
		stillGoing.add(key);
		const matterName = (taken.name ?? "").trim() || (link?.name ?? "").trim() || "A Matter";
		const coming = largest(link?.receiving);
		const bytes = Math.max(coming?.bytes ?? 0, taken.bytes ?? 0);
		const bytesHere = Math.min(bytes, coming?.bytesHere ?? 0);
		const who = (taken.fromName ?? "") != "" ? `${taken.fromName}’s computer` : whoseComputer(taken.fromDeviceId, names.get(matterId), "");
		const isConnected = coming?.reachable == true || (link?.connected ?? []).includes(taken.fromDeviceId);
		const isAway = isConnected == false && now - (taken.at ?? now) > CONNECTING_MILLISECONDS;
		const amount = `${sizeWords(bytesHere)} of ${sizeWords(bytes)}`;
		notices.push({
			key, kind: "in", tone: isAway ? "waiting" : "working",
			progress: bytes > 0 ? bytesHere / bytes : null,
			text: isAway
				? `${matterName} waits for ${who} to come online${bytes > 0 ? `: ${amount} here` : "."}`
				: `${matterName} is coming from ${who}${bytes > 0 ? `: ${amount}` : "."}`,
			doneText: `${matterName} has arrived.`,
		});
	}
	for (const link of matters ?? []) {
		const matterName = (link.name ?? "").trim() || "A Matter";
		const offering = link.offering ?? [];
		const receiving = link.receiving ?? [];
		// Asked to join, and nothing offered yet: the computer that invited this
		// one has to let it in, its lawyer comparing this computer's code.
		if (link.state == "waiting" && offering.length == 0 && receiving.length == 0) {
			if (askedHere.has(link.matterId) == false) continue;
			notices.push({
				key: `let-in:${link.matterId}`, kind: "wait", tone: "waiting", progress: null,
				text: `Waiting for the computer that invited you to let this one into ${matterName}. This computer’s code is ${deviceCode}.`,
			});
			continue;
		}
		const matterNames = names.get(link.matterId);
		// Coming here: from the computer sending the most, as one notice. One
		// offered, whose key waits for the lawyer, is said however small: it
		// asks for the lawyer's comparison of codes. One accepted here is said
		// above.
		const coming = accepted.has(link.matterId) ? null : largest(receiving);
		const isOffered = link.state != "carried" && offering.length > 0;
		if (coming != null && (isOffered || (coming.bytesHere ?? 0) < (coming.bytes ?? 0))) {
			const key = `in:${link.matterId}`;
			stillGoing.add(key);
			const isAway = link.state == "carried" && coming.reachable == false;
			if (isOffered || isWorthSaying(coming, isAway, watched.has(key))) {
				const offer = offering.find(entry => entry.deviceId == coming.deviceId);
				const who = whoseComputer(coming.deviceId, matterNames, offer?.code);
				const amount = `${sizeWords(coming.bytesHere)} of ${sizeWords(coming.bytes)}`;
				const text = link.state != "carried"
					? `${matterName} is coming from ${who}: ${amount} here. Compare its code with your colleague in Matters, then take it.`
					: isAway
						? `${matterName} waits for ${who} to come back: ${amount} here`
						: `${matterName} is arriving from ${who}: ${amount}`;
				notices.push({
					key, kind: "in", tone: link.state == "carried" && isAway == false ? "working" : "waiting",
					progress: (coming.bytesHere ?? 0) / Math.max(1, coming.bytes ?? 0), text,
					doneText: `${matterName} has arrived. Open it from Matters.`,
				});
			}
		}
		// Going to each computer this one sends it to. A Matter given from here
		// is said the first time it goes however small it is; and if it went
		// whole between two readings - the backend keeps a transfer a few seconds
		// after the other computer confirmed it -, it is still said, once.
		const isGivenHere = givenHere.has(link.matterId);
		for (const going of link.sending ?? []) {
			const key = `out:${link.matterId}:${going.deviceId}`;
			const who = whoseComputer(going.deviceId, matterNames, "");
			const doneText = `${matterName} has reached ${who}.`;
			if ((going.bytesHere ?? 0) >= (going.bytes ?? 0)) {
				if (isGivenHere && watched.has(key) == false && (going.bytes ?? 0) > 0) {
					notices.push({
						key, kind: "out", tone: "working", progress: 1, isDone: true, doneText,
						text: `Sending ${matterName} to ${who}: ${sizeWords(going.bytes)} of ${sizeWords(going.bytes)}`,
					});
				}
				continue;
			}
			stillGoing.add(key);
			const isAway = going.reachable == false;
			if (isGivenHere == false && isWorthSaying(going, isAway, watched.has(key)) == false) continue;
			notices.push({
				key, kind: "out", tone: isAway ? "waiting" : "working",
				progress: (going.bytesHere ?? 0) / Math.max(1, going.bytes ?? 0),
				text: isAway
					? `${matterName} waits for ${who} to come online: ${sizeWords((going.bytes ?? 0) - (going.bytesHere ?? 0))} to send`
					: `Sending ${matterName} to ${who}: ${sizeWords(going.bytesHere)} of ${sizeWords(going.bytes)}`,
				doneText,
			});
		}
	}
	return { notices, watched: stillGoing };
}

// How a notice no longer called for ends: "done" when what it told of came
// whole - the Matter carried here with nothing more coming, or every byte
// going to that computer confirmed -, "drop" otherwise, unsaid.
export function endingOf(item, matters) {
	if (item.kind == "wait") return "drop";
	const matterId = item.key.split(":")[1];
	const link = (matters ?? []).find(entry => entry.matterId == matterId);
	if (link == null) return "drop";
	if (item.kind == "in") return link.state == "carried" && (link.receiving ?? []).length == 0 ? "done" : "drop";
	const deviceId = item.key.split(":").slice(2).join(":");
	const going = (link.sending ?? []).find(entry => entry.deviceId == deviceId);
	return going != null && (going.bytesHere ?? 0) >= (going.bytes ?? 0) ? "done" : "drop";
}
