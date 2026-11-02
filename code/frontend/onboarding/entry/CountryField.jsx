import { memo, useCallback, useEffect, useId, useLayoutEffect, useMemo, useRef, useState } from "react";
import { COUNTRIES, countryNamed, findCountries } from "./countries.js";
import styles from "./CountryField.module.css";

// Hover and keyboard navigation change only the two affected rows, not the
// whole list. Names alone are shown; ISO codes still take part in search.
const CountryOption = memo(function CountryOption({ country, index, listId, active, onChoose }) {
	return (
		<li id={`${listId}-${country.code}`} data-index={index} role="option" aria-selected={active}
			className={styles.option} title={country.label}
			onMouseDown={(event) => { event.preventDefault(); onChoose(country); }}
			onClick={() => onChoose(country)}>
			<span className={styles.name}>{country.label}</span>
		</li>
	);
});

// Keep the active option inside this scroll container. scrollIntoView can
// also move the page behind a popup; pointer navigation must never scroll.
function revealOption(list, index) {
	const option = list?.children[index];
	if (!option || list.clientHeight === 0) return;
	const top = option.offsetTop;
	const bottom = top + option.offsetHeight;
	if (top < list.scrollTop) list.scrollTop = top;
	else if (bottom > list.scrollTop + list.clientHeight) list.scrollTop = bottom - list.clientHeight;
}

