import styles from "./Views.module.css";

// Which of a list's members, conversations or items a page shows, as Inbox
// chooses its own: a quiet track with the chosen one raised in white, no
// frame; the arrows, Home and End move along it. `stretch` spreads it over
// its column, its choices sharing the width.
export function Views({ label, views, view, onChange, controls, stretch = false }) {
	return (
		<div className={stretch ? `${styles.track} ${styles.stretch}` : styles.track} role="tablist" aria-label={label}>
			{views.map((item, index) => (
				<button
					key={item.id}
					type="button"
					role="tab"
					aria-selected={view == item.id}
					aria-controls={controls}
					tabIndex={view == item.id ? 0 : -1}
					onClick={() => onChange(item.id)}
					onKeyDown={event => {
						const next = event.key == "ArrowRight" ? (index + 1) % views.length : event.key == "ArrowLeft" ? (index + views.length - 1) % views.length
							: event.key == "Home" ? 0 : event.key == "End" ? views.length - 1 : null;
						if (next == null) return;
						event.preventDefault();
						onChange(views[next].id);
						event.currentTarget.parentElement.children[next].focus();
					}}
				>{item.label}</button>
			))}
		</div>
	);
}
