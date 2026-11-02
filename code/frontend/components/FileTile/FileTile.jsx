import { FileKindIcon } from "../FileKind/FileKind.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { toast } from "../Toast/toast.js";

import styles from "./FileTile.module.css";

// What a file's kind is called lives with its drawing (FileKind.jsx).
export { kindOf, wordOf } from "../FileKind/FileKind.jsx";

// A file as lawyers send one another (the owner, 2026-10-24: "a legal result
// is usually a file"): a page with its type on it, the file's own name, under
// it one line in words of what is inside ("9 pages with tracked changes"),
// and its size alone on the right, as a file list keeps it. Nothing is joined
// by a bullet dot (D-057). A press opens the file, in the app where it can or
// in its own program; under the pointer the size gives its place to the
// download. One a later version replaced stands back. In a reply being
// written, `onRemove` takes the file off it, and its way to do so shows at
// all times.
export function FileTile({ name, line, size, isOld = false, onOpen, onRemove, onDownload, downloadLabel = "Download", downloadIcon = "download" }) {
	const isDraft = onRemove != null;
	const classes = [styles.tile, isOld ? styles.tile_old : "", isDraft ? styles.tile_draft : ""].filter(Boolean).join(" ");
	return (
		<div className={classes}>
			<button className={styles.open} type="button" onClick={onOpen ?? (() => toast(`${name} opens in its own program.`))}>
				<FileKindIcon name={name} />
				<span className={styles.text}>
					<span className={styles.name}>{name}</span>
					{line != null ? <span className={styles.line}>{line}</span> : null}
				</span>
				<span className={styles.end}>
					{size != null ? <span className={styles.size}>{size}</span> : null}
				</span>
			</button>
			{isDraft ? (
				<button className={`${styles.act} ${styles.remove}`} type="button" aria-label={`Take ${name} off`} data-tooltip="Take it off" onClick={onRemove}>
					<Icon name="close" size={16} />
				</button>
			) : (
				<button className={`${styles.act} ${styles.download}`} type="button" aria-label={`${downloadLabel}: ${name}`} data-tooltip={downloadLabel} onClick={onDownload ?? (() => toast(`Saved ${name} to Downloads.`))}>
					<Icon name={downloadIcon} size={18} />
				</button>
			)}
		</div>
	);
}
