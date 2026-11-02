#include "Chat/MessageProvenance/MessageProvenance.hpp"

#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Tool/DeviceTrust/DeviceTrust.hpp"

#include <nlohmann/json.hpp>

namespace singularity::chat {
namespace {

// What the signature covers: everything that makes the message the
// instruction it is. The record's kind and the Matter are inside it, so a
// signature copied onto another message, another thread or another Matter
// does not verify.
std::string signed_bytes(const std::string &project_id, const matters::MatterTask &message) {
	nlohmann::json covered{{"projectId", project_id}, {"messageId", message.id},
		{"chatId", message.chat_id.value_or(std::string())}, {"createdAt", message.created_at},
		{"authorActorId", message.author_actor_id}, {"authorName", message.author_name}, {"text", message.prompt},
		{"mentions", message.mentions}};
	// What else makes the message the one it is - the message it replies to, the
	// documents it hands over, its edit or deletion, what it did not check, the
	// answers it offers or gives, a thread's closing or reopening, the place in a
	// document it is about - covered only
	// where it has them, so a message signed before they were kept still verifies.
	if (!message.quote_message_id.empty()) covered["quoteMessageId"] = message.quote_message_id;
	if (!message.attachment_document_ids.empty()) covered["attachmentDocumentIds"] = message.attachment_document_ids;
	if (!message.edited_at.empty()) covered["editedAt"] = message.edited_at;
	if (!message.deleted_at.empty()) covered["deletedAt"] = message.deleted_at;
	if (!message.not_checked.empty()) covered["notChecked"] = message.not_checked;
	if (!message.ask_options.empty()) covered["askOptions"] = message.ask_options;
	if (!message.answer_option.empty()) covered["answerOption"] = message.answer_option;
	if (!message.thread_event.empty()) covered["threadEvent"] = message.thread_event;
	if (!message.place.empty()) {
		covered["place"] = nlohmann::json{{"documentId", message.place.document_id}, {"section", message.place.section},
			{"heading", message.place.heading}, {"quote", message.place.quote}};
	}
	return "singularity-signed-record-v1\nmember_message\n" + covered.dump();
}

} // namespace

const char *to_string(MessageProvenance provenance) {
	switch (provenance) {
		case MessageProvenance::ThisComputer: return "this_computer";
		case MessageProvenance::Confirmed: return "confirmed";
		case MessageProvenance::Unconfirmed: return "unconfirmed";
		case MessageProvenance::Revoked: return "revoked";
		case MessageProvenance::Unverified: return "unverified";
		case MessageProvenance::Unsigned: return "unsigned";
	}
	return "unverified";
}

MessageProvenance signed_record_provenance(const std::string &actor_id, const std::string &public_key,
		const std::string &signature, const std::string &signed_bytes) {
	if (signature.empty() || public_key.empty()) return MessageProvenance::Unsigned;
	if (!platform::verify_device_signature(public_key, signed_bytes, signature)) return MessageProvenance::Unverified;
	switch (tools::device_trust(actor_id, public_key)) {
		case tools::DeviceTrustState::ThisComputer: return MessageProvenance::ThisComputer;
		case tools::DeviceTrustState::Confirmed: return MessageProvenance::Confirmed;
		case tools::DeviceTrustState::Revoked: return MessageProvenance::Revoked;
		case tools::DeviceTrustState::Unconfirmed: return MessageProvenance::Unconfirmed;
	}
	return MessageProvenance::Unverified;
}

void sign_member_message(const std::string &project_id, matters::MatterTask &message) {
	message.signer_public_key = platform::device_public_key();
	message.signature = platform::sign_with_device_key(signed_bytes(project_id, message));
}

MessageProvenance member_message_provenance(const std::string &project_id, const matters::MatterTask &message) {
	return signed_record_provenance(message.author_actor_id, message.signer_public_key, message.signature,
		signed_bytes(project_id, message));
}

bool counts_as_authors(const std::string &project_id, const matters::MatterTask &message) {
	const MessageProvenance provenance = member_message_provenance(project_id, message);
	return provenance == MessageProvenance::ThisComputer || provenance == MessageProvenance::Confirmed;
}

} // namespace singularity::chat
