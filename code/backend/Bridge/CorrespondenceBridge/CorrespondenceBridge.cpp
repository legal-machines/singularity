#include "Bridge/CorrespondenceBridge/CorrespondenceBridge.hpp"
#include "Platform/MailDraft/MailDraft.hpp"
#include "Platform/OpenExternal/OpenExternal.hpp"

#include "Correspondence/CorrespondenceActions/CorrespondenceActions.hpp"
#include "Office/OfficePaths/OfficePaths.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace singularity::bridge::correspondence_bridge {
namespace {

using Json = nlohmann::json;
namespace mail = singularity::correspondence;

std::string required_text(const Json &params, const char *name) {
	if (!params.contains(name) || !params.at(name).is_string() || params.at(name).get<std::string>().empty()) {
		throw std::invalid_argument(std::string(name) + " is required.");
	}
	return params.at(name).get<std::string>();
}

std::string text_in(const Json &value, const char *name) {
	if (!value.contains(name) || value.at(name).is_null()) return std::string();
	if (!value.at(name).is_string()) throw std::invalid_argument(std::string(name) + " must be text.");
	return value.at(name).get<std::string>();
}

mail::Party party_in(const Json &value) {
	if (!value.is_object()) throw std::invalid_argument("A sender or recipient is a name and an address.");
	return mail::Party{text_in(value, "name"), text_in(value, "address")};
}

std::vector<mail::Party> parties_in(const Json &value, const char *name) {
	std::vector<mail::Party> parties;
	if (!value.contains(name) || value.at(name).is_null()) return parties;
	if (!value.at(name).is_array()) throw std::invalid_argument(std::string(name) + " must be a list.");
	for (const Json &entry : value.at(name)) parties.push_back(party_in(entry));
	return parties;
}

// A message as a mail client describes it.
mail::MessageFacts message_in(const Json &params) {
	if (!params.contains("message") || !params.at("message").is_object()) throw std::invalid_argument("message is required.");
	const Json &message = params.at("message");
	mail::MessageFacts facts;
	facts.internet_message_id = text_in(message, "internetMessageId");
	facts.conversation_id = text_in(message, "conversationId");
	facts.mailbox = text_in(message, "mailbox");
	facts.subject = text_in(message, "subject");
	if (message.contains("from") && !message.at("from").is_null()) facts.from = party_in(message.at("from"));
	facts.to = parties_in(message, "to");
	facts.cc = parties_in(message, "cc");
	facts.sent_at = text_in(message, "sentAt");
	facts.text = text_in(message, "text");
	if (message.contains("attachments") && !message.at("attachments").is_null()) {
		if (!message.at("attachments").is_array()) throw std::invalid_argument("attachments must be a list.");
		for (const Json &entry : message.at("attachments")) {
			if (!entry.is_object()) throw std::invalid_argument("An attachment is described by its name and id.");
			mail::LetterAttachment attachment;
			attachment.name = text_in(entry, "name");
			attachment.content_type = text_in(entry, "contentType");
			attachment.provider_id = text_in(entry, "providerId");
			if (entry.contains("size") && entry.at("size").is_number_integer()) attachment.size = entry.at("size").get<long long>();
			facts.attachments.push_back(std::move(attachment));
		}
	}
	return facts;
}

Json suggestion_json(const mail::Suggestion &suggestion) {
	Json matters = Json::array();
	for (const mail::MatterEvidence &entry : suggestion.matters) {
		matters.push_back(Json{{"matterId", entry.matter_id}, {"matterName", entry.matter_name}, {"strongest", entry.strongest},
			{"evidence", entry.evidence}, {"letterId", entry.letter_id}});
	}
	return Json{{"outcome", suggestion.outcome}, {"matters", matters}};
}

} // namespace

Json suggest(const Json &params) {
	return suggestion_json(mail::suggest(message_in(params)));
}

