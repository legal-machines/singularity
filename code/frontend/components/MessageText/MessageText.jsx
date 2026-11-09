import { createContext, Fragment, useContext } from "react";

import { MentionedWords } from "../Mentions/Mentions.jsx";
import { sentSegments } from "../../data/masking.js";
import { isSafeAddress, parseBlocks, parseInline, plainOf } from "./markdown.js";
import { hostOf, useSiteIcon } from "./siteIcons.js";

import styles from "./MessageText.module.css";

// A message's words as their markdown means them, in a thread's post or a
// chat's message, whoever wrote them, a person or an AI colleague: bold,
// italic, struck, code, links, headings, quotes, lists, code blocks, rules and
// tables are drawn as what they are, and their marks never show. @mentions
// stand set apart as everywhere (Mentions). A plain list stands one line under
// another with no mark before each; a numbered one keeps its numbers. Every
// message body goes through here, so a place that shows it otherwise - such as
// how an outside provider sees it - wraps this one component. An AI
// colleague's words carry the web sources they cite: each citation stands
// where the words stand it, as the source's site, and opens the page.
//
// Read as an AI provider receives it, `symbols` are Papinian Guard's
// replacements for `text`, the masked words: the message reads as it always
// does, its markdown drawn, and each symbol stands set apart where the words
// it replaces stood.
export function MessageText({ text, className = "", sources = null, symbols = null }) {
	const blocks = parseBlocks(text);
	return (
		<SourcesContext.Provider value={sources ?? []}>
			<SymbolsContext.Provider value={symbols ?? []}>
				<div className={`${styles.text} ${className}`}>
					{blocks.map((block, index) => <Block key={index} block={block} />)}
				</div>
			</SymbolsContext.Provider>
		</SourcesContext.Provider>
	);
}

const SourcesContext = createContext([]);
const SymbolsContext = createContext([]);

// A run of the words: as written, or, read as a provider receives it, each
// symbol set apart, the words it stands for shown when pointed at.
function Words({ text }) {
	const symbols = useContext(SymbolsContext);
	if (symbols.length == 0) return <MentionedWords text={text} />;
	return sentSegments(text, symbols).map((segment, index) => segment.tag == null
		? <MentionedWords key={index} text={segment.text} />
		: <span key={index} className={styles.symbol} data-tooltip={segment.words != "" ? segment.words : undefined}>{segment.tag}</span>);
}

// A source's site as a reader names it: its address's host, without "www.".
export function siteOf(source) {
	const named = (source.site ?? "").replace(/^www\./, "");
	if (named != "") return named;
	try {
		return new URL(source.url).hostname.replace(/^www\./, "");
	} catch {
		return "";
	}
}

// One citation of the words: the first source's site, by its icon and its
// name, and how many more stand with it; it opens the first, and names
// them all when pointed at.
function Cited({ numbers }) {
	const sources = useContext(SourcesContext);
	const cited = numbers.map(number => sources.find(source => source.number == number)).filter(source => source != null && isSafeAddress(source.url ?? ""));
	if (cited.length == 0) return null;
	const first = cited[0];
	const named = cited.map(source => source.title || siteOf(source)).join("\n");
	return (
		<a className={styles.source} href={first.url} target="_blank" rel="noreferrer" data-tooltip={named} aria-label={`Source: ${named}`}>
			<SiteIcon source={first} />
			<span className={styles.sourceSite}>{siteOf(first) || "Source"}</span>
			{cited.length > 1 ? <span className={styles.sourceMore}>+{cited.length - 1}</span> : null}
		</a>
	);
}

// A source's site by its own icon, as the site shows it in a browser's
// tab; until it comes, or where the site has none, the site's first letter
// stands in its place, so nothing moves when it arrives.
function SiteIcon({ source }) {
	const icon = useSiteIcon(hostOf(source));
	if (typeof icon == "string") return <img className={styles.siteIcon} src={icon} alt="" draggable={false} />;
	return <span className={`${styles.siteIcon} ${styles.siteLetter}`} aria-hidden="true">{siteOf(source).slice(0, 1).toUpperCase()}</span>;
}

