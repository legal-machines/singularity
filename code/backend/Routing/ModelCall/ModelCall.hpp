#pragma once

// The vocabulary of one model call on behalf of an operation: whom a call is
// for, the form of reply it accepts, and the one class each failure has.
// Neutral: no colleague, thread, role or policy type - a workflow resolves
// those before it calls. Pure functions only; nothing here sends, records or
// decides what a reply means.

#include "Provider/Types/TaskError.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace singularity::inference {

// The operation a call is for: the key of its authority record and of its
// usage, made by the workflow and kept across its calls, its retries and its
// resumption.
struct OperationRef {
	std::string matter_id;
	std::string operation_id;
};

// One call the operation asks for, stable across the call's attempts.
struct CallIdentity {
	OperationRef operation;
	int sequence = 1;
};

// What a call accepts as its reply: any words, one object that satisfies the
// schema, or either - an object, which must then satisfy it, or plain words.
enum class ReplyForm {
	Text,
	Structured,
	StructuredOrText,
};

struct ReplyContract {
	ReplyForm form = ReplyForm::Text;
	// A JSON Schema object, for Structured and StructuredOrText.
	nlohmann::json schema;
};

// Each failure's one class, for every caller (the contract's closed list).
enum class FailureClass {
	// A network error, a timeout, a short busy spell: the provider's own retry
	// is the one more attempt it gets within the call.
	Passing,
	// Busy for longer than the provider waits.
	RateLimited,
	// Credentials, an invalid request, a model not offered, out of credit, or
	// a refusal of the request path's own.
	Refused,
	TooLargeForModel,
	// Sending is not allowed now: the work stays on this computer, or its
	// protection cannot run.
	NotAllowedHere,
	OverSpendAllowance,
	Stopped,
	Closing,
	// A reply that fails its form, or one that could not be read.
	Unusable,
	// An answer cut at its length.
	Incomplete,
};

const char *to_string(FailureClass failure) noexcept;

// A provider's failure classed once. A failure while the call is stopped is
// Stopped, while Singularity closes Closing, whatever the provider said.
FailureClass classify(providers::TaskErrorCode code, bool isStopped, bool isClosing) noexcept;

// Whether the call itself may send its attempt once more: only for a passing
// failure. Whether the work is retried is the workflow's to decide.
bool is_retried_within_the_call(FailureClass failure) noexcept;

// A reply that met its form: its text, and its object when it is one.
struct Reply {
	std::string text;
	std::optional<nlohmann::json> object;
};

// A reply checked: the reply, or why it cannot be used.
struct ReplyCheck {
	std::optional<Reply> reply;
	std::string why_unusable;
};

// The reply checked against the form the call declared. Never interpreted:
// whether words answer the question is the workflow's to decide.
ReplyCheck check_reply(const std::string &text, const ReplyContract &contract);

// Where a value breaks its schema - type, required, properties,
// additionalProperties, items, enum - or empty when it keeps to it.
std::string schema_violation(const nlohmann::json &value, const nlohmann::json &schema);

} // namespace singularity::inference
