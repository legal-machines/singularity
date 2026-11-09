import { hasNative, realUnreadThreads, shownChats, shownThreads, unreadInConversation } from "../../data/native.js";
import { viewerId } from "../../data/workspace.js";
import { conversationsFor } from "../../pages/Messages/conversations.js";
import { isChatUnread, isThreadUnread, lastEvent } from "../../pages/threadState.js";
import { findMember, firstName } from "../../pages/words.js";

// What waits unread for the member, as the sidebar counts it: the threads of
// every Matter with another's message the member has not seen - on the Inbox,
// and on each Matter its own - and the direct messages not yet seen, on
// Messages; and whether any of those threads names the member with @, which
// is marked apart from the count.
export function unreadCounts(state) {
	const threads = hasNative() ? realUnreadThreads() : sampleUnreadThreads(state);
	const matters = new Map();
	for (const thread of threads) {
		const counted = matters.get(thread.matterId) ?? { count: 0, namesViewer: false };
		matters.set(thread.matterId, { count: counted.count + 1, namesViewer: counted.namesViewer || thread.namesViewer });
	}
	const messages = hasNative()
		? conversationsFor(state).filter(conversation => conversation.kind == "member").reduce((sum, conversation) => sum + unreadInConversation(conversation), 0)
		: shownChats(state).filter(chat => isChatUnread(chat, state)).length;
	return {
		inbox: { count: threads.length, namesViewer: threads.some(thread => thread.namesViewer) },
		messages: { count: messages, namesViewer: false },
		matters,
	};
}

// The prototype's threads unread as its inbox has them; one names the member
// where its latest words name them with @.
function sampleUnreadThreads(state) {
	const viewer = findMember(viewerId);
	const handle = viewer != null ? `@${firstName(viewer)}` : null;
	return shownThreads()
		.filter(thread => isThreadUnread(thread.id, state) && state.doneIds.includes(thread.id) == false)
		.map(thread => {
			const event = Array.isArray(thread.posts) ? lastEvent(thread, state) : null;
			const words = (event?.paragraphs ?? []).join(" ");
			return { threadId: thread.id, matterId: thread.matterId, namesViewer: handle != null && words.includes(handle) };
		});
}