function Block({ block }) {
	if (block.type == "paragraph") {
		return (
			<p className={styles.paragraph}>
				{block.lines.map((line, index) => (
					<Fragment key={index}>
						{index > 0 ? <br /> : null}
						<Inline pieces={parseInline(line)} />
					</Fragment>
				))}
			</p>
		);
	}
	if (block.type == "heading") {
		return <p className={styles.heading} data-level={Math.min(block.level, 3)}><Inline pieces={parseInline(block.text)} /></p>;
	}
	if (block.type == "rule") return <hr className={styles.rule} />;
	if (block.type == "quote") {
		return <blockquote className={styles.quote}>{block.blocks.map((inner, index) => <Block key={index} block={inner} />)}</blockquote>;
	}
	if (block.type == "code") {
		return <pre className={styles.codeBlock}><code>{block.text}</code></pre>;
	}
	if (block.type == "list") return <List block={block} />;
	if (block.type == "table") {
		const widths = columnWidths(block);
		return (
			<div className={styles.tableScroll}>
				<table className={styles.table}>
					<thead>
						<tr>{block.head.map((cell, index) => <th key={index} style={{ textAlign: block.align[index] ?? undefined, minWidth: widths[index] }}><Inline pieces={parseInline(cell)} /></th>)}</tr>
					</thead>
					<tbody>
						{block.rows.map((row, rowIndex) => (
							<tr key={rowIndex}>{block.head.map((_, index) => <td key={index} style={{ textAlign: block.align[index] ?? undefined }}><Inline pieces={parseInline(row[index] ?? "")} /></td>)}</tr>
						))}
					</tbody>
				</table>
			</div>
		);
	}
	return null;
}

// The least width of each column, from how much its cells say: a column of
// a few words is as wide as its words; one of sentences is given room to
// read, so a table of many columns scrolls sideways rather than squeezing
// each into a strip.
export function columnWidths(block) {
	return block.head.map((head, index) => {
		const longest = Math.max(plainOf(head).length, ...block.rows.map(row => plainOf(row[index] ?? "").length));
		if (longest <= 14) return undefined;
		if (longest <= 40) return "7.5rem";
		if (longest <= 120) return "11rem";
		return "15rem";
	});
}

// Items stand at their depth; a numbered one says its number, a plain one
// nothing before it.
function List({ block }) {
	const Tag = block.ordered ? "ol" : "ul";
	let counter = block.start - 1;
	return (
		<Tag className={styles.list} data-ordered={block.ordered || undefined}>
			{block.items.map((item, index) => {
				if (item.ordered && item.depth == 0) counter = item.number ?? counter + 1;
				const lines = item.text.split("\n");
				return (
					<li key={index} className={styles.item} style={item.depth > 0 ? { marginLeft: `${item.depth * 18}px` } : undefined}>
						{item.ordered ? <span className={styles.number}>{item.depth == 0 ? counter : item.number}.</span> : null}
						<span className={styles.itemText}>
							{lines.map((line, lineIndex) => (
								<Fragment key={lineIndex}>
									{lineIndex > 0 ? <br /> : null}
									<Inline pieces={parseInline(line)} />
								</Fragment>
							))}
						</span>
					</li>
				);
			})}
		</Tag>
	);
}

function Inline({ pieces }) {
	return pieces.map((piece, index) => {
		if (piece.type == "text") return <Words key={index} text={piece.text} />;
		if (piece.type == "code") return <code key={index} className={styles.code}>{piece.text}</code>;
		if (piece.type == "cite") return <Cited key={index} numbers={piece.numbers} />;
		if (piece.type == "strong") return <strong key={index} className={styles.strong}><Inline pieces={piece.children} /></strong>;
		if (piece.type == "em") return <em key={index}><Inline pieces={piece.children} /></em>;
		if (piece.type == "strike") return <s key={index}><Inline pieces={piece.children} /></s>;
		if (piece.type == "link" && isSafeAddress(piece.href)) {
			return <a key={index} className={styles.link} href={piece.href} target="_blank" rel="noreferrer">{plainChildren(piece.children)}</a>;
		}
		return null;
	});
}

// A link's own words, with their emphasis but never a mention's button
// inside a link.
function plainChildren(pieces) {
	return pieces.map((piece, index) => {
		if (piece.type == "text") return <Fragment key={index}>{piece.text}</Fragment>;
		if (piece.type == "code") return <code key={index} className={styles.code}>{piece.text}</code>;
		const Tag = piece.type == "strong" ? "strong" : piece.type == "em" ? "em" : piece.type == "strike" ? "s" : Fragment;
		return <Tag key={index}>{plainChildren(piece.children ?? [])}</Tag>;
	});
}
