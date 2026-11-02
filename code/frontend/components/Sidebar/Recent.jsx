import { useRef, useState } from "react";

import { Avatar } from "../Avatar/Avatar.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { addressOf, useNavigation } from "../Navigation/navigation.js";
import { toast } from "../Toast/toast.js";
import { bindRealMatter, realDocument, realLetter, shownMatters, shownMembers, shownThreads, useRealWork } from "../../data/native.js";
import { initialState } from "../../pages/threadState.js";
import { recentConversations } from "./recent.js";

import styles from "./Recent.module.css";

// The conversations where something was written last - a direct message or
// a thread of any Matter, the latest three (recent.js) - each by a face: a
// direct message's member, and in a thread whoever wrote last (the owner,
// 2026-11-02). No message excerpts or timestamps. The backend reopens a real
// Matter before a thread's address is followed, so another Matter's thread
// opens.
export function Recent({ entries: given = null, state = initialState, opened = {} }) {
	// Read again as the backend says something was written.
	useRealWork();
	const entries = given ?? recentConversations(state, opened);
	const { place, open } = useNavigation();
	const [opening, setOpening] = useState(null);
	const currentPlace = useRef(place);
	currentPlace.current = place;
	if (entries.length == 0) return null;

	async function follow(entry) {
		if (opening != null) return;
		setOpening(entry.key);
		try {
			if (entry.memberId != null && !shownMembers().some(member => member.id == entry.memberId)) { toast("This colleague is no longer available here."); return; }
			if (entry.matterId != null) {
				const matter = shownMatters().find(item => item.id == entry.matterId);
				if (matter == null) { toast("This Matter is no longer available here."); return; }
				if (matter.realMatterId != null) await bindRealMatter(matter.realMatterId);
				// A newer navigation wins while a Matter is being read.
				if (currentPlace.current != place) return;
				const missing = entry.place.kind == "thread" && !shownThreads().some(thread => thread.id == entry.place.threadId)
					|| entry.place.kind == "document" && matter.realMatterId != null && realDocument(entry.place.documentId) == null
					|| entry.place.kind == "letter" && realLetter(entry.place.letterId) == null;
				if (missing) { toast("Couldn’t reopen this item. Its Matter is open."); open({ kind: "matter", matterId: matter.id, tab: "overview" }); return; }
			}
			open(entry.place);
		} catch {
			toast("This place could not be opened. Try again.");
		} finally { setOpening(null); }
	}

	return (
		<nav className={styles.section} aria-label="Recent places">
			<div className={styles.heading}><h2>Recent</h2></div>
			<ul className={styles.rows}>
				{entries.map(entry => {
					const member = entry.faceId != null ? shownMembers().find(item => item.id == entry.faceId) ?? null : null;
					return <li key={entry.key}><button className={styles.recent} type="button" title={entry.detail != null ? `${entry.name}, ${entry.detail}` : entry.name} aria-current={addressOf(place) == entry.key ? "page" : undefined} aria-busy={opening == entry.key || undefined} onClick={() => follow(entry)}>
						<span className={styles.recent_icon}>{member != null ? <Avatar member={member} size={22} showsOnline={false} /> : <Icon name={entry.place.kind == "messages" ? "messages" : "notes"} size={18} />}</span>
						<span className={styles.recent_name}>{entry.name}</span>
					</button></li>;
				})}
			</ul>
		</nav>
	);
}
