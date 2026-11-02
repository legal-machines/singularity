#pragma once

#include "Matter/Types/Matter.hpp"

#include <string>
#include <map>
#include <filesystem>
#include <optional>
#include <functional>
#include <vector>

namespace singularity::chat {

// The narrow Phase 20 read boundary. It returns Chat identity plus stable
// links to the existing MatterTask records, never a second transcript.
matters::ChatList list(const std::string &project_id);

// `selected_document_ids` is this Chat's real, persisted starting selection
// (Phase 09's "Ask in Chat" - canonical Document IDs only, never validated
// against the Matter here since a since-removed id is still honestly this
// Chat's recorded intent, exactly like MatterTask::selected_document_ids
// already tolerates). See MatterChat::selected_document_ids for exactly
// which consumer proves this today.
matters::Matter create(const std::string& matter_id, std::string title,
                       std::vector<std::string> selected_document_ids = {});

// Product-facing New Chat: fixed initial title, no naming/configuration
// dialog. Returns only the created ProjectChat.
matters::MatterChat create(const std::string &project_id);

// The chat a personal conversation of two members is held in (D-042): the
// first of its project's chats that stands, by when it was begun and then by
// id, so both members' computers read the same one; made when `make` and there
// is none. "" when there is none and none is made. Throws when the project is
// not on this computer.
std::string conversation_chat(const std::string &project_id, bool make);
// Every chat of the conversation's project that stands, the conversation's own
// first: two computers that began it at once each began one, and what either
// member wrote in either is the conversation's.
std::vector<std::string> conversation_chats(const std::string &project_id);

// A user rename records user title authority. Automatic title updates are
// rejected once that authority has been established.
matters::MatterChat rename(const std::string &project_id, const std::string &chat_id,
                           std::string title, long long expected_revision,
                           matters::ChatTitleAuthority authority = matters::ChatTitleAuthority::User);

// A thread archived or restored (D-214): it leaves the Matter's threads for
// its Archived ones, or comes back, for every computer of the Matter; nothing
// in it changes.
matters::MatterChat set_archived(const std::string &project_id, const std::string &chat_id, bool archived);

// A short, readable title for a Chat, derived from its first request: the
// first line, the first sentence when it fits, otherwise cut at a word
// boundary with an ellipsis. Counts characters, not bytes, so Cyrillic and
// other multi-byte text is never cut mid-character. Empty when the request
// has no usable text.
std::string derive_automatic_chat_title(const std::string &prompt);

// Names a Chat that still carries its default "New chat" title after its
// first completed request. Never touches a title a person chose, never
// renames an already automatic title, and never throws: naming is a
// convenience, so a failure leaves the Chat exactly as it was. Returns true
// when the title changed.
bool apply_automatic_title_if_default(const std::string &project_id, const std::string &chat_id,
                                      const std::string &prompt);
// Names a chat nobody and nothing has named yet with a title written for it (by
// a model). Like the function above it never throws and never touches a chat
// that already has a name of any authority: a chat is named once.
bool apply_generated_title_if_default(const std::string &project_id, const std::string &chat_id,
                                      const std::string &title);
// What is kept as a chat's name from a model's reply: the quick local title,
// or the chatTitle an answer gave (with its question, its web search or its
// result). One rule for every way a model names a chat, so a name reads like a
// lawyer's own label for the work:
// - its last plain line, without reasoning, a "Title:" label, quotation or
//   emphasis marks, or a trailing full stop;
// - the subject, not what the chat is ("Conversation about SPA drafts" is
//   "SPA drafts");
// - a word the lawyer wrote with a capital inside it (SPA, NDA, McKinsey) as
//   the lawyer wrote it;
// - a name a model wrote in Title Case written as a sentence, when every
//   capital in it can be accounted for by the lawyer's own message; when one
//   cannot, it may belong to a name, and the capitals stay as written;
// - a few words (nine at most, 80 characters at most) opening with a capital,
//   in any script.
// "" when the reply holds no name.
[[nodiscard]] std::string chat_title_from_reply(const std::string &reply, const std::string &first_message = {});

// Persists this Chat's explicit AI Setup differences from the effective
// Project setup (Phase 06, AI_SETUP_CONTRACT.md). `overrides` replaces the
// full override set for this Chat - an empty vector is "Reset to Project
// setup". A Chat override only affects subsequent operations in that Chat;
// it never rewrites the provenance of already-completed tasks. Each entry
// is validated against Routing::AiSetup's real role-compatibility rules
// before it is persisted.
matters::Matter save_ai_setup_overrides(const std::string &matter_id, const std::string &chat_id,
                                        std::vector<matters::AiSetupRoleOverride> overrides);

// Phase 27's restart-surviving draft (unsent composer text) - backend/`.lm`
// persisted so a React-only copy is never the sole durable copy. Does not
// bump `revision`: see MatterChat::draft_text's own doc comment for why an
// ordinary debounced autosave is not the optimistic-lock concern rename/AI
// Setup guard against.
matters::MatterChat save_draft(const std::string &project_id, const std::string &chat_id, std::string draft_text);

// Records a document for this Chat (MatterChat::working_document_id), kept
// for API compatibility: it no longer binds where a request's change lands -
// the request says that, and the model names the document. An empty
// document_id clears it. The document must be a real, writable Project-folder
// document of this Project - a source-only or unwritable file is refused with
// the reason, never silently ignored. Does not bump `revision`, for
// save_draft()'s own reason.
matters::MatterChat set_working_document(const std::string &project_id, const std::string &chat_id,
                                        const std::string &document_id);

// Makes a chat an agent's thread (MatterChat::agent_id), with the agent's
// brief as its standing instructions, or brings the brief up to date. Every
// request in the chat carries the brief from then on, whoever sends it.
matters::MatterChat bind_agent_thread(const std::string &project_id, const std::string &chat_id,
                                     const std::string &agent_id, const std::string &instructions);

// An agent's own words in its thread - introducing itself, telling what it
// found (MatterTask "agent_message"): no request and no model, so nothing
// leaves this computer and nothing is charged. Returns the message's id.
// `kept_here` for an agent that works only on this computer: what it says is
// kept here as its work is, and its thread with it (chat_holds_work_kept_here),
// on every computer that opens the Matter.
// What a message carries besides its words, whoever writes it: the message of
// the same thread it replies to, the Matter's documents it hands over with a
// line for what its author did not check, the answers it offers to a question
// it asks (two to four), and - for a reply that answers such a question - the
// answer it gives, which must be one the quoted message offers.
struct MessageExtras {
  std::string quote_message_id;
  std::vector<std::string> attachment_document_ids;
  std::string not_checked;
  std::vector<std::string> ask_options;
  std::string answer_option;
  // The place in one of the Matter's documents the message is about.
  matters::DocumentPlace place;
  // Whom an AI colleague's message addresses (D-168): the people (by actor id)
  // and AI colleagues (by agent id) it names with "@", as a person's message
  // names them. A person named finds it among what asks them.
  std::vector<std::string> mentions;
};

// A place in a document as a colleague reads it: "Section 2.1 (Purpose) of
// "NDA.docx", where it reads: "...""; for cells of a workbook, "the cells
// A2:M2 of the Contracts sheet of "Tracker.xlsx", which hold: "..."" (D-110);
// empty for no place.
std::string place_words(const matters::Matter &project, const matters::DocumentPlace &place);

// Whether a document is a workbook, whose places are cells, never clauses
// (D-110).
bool is_workbook(const matters::MatterDocument &document);

// Whether a document is a deck, whose places are slides: "slide 2 (Key
// commercial terms) of "Status.pptx", which reads: "..."" (D-112).
bool is_presentation(const matters::MatterDocument &document);

// A person's message as the colleagues it names take it up as work: its words,
// and the place in a document it is about, when it points at one. The same
// words on every computer, from the message as it was kept and signed.
std::string words_for_colleagues(const matters::Matter &project, const matters::MatterTask &message);
std::string words_for_colleagues(const std::string &project_id, const matters::MatterTask &message);

// Whom a person's message addresses among the colleagues: those it names (@),
// and the colleague whose message it replies to - a reply is addressed as a
// mention is (the owner, 2026-10-26: "she should react to replies too"). The
// one replied to is named directly: it stands first, before any "all".
std::vector<std::string> addressed_colleagues(const matters::Matter &project, const matters::MatterTask &message);
std::vector<std::string> addressed_colleagues(const std::string &project_id, const matters::MatterTask &message);

std::string post_agent_message(const std::string &project_id, const std::string &chat_id,
                               const std::string &agent_id, const std::string &agent_name, const std::string &text,
                               bool kept_here, const MessageExtras &extras = MessageExtras());

// A person's words to their colleagues in a thread (MatterTask
// "member_message"): no request and no model - the team reads it on every
// computer that opens the Matter. `mentions` names agents (by id) and people
// (by actor id) it is addressed to; an agent named takes it as work. A message
// that hands over files may have no words. Returns the message as it was kept.
matters::MatterTask post_member_message(const std::string &project_id, const std::string &chat_id,
                                        const std::string &text, const std::vector<std::string> &mentions,
                                        const MessageExtras &extras = MessageExtras());

// The same rule for every member, a person or an AI colleague: a message is
// changed or deleted only by its author, and only where its author writes - the
// person at this computer, for a message this computer signed. A deleted
// message keeps its place in the thread, without its words or what it carried.
// Both sign the message again (Chat/MessageProvenance).
// A thread's closing, with its conclusion, or its reopening, by the person at
// this computer (D-088): a record of its own in the thread, signed like a
// message, and never edited or deleted afterwards. Closing a closed thread or
// reopening an open one is refused. A reply to a closed thread reopens it
// without a record.
matters::MatterTask close_thread(const std::string &project_id, const std::string &chat_id,
                                 const std::string &conclusion);
matters::MatterTask reopen_thread(const std::string &project_id, const std::string &chat_id);

matters::MatterTask edit_member_message(const std::string &project_id, const std::string &chat_id,
                                        const std::string &message_id, const std::string &text);
matters::MatterTask delete_member_message(const std::string &project_id, const std::string &chat_id,
                                          const std::string &message_id);

// A reaction of the person at this computer to any message of a thread, a
// person's or an AI colleague's: put, or taken back if it was there. Returns the
// message's reactions as they now stand.
std::map<std::string, std::vector<std::string>> react_to_message(const std::string &project_id,
                                                                 const std::string &chat_id,
                                                                 const std::string &message_id,
                                                                 const std::string &emoji);
// An AI colleague's reaction to a message, as a person's is kept, under the
// colleague's own id (the owner, 2026-10-26: "teach the AI agents to put
// reactions on messages"): put once, never taken back by putting it again.
std::map<std::string, std::vector<std::string>> react_as_colleague(const std::string &project_id, const std::string &chat_id,
                                                                   const std::string &message_id, const std::string &agent_id,
                                                                   const std::string &emoji);

// The messages of a chat that other people wrote, marked as having reached
// the member at this computer now (D-212), each one not marked so before: a
// messenger's "received", kept in the chat's own record, so its author sees
// it too. The window says so once it has read them. Returns how many it
// marked; nothing is written when none was new.
std::size_t mark_received(const std::string &project_id, const std::string &chat_id);

// Phase 27's real backend dependency check before Chat deletion
// (phase-27-chat-lifecycle-observe.md's "Delete Chat"). Counts only what a
// person would otherwise wrongly believe is about to disappear: this Chat's
// own turn history (real MatterTasks this Chat groups, which do become
// inaccessible through Chat once the Chat itself is deleted) versus the
// independent, already-copied Work Product/Issues and Project Context items
// created from those operations (WorkProductOrigin::ChatTask/Finding,
// ContextItemSource::chat_operation_id) - which this deletion never removes.
struct ChatDeletionImpact {
  int turn_count = 0;
  int work_product_count = 0;
  int context_item_count = 0;
};
ChatDeletionImpact deletion_impact(const std::string &project_id, const std::string &chat_id);

// Removes this Chat from the Project (matters::Matter::chats) - a real
// deletion, not the separate `archived` hide-from-list state. The Chat's own
// MatterTask records are never deleted: they remain durable Project
// operation history (Usage/Routing Log/Observe keep working, and
// WorkProductActions already resolves "Source Chat deleted" honestly for a
// Work Product whose origin task's chat_id no longer names a real Chat).
// `expected_revision` is the same optimistic-concurrency guard rename() uses.
matters::Matter delete_chat(const std::string &project_id, const std::string &chat_id, long long expected_revision);

// One real backend text match over this Chat title/prompts/response text -
// Phase 27's "Search" (phase-27-chat-lifecycle-observe.md). Unicode-safe
// case-insensitive substring search, never a frontend-only filter dressed
// up as full search. `match_location` is "title", "prompt" or "response".
struct ChatSearchMatch {
  std::string chat_id;
  std::string chat_title;
  std::string match_location;
  std::string snippet;
};
// Brings back a deleted Chat (its turns were never removed). A Chat that is
// not deleted is returned unchanged.
matters::Matter restore(const std::string &project_id, const std::string &chat_id);

// "/compact" - the person asking Singularity to carry this conversation forward in
// a shorter form. What it produces is derived from this Chat's OWN canonical
// operations (matters::ChatCompaction), never written by a model: a compaction
// that asked a model to summarise would be a new place for facts to be
// invented, and the one thing a compacted Chat must never do is remember
// something that did not happen. It therefore costs no provider call, behaves
// identically on a frontier model and a local one, and survives a restart.
//
// Every turn of the Chat so far is covered; anything sent afterwards is carried
// as itself. Compacting a Chat that has nothing to compact is refused rather
// than silently recorded. Returns the compacted Chat.
struct ChatCompactionResult {
	matters::MatterChat chat;
	std::size_t covered_turns = 0;
	// What the conversation carried before and after, in estimated tokens, so
	// the person is told what compacting actually did.
	std::size_t tokens_before = 0;
	std::size_t tokens_after = 0;
	// And what it carries when the window cannot hold even that: every turn by
	// its brief record alone. A compacted conversation is never bigger than
	// this, whatever model answers next.
	std::size_t tokens_after_brief = 0;
};

ChatCompactionResult compact(const std::string &project_id, const std::string &chat_id);

std::vector<ChatSearchMatch> search(const std::string &project_id, const std::string &query);

// What a member has read of each conversation (D-202): for each - a thread or
// a direct message, by its chat's id - the time of the latest message they
// have seen there. Kept in the firm's record (D-203), each computer writing
// only its own file of each member's marks (`.lm/ReadMarks/<member>/<device>.json`)
// and every computer of the member reading the latest of all; with no firm's
// record installed, with the member's preferences here (ReadMarks.json),
// which are still read. A key is at most 120 letters of [A-Za-z0-9_.:-], a
// time at most 40 of an ISO time's; others are refused.
struct ReadMarksPlace {
	// The firm's record's folder here; none while it cannot be kept.
	std::function<std::optional<std::filesystem::path>()> firm;
	// This computer, whose file it writes.
	std::function<std::string()> device_id;
	// A mark was written: the exchange carries it at once.
	std::function<void()> written;
};
void install_read_marks_place(ReadMarksPlace place);
std::map<std::string, std::string> read_marks(const std::string &actor_id);
// The member has seen `conversation` up to `up_to`: kept unless a later time
// is kept already, so a mark never goes back. At most 4,000 kept for a member,
// the oldest let go first. Returns every mark of the member.
std::map<std::string, std::string> mark_read(const std::string &actor_id, const std::string &conversation, const std::string &up_to);

} // namespace singularity::chat
