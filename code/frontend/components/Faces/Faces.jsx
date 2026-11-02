import { Avatar } from "../Avatar/Avatar.jsx";
import { PersonButton } from "../PersonCard/PersonCard.jsx";

import styles from "./Faces.module.css";

// Who takes part, as a row of faces that overlap a little, each set apart by
// a ring of the page's white in its own G2 shape (the owner, 2026-10-24: "the
// participants' avatars need separating somehow"). A face names its member
// under the pointer, and a press on it opens the member's card.
export function Faces({ people, size = 22 }) {
	return (
		<span className={styles.faces}>
			{people.map(member => (
				<span key={member.id} className={styles.face} data-tooltip={member.name}>
					<PersonButton memberId={member.id} label={member.name}>
						<Avatar member={member} size={size} />
					</PersonButton>
				</span>
			))}
		</span>
	);
}
