// The conversations Messages lists (D-042): one with each member - an AI
// colleague or a person, side by side (UI-5) - and one with each
// correspondent whose letters reached the firm's Matters through Outlook
// (D-106). In the real app they come from the backend (data/native.js); in
// the prototype from its sample chats and mail, labelled mock data (AG-12).

import { hasNative, realConversations } from "../../data/native.js";
import { projectMail } from "../../data/mail.js";
import { chats, members, viewerId } from "../../data/workspace.js";

// Every conversation, the one with the latest word first.
export function conversationsFor(state) {
	const found = hasNative() ? realConversations() : sampleConversations(state);
	return found
		.map(entry => ({ ...entry, latestAt: entry.items.at(-1)?.at ?? "" }))
		.sort((first, second) => second.latestAt.localeCompare(first.latestAt));
}

// A conversation's key from the address Messages was opened at: a member's
// id, or "mail:" and a correspondent's address.
export function keyOf(id) {
	if (id == null || id == "") return null;
	return id.startsWith("mail:") ? id : `member:${id}`;
}

// The address that opens a conversation: the inverse of keyOf.
export function idOf(key) {
	return key.startsWith("member:") ? key.slice("member:".length) : key;
}

// The words a conversation's line in the list shows of its latest item.
export function previewOf(entry) {
	const last = entry.items.at(-1);
	if (last == null) return "";
	const words = last.subject != null && last.subject != "" ? last.subject : (last.paragraphs ?? []).join(" ");
	return last.isOwn == true ? `You: ${words}` : words;
}

// A correspondent's face: their initials on a ground their address chooses,
// the same on every computer.
const GROUNDS = ["#6b5a8e", "#2a7d8c", "#8b6a4b", "#4a4f61", "#5f7a3e", "#9a4f5c", "#3f6a96"];

export function correspondentFace(contact) {
	const words = (contact.name ?? contact.address).replace(/@.*/, "").split(/[\s._-]+/).filter(Boolean);
	const initials = words.slice(0, 2).map(word => word[0].toUpperCase()).join("") || "?";
	let hash = 0;
	for (const character of contact.address) hash = (hash * 31 + character.charCodeAt(0)) >>> 0;
	return { id: `mail_${contact.address}`, kind: "person", name: contact.name ?? contact.address, initials, color: contact.color ?? GROUNDS[hash % GROUNDS.length] };
}

// The prototype's conversations: its sample personal chats with what the
// member wrote in this session, every member it has not talked to yet, and
// its sample mail by correspondent.
function sampleConversations(state) {
	const found = [];
	for (const member of members) {
		if (member.id == viewerId) continue;
		const chat = chats.find(item => item.memberId == member.id);
		const messages = [...(chat?.messages ?? []), ...(state?.extraMessages?.[member.id] ?? [])];
		found.push({
			key: `member:${member.id}`,
			kind: "member",
			memberId: member.id,
			items: messages.map(message => ({
				...message,
				isOwn: message.authorId == viewerId,
				paragraphs: message.text != null && message.text != "" ? [message.text] : [],
			})),
			isAnswering: (state?.typing?.[member.id] ?? []).includes(member.id),
			send: { kind: "sample" },
		});
	}
	const byAddress = new Map();
	for (const message of projectMail) {
		const entry = byAddress.get(message.from) ?? { contact: message.contact, items: [] };
		entry.items.push({
			id: message.id,
			at: message.at,
			isOwn: false,
			subject: message.subject,
			paragraphs: [message.greeting, ...message.paragraphs, message.signoff.join("\n")].filter(text => text != null && text != ""),
			attachments: message.attachments,
			matterId: message.matterId,
		});
		byAddress.set(message.from, entry);
	}
	for (const [address, entry] of byAddress) {
		found.push({
			key: `mail:${address}`,
			kind: "mail",
			contact: { ...entry.contact, address },
			items: entry.items.sort((first, second) => first.at.localeCompare(second.at)),
			send: { kind: "sample" },
		});
	}
	return found;
}
