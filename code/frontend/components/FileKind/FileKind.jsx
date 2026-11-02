import styles from "./FileKind.module.css";

// What kind of file a name is, and how a lawyer knows it at a glance: a page
// with its corner folded and its type on a label in the colour of its kind -
// Word's blue, Excel's green, PowerPoint's orange, a PDF's red, Outlook's blue
// for mail, plain text in grey (tokens.css, --kind-*). One drawing for every
// place a file stands: a document's row, a file in a message, a draft.

const KINDS = {
	docx: { family: "word", label: "DOCX", word: "Word document" },
	doc: { family: "word", label: "DOC", word: "Word document" },
	docm: { family: "word", label: "DOCM", word: "Word document" },
	dotx: { family: "word", label: "DOTX", word: "Word template" },
	rtf: { family: "word", label: "RTF", word: "Rich text document" },
	odt: { family: "word", label: "ODT", word: "OpenDocument text" },
	xlsx: { family: "sheet", label: "XLSX", word: "Excel workbook" },
	xlsm: { family: "sheet", label: "XLSM", word: "Excel workbook" },
	xls: { family: "sheet", label: "XLS", word: "Excel workbook" },
	ods: { family: "sheet", label: "ODS", word: "OpenDocument spreadsheet" },
	csv: { family: "sheet", label: "CSV", word: "Comma-separated table" },
	tsv: { family: "sheet", label: "TSV", word: "Tab-separated table" },
	pptx: { family: "slides", label: "PPTX", word: "PowerPoint presentation" },
	ppt: { family: "slides", label: "PPT", word: "PowerPoint presentation" },
	odp: { family: "slides", label: "ODP", word: "OpenDocument presentation" },
	key: { family: "slides", label: "KEY", word: "Keynote presentation" },
	pdf: { family: "pdf", label: "PDF", word: "PDF document" },
	eml: { family: "mail", label: "EML", word: "Email message" },
	msg: { family: "mail", label: "MSG", word: "Outlook message" },
	png: { family: "image", label: "PNG", word: "Picture" },
	jpg: { family: "image", label: "JPG", word: "Picture" },
	jpeg: { family: "image", label: "JPG", word: "Picture" },
	gif: { family: "image", label: "GIF", word: "Picture" },
	tif: { family: "image", label: "TIF", word: "Picture" },
	tiff: { family: "image", label: "TIF", word: "Picture" },
	heic: { family: "image", label: "HEIC", word: "Picture" },
	webp: { family: "image", label: "WEBP", word: "Picture" },
	json: { family: "data", label: "JSON", word: "JSON data" },
	xml: { family: "data", label: "XML", word: "XML data" },
	html: { family: "data", label: "HTML", word: "Web page" },
	htm: { family: "data", label: "HTM", word: "Web page" },
	txt: { family: "text", label: "TXT", word: "Text file" },
	text: { family: "text", label: "TXT", word: "Text file" },
	md: { family: "text", label: "MD", word: "Markdown document" },
	markdown: { family: "text", label: "MD", word: "Markdown document" },
};

// The kind of a file by its name's last extension: "docx", "pdf", "" for none.
export function kindOf(name) {
	const last = String(name ?? "").split(/[\\/]/).pop();
	const dot = last.lastIndexOf(".");
	return dot <= 0 ? "" : last.slice(dot + 1).toLowerCase();
}

// "word", "sheet", "slides", "pdf", "mail", "image", "data", "text" or "other".
export function familyOf(kind) {
	return KINDS[kind]?.family ?? "other";
}

// The label on the page: "DOCX", "PDF"; a kind it does not know, as written,
// at most four letters; "FILE" for a name with none.
export function labelOf(kind) {
	if (KINDS[kind] != null) return KINDS[kind].label;
	return kind == "" ? "FILE" : kind.slice(0, 4).toUpperCase();
}

// What the file is, in words: "Word document", "PDF document".
export function wordOf(kind) {
	if (KINDS[kind] != null) return KINDS[kind].word;
	return kind == "" ? "File" : `${kind.toUpperCase()} file`;
}

// The page itself. `size` is "tile" (a file in a message or a draft, 32 by 40)
// or "row" (a document in a list, 26 by 32). `dim` is a file no longer there.
export function FileKindIcon({ name, size = "tile", dim = false }) {
	const kind = kindOf(name);
	const classes = [styles.icon, size == "row" ? styles.row : styles.tile, dim ? styles.dim : ""].filter(Boolean).join(" ");
	return (
		<span className={classes} data-family={familyOf(kind)} aria-hidden="true">
			<svg viewBox="0 0 32 40" className={styles.drawing}>
				<path className={styles.page} d="M5 0.75H20.5L31.25 11.5V37A2.25 2.25 0 0 1 29 39.25H5A2.25 2.25 0 0 1 2.75 37V3A2.25 2.25 0 0 1 5 0.75Z" />
				<path className={styles.fold} d="M20.5 0.75V9.25A2.25 2.25 0 0 0 22.75 11.5H31.25" />
			</svg>
			<span className={styles.label}>{labelOf(kind)}</span>
		</span>
	);
}
