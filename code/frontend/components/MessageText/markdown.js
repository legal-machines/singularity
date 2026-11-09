// What a message's words say as markdown, read into blocks and inline pieces
// for MessageText to draw. Nothing here makes HTML: every piece becomes a
// React element, so a message can never inject markup, and a link goes only
// where http(s) or mailto takes it.
//
// Blocks: paragraphs (a single line break stays a line break, as in any
// messenger), headings (#), quotes (>), lists - plain (-, *, +) and numbered,
// nested by indentation -, fenced code (``` or ~~~), rules (---) and tables
// (| a | b | over | --- | --- |). Inline: **bold** or __bold__, *italic* or
// _italic_, ~~struck~~, `code`, [words](address), bare addresses, and \
// before a mark to keep it as itself.

const FENCE = /^ {0,3}(`{3,}|~{3,})\s*([\w+#.-]*)\s*$/;
const HEADING = /^ {0,3}(#{1,6})\s+(.*?)\s*#*\s*$/;
const RULE = /^ {0,3}([-*_])(?:\s*\1){2,}\s*$/;
const QUOTE = /^ {0,3}>\s?(.*)$/;
const BULLET = /^(\s*)[-*+]\s+(.*)$/;
const NUMBERED = /^(\s*)(\d{1,9})[.)]\s+(.*)$/;
const TABLE_RULE = /^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)*\|?\s*$/;

function isBlank(line) {
	return line.trim() == "";
}

function depthOf(indent) {
	return Math.min(4, Math.floor(indent.replace(/\t/g, "  ").length / 2));
}

function cellsOf(line) {
	let row = line.trim();
	if (row.startsWith("|")) row = row.slice(1);
	if (row.endsWith("|") && row.endsWith("\\|") == false) row = row.slice(0, -1);
	const cells = [];
	let cell = "";
	for (let at = 0; at < row.length; at += 1) {
		if (row[at] == "\\" && row[at + 1] == "|") {
			cell += "|";
			at += 1;
		} else if (row[at] == "|") {
			cells.push(cell.trim());
			cell = "";
		} else {
			cell += row[at];
		}
	}
	cells.push(cell.trim());
	return cells;
}

function isTableAt(lines, at) {
	return lines[at].includes("|") && at + 1 < lines.length && TABLE_RULE.test(lines[at + 1]) && lines[at + 1].includes("-");
}

// A line that begins a block of its own, which ends a paragraph above it.
function startsBlock(lines, at) {
	const line = lines[at];
	return FENCE.test(line) || HEADING.test(line) || RULE.test(line) || QUOTE.test(line) || BULLET.test(line)
		|| NUMBERED.test(line) || isTableAt(lines, at);
}

export function parseBlocks(text) {
	const lines = String(text ?? "").replace(/\r\n?/g, "\n").split("\n");
	const blocks = [];
	let at = 0;
	while (at < lines.length) {
		const line = lines[at];
		if (isBlank(line)) {
			at += 1;
			continue;
		}
		const fence = FENCE.exec(line);
		if (fence != null) {
			const mark = fence[1];
			const body = [];
			at += 1;
			while (at < lines.length) {
				const closing = /^ {0,3}(`{3,}|~{3,})\s*$/.exec(lines[at]);
				if (closing != null && closing[1][0] == mark[0] && closing[1].length >= mark.length) {
					at += 1;
					break;
				}
				body.push(lines[at]);
				at += 1;
			}
			blocks.push({ type: "code", language: fence[2], text: body.join("\n") });
			continue;
		}
		const heading = HEADING.exec(line);
		if (heading != null) {
			blocks.push({ type: "heading", level: heading[1].length, text: heading[2] });
			at += 1;
			continue;
		}
		if (RULE.test(line)) {
			blocks.push({ type: "rule" });
			at += 1;
			continue;
		}
		if (QUOTE.test(line)) {
			const inner = [];
			while (at < lines.length && QUOTE.test(lines[at])) {
				inner.push(QUOTE.exec(lines[at])[1]);
				at += 1;
			}
			blocks.push({ type: "quote", blocks: parseBlocks(inner.join("\n")) });
			continue;
		}
		if (BULLET.test(line) || NUMBERED.test(line)) {
			const ordered = NUMBERED.test(line) && BULLET.test(line) == false;
			const items = [];
			let start = ordered ? Number(NUMBERED.exec(line)[2]) : 1;
			while (at < lines.length) {
				const current = lines[at];
				const bullet = BULLET.exec(current);
				const numbered = NUMBERED.exec(current);
				if (bullet != null && (ordered == false || depthOf(bullet[1]) > 0)) {
					items.push({ text: bullet[2], depth: depthOf(bullet[1]), ordered: false });
				} else if (numbered != null && (ordered == true || depthOf(numbered[1]) > 0)) {
					items.push({ text: numbered[3], depth: depthOf(numbered[1]), ordered: true, number: Number(numbered[2]) });
				} else if (items.length > 0 && isBlank(current) == false && /^\s{2,}/.test(current) && startsBlock(lines, at) == false) {
					// A line indented under an item goes on that item.
					items[items.length - 1].text += `\n${current.trim()}`;
				} else {
					break;
				}
				at += 1;
			}
			blocks.push({ type: "list", ordered, start, items });
			continue;
		}
		if (isTableAt(lines, at)) {
			const head = cellsOf(line);
			const align = cellsOf(lines[at + 1]).map(cell => cell.startsWith(":") && cell.endsWith(":") ? "center" : cell.endsWith(":") ? "right" : cell.startsWith(":") ? "left" : null);
			const rows = [];
			at += 2;
			while (at < lines.length && isBlank(lines[at]) == false && lines[at].includes("|")) {
				rows.push(cellsOf(lines[at]));
				at += 1;
			}
			blocks.push({ type: "table", head, align, rows });
			continue;
		}
		const paragraph = [line];
		at += 1;
		while (at < lines.length && isBlank(lines[at]) == false && startsBlock(lines, at) == false) {
			paragraph.push(lines[at]);
			at += 1;
		}
		blocks.push({ type: "paragraph", lines: paragraph });
	}
	return blocks;
}

