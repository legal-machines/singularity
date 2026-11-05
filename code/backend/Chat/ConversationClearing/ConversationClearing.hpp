#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

// A personal conversation cleared by the member at this computer (the owner,
// 2026-11-04: "a way to clear the messages of a direct conversation"): the
// messages up to that moment leave the member's view here, and none of them
// goes into an AI colleague's request made from here any more, so the
// colleague answers as from a conversation begun afresh. The other member's
// messages stay with them; the conversation's own record, shared with their
// computers, is not changed. Kept on this computer, beside its other records
// ("Cleared conversations.json"), by chat id.
namespace singularity::chat::conversation_clearing {

// When each chat was last cleared here (ISO 8601, UTC).
std::map<std::string, std::string> all();

std::optional<std::string> cleared_before(const std::string &chat_id);

// Clears it as of `now`; returns when it was cleared before, if ever, for an
// Undo.
std::optional<std::string> clear(const std::string &chat_id, const std::string &now);

// Puts back what an Undo gives: when it was cleared before, or never.
void restore(const std::string &chat_id, const std::optional<std::string> &cleared_before);

// Where they are kept; the tests' own folder when they give one.
void use_records_file_for_testing(const std::filesystem::path &file);

// This computer's own record of them, beside its other records
// (ConversationClearingRecords.cpp).
std::filesystem::path default_records_file();

} // namespace singularity::chat::conversation_clearing
