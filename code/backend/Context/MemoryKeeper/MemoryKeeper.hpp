#pragma once

#include "PapinianGuard/Types/PrivacyMetadata.hpp"
#include "Provider/Types/UsageMetadata.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The Project's memory has one keeper.
//
// What a Project remembers is one living brief (the Project Context item named
// Memory - Context/ContextCuration). This keeper alone rewrites it, through the
// model the lawyer assigned to keep it: a model on this computer, or a
// provider's model reached through Papinian Guard like every other request
// (names and other protected values replaced before anything leaves this Mac).
// It folds in three things, as they happen:
//
//   - what a chat arrived at: every answered request, after it is saved;
//   - what the Project's documents say: each document is read once into a
//     digest of a few lines (kept with the fingerprint it was read at, so only
//     a changed document is read again), and the digests are folded into the
//     brief together;
//   - what the lawyer tells it directly, which it also answers.
//
// All three go through one queue and one lock: two rewrites never race, and an
// answer never waits for its memory. With no model to keep it there is no
// keeper and the memory does not move - which status() says.
//
// What it costs is counted: every piece of work leaves one operation record
// (Usage/LocalWork) - local work, its model and its tokens, never its content.
// And it paces itself: how much of a document it reads is set by how fast this
// computer's model actually read the last one, so a pass over a data room is
// minutes on any machine rather than hours on a small one.
namespace singularity::memory_keeper {

// Which model keeps the memory, and where it runs: "local" or "external".
struct Keeper {
	std::string provider_id;
	std::string model_id;
	std::string execution_kind;
};

struct Reply {
	std::string reply;
	bool memory_changed = false;
	std::string model_id;
	std::string execution_kind;
};

struct Status {
	// Whether the keeper is running: it is started once Singularity has finished
	// getting ready, and until then no model - chosen or not - is asked.
	bool ready = false;
	bool model_available = false;
	std::string model_id;
	std::string execution_kind;
	bool is_updating = false;
	// What is being folded in right now: "chat", "documents", "message" or "".
	std::string source;
	std::size_t documents_done = 0;
	std::size_t documents_total = 0;
	// How many of the Project's readable documents the memory has read as they are now.
	std::size_t documents_read = 0;
	std::size_t documents_readable = 0;
	// Why the last update did not happen - or, with no model to keep the memory,
	// why it cannot move now - in words a person can act on; empty when it did.
	std::string last_error;
};

struct Generated {
	std::string text;
	long long input_tokens = 0;
	long long output_tokens = 0;
	// What the provider reported for the call, its price included when it gave
	// one - so a provider's model keeping the memory is counted at what it cost.
	providers::UsageMetadata usage;
	// How Papinian Guard protected the call, as it reported it - so Usage says a
	// provider's model kept the memory under protection, not with none.
	bool protected_api_applied = false;
	protected_api::papinian::PrivacyMetadata privacy_metadata;
};

// What the keeper needs from the application, which alone is linked against
// the local runtime and the document store. A test supplies its own.
struct Dependencies {
	// Which model keeps this Project's memory; nullopt when there is none.
	std::function<std::optional<Keeper>(const std::string &project_id)> choose_model;
	// One generation on that model: its text, and what it took. No ceiling is
	// put on the answer - the keeper asks for a whole brief and gets one -
	// which the application resolves as no max_tokens for a provider's model
	// and the window's own room for a local one. Throws when it cannot be had.
	std::function<Generated(const std::string &project_id, const Keeper &keeper,
		const std::string &prompt)> generate;
	// The readable text of one Project document, from its beginning, at most
	// `maximum_bytes` of it; empty when it cannot be read.
	std::function<std::string(const std::string &project_id, const std::string &document_id,
		std::size_t maximum_bytes)> read_document;
};

// Starts the keeper's worker. Calling it again replaces the dependencies.
void start(Dependencies dependencies);
// Stops taking work. A generation in flight is left to end on its own and its
// result is dropped; nothing is written once this has been called.
void stop() noexcept;

// An answered request: folded into the memory in the background.
void chat_answered(const std::string &project_id, const std::string &task_id);
// The Project's documents: those not read yet as they are now - or every one,
// when `everything` - are read in the background and folded in.
void read_documents(const std::string &project_id, bool everything);
// What the lawyer says to the memory. Answered in the caller's thread, in turn
// with whatever the worker is doing. Throws when there is no model to keep it.
Reply tell(const std::string &project_id, const std::string &message);

Status status(const std::string &project_id);

// ---- Pure parts, exposed so they are proved without a model -------------------

struct ParsedReply {
	std::string reply;
	std::optional<std::string> brief;
};
// Lenient by design: a small model may think aloud first or wrap its JSON in
// prose; whatever cannot be read as the expected object is simply the answer.
[[nodiscard]] ParsedReply parse_reply(const std::string &model_text);
// A document's digest as the model gave it: thinking dropped, bounded, empty
// when there is nothing in it.
[[nodiscard]] std::string clean_digest(const std::string &model_text);
// `text` cut to at most `maximum_bytes`, never inside a UTF-8 sequence.
[[nodiscard]] std::string bounded_text(const std::string &text, std::size_t maximum_bytes);
// How much of the next document to read, from how long the last one took:
// what this computer's model reads in about half a minute, within fixed bounds.
[[nodiscard]] std::size_t next_document_bytes(std::size_t bytes_read, double seconds_taken);

enum class Material { Chat, Documents, Message };
// The one prompt every rewrite is asked with; `material` is what is new, and
// `identity_register` is how this Project itself writes the names in it - the
// spellings the brief must use, so a party is never named by a role word or by
// a form of its name the keeper made up.
[[nodiscard]] std::string rewrite_prompt(const std::string &current_brief, const std::string &other_entries,
	const std::string &identity_register, Material kind, const std::string &material);
[[nodiscard]] std::string digest_prompt(const std::string &document_name, const std::string &document_text,
	const std::string &identity_register);

} // namespace singularity::memory_keeper
