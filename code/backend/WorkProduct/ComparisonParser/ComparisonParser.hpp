#pragma once

#include "Context/SourceMarkers/SourceMarkers.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::context {

// One structured comparison change parsed from a model's response text
// (Master Spec Sec.71). Deliberately lacks `id`/`matterId` - those are
// assigned by the caller (see matters::Change), not by the model or this
// parser, the same rule WP6-B/WP6-C/WP6-D's Finding/Obligation/Event already
// apply. Sec.71 gives no explicit domain struct in the spec text - only
// prose instructing what the model should be prompted to identify
// (substantive differences, added/removed obligations, changed dates/
// amounts, changed risk allocation, inconsistency) - so this shape is the
// smallest one consistent with that prose, not a literal transcription.
// `category` is kept as whatever free text the model produced (if any),
// never validated or restricted to a fixed enum - the same treatment
// Obligation's `trigger`/`deadline` already receive, and directly reflects
// Sec.71's own list of what to prompt for without turning it into a rigid
// taxonomy this parser would have to police.
struct ParsedChange {
  std::optional<std::string> category;
  std::string description;
  // Every entry here was validated Supplied against the exact
  // SourceMarkerRegistry passed to parse_comparison() - never a fresh
  // retrieval or a re-derivation against current Matter state (Master Spec
  // Sec.22), the identical contract WP6-A/WP6-B/WP6-C/WP6-D already use. A
  // single change may cite markers from more than one supplied document -
  // nothing in this parser or in SourceMarkerRegistry restricts a Sources
  // line to one document, since Sec.71 compares two or more documents at
  // once and a single substantive difference is often only expressible by
  // citing both sides.
  std::vector<ResolvedSourceReference> source_refs;
};

// Parses every "[CHANGE]...[/CHANGE]" block in `response_text`. Expected
// block shape (case-insensitive keys, order-independent, multi-line values
// supported until the next recognized key or the block's end):
//
//   [CHANGE]
//   Category: Changed risk allocation   (optional, free text)
//   Description: ...
//   Sources: [SOURCE:DOC_1:CHUNK_2] [SOURCE:DOC_2:CHUNK_5]   (optional)
//   [/CHANGE]
//
// A block missing a non-blank Description is not a Change and is silently
// skipped - Master Spec Sec.71 never requires Compare Documents to force a
// rigid structure onto every difference; response_text itself is never
// modified either way, so a comparison change is never a replacement for the
// natural-language response, only an additional structured view over it.
//
// `Category` is preserved exactly as the model wrote it - never mapped onto
// a fixed set of change types (e.g. no generic added/removed/modified/
// unchanged status is imposed here), since Sec.71's own list is prompt
// guidance for what to look for, not a wire-format taxonomy.
//
// Every "[SOURCE:...]" marker in a block's Sources field is validated by
// calling `supplied_markers.validate_response()` on that exact substring -
// the identical registry/mechanism WP6-A/WP6-B/WP6-C/WP6-D already use for
// the whole response, never a second retrieval or a new citation format. A
// NotSupplied or Malformed marker cited inside a Sources field is dropped
// from that change's source_refs, not treated as a citation; the change
// itself is still produced when its Description text is otherwise valid.
[[nodiscard]] std::vector<ParsedChange>
parse_comparison(const std::string &response_text, const SourceMarkerRegistry &supplied_markers);

} // namespace singularity::context
