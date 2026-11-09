#pragma once

#include "Context/ContextRetriever/ContextRetriever.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace singularity::context {

// One chunk actually assigned an opaque provider-facing marker by a
// SourceMarkerRegistry, together with everything needed to resolve that
// marker to a human-readable local label after a model response returns.
// Safe to persist on MatterTask: this is ordinary local Matter/document
// metadata, no different in kind from what MatterDocument already stores on
// disk - not the Papinian restoration map, a provider secret or a
// filesystem path (Master Spec Sec.45/9 govern those, not this).
struct ResolvedSourceReference {
  std::string document_token; // "DOC_1" - the marker's document component.
  std::string chunk_token;    // "CHUNK_1" - the marker's chunk component.
  std::string document_id;
  std::string chunk_id;
  std::string document_name;
  std::optional<std::size_t> page;
  std::optional<std::string> heading;
  // The reading the cited passage was cut from (DocumentChunk::
  // extractor_version); empty for a citation recorded before readings were
  // kept, which was cut from the reading its format had then.
  std::string extractor_version;

  // "[SOURCE:DOC_1:CHUNK_1]" - the exact provider-facing marker text.
  [[nodiscard]] std::string marker() const;
};

// A human-readable local label for a resolved reference, e.g.
// "summary.txt, p. 4 - Indemnification". Built only after a model response
// returns; never sent to a provider.
[[nodiscard]] std::string format_reference_label(const ResolvedSourceReference &reference);

enum class SourceReferenceStatus {
  // The marker's document/chunk tokens were both issued by the validating
  // registry - it points to material actually supplied to the model for
  // this request.
  Supplied,
  // Well-formed "[SOURCE:<doc>:<chunk>]" syntax, but the validating
  // registry never issued this exact document/chunk token pair - a
  // fabricated marker, or one whose chunk was retrieved but dropped by the
  // context budget before this request was actually sent. Master Spec
  // Sec.22: "A fabricated model marker is NOT a source."
  NotSupplied,
  // Did not parse as a "[SOURCE:<doc>:<chunk>]" marker at all.
  Malformed,
};

// One "[SOURCE:...]"-shaped span found in a model's response text, and what
// it validated to.
struct ValidatedSourceReference {
  std::string raw_text;
  SourceReferenceStatus status = SourceReferenceStatus::Malformed;
  // Set only when status == Supplied.
  std::optional<ResolvedSourceReference> resolved;
};

// The deduplicated, first-citation-ordered set of every reference a model
// response actually and validly cited - what MatterTask persists (see
// matters::MatterTask::source_references). Never includes a NotSupplied or
// Malformed entry.
[[nodiscard]] std::vector<ResolvedSourceReference>
cited_references(const std::vector<ValidatedSourceReference> &validated);

// One rendered piece of a model response, already stripped of every
// internal marker. Exactly one of the two is meaningful: `text` is ordinary
// response prose, and a `citation_number` above zero is a citation control
// belonging at this exact position in the reading order. Deliberately
// pre-split by the backend so a renderer never has to recognise, parse or
// even see "[SOURCE:...]" syntax (Master Spec Sec.22 - an internal marker is
// never user-facing text).
struct ProjectedResponseSegment {
  std::string text;
  int citation_number = 0;
};

// The render-safe projection of one persisted response: its prose split
// around numbered citations, the backend-resolved identity behind each
// number, and how many marker-shaped spans could not be resolved to
// material actually supplied to that operation.
struct ProjectedResponse {
  std::vector<ProjectedResponseSegment> segments;
  // citations[n - 1] is the reference for citation_number n.
  std::vector<ResolvedSourceReference> citations;
  // Malformed spans plus spans naming a reference this operation never
  // supplied. Never rendered as a citation, never shown as marker text -
  // only ever counted, so a partial failure stays truthful without
  // exposing internal syntax.
  std::size_t unresolved_reference_count = 0;
};

// Projects a persisted response against the references that operation
// actually recorded as supplied and cited (matters::MatterTask::
// source_references, itself produced by cited_references() above from the
// live request's own SourceMarkerRegistry). This is deliberately a pure
// function of durable operation evidence rather than a second retrieval:
// membership in `cited` is exactly the Supplied verdict the original
// registry reached, so the same answer reconstructs identically in a fresh
// process, and a marker that was fabricated or not supplied can never be
// promoted to a citation later. Never re-runs retrieval and never consults
// current Project state. A bare handle outside a marker ("DOC_2") is rendered
// by render_source_handles()'s rule against the same cited set and counted
// when it resolves to nothing cited.
[[nodiscard]] ProjectedResponse
project_response_references(const std::string &response_text,
                            const std::vector<ResolvedSourceReference> &cited);

// Text that will be retained as a lawyer-facing artifact must not keep
// internal handles. Every handle in `text` - the exact "[SOURCE:DOC_n:CHUNK_m]"
// marker, or a paraphrase of the same Singularity token grammar ("DOC_1, Chunk 1",
// "DOC_1") - is resolved against `basis` (the references actually supplied to
// that operation) and replaced by label_for(reference). A handle `basis` never
// issued becomes kUnverifiedSourceLabel; nothing is ever guessed from a name.
inline constexpr const char *kUnverifiedSourceLabel = "[unverified source reference]";

