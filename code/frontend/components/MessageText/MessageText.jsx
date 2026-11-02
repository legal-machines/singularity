import { Fragment } from "react";

import { MentionedWords } from "../Mentions/Mentions.jsx";
import { isSafeAddress, parseBlocks, parseInline } from "./markdown.js";

import styles from "./MessageText.module.css";

// A message's words as their markdown means them, in a thread's post or a
// chat's message, whoever wrote them, a person or an AI colleague (the owner,
// 2026-11-02: "support all md tags, but the md itself is not shown"): bold,
// italic, struck, code, links, headings, quotes, lists, code blocks, rules and
// tables are drawn as what they are, and their marks never show. @mentions
// stand set apart as everywhere (Mentions). A plain list stands one line under
// another with no mark before each (D-057: nothing is set off by a bullet
// dot); a numbered one keeps its numbers. Every message body goes through
// here, so a place that shows it otherwise - such as how an outside provider
// sees it - wraps this one component.
export function MessageText({ text, className = "" }) {
	const blocks = parseBlocks(text);
	return (
		<div className={`${styles.text} ${className}`}>
			{blocks.map((block, index) => <Block key={index} block={block} />)}
		</div>
	);
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
		return (
			<div className={styles.tableScroll}>
				<table className={styles.table}>
					<thead>
						<tr>{block.head.map((cell, index) => <th key={index} style={{ textAlign: block.align[index] ?? undefined }}><Inline pieces={parseInline(cell)} /></th>)}</tr>
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
		if (piece.type == "text") return <MentionedWords key={index} text={piece.text} />;
		if (piece.type == "code") return <code key={index} className={styles.code}>{piece.text}</code>;
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