const ESCAPABLE = "\\`*_~[]()#+-.!|>";

// A web source a colleague's words cite, where they cite it: its number
// between two marks of Unicode's private use, as native.js writes the
// backend's citation into the words - never anything a person types. Several
// side by side are one citation of several sources.
export const CITE_OPEN = "\uE000";
export const CITE_CLOSE = "\uE001";
const CITED = /^\uE000(\d{1,4})\uE001/;
const ADDRESS = /^(?:https?:\/\/|mailto:)[^\s<>"]+/i;

export function isSafeAddress(address) {
	return /^(https?:\/\/|mailto:)/i.test(address);
}

// A bare address's end, without the punctuation that closes the sentence
// around it, and without a closing bracket it did not open.
function addressEnd(address) {
	let end = address.length;
	while (end > 0 && /[.,;:!?'"*_~]/.test(address[end - 1])) end -= 1;
	let trimmed = address.slice(0, end);
	while (trimmed.endsWith(")") && (trimmed.match(/\(/g) ?? []).length < (trimmed.match(/\)/g) ?? []).length) trimmed = trimmed.slice(0, -1);
	return trimmed;
}

function isWordCharacter(character) {
	return character != null && /[\p{L}\p{N}]/u.test(character);
}

// The closing mark of a span that opened at `from`: never right after a space,
// and for _ never inside a word.
function closingOf(text, mark, from) {
	let at = text.indexOf(mark, from);
	while (at >= 0) {
		const before = text[at - 1];
		const after = text[at + mark.length];
		const isSpaced = before == null || /\s/.test(before);
		const isEscaped = before == "\\";
		const isWordy = mark[0] == "_" && isWordCharacter(after);
		// A single * or _ is not part of a double one.
		const isDouble = mark.length == 1 && (text[at + 1] == mark || text[at - 1] == mark);
		if (at > from && isSpaced == false && isEscaped == false && isWordy == false && isDouble == false) return at;
		at = text.indexOf(mark, at + 1);
	}
	return -1;
}

function pushText(pieces, text) {
	if (text == "") return;
	const last = pieces[pieces.length - 1];
	if (last?.type == "text") last.text += text;
	else pieces.push({ type: "text", text });
}

export function parseInline(text, { inLink = false } = {}) {
	const source = String(text ?? "");
	const pieces = [];
	let at = 0;
	while (at < source.length) {
		const character = source[at];
		const rest = source.slice(at);
		if (character == CITE_OPEN) {
			const cited = CITED.exec(rest);
			if (cited != null) {
				const last = pieces[pieces.length - 1];
				const number = Number(cited[1]);
				if (last?.type == "cite") last.numbers.push(number);
				else if (last?.type == "text" && /\s$/.test(last.text) && pieces[pieces.length - 2]?.type == "cite" && last.text.trim() == "") {
					pieces.pop();
					pieces[pieces.length - 1].numbers.push(number);
				} else pieces.push({ type: "cite", numbers: [number] });
				at += cited[0].length;
				continue;
			}
			at += 1;
			continue;
		}
		if (character == "\\" && ESCAPABLE.includes(source[at + 1] ?? "")) {
			pushText(pieces, source[at + 1]);
			at += 2;
			continue;
		}
		if (character == "`") {
			const run = /^`+/.exec(rest)[0];
			const close = source.indexOf(run, at + run.length);
			if (close > at) {
				pieces.push({ type: "code", text: source.slice(at + run.length, close).replace(/^ (.+) $/, "$1") });
				at = close + run.length;
				continue;
			}
			pushText(pieces, run);
			at += run.length;
			continue;
		}
		if (character == "[" && inLink == false) {
			const link = /^\[([^\]\n]+)\]\(\s*<?([^\s)>]+)>?\s*\)/.exec(rest);
			if (link != null && isSafeAddress(link[2])) {
				pieces.push({ type: "link", href: link[2], children: parseInline(link[1], { inLink: true }) });
				at += link[0].length;
				continue;
			}
		}
		if (inLink == false && (character == "h" || character == "H" || character == "m" || character == "M")
				&& isWordCharacter(source[at - 1]) == false) {
			const address = ADDRESS.exec(rest);
			if (address != null) {
				const kept = addressEnd(address[0]);
				if (kept.length > 8) {
					pieces.push({ type: "link", href: kept, children: [{ type: "text", text: kept }] });
					at += kept.length;
					continue;
				}
			}
		}
		const spans = [["~~", "strike"], ["**", "strong"], ["__", "strong"], ["*", "em"], ["_", "em"]];
		let isSpan = false;
		for (const [mark, type] of spans) {
			if (rest.startsWith(mark) == false) continue;
			const next = source[at + mark.length];
			if (next == null || /\s/.test(next)) continue;
			if (mark[0] == "_" && isWordCharacter(source[at - 1])) continue;
			if (mark.length == 1 && source[at + 1] == mark) continue;
			const close = closingOf(source, mark, at + mark.length);
			if (close < 0) continue;
			pieces.push({ type, children: parseInline(source.slice(at + mark.length, close), { inLink }) });
			at = close + mark.length;
			isSpan = true;
			break;
		}
		if (isSpan) continue;
		pushText(pieces, character);
		at += 1;
	}
	return pieces;
}

// The words alone, every mark gone: what a reply's quote, a preview or a
// notice shows of a message.
export function plainOf(text) {
	const inline = pieces => pieces.map(piece => piece.type == "text" || piece.type == "code" ? piece.text : inline(piece.children ?? [])).join("");
	return parseBlocks(text).map(block => {
		if (block.type == "paragraph") return inline(parseInline(block.lines.join(" ")));
		if (block.type == "heading") return inline(parseInline(block.text));
		if (block.type == "quote") return plainOf(block.blocks.map(inner => inner.type == "paragraph" ? inner.lines.join(" ") : "").join("\n"));
		if (block.type == "list") return block.items.map(item => inline(parseInline(item.text))).join(" ");
		if (block.type == "code") return block.text;
		if (block.type == "table") return [block.head, ...block.rows].map(row => row.map(cell => inline(parseInline(cell))).join(" ")).join(" ");
		return "";
	}).filter(part => part != "").join(" ");
}
