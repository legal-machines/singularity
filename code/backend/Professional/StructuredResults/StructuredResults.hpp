#pragma once

namespace singularity::professional {

// Which typed professional results an objective's OWN output contract
// includes: a risk review produces findings, an obligations extraction
// produces obligations, a chronology produces events, a comparison produces
// changes. These are semantics of the resolved objective - the Legal task the
// user chose in the composer and Singularity already records on every task
// (matters::MatterTask::legal_task_preset_id) - never a classification of the
// request's wording and never a second model call to decide (see
// ProfessionalExecutionIntent.hpp's own product law on that split).
//
// Required results are part of what the request IS, not decoration on top of
// it: they are never dropped to make a request fit a smaller model's context
// window. The same objective has the same professional semantics on every
// model; if the mandatory part genuinely cannot fit, the request is refused
// truthfully instead of quietly answering with less.
enum class StructuredResultKind : unsigned {
	None = 0,
	Findings = 1U << 0,
	Obligations = 1U << 1,
	Events = 1U << 2,
	Changes = 1U << 3,
};

using StructuredResultKinds = StructuredResultKind;

[[nodiscard]] constexpr StructuredResultKinds operator|(StructuredResultKinds left, StructuredResultKinds right) noexcept {
	return static_cast<StructuredResultKinds>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}

[[nodiscard]] constexpr bool includes(StructuredResultKinds set, StructuredResultKind kind) noexcept {
	return (static_cast<unsigned>(set) & static_cast<unsigned>(kind)) != 0U;
}

[[nodiscard]] constexpr bool any_structured_result_required(StructuredResultKinds set) noexcept {
	return static_cast<unsigned>(set) != 0U;
}

} // namespace singularity::professional
