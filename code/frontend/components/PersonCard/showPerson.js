import { createContext, useContext } from "react";

// What opens a member's card (PersonCard.jsx) from anywhere in the window:
// show(memberId, the element pressed). Kept apart from the components so the
// card's file holds components only and reloads in place while it is edited.
export const ShowPerson = createContext(() => {});

export function useShowPerson() {
	return useContext(ShowPerson);
}
