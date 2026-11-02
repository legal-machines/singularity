#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace singularity::chat {

// The explicit, backend-owned source-policy contract (Phase 21 corrective
// closure). A caller always states which mode it means; the mode is never
// inferred from whether selected_document_ids happens to be empty - that
// inference was the original gap this enum closes. See resolve_sources()
// below for the exact fail-closed rule each mode enforces.
enum class SourcePolicyMode { Relevant, Selected };
[[nodiscard]] const char *to_string(SourcePolicyMode mode) noexcept;
// Parses exactly "relevant"/"selected" (the wire values to_string() above
// produces). Anything else - an unknown string, a stale/future mode - is
// nullopt so the Bridge layer can reject it explicitly rather than silently
// defaulting to Relevant.
[[nodiscard]] std::optional<SourcePolicyMode> source_policy_mode_from_string(const std::string &value) noexcept;

// One local Project document excerpt actually available to compose a
// not-yet-sent Chat request from - a pre-send preview/handoff shape for
// Chat composition (Phase 21, phase-21-chat-composer-sources.md), never the
// provider-facing evidence context::ResolvedSourceReference represents
// (that is only assigned once a real model request is actually assembled -
// see Context/SourceMarkers/SourceMarkers.hpp).
struct ResolvedChatSource {
  std::string document_id;
  std::string document_name;
  std::string chunk_id;
  std::optional<std::size_t> page;
  std::optional<std::string> heading;
  std::string text_preview;
};

struct ChatSourceResolution {
  // The backend-owned effective mode this resolution actually ran under -
  // always exactly the caller's requested mode (resolve_sources() below
  // never falls back from Selected to Relevant, or the reverse); this field
  // exists so a caller can render/assert the real outcome without trusting
  // its own request echo.
  SourcePolicyMode mode = SourcePolicyMode::Relevant;
  // Echoes exactly the caller-supplied selection this resolution was
  // computed from - always empty for Relevant Project sources.
  std::vector<std::string> selected_document_ids;
  std::vector<ResolvedChatSource> sources;
};

// Resolves this Project's real source policy for one not-yet-sent Chat
// query - the narrow, backend-owned composition/source-resolution handoff
// Phase 24's real Send invokes to build actual requests from (see
// phase-21-chat-composer-sources.md's "Phase boundary correction"). Never
// runs a model, never creates a MatterTask: built entirely from the
// existing local Project document pool (matter::list_documents(), the same
// steady-state reconciled read Bridge/DocumentBridge/DocumentBridge.cpp's
// own list_documents() already uses) filtered through the existing
// ContextRetriever (matters::retrieve_document_context()) - no second
// retrieval system, no document/chunk copy kept anywhere in Chat state.
//
// `mode` is the explicit, caller-stated source-policy mode - never inferred
// from `selected_document_ids`. Each mode enforces its own fail-closed rule
// rather than silently reinterpreting the other:
//   - Relevant: `selected_document_ids` must be empty. A non-empty selection
//     is a contradictory request (asking for both "search everything" and
//     "only these documents" at once) and is rejected outright, never
//     silently ignored - the existing ContextRetriever then selects locally
//     relevant excerpts from every AI-ready Project document; never "send
//     the whole Project" (PROJECT_CONTRACT.md).
//   - Selected: `selected_document_ids` must be non-empty - an empty
//     selection fails closed and NEVER falls back to Relevant, since that
//     silent reinterpretation is exactly the ambiguity this contract
//     exists to remove. Every supplied id must genuinely belong to this
//     Project and be AI-ready - one that does not (another Project's
//     document, one gone from the folder, one that failed extraction) fails
//     the whole resolution rather than silently narrowing to what was
//     usable (ContextRetriever::retrieve()'s own fail-closed contract). A
//     document merely not read yet is not one of those: a selected document
//     still being read is read before resolution
//     (matter::wait_for_needed_documents_read()), and one still unread is read
//     from its current bytes at the read boundary. Nothing else the Project
//     is reading is waited for.
//   An explicit target (`target_document_id`, the document a change is for) is
//   waited for as a selected document is, in either mode.
ChatSourceResolution resolve_sources(const std::string &project_id, const std::string &query,
                                     SourcePolicyMode mode, std::vector<std::string> selected_document_ids,
                                     const std::optional<std::string> &target_document_id = std::nullopt);

} // namespace singularity::chat
