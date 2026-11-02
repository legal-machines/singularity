#include "Agent/AgentCard/AgentCard.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace singularity::agents {
namespace {

using Json = nlohmann::json;

bool is_card_id(const std::string &value) {
	if (value.empty() == true || value.size() > 80) return false;
	for (const char character : value) {
		const bool isAllowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
		if (isAllowed == false) return false;
	}
	return true;
}

std::filesystem::path cards_folder(const std::filesystem::path &matter_folder) {
	return matter_folder / ".lm" / "Agents";
}

std::string text_of(const Json &value, const char *key) {
	const auto found = value.find(key);
	if (found == value.end() || found->is_string() == false) return "";
	return found->get<std::string>();
}

int number_of(const Json &value, const char *key) {
	const auto found = value.find(key);
	if (found == value.end() || found->is_number_integer() == false) return 0;
	return found->get<int>();
}

std::string clipped(const std::string &text, std::size_t most) {
	std::size_t count = 0;
	for (std::size_t index = 0; index < text.size(); ++index) {
		if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) {
			if (count == most) return text.substr(0, index) + "…";
			++count;
		}
	}
	return text;
}

std::string plural(std::size_t count, const char *one, const char *many) {
	return std::to_string(count) + " " + (count == 1 ? one : many);
}

// How a run went, in a few words, never a document's contents.
std::string run_words(const AgentRun &run) {
	if (run.status == "running") return "Running now";
	if (run.status == "failed") return "Its last run could not finish";
	if (run.status == "access_needed") return "It cannot read its folder";
	if (run.status == "cancelled") return "Its last run was stopped";
	if (run.status == "interrupted") return "Its last run was interrupted";
	if (run.task.has_value() == true) {
		// Why it did not run, in the run's own words, without their full stop.
		if (run.task->state == "skipped") {
			std::string why = run.task->message.empty() == true ? "Nothing changed, so it did not run" : run.task->message;
			if (why.empty() == false && why.back() == '.') why.pop_back();
			return why;
		}
		if (run.task->state == "waiting_for_approval") return "A change it proposes waits for approval";
		if (run.task->state == "waiting_for_search") return "A web search it needs waits for approval";
		if (run.task->state == "waiting_for_answer") return "A question it asks waits for your answer";
		if (run.task->state == "completed") return "Answered in its chat";
		return "Its task could not be carried out";
	}
	if (run.is_baseline == true) return "Recorded the folder";
	std::vector<std::string> parts;
	if (run.counts.changed > 0) parts.push_back(plural(run.counts.changed, "changed", "changed"));
	if (run.counts.added > 0) parts.push_back(plural(run.counts.added, "new", "new"));
	if (run.counts.removed > 0) parts.push_back(plural(run.counts.removed, "removed", "removed"));
	if (run.counts.moved > 0) parts.push_back(plural(run.counts.moved, "moved", "moved"));
	if (parts.empty() == true) return "Nothing changed";
	std::string words;
	for (std::size_t index = 0; index < parts.size(); ++index) words += (index == 0 ? "" : ", ") + parts[index];
	return words;
}

} // namespace

AgentCard card_of(const Agent &agent, const std::optional<AgentRun> &latest_run, const std::string &state,
		const std::string &host_device_id, const std::string &now) {
	AgentCard card;
	card.agent_id = agent.id;
	card.matter_id = agent.matter_id;
	card.name = agent.name;
	card.kind = agent.kind;
	card.character = agent.character;
	card.office_title = agent.office.title;
	card.office_practice = agent.office.practice;
	card.mode = to_string(agent.mode);
	card.goal = clipped(agent.goal, 600);
	card.trigger = to_string(agent.trigger);
	card.daily_minute = agent.daily_minute;
	card.weekly_day = agent.weekly_day;
	card.follows_agent_id = agent.follows_agent_id;
	card.status = to_string(agent.status);
	card.state = state;
	card.responsible_actor_id = agent.responsible_actor_id.empty() == true ? agent.approved_by_actor_id : agent.responsible_actor_id;
	card.responsible_name = agent.responsible_name;
	card.host_device_id = host_device_id;
	card.id_nonce = agent.id_nonce;
	if (latest_run.has_value() == true) {
		card.last_run_at = latest_run->finished_at.empty() == true ? latest_run->started_at : latest_run->finished_at;
		card.last_run_words = run_words(*latest_run);
		card.needs_review = state == "needs_review";
	}
	card.updated_at = now;
	return card;
}

