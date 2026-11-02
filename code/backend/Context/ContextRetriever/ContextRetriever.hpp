#pragma once

#include "Document/DocumentChunker/DocumentChunker.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace singularity::context {

// A fingerprint-matched, AI-ready local document and its deterministic Phase
// 02 chunks. ContextRetriever never opens Matter files or contacts a provider.
struct RetrievalDocument {
  std::string document_id;
  std::string name;
  std::vector<documents::DocumentChunk> chunks;
  // Where the lawyer keeps it in the Project - its folder and file name - as a
  // prompt names it above its passages. Empty: its name alone.
  std::string title;
};

// The historical relevance window: how many passages automatic retrieval
// returns, and the passage bound delivery falls back to when the executing
// model's capacity is not resolved (see PromptAssemblyInput).
inline constexpr std::size_t kRelevancePassageWindow = 8;

// Capacity-bounded relevance delivery. A caller whose prompt is admitted by
// the executing model's resolved capacity (PromptAssembler) does not need
// retrieval to guess a passage count for it: eight passages are one excerpt
// each from eight documents, which cannot answer "every agreement the sale
// triggers" across a thirty-document data room however large the model's
// window is. Such a caller receives every passage the request genuinely points
// at, best first, and capacity decides how much of it is sent.
//   - "Genuinely points at" is a share of the best passage's own score, never
//     an absolute number: lexical scores have no fixed scale, and a passage
//     that shares only incidental words with the request scores a small
//     fraction of one that is about it.
//   - The ceiling only bounds the work of assembling a prompt; it is far above
//     what a data room question needs and is not a relevance judgment.
inline constexpr double kRelevanceFloorShare = 0.2;
inline constexpr std::size_t kRelevanceCandidateCeiling = 96;

struct ContextRetrievalRequest {
  std::string query;
  // Non-empty means the user chose specific documents. This is a privacy
  // boundary: unselected documents are not retrieval candidates.
  std::vector<std::string> selected_document_ids;
  std::vector<RetrievalDocument> documents;
  std::size_t maximum_chunks = kRelevancePassageWindow;
  // Automatic (no explicit selection) retrieval only. Above zero, passages
  // scoring below this share of the best passage's score are left out once
  // the historical window's worth of best passages has been taken. Zero keeps
  // the historical behaviour: fill to maximum_chunks with anything relevant.
  double relevance_floor_share = 0.0;
};

struct RetrievedContextChunk {
  std::string document_id;
  std::string document_name;
  documents::DocumentChunk chunk;
  bool explicitly_selected = false;
  double lexical_score = 0.0;
  // RetrievalDocument::title, or the name where it has none.
  std::string document_title;
};

class ContextRetriever final {
public:
  // Unknown/duplicate selected IDs fail closed. With explicit selection, at
  // least one best-ranked chunk per selected document is returned before
  // remaining capacity is filled by global lexical/heading rank.
  [[nodiscard]] static std::vector<RetrievedContextChunk>
  retrieve(const ContextRetrievalRequest &request);
};

} // namespace singularity::context
