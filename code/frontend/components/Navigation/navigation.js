import { createContext, useContext } from "react";

// Where the member is and how to move: the place shown, opening another, and
// stepping back and forward through the places visited, as a browser does
// (Alt+Left, Alt+Right, a mouse's side buttons, the sheet's arrows).
export const NavigationContext = createContext({
	place: { kind: "inbox" },
	open: () => {},
	back: () => {},
	forward: () => {},
	canBack: false,
	canForward: false,
});

export function useNavigation() {
	return useContext(NavigationContext);
}

// The address a place is written to, so a reload or a link opens it again:
// "#thread/t_sablewood", "#matter/x_sablewood/documents", "#team".
export function addressOf(place) {
	switch (place.kind) {
		case "thread": return `#thread/${place.threadId}`;
		case "mail": return `#mail/${place.mailId}`;
		case "letter": return `#letter/${place.letterId}`;
		case "messages": return place.memberId != null ? `#messages/${place.memberId}` : "#messages";
		case "matter": return `#matter/${place.matterId}${place.tab != null && place.tab != "overview" ? `/${place.tab}` : ""}`;
		case "document": return `#document/${place.documentId}`;
		case "search": return `#search/${encodeURIComponent(place.query ?? "")}`;
		case "profile": return `#profile/${place.memberId}`;
		case "calendar": return place.tab == "time" ? "#calendar/time" : "#calendar";
		case "invoice": return `#invoice/${place.matterId}`;
		case "inbox": return place.threadId != null ? `#inbox/${place.threadId}` : "#inbox";
		case "settings": return place.tab == "keys" ? "#settings/keys" : "#settings";
		default: return `#${place.kind}`;
	}
}

export function placeOf(hash) {
	const [kind, id, tab] = hash.replace("#", "").split("/");
	if (kind == "thread" && id != null) return { kind: "thread", threadId: id, from: "inbox" };
	if (kind == "mail" && id != null) return { kind: "mail", mailId: id, from: "inbox" };
	if (kind == "letter" && id != null) return { kind: "letter", letterId: id };
	if (kind == "inbox") return id != null ? { kind: "inbox", threadId: id } : { kind: "inbox" };
	if (kind == "messages" && id != null) return { kind: "messages", memberId: id };
	if (kind == "messages") return { kind: "messages" };
	if (kind == "matter" && id != null) return { kind: "matter", matterId: id, tab: tab ?? "overview" };
	if (kind == "document" && id != null) return { kind: "document", documentId: id };
	if (kind == "search") return { kind: "search", query: decodeURIComponent(id ?? "") };
	if (kind == "team") return { kind: "team" };
	if (kind == "matters") return { kind: "matters" };
	if (kind == "profile" && id != null) return { kind: "profile", memberId: id };
	if (kind == "hire") return { kind: "hire" };
	// The model accounts were a page of their own before they were Settings'
	// API keys: an old address opens them there.
	if (kind == "models") return { kind: "settings", tab: "keys" };
	if (kind == "settings") return id == "keys" ? { kind: "settings", tab: "keys" } : { kind: "settings" };
	if (kind == "calendar") return { kind: "calendar", tab: id == "time" ? "time" : "calendar" };
	if (kind == "invoice" && id != null) return { kind: "invoice", matterId: id };
	if (kind == "gallery") return { kind: "gallery" };
	return { kind: "inbox" };
}
