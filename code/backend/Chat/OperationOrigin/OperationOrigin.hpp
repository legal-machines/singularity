#pragma once

#include "Chat/MessageProvenance/MessageProvenance.hpp"
#include "Matter/Types/Matter.hpp"
#include "Tool/OperationAuthority/OperationAuthority.hpp"

#include <string>

namespace singularity::chat {

/*
 * Who began an operation, and what it may do, wherever its Matter goes.
 *
 * The computer that begins an operation keeps its authority on its own disk
 * (Tool/OperationAuthority) and signs the same facts into the Matter with its
 * key (Platform/DeviceKey): who began it - a person or an agent - the most it
 * may do without asking, and whether it works only on that computer.
 *
 * A Matter folder may be shared, and anyone who can write it can rewrite or
 * remove that statement. So a computer carrying an operation on - Retry, Run
 * with another model, Edit and continue, a follow-up - takes every restriction
 * the Matter states for it, signed or not, and takes the ABSENCE of one only
 * from a statement signed by itself or by a computer its person confirmed as
 * the operation's person's (Tool/DeviceTrust). Anything else stays on the
 * computer carrying it on: local models, no network. What this computer has
 * once seen kept here stays kept here (OperationAuthority's SeenOperations).
 * Shared state can add a restriction; it never lifts one.
 *
 * Operations a Matter already held, unsigned, the first time this computer
 * looked at it with a version that signs - begun before origins were signed -
 * are taken as they stood then: their own marks, never a signature they never
 * had.
 */

// Signs `task`'s origin - `authority` as this computer recorded it - into the
// task. Its id, creation time, operation and Chat are set before.
void sign_operation_origin(const std::string &project_id, matters::MatterTask &task,
	const tools::OperationAuthority &authority);

// Whose the operation `task` records is, as this computer can tell: its
// statement signed by this computer, or by one confirmed as its person's.
MessageProvenance operation_origin_provenance(const std::string &project_id, const matters::MatterTask &task);

// Whether the work `task` records, carried on from this computer, must stay
// on it.
bool task_works_only_here(const matters::Matter &matter, const matters::MatterTask &task);

// Whether `operation_id`, carried on from this computer, must stay on it:
// this computer recorded it so, or any of its tasks must.
bool operation_tasks_work_only_here(const matters::Matter &matter, const std::string &operation_id);

// Whether `operation_id` stays here only because this computer cannot confirm
// who began it - its origin is unsigned, changed, or signed by a computer not
// confirmed here - rather than because anyone asked for it.
bool origin_unconfirmed_here(const matters::Matter &matter, const std::string &operation_id);

// Whether the chat holds an agent's work kept on this computer: the runs and
// answers of an agent that works only here, or what it said in the chat. Every
// later request of the chat carries its turns as history, so such a chat
// stays here too. What counts is what the Matter states and what this computer
// recorded, so the shared folder can add the restriction, never lift it.
bool chat_holds_work_kept_here(const matters::Matter &matter, const std::string &chat_id);

} // namespace singularity::chat
