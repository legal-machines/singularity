import { setMatterArchived } from "../../data/native.js";
import { toast } from "../Toast/toast.js";

import { ConfirmDialog } from "./Manage.jsx";

// A Matter is archived once the member confirms it (the owner, 2026-11-02:
// "if I want to archive a Matter, ask for confirmation"): it leaves the open
// Matters for everyone on it (D-214), and Undo brings it back.
export function ArchiveMatterDialog({ matter, onClose, onArchived }) {
	return (
		<ConfirmDialog
			title={`Archive ${matter.name}?`}
			description="It leaves the open Matters for everyone on it. Nothing in it is lost, and anyone on it can restore it."
			confirm="Archive"
			onClose={onClose}
			onConfirm={async () => {
				await setMatterArchived(matter.id, true);
				onArchived?.();
				toast(`${matter.name} is archived. It stands apart from your open Matters.`, { action: { label: "Undo", run: () => setMatterArchived(matter.id, false) } });
			}} />
	);
}
