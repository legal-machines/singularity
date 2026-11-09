#pragma once

#include "Matter/Types/Matter.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace singularity::playbook_changes {

/*
 * The changes that bring a Matter's documents to the firm's position.
 *
 * From a Review Table checked against a Playbook, each document whose answers
 * depart from the firm's position, or lack a term the firm requires, is given
 * one request: revise this document - the words quoted from it, the firm's
 * position, the clause the firm proposes - as tracked changes in the document
 * itself. The request runs as any Chat request does (Chat/ChatExecution): on
 * the table's model, kept on this computer when the table is, and every change
 * it prepares waits for the lawyer's approval - never written before it, and,
 * once approved, written as Word tracked changes with the previous version
 * kept (Document/DocumentVersionStore).
 */

struct PreparedChange {
	std::string document_id;
	std::string document_name;
	std::string chat_id;
	std::string operation_id;
	// The request's state: "waiting_for_approval" when its change waits for the
	// lawyer, "waiting_for_answer" when it asked the lawyer first, "completed",
	// "failed", "stopped", or "interrupted" when Singularity closed on it.
	std::string state;
	std::string message;
	// The question it asked, in a line, while it waits for the answer.
	std::string question;
	// The questions (columns) whose answers the change addresses.
	std::vector<std::string> column_ids;
};

// The agent preparing the changes, when an agent does (Agent/): each request
// is its operation, the change waits for the lawyer either way.
struct PreparedBy {
	std::string agent_id;
	std::string agent_name;
};

// Prepares the changes for `document_ids` - every document of the table with
// a departure, when empty. Blocking: one request per document, in order. Once
// `cancelled` is set, or the lawyer stops the batch (request_stop), the
// request running then stops as Stop in its chat stops it, and no other
// document is begun; the changes prepared before stay as they are, each
// waiting for the lawyer. Once Singularity is closing nothing more is begun,
// and none begins while it closes. One batch of a table at a time.
std::vector<PreparedChange> prepare(const std::string &project_id, const std::string &table_id,
	const std::vector<std::string> &document_ids = {}, const PreparedBy &by = {},
	const std::atomic<bool> *cancelled = nullptr);

// Stops the changes being prepared from this table on this computer: the
// request running is stopped as Stop in its chat stops it, and no other
// document is begun. Whether a batch was being prepared.
bool request_stop(const std::string &project_id, const std::string &table_id);

// The instruction one document's change is asked with (tests and the CLI).
std::string change_instruction(const matters::ReviewTable &table, const matters::ReviewRow &row,
	const std::string &document_name);

} // namespace singularity::playbook_changes
