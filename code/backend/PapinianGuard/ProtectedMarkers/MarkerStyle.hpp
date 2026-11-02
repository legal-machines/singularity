#pragma once

// The style a protected symbol is written in (ProtectedMarkers.hpp), apart so
// the operation table can carry it without the marker readers.
namespace singularity::protected_api::papinian::markers {

enum class MarkerStyle { Typed, Compact };

// The style a new operation table issues its symbols in: typed - short, the
// kind visible in the symbol itself ([[Org1]], [[Person2]], [[Amount3]],
// [[Date4]], [[Rate2]], [[Number5]], [[Duration2]], [[Id3]]) - and every
// reader still reads the compact [[E17]] of stored operations and disclosures.
// A table whose material already writes a typed symbol's shape issues compact
// ones (markers::collides over the data it protects, never Singularity's own
// instructions). This constant is the switch for a live A/B.
inline constexpr MarkerStyle kMarkerStyle = MarkerStyle::Typed;

} // namespace singularity::protected_api::papinian::markers
