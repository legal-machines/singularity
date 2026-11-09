// The pigments a project's mark may take, each by the name a Matter keeps it
// under: its appearance's markGround holds lower-case names only, and what a
// name means is the window's to decide.
export const pigments = { blue: "#2f56b0", teal: "#2a7d8c", green: "#2f8a6e", amber: "#c2851a", vermilion: "#c43a24", rose: "#b04a6e", violet: "#7a4b97", umber: "#8b6a4b" };

export function pigmentName(color) {
	return Object.keys(pigments).find(name => pigments[name] == (color ?? "").toLowerCase()) ?? "";
}
