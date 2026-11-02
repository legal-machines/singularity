#include "WorkProduct/Playbook/Playbook.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "WorkProduct/ReviewTable/ReviewTable.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>

namespace singularity::playbooks {
namespace {

using Json = nlohmann::json;

std::mutex &store_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::filesystem::path store() {
	return platform::app_settings_directory() / "Playbooks";
}

// 16 digits that are 64 bits from the system's secure source
// (Platform/SecureRandom).
std::string new_id(const char *prefix) {
	return std::string(prefix) + platform::secure_random_hex(8);
}

// An id names a folder of this computer's: letters, digits and _ only.
bool plain_id(const std::string &id) {
	return !id.empty() && std::all_of(id.begin(), id.end(), [](char character) {
		return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
	});
}

std::string text(const Json &value, const char *key) {
	return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>() : std::string();
}

std::string trimmed(const std::string &value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::optional<Json> read_json(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) return std::nullopt;
	Json value = Json::parse(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()),
		nullptr, false);
	if (!value.is_object()) return std::nullopt;
	return value;
}

void write_json(const std::filesystem::path &path, const Json &value) {
	std::filesystem::create_directories(path.parent_path());
	const std::filesystem::path staged = platform::staged_sibling_path(path);
	try {
		platform::write_file_durably(staged, value.dump(2));
		platform::commit_staged_file(staged, path, platform::StagedCommitMode::CreateExclusive, false);
	} catch (...) {
		platform::discard_staged_file(staged);
		throw;
	}
}

int latest_version(const std::string &id) {
	int latest = 0;
	std::error_code error;
	for (const auto &entry : std::filesystem::directory_iterator(store() / id, error)) {
		const std::string name = entry.path().filename().u8string();
		if (name.size() < 7 || name[0] != 'v' || name.substr(name.size() - 5) != ".json") continue;
		try {
			latest = std::max(latest, std::stoi(name.substr(1, name.size() - 6)));
		} catch (const std::exception &) {
		}
	}
	return latest;
}

std::optional<Playbook> read_version(const std::string &id, int version) {
	if (!plain_id(id) || version <= 0) return std::nullopt;
	const std::optional<Json> value = read_json(store() / id / ("v" + std::to_string(version) + ".json"));
	if (!value) return std::nullopt;
	Playbook playbook = from_json(*value);
	if (playbook.id != id || playbook.version != version) return std::nullopt;
	return playbook;
}

} // namespace

Json to_json(const Playbook &playbook) {
	Json positions = Json::array();
	for (const Position &position : playbook.positions) {
		Json fallbacks = Json::array();
		for (const Fallback &fallback : position.fallbacks)
			fallbacks.push_back(Json{{"position", fallback.position}, {"clause", fallback.clause}});
		Json law = Json::array();
		for (const LawReference &reference : position.law)
			law.push_back(Json{{"citation", reference.citation}, {"note", reference.note}});
		positions.push_back(Json{{"id", position.id}, {"topic", position.topic}, {"question", position.question},
			{"answerKind", position.answer_kind}, {"standard", position.standard},
			{"standardClause", position.standard_clause}, {"fallbacks", std::move(fallbacks)},
			{"redLine", position.red_line}, {"severity", position.severity}, {"law", std::move(law)},
			{"guidance", position.guidance}});
	}
	return Json{{"format", "singularity-playbook"}, {"formatVersion", 1}, {"id", playbook.id}, {"name", playbook.name},
		{"description", playbook.description}, {"contractType", playbook.contract_type}, {"version", playbook.version},
		{"updatedAt", playbook.updated_at}, {"updatedByActorId", playbook.updated_by_actor_id},
		{"positions", std::move(positions)}};
}

Playbook from_json(const Json &value) {
	Playbook playbook;
	if (!value.is_object()) return playbook;
	playbook.id = text(value, "id");
	playbook.name = text(value, "name");
	playbook.description = text(value, "description");
	playbook.contract_type = text(value, "contractType");
	if (value.contains("version") && value.at("version").is_number_integer()) playbook.version = value.at("version").get<int>();
	playbook.updated_at = text(value, "updatedAt");
	playbook.updated_by_actor_id = text(value, "updatedByActorId");
	if (value.contains("positions") && value.at("positions").is_array())
		for (const Json &entry : value.at("positions")) {
			if (!entry.is_object()) continue;
			Position position;
			position.id = text(entry, "id");
			position.topic = text(entry, "topic");
			position.question = text(entry, "question");
			const std::string kind = text(entry, "answerKind");
			if (!kind.empty()) position.answer_kind = kind;
			position.standard = text(entry, "standard");
			position.standard_clause = text(entry, "standardClause");
			if (entry.contains("fallbacks") && entry.at("fallbacks").is_array())
				for (const Json &fallback : entry.at("fallbacks"))
					if (fallback.is_object())
						position.fallbacks.push_back(Fallback{text(fallback, "position"), text(fallback, "clause")});
			position.red_line = text(entry, "redLine");
			const std::string severity = text(entry, "severity");
			if (!severity.empty()) position.severity = severity;
			if (entry.contains("law") && entry.at("law").is_array())
				for (const Json &reference : entry.at("law"))
					if (reference.is_object())
						position.law.push_back(LawReference{text(reference, "citation"), text(reference, "note")});
			position.guidance = text(entry, "guidance");
			playbook.positions.push_back(std::move(position));
		}
	return playbook;
}

