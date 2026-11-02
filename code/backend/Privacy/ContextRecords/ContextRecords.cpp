#include "Privacy/ContextRecords/ContextRecords.hpp"

#include <algorithm>
#include <cstddef>

namespace singularity::privacy::context_records {
namespace {

struct Leaf {
	nlohmann::ordered_json *value = nullptr;
	std::vector<std::string> candidates;
};

// Every text of a record that left with a marker in it, with the Project's
// values that line up with it.
bool find_leaves(nlohmann::ordered_json &value, const std::string &key, const ValuePool &pool,
		const std::vector<sent_passages::MarkerFact> &facts, std::vector<Leaf> &leaves) {
	if (value.is_array() == true) {
		for (nlohmann::ordered_json &entry : value)
			if (find_leaves(entry, key, pool, facts, leaves) == false) return false;
		return true;
	}
	if (value.is_object() == true) {
		for (auto entry = value.begin(); entry != value.end(); ++entry)
			if (find_leaves(entry.value(), entry.key(), pool, facts, leaves) == false) return false;
		return true;
	}
	if (value.is_string() == false) return true;
	const std::string &sent = value.get_ref<const std::string &>();
	bool isHiding = false;
	for (const sent_passages::Segment &segment : sent_passages::sent_segments(sent, facts))
		if (segment.is_hidden == true) isHiding = true;
	if (isHiding == false) return true;
	auto named = pool.find(key);
	if (named == pool.end() || named->second.empty() == true) named = pool.find(std::string());
	if (named == pool.end()) return false;
	Leaf leaf;
	leaf.value = &value;
	for (const std::string &candidate : named->second)
		if (sent_passages::align(candidate, sent, facts).fit == sent_passages::Fit::Aligned) leaf.candidates.push_back(candidate);
	if (leaf.candidates.empty() == true) return false;
	leaves.push_back(std::move(leaf));
	return true;
}

} // namespace

void add_value(ValuePool &pool, const std::string &key, const std::string &value) {
	if (value.empty() == true) return;
	for (const std::string &named : {key, std::string()}) {
		std::vector<std::string> &values = pool[named];
		if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
	}
}

void add_record_values(ValuePool &pool, const nlohmann::ordered_json &value, const std::string &key) {
	if (value.is_string() == true) {
		add_value(pool, key, value.get<std::string>());
		return;
	}
	if (value.is_array() == true) {
		for (const nlohmann::ordered_json &entry : value) add_record_values(pool, entry, key);
		return;
	}
	if (value.is_object() == false) return;
	for (auto entry = value.begin(); entry != value.end(); ++entry) add_record_values(pool, entry.value(), entry.key());
}

Rebuilt rebuild_record(const std::string &sent, const std::string &content_sha256, const ProjectValues &values,
		const std::vector<sent_passages::MarkerFact> &facts) {

	// Each way of putting the values together is one digest of a short line.
	constexpr std::size_t kMostTries = 4096;
	Rebuilt rebuilt;
	for (const std::string &line : values.lines) {
		rebuilt.line = sent_passages::built_from(content_sha256, line);
		if (rebuilt.line.has_value() == true) return rebuilt;
	}

	/* - - - - - - */

	nlohmann::ordered_json record = nlohmann::ordered_json::parse(sent, nullptr, false);
	if (record.is_discarded() == true || record.is_object() == false) return rebuilt;
	std::vector<Leaf> leaves;
	if (find_leaves(record, std::string(), values.pool, facts, leaves) == false || leaves.empty() == true) return rebuilt;
	std::size_t tries = 1;
	for (const Leaf &leaf : leaves) {
		if (tries > kMostTries / leaf.candidates.size()) {
			rebuilt.is_cut = true;
			return rebuilt;
		}
		tries *= leaf.candidates.size();
	}

	/* - - - - - - */

	std::vector<std::size_t> choice(leaves.size(), 0);
	for (std::size_t attempt = 0; attempt < tries; ++attempt) {
		for (std::size_t index = 0; index < leaves.size(); ++index) *leaves[index].value = leaves[index].candidates[choice[index]];
		const std::string line = record.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
		rebuilt.line = sent_passages::built_from(content_sha256, line);
		if (rebuilt.line.has_value() == true) return rebuilt;
		for (std::size_t index = 0; index < choice.size(); ++index) {
			if (++choice[index] < leaves[index].candidates.size()) break;
			choice[index] = 0;
		}
	}
	return rebuilt;

}

} // namespace singularity::privacy::context_records
