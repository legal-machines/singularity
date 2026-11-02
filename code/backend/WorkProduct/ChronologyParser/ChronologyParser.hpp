#pragma once

#include "Context/SourceMarkers/SourceMarkers.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::context {

// One structured chronology event parsed from a model's response text
// (Master Spec Sec.73). Deliberately lacks `id`/`matterId` - those are
// assigned by the caller (see matters::Event), not by the model or this
// parser, the same rule WP6-B/WP6-C's Finding/Obligation already apply.
// `date` is kept as whatever text the model produced, exactly as supplied -
// never parsed, normalized, or inferred. Master Spec Sec.73: "No fake
// dates." An event with no discernible date leaves `date` unset rather than
// guessing one from document order, surrounding Matter metadata, or task
// creation time.
struct ParsedEvent {
  std::optional<std::string> date;
  std::string description;
  // Every entry here was validated Supplied against the exact
  // SourceMarkerRegistry passed to parse_chronology() - never a fresh
  // retrieval or a re-derivation against current Matter state (Master Spec
  // Sec.22), the identical contract WP6-A/WP6-B/WP6-C already use.
  std::vector<ResolvedSourceReference> source_refs;
};

// Parses every "[EVENT]...[/EVENT]" block in `response_text`. Expected
// block shape (case-insensitive keys, order-independent, multi-line values
// supported until the next recognized key or the block's end):
//
//   [EVENT]
//   Date: 15 August 2026        (optional)
//   Description: ...
//   Sources: [SOURCE:DOC_1:CHUNK_2]   (optional)
//   [/EVENT]
//
// A block missing a non-blank Description is not an Event and is silently
// skipped - Master Spec Sec.73: store/display as structured chronology only
// when parsing succeeds. response_text itself is never modified; this only
// ever adds a structured view on top of it, so a chronology event is never a
// replacement for the natural-language response, only an additional
// structured view over it.
//
// `Date` is preserved exactly as the model wrote it - never parsed into a
// calendar type, never normalized to a more precise form than supplied, and
// never fabricated when absent. This is a deliberately conservative choice:
// canonical date normalization is a distinct feature the Master Spec does
// not require here.
//
// Every "[SOURCE:...]" marker in a block's Sources field is validated by
// calling `supplied_markers.validate_response()` on that exact substring -
// the identical registry/mechanism WP6-A/WP6-B/WP6-C already use for the
// whole response, never a second retrieval or a new citation format. A
// NotSupplied or Malformed marker cited inside a Sources field is dropped
// from that event's source_refs, not treated as a citation; the event
// itself is still produced when its Description text is otherwise valid.
[[nodiscard]] std::vector<ParsedEvent>
parse_chronology(const std::string &response_text, const SourceMarkerRegistry &supplied_markers);

} // namespace singularity::context
