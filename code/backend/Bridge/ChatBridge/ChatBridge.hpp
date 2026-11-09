#pragma once

#include "Chat/ChatOutboundPreview/ChatOutboundPreview.hpp"

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::chat_bridge {

nlohmann::json list(const nlohmann::json &params);
nlohmann::json create_project_chat(const nlohmann::json &params);
nlohmann::json rename(const nlohmann::json &params);
nlohmann::json create(const nlohmann::json &params);
nlohmann::json save_ai_setup(const nlohmann::json &params);

// Phase 27's restart-surviving draft (Chat/ChatActions/ChatActions.hpp's
// save_draft()) - backend/`.lm` persisted so a React-only copy is never the
// sole durable copy.
nlohmann::json save_draft(const nlohmann::json &params);

// The one document this Chat works on (MatterChat::working_document_id) - a
// real, visible, persisted choice, so binding a document to work on no longer
// depends on quoting its file name in the message. "" clears it.
nlohmann::json set_working_document(const nlohmann::json &params);

// A message of a thread, as every member's is (Chat/ChatActions): its author
// changes or deletes it on the computer that wrote it; anyone reacts to it.
nlohmann::json edit_message(const nlohmann::json &params);
nlohmann::json delete_message(const nlohmann::json &params);
nlohmann::json react(const nlohmann::json &params);
// The messages of a chat other people wrote, marked as having reached the
// member at this computer.
nlohmann::json mark_received(const nlohmann::json &params);
// What the member at this computer has read of each conversation, and marking
// one read up to a time.
nlohmann::json read_marks(const nlohmann::json &params);
nlohmann::json mark_read(const nlohmann::json &params);
// A thread's closing with its conclusion, and its reopening.
nlohmann::json close_thread(const nlohmann::json &params);
nlohmann::json reopen_thread(const nlohmann::json &params);


// Phase 27's real backend dependency check before Chat deletion
// (Chat/ChatActions/ChatActions.hpp's deletion_impact()) - what a Delete
// Chat confirmation honestly tells the user before it happens.
nlohmann::json deletion_impact(const nlohmann::json &params);

// Phase 27's Delete Chat (Chat/ChatActions/ChatActions.hpp's delete_chat())
// - a real removal from Project.chats, never merely the separate `archived`
// hide-from-list state.
nlohmann::json delete_chat(const nlohmann::json &params);
// Brings back a deleted Chat that new work reached after its deletion.
// chat.compact - "/compact": carry this conversation forward in a shorter
// form, derived from its own canonical records and never written by a model.
nlohmann::json compact(const nlohmann::json &params);

nlohmann::json restore(const nlohmann::json &params);
// chat.setArchived {projectId, chatId, archived} -> ProjectChat.
nlohmann::json set_archived(const nlohmann::json &params);

// Phase 27's Search (Chat/ChatActions/ChatActions.hpp's search()) - a real
// backend text match over Chat title/prompts/response text, never a
// frontend-only filter.
nlohmann::json search(const nlohmann::json &params);

// Phase 21's real composition/source-resolution handoff - Chat/
// ChatSourceResolver/ChatSourceResolver.hpp. Never a model call: resolves
// Relevant Project sources or a strict Selected documents boundary from the
// existing Project document pool via the existing ContextRetriever.
nlohmann::json resolve_sources(const nlohmann::json &params);

// Phase 22's backend-owned legal task truth for Chat's composer
// (Routing/LegalTaskPresets/LegalTaskPresets.hpp's
// selectable_legal_task_presets()) - the current (id, version, name) for
// every task the composer may offer through progressive disclosure. Never
// includes General, which is never permanently displayed.
nlohmann::json legal_task_options(const nlohmann::json &params);

// Phase 22's narrow pre-execution resolution boundary (Chat/
// ChatOperationSetup/ChatOperationSetup.hpp) - resolves, but never
// executes, the legal task/preset, primary result-producing role/model and
// full effective role assignment set a real Phase 24 operation would use.
// A one-time primary-role model override in params is validated here
// against the live model/provider registry (routing::
// validate_role_model_assignment(), the same live-registry lookup
// save_ai_setup() above already performs) before the domain resolver ever
// sees it - see that function's own comment for why this stays in the
// Bridge layer.
nlohmann::json resolve_operation_setup(const nlohmann::json &params);

// The one place a Chat wire request is parsed into a composition - shared
// by resolve_operation_setup() above, Bridge/ChatPreviewBridge (Phase 23)
// and chat.send, so a one-time primary-model override can never reach a
// resolver through a second, unvalidated path and an unknown source-policy
// mode can never be silently defaulted.
//
// chat_request_setup reads what a request names before it is written: its
// Project, its Chat, its legal task and a one-time model. The callers that
// do not compose a request (the operation setup and the composer's
// protection state) use it alone, so they read nothing else.
chat::ChatRequestComposition chat_request_setup(const nlohmann::json &params);
// chat_request_composition() reads the setup, then the request itself: its
// words, its sources, its target document, whether it may ask the lawyer
// and the request it replaces. A send and its preview use it.
chat::ChatRequestComposition chat_request_composition(const nlohmann::json &params);

} // namespace singularity::bridge::chat_bridge