struct RenderedSourceHandles {
  std::string text;
  // Deduplicated, first-occurrence order; keeps the internal tokens and
  // canonical document/chunk ids as provenance.
  std::vector<ResolvedSourceReference> resolved;
  std::size_t unresolved_count = 0;
};

[[nodiscard]] RenderedSourceHandles
render_source_handles(const std::string &text, const std::vector<ResolvedSourceReference> &basis,
                      const std::function<std::string(const ResolvedSourceReference &)> &label_for);

// The same handles named for a model that reads the text again - an earlier
// answer carried in a chat's history. A model writes its citations side by side
// and right against a word ("…Records and Audit[SOURCE:DOC_3:CHUNK_2]
// [SOURCE:DOC_5:CHUNK_1] reply"), and named in place they ran together:
// "…9. Records and AuditCorvane reply - …md", «…».
// Glued so, a name or a code in a label is no longer a word of its own. Here
// each run of handles standing together is named once, apart from the words
// around it: " (Source: A; B)".
[[nodiscard]] RenderedSourceHandles
render_source_handles_apart(const std::string &text, const std::vector<ResolvedSourceReference> &basis,
                            const std::function<std::string(const ResolvedSourceReference &)> &label_for);

// The same handles taken out of the text instead of named. The words of a
// legal instrument, or of a change to a document, never carry a citation: the
// answer beside them does. Each handle goes with the space before it, and a
// parenthesis or list separator that held nothing but citations goes with it
// too, so "LLC [SOURCE:DOC_1:CHUNK_1]." reads "LLC.". A handle `basis` never
// issued goes the same way and is counted. The references it resolves are kept
// as provenance.
[[nodiscard]] RenderedSourceHandles
strip_source_handles(const std::string &text, const std::vector<ResolvedSourceReference> &basis);

// Text already rendered with labels, with every kUnverifiedSourceLabel taken
// out the same way: in a document the lawyer keeps, a bracketed note about a
// source reads as part of the document.
[[nodiscard]] std::string without_unverified_labels(const std::string &text);

// Assigns a stable opaque marker to every chunk actually supplied to one
// model invocation, renders provider-facing source blocks under those
// markers instead of any document/Matter/filename identifier, and - after a
// response returns - parses and validates a model's cited markers against
// this exact same supplied set. Never falls back to a fresh retrieval to
// validate a citation (Master Spec Sec.22): a marker is only ever checked
// against what one registry instance was actually built from.
//
// Markers are numbered purely by first-appearance order within the supplied
// chunks - deterministic for identical input, but carrying no meaning across
// requests or Matters; chunk_id (already stable/content-addressed) remains
// the real persisted identity. See restricted_to() for how a request-wide
// context budget that drops a retrieved chunk is reflected here.
class SourceMarkerRegistry final {
public:
  explicit SourceMarkerRegistry(const std::vector<RetrievedContextChunk> &chunks);

  // The opaque marker text for `chunk_id`, for embedding into that chunk's
  // own provider-facing rendering. Throws std::out_of_range if this
  // registry was not built from `chunk_id`.
  [[nodiscard]] std::string marker_for(const std::string &chunk_id) const;

  // The document component of `chunk_id`'s marker ("DOC_1") - the handle a
  // provider-facing statement about that whole document uses. Throws
  // std::out_of_range if this registry was not built from `chunk_id`.
  [[nodiscard]] const std::string &document_token_for(const std::string &chunk_id) const;

  // One provider-facing SOURCE block: the opaque marker followed by the
  // chunk's untrusted evidence text. Contains no document_id, filename,
  // page or heading - see Master Spec Sec.22/46.
  [[nodiscard]] std::string format_chunk(const RetrievedContextChunk &chunk) const;

  // format_chunk() for every supplied chunk, concatenated - the full
  // provider-facing SOURCE_MATERIAL block.
  [[nodiscard]] std::string
  format_all(const std::vector<RetrievedContextChunk> &chunks) const;

  [[nodiscard]] const std::vector<ResolvedSourceReference> &references() const;

  // A registry containing only the entries whose chunk_id is in
  // `supplied_chunk_ids`, with every marker/token unchanged - the "exact
  // request context snapshot" a context-budget decision (see
  // PromptAssembler::assemble_with_budget) can narrow after this registry
  // was built from the full pre-budget retrieval result. An entry excluded
  // from `supplied_chunk_ids` can never validate afterward, even though it
  // was retrieved and assigned a well-formed marker here.
  [[nodiscard]] SourceMarkerRegistry
  restricted_to(const std::unordered_set<std::string> &supplied_chunk_ids) const;

  // Parses every "[SOURCE:...]"-shaped span in `response_text` and
  // classifies each against this registry's own supplied set. Never treats
  // an unparseable or unissued marker as a citation.
  [[nodiscard]] std::vector<ValidatedSourceReference>
  validate_response(const std::string &response_text) const;

private:
  SourceMarkerRegistry() = default;

  std::vector<ResolvedSourceReference> references_;
  std::unordered_map<std::string, std::size_t> index_by_chunk_id_;
  std::unordered_map<std::string, std::size_t> index_by_marker_;
};

} // namespace singularity::context
