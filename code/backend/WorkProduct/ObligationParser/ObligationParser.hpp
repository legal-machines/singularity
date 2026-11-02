#pragma once

#include "Context/SourceMarkers/SourceMarkers.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::context {

// One structured obligation parsed from a model's response text (Master
// Spec Sec.72). Deliberately lacks `id`/`matterId` - those are assigned by
// the caller (see matters::Obligation), not by the model or this parser.
// Only `obligation` itself is required by the Master Spec's own domain
// shape (`party?`/`trigger?`/`deadline?`); no speculative field is added
// beyond it.
struct ParsedObligation {
  std::optional<std::string> party;
  std::string obligation;
  std::optional<std::string> trigger;
  std::optional<std::string> deadline;
  // Every entry here was validated Supplied against the exact
  // SourceMarkerRegistry passed to parse_obligations() - never a fresh
  // retrieval or a re-derivation against current Matter state (Master Spec
  // Sec.22), the identical contract WP6-A/WP6-B already use.
  std::vector<ResolvedSourceReference> source_refs;
};

// Parses every "[OBLIGATION]...[/OBLIGATION]" block in `response_text`.
// Expected block shape (case-insensitive keys, order-independent,
// multi-line values supported until the next recognized key or the block's
// end):
//
//   [OBLIGATION]
//   Party: ...                  (optional)
//   Obligation: ...
//   Trigger: ...                (optional)
//   Deadline: ...                (optional)
//   Sources: [SOURCE:DOC_1:CHUNK_1] [SOURCE:DOC_2:CHUNK_3]   (optional)
//   [/OBLIGATION]
//
// A block missing a non-blank Obligation is not an Obligation and is
// silently skipped - Master Spec Sec.72: "Do not force extraction if model
// output cannot be safely parsed. Fallback to source-aware text." Deadline
// is kept as whatever text the model produced, never parsed or validated as
// a calendar date (Sec.73's "no fake dates" governs Chronology's own date
// extraction, not this WP). response_text itself is never modified; this
// only ever adds a structured view on top of it.
//
// Every "[SOURCE:...]" marker in a block's Sources field is validated by
// calling `supplied_markers.validate_response()` on that exact substring -
// the identical registry/mechanism WP6-A/WP6-B already use for the whole
// response, never a second retrieval or a new citation format. A
// NotSupplied or Malformed marker cited inside a Sources field is dropped
// from that obligation's source_refs, not treated as a citation; the
// obligation itself is still produced when its Obligation text is
// otherwise valid.
[[nodiscard]] std::vector<ParsedObligation>
parse_obligations(const std::string &response_text, const SourceMarkerRegistry &supplied_markers);

} // namespace singularity::context