void publish_agent_card(const std::filesystem::path &matter_folder, const AgentCard &card) {
	if (is_card_id(card.agent_id) == false) return;
	std::error_code error;
	if (std::filesystem::is_directory(matter_folder / ".lm", error) == false) return;
	const std::filesystem::path path = cards_folder(matter_folder) / (card.agent_id + ".json");
	const Json value{
		{"format", "singularity-agent-card"}, {"formatVersion", card.format_version},
		{"agentId", card.agent_id}, {"matterId", card.matter_id}, {"name", card.name}, {"kind", card.kind}, {"mode", card.mode},
		{"character", card.character}, {"officeTitle", card.office_title}, {"officePractice", card.office_practice},
		{"goal", card.goal}, {"trigger", card.trigger}, {"dailyMinute", card.daily_minute}, {"weeklyDay", card.weekly_day},
		{"followsAgentId", card.follows_agent_id}, {"status", card.status}, {"state", card.state},
		{"responsibleActorId", card.responsible_actor_id}, {"responsibleName", card.responsible_name},
		{"hostDeviceId", card.host_device_id}, {"hostDeviceKey", card.host_device_key}, {"idNonce", card.id_nonce},
		{"counter", card.counter}, {"signature", card.signature},
		{"lastRunAt", card.last_run_at}, {"lastRunWords", card.last_run_words},
		{"needsReview", card.needs_review}, {"updatedAt", card.updated_at},
	};
	try {
		platform::ensure_record_directory(path.parent_path());
		const std::filesystem::path staged = platform::staged_sibling_path(path);
		try {
			platform::write_file_durably(staged, value.dump(1, '\t'));
			platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
		} catch (...) {
			platform::discard_staged_file(staged);
			throw;
		}
	} catch (const std::exception &) {
	}
}

std::vector<AgentCard> load_agent_cards(const std::filesystem::path &matter_folder) {
	std::vector<AgentCard> cards;
	std::error_code error;
	std::filesystem::directory_iterator entries(cards_folder(matter_folder), error);
	if (error) return cards;
	for (const auto &entry : entries) {
		if (entry.path().extension() != ".json") continue;
		std::ifstream input(entry.path(), std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		const Json value = Json::parse(bytes, nullptr, false);
		if (value.is_discarded() == true || value.is_object() == false) continue;
		AgentCard card;
		card.agent_id = text_of(value, "agentId");
		// A card speaks only for the agent its file names.
		if (is_card_id(card.agent_id) == false || entry.path().stem().string() != card.agent_id) continue;
		card.format_version = number_of(value, "formatVersion");
		card.matter_id = text_of(value, "matterId");
		card.name = text_of(value, "name");
		card.kind = text_of(value, "kind");
		card.mode = text_of(value, "mode");
		card.character = text_of(value, "character");
		card.office_title = text_of(value, "officeTitle");
		card.office_practice = text_of(value, "officePractice");
		card.goal = text_of(value, "goal");
		card.trigger = text_of(value, "trigger");
		card.daily_minute = number_of(value, "dailyMinute");
		card.weekly_day = number_of(value, "weeklyDay");
		card.follows_agent_id = text_of(value, "followsAgentId");
		card.status = text_of(value, "status");
		card.state = text_of(value, "state");
		card.responsible_actor_id = text_of(value, "responsibleActorId");
		card.responsible_name = text_of(value, "responsibleName");
		card.host_device_id = text_of(value, "hostDeviceId");
		card.host_device_key = text_of(value, "hostDeviceKey");
		card.id_nonce = text_of(value, "idNonce");
		if (const auto counter = value.find("counter"); counter != value.end() && counter->is_number_integer() == true) {
			card.counter = counter->get<std::int64_t>();
		}
		card.signature = text_of(value, "signature");
		card.last_run_at = text_of(value, "lastRunAt");
		card.last_run_words = text_of(value, "lastRunWords");
		const auto review = value.find("needsReview");
		card.needs_review = review != value.end() && review->is_boolean() == true && review->get<bool>() == true;
		card.updated_at = text_of(value, "updatedAt");
		cards.push_back(std::move(card));
	}
	std::sort(cards.begin(), cards.end(), [](const AgentCard &left, const AgentCard &right) { return left.name < right.name; });
	return cards;
}

} // namespace singularity::agents
