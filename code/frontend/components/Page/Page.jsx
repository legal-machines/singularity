import { createContext, useContext, useRef, useState } from "react";
import { createPortal } from "react-dom";

import { MovingHead } from "../MovingHead/MovingHead.jsx";
import { isEmulation } from "../../emulation/emulation.js";

import { useStableGutter } from "./stableGutter.js";

import styles from "./Page.module.css";

// What a page tells its head: its scrolling area, and the layer over the page
// that the head's title travels in.
const PageContext = createContext(null);

// A place's page: its band, then its content, which scrolls (DESIGN.md,
// FOLIO-12). The content keeps the page's gutters and, on a very wide
// window, a width a reader can follow. Its head's title travels up into the
// band as it scrolls (PageHead, MovingHead).
export function Page({ bar, children, wide = false, asInbox = false, hardEdge = false, className }) {
	const scrollRef = useRef(null);
	useStableGutter(scrollRef);
	const [host, setHost] = useState(null);

	return (
		<PageContext.Provider value={{ scrollRef, host }}>
			<div className={styles.page} data-print="flow" data-demonstration={isEmulation || undefined}>
				{bar}
				<div ref={scrollRef} className={styles.scroll} data-print="flow" data-fade-edge={isEmulation || hardEdge ? undefined : ""}>
					<div className={[styles.content, wide ? styles.content_wide : null, asInbox ? styles.content_inbox : null, className].filter(Boolean).join(" ")}>{children}</div>
				</div>
				<div ref={setHost} className={styles.head_host} data-print="hide" />
			</div>
		</PageContext.Provider>
	);
}

// A page's head: its title, one sentence under it, and its actions on the
// right, the leading one last. Its title travels into the window's band as
// the page scrolls (the owner, 2026-10-24: "not every page has a header like
// the thread's"); the rest scrolls away with the page.
export function PageHead({ lead, title, line, lineAfter, actions, asInbox = false, compact = false, carries = false, children }) {
	const page = useContext(PageContext);
	const headRef = useRef(null);
	const travels = page != null && !isEmulation;
	const Title = travels ? "div" : "h1";
	// The face and the buttons travel into the band with the title; their
	// room stays in the head.
	const carried = travels && carries;

	return (
		<header ref={headRef} className={[styles.head, asInbox ? styles.head_inbox : null, compact ? styles.head_compact : null].filter(Boolean).join(" ")}>
			{lead != null ? <span className={carried ? `${styles.head_lead} ${styles.title_room}` : styles.head_lead} data-part={carried ? "lead" : undefined} aria-hidden={carried ? "true" : undefined}>{lead}</span> : null}
			<div className={styles.head_text}>
				<Title className={styles.title}>
					<span className={travels ? styles.title_room : undefined} data-part="title" aria-hidden={travels ? "true" : undefined}>{title}</span>
					{children}
				</Title>
				{line != null ? (
					<p className={carried ? `${styles.line} ${styles.title_room}` : styles.line} data-part={carried ? "band" : undefined} aria-hidden={carried ? "true" : undefined}>{line}</p>
				) : null}
				{lineAfter != null ? <p className={styles.line}>{lineAfter}</p> : null}
			</div>
			{actions != null ? <div className={carried ? `${styles.actions} ${styles.title_room}` : styles.actions} data-part={carried ? "actions" : undefined} aria-hidden={carried ? "true" : undefined}>{actions}</div> : null}
			{travels && page.host != null ? createPortal(
				<MovingHead
					scrollRef={page.scrollRef}
					placeholderRef={headRef}
					title={title}
					lead={carried ? lead : undefined}
					band={carried && line != null ? <span className={styles.line_travelling}>{line}</span> : undefined}
					actions={carried ? actions : undefined}
					onTop={() => page.scrollRef.current?.scrollTo({ top: 0, behavior: "smooth" })}
				/>,
				page.host,
			) : null}
		</header>
	);
}
