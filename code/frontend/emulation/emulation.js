// The app at work, inside onboarding (D-134). Onboarding embeds the app
// itself in a frame, with `?emulation` in the frame's address, and plays a
// scripted day of a fictional firm in it: the same pages, components and
// logic the member uses, driven by a script instead of a hand.
//
// That copy runs on the prototype's mock data alone and never reaches the
// bridge: the client does not call it there, and the window's native side
// refuses every message from a frame (Platform/WindowNavigation).
export const isEmulation = typeof window != "undefined" && new URLSearchParams(window.location.search).has("emulation");

// The two sides of the frame speak by messages, each marked with its side.
export const FROM_ONBOARDING = "singularity-onboarding";
export const FROM_EMULATION = "singularity-emulation";

// The member at onboarding's keyboard, once they introduce themselves (D-162).
export const DEMO_MEMBER = "m_you";
