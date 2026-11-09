import { nowDate, nowLocal } from "../data/clock.js";
import { matters, general, members, viewerId } from "../data/workspace.js";
import { DEMO_MEMBER, isEmulation } from "../emulation/emulation.js";

const numberWords = [
	"No", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten",
	"Eleven", "Twelve", "Thirteen", "Fourteen", "Fifteen", "Sixteen", "Seventeen", "Eighteen", "Nineteen", "Twenty",
];

// Whether a member is the person reading. In onboarding's sample firm, which
// is Gabriel's until the person introduces themselves (emulation/world.js),
// nobody but the person is: a colleague joining the firm read "Gabriel Moore
// (you)".
export function isViewerMember(memberId) {
	return memberId == viewerId && (isEmulation == false || memberId == DEMO_MEMBER);
}

// A member's name as the person reading sees it: their own marked "(you)".
export function nameAsSeen(member) {
	return isViewerMember(member.id) ? `${member.name} (you)` : member.name;
}

export function findMember(memberId) {
	return members.find(member => member.id == memberId);
}

// The runs of a text, each with whether its author wrote it as **emphasis**.
// As in Markdown, emphasis neither begins nor ends with a space.
export function emphasised(text) {
	return (text ?? "").split(/(\*\*(?![\s*])[^*\n]*?[^\s*]\*\*)/g)
		.filter(piece => piece != "")
		.map(piece => piece.length > 4 && piece.startsWith("**") && piece.endsWith("**")
			? { text: piece.slice(2, -2), isStrong: true }
			: { text: piece, isStrong: false });
}

// A text as a one-line preview shows it - an inbox row, the sidebar's Recent:
// its emphasis as plain words, never its asterisks.
export function plainWords(text) {
	return emphasised(text).map(piece => piece.text).join("");
}

export function findMatter(matterId) {
	if (matterId == general.id) return general;
	return matters.find(matter => matter.id == matterId);
}

// A member's place in the firm, in one line, as a US firm's directory sets
// it: the position, then the practice in brackets, never joined by a dot:
// "Senior Associate (Labor & Employment)". An AI colleague's position is its
// role, and a badge beside its name says it is one.
export function roleLine(member) {
	const position = positionOf(member);
	return member.practice != null ? `${position} (${member.practice})` : position;
}

// A member's office in one phrase, its parts in one order: their role on the
// firm's scale, then their own words for their position, in their practice -
// "Partner, IP Lead in Intellectual Property", "Partner in Mergers &
// Acquisitions", "Counsel, Head of Finance". A part not given is left out,
// and a position that says the role again is said once.
export function officeLine(member) {
	if (member == null) return "";
	const role = roleOf(member);
	const words = positionWordsOf(member);
	const position = words.toLowerCase() == role.toLowerCase() ? "" : words;
	const practice = (member.practice ?? "").trim();
	const inPractice = practice == "" ? "" : ` in ${practice}`;
	if (role == "") return position == "" ? practice : `${position}${inPractice}`;
	return position == "" ? `${role}${inPractice}` : `${role}, ${position}${inPractice}`;
}

// The member's role on the firm's scale: a person's position in the firm, an
// AI colleague's the position it ranks with.
function roleOf(member) {
	const role = member.kind == "ai_colleague" ? member.office?.ranksWith || member.ranksWith : member.position;
	return (role ?? "").trim();
}

// The member's own words for their position, where the firm gave them.
function positionWordsOf(member) {
	const words = member.kind == "ai_colleague" ? member.office?.title || (member.role != "AI colleague" ? member.role : "") : member.title;
	return (words ?? "").trim();
}

// The firm's one scale of positions, the highest first.
export const positionLevels = ["Partner", "Senior Counsel", "Of Counsel", "Counsel", "Senior Associate", "Associate", "Paralegal"];

// Members in the firm's order: by the level of their position, then by
// surname and given name. An AI colleague ranks with the position its role
// holds, and its one name is its surname.
export function byFirmOrder(first, second) {
	const level = member => {
		const index = positionLevels.indexOf(member.position ?? member.ranksWith);
		return index < 0 ? positionLevels.length : index;
	};
	const sortName = member => {
		const words = member.name.split(" ");
		return `${words[words.length - 1]} ${words.slice(0, -1).join(" ")}`.trim();
	};
	return level(first) - level(second) || sortName(first).localeCompare(sortName(second));
}