export function CountryField({ className, id, label, value, onChange, inputRef, invalid, busy, describedBy }) {
	const listId = useId();
	const list = useRef(null);
	const container = useRef(null);
	const popupInteraction = useRef(false);
	const closeLatest = useRef(null);
	const pointer = useRef(null);
	const composing = useRef(false);
	const [open, setOpen] = useState(false);
	// null means the complete list rather than a filtered query.
	const [typed, setTyped] = useState(null);
	const [cursor, setCursor] = useState({ index: 0, reveal: true });
	const matches = useMemo(() => (typed === null ? COUNTRIES : findCountries(typed)), [typed]);
	const expanded = open && !busy;

	useLayoutEffect(() => {
		if (expanded && cursor.reveal) revealOption(list.current, cursor.index);
	}, [cursor, expanded, matches]);

	const show = () => {
		if (busy) return;
		const chosen = countryNamed(value);
		setTyped(null);
		setCursor({ index: chosen ? Math.max(0, COUNTRIES.indexOf(chosen)) : 0, reveal: true });
		setOpen(true);
	};
	const choose = useCallback((country) => {
		if (busy) return;
		onChange(country.label);
		setTyped(null);
		setOpen(false);
	}, [busy, onChange]);
	const filter = (text) => {
		setTyped(text);
		setCursor({ index: 0, reveal: true });
		setOpen(true);
	};
	const onKeyDown = (event) => {
		if (busy || composing.current || event.nativeEvent.isComposing || event.keyCode === 229) return;
		if (event.key === "Escape" && expanded || event.altKey && event.key === "ArrowUp" && expanded) {
			event.preventDefault();
			event.stopPropagation();
			setOpen(false);
			return;
		}
		if (event.key === "Enter" && expanded) {
			event.preventDefault();
			if (matches[cursor.index]) choose(matches[cursor.index]);
			return;
		}
		const arrow = event.key === "ArrowDown" || event.key === "ArrowUp";
		const edge = expanded && !event.ctrlKey && !event.metaKey && !event.shiftKey && (event.key === "Home" || event.key === "End");
		const page = expanded && (event.key === "PageDown" || event.key === "PageUp");
		if (!arrow && !edge && !page) return;
		event.preventDefault();
		if (!expanded) return show();
		const pageSize = Math.max(1, Math.floor((list.current?.clientHeight || 156) / (list.current?.children[0]?.offsetHeight || 36)) - 1);
		setCursor(({ index }) => ({
			index: event.key === "Home" ? 0 : event.key === "End" ? Math.max(0, matches.length - 1)
				: Math.max(0, Math.min(matches.length - 1, index + (event.key.endsWith("Down") ? 1 : -1) * (page ? pageSize : 1))),
			reveal: true,
		}));
	};
	const onPointerMove = (event) => {
		if (event.pointerType === "touch") return;
		// Scrolling under a stationary pointer must not undo keyboard navigation.
		if (pointer.current?.x === event.clientX && pointer.current?.y === event.clientY) return;
		pointer.current = { x: event.clientX, y: event.clientY };
		const option = event.target.closest?.('[role="option"]');
		if (!option) return;
		const index = Number(option.dataset.index);
		setCursor((current) => current.index === index ? current : { index, reveal: false });
	};
	const close = () => {
		const named = countryNamed(value) ?? (typed?.trim() ? matches[cursor.index] : null);
		setOpen(false);
		setTyped(null);
		if (!busy && named && named.label !== value) onChange(named.label);
	};
	useLayoutEffect(() => { closeLatest.current = close; });
	useEffect(() => {
		if (!expanded) { popupInteraction.current = false; return undefined; }
		const insidePopup = (event) => {
			const popup = list.current;
			if (!popup) return false;
			if (popup.contains(event.target)) return true;
			// Native scrollbar tracks may report their host rather than a row.
			const box = popup.getBoundingClientRect();
			return event.clientX >= box.left && event.clientX < box.right && event.clientY >= box.top && event.clientY < box.bottom;
		};
		const down = (event) => {
			popupInteraction.current = insidePopup(event);
			if (!popupInteraction.current && !container.current?.contains(event.target)) closeLatest.current();
		};
		const up = (event) => {
			if (popupInteraction.current || insidePopup(event)) inputRef.current?.focus({ preventScroll: true });
			popupInteraction.current = false;
		};
		const focus = (event) => {
			if (event.target !== document.body && !popupInteraction.current && !container.current?.contains(event.target)) closeLatest.current();
		};
		document.addEventListener("mousedown", down, true);
		document.addEventListener("mouseup", up, true);
		document.addEventListener("focusin", focus);
		return () => {
			popupInteraction.current = false;
			document.removeEventListener("mousedown", down, true);
			document.removeEventListener("mouseup", up, true);
			document.removeEventListener("focusin", focus);
		};
	}, [expanded, inputRef]);
	const activeId = expanded && matches[cursor.index] ? `${listId}-${matches[cursor.index].code}` : undefined;

	return (
		<div ref={container} className={`${className} ${styles.combo}`}>
			<label htmlFor={id}>{label}</label>
			<input ref={inputRef} id={id} name="country" type="text" value={value} maxLength={120}
				readOnly={busy} autoComplete="off" autoCapitalize="words" spellCheck={false}
				role="combobox" aria-autocomplete="list" aria-expanded={expanded} aria-controls={expanded ? listId : undefined}
				aria-activedescendant={activeId} aria-invalid={invalid} aria-describedby={describedBy}
				onFocus={() => !expanded && show()} onClick={() => !expanded && show()}
				onBlur={(event) => { if (!popupInteraction.current && event.relatedTarget !== null) close(); }} onKeyDown={onKeyDown}
				onCompositionStart={() => { composing.current = true; }}
				onCompositionEnd={(event) => { composing.current = false; filter(event.currentTarget.value); }}
				onChange={(event) => {
					onChange(event.target.value);
					if (!composing.current && !event.nativeEvent.isComposing) filter(event.target.value);
				}} />
			{expanded && (
				<ul ref={list} id={listId} role="listbox" aria-label={label} className={styles.list} onPointerMove={onPointerMove}>
					{matches.length === 0 && <li className={styles.none} role="presentation">No country or territory by that name.</li>}
					{matches.map((country, index) => <CountryOption key={country.code} country={country} index={index}
						listId={listId} active={index === cursor.index} onChoose={choose} />)}
				</ul>
			)}
		</div>
	);
}
