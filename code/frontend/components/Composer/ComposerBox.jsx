import { forwardRef, useId, useMemo, useRef, useState } from "react";

import { firstName, positionOf } from "../../pages/words.js";
import { TextArea } from "../Field/Field.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { Popover } from "../Popover/Popover.jsx";
import { formatMarkOf, isInCodeBlock, toggledMark } from "./formatting.js";
import { MentionList } from "./MentionList.jsx";
import { mentionAt, mentionOptions, withMention } from "./mentions.js";

import styles from "./ComposerBox.module.css";

// The box a member writes in, one for every place it writes, a thread, a
// chat or a letter, drawn as Codex draws its own (the owner, 2026-10-24: "too
// small, copy it from here"). Two calm rows on one grid: two lines to write
// in, 18px in from the frame; under them a row of round tools 10px from the
// edges, what the place adds on the left (the "+", a chat's filing) and the
// send on the right. A press anywhere in the box that is not a tool puts the
// cursor in it. The send stays quiet until there is something to send, and
// Ctrl+Enter (Command-Enter on a Mac) sends; where `sendsOnEnter`, as in a
// messenger, Enter sends and Shift+Enter starts a new line - and inside a code
// block begun with ``` Enter starts a new line too. Command-B, -I and -E
// (Ctrl elsewhere) and Command-Shift-X put bold, italic, code and strike
// marks around the selection, or take them away (formatting.js); the words
// stay plain here and every message draws them (MessageText). `submit` puts
// another act in the send's place, such as closing a thread. Where it is given
// `mentionables`, an @ opens the list of whom the message can name
// (MentionList), with "all" first where `allowsAll`.
// The emoji a message is most often given, put in where the caret stands.
const EMOJI = ["🙂", "😊", "😂", "😉", "🙏", "👍", "👏", "🎉", "✅", "👀", "🤝", "💡", "🔥", "⏰", "📎", "❗", "❓", "❤️"];

