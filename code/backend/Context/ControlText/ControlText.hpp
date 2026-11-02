#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Singularity's own words to the model inside a request: the lines it writes after
// the lawyer's request to continue a task ("SINGULARITY: Continue the same task
// with these facts ... (2 more rounds can be answered)."), the headers of the
// facts and answers it returns, and the field labels of a proposal it
// reproduces for correction ("artifactAction.operations[0].expectedOldValue:").
//
// They are control text, not the lawyer's data, and Papinian Guard must never
// read them as data: read so, the model's own field name "localFacts" and the
// round count "2" went out as symbols ("[[E120]] is still there ... ([[E19]]
// more rounds)"), in 24 of 25 such lines of one run (26 Sep 2026).
//
// Every such line is written from a template here, and recognised by the same
// template: a span is control text only when it is exactly a template's
// words with each slot holding what its kind allows - a count of one to three
// figures, one of Singularity's own field names, one of Singularity's own lower-case
// protocol words. None of these can carry a word of the lawyer's material, so
// a line forged in data that happens to match carries nothing either. Anything
// else - a template with one word changed, a name in a slot - is not
// recognised, and is scored and masked as data. The data between control
// lines (facts, answers, the reproduced proposal's field text) stays data.
namespace singularity::context::control_text {

enum class Template {
	LocalFactsHeader,
	FactsAnswerNowRounds,
	FactsAnswerNowRound,
	FactsAnswerNowLast,
	FactsContinueRounds,
	FactsContinueRound,
	FactsContinueLast,
	FactsClosed,
	ProjectEvidenceHeader,
	ProjectEvidenceRepeated,
	ProjectEvidenceNoWindow,
	ProjectEvidenceExhausted,
	ProjectEvidenceMoreRequests,
	ProjectEvidenceMoreRequest,
	ProjectEvidenceLast,
	LawyerLeftHeader,
	LawyerLeftDecide,
	LawyerAnswerHeader,
	LawyerContinueMore,
	LawyerContinueNone,
	LawyerNoMoreQuestions,
	CorrectionHeader,
	CorrectionFooterCreate,
	CorrectionFooterUpdate,
	CompletionHeader,
	CompletionFooterCreate,
	CompletionFooterUpdate,
	FieldLabel,
	FieldLabelBlock,
	FieldEnd,
	FieldNone,
	// An operation Singularity worked out for the model (PapinianGuard/
	// ProtectedOperations): each line begins with its number and its op, then
	// its answer over symbols - exact, bounded, unknown (with a closed reason)
	// or refused.
	OperationResult,
	OperationBounded,
	OperationUnknown,
	OperationRefused,
	// The answer check: a figure no symbol, source or operation gives, or a
	// result other than the one an operation gave - one bounded correction.
	UntracedFigure,
	// What the lawyer reads beside the answer - the chat's status line, never
	// the work product - when a figure is still untraced after that correction
	// (DESIGN3 9.8 d): one figure, or {count} of them.
	FigureUnresolvedOne,
	FigureUnresolvedMany,

