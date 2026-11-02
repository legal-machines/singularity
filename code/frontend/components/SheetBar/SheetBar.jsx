import styles from "./SheetBar.module.css";

// The sheet's top band (DESIGN.md, FOLIO-12): the window's title bar, which
// drags it where nothing can be pressed, and holds the actions of what is
// open, the leading one last. It draws no trail and no search (the owner,
// 2026-10-24: "remove Inbox", "remove the search"): the page's title says
// where the member is, and on a thread or a letter it travels up into the
// band as the page scrolls (MovingHead); the sidebar, Alt+Left, Alt+Right and
// a mouse's side buttons move between places (App), and Ctrl+K opens the
// search. A page still names its trail, for when the band says it again. On
// Windows the band leaves the corner to the window's buttons.
export function SheetBar({ actions, children, className, corner = true }) {
	return (
		<header className={[styles.bar, corner ? null : styles.bar_inner, className].filter(Boolean).join(" ")} data-print="hide">
			{children != null ? <div className={styles.middle}>{children}</div> : <span className={styles.space} />}
			{actions != null ? <div className={styles.actions}>{actions}</div> : null}
		</header>
	);
}
