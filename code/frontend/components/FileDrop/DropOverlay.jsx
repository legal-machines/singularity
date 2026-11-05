import { Icon } from "../Icon/Icon.jsx";

import styles from "./DropOverlay.module.css";

// Over a place that takes files while they are dragged over it: it says what
// it does with them, moves nothing, and goes once they are let go (as a
// personal chat says it, MessagesPage.module.css).
export function DropOverlay({ words }) {
	return (
		<div className={styles.drop} aria-hidden="true">
			<Icon name="attach" size={24} />
			<p>{words}</p>
		</div>
	);
}
