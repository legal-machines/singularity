#pragma once

#include "Agent/AgentRunner/AgentRunner.hpp"
#include "Agent/AiAppConnections/AiAppConnections.hpp"

#include <functional>
#include <optional>
#include <string>

namespace singularity::agents {

// What agents need from the rest of Singularity, as the application has it:
//
// - a Matter's folder and documents, from the Matter repository;
// - a file's words, from the same local extractor a Matter's documents use;
// - one analysis, through the one model path every request takes
//   (routing::execute): the model the Matter's AI Setup gives its reasoning,
//   or only a local model; Papinian Guard for anything that leaves this
//   computer; an operation of its own in the Matter, so Usage counts it and
//   "What left this device" shows every call; no tools, no writes;
// - the report, kept in the Matter as Work Product.
RunnerDependencies production_runner_dependencies();

// What the AI apps' requests need: the Matters on this computer, by id and
// name, from the Matter repository.
AiAppDependencies production_ai_app_dependencies();

// The agent is set to work only on this computer (AgentModelPolicy::LocalOnly),
// by this computer's own record of it; false for an agent it does not know.
bool agent_works_only_here(const std::string &agent_id);

// The work a lawyer gives a colleague in a thread (D-091, D-115), carried out
// by a domain this one does not depend on (DocumentReview): the colleague
// understands the request first, then answers in words, asks, or works on the
// Matter's documents. The application installs it when it starts
// (document_review::installForColleagues); a build without it leaves every
// request to the colleague's ordinary answer in its thread.
struct ThreadDocumentWork {
	// Whether this request, in this thread, is that work for this colleague.
	std::function<bool(const std::string &matter_id, const std::string &chat_id, const std::string &agent_id,
		const std::string &instruction)> is_requested;
	// The work, done, as the colleague's run reports it; nothing where the
	// request is one the colleague answers in words, by its ordinary answer in
	// the thread. `brief` is the colleague's brief, in the words its thread
	// carries.
	std::function<std::optional<TaskRunOutput>(const Agent &agent, const TaskRunInput &input, const std::string &chat_id,
		const std::string &brief, TaskRunOutput output)> run;
	// The record of the work this colleague did in the thread, which its
	// ordinary answer is given before it answers a question about that work
	// (D-120); empty where there is none.
	std::function<std::string(const std::string &matter_id, const std::string &chat_id, const std::string &agent_id)> record_of;
	// A run the colleague begins itself - its schedule, a change it noticed,
	// another colleague's report - carried as its talk (D-129): `why` is its
	// standing work and why it runs now. Nothing where it is left to the
	// former answer.
	std::function<std::optional<TaskRunOutput>(const Agent &agent, const TaskRunInput &input, const std::string &chat_id,
		const std::string &brief, const std::string &why, TaskRunOutput output)> run_own;
};
void install_thread_document_work(ThreadDocumentWork work);

} // namespace singularity::agents
