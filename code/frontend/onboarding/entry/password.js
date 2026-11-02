// How strong the administrator's new password is (the owner, 2026-10-29: "a
// password for the admin, recommending a strong one"): an estimate of the
// guesses it would take, from its length and the kinds of characters in it,
// less what leans on something a guesser tries first - a common password or
// word, the organisation's own name or email, a year, a repeat or a run along
// the keyboard or the alphabet. Legal Machines takes 12 characters at least;
// a password this estimate calls weak is not taken either, and the words say
// why.
export const MIN_PASSWORD_LENGTH = 12;

const COMMON = [
	"password", "qwerty", "letmein", "welcome", "admin", "administrator", "iloveyou", "monkey", "dragon",
	"sunshine", "princess", "football", "baseball", "master", "secret", "login", "changeme", "default",
	"trustno1", "starwars", "superman", "batman", "shadow", "michael", "jennifer", "summer", "winter",
	"spring", "autumn", "london", "newyork", "moscow", "legal", "lawyer", "attorney", "counsel", "partner",
	"office", "company", "corporate", "singularity", "machines", "pass", "test", "user", "abc123",
];
// Rows of a keyboard and the alphabets, read either way.
const RUNS = [
	"abcdefghijklmnopqrstuvwxyz", "0123456789", "qwertyuiop", "asdfghjkl", "zxcvbnm",
	"абвгдеёжзийклмнопрстуфхцчшщъыьэюя", "йцукенгшщзхъ", "фывапролджэ", "ячсмитьбю",
];
// What a guesser substitutes back: p@ssw0rd is password.
const LEET = { "@": "a", "4": "a", "8": "b", "3": "e", "6": "g", "1": "l", "!": "i", "0": "o", "$": "s", "5": "s", "7": "t", "+": "t", "2": "z" };

// Bits a guesser spends on each thing it tries first.
const WORD_BITS = 11;
const OWN_WORD_BITS = 4;
const YEAR_BITS = 7;

function poolOf(characters) {
	let pool = 0;
	if (characters.some((character) => /[a-z]/.test(character))) pool += 26;
	if (characters.some((character) => /[A-Z]/.test(character))) pool += 26;
	if (characters.some((character) => /[0-9]/.test(character))) pool += 10;
	if (characters.some((character) => /[^A-Za-z0-9]/.test(character) && character.charCodeAt(0) < 128)) pool += 33;
	if (characters.some((character) => character.charCodeAt(0) >= 128)) pool += 64;
	return Math.max(pool, 1);
}

// The words a guesser who knows this organisation tries: its name, its
// email's name and domain.
function ownWords(context) {
	return context
		.flatMap((text) => String(text ?? "").toLowerCase().split(/[^\p{L}\p{N}]+/u))
		.filter((word) => word.length >= 4 && !["limited", "corporation", "incorporated"].includes(word));
}

// Marks the characters that lean on something guessable, and adds what that
// costs a guesser instead.
function guessable(text, plain, own) {
	const covered = new Array(text.length).fill(false);
	let bits = 0;
	const cover = (from, length, cost) => {
		let fresh = false;
		for (let at = from; at < from + length; at += 1) {
			if (!covered[at]) fresh = true;
			covered[at] = true;
		}
		if (fresh) bits += cost;
	};
	// Words as typed and with their substitutions read back.
	const find = (word, cost) => {
		for (const form of [text, plain]) {
			for (let at = form.indexOf(word); at >= 0; at = form.indexOf(word, at + 1)) cover(at, word.length, cost);
		}
	};
	for (const word of own) find(word, OWN_WORD_BITS);
	for (const word of COMMON) find(word, WORD_BITS);
	for (const match of text.matchAll(/(?:19|20)\d\d/g)) cover(match.index, 4, YEAR_BITS);
	// A character three times or more in a row counts once.
	for (const match of text.matchAll(/(.)\1{2,}/gu)) cover(match.index + 1, match[0].length - 1, 1);
	// Four or more in a row along a run, either way, as typed.
	for (let at = 0; at + 4 <= text.length; at += 1) {
		let length = 0;
		for (const run of RUNS) {
			for (const line of [run, [...run].reverse().join("")]) {
				const start = line.indexOf(text[at]);
				if (start < 0) continue;
				let end = at;
				while (end < text.length && line[start + (end - at)] === text[end]) end += 1;
				length = Math.max(length, end - at);
			}
		}
		if (length >= 4) cover(at, length, 3);
	}
	return { covered: covered.filter(Boolean).length, bits };
}

// { level, enough, words }: level is "short", "weak", "fair", "good" or
// "strong"; enough says whether Legal Machines takes it; words say what to
// do - a sentence for a short one, the rest of one after the level's name
// otherwise - or nothing when the level says it all. context: the
// organisation's name and email.
export function passwordStrength(password, context = []) {
	const characters = [...password];
	if (characters.length < MIN_PASSWORD_LENGTH) {
		const left = MIN_PASSWORD_LENGTH - characters.length;
		return {
			level: "short",
			enough: false,
			words: characters.length == 0 ? `At least ${MIN_PASSWORD_LENGTH} characters` : `${left} more character${left == 1 ? "" : "s"}`,
		};
	}
	const lower = characters.join("").toLowerCase();
	const plain = [...lower].map((character) => LEET[character] ?? character).join("");
	const found = guessable(lower, plain, ownWords(context));
	const bits = (characters.length - found.covered) * Math.log2(poolOf(characters)) + found.bits;
	if (bits < 45) return { level: "weak", enough: false, words: "avoid common words, names and dates" };
	if (bits < 60) return { level: "fair", enough: true, words: "a few more characters make it strong" };
	if (bits < 80) return { level: "good", enough: true, words: "" };
	return { level: "strong", enough: true, words: "" };
}