// A member's role on the firm's scale, as a US firm writes it: "Senior
// Associate"; an AI colleague's, the position it ranks with.
export function positionOf(member) {
	// The role first, and alone where one thing is said; the member's own
	// words for their position only where no role is given.
	return roleOf(member) || positionWordsOf(member) || (member.role ?? "");
}

export function firstName(member) {
	return member.name.split(" ")[0];
}

// Twist's ages: "moments ago", "7m", "2h", "1d".
export function formatAge(iso) {
	const minutes = Math.floor((nowDate() - new Date(iso)) / 60000);

	if (minutes < 1) return "moments ago";
	if (minutes < 60) return `${minutes}m`;
	if (minutes < 60 * 24) return `${Math.floor(minutes / 60)}h`;
	return `${Math.floor(minutes / (60 * 24))}d`;
}

export function formatClock(iso) {
	return new Intl.DateTimeFormat("en-US", { hour: "numeric", minute: "2-digit" }).format(new Date(iso));
}

export function formatDay(iso) {
	const date = new Date(iso);
	const now = nowDate();
	const yesterday = new Date(now);
	yesterday.setDate(now.getDate() - 1);

	if (date.toDateString() == now.toDateString()) return "Today";
	if (date.toDateString() == yesterday.toDateString()) return "Yesterday";
	return new Intl.DateTimeFormat("en-US", { weekday: "long", month: "long", day: "numeric" }).format(date);
}

// When something happened, as a sentence goes on: "today at 9:58 AM",
// "yesterday at 4:40 PM", "on Thursday, October 23".
export function formatWhen(iso) {
	const day = formatDay(iso);
	if (day == "Today") return `today at ${formatClock(iso)}`;
	if (day == "Yesterday") return `yesterday at ${formatClock(iso)}`;
	return `on ${day}`;
}

export function countWord(count) {
	return numberWords[count] ?? String(count);
}

// What waits for the viewer, said in one sentence: "Two decisions and one
// question wait for you."
export function waitingSentence(decisionCount, questionCount) {
	const parts = [];
	if (decisionCount > 0) parts.push(`${countWord(decisionCount).toLowerCase()} ${decisionCount == 1 ? "decision" : "decisions"}`);
	if (questionCount > 0) parts.push(`${countWord(questionCount).toLowerCase()} ${questionCount == 1 ? "question" : "questions"}`);

	if (parts.length == 0) return "Nothing waits for you. Your colleagues carry the rest.";

	const sentence = parts.join(" and ");
	const verb = decisionCount + questionCount == 1 ? "waits" : "wait";
	return `${sentence.charAt(0).toUpperCase()}${sentence.slice(1)} ${verb} for you.`;
}

// A moment as a member reads it in a thread: the time alone today,
// "Yesterday 5:40 PM", or "Oct 21, 3:00 PM".
export function formatMoment(iso) {
	const day = formatDay(iso);
	const clock = formatClock(iso);
	if (day == "Today") return clock;
	if (day == "Yesterday") return `Yesterday ${clock}`;
	return `${new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric" }).format(new Date(iso))}, ${clock}`;
}

// A date to come, as a deadline reads: "today", "tomorrow", "Wed, Oct 28".
export function formatDue(day) {
	const date = new Date(`${day}T12:00:00`);
	const today = new Date(`${nowLocal().slice(0, 10)}T12:00:00`);
	const days = Math.round((date - today) / 86400000);
	if (days == 0) return "today";
	if (days == 1) return "tomorrow";
	if (days < 0) return `${-days} ${days == -1 ? "day" : "days"} ago`;
	return new Intl.DateTimeFormat("en-US", { weekday: "short", month: "short", day: "numeric" }).format(date);
}

export function daysUntil(day) {
	const date = new Date(`${day}T12:00:00`);
	const today = new Date(`${nowLocal().slice(0, 10)}T12:00:00`);
	return Math.round((date - today) / 86400000);
}
