#include "Matter/SharedMatter/SharedMatterService.hpp"

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/DocumentVersionStore/DocumentVersionStore.hpp"
#include "Document/ProfessionalArtifactAction/ProfessionalArtifactAction.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Tool/DeviceTrust/DeviceTrust.hpp"

#include <algorithm>
#include <map>
#include <mutex>

#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif
#include <set>
#include <stdexcept>
#include <system_error>

namespace singularity::matter {
namespace {

// Events already looked at in this session: each change found on disk is
// looked at once.
// Never destroyed: a folder watch's callback can still run while the process
// exits.
std::mutex &checked_mutex() {
	static std::mutex *mutex = new std::mutex();
	return *mutex;
}

std::set<std::string> &checked_events() {
	static std::set<std::string> *events = new std::set<std::string>();
	return *events;
}

std::string normalised(const std::string &paragraph) {
	std::string result;
	bool isSpace = true;
	for (std::size_t index = 0; index < paragraph.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(paragraph[index]);
		// A no-break space (U+00A0, C2 A0) is a space.
		const bool isNoBreak = byte == 0xC2 && index + 1 < paragraph.size() && static_cast<unsigned char>(paragraph[index + 1]) == 0xA0;
		if (byte == ' ' || byte == '\t' || byte == '\r' || isNoBreak) {
			if (isNoBreak) ++index;
			if (!isSpace) result += ' ';
			isSpace = true;
			continue;
		}
		result += paragraph[index];
		isSpace = false;
	}
	while (!result.empty() && result.back() == ' ') result.pop_back();
	return result;
}

void check_for_overwrites(const std::string &project_id) {
	const matters::Matter matter = read_project_documents(project_id);
	if (!matter.directory_path || matter.directory_path->empty()) return;
	const std::filesystem::path folder(*matter.directory_path);
	const std::filesystem::path versionsRoot = documents::document_versions_root(matter.directory_path,
		node::require_active_workspace_path(), project_id);
	const std::string device = platform::current_device_id();
	const std::size_t first = matter.document_events.size() > 200 ? matter.document_events.size() - 200 : 0;
	for (std::size_t index = first; index < matter.document_events.size(); ++index) {
		const documents::DocumentEvent &event = matter.document_events[index];
		if (!event.actor.empty() || event.kind != documents::DocumentEventKind::Changed) continue;
		{
			const std::lock_guard<std::mutex> lock(checked_mutex());
			if (!checked_events().insert(project_id + "\n" + event.id).second) continue;
		}
		const std::filesystem::path current = folder / std::filesystem::u8path(event.relative_path);
		std::error_code error;
		// Only the document as this change left it is compared; a later change
		// has its own event.
		if (!std::filesystem::is_regular_file(current, error) || platform::sha256_hex_digest_of_file(current) != event.fingerprint) continue;
		(void)detect_overwritten_change(folder, versionsRoot, matter, event, current, read_document_paragraphs, device,
			workspaces::current_timestamp());
	}
}

} // namespace

std::optional<std::vector<std::string>> read_document_paragraphs(const std::filesystem::path &file) {
	documents::DocumentExtractionRequest request;
	request.path = file;
	const documents::DocumentExtractionResult result = documents::LocalDocumentExtractor().extract(request);
	if (result.status != documents::ExtractionStatus::ready) return std::nullopt;
	std::vector<std::string> paragraphs;
	std::size_t start = 0;
	while (start <= result.text.size()) {
		const std::size_t end = result.text.find('\n', start);
		const std::string line = normalised(result.text.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (!line.empty()) paragraphs.push_back(line);
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return paragraphs;
}

SharedMatterHooks production_shared_matter_hooks() {
	SharedMatterHooks hooks;
	hooks.opened = [](const std::string &, const std::filesystem::path &folder) {
		const platform::ActorIdentity identity = platform::resolve_actor_identity();
		record_member(folder, identity.actor_id, identity.display_name.value_or(""), identity.organisation_name.value_or(""),
			own_office(), identity.is_authenticated, identity.device_id, workspaces::current_timestamp());
	};
	hooks.reconciled = [](const std::string &project_id) { check_for_overwrites(project_id); };
	return hooks;
}

SharedActivity shared_activity(const std::string &project_id, std::size_t most_changes) {
	SharedActivity activity;
	const matters::Matter matter = read_project_documents(project_id);
	activity.my_actor_id = platform::resolve_actor_identity().actor_id;
	if (matter.directory_path && !matter.directory_path->empty()) {
		const std::filesystem::path folder(*matter.directory_path);
		std::error_code error;
		if (!std::filesystem::is_directory(folder, error)) {
			activity.access = "unavailable";
		} else {
#if defined(__APPLE__) || defined(__linux__)
			if (::access(folder.c_str(), W_OK) != 0) activity.access = "read_only";
#endif
			activity.members = load_members(folder);
			activity.agents = agents::load_agent_cards(folder);
			activity.overwritten = load_overwritten_changes(folder);
		}
	}
	activity.this_device_id = platform::current_device_id();
	activity.changes = attributed_changes(matter, activity.members, activity.my_actor_id, most_changes);
	for (const SharedMember &member : activity.members)
		if (member.actor_id != activity.my_actor_id) activity.is_shared = true;
	for (const AttributedChange &change : activity.changes)
		if (change.author == ChangeAuthor::Member) activity.is_shared = true;
	return activity;
}

SharedActivity decide_member_computer(const std::string &project_id, const std::string &actor_id,
		const std::string &device_id, bool confirm) {
	const SharedActivity activity = shared_activity(project_id, 1);
	const SharedDeviceKey *stated = nullptr;
	for (const SharedMember &member : activity.members) {
		if (member.actor_id != actor_id) continue;
		for (const SharedDeviceKey &key : member.device_keys)
			if (key.device_id == device_id) stated = &key;
	}
	if (stated == nullptr)
		throw std::runtime_error("This computer is not one of the people working on this Matter now, so it cannot be "
			"confirmed here. Open the Matter's team again.");
	if (confirm) tools::confirm_device(actor_id, stated->public_key, activity.my_actor_id);
	else tools::revoke_device(actor_id, stated->public_key, activity.my_actor_id);
	return shared_activity(project_id, 60);
}

SharedActivity decide_asking_computer(const std::string &project_id, const std::string &actor_id,
		const std::string &device_key, bool confirm) {
	const std::string me = platform::resolve_actor_identity().actor_id;
	if (confirm) tools::confirm_device(actor_id, device_key, me);
	else tools::revoke_device(actor_id, device_key, me);
	return shared_activity(project_id, 60);
}

FirmTeam firm_team() {
	FirmTeam team;
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	team.my_actor_id = identity.actor_id;
	team.this_device_id = platform::current_device_id();
	std::vector<matters::Matter> known;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		// A colleague's new computer may have no workspace yet: no Matter is known
		// here, and the firm's record still says who sits in each seat (D-205,
		// D-209; the PC's walk, 2026-11-02: its Team listed no one but the member).
		const std::optional<std::filesystem::path> workspace = node::configured_workspace_path();
		if (workspace.has_value() == true) known = matters::load_matters(*workspace);
	}
	std::map<std::string, std::size_t> byActor;
	for (const matters::Matter &matter : known) {
		if (!matter.directory_path || matter.directory_path->empty() || matter::is_desk(matter)) continue;
		const std::filesystem::path folder(*matter.directory_path);
		std::error_code error;
		if (!std::filesystem::is_directory(folder, error)) continue;
		for (const SharedMember &member : load_members(folder)) {
			const auto found = byActor.find(member.actor_id);
			if (found == byActor.end()) {
				byActor[member.actor_id] = team.people.size();
				FirmPerson person;
				person.member = member;
				person.is_you = member.actor_id == team.my_actor_id;
				person.matters.push_back({matter.id, matter.name});
				team.people.push_back(std::move(person));
			} else {
				FirmPerson &person = team.people[found->second];
				person.matters.push_back({matter.id, matter.name});
				// Their computers as every Matter knows them.
				for (const SharedDeviceKey &key : member.device_keys) {
					const bool isKnown = std::any_of(person.member.device_keys.begin(), person.member.device_keys.end(),
						[&key](const SharedDeviceKey &held) { return held.public_key == key.public_key; });
					if (isKnown == false) person.member.device_keys.push_back(key);
				}
				if (member.last_seen_at > person.member.last_seen_at) {
					person.member.last_seen_at = member.last_seen_at;
					if (member.name.empty() == false) person.member.name = member.name;
				}
			}
		}
		for (agents::AgentCard &card : agents::load_agent_cards(folder)) team.agents.push_back(FirmAgent{std::move(card), matter.name});
	}
	std::sort(team.people.begin(), team.people.end(), [](const FirmPerson &left, const FirmPerson &right) {
		if (left.is_you != right.is_you) return left.is_you;
		return left.member.last_seen_at > right.member.last_seen_at;
	});
	return team;
}

SharedActivity resolve_overwritten(const std::string &project_id, const std::string &change_id, const std::string &choice) {
	const matters::Matter matter = read_project_documents(project_id);
	if (!matter.directory_path || matter.directory_path->empty())
		throw std::invalid_argument("This Matter has no folder on this computer.");
	const std::filesystem::path folder(*matter.directory_path);
	std::optional<OverwrittenChange> found;
	for (const OverwrittenChange &change : load_overwritten_changes(folder))
		if (change.id == change_id) found = change;
	if (!found) throw std::invalid_argument("This change is not one Singularity found.");
	if (!found->resolution.empty()) throw std::invalid_argument("This was already decided.");
	const std::string actor = platform::resolve_actor_identity().actor_id;
	if (choice == "restore_written") {
		const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
			[&](const matters::MatterDocument &entry) { return entry.id == found->document_id; });
		if (document == matter.documents.end()) throw std::invalid_argument("This document is no longer in the Matter.");
		// The document as it is now is kept before the change is put back, so
		// the later save is not lost either.
		(void)documents::restore_document_version(project_id, found->document_id, found->written_version, document->fingerprint);
		(void)resolve_overwritten_change(folder, change_id, "restored_written", actor, workspaces::current_timestamp());
	} else if (choice == "keep_current") {
		(void)resolve_overwritten_change(folder, change_id, "kept_current", actor, workspaces::current_timestamp());
	} else {
		throw std::invalid_argument("Choose whether to keep the document as it is or bring the change back.");
	}
	return shared_activity(project_id, 60);
}

} // namespace singularity::matter
