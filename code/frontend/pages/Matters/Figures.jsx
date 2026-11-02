import { Icon } from "../../components/Icon/Icon.jsx";

import styles from "./Figures.module.css";

// What a Matter holds and what its work came to, each a small sign and its
// figure, its words on hover (D-222): on the Matters page beside its name,
// and under its name on its own page.
export function Figures({ figures }) {
	if (figures.length == 0) return null;
	return (
		<span className={styles.figures}>
			{figures.map(figure => (
				<span key={figure.key} className={styles.figure} data-tooltip={figure.words} aria-label={figure.words}>
					<Icon name={figure.icon} size={15} />
					{figure.text}
				</span>
			))}
		</span>
	);
}
