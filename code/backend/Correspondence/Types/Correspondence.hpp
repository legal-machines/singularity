#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// A Matter's correspondence: the letters filed to it - incoming and
// outgoing email, kept apart from the Matter's internal threads - each with
// the provenance of its filing. Kept in the Matter's own data
// (MatterRepository, the "correspondence" collection): one letter is one
// object, written whole.
namespace singularity::correspondence {

struct Party {
	std::string name;
	std::string address;
};

// A rule of the Matter's correspondence intake: a letter that reaches `mailbox`
// from `from` is the Matter's, filed by the rule as Outlook shows it. Both are
// addresses as LetterRouting compares them: on one line, in lower case. Kept in
// the Matter's own data, so every computer of it files alike.
struct IntakeRule {
	std::string mailbox;
	std::string from;
};

inline nlohmann::json intake_rule_to_json(const IntakeRule &rule) {
	return nlohmann::json{{"mailbox", rule.mailbox}, {"from", rule.from}};
}

inline IntakeRule intake_rule_from_json(const nlohmann::json &value) {
	IntakeRule rule;
	if (!value.is_object()) return rule;
	if (value.contains("mailbox") && value.at("mailbox").is_string()) rule.mailbox = value.at("mailbox").get<std::string>();
	if (value.contains("from") && value.at("from").is_string()) rule.from = value.at("from").get<std::string>();
	return rule;
}

struct LetterAttachment {
	std::string name;
	std::string content_type;
	long long size = 0;
	// The mail provider's id for it in the mailbox it was filed from.
	std::string provider_id;
	// Once saved to the Matter: its bytes' SHA-256, and the file in the
	// Matter's folder that holds them - one the Matter already had when the
	// same bytes were there before.
	std::string sha256;
	std::string relative_path;
	bool was_already_there = false;
};

// Who filed a letter to its Matter, when, from where, and on what evidence.
struct Filing {
	// "person": a member's own decision; "rule": a rule of the Matter's intake
	// matched it as Outlook showed it.
	std::string by = "person";
	std::string actor_id;
	std::string actor_name;
	std::string at;
	// Where it was filed from: "outlook".
	std::string from;
	// What the member saw that made it this Matter, in plain sentences.
	std::vector<std::string> evidence;
};

struct Letter {
	std::string id;
	// "incoming" or "outgoing", as the mailbox it was filed from saw it.
	std::string direction;
	std::string provider = "outlook";
	// The message's identities: its Internet Message-ID, the same in every
	// mailbox; the provider's conversation id, shared by the letters of one
	// conversation; and the mailbox it was filed from.
	std::string internet_message_id;
	std::string conversation_id;
	std::string mailbox;
	std::string subject;
	Party from;
	std::vector<Party> to;
	std::vector<Party> cc;
	std::string sent_at;
	std::string text;
	std::vector<LetterAttachment> attachments;
	Filing filing;
	std::string updated_at;
};

namespace json_detail {

inline std::string text_of(const nlohmann::json &value, const char *key) {
	return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>() : std::string();
}

inline nlohmann::json party_to_json(const Party &party) {
	return nlohmann::json{{"name", party.name}, {"address", party.address}};
}

inline Party party_from_json(const nlohmann::json &value) {
	Party party;
	if (!value.is_object()) return party;
	party.name = text_of(value, "name");
	party.address = text_of(value, "address");
	return party;
}

inline nlohmann::json parties_to_json(const std::vector<Party> &parties) {
	nlohmann::json list = nlohmann::json::array();
	for (const Party &party : parties) list.push_back(party_to_json(party));
	return list;
}

inline std::vector<Party> parties_from_json(const nlohmann::json &value, const char *key) {
	std::vector<Party> parties;
	if (!value.contains(key) || !value.at(key).is_array()) return parties;
	for (const nlohmann::json &entry : value.at(key)) parties.push_back(party_from_json(entry));
	return parties;
}

} // namespace json_detail

inline nlohmann::json attachment_to_json(const LetterAttachment &attachment) {
	return nlohmann::json{{"name", attachment.name}, {"contentType", attachment.content_type}, {"size", attachment.size},
		{"providerId", attachment.provider_id}, {"sha256", attachment.sha256}, {"relativePath", attachment.relative_path},
		{"wasAlreadyThere", attachment.was_already_there}};
}

inline LetterAttachment attachment_from_json(const nlohmann::json &value) {
	using json_detail::text_of;
	LetterAttachment attachment;
	if (!value.is_object()) return attachment;
	attachment.name = text_of(value, "name");
	attachment.content_type = text_of(value, "contentType");
	attachment.size = value.contains("size") && value.at("size").is_number_integer() ? value.at("size").get<long long>() : 0;
	attachment.provider_id = text_of(value, "providerId");
	attachment.sha256 = text_of(value, "sha256");
	attachment.relative_path = text_of(value, "relativePath");
	attachment.was_already_there = value.contains("wasAlreadyThere") && value.at("wasAlreadyThere").is_boolean()
		&& value.at("wasAlreadyThere").get<bool>();
	return attachment;
}

inline nlohmann::json letter_to_json(const Letter &letter) {
	using json_detail::parties_to_json;
	using json_detail::party_to_json;
	nlohmann::json attachments = nlohmann::json::array();
	for (const LetterAttachment &attachment : letter.attachments) attachments.push_back(attachment_to_json(attachment));
	return nlohmann::json{{"id", letter.id}, {"direction", letter.direction}, {"provider", letter.provider},
		{"internetMessageId", letter.internet_message_id}, {"conversationId", letter.conversation_id},
		{"mailbox", letter.mailbox}, {"subject", letter.subject}, {"from", party_to_json(letter.from)},
		{"to", parties_to_json(letter.to)}, {"cc", parties_to_json(letter.cc)}, {"sentAt", letter.sent_at},
		{"text", letter.text}, {"attachments", attachments},
		{"filing", nlohmann::json{{"by", letter.filing.by}, {"actorId", letter.filing.actor_id},
			{"actorName", letter.filing.actor_name}, {"at", letter.filing.at}, {"from", letter.filing.from},
			{"evidence", letter.filing.evidence}}},
		{"updatedAt", letter.updated_at}};
}

inline Letter letter_from_json(const nlohmann::json &value) {
	using json_detail::parties_from_json;
	using json_detail::party_from_json;
	using json_detail::text_of;
	Letter letter;
	if (!value.is_object()) return letter;
	letter.id = text_of(value, "id");
	letter.direction = text_of(value, "direction");
	letter.provider = text_of(value, "provider");
	letter.internet_message_id = text_of(value, "internetMessageId");
	letter.conversation_id = text_of(value, "conversationId");
	letter.mailbox = text_of(value, "mailbox");
	letter.subject = text_of(value, "subject");
	if (value.contains("from")) letter.from = party_from_json(value.at("from"));
	letter.to = parties_from_json(value, "to");
	letter.cc = parties_from_json(value, "cc");
	letter.sent_at = text_of(value, "sentAt");
	letter.text = text_of(value, "text");
	if (value.contains("attachments") && value.at("attachments").is_array()) {
		for (const nlohmann::json &entry : value.at("attachments")) letter.attachments.push_back(attachment_from_json(entry));
	}
	if (value.contains("filing") && value.at("filing").is_object()) {
		const nlohmann::json &filing = value.at("filing");
		letter.filing.by = text_of(filing, "by");
		letter.filing.actor_id = text_of(filing, "actorId");
		letter.filing.actor_name = text_of(filing, "actorName");
		letter.filing.at = text_of(filing, "at");
		letter.filing.from = text_of(filing, "from");
		if (filing.contains("evidence") && filing.at("evidence").is_array()) {
			for (const nlohmann::json &line : filing.at("evidence")) {
				if (line.is_string()) letter.filing.evidence.push_back(line.get<std::string>());
			}
		}
	}
	letter.updated_at = text_of(value, "updatedAt");
	return letter;
}

} // namespace singularity::correspondence