std::vector<Playbook> list() {
	const std::lock_guard<std::mutex> lock(store_mutex());
	std::vector<Playbook> playbooks;
	std::error_code error;
	for (const auto &entry : std::filesystem::directory_iterator(store(), error)) {
		if (!entry.is_directory()) continue;
		const std::string id = entry.path().filename().u8string();
		if (std::optional<Playbook> playbook = read_version(id, latest_version(id))) playbooks.push_back(std::move(*playbook));
	}
	std::sort(playbooks.begin(), playbooks.end(), [](const Playbook &left, const Playbook &right) { return left.name < right.name; });
	return playbooks;
}

std::optional<Playbook> get(const std::string &id, std::optional<int> version) {
	const std::lock_guard<std::mutex> lock(store_mutex());
	if (!plain_id(id)) return std::nullopt;
	return read_version(id, version.value_or(latest_version(id)));
}

Playbook save(Playbook playbook) {
	playbook.name = trimmed(playbook.name);
	if (playbook.name.empty()) throw std::runtime_error("Give the Playbook a name.");
	if (playbook.positions.empty()) throw std::runtime_error("A Playbook states at least one position.");
	std::set<std::string> ids;
	for (Position &position : playbook.positions) {
		position.topic = trimmed(position.topic);
		position.question = trimmed(position.question);
		position.standard = trimmed(position.standard);
		if (position.topic.empty()) throw std::runtime_error("Name the topic of every position.");
		if (position.standard.empty())
			throw std::runtime_error("State the firm's position on \"" + position.topic + "\".");
		if (position.question.empty()) position.question = "What does the document provide on " + position.topic + "?";
		if (!plain_id(position.id) || !ids.insert(position.id).second) {
			position.id = new_id("pos_");
			ids.insert(position.id);
		}
		if (position.severity != "high" && position.severity != "medium" && position.severity != "low")
			position.severity = "medium";
	}
	const std::lock_guard<std::mutex> lock(store_mutex());
	if (!plain_id(playbook.id)) playbook.id = new_id("pb_");
	playbook.version = latest_version(playbook.id) + 1;
	playbook.updated_at = workspaces::current_timestamp();
	playbook.updated_by_actor_id = platform::resolve_actor_identity().actor_id;
	// Each version is written once and never changed: a Review Table names the
	// version it checked against.
	write_json(store() / playbook.id / ("v" + std::to_string(playbook.version) + ".json"), to_json(playbook));
	return playbook;
}

bool remove(const std::string &id) {
	const std::lock_guard<std::mutex> lock(store_mutex());
	if (!plain_id(id)) return false;
	std::error_code error;
	return std::filesystem::remove_all(store() / id, error) > 0;
}

Playbook import_json(const std::string &value) {
	const Json parsed = Json::parse(value, nullptr, false);
	if (!parsed.is_object() || text(parsed, "format") != "singularity-playbook")
		throw std::runtime_error("This file is not a Playbook Singularity exported.");
	Playbook playbook = from_json(parsed);
	playbook.id.clear();
	return save(std::move(playbook));
}

std::string position_text(const Position &position) {
	std::string said = "Preferred: " + position.standard;
	if (!position.fallbacks.empty()) {
		said += "\nAcceptable instead, in order of preference:";
		for (std::size_t index = 0; index < position.fallbacks.size(); ++index)
			said += "\n" + std::to_string(index + 1) + ". " + position.fallbacks[index].position;
	}
	if (!position.red_line.empty()) said += "\nNever acceptable: " + position.red_line;
	return said;
}

std::string law_text(const Position &position) {
	std::string said;
	for (const LawReference &reference : position.law) {
		if (!said.empty()) said += "\n";
		said += reference.citation + (reference.note.empty() ? "" : " - " + reference.note);
	}
	return said;
}

matters::ReviewTable review_table(const std::string &project_id, const std::string &playbook_id,
		const std::vector<std::string> &document_ids, const std::string &title, bool local_only) {
	const std::optional<Playbook> playbook = get(playbook_id);
	if (!playbook) throw std::runtime_error("This Playbook is no longer on this computer.");
	review_table::CreateReviewTable command;
	command.project_id = project_id;
	command.title = trimmed(title).empty() ? playbook->name + " review" : title;
	command.document_ids = document_ids;
	command.playbook_id = playbook->id;
	command.playbook_version = playbook->version;
	command.local_only = local_only;
	for (const Position &position : playbook->positions)
		command.columns.push_back(review_table::NewColumn{position.topic, position.question, position.answer_kind,
			playbook->id, position.id, position_text(position), law_text(position)});
	return review_table::create(command);
}

} // namespace singularity::playbooks
