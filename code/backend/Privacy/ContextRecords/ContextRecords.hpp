#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Privacy/SentPassages/SentPassages.hpp"

// What the Project remembers goes into a request one record a line - a data
// room change, a Context item - and Routing/RoutingLog keeps each line that
// left with a marker in it as it left, beside only a digest of the line it
// was built from. This finds that line again from the Project's own records,
// so a lawyer can see each hidden value's original: never from a request's
// restoration mapping, which is never kept.
namespace singularity::privacy::context_records {

// The Project's values such records are made of, each key's by its key (and
// every one of them under "").
using ValuePool = std::map<std::string, std::vector<std::string>>;

struct ProjectValues {
	ValuePool pool;
	// Whole lines a request is given as they are - the data room's journal -
	// each without its line feed.
	std::vector<std::string> lines;
};

void add_value(ValuePool &pool, const std::string &key, const std::string &value);

// Every text of a JSON record, under the key it stands at.
void add_record_values(ValuePool &pool, const nlohmann::ordered_json &value, const std::string &key);

// The line a record was built from, when the Project can still make it byte
// for byte: first as one of its whole lines, then from its values, a value
// taken only where it lines up with what left. `is_cut` when the values that
// line up are too many to try every way of putting them together: the line
// may not have changed at all, and is not said to have.
struct Rebuilt {
	std::optional<std::string> line;
	bool is_cut = false;
};

Rebuilt rebuild_record(const std::string &sent, const std::string &content_sha256, const ProjectValues &values,
	const std::vector<sent_passages::MarkerFact> &facts);

} // namespace singularity::privacy::context_records
