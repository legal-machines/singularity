// The Office application the pane is in, through Office's own script
// (office.js): which one it is, the file it has open, where the person's
// cursor stands, and the few things the pane does in the file itself.
// Everything else - the Matter, its threads, its colleagues - comes from Singularity
// (data.js).

const office = () => window.Office;

// Waits for Office, and says which application this is: "word", "excel",
// "powerpoint", "outlook", or "none" when the page is open outside Office.
export function officeReady() {
	return new Promise(resolve => {
		if (office() == null) {
			resolve("none");
			return;
		}
		office().onReady(info => {
			const host = String(info?.host ?? "").toLowerCase();
			resolve(["word", "excel", "powerpoint", "outlook"].includes(host) ? host : "none");
		});
	});
}

// The application Office names in the pane's address as it opens the pane
// ("?_host_Info=Word$Win32$16.01$en-US$..."), read without Office's script,
// which comes from Microsoft and does not come without the internet: "word",
// "excel", "powerpoint", "outlook", or "" where the address names none.
export function hostInAddress(address = window.location) {
	for (const part of [address.search ?? "", (address.hash ?? "").replace(/^#/, "?")]) {
		const named = new URLSearchParams(part).get("_host_Info") ?? "";
		const host = named.split(/[$|]/)[0].toLowerCase();
		if (["word", "excel", "powerpoint", "outlook"].includes(host)) return host;
	}
	return "";
}

// Whether this Office has a requirement set, as Office itself reports it.
export function supports(set, version) {
	try {
		return office()?.context?.requirements?.isSetSupported(set, version) == true;
	} catch {
		return false;
	}
}

// The open file's place: a path or a web address, "" for a new file.
export function fileLocation() {
	return office()?.context?.document?.url ?? "";
}

// Calls back when the person moves the cursor or the selection.
export function onSelectionChanged(callback) {
	const document = office()?.context?.document;
	if (document == null) return () => {};
	const handler = () => callback();
	document.addHandlerAsync(office().EventType.DocumentSelectionChanged, handler);
	return () => document.removeHandlerAsync(office().EventType.DocumentSelectionChanged, { handler });
}

// A clause number as a lawyer writes it, at the start of a paragraph's text:
// "2.1", "12", "3.4.1".
const NUMBER = /^\s*(?:section\s+)?(\d{1,2}(?:\.\d{1,2}){0,3})\.?\s+/i;

function clauseOf(text, listString) {
	const listed = (listString ?? "").replace(/\.$/, "").trim();
	if (/^\d{1,2}(\.\d{1,2}){0,3}$/.test(listed)) return listed;
	const typed = NUMBER.exec(text ?? "");
	return typed != null ? typed[1] : null;
}

// The words a clause opens with, as its heading: "Purpose" from
// "2.1 Purpose. The Receiving Party shall...".
function headingOf(text, number) {
	let rest = (text ?? "").trim();
	if (number != null) rest = rest.replace(NUMBER, "");
	const sentence = rest.split(/(?<=[.:])\s/)[0] ?? "";
	return sentence.length <= 60 ? sentence.replace(/[.:]$/, "") : "";
}

// Word: the clause the cursor stands in - its number, the words it opens with -
// and the words selected, if any. Walks back to the paragraph that carries the
// number when the cursor is in one of its later paragraphs.
export async function clauseAtCursor() {
	if (window.Word == null) return null;
	return window.Word.run(async context => {
		const selection = context.document.getSelection();
		selection.load("text");
		const paragraphs = selection.paragraphs;
		paragraphs.load("items/text");
		await context.sync();
		let paragraph = paragraphs.items[0];
		if (paragraph == null) return null;
		const selected = (selection.text ?? "").trim();
		for (let steps = 0; steps < 12 && paragraph != null; ++steps) {
			const list = paragraph.listItemOrNullObject;
			list.load("listString");
			paragraph.load("text");
			await context.sync();
			const number = clauseOf(paragraph.text, list.isNullObject ? "" : list.listString);
			if (number != null) {
				return { number, heading: headingOf(paragraph.text, number), text: paragraph.text.trim(), selected };
			}
			const previous = paragraph.getPreviousOrNullObject();
			previous.load("text");
			await context.sync();
			paragraph = previous.isNullObject ? null : previous;
		}
		return { number: null, heading: "", text: "", selected };
	});
}

// Word: selects the first place the words stand in the document, and says
// whether it found them. Long words are searched by their opening.
export async function showInDocument(words) {
	if (window.Word == null || (words ?? "").trim() == "") return false;
	const wanted = words.trim().replace(/\s+/g, " ").slice(0, 200);
	return window.Word.run(async context => {
		const found = context.document.body.search(wanted, { matchCase: false, ignoreSpace: true });
		found.load("items");
		await context.sync();
		if (found.items.length == 0) return false;
		found.items[0].select();
		await context.sync();
		return true;
	});
}

// Word: goes to a place in the document - the words quoted from it where they
// still stand as they were, else the section by its number - and selects it.
// Says whether it found it.
export async function revealPlace(place) {
	if (window.Word == null || place == null) return false;
	if (await showInDocument(place.quote).catch(() => false)) return true;
	if ((place.section ?? "") == "") return false;
	return window.Word.run(async context => {
		const paragraphs = context.document.body.paragraphs;
		paragraphs.load("items/text");
		await context.sync();
		const lists = paragraphs.items.map(paragraph => {
			const item = paragraph.listItemOrNullObject;
			item.load("listString");
			return item;
		});
		await context.sync();
		for (let index = 0; index < paragraphs.items.length; ++index) {
			const list = lists[index];
			if (clauseOf(paragraphs.items[index].text, list.isNullObject ? "" : list.listString) != place.section) continue;
			paragraphs.items[index].select();
			await context.sync();
			return true;
		}
		return false;
	});
}

// Word: what the person marked up in the document themselves - their tracked
// changes and their comments - leaving out the colleague's own. What Word
// cannot report on this version is said as nothing found.
export async function markupBy(personOtherThan) {
	if (window.Word == null) return { changes: [], comments: [] };
	const other = (personOtherThan ?? "").trim().toLowerCase();
	return window.Word.run(async context => {
		const body = context.document.body;
		const marked = { changes: [], comments: [] };
		if (supports("WordApi", "1.6")) {
			const changes = body.getTrackedChanges();
			changes.load("items/author,items/type,items/text");
			await context.sync();
			for (const change of changes.items) {
				if ((change.author ?? "").trim().toLowerCase() == other) continue;
				marked.changes.push({ author: change.author, type: change.type, text: change.text });
			}
		}
		if (supports("WordApi", "1.4")) {
			const comments = body.getComments();
			comments.load("items/authorName,items/content,items/resolved");
			await context.sync();
			const anchors = comments.items.map(comment => {
				const range = comment.getRange();
				range.load("text");
				return range;
			});
			await context.sync();
			comments.items.forEach((comment, index) => {
				if (comment.resolved == true || (comment.authorName ?? "").trim().toLowerCase() == other) return;
				marked.comments.push({ author: comment.authorName, text: comment.content, anchor: anchors[index].text });
			});
		}
		return marked;
	});
}

// ---- Excel -----------------------------------------------------------------------------

// A column's letters from its index from 0: 0 is "A", 26 is "AA".
export function columnName(index) {
	let name = "";
	for (let rest = index + 1; rest > 0; rest = Math.floor((rest - 1) / 26)) {
		name = String.fromCharCode(65 + ((rest - 1) % 26)) + name;
	}
	return name;
}

// How much of a selection is quoted to a colleague: as many cells as fit the
// place a message keeps (1,200 characters), the first forty rows and columns.
const MOST_ROWS = 40;
const MOST_COLUMNS = 40;
const MOST_QUOTE = 1200;

// The currency a column's heading names - "Unit price CV-220 (USD)" - which
// each plain figure under it is written with, so "421.00" reads as the amount
// it is, "421.00 USD", to a colleague and to Papinian Guard's comparison of
// amounts; null where the heading names none.
export function currencyOf(head) {
	const found = /\((USD|EUR|GBP|CHF|JPY|CNY|RUB|CAD|AUD|\$|€|£)\)/i.exec(String(head ?? ""));
	if (found == null) return null;
	const code = found[1].toUpperCase();
	return code == "$" ? "USD" : code == "€" ? "EUR" : code == "£" ? "GBP" : code;
}

// A cell's words with its column's currency, where they are a plain figure.
export function withCurrency(text, currency) {
	if (currency == null || !/^\(?-?[\d,]+(\.\d+)?\)?$/.test(text)) return text;
	return `${text} ${currency}`;
}

function quotedCells(parts) {
	let text = "";
	for (const part of parts) {
		const next = text == "" ? part : `${text} | ${part}`;
		if (next.length > MOST_QUOTE - 4) return `${text} | …`;
		text = next;
	}
	return text;
}

// How far above the selection its headings are looked for.
const MOST_ROWS_ABOVE = 200;

// The headings the selected columns stand under, and the row they stand in
// (from 0): the header row of the Excel table the selection is in; or else the
// first row of the block of filled rows the selection is in that is filled
// across nearly as many of its columns as the fullest - a checklist's own
// header row, never the title and date above it, never simply row 1 (a live
// run read "As of" and "2026-10-24" as a checklist's headings, 26 Oct 2026).
// null where no row of headings can be told.
async function headingsOver(context, sheet, range, columns) {
	const first = columnName(range.columnIndex);
	const last = columnName(range.columnIndex + columns - 1);
	if (supports("ExcelApi", "1.9")) {
		const tables = range.getTables(false);
		tables.load("items/name");
		await context.sync();
		if (tables.items.length > 0) {
			const header = tables.items[0].getHeaderRowRange();
			header.load("text, rowIndex, columnIndex");
			await context.sync();
			const texts = [];
			for (let column = 0; column < columns; ++column) {
				texts.push(String(header.text[0]?.[range.columnIndex + column - header.columnIndex] ?? "").trim());
			}
			return { row: header.rowIndex, texts };
		}
	}
	const top = Math.max(0, range.rowIndex - MOST_ROWS_ABOVE);
	const above = sheet.getRange(`${first}${top + 1}:${last}${range.rowIndex + 1}`);
	above.load("text");
	await context.sync();
	const filled = row => (above.text[row - top] ?? []).filter(text => String(text ?? "").trim() != "").length;
	let blockTop = range.rowIndex;
	while (blockTop > top && filled(blockTop - 1) > 0) --blockTop;
	// A block that runs past where it was looked for has headings out of sight.
	if (blockTop == top && top > 0) return null;
	let fullest = 0;
	for (let row = blockTop; row <= range.rowIndex; ++row) fullest = Math.max(fullest, filled(row));
	for (let row = blockTop; row <= range.rowIndex; ++row) {
		if (fullest > 0 && filled(row) * 5 >= fullest * 3) {
			return { row, texts: (above.text[row - top] ?? []).map(text => String(text ?? "").trim()) };
		}
	}
	return null;
}

// Excel: the cells selected - their sheet, their address on it, and what they
// hold as the lawyer sees them, each under its column's heading (headingsOver)
// and with its formula where it has one: "Unit price CV-220 (USD): G2 =
// 421.00", "Total commitment (USD): J2 = 5,550,000 {=H2*I2}". A blank cell is
// left out.
export async function cellsAtSelection() {
	if (window.Excel == null) return null;
	return window.Excel.run(async context => {
		const range = context.workbook.getSelectedRange();
		range.load("address, rowIndex, columnIndex, rowCount, columnCount");
		const sheet = range.worksheet;
		sheet.load("name");
		await context.sync();
		const rows = Math.min(range.rowCount, MOST_ROWS);
		const columns = Math.min(range.columnCount, MOST_COLUMNS);
		const first = columnName(range.columnIndex);
		const last = columnName(range.columnIndex + columns - 1);
		const shown = sheet.getRange(`${first}${range.rowIndex + 1}:${last}${range.rowIndex + rows}`);
		shown.load("text, formulas");
		await context.sync();
		const heads = await headingsOver(context, sheet, range, columns).catch(() => null);
		const parts = [];
		for (let row = 0; row < rows; ++row) {
			for (let column = 0; column < columns; ++column) {
				const text = String(shown.text[row][column] ?? "").trim();
				const formula = String(shown.formulas[row][column] ?? "");
				const isFormula = formula.startsWith("=");
				if (text == "" && !isFormula) continue;
				const cell = `${columnName(range.columnIndex + column)}${range.rowIndex + row + 1}`;
				// The heading row itself, and what stands above it, is said as it is.
				const head = heads != null && range.rowIndex + row > heads.row ? heads.texts[column] ?? "" : "";
				const said = withCurrency(text, currencyOf(head));
				parts.push(`${head != "" ? `${head}: ` : ""}${cell} = ${said}${isFormula ? ` {${formula}}` : ""}`);
			}
		}
		const address = String(range.address ?? "").split("!").pop().replaceAll("$", "");
		return { sheet: sheet.name, address, count: parts.length, reading: quotedCells(parts) };
	});
}

// Excel: selects cells on their sheet, as a message's place names them. Says
// whether it found them.
export async function selectCells(sheetName, address) {
	if (window.Excel == null || (address ?? "") == "") return false;
	return window.Excel.run(async context => {
		const sheets = context.workbook.worksheets;
		sheets.load("items/name");
		await context.sync();
		const sheet = (sheetName ?? "") == "" ? sheets.getActiveWorksheet() : sheets.items.find(entry => entry.name == sheetName);
		if (sheet == null) return false;
		sheet.activate();
		sheet.getRange(address).select();
		await context.sync();
		return true;
	}).catch(() => false);
}

// ---- PowerPoint -------------------------------------------------------------------------------

// The words on one slide, shape by shape as PowerPoint orders them, each line
// on its own; [] where this PowerPoint does not give a shape's words
// (PowerPointApi 1.4) or a shape has none, as a picture or a chart.
async function slideWords(index) {
	if (window.PowerPoint == null || !supports("PowerPointApi", "1.4")) return [];
	return window.PowerPoint.run(async context => {
		const shapes = context.presentation.slides.getItemAt(index).shapes;
		shapes.load("items/id");
		await context.sync();
		const lines = [];
		for (const shape of shapes.items) {
			try {
				const range = shape.textFrame.textRange;
				range.load("text");
				await context.sync();
				for (const line of String(range.text ?? "").split(/\r\n?|\n|\v/)) {
					if (line.trim() != "") lines.push(line.trim());
				}
			} catch {
				// No words to read in this shape.
			}
		}
		return lines;
	});
}

// PowerPoint: the slide selected - its number, its title, and its words as a
// colleague is to read them, its lines joined by " | " as far as the place a
// message keeps (1,200 characters).
export async function slideAtSelection() {
	const document = office()?.context?.document;
	if (document == null) return null;
	const selected = await new Promise(resolve => {
		document.getSelectedDataAsync(office().CoercionType.SlideRange, result => {
			resolve(result.status == office().AsyncResultStatus.Succeeded ? result.value?.slides?.[0] ?? null : null);
		});
	});
	if (selected == null || !(selected.index >= 1)) return null;
	const lines = await slideWords(selected.index - 1).catch(() => []);
	const title = String(selected.title ?? "").trim() || (lines[0] ?? "");
	return { number: selected.index, title, reading: quotedCells(lines.length > 0 ? lines : [title].filter(Boolean)) };
}

// PowerPoint: goes to a slide by its number. Says whether it went.
export function goToSlide(number) {
	return new Promise(resolve => {
		const document = office()?.context?.document;
		if (document?.goToByIdAsync == null || !(number >= 1)) {
			resolve(false);
			return;
		}
		document.goToByIdAsync(number, office().GoToType.Index, result => resolve(result.status == office().AsyncResultStatus.Succeeded));
	});
}

// A slide's number from a place's section ("Slide 2"); null for none.
export function slideNumberOf(section) {
	const found = /^slide\s+(\d{1,4})$/i.exec(String(section ?? "").trim());
	return found == null ? null : Number(found[1]);
}

// The open document's bytes, whole, as Base64: what Word or Excel holds now,
// saved or not.
export function fileBase64() {
	return new Promise((resolve, reject) => {
		office().context.document.getFileAsync(office().FileType.Compressed, { sliceSize: 4 * 1024 * 1024 }, opened => {
			if (opened.status != office().AsyncResultStatus.Succeeded) {
				reject(new Error("Office did not hand over the file."));
				return;
			}
			const file = opened.value;
			const parts = [];
			let index = 0;
			const next = () => {
				if (index >= file.sliceCount) {
					file.closeAsync();
					const bytes = new Uint8Array(parts.reduce((total, part) => total + part.length, 0));
					let offset = 0;
					for (const part of parts) {
						bytes.set(part, offset);
						offset += part.length;
					}
					let binary = "";
					for (let at = 0; at < bytes.length; at += 0x8000) binary += String.fromCharCode(...bytes.subarray(at, at + 0x8000));
					resolve(btoa(binary));
					return;
				}
				file.getSliceAsync(index, sliced => {
					if (sliced.status != office().AsyncResultStatus.Succeeded) {
						file.closeAsync();
						reject(new Error("Office did not hand over the file."));
						return;
					}
					parts.push(new Uint8Array(sliced.value.data));
					index += 1;
					next();
				});
			};
			next();
		});
	});
}
