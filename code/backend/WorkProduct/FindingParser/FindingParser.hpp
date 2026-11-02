#pragma once

#include "Context/SourceMarkers/SourceMarkers.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::context {

// One structured finding parsed from a model's response text (Master Spec
// Sec.25: "For Risk Review / DD tasks, optionally support"). Deliberately
// lacks `id`/`matterId` - those are assigned by the caller (see
// matters::Finding), not by the model or this parser.
struct ParsedFinding {
  std::string title;
  std::string description;
  std::optional<std::string> recommendation;
  std::optional<std::string> severity;
  // Every entry here was validated Supplied against the exact
  // SourceMarkerRegistry passed to parse_findings() - never a fresh
  // retrieval or a re-derivation against current Matter state, and never a
  // NotSupplied/Malformed marker (Master Spec Sec.22).
  std::vector<ResolvedSourceReference> source_refs;
};

// Parses every "[FINDING]...[/FINDING]" block in `response_text`. Expected
// block shape (case-insensitive keys, order-independent, multi-line values
// supported until the next recognized key or the block's end):
//
//   [FINDING]
//   Title: ...
//   Description: ...
//   Recommendation: ...          (optional)
//   Severity: ...                (optional)
//   Sources: [SOURCE:DOC_1:CHUNK_1] [SOURCE:DOC_2:CHUNK_3]   (optional)
//   [/FINDING]
//
// A block missing a non-blank Title or Description is not a Finding and is
// silently skipped - Master Spec Sec.25: "Do not require every AI response
// to parse into rigid JSON... never throw away useful work merely because
// Finding parsing failed." response_text itself is never modified; this
// only ever adds a structured view on top of it.
//
// Every "[SOURCE:...]" marker in a block's Sources field is validated by
// calling `supplied_markers.validate_response()` on that exact substring -
// the identical registry/mechanism WP6-A already uses for the whole
// response, never a second retrieval or a new citation format. A
// NotSupplied or Malformed marker cited inside a Sources field is dropped
// from that finding's source_refs, not treated as a citation; the finding
// itself is still produced when its title/description are otherwise valid.
[[nodiscard]] std::vector<ParsedFinding>
parse_findings(const std::string &response_text, const SourceMarkerRegistry &supplied_markers);

} // namespace singularity::context
