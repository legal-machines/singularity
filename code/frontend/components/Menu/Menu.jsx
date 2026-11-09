import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

import { placeNear } from "../Floating/position.js";
import { Icon } from "../Icon/Icon.jsx";

import styles from "./Menu.module.css";

// A menu, as a desktop menu behaves: it opens beside what opened it, itself
// in focus and no item lit until the pointer or a key reaches one; Up and
// Down move, Home and End jump, a letter finds the item that starts with it,
// Enter or Space chooses, Right opens an item's submenu and Left closes it,
// Escape closes the menu and gives the focus back. A press outside closes
// it. Each item may carry an icon, a hint on its right (a date, a code), a
// check, or a submenu; `heading` and `separator` items group the rest. A
// group runs on at the sidebar's pitch, with no line and no gap, and no item
// shows a keyboard shortcut. With `matchAnchorWidth` it is exactly as wide
// as what opened it, edge to edge, as the account's menu is the member's
// row's.
export function Menu({ anchor, items, label, placement = "bottom-start", minWidth = 200, matchAnchorWidth = false, onClose, isSubmenu = false }) {
	const menuRef = useRef(null);
	const [place, setPlace] = useState(null);
	const [openSub, setOpenSub] = useState(null);
	const typed = useRef({ text: "", at: 0 });

	useLayoutEffect(() => {
		if (anchor == null || menuRef.current == null) return;
		const rect = menuRef.current.getBoundingClientRect();
		setPlace(placeNear(anchor.getBoundingClientRect(), { width: rect.width, height: rect.height }, { placement, gap: isSubmenu ? 2 : 6 }));
	}, [anchor, placement, isSubmenu]);

	useEffect(() => {
		if (place == null) return;
		menuRef.current?.focus({ preventScroll: true });
	}, [place]);

	useEffect(() => {
		if (isSubmenu) return undefined;
		function outside(event) {
			if (event.target.closest?.("[data-menu-layer]") != null) return;
			if (anchor?.contains(event.target)) return;
			onClose();
		}
		document.addEventListener("pointerdown", outside, true);
		return () => document.removeEventListener("pointerdown", outside, true);
	}, [anchor, onClose, isSubmenu]);

	function onKeyDown(event) {
		const list = itemsOf(menuRef.current);
		const index = list.indexOf(document.activeElement);
		const move = next => {
			event.preventDefault();
			list[(next + list.length) % list.length]?.focus();
		};

		if (event.key == "ArrowDown") move(index + 1);
		else if (event.key == "ArrowUp") move(index < 0 ? list.length - 1 : index - 1);
		else if (event.key == "Home") move(0);
		else if (event.key == "End") move(list.length - 1);
		else if (event.key == "Escape" || (isSubmenu && event.key == "ArrowLeft")) {
			event.preventDefault();
			event.stopPropagation();
			onClose({ returnFocus: true });
		} else if (event.key == "Tab") {
			event.preventDefault();
			onClose({ returnFocus: true });
		} else if (event.key.length == 1 && /\S/.test(event.key)) {
			const now = Date.now();
			typed.current = { text: (now - typed.current.at < 600 ? typed.current.text : "") + event.key.toLowerCase(), at: now };
			const found = list.find(item => item.textContent.trim().toLowerCase().startsWith(typed.current.text));
			if (found != null) {
				event.preventDefault();
				found.focus();
			}
		}
	}

	return createPortal(
		<div
			ref={menuRef}
			className={place == null ? styles.menu : `${styles.menu} ${styles.shown}`}
			style={{ top: place?.top ?? -9999, left: place?.left ?? -9999, minWidth, ...(matchAnchorWidth && anchor != null ? { width: anchor.getBoundingClientRect().width, minWidth: 0, maxWidth: "none" } : {}) }}
			role="menu"
			aria-label={label}
			tabIndex={-1}
			data-menu-layer=""
			onKeyDown={onKeyDown}
		>
			{items.map((item, index) => {
				if (item == null || item == false) return null;
				if (item.separator) return null;
				if (item.heading != null) return <div key={`heading-${index}`} className={styles.heading} role="presentation">{item.heading}</div>;
				// A fact about what the menu is for, not an act: when a message was
				// sent and when it reached the other. Read, never chosen.
				if (item.info != null) {
					return (
						<div key={item.key ?? `info-${index}`} className={`${styles.item} ${styles.info}`} role="none">
							<span className={styles.lead}>{item.icon != null ? <Icon name={item.icon} size={18} /> : null}</span>
							<span className={styles.text}><span className={styles.label}>{item.info}</span></span>
							{item.hint != null ? <span className={styles.hint}>{item.hint}</span> : null}
						</div>
					);
				}
				return (
					<MenuItem
						key={item.key ?? item.label}
						item={item}
						isSubOpen={openSub == (item.key ?? item.label)}
						onOpenSub={() => setOpenSub(item.key ?? item.label)}
						onCloseSub={() => setOpenSub(null)}
						onHoverPlain={() => setOpenSub(null)}
						onChosen={() => onClose({ returnFocus: true, chosen: true })}
					/>
				);
			})}
		</div>,
		document.body,
	);
}

