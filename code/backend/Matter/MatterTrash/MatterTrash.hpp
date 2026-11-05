#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Matters deleted from this computer go to its Trash for 30 days first (the
// owner, 2026-11-04: "Matters can be cleared away, they go to the Trash and
// are kept there for 30 days"). A Matter in the Trash leaves every list here
// and comes back whole with Restore. After 30 days, or with Delete now, it is
// deleted from this computer as before (D-214): it stops travelling here and
// leaves the list; its folder and documents stay where they are, and the
// firm's other computers keep it.
//
// Kept beside the workspace's list of Matters, in
// .singularity-workspace/trashed-matters.json: this computer's own, never in
// a Matter's .lm, which travels to the firm's other computers.
namespace singularity::matter::trash {

inline constexpr int kKeptDays = 30;

struct Trashed {
	std::string id;
	// When it went to the Trash, and when it is deleted from it (ISO 8601, UTC).
	std::string trashed_at;
	std::string deletes_at;
};

std::vector<Trashed> list(const std::filesystem::path &workspace);

// Puts a Matter in the Trash as of `now`; one already there keeps its date.
Trashed put(const std::filesystem::path &workspace, const std::string &id, const std::string &now);

// Takes a Matter out of the Trash; whether it was there.
bool take_back(const std::filesystem::path &workspace, const std::string &id);

// The Matters whose 30 days have passed by `now`.
std::vector<std::string> expired(const std::filesystem::path &workspace, const std::string &now);

// `kKeptDays` after a moment, in the same form; empty for one unreadable.
std::string deletes_at(const std::string &trashed_at);

} // namespace singularity::matter::trash
