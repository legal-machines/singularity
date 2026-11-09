#include "Organisation/OrganisationProduction/OrganisationProduction.hpp"

#include "Agent/AgentActions/AgentActions.hpp"
#include "Agent/AgentRoles/AgentRoles.hpp"
#include "Agent/AgentStore/AgentStore.hpp"
#include "Chat/ChatExecution/ChatExecution.hpp"
#include "Matter/FirmComputers/FirmComputers.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterCoordination/MatterCoordination.hpp"
#include "Matter/SharedMatter/SharedMatter.hpp"
#include "Matter/SharedMatter/SharedMatterService.hpp"
#include "Organisation/FirmPeople/FirmPeople.hpp"
#include "Organisation/FirmRole/FirmRole.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Tool/DeviceTrust/DeviceTrust.hpp"
#include "WorkProduct/Playbook/Playbook.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>

namespace singularity::organisation {

namespace {

std::filesystem::path own_office_path() {
	return platform::app_settings_directory() / "Member Office.json";
}

std::filesystem::path own_photo_path() {
	return platform::app_settings_directory() / "Member Photo.json";
}

std::mutex &own_office_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::string trimmed_words(const std::string &text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	const std::size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

std::size_t characters_of(const std::string &text) {
	std::size_t count = 0;
	for (const char byte : text) {
		if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) ++count;
	}
	return count;
}

} // namespace

MemberOffice own_office() {
	const std::lock_guard<std::mutex> lock(own_office_mutex());
	std::ifstream file(own_office_path(), std::ios::binary);
	if (!file) return MemberOffice{};
	const nlohmann::json value = nlohmann::json::parse(file, nullptr, false);
	if (value.is_object() == false) return MemberOffice{};
	MemberOffice office;
	if (value.contains("position") == true && value.at("position").is_string() == true) office.position = value.at("position").get<std::string>();
	if (value.contains("practice") == true && value.at("practice").is_string() == true) office.practice = value.at("practice").get<std::string>();
	return office;
}

namespace {

// Every Matter known here says it at once: the record of this person in it,
// with their office and their photo.
void record_in_every_matter(const MemberOffice &office) {
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	std::vector<matters::Matter> known;
	try {
		known = matter::list();
	} catch (const std::exception &) {
	}
	for (const matters::Matter &matter : known) {
		if (matter.directory_path.has_value() == false || matter.directory_path->empty() == true) continue;
		matter::record_member(std::filesystem::u8path(*matter.directory_path), identity.actor_id, identity.display_name.value_or(""),
			identity.organisation_name.value_or(""), office, identity.is_authenticated, identity.device_id, agents::agent_timestamp_now());
	}
}

} // namespace

MemberOffice set_own_office(const MemberOffice &wanted) {
	MemberOffice office;
	office.position = trimmed_words(wanted.position);
	office.practice = trimmed_words(wanted.practice);
	if (characters_of(office.position) > 80) throw std::invalid_argument("Your position is longer than 80 characters.");
	if (characters_of(office.practice) > 160) throw std::invalid_argument("Your practice's name is longer than 160 characters.");
	{
		const std::lock_guard<std::mutex> lock(own_office_mutex());
		const std::filesystem::path path = own_office_path();
		std::filesystem::create_directories(path.parent_path());
		const std::filesystem::path staged = platform::staged_sibling_path(path);
		platform::write_file_durably(staged, nlohmann::json{{"position", office.position}, {"practice", office.practice}}.dump(2) + "\n");
		platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
	}
	record_in_every_matter(office);
	publish_own_profile();
	return office;
}

std::string own_photo() {
	const std::lock_guard<std::mutex> lock(own_office_mutex());
	std::ifstream file(own_photo_path(), std::ios::binary);
	if (!file) return std::string();
	const nlohmann::json value = nlohmann::json::parse(file, nullptr, false);
	if (value.is_object() == false || value.contains("photo") == false || value.at("photo").is_string() == false) return std::string();
	const std::string photo = value.at("photo").get<std::string>();
	return matter::is_member_photo(photo) ? photo : std::string();
}

std::string set_own_photo(const std::string &photo) {
	if (photo.empty() == false && matter::is_member_photo(photo) == false)
		throw std::invalid_argument("This photo can't be used. Choose a JPEG or PNG picture.");
	{
		const std::lock_guard<std::mutex> lock(own_office_mutex());
		const std::filesystem::path path = own_photo_path();
		if (photo.empty() == true) {
			std::error_code error;
			std::filesystem::remove(path, error);
		} else {
			std::filesystem::create_directories(path.parent_path());
			const std::filesystem::path staged = platform::staged_sibling_path(path);
			platform::write_file_durably(staged, nlohmann::json{{"photo", photo}}.dump() + "\n");
			platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
		}
	}
	record_in_every_matter(own_office());
	publish_own_profile();
	return photo;
}

namespace {

// When this computer last took its own seat's profile from the firm's
// record, so one profile is taken once and a profile this computer wrote is
// never taken back as news.
std::filesystem::path profile_taken_path() {
	return platform::app_settings_directory() / "Member Profile.json";
}

std::string profile_taken_at() {
	std::ifstream file(profile_taken_path(), std::ios::binary);
	if (!file) return std::string();
	const nlohmann::json value = nlohmann::json::parse(file, nullptr, false);
	return value.is_object() && value.contains("at") && value.at("at").is_string() ? value.at("at").get<std::string>() : std::string();
}

void keep_profile_taken_at(const std::string &at) {
	const std::filesystem::path path = profile_taken_path();
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	const std::filesystem::path staged = platform::staged_sibling_path(path);
	platform::write_file_durably(staged, nlohmann::json{{"at", at}}.dump() + "\n");
	platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
}

// The seat signed in here, or none.
std::string own_seat() {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const platform::AppSettings settings = platform::load_app_settings();
	return settings.account.has_value() ? settings.account->seat_id : std::string();
}

std::string seat_id_of(std::string value) {
	value = trimmed_words(value);
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

bool is_firm_position(const std::string &position) {
	const std::vector<std::string> &positions = firm_positions();
	return std::find(positions.begin(), positions.end(), position) != positions.end();
}

} // namespace

void publish_own_profile() {
	const std::string seat = own_seat();
	if (seat.empty() == true) return;
	try {
		const std::optional<SeatProfile> known = seat_profile(seat);
		SeatProfile profile = known.value_or(SeatProfile{});
		profile.seat_id = seat;
		const std::string name = trimmed_words(platform::resolve_actor_identity().display_name.value_or(""));
		if (name.empty() == false) profile.name = name;
		const MemberOffice office = own_office();
		if (is_firm_position(office.position) == true) profile.rank = office.position;
		profile.practice = office.practice;
		profile.photo = own_photo();
		if (profile.name.empty() == true) return;
		if (known.has_value() == true && known->name == profile.name && known->rank == profile.rank && known->practice == profile.practice
				&& known->photo == profile.photo) {
			return;
		}
		const SeatProfile saved = save_seat_profile(profile);
		keep_profile_taken_at(saved.at);
	} catch (const std::exception &) {
		// Without the firm's record here yet, it is said once there is one.
	}
}

void adopt_own_profile() {
	const std::string seat = own_seat();
	if (seat.empty() == true) return;
	std::optional<SeatProfile> profile;
	try {
		profile = seat_profile(seat);
	} catch (const std::exception &) {
		return;
	}
	// The administrator's own profile is theirs alone to write: said to the
	// firm's record once when it holds none, so the firm's other computers know
	// who the administrator is. A member's waits for the one the administrator
	// wrote, which their computer then takes as its own.
	if (profile.has_value() == false) {
		if (is_administrator() == true) publish_own_profile();
		return;
	}
	if (profile->at <= profile_taken_at()) return;
	keep_profile_taken_at(profile->at);
	if (profile->name.empty() == false) {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		platform::AppSettings settings = platform::load_app_settings();
		if (settings.local_display_name != profile->name) {
			settings.local_display_name = profile->name;
			if (settings.account.has_value() == true) settings.account->display_name = profile->name;
			platform::save_app_settings(settings);
		}
	}
	try {
		const MemberOffice office = own_office();
		if (office.position != profile->rank || office.practice != profile->practice) {
			MemberOffice wanted;
			wanted.position = profile->rank;
			wanted.practice = profile->practice;
			(void)set_own_office(wanted);
		}
		if (own_photo() != profile->photo) (void)set_own_photo(profile->photo);
	} catch (const std::exception &) {
	}
}

void adopt_firm_ai_setup() {
	try {
		if (is_administrator() == true) {
			std::vector<FirmModelRole> roles;
			{
				const persistence::PersistenceLock lock(persistence::persistence_mutex());
				for (const platform::RoleModelAssignment &assignment : platform::load_app_settings().local_node_setup.role_assignments)
					roles.push_back(FirmModelRole{assignment.role, assignment.provider_id, assignment.model_id});
			}
			(void)save_firm_ai_setup(roles);
			return;
		}
		const std::optional<FirmAiSetup> firm = firm_ai_setup();
		if (firm.has_value() == false) return;
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		platform::AppSettings settings = platform::load_app_settings();
		platform::LocalNodeSetup &setup = settings.local_node_setup;
		bool isChanged = false;
		for (const FirmModelRole &role : firm->roles) {
			const bool isConnected = std::any_of(setup.connected_providers.begin(), setup.connected_providers.end(),
				[&role](const platform::ConnectedProvider &provider) { return provider.id == role.provider_id; });
			if (isConnected == false) continue;
			const auto kept = std::find_if(setup.role_assignments.begin(), setup.role_assignments.end(),
				[&role](const platform::RoleModelAssignment &assignment) { return assignment.role == role.role; });
			if (kept != setup.role_assignments.end() && kept->provider_id == role.provider_id && kept->model_id == role.model_id) continue;
			if (kept == setup.role_assignments.end()) {
				setup.role_assignments.push_back(platform::RoleModelAssignment{role.role, role.provider_id, role.model_id});
			} else {
				kept->provider_id = role.provider_id;
				kept->model_id = role.model_id;
			}
			// The legacy mirror of the Reasoning role moves with it.
			if (role.role == "reasoning") setup.default_model = role.provider_id + ":" + role.model_id;
			isChanged = true;
		}
		if (isChanged == true) platform::save_app_settings(settings);
	} catch (const std::exception &) {
		// Without the firm's record here yet, the firm's setup comes with it.
	}
}

SeatProfile save_profile_of_seat(const SeatProfile &wanted) {
	const std::string seat = own_seat();
	const bool isOwn = seat.empty() == false && seat_id_of(wanted.seat_id) == seat;
	// The firm's administrator alone writes who sits in a seat, the member's
	// own included.
	if (is_administrator() == false) throw std::invalid_argument("Only the firm's administrator changes a profile.");
	// A profile's words change here, never where its colleague stands.
	SeatProfile edited = wanted;
	try {
		const std::optional<SeatProfile> known = seat_profile(seat_id_of(wanted.seat_id));
		edited.state = known.has_value() == true ? known->state : std::string();
	} catch (const std::exception &) {
		edited.state.clear();
	}
	const SeatProfile saved = save_seat_profile(edited);
	if (isOwn == true) adopt_own_profile();
	return saved;
}

SeatProfile set_seat_state(const std::string &seat_id, const std::string &state, const std::function<void()> &at_legal_machines) {
	if (is_administrator() == false) throw std::invalid_argument("Only the firm's administrator archives, restores or removes a colleague.");
	if (state.empty() == false && state != "archived" && state != "removed") throw std::invalid_argument("A colleague is at work, archived or removed.");
	const std::string seat = seat_id_of(seat_id);
	if (seat.empty() == true) throw std::invalid_argument("Choose the colleague.");
	if (seat == own_seat()) throw std::invalid_argument("Your own seat stays: you are the firm's administrator.");
	const std::optional<SeatProfile> known = seat_profile(seat);
	const std::string before = known.has_value() == true ? known->state : std::string();
	SeatProfile profile = known.value_or(SeatProfile{});
	if (before == state) return profile;
	if (before == "removed") throw std::invalid_argument("This colleague was removed from the firm. Add them as a new colleague.");
	profile.seat_id = seat;
	// Who sat in a seat made before the firm's record said so: as the team knows them.
	if (profile.name.empty() == true) {
		try {
			for (const matter::FirmPerson &person : matter::firm_team().people)
				if (person.member.actor_id == "seat:" + seat && person.member.name.empty() == false) profile.name = person.member.name;
		} catch (const std::exception &) {
		}
		if (profile.name.empty() == true) profile.name = seat;
	}
	// Legal Machines first: a seat still open is closed, an archived one made again.
	if (before.empty() == true || state.empty() == true) {
		if (at_legal_machines) at_legal_machines();
	}
	profile.state = state;
	return save_seat_profile(profile);
}

matter::FirmTeam firm_team_with_profiles() {
	matter::FirmTeam team = matter::firm_team();
	std::vector<SeatProfile> profiles;
	try {
		profiles = seat_profiles();
	} catch (const std::exception &) {
	}
	for (const SeatProfile &profile : profiles) {
		const std::string actor = "seat:" + profile.seat_id;
		auto found = std::find_if(team.people.begin(), team.people.end(),
			[&actor](const matter::FirmPerson &person) { return person.member.actor_id == actor; });
		// Removed from the firm: no longer of its team, whatever Matter still
		// names them; their words stay where they wrote them.
		if (profile.state == "removed") {
			if (found != team.people.end()) found->is_removed = true;
			continue;
		}
		if (found == team.people.end()) {
			// A colleague the firm made a seat for, before their computer opened
			// any of its Matters: the firm's record says who they are.
			matter::FirmPerson person;
			person.member.actor_id = actor;
			person.is_you = actor == team.my_actor_id;
			team.people.push_back(std::move(person));
			found = std::prev(team.people.end());
		}
		if (profile.name.empty() == false) found->member.name = profile.name;
		if (profile.rank.empty() == false) found->member.position = profile.rank;
		found->member.practice = profile.practice;
		found->member.photo = profile.photo;
		found->member.title = profile.title;
		found->member.about = profile.about;
		found->is_archived = profile.state == "archived";
	}
	return team;
}

void refuse_removed_colleagues(const matter::FirmTeam &team) {
	std::set<std::string> removed;
	std::vector<SeatComputer> known;
	for (const matter::FirmPerson &person : team.people) {
		if (person.member.actor_id.rfind("seat:", 0) != 0) continue;
		const std::string seat = person.member.actor_id.substr(5);
		if (person.is_removed == true) removed.insert(seat);
		for (const matter::SharedDeviceKey &key : person.member.device_keys) known.push_back({seat, key.public_key});
	}
	if (removed.empty() == true) return;
	try {
		for (const matter::firm_computers::Computer &computer : matter::firm_computers::checked_records())
			known.push_back({computer.seat_id, computer.device_key});
	} catch (const std::exception &) {
	}
	const std::string me = platform::resolve_actor_identity().actor_id;
	bool isRefused = false;
	for (const SeatComputer &computer : computers_of_removed(removed, own_seat(), known)) {
		if (tools::is_refused_key(computer.device_key) == true) continue;
		tools::revoke_device("seat:" + computer.seat_id, computer.device_key, me);
		isRefused = true;
	}
	// The round that follows sends them nothing.
	if (isRefused == true) matter::coordination::wake();
}

std::string picture_from_file(const std::filesystem::path &path) {
	constexpr std::uintmax_t kMostBytes = 20u * 1024u * 1024u;
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error) throw std::invalid_argument("This picture couldn't be opened. Choose another.");
	if (size > kMostBytes) throw std::invalid_argument("This picture is larger than 20 MB. Choose a smaller one.");
	std::ifstream file(path, std::ios::binary);
	if (!file) throw std::invalid_argument("This picture couldn't be opened. Choose another.");
	const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const std::string type = matter::picture_type(bytes);
	if (type.empty() == true) throw std::invalid_argument("Choose a JPEG, PNG, GIF or WebP picture.");
	return "data:" + type + ";base64," + platform::base64_encode(bytes);
}

Viewer viewer() {
	Viewer viewer;
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	viewer.member_id = identity.actor_id;
	viewer.name = identity.display_name.value_or("");
	viewer.device_id = identity.device_id;
	// The firm as this computer names it where nobody is signed in to Legal
	// Machines (Settings, or a whole-product test's onboarding); a signed-in
	// account's own name takes its place below.
	viewer.organisation_name = identity.organisation_name.value_or("");
	// The firm's AI colleagues and its model accounts are set up by its
	// administrative account; a colleague who joined does neither.
	const bool isAdministrator = is_administrator();
	viewer.manages_colleagues = isAdministrator;
	viewer.connects_models = isAdministrator;
	viewer.manages_seats = isAdministrator;
	const MemberOffice office = own_office();
	viewer.position = office.position;
	viewer.practice = office.practice;
	viewer.photo = own_photo();
	platform::AppSettings settings;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		settings = platform::load_app_settings();
	}
	if (settings.account) {
		viewer.organisation_id = settings.account->organisation_id;
		viewer.seat_id = settings.account->seat_id;
		viewer.organisation_name = settings.account->organisation_name.value_or("");
		if (viewer.name.empty()) viewer.name = settings.account->display_name.value_or("");
	}
	return viewer;
}