Json file(const Json &params) {
	std::vector<std::string> evidence;
	if (params.contains("evidence") && !params.at("evidence").is_null()) {
		if (!params.at("evidence").is_array()) throw std::invalid_argument("evidence must be a list of sentences.");
		for (const Json &line : params.at("evidence")) {
			if (line.is_string()) evidence.push_back(line.get<std::string>());
		}
	}
	const std::string from = params.contains("from") && params.at("from").is_string() ? params.at("from").get<std::string>() : "outlook";
	// Filed by a rule of the Matter's intake, which the Matter checks.
	const bool byRule = params.contains("by") && params.at("by").is_string() && params.at("by").get<std::string>() == "rule";
	return mail::letter_to_json(mail::file_letter(required_text(params, "matterId"), message_in(params), evidence, from, byRule));
}

Json move(const Json &params) {
	return mail::letter_to_json(mail::move_letter(required_text(params, "matterId"), required_text(params, "letterId"),
		required_text(params, "toMatterId")));
}

Json unfile(const Json &params) {
	mail::unfile_letter(required_text(params, "matterId"), required_text(params, "letterId"));
	return Json{{"unfiled", true}};
}

Json list(const Json &params) {
	Json letters = Json::array();
	for (const mail::Letter &letter : mail::letters_of(required_text(params, "matterId"))) letters.push_back(mail::letter_to_json(letter));
	return Json{{"letters", letters}};
}

Json draft_reply(const Json &params) {
	const std::string matterId = required_text(params, "matterId");
	const std::string letterId = required_text(params, "letterId");
	const std::string text = params.contains("text") && params.at("text").is_string() ? params.at("text").get<std::string>() : std::string();
	std::vector<std::string> documentIds;
	if (params.contains("attachmentDocumentIds") && params.at("attachmentDocumentIds").is_array()) {
		for (const Json &id : params.at("attachmentDocumentIds")) {
			if (id.is_string() == false || id.get<std::string>().empty()) throw std::invalid_argument("attachmentDocumentIds must hold document ids.");
			documentIds.push_back(id.get<std::string>());
		}
	}
	const std::vector<mail::Letter> letters = mail::letters_of(matterId);
	const auto letter = std::find_if(letters.begin(), letters.end(), [&letterId](const mail::Letter &item) { return item.id == letterId; });
	if (letter == letters.end()) throw mail::CorrespondenceRefused("This letter is no longer in the Matter.");
	const mail::ReplyDraft draft = mail::reply_draft(*letter, text);
	if (platform::is_plain_mail_address(draft.to) == false)
		throw mail::CorrespondenceRefused("This letter's address can't go into a mail draft. Reply to it from your mail app.");
	// With files, the reply is an unsent email that carries them, opened in
	// the mail app as a new message; a mail link carries no file.
	if (documentIds.empty() == false) {
		const mail::ReplyFile written = mail::write_reply_with_files(matterId, *letter, text, documentIds);
		if (platform::open_mail_draft(draft.to, draft.subject, draft.body, written.files, written.path) == false)
			throw mail::CorrespondenceRefused("Your mail app could not open the draft. It is kept in Singularity's Mail drafts "
				"folder: open your mail app, then try again.");
		return Json{{"opened", true}, {"carriesText", true}};
	}
	const platform::MailComposed composed = platform::open_mail_compose(draft.to, draft.subject, draft.body);
	if (composed.opened == false) throw mail::CorrespondenceRefused("Your mail app could not be opened. Open it, then try again.");
	return Json{{"opened", true}, {"carriesText", composed.carries_body}};
}

Json save_attachment(const Json &params) {
	const auto bytes = office::decode_base64(required_text(params, "base64"));
	if (!bytes) throw std::invalid_argument("The attachment's bytes could not be read.");
	return mail::attachment_to_json(mail::save_attachment(required_text(params, "matterId"), required_text(params, "letterId"),
		required_text(params, "providerId"), *bytes));
}

} // namespace singularity::bridge::correspondence_bridge
