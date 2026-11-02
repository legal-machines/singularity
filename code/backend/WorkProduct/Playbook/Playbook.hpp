#pragma once

#include "Matter/Types/Matter.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace singularity::playbooks {

/*
 * A firm's Playbook: its approved positions for one kind of contract - what it
 * wants, what it accepts instead, what it never accepts, and the clause it
 * proposes - kept as a reusable professional object, versioned, and used to
 * review a Matter's documents (WorkProduct/ReviewTable) and to prepare the
 * changes that bring them to the firm's position (Word tracked changes, put to
 * the lawyer for approval).
 *
 * A Playbook keeps four kinds of statement apart, and so does everything made
 * from it: the firm's policy (its positions), the law the firm cites for them
 * (its own citations - never one a model supplied), the facts of a document
 * (the words quoted from it, found there by Singularity) and the model's
 * assessment of how the two compare.
 *
 * Kept beside this computer's settings (`Playbooks/<id>/v<N>.json`), every
 * version kept; shared across the firm as a file (export, import). A Review
 * Table made from a Playbook keeps the positions as they stood, with the
 * Playbook's version.
 */

struct LawReference {
	std::string citation;
	std::string note;
};

struct Fallback {
	std::string position;
	// The clause the firm proposes for this fallback, when it has one.
	std::string clause;
};

struct Position {
	std::string id;
	std::string topic;
	// What a review asks of each document for this position.
	std::string question;
	std::string answer_kind = "text";
	// The firm's preferred position, and the clause it proposes for it.
	std::string standard;
	std::string standard_clause;
	// Acceptable instead, in the firm's order of preference.
	std::vector<Fallback> fallbacks;
	// What the firm never accepts.
	std::string red_line;
	// How much a departure matters: "high", "medium" or "low".
	std::string severity = "medium";
	std::vector<LawReference> law;
	std::string guidance;
};

struct Playbook {
	std::string id;
	std::string name;
	std::string description;
	std::string contract_type;
	int version = 0;
	std::string updated_at;
	std::string updated_by_actor_id;
	std::vector<Position> positions;
};

nlohmann::json to_json(const Playbook &playbook);
Playbook from_json(const nlohmann::json &value);

std::vector<Playbook> list();
// The Playbook as it stands now, or as it stood at `version`.
std::optional<Playbook> get(const std::string &id, std::optional<int> version = std::nullopt);
// Keeps `playbook` as a new version (the first, when it has no id yet) and
// returns it as kept. Positions without an id are given one.
Playbook save(Playbook playbook);
bool remove(const std::string &id);
// A Playbook from a file another member of the firm exported: kept as a new
// Playbook of this computer, its own versions starting again.
Playbook import_json(const std::string &text);

// The firm's position for `position`, as a Review Table column keeps it: the
// policy alone, and the law the firm cites for it, apart.
std::string position_text(const Position &position);
std::string law_text(const Position &position);

// A Review Table of `document_ids` with a question per position of the
// Playbook, each column keeping the position and the law as they stand now.
matters::ReviewTable review_table(const std::string &project_id, const std::string &playbook_id,
	const std::vector<std::string> &document_ids, const std::string &title = std::string(), bool local_only = false);

} // namespace singularity::playbooks