namespace {

std::string role_name_of(const std::string &kind) {
	const agents::AgentRole *role = agents::agent_role(kind);
	return role != nullptr ? role->label : std::string();
}

// The folder a colleague watches, relative to its Matter's; empty for the
// whole Matter or when the two do not meet.
std::string folder_within(const std::string &source_root, const std::string &matter_folder) {
	if (source_root.empty() || matter_folder.empty()) return "";
	std::error_code error;
	const std::filesystem::path relative = std::filesystem::u8path(source_root).lexically_normal()
		.lexically_relative(std::filesystem::u8path(matter_folder).lexically_normal());
	if (error || relative.empty()) return "";
	const std::string text = relative.generic_u8string();
	if (text == "." || text.rfind("..", 0) == 0) return "";
	return text;
}

ColleagueFacts colleague_here(const agents::AgentView &view, const std::map<std::string, std::string> &matter_folders) {
	const agents::Agent &agent = view.agent;
	ColleagueFacts colleague;
	colleague.agent_id = agent.id;
	colleague.name = agent.name;
	colleague.role = agent.kind;
	colleague.role_name = role_name_of(agent.kind);
	colleague.mode = agents::to_string(agent.mode);
	colleague.character = agent.character;
	colleague.matter_id = agent.matter_id;
	colleague.state = view.state;
	colleague.status = agents::to_string(agent.status);
	colleague.is_running = view.is_running;
	colleague.presence = view.presence;
	colleague.runs_here = true;
	colleague.responsible_actor_id = agent.responsible_actor_id;
	const auto folder = matter_folders.find(agent.matter_id);
	colleague.folder = folder_within(agent.source_root, folder == matter_folders.end() ? std::string() : folder->second);
	colleague.trigger = agents::to_string(agent.trigger);
	colleague.playbook_id = agent.playbook_id;
	if (agent.playbook_id.empty() == false) {
		try {
			if (const std::optional<playbooks::Playbook> playbook = playbooks::get(agent.playbook_id))
				colleague.playbook_name = playbook->name;
		} catch (const std::exception &) {
		}
	}
	colleague.instruction = agent.goal;
	colleague.paused_reason = agent.paused_reason;
	colleague.work_given = agent.work_given;
	try {
		for (const agents::AgentAssignment &work : agents::list_assignments(agent.id, 10))
			colleague.work.push_back(WorkGivenFacts{work.id, work.state, work.instruction, work.given_by_actor_id,
				work.given_at, work.started_at, work.finished_at, work.chat_id});
		for (const agents::AgentRun &run : agents::load_runs(agent.id, 20))
			colleague.runs.push_back(RunFacts{run.status, run.started_at, run.finished_at});
	} catch (const std::exception &) {
	}
	colleague.spend = agent.spend;
	colleague.spend_currency = agent.spend_currency;
	colleague.spend_month = agent.spend_month;
	colleague.last_run_at = view.latest_run ? view.latest_run->started_at : std::string();
	colleague.updated_at = agent.updated_at;
	return colleague;
}

ColleagueFacts colleague_elsewhere(const matter::FirmAgent &published) {
	const agents::AgentCard &card = published.card;
	ColleagueFacts colleague;
	colleague.agent_id = card.agent_id;
	colleague.name = card.name;
	colleague.role = card.kind;
	colleague.role_name = role_name_of(card.kind);
	colleague.mode = card.mode;
	colleague.character = card.character;
	colleague.matter_id = card.matter_id;
	colleague.state = card.state;
	colleague.status = card.status;
	// Another computer runs it: only its state is known here.
	colleague.presence = agents::agent_presence(card.state, true, std::string());
	colleague.runs_here = false;
	colleague.responsible_actor_id = card.responsible_actor_id;
	colleague.trigger = card.trigger;
	colleague.instruction = card.goal;
	colleague.last_run_at = card.last_run_at;
	return colleague;
}

MatterFacts matter_facts(const matters::Matter &matter) {
	MatterFacts facts;
	facts.id = matter.id;
	facts.name = matter.name;
	// Whose work each operation is, as the Matter recorded it.
	std::map<std::string, const matters::MatterTask *> taskOf;
	for (const matters::MatterTask &task : matter.tasks)
		if (task.agent_operation_id) taskOf[*task.agent_operation_id] = &task;
	try {
		for (const chat::ChatOperationSummary &summary : chat::list_operations(matter.id)) {
			OperationFacts operation;
			operation.operation_id = summary.operation_id;
			operation.chat_id = summary.chat_id.value_or("");
			operation.state = chat::to_string(summary.state);
			operation.activity = summary.activity;
			operation.prompt = summary.prompt;
			operation.updated_at = summary.updated_at;
			operation.runs_elsewhere = summary.runs_elsewhere;
			const auto task = taskOf.find(summary.operation_id);
			if (task != taskOf.end()) {
				operation.agent_id = task->second->agent_id;
				operation.author_actor_id = task->second->author_actor_id;
				if (task->second->lawyer_questions.empty() == false)
					operation.question_title = task->second->lawyer_questions.back().title.value_or("");
			}
			facts.operations.push_back(operation);
		}
	} catch (const std::exception &) {
	}
	if (matter.directory_path && matter.directory_path->empty() == false) {
		try {
			for (const matter::OverwrittenChange &change :
					matter::load_overwritten_changes(std::filesystem::u8path(*matter.directory_path)))
				if (change.resolution.empty()) facts.overwritten.push_back(OverwrittenFacts{change.id, change.name, change.found_at});
		} catch (const std::exception &) {
		}
	}
	try {
		for (const matter::ProjectConflictSummary &conflict : matter::list_project_conflicts(matter.id))
			facts.conflicts.push_back(ConflictFacts{conflict.id, conflict.label, conflict.area, conflict.current_timestamp});
	} catch (const std::exception &) {
	}
	return facts;
}

} // namespace