// The box a member writes in, as Twist draws its own (the owner, 2026-11-02:
// "make the input field like this"): the words on top, and under them, at the
// left, the box's tools - its own, attaching where the page gives it, and
// emoji - and at the right what to do with the words: put them away, or send
// them. `above` stands in the box over the words: the files a message hands
// over, as Codex shows them.
export const ComposerBox = forwardRef(function ComposerBox({ value, onChange, onSubmit, canSend, placeholder, label, tools, submit, above = null, sendsOnEnter = false, mentionables = [], allowsAll = false, allLine = "", onDiscard }, ref) {
	const own = useRef(null);
	const field = ref ?? own;
	const boxRef = useRef(null);
	const listId = useId();
	const [mention, setMention] = useState(null);
	const [active, setActive] = useState(0);
	const [dismissedAt, setDismissedAt] = useState(null);
	// While a name just chosen goes into the words: the key's own release
	// would read the words as they were and open the list again.
	const isPlacing = useRef(false);
	const [emojiAt, setEmojiAt] = useState(null);
	const options = useMemo(() => mention == null ? [] : mentionOptions(mention.query, mentionables, { allowsAll, allLine, firstNameOf: firstName, lineOf: positionOf }), [mention, mentionables, allowsAll, allLine]);
	const isListed = mention != null && options.length > 0 && dismissedAt != mention.start;

	// What the caret stands after, once the words or the caret moved.
	function look(area) {
		if (mentionables.length == 0 && allowsAll == false) return;
		if (isPlacing.current == true) return;
		const next = mentionAt(area.value, area.selectionStart, area.selectionEnd);
		if (next == null) {
			setMention(null);
			setDismissedAt(null);
			return;
		}
		if (mention == null || mention.start != next.start || mention.query != next.query) setActive(0);
		setMention(next);
		if (next.start != dismissedAt) setDismissedAt(null);
	}

	function pick(option) {
		const area = field.current;
		if (area == null || mention == null) return;
		const next = withMention(value, mention, area.selectionStart, option.word);
		isPlacing.current = true;
		onChange(next.text);
		setMention(null);
		requestAnimationFrame(() => {
			area.focus();
			area.setSelectionRange(next.caret, next.caret);
			isPlacing.current = false;
		});
	}

	// An emoji where the caret stands, the caret after it.
	function put(emoji) {
		const area = field.current;
		const start = area?.selectionStart ?? value.length;
		const end = area?.selectionEnd ?? value.length;
		onChange(value.slice(0, start) + emoji + value.slice(end));
		setEmojiAt(null);
		requestAnimationFrame(() => {
			area?.focus();
			area?.setSelectionRange(start + emoji.length, start + emoji.length);
		});
	}

	// Marks around the selection, through the editor itself, so Undo takes
	// them back in one step.
	function format(mark) {
		const area = field.current;
		if (area == null) return;
		const change = toggledMark(area.value, area.selectionStart, area.selectionEnd, mark);
		area.focus();
		area.setSelectionRange(change.from, change.to);
		const isTyped = typeof document.execCommand == "function" && document.execCommand("insertText", false, change.insert) == true;
		if (isTyped == false) onChange(area.value.slice(0, change.from) + change.insert + area.value.slice(change.to));
		requestAnimationFrame(() => area.setSelectionRange(change.start, change.end));
	}

	function discard() {
		if (onDiscard) onDiscard();
		else onChange("");
		field.current?.focus();
	}

	return (
		<div
			ref={boxRef}
			className={styles.box}
			onMouseDown={event => {
				if (event.target == field.current || event.target.closest("button, select, input, label, a, [role='menu']") != null) return;
				event.preventDefault();
				field.current?.focus();
			}}
		>
			{above}
			<TextArea
				ref={field}
				className={styles.field}
				minRows={2}
				maxRows={12}
				placeholder={placeholder}
				aria-label={label}
				value={value}
				aria-autocomplete={mentionables.length > 0 || allowsAll ? "list" : undefined}
				aria-expanded={isListed}
				aria-controls={isListed ? listId : undefined}
				aria-activedescendant={isListed ? `${listId}-${active}` : undefined}
				onChange={event => {
					onChange(event.target.value);
					look(event.target);
				}}
				onSelect={event => look(event.target)}
				onBlur={() => setMention(null)}
				onKeyDown={event => {
					if (isListed == true && event.nativeEvent.isComposing != true) {
						if (event.key == "ArrowDown" || event.key == "ArrowUp") {
							event.preventDefault();
							const step = event.key == "ArrowDown" ? 1 : -1;
							setActive(current => (current + step + options.length) % options.length);
							return;
						}
						if (event.key == "Enter" || event.key == "Tab") {
							event.preventDefault();
							pick(options[Math.min(active, options.length - 1)]);
							return;
						}
						if (event.key == "Escape") {
							event.preventDefault();
							event.stopPropagation();
							setDismissedAt(mention.start);
							return;
						}
					}
					const mark = formatMarkOf(event, isMac());
					if (mark != null) {
						event.preventDefault();
						format(mark);
						return;
					}
					if (event.key != "Enter" || event.nativeEvent.isComposing == true) return;
					const isCommand = event.ctrlKey || event.metaKey;
					const area = event.currentTarget;
					const isSend = isCommand
						|| (sendsOnEnter && event.shiftKey == false && isInCodeBlock(area.value, area.selectionStart) == false);
					if (isSend == false) return;
					event.preventDefault();
					if (canSend) onSubmit();
				}}
			/>
			{isListed ? <MentionList id={listId} anchor={boxRef.current} options={options} active={Math.min(active, options.length - 1)} onPick={pick} onPoint={setActive} /> : null}
			<div className={styles.tools}>
				<div className={styles.tools_start}>
					{tools}
					<button className={styles.tool} type="button" aria-label="Emoji" data-tooltip="Emoji" aria-pressed={emojiAt != null}
						onClick={event => setEmojiAt(current => current == null ? event.currentTarget : null)}>
						<Icon name="smile" size={18} />
					</button>
				</div>
				<div className={styles.tools_end}>
					{value != "" ? (
						<button className={styles.tool} type="button" aria-label="Discard" data-tooltip="Discard" onClick={discard}>
							<Icon name="trash" size={18} />
						</button>
					) : null}
					{submit ?? (
						<button className={styles.send} type="button" disabled={!canSend} aria-label="Send" data-tooltip="Send" onClick={onSubmit}>
							<Icon name="send" size={18} />
						</button>
					)}
				</div>
			</div>
			{emojiAt != null ? (
				<Popover anchor={emojiAt} onClose={() => setEmojiAt(null)} placement="top-start" width={232} label="Emoji">
					<div className={styles.emoji}>
						{EMOJI.map(emoji => (
							<button key={emoji} className={styles.emoji_pick} type="button" aria-label={emoji} onClick={() => put(emoji)}>{emoji}</button>
						))}
					</div>
				</Popover>
			) : null}
		</div>
	);
});

function isMac() {
	return document.documentElement.dataset.nativePlatform == "darwin" || navigator.platform.startsWith("Mac");
}

// An act in the send's place, named in words: "Close thread".
export function ComposerAction({ children, disabled, tooltip, onClick }) {
	return (
		<button className={styles.action} type="button" disabled={disabled} data-tooltip={tooltip} onClick={onClick}>
			{children}
		</button>
	);
}