	// ---------------------------------------------------------------------
	// The protected computation protocol (DESIGN4). Declared by the seam with
	// placeholder words; each lane writes its own templates' words (and slots)
	// in ControlText.cpp. Nothing renders them yet.
	//
	// The results block and its rounds (DESIGN4 §1.4, §1.5, §8.5, §10; lane K):
	// the block's header, the line that continues the answer, the line when no
	// more operations can be worked out, each op's line by status, the closed
	// hints of a refusal or an error, the labels a resumed call lost, and an
	// identical failing op answered again from the log.
	ComputeHeader,
	ComputeContinueRounds,
	ComputeContinueRound,
	ComputeContinueLast,
	ComputeClosed,
	ComputeReopened,
	ComputeLineOk,
	ComputeLineRelation,
	ComputeLineBounded,
	ComputeLineBoundedFrom,
	ComputeLineBoundedTo,
	ComputeLineUnknown,
	ComputeLineError,
	ComputeLineRefused,
	ComputeRefusedConstantNotInMaterial,
	ComputeRefusedUnsafeLiteral,
	ComputeRefusedConstantDepth,
	ComputeRefusedDisclosureBudget,
	ComputeRefusedSameOperand,
	ComputeRefusedExternalOperand,
	ComputeRefusedOther,
	ComputeErrorHint,
	ComputeLabelsLost,
	ComputeRepeated,
	// Bounded and unknown wording and the calendar note (DESIGN4 §7.2, §7.3):
	// symbols, codes and closed words only - never a digit. Their words are
	// lane K's (DESIGN4 §13.1: every Compute* text); lane C sets what they
	// must say (§7.2) and tests that none carries a digit.
	ComputeBoundedDate,
	ComputeBoundedAsk,
	ComputeEqualAsWritten,
	ComputeBoundedFromDate,
	ComputeBoundedToDate,
	ComputeHolidayFromDate,
	ComputeHolidayToDate,
	ComputeBoundedFromValue,
	ComputeBoundedToValue,
	ComputeBoundedValue,
	ComputeUnknownReason,
	ComputeNotGoverningCalendar,
	// The tables block's line for sheets it does not describe, and a "same
	// role, two sources" line (DESIGN4 §6.2, §5.7; lane R).
	TablesMoreSheets,
	RoleCrossCheck,
	// The answer contract (DESIGN4 §9; lane V): the repair message, its
	// violation lines, its two soft nudges, the chat status line when a figure
	// was withheld, and the neutral gap a document shows instead.
	AnswerContractHeader,
	AnswerContractViolation,
	AnswerContractFooter,
	AnswerContractNudgeCompute,
	AnswerContractNudgeCrossSource,
	FigureWithheldOne,
	FigureWithheldMany,
	FigureGapAmountEn,
	FigureGapAmountRu,
	FigureGapDateEn,
	FigureGapDateRu,
	FigureGapRateEn,
	FigureGapRateRu,
	FigureGapOtherEn,
	FigureGapOtherRu,
};

// What a slot may hold. Count, FieldPath and Word are the cycle-3 kinds; the
// others are the protected computation protocol's (DESIGN4 §1.5, §8.5, §10),
// written in a template as {symbol}, {type}, {code}, {op}, {letter},
// {handle}, {label}. None of them can hold a figure of the lawyer's material:
//   Symbol   an issued symbol, "[[Amount12]]", "[[Person5.first]]" - the
//            protected form, never a local anchor or a value;
//   TypeName a type as compute::written() writes it: "MONEY(USD/month)",
//            "DURATION(business days)", "ROW_SET([[Table1]])" - digits only
//            inside a [[handle]];
//   Code     a closed lower-case code, "type_mismatch", "unsafe_literal";
//   OpId     an op id, "q1" .. "q999";
//   Letter   a column letter, "B", "AA";
//   Handle   a local handle: "[[Table1]]", "[[Rows2]]", "[[Groups1]]",
//            "[[Schedule1]]", "[[Calendar2]]", "DOC_3", "governing",
//            "calendar_days";
//   Label    a model's bind name, "base_rent_2026" - the §2.1 label rule:
//            [a-z][a-z0-9_]{0,31}, no run of 3+ digits but one 4-digit year.
struct Slot {
	enum class Kind { Count, FieldPath, Word, Symbol, TypeName, Code, OpId, Letter, Handle, Label };
	Kind kind = Kind::Count;
	std::string value;
};

struct ControlSpan {
	std::size_t from = 0;
	std::size_t to = 0;
};

// A template's words with its slots filled, in order. Throws when a slot's
// value is not of its kind: control text can never be made to carry data.
std::string render(Template id, const std::vector<Slot> &slots = {});

// Whether `value`, whole, is a value of `kind` - render()'s own test, for the
// lanes that build slots (a slot of any other shape is refused by render()).
[[nodiscard]] bool fits(Slot::Kind kind, std::string_view value);

// Every span of `text` that is exactly one of the templates, left to right and
// never overlapping.
std::vector<ControlSpan> recognise(std::string_view text);

// Data written into a continuation beside control text, with Singularity's own
// boundary token rendered as a literal - PromptAssembler's rule - so no data
// can open a section or pass for Singularity's own words.
std::string escape_data(std::string_view data);

} // namespace singularity::context::control_text
