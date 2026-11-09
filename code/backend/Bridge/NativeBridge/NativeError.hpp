#pragma once

#include <exception>
#include <filesystem>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>

#include "Agent/Types/Agent.hpp"
#include "Billing/TimeEntries/TimeEntries.hpp"
#include "Context/PromptAssembler/PromptAssembler.hpp"
#include "Correspondence/CorrespondenceActions/CorrespondenceActions.hpp"
#include "DocumentReview/DocumentReviewActions/DocumentReviewActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/LegalMachinesAccount/LegalMachinesAccount.hpp"
#include "Persistence/ProjectStore/ProjectStore.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/DiskSpace/DiskSpace.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Tool/Types/ToolTypes.hpp"

#include <nlohmann/json.hpp>

namespace singularity::bridge {

// A failure as the interface is given it - the one place an exception becomes
// what the lawyer can be shown.
//
// `code` is stable and never empty: the interface says a failure by its code,
// never by guessing at words. `userMessage` is always a sentence written for a
// lawyer - what happened and what to do next - with no path, identifier,
// service reply or exception text in it. `recorded` is what Singularity or a
// service said about the failure, word for word; it travels as the envelope's
// `message` and is shown only under a closed "Details".
struct NativeError {
	std::string code;
	std::string userMessage;
	std::string recorded;
	bool isRetryable = false;
	std::optional<std::string> degradedCapability;
};

// The plain words for a code, for a failure whose own words were not written
// for a lawyer. Every sentence says three things: what happened, what is safe
// (nothing was sent, nothing was written, everything already saved is kept),
// and the one thing to do next. Every code the bridge itself reports has its
// sentence here; a code it does not know falls back to the sentence of a
// failure Singularity could not name, so no code can ever leave the lawyer with
// nothing to read. The page keeps the same words for a failure that reaches it
// with its code alone (App/services/presentError.js).
inline std::string plainWordsFor(const std::string& code) {

	if (code == "provider_authentication")
		return "The online service did not accept Singularity's key. Everything already saved is kept. Reconnect the service in Models, then try again.";
	if (code == "provider_rate_limit")
		return "The online service asked Singularity to wait. Everything already saved is kept. Try again in a moment.";
	if (code == "network_error")
		return "Singularity could not reach the online service. Everything already saved is kept. Check the internet connection, then try again.";
	if (code == "unsupported_model")
		return "This model cannot take this kind of request. Everything already saved is kept. Choose another model, then try again.";
	if (code == "provider_rejected_request")
		return "The online service declined this request. Everything already saved is kept. Choose another model, then try again.";
	if (code == "privacy_protection_unavailable")
		return "Papinian Guard was not ready to protect this request, so nothing unprotected left this computer. Try again once protection is ready.";
	if (code == "protected_payload_leak_detected")
		return "Singularity could not hide one protected detail of this request, so nothing was sent. Your chat and documents are unchanged. Send it again, or choose a model on this computer.";
	if (code == "protected_response_invalid")
		return "Singularity could not put the protected names back into the answer, so it was set aside and nothing was written from it. Try again.";
	if (code == "response_incomplete") return "The model's answer came back incomplete, so nothing was written from it. Try again.";
	if (code == "context_capacity_exceeded")
		return "This request was more than the model can hold, so nothing was sent to the model. Choose fewer documents, or a model with more room, then send again.";
	if (code == "target_changed")
		return "The document changed before this change was written, so nothing was written. Prepare it again for the current version.";
	if (code == "evidence_changed")
		return "A document this change was prepared from has changed, so nothing was written. Prepare it again for the current version.";
	if (code == "change_rejected") return "The change was rejected, and nothing was written.";
	if (code == "unknown") return "Singularity could not finish this request. Everything already saved is kept. Try again in a moment.";

	if (code == "project_data_unavailable")
		return "Singularity cannot reach this Project's folder, so nothing was changed. Make sure the folder is where it was and Singularity may open it, then try again.";
	if (code == "project_data_damaged")
		return "This Project's Singularity data is damaged, so nothing was changed. The documents in the folder are untouched. Restore the folder's Singularity data from a backup, then try again.";
	if (code == "project_data_newer_version")
		return "This Project was saved by a newer version of Singularity. Its data is fine. Update Singularity to work in it.";
	if (code == "storage.disk_full")
		return "The disk is full, so Singularity could not save this. Everything saved before is kept. Free some space on this computer, then try again.";
	if (code == "file_unavailable")
		return "Singularity could not reach a file it needed. Everything already saved is kept. Make sure the Project folder is where it was, then try again.";

	if (code == "target_changed_on_disk")
		return "The document changed on disk after this change was prepared, so nothing was written. Prepare it again for the current version.";
	if (code == "target_open_elsewhere") return "The document is open in another program, so nothing was written. Close it there, then try again.";
	if (code == "target_read_only") return "The document is locked or read-only, so nothing was written. Unlock it, then try again.";
	if (code == "target_name_taken") return "A file with that name is in the Project now, so nothing was written. Prepare it again under a name of its own.";

	if (code == "account.invalid_input") return "Check what was typed in the sign-in, then try again.";
	if (code == "account.wrong_credentials")
		return "The Organisation ID, Seat ID or password is not right. Check them and try again.";
	if (code == "account.wrong_code")
		return "That code was not accepted. Check it and try again, or sign in again for a new one.";
	if (code == "account.too_many_attempts")
		return "Legal Machines paused signing in to this seat after several tries. Wait a few minutes, then try again.";
	if (code == "account.service_unavailable")
		return "Legal Machines is not answering sign-ins right now. Your work in Singularity is unaffected. Try again in a few minutes.";
	if (code == "account.offline")
		return "Singularity could not reach Legal Machines. Check the internet connection, then try again.";
	if (code == "account.refused") return "Legal Machines did not accept this sign-in. Try again in a few minutes.";
	if (code == "account.two_step_setup_required")
		return "Your firm's sign-in needs two-step sign-in set up first. Nothing was sent. Set it up, then try again.";
	if (code == "account.two_step_code_required")
		return "The code from the email was accepted, and your password is not changed yet. Enter the code your authenticator app shows, or a recovery code.";
	if (code == "account.code_required")
		return "Legal Machines needs a new sign-in code before this computer can download the protection model. "
			"Sign in again, and enter the code it sends.";
	if (code == "account.awaiting_approval")
		return "Legal Machines has not approved your organisation yet. It reviews each new organisation and writes to "
			"its contact address once it is approved; then sign in again.";
	if (code == "account.organisation_not_active")
		return "Your organisation's email is not confirmed yet. Open the message Legal Machines sent to it, confirm it, "
			"then sign in again.";

	if (code == "native.out_of_memory")
		return "This computer ran out of memory for this. Everything already saved is kept. Close some other programs, then try again.";
	if (code == "native.method_not_found" || code == "native.invalid_request" || code == "native.data_invalid")
		return "Singularity could not do this, and nothing was changed. Try again; if it happens again, restart Singularity.";

	// native.handler_failed and any code nobody named: Project state and
	// documents are replaced whole or not at all (a staged file, then a
	// rename), so a failure part-way never takes back what was already saved.
	return "Singularity could not finish this. Everything already saved is kept. Try again in a moment.";

}

// A failure the bridge names itself, from its code and what was recorded.
inline NativeError nativeErrorFor(const std::string& code, const std::string& recorded, bool isRetryable = false) {

	NativeError error;
	error.code = code;
	error.userMessage = plainWordsFor(code);
	error.recorded = recorded;
	error.isRetryable = isRetryable;
	return error;

}

// A failure whose own words were written for the lawyer by the domain that
// raised it (a refused approval, a Project that cannot take a change, a full
// disk, a document Word holds open): those words lead, the code's own sentence
// stands in only when there are none.
inline NativeError nativeErrorInOwnWords(const std::string& code, const std::string& said, bool isRetryable = false) {

	NativeError error = nativeErrorFor(code, said, isRetryable);
	if (said.empty() == false) error.userMessage = said;
	return error;

}

// What a lawyer can do about a Project whose data cannot be used, by its code.
inline std::string projectDataNextStep(const std::string& code) {
	if (code == "project_data_damaged") return "Restore the folder's Singularity data from a backup, then try again.";
	return "Make sure the folder is where it was and Singularity may open it, then try again.";
}

// A Project whose data cannot be used (matters::ProjectDataError), said to the
// lawyer. The Project domain words each refusal for the action it refused
// ("so nothing was removed", "Update Singularity to work in it"), and those words
// lead - with two exceptions. Words that name the ".lm" folder name a file
// format the lawyer never meets, so the code's own sentence stands in for
// them. And words of a single sentence say what happened and what is safe but
// not what to do, so the code's next step follows them.
inline NativeError projectDataErrorOf(const std::string& code, const std::string& said) {

	if (said.empty() == true || said.find(".lm") != std::string::npos) return nativeErrorFor(code, said);
	NativeError error = nativeErrorInOwnWords(code, said);
	const std::size_t end = said.find_last_not_of(" \t\n");
	const std::string sentence = said.substr(0, end == std::string::npos ? 0 : end + 1);
	const bool oneSentence = sentence.find(". ") == std::string::npos && sentence.find("? ") == std::string::npos
		&& sentence.find("! ") == std::string::npos;
	if (oneSentence == true) error.userMessage = sentence + " " + projectDataNextStep(code);
	return error;

}

// What a caught exception is, as the interface is told it. Typed failures are
// named by the code their type or their own code() carries; any other
// exception is named by its kind, and its words - which may be a path, JSON or
// a program's own vocabulary - are kept only as what was recorded.
inline NativeError nativeErrorOf(std::exception_ptr failure) {

	try {
		std::rethrow_exception(failure);
	} catch (const tools::ApprovalRefused& error) {
		return nativeErrorInOwnWords(error.code(), error.what());
	} catch (const agents::AgentRefused& error) {
		return nativeErrorInOwnWords("agent.refused", error.what());
	} catch (const document_review::ReviewRefused& error) {
		return nativeErrorInOwnWords("review.refused", error.what());
	} catch (const correspondence::CorrespondenceRefused& error) {
		return nativeErrorInOwnWords("correspondence.refused", error.what());
	} catch (const billing::TimeRefused& error) {
		return nativeErrorInOwnWords("time.refused", error.what());
	} catch (const node::AccountFailure& error) {
		// Written for the person signing in, and never holding what they typed.
		return nativeErrorInOwnWords(error.code(), error.what(),
			error.getKind() == node::AccountFailureKind::Offline
				|| error.getKind() == node::AccountFailureKind::ServiceUnavailable);
	} catch (const matters::ProjectDataError& error) {
		return projectDataErrorOf(error.code(), error.what());
	} catch (const platform::DiskFull& error) {
		// Nothing was saved and nothing was lost: once there is room, the same
		// action works.
		return nativeErrorInOwnWords("storage.disk_full", error.what(), true);
	} catch (const platform::DocumentWriteConflict& error) {
		return nativeErrorInOwnWords(platform::to_string(error.kind()), error.what());
	} catch (const persistence::ProjectDataAccessDenied& error) {
		// Its words name the path it could not read.
		return nativeErrorFor("project_data_unavailable", error.what());
	} catch (const persistence::ProjectDataRequiresNewerVersion& error) {
		return nativeErrorInOwnWords("project_data_newer_version", error.what());
	} catch (const providers::TaskError& error) {
		// A provider's failure carries the service's own reply, which is never
		// the lawyer's to read as it came.
		NativeError named = nativeErrorFor(providers::to_string(error.code()), error.what(),
			providers::is_retryable(error.code()));
		if (error.code() == providers::TaskErrorCode::PrivacyProtectionUnavailable)
			named.degradedCapability = "protected_external_inference";
		return named;
	} catch (const context::ContextCapacityExceeded& error) {
		return nativeErrorFor("context_capacity_exceeded", error.what());
	} catch (const std::filesystem::filesystem_error& error) {
		return nativeErrorFor("file_unavailable", error.what());
	} catch (const nlohmann::json::exception& error) {
		return nativeErrorFor("native.data_invalid", error.what());
	} catch (const std::bad_alloc& error) {
		return nativeErrorFor("native.out_of_memory", error.what());
	} catch (const std::invalid_argument& error) {
		return nativeErrorFor("native.invalid_request", error.what());
	} catch (const std::exception& error) {
		return nativeErrorFor("native.handler_failed", error.what());
	} catch (...) {
		return nativeErrorFor("native.handler_failed", "Unknown native error");
	}

}

// A failure an operation recorded rather than threw - a chat request whose
// provider refused it ends Failed with the code on its task, and a change
// whose document moved on ends in a Conflict - named exactly as a thrown one
// would be: by the code it recorded, in that code's plain words, with what it
// recorded kept word for word for Details. A conflict that recorded no code is
// a document that changed; a failure that recorded none is one Singularity could
// not name.
inline NativeError recordedFailureOf(const std::optional<providers::TaskErrorCode>& recordedCode, bool isConflict,
	const std::string& recorded) {

	const providers::TaskErrorCode code = recordedCode.has_value() == true ? *recordedCode
		: isConflict == true ? providers::TaskErrorCode::TargetChanged
		: providers::TaskErrorCode::Unknown;
	NativeError error = nativeErrorFor(providers::to_string(code), recorded, providers::is_retryable(code));
	if (code == providers::TaskErrorCode::PrivacyProtectionUnavailable)
		error.degradedCapability = "protected_external_inference";
	return error;

}

// The NativeError envelope (docs/contracts/native/Schemas.json).
inline nlohmann::json nativeErrorJson(const NativeError& error) {
	return nlohmann::json{
		{"code", error.code},
		{"message", error.recorded},
		{"userMessage", error.userMessage},
		{"retryable", error.isRetryable},
		{"degradedCapability", error.degradedCapability ? nlohmann::json(*error.degradedCapability) : nlohmann::json(nullptr)},
	};
}

} // namespace singularity::bridge
