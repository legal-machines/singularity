#pragma once

#include "Context/RussianNames/RussianNames.hpp"

#include <cstddef>
#include <string>
#include <vector>

// Restored Russian names in the form their place in the sentence needs,
// written on the lawyer's computer.
//
// A provider's model writes a person or a company as a symbol ([[E5]]) and
// cannot decline what it never sees; restoration then puts back one writing of
// the name for every place it stands. A live Russian letter read «Уважаемый(ая)
// Лебедеву А. И.,» and «С уважением, Зайцевой О. Н.» (24 Sep 2026). Here each
// restored name takes the case its position asks for - a preposition or a
// fixed phrase before it, the role noun it stands with («Генеральному
// директору … Лебедеву А. И.»), a salutation, a signature, an address block -
// and the words that agree with it («Уважаемый(ая)», «подготовил(а)»,
// «действующего») follow its gender. Nothing about the person leaves the
// device to make that possible, and nothing else in the text is changed.
namespace singularity::protected_api::papinian::restoration_grammar {

enum class SpanKind { Person, Organisation };

// A name restoration wrote into the text.
struct RestoredSpan {
	// Where it stands in the text, in bytes.
	std::size_t offset = 0;
	std::size_t length = 0;
	SpanKind kind = SpanKind::Person;
	// The fullest writing of the name known locally, in any case («Лебедев
	// Андрей Игоревич», «Зайцевой О. Н.»); a person is written in its style
	// (a full name, or initials). Empty: the span's own text.
	std::string canonical;
	// Decided by the caller from the fullest writing; Unknown lets the name's
	// own words decide, and a person whose gender cannot be told is left as it is.
	context::russian_names::Gender gender = context::russian_names::Gender::Unknown;
};

// `text` with every span written in the form its position requires, and the
// gendered alternatives and participles next to each span agreed with it.
// Text without Cyrillic, and spans that overlap one another, are left as they are.
[[nodiscard]] std::string agree(const std::string &text, const std::vector<RestoredSpan> &spans);

} // namespace singularity::protected_api::papinian::restoration_grammar