function MenuItem({ item, isSubOpen, onOpenSub, onCloseSub, onHoverPlain, onChosen }) {
	const itemRef = useRef(null);
	const hasSub = Array.isArray(item.submenu);
	const role = item.checked != null ? "menuitemradio" : "menuitem";

	function choose() {
		if (item.disabled) return;
		if (hasSub) {
			onOpenSub();
			return;
		}
		// An item that answers in place - this computer's code shown where its
		// name was - leaves the menu open.
		if (item.keepsOpen != true) onChosen();
		item.onSelect?.();
	}

	function onKeyDown(event) {
		if (hasSub && (event.key == "ArrowRight" || event.key == "Enter" || event.key == " ")) {
			event.preventDefault();
			event.stopPropagation();
			onOpenSub();
		} else if (!hasSub && (event.key == "Enter" || event.key == " ")) {
			event.preventDefault();
			choose();
		}
	}

	const classes = [styles.item, item.tone == "seal" ? styles.item_seal : null, isSubOpen ? styles.item_open : null].filter(Boolean).join(" ");

	return (
		<>
			<button
				ref={itemRef}
				className={classes}
				type="button"
				role={role}
				aria-checked={item.checked != null ? item.checked : undefined}
				aria-haspopup={hasSub ? "menu" : undefined}
				aria-expanded={hasSub ? isSubOpen : undefined}
				aria-disabled={item.disabled || undefined}
				tabIndex={-1}
				onClick={choose}
				onKeyDown={onKeyDown}
				onPointerEnter={() => {
					itemRef.current?.focus();
					if (hasSub) onOpenSub();
					else onHoverPlain();
				}}
			>
				<span className={styles.lead}>
					{item.checked == true ? <Icon name="check" size={16} /> : item.icon != null ? <Icon name={item.icon} size={18} /> : null}
				</span>
				<span className={styles.text}>
					<span className={item.isCode ? `${styles.label} ${styles.label_code}` : styles.label}>{item.label}</span>
					{item.description != null ? <span className={styles.description}>{item.description}</span> : null}
				</span>
				{item.hint != null ? <span className={styles.hint}>{item.hint}</span> : null}
				{hasSub ? <span className={styles.more}><Icon name="chevron_right" size={16} /></span> : null}
			</button>
			{hasSub && isSubOpen ? (
				<Menu
					anchor={itemRef.current}
					items={item.submenu.map(child => ({ ...child, onSelect: () => { child.onSelect?.(); onChosen(); } }))}
					label={item.label}
					placement="right-start"
					minWidth={180}
					isSubmenu={true}
					onClose={({ returnFocus } = {}) => {
						onCloseSub();
						if (returnFocus) itemRef.current?.focus();
					}}
				/>
			) : null}
		</>
	);
}

function itemsOf(menu) {
	if (menu == null) return [];
	return [...menu.querySelectorAll(":scope > [role^='menuitem']")].filter(item => item.getAttribute("aria-disabled") != "true");
}

// A trigger and its menu: the trigger says the menu is open while it is, and
// gets the focus back when it closes.
export function useMenu() {
	const [anchor, setAnchor] = useState(null);

	return {
		isOpen: anchor != null,
		anchor,
		open: element => setAnchor(element),
		toggle: element => setAnchor(current => (current == null ? element : null)),
		close: ({ returnFocus } = {}) => {
			setAnchor(current => {
				if (returnFocus) current?.focus();
				return null;
			});
		},
	};
}
