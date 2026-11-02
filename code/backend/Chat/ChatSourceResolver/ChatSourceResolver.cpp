#include "Chat/ChatSourceResolver/ChatSourceResolver.hpp"

#include "Context/ContextRetriever/ContextRetriever.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Routing/Observability/OperationEvent.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace singularity::chat {

namespace {

// How long a request waits for the documents it selected to be read, and, with
// none selected, for the few just added to the folder.
constexpr std::chrono::seconds kSelectedReadingWait{120};
constexpr std::chrono::seconds kRelevantReadingWait{15};

} // namespace

const char *to_string(SourcePolicyMode mode) noexcept {
  switch (mode) {
    case SourcePolicyMode::Relevant: return "relevant";
    case SourcePolicyMode::Selected: return "selected";
  }
  return "relevant";
}

std::optional<SourcePolicyMode> source_policy_mode_from_string(const std::string &value) noexcept {
  if (value == "relevant") return SourcePolicyMode::Relevant;
  if (value == "selected") return SourcePolicyMode::Selected;
  return std::nullopt;
}

ChatSourceResolution resolve_sources(const std::string &project_id, const std::string &query,
                                     SourcePolicyMode mode, std::vector<std::string> selected_document_ids,
                                     const std::optional<std::string> &target_document_id) {
  // The explicit-mode fail-closed contract itself (see the .hpp's own
  // comment on resolve_sources()) - checked before any retrieval work so
  // neither direction can ever silently reinterpret the other:
  //   - Relevant never tolerates a selection it would otherwise ignore.
  //   - Selected + [] never quietly becomes Relevant.
  if (mode == SourcePolicyMode::Relevant && !selected_document_ids.empty()) {
    throw std::runtime_error(
        "Relevant Project sources does not accept a document selection. Use Selected documents instead.");
  }
  if (mode == SourcePolicyMode::Selected && selected_document_ids.empty()) {
    throw std::runtime_error("Select at least one Project document for Selected documents.");
  }

  const auto workspace_path = node::require_active_workspace_path();
  // The same steady-state reconciled read Documents' own list already uses
  // (Bridge/DocumentBridge/DocumentBridge.cpp's list_documents()), so
  // Selected documents' strict boundary and Relevant Project sources' whole
  // candidate pool both see genuinely current Processing/Ready/Unreadable
  // state - never a stale in-memory snapshot, and no reattachment required
  // after a fresh process start (matter::list_documents() runs the real
  // startup/restart audit the first time this Project is observed).
  routing::observability::emit({routing::observability::OperationEventKind::SourceReadStart,
      "Resolving and reading Project sources",
      {{"mode", to_string(mode)}, {"selectedCount", std::to_string(selected_document_ids.size())}},
      std::nullopt});
  // Listed without reading: a data room still being read is read by its own
  // pass, never on this request's thread.
  matters::Matter matter = matter::list_documents_for_request(project_id);
  // Only what this request needs is waited for: the selected documents still
  // being read are read now, so they are resolved as read rather than refused;
  // with none selected, a few just added to the folder are read, and a larger
  // reading goes on while the request is resolved from what is read. Never
  // under the persistence lock: none is held here.
  const matter::DocumentReadingWait waited = matter::wait_for_request_documents_read(project_id,
      selected_document_ids, target_document_id,
      std::chrono::duration_cast<std::chrono::milliseconds>(kSelectedReadingWait),
      std::chrono::duration_cast<std::chrono::milliseconds>(kRelevantReadingWait),
      [](std::size_t count) {
        routing::observability::emit({routing::observability::OperationEventKind::SourceReadStart,
            "Reading the Project's newly added or changed documents",
            {{"documentCount", std::to_string(count)}, {"mode", "project_documents"}}, std::nullopt});
      });
  if (waited.awaited_count > 0) matter = matter::read_project_documents(project_id);
  if (!waited.unread_document_ids.empty())
    routing::observability::emit({routing::observability::OperationEventKind::SourceReadProgress,
        waited.unread_document_ids.size() == 1 ? "One document is still being read, so it is not used yet"
            : std::to_string(waited.unread_document_ids.size()) + " documents are still being read, so they are not used yet",
        {{"unreadCount", std::to_string(waited.unread_document_ids.size())}}, std::nullopt});

  ChatSourceResolution result;
  result.mode = mode;
  result.selected_document_ids = selected_document_ids;

  // No second retrieval system: the exact existing primitive real task
  // execution already uses (Routing/MatterRequestExecution.cpp's
  // prepare_task_request()) to build candidates from this Project's own
  // AI-ready documents and fail closed on any selected id that is not one
  // of them - another Project's document, a Processing/unreadable one, or
  // one simply not selected.
  // The same passages a send's capacity-first delivery considers.
  const auto chunks = matters::retrieve_document_context(workspace_path, matter, query, selected_document_ids,
      nullptr, matters::SelectedSourceDelivery::EveryPassage);
  result.sources.reserve(chunks.size());
  for (const auto &chunk : chunks) {
    result.sources.push_back(ResolvedChatSource{
        chunk.document_id,
        chunk.document_name,
        chunk.chunk.chunk_id,
        chunk.chunk.page,
        chunk.chunk.heading,
        chunk.chunk.text,
    });
  }
  routing::observability::emit({routing::observability::OperationEventKind::SourceReadEnd,
      "Project sources are ready",
      {{"documentCount", std::to_string(result.selected_document_ids.empty()
            ? matter.documents.size() : result.selected_document_ids.size())},
       {"chunkCount", std::to_string(result.sources.size())}}, std::nullopt});
  return result;
}

} // namespace singularity::chat
