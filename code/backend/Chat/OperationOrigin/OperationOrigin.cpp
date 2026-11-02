#include "Chat/OperationOrigin/OperationOrigin.hpp"

#include "Platform/DeviceKey/DeviceKey.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <vector>

namespace singularity::chat {
namespace {

// Operations a Matter holds unsigned, recorded before this day, may be ones
// begun before origins were signed (the header's last paragraph); anything
// recorded since by a version that signs is signed.
constexpr const char *kOriginsSignedFrom = "2026-10-13";

// What the signature covers: the operation it is - its Matter, task, operation
// and Chat - and everything its authority says. Copied onto another task,
// operation or Matter, it does not verify.
std::string signed_bytes(const std::string &project_id, const matters::MatterTask &task,
		const matters::OperationOriginStatement &origin) {
	const nlohmann::json covered{{"projectId", project_id}, {"taskId", task.id},
		{"operationId", task.agent_operation_id.value_or(std::string())},
		{"chatId", task.chat_id.value_or(std::string())}, {"createdAt", task.created_at},
		{"actorId", origin.actor_id}, {"by", origin.by}, {"agentId", origin.agent_id}, {"ceiling", origin.ceiling},
		{"localOnly", origin.local_only}};
	return "singularity-signed-record-v1\noperation_origin\n" + covered.dump();
}

// A request: what Retry, Run with another model and a follow-up carry on.
bool is_operation(const matters::MatterTask &task) {
	return task.operation_type == "matter_task";
}

// A restriction the Matter states for the task, signed or not: either only
// keeps work here.
bool states_kept_here(const matters::MatterTask &task) {
	return task.local_only || (task.origin && task.origin->local_only);
}

tools::SeenOperations see_operations(const matters::Matter &matter) {
	std::vector<std::string> unsignedNow;
	std::vector<std::string> keptHereNow;
	for (const matters::MatterTask &task : matter.tasks) {
		if (!is_operation(task)) continue;
		if (states_kept_here(task)) keptHereNow.push_back(task.id);
		if (!task.origin && task.created_at < kOriginsSignedFrom) unsignedNow.push_back(task.id);
	}
	try {
		return tools::note_seen_operations(matter.id, unsignedNow, keptHereNow);
	} catch (const std::exception &) {
		// Unable to read or keep what it has seen, this computer takes nothing
		// unsigned as begun before: such work stays here.
		tools::SeenOperations nothing;
		nothing.kept_here.insert(keptHereNow.begin(), keptHereNow.end());
		return nothing;
	}
}

enum class Kept { No, Asked, OriginUnconfirmed };

Kept kept_here(const matters::Matter &matter, const matters::MatterTask &task, const tools::SeenOperations &seen) {
	if (states_kept_here(task) || seen.kept_here.count(task.id) > 0) return Kept::Asked;
	if (!is_operation(task)) return Kept::No;
	// Begun here: this computer's own record says what it may do.
	if (task.agent_operation_id) {
		const std::optional<tools::OperationAuthority> authority =
			tools::operation_authority(matter.id, *task.agent_operation_id);
		if (authority) return authority->local_only ? Kept::Asked : Kept::No;
	}
	const MessageProvenance provenance = operation_origin_provenance(matter.id, task);
	if (provenance == MessageProvenance::ThisComputer || provenance == MessageProvenance::Confirmed) return Kept::No;
	if (provenance == MessageProvenance::Unsigned && seen.earlier.count(task.id) > 0) return Kept::No;
	return Kept::OriginUnconfirmed;
}

Kept operation_kept_here(const matters::Matter &matter, const std::string &operation_id) {
	if (operation_id.empty()) return Kept::No;
	const std::optional<tools::OperationAuthority> authority = tools::operation_authority(matter.id, operation_id);
	if (authority && authority->local_only) return Kept::Asked;
	bool hasTask = false;
	for (const matters::MatterTask &task : matter.tasks)
		if (task.agent_operation_id == std::optional<std::string>(operation_id)) hasTask = true;
	if (!hasTask) return Kept::No;
	const tools::SeenOperations seen = see_operations(matter);
	Kept result = Kept::No;
	for (const matters::MatterTask &task : matter.tasks) {
		if (task.agent_operation_id != std::optional<std::string>(operation_id)) continue;
		const Kept kept = kept_here(matter, task, seen);
		if (kept == Kept::Asked) return Kept::Asked;
		if (kept == Kept::OriginUnconfirmed) result = kept;
	}
	return result;
}

} // namespace

void sign_operation_origin(const std::string &project_id, matters::MatterTask &task,
		const tools::OperationAuthority &authority) {
	matters::OperationOriginStatement origin;
	origin.actor_id = authority.actor_id;
	origin.by = authority.origin == tools::OperationOrigin::Agent ? "agent" : "person";
	origin.agent_id = authority.agent_id;
	origin.ceiling = tools::to_string(authority.ceiling);
	origin.local_only = authority.local_only;
	try {
		origin.public_key = platform::device_public_key();
		origin.signature = platform::sign_with_device_key(signed_bytes(project_id, task, origin));
	} catch (const std::exception &) {
		// This computer's key cannot be read: the origin is kept unsigned, and
		// every other computer keeps the work on itself.
		origin.public_key.clear();
		origin.signature.clear();
	}
	task.origin = std::move(origin);
}

MessageProvenance operation_origin_provenance(const std::string &project_id, const matters::MatterTask &task) {
	if (!task.origin) return MessageProvenance::Unsigned;
	return signed_record_provenance(task.origin->actor_id, task.origin->public_key, task.origin->signature,
		signed_bytes(project_id, task, *task.origin));
}

bool task_works_only_here(const matters::Matter &matter, const matters::MatterTask &task) {
	if (states_kept_here(task)) return true;
	if (!is_operation(task)) return false;
	if (task.agent_operation_id) return operation_kept_here(matter, *task.agent_operation_id) != Kept::No;
	return kept_here(matter, task, see_operations(matter)) != Kept::No;
}

bool operation_tasks_work_only_here(const matters::Matter &matter, const std::string &operation_id) {
	return operation_kept_here(matter, operation_id) != Kept::No;
}

bool origin_unconfirmed_here(const matters::Matter &matter, const std::string &operation_id) {
	return operation_kept_here(matter, operation_id) == Kept::OriginUnconfirmed;
}

bool chat_holds_work_kept_here(const matters::Matter &matter, const std::string &chat_id) {
	std::optional<tools::SeenOperations> seen;
	for (const matters::MatterTask &task : matter.tasks) {
		if (task.chat_id != std::optional<std::string>(chat_id)) continue;
		// What keeps a chat here is an agent's work kept here - its runs, its
		// answers where it was named, its notes. A turn kept here only because
		// this computer could not confirm who began the work it carries on stays
		// here itself (operation_kept_here) and lifts once that computer is
		// confirmed; it does not hold its whole chat.
		const bool isAgentWork = task.agent_id.empty() == false || task.agent_initiated == true
			|| task.operation_type == "agent_message";
		if (isAgentWork == false) continue;
		if (states_kept_here(task)) return true;
		if (!is_operation(task)) continue;
		// The Matter's mark removed after this computer saw it, or this computer's
		// own record of the operation, still counts.
		if (!seen) seen = see_operations(matter);
		if (kept_here(matter, task, *seen) == Kept::Asked) return true;
	}
	return false;
}

} // namespace singularity::chat
