// Folio's icons: one 1.5-pixel stroke on a 20-pixel square, round ends and
// joins, drawn for this interface. An icon stands beside a word that names
// the same thing, or alone with a label that names it (IconButton).
const paths = {
	search: <><circle cx="9" cy="9" r="5.75" /><path d="M13.25 13.25L17 17" /></>,
	inbox: <><path d="M3 11.5L5.2 4.6C5.4 4 5.9 3.5 6.6 3.5H13.4C14.1 3.5 14.6 4 14.8 4.6L17 11.5V15C17 15.8 16.3 16.5 15.5 16.5H4.5C3.7 16.5 3 15.8 3 15V11.5Z" /><path d="M3 11.5H7L8 13.5H12L13 11.5H17" /></>,
	saved: <path d="M5.5 3.5H14.5V17L10 13.8L5.5 17V3.5Z" />,
	saved_filled: <path d="M5.5 3.5H14.5V17L10 13.8L5.5 17V3.5Z" fill="currentColor" />,
	messages: <><path d="M4.2 15.2C3.1 13.9 2.5 12.4 2.5 10.8C2.5 6.8 5.9 3.5 10 3.5C14.1 3.5 17.5 6.8 17.5 10.8C17.5 14.8 14.1 18 10 18C8.9 18 7.9 17.8 7 17.4L3.5 18L4.2 15.2Z" /><path d="M7 10.8H7.01M10 10.8H10.01M13 10.8H13.01" strokeWidth="2" /></>,
	calendar: <><rect x="3.5" y="4.5" width="13" height="12" rx="2" /><path d="M3.5 8.5H16.5M7 2.5V5.5M13 2.5V5.5" /></>,
	back: <path d="M12.5 4.5L7 10L12.5 15.5" />,
	bug: <><path d="M7.25 6.75C7.25 5.23 8.48 4 10 4C11.52 4 12.75 5.23 12.75 6.75" /><path d="M6.25 8.25H13.75V12.5C13.75 14.57 12.07 16.25 10 16.25C7.93 16.25 6.25 14.57 6.25 12.5V8.25Z" /><path d="M10 8.25V16.25M6.25 11H3.5M16.5 11H13.75M6.5 14L4.25 15.75M13.5 14L15.75 15.75M6.5 8.25L4.5 6.5M13.5 8.25L15.5 6.5" /></>,
	forward: <path d="M7.5 4.5L13 10L7.5 15.5" />,
	up: <path d="M4.5 12.5L10 7L15.5 12.5" />,
	arrow_up: <path d="M10 15.5V4.75M5.25 9.5L10 4.75L14.75 9.5" />,
	down: <path d="M4.5 7.5L10 13L15.5 7.5" />,
	chevron: <path d="M6 8L10 12L14 8" />,
	chevron_right: <path d="M8 6L12 10L8 14" />,
	selector: <path d="M7 7.5L10 4.5L13 7.5M7 12.5L10 15.5L13 12.5" />,
	check: <path d="M4.5 10.5L8.2 14L15.5 6.5" />,
	closed: <><circle cx="10" cy="10" r="7" /><path d="M6.8 10.2L9 12.3L13.3 7.9" /></>,
	document: <><path d="M5.5 2.5H11.5L15 6V17.5H5.5V2.5Z" /><path d="M11.5 2.5V6H15M8 10H12.5M8 13H12.5" /></>,
	send: <path d="M3.5 10L16.5 4L12.5 16.5L9.5 10.5L3.5 10Z" />,
	people: <><circle cx="7.5" cy="7" r="2.75" /><path d="M2.75 15.5C3.15 12.9 5.1 11.25 7.5 11.25C9.9 11.25 11.85 12.9 12.25 15.5" /><circle cx="14" cy="7.25" r="2.25" /><path d="M14 10.75C15.85 10.75 17.25 12 17.5 14" /></>,
	menu: <path d="M3.5 6H16.5M3.5 10H16.5M3.5 14H16.5" />,
	// Twist's sidebar mark, nearly as it is: a window whose left panel is shaded.
	sidebar: <><path d="M4.25 3H7.5V17H4.25C2.87 17 1.75 15.88 1.75 14.5V5.5C1.75 4.12 2.87 3 4.25 3Z" fill="currentColor" fillOpacity="0.4" stroke="none" /><rect x="1.75" y="3" width="16.5" height="14" rx="2.5" /><path d="M7.5 3V17" /></>,
	plus: <path d="M10 4.5V15.5M4.5 10H15.5" />,
	close: <path d="M5.5 5.5L14.5 14.5M14.5 5.5L5.5 14.5" />,
	trash: <><path d="M4 6H16M8 6V4.5H12V6M5.5 6L6.3 16.5H13.7L14.5 6" /><path d="M8.5 9V13.5M11.5 9V13.5" /></>,
	edit: <path d="M12.8 4.2L15.8 7.2L7.5 15.5L4 16L4.5 12.5L12.8 4.2Z" />,
	place: <><path d="M10 17.5C10 17.5 15.5 12.6 15.5 8.5C15.5 5.5 13 3 10 3C7 3 4.5 5.5 4.5 8.5C4.5 12.6 10 17.5 10 17.5Z" /><circle cx="10" cy="8.5" r="2" /></>,
	clock: <><circle cx="10" cy="10" r="7" /><path d="M10 6V10L12.5 12" /></>,
	notes: <path d="M4 5.5H16M4 10H16M4 14.5H11" />,
	repeat: <><path d="M4 9V8C4 6.3 5.3 5 7 5H15M12.5 2.5L15 5L12.5 7.5" /><path d="M16 11V12C16 13.7 14.7 15 13 15H5M7.5 17.5L5 15L7.5 12.5" /></>,
	mail: <><rect x="3" y="5" width="14" height="10" rx="1.5" /><path d="M3.5 6L10 11L16.5 6" /></>,
	// An address, for a letter's conversation.
	at: <><circle cx="10" cy="10" r="3.25" /><path d="M13.25 6.75V10.75A2.25 2.25 0 0 0 17.75 10.75V10A7.75 7.75 0 1 0 14.75 16.15" /></>,
	open: <path d="M7 13L13 7M8 7H13V12" />,
	matter: <><path d="M2.75 6.25C2.75 5.42 3.42 4.75 4.25 4.75H7.6L9.1 6.25H15.75C16.58 6.25 17.25 6.92 17.25 7.75V14.75C17.25 15.58 16.58 16.25 15.75 16.25H4.25C3.42 16.25 2.75 15.58 2.75 14.75V6.25Z" /></>,
	reply: <path d="M8 5L3.5 9.5L8 14M4 9.5H11.5C14 9.5 16 11.5 16 14V15" />,
	smile: <><circle cx="10" cy="10" r="7" /><path d="M7.25 11.6C7.9 12.65 8.9 13.2 10 13.2C11.1 13.2 12.1 12.65 12.75 11.6" /><path d="M7.6 8.2H7.61M12.4 8.2H12.41" strokeWidth="2" /></>,
	copy: <><rect x="7" y="7" width="9" height="9" rx="2" /><path d="M13 7V5.5C13 4.67 12.33 4 11.5 4H5.5C4.67 4 4 4.67 4 5.5V11.5C4 12.33 4.67 13 5.5 13H7" /></>,
	key: <><circle cx="7" cy="10" r="3.5" /><path d="M10.5 10H17M14.5 10V12.5M16.5 10V12" /></>,
	usage: <><path d="M3.25 14.5A7 7 0 1 1 16.75 14.5" /><path d="M10 12.65L13.25 9" /></>,
	archive: <><rect x="2.75" y="4" width="14.5" height="3.5" rx="1" /><path d="M4.25 7.5V15C4.25 15.83 4.92 16.5 5.75 16.5H14.25C15.08 16.5 15.75 15.83 15.75 15V7.5" /><path d="M8.25 10.5H11.75" /></>,
	plug: <><path d="M7.5 2.75V6M12.5 2.75V6" /><path d="M5 6H15V8.75C15 11.51 12.76 13.75 10 13.75C7.24 13.75 5 11.51 5 8.75V6Z" /><path d="M10 13.75V17.25" /></>,
	// This computer: its code and what it holds.
	computer: <path d="M4.5 13.5V5.5A1 1 0 0 1 5.5 4.5H14.5A1 1 0 0 1 15.5 5.5V13.5M2.5 15.5H17.5" />,
	// Power: signing out.
	power: <path d="M10 3V9.5M6.11 5.5A6.25 6.25 0 1 0 13.89 5.5" />,
	more: <path d="M5 10H5.01M10 10H10.01M15 10H15.01" strokeWidth="2.25" />,
	alert: <><path d="M10 3.25L17.5 16.25H2.5L10 3.25Z" /><path d="M10 8.25V11.25M10 13.75H10.01" /></>,
	info: <><circle cx="10" cy="10" r="7" /><path d="M10 9.25V13.75M10 6.5H10.01" /></>,
	team: <><circle cx="10" cy="6.75" r="2.75" /><path d="M5 16C5.4 13.2 7.4 11.5 10 11.5C12.6 11.5 14.6 13.2 15 16" /><path d="M4.75 9.5C3.6 9.9 2.75 11 2.6 12.5M15.25 9.5C16.4 9.9 17.25 11 17.4 12.5" /></>,
	hire: <><circle cx="8.5" cy="6.75" r="2.75" /><path d="M3.5 16C3.9 13.2 5.9 11.5 8.5 11.5C9.6 11.5 10.6 11.8 11.4 12.3M15 11V17M12 14H18" /></>,
	matters: <><path d="M2.75 5.75C2.75 4.92 3.42 4.25 4.25 4.25H7.69C8.09 4.25 8.47 4.41 8.75 4.69L9.81 5.75H15.75C16.58 5.75 17.25 6.42 17.25 7.25V14.75C17.25 15.58 16.58 16.25 15.75 16.25H4.25C3.42 16.25 2.75 15.58 2.75 14.75V5.75Z" /><path d="M2.75 8.25H17.25" /></>,
	history: <><path d="M3.5 10A6.5 6.5 0 1 0 5.4 5.4" /><path d="M3.25 3.5V6.25H6" /><path d="M10 7V10.25L12.25 11.75" /></>,
	filter: <path d="M3.5 5H16.5M6 10H14M8.5 15H11.5" />,
	link: <><path d="M8.5 11.5L11.5 8.5" /><path d="M9.25 6.25L10.6 4.9C11.9 3.6 14 3.6 15.3 4.9C16.6 6.2 16.6 8.3 15.3 9.6L13.75 11.15" /><path d="M10.75 13.75L9.4 15.1C8.1 16.4 6 16.4 4.7 15.1C3.4 13.8 3.4 11.7 4.7 10.4L6.25 8.85" /></>,
	return: <><path d="M7.5 5L3.5 9L7.5 13" /><path d="M3.75 9H12C14.2 9 16 10.8 16 13V15" /></>,
	attach: <path d="M14.5 9.25L9.6 14.15C8.2 15.55 5.95 15.55 4.55 14.15C3.15 12.75 3.15 10.5 4.55 9.1L10.1 3.55C11.05 2.6 12.6 2.6 13.55 3.55C14.5 4.5 14.5 6.05 13.55 7L8.15 12.4C7.7 12.85 6.95 12.85 6.5 12.4C6.05 11.95 6.05 11.2 6.5 10.75L11.25 6" />,
	// A gear, never rays that read as a sun.
	settings: <><path d="M8.47 4.3L8.75 2.5H11.25L11.53 4.3A5.9 5.9 0 0 1 12.95 4.89L14.41 3.81L16.19 5.59L15.11 7.05A5.9 5.9 0 0 1 15.7 8.47L17.5 8.75V11.25L15.7 11.53A5.9 5.9 0 0 1 15.11 12.95L16.19 14.41L14.41 16.19L12.95 15.11A5.9 5.9 0 0 1 11.53 15.7L11.25 17.5H8.75L8.47 15.7A5.9 5.9 0 0 1 7.05 15.11L5.59 16.19L3.81 14.41L4.89 12.95A5.9 5.9 0 0 1 4.3 11.53L2.5 11.25V8.75L4.3 8.47A5.9 5.9 0 0 1 4.89 7.05L3.81 5.59L5.59 3.81L7.05 4.89A5.9 5.9 0 0 1 8.47 4.3Z" /><circle cx="10" cy="10" r="2.5" /></>,
	download: <><path d="M10 3.5V12.5M6.5 9L10 12.5L13.5 9" /><path d="M4 14.5V16H16V14.5" /></>,
	print: <><path d="M6 7V3.5H14V7" /><rect x="3.5" y="7" width="13" height="6.5" rx="1.5" /><path d="M6 11.5H14V16.5H6V11.5Z" /></>,
	timer: <><circle cx="10" cy="11" r="6" /><path d="M10 8V11L12 12.5M8 2.75H12" /></>,
	play: <path d="M7 5.5L14.5 10L7 14.5V5.5Z" />,
	pause: <path d="M7.5 5.5V14.5M12.5 5.5V14.5" />,
	shield: <path d="M10 2.75L16 5V9.5C16 13.2 13.4 16.2 10 17.25C6.6 16.2 4 13.2 4 9.5V5L10 2.75Z" />,
	// What an AI provider sees of a conversation.
	eye: <><path d="M2.5 10C4.2 6.5 6.9 4.75 10 4.75C13.1 4.75 15.8 6.5 17.5 10C15.8 13.5 13.1 15.25 10 15.25C6.9 15.25 4.2 13.5 2.5 10Z" /><circle cx="10" cy="10" r="2.5" /></>,
	// What is typed out of sight, shown again: the eye crossed through.
	eye_off: <><path d="M2.5 10C4.2 6.5 6.9 4.75 10 4.75C13.1 4.75 15.8 6.5 17.5 10C15.8 13.5 13.1 15.25 10 15.25C6.9 15.25 4.2 13.5 2.5 10Z" /><circle cx="10" cy="10" r="2.5" /><path d="M3.75 3.75L16.25 16.25" /></>,
	external: <><path d="M11 3.5H16.5V9" /><path d="M16.5 3.5L9.5 10.5" /><path d="M14 12V16H4V6H8" /></>,
};

export function Icon({ name, size }) {
	return (
		<svg
			width={size ?? 20}
			height={size ?? 20}
			viewBox="0 0 20 20"
			fill="none"
			stroke="currentColor"
			strokeWidth="1.5"
			strokeLinecap="round"
			strokeLinejoin="round"
			aria-hidden="true"
			focusable="false"
		>
			{paths[name]}
		</svg>
	);
}