OrganisationFacts gather_facts() {
	OrganisationFacts facts;
	facts.viewer = viewer();
	facts.now = agents::agent_timestamp_now();

	std::vector<matters::Matter> matters;
	try {
		matters = matter::list();
	} catch (const std::exception &) {
	}
	std::map<std::string, std::string> matterFolders;
	for (const matters::Matter &matter : matters) {
		if (matter.directory_path) matterFolders[matter.id] = *matter.directory_path;
		facts.matters.push_back(matter_facts(matter));
	}

	matter::FirmTeam team;
	try {
		team = matter::firm_team();
	} catch (const std::exception &) {
	}
	for (const matter::FirmPerson &person : team.people) {
		PersonFacts facts_of;
		facts_of.actor_id = person.member.actor_id;
		facts_of.name = person.member.name;
		facts_of.organisation = person.member.organisation;
		facts_of.is_you = person.is_you;
		for (const auto &matter : person.matters) facts_of.matter_ids.push_back(matter.first);
		facts_of.last_seen_at = person.member.last_seen_at;
		facts.people.push_back(facts_of);
	}

	std::set<std::string> here;
	try {
		for (const agents::AgentView &view : agents::list_agent_views(std::nullopt)) {
			here.insert(view.agent.id);
			facts.colleagues.push_back(colleague_here(view, matterFolders));
		}
	} catch (const std::exception &) {
	}
	// A colleague another computer runs, once: its card stands in each Matter
	// that computer carries (Agent/AgentCard), the newest as it signed it last.
	std::map<std::string, const matter::FirmAgent *> newest;
	for (const matter::FirmAgent &published : team.agents) {
		if (here.count(published.card.agent_id) > 0) continue;
		const matter::FirmAgent *&kept = newest[published.card.agent_id];
		if (kept == nullptr || published.card.counter > kept->card.counter
				|| (published.card.counter == kept->card.counter && published.card.updated_at > kept->card.updated_at))
			kept = &published;
	}
	for (const matter::FirmAgent &published : team.agents) {
		const auto found = newest.find(published.card.agent_id);
		if (found != newest.end() && found->second == &published) facts.colleagues.push_back(colleague_elsewhere(published));
	}
	return facts;
}

OrganisationView read() {
	return build(gather_facts());
}

} // namespace singularity::organisation
