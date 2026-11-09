#pragma once

#include <cstddef>

// How much protected computation one answer, one task and one matter may ask
// for (DESIGN4 §1.4, §8.3, §9.2).
//
// The observation limits are the privacy bound itself (DESIGN4 §8.4): at most
// four charged observations about any one hidden fact for the life of the
// matter, whatever the model asks and however it builds the question.
namespace singularity::protected_api::papinian::compute {

// Operations one reply may carry; the 25th and later are `too_many_ops`.
inline constexpr std::size_t kOpsPerRound = 24;
// Rounds of operations one provider call may be answered, and the rounds in
// which no operation could be answered (a repair) on top of them.
inline constexpr std::size_t kComputeRoundsPerCall = 4;
inline constexpr std::size_t kRepairRoundsPerCall = 2;
// The same over every call of one task (a draft, its review, a redraft).
inline constexpr std::size_t kComputeRoundsPerTask = 10;
inline constexpr std::size_t kOpsPerTask = 160;
// Charged observations, for the life of the matter: per hidden root fact, per
// date root on its calendar budget (part of the root's own), per public
// grouping key; and per task.
inline constexpr std::size_t kObservationsPerRoot = 4;
inline constexpr std::size_t kCalendarObservationsPerDateRoot = 4;
inline constexpr std::size_t kObservationsPerGroupKey = 12;
inline constexpr std::size_t kObservationsPerTask = 40;
// What a preload (the relations planner, the legend's timeline) may spend of
// those same limits - never more, never a budget of its own: it stops where
// half of each is left, so an operation the lawyer's task needs is never the
// one refused because a relation it did not ask for came first (owner, 28 Sep
// 2026: on-demand computation, then completeness repair, then preload).
inline constexpr std::size_t kPreloadObservationsPerRoot = kObservationsPerRoot / 2;
inline constexpr std::size_t kPreloadCalendarObservationsPerDateRoot = kCalendarObservationsPerDateRoot / 2;
inline constexpr std::size_t kPreloadObservationsPerGroupKey = kObservationsPerGroupKey / 2;
inline constexpr std::size_t kPreloadObservationsPerTask = kObservationsPerTask / 2;
// The size of what may be worked out, decided from the operation alone before
// anything is evaluated.
inline constexpr std::size_t kMaxDerivationDepth = 8;
inline constexpr std::size_t kMaxBitsPerValue = 4096;
inline constexpr std::size_t kMaxConstantsPerTree = 2;
inline constexpr std::size_t kMaxNormalisedMonomials = 512;
// Tables: the rows one table model holds, and the rows or groups one answer
// lists.
inline constexpr std::size_t kRowsPerTable = 5000;
inline constexpr std::size_t kListedRows = 24;
// The request's own shape (DESIGN4 §2.1): arguments and filter atoms of one
// operation, the bytes of its "why", decimal places, a result's name.
inline constexpr std::size_t kMaxArguments = 64;
inline constexpr std::size_t kMaxWhereAtoms = 8;
inline constexpr std::size_t kMaxWhyBytes = 120;
inline constexpr int kMaxPlaces = 9;
inline constexpr std::size_t kMaxLabelLength = 48;
// Corrections the answer contract may ask for in one call (DESIGN4 §9.2).
inline constexpr std::size_t kAnswerContractRepairs = 2;
// Derived facts a later call's symbol table lists (DESIGN4 §4.5), and the
// "same role, two sources" lines one payload carries (DESIGN4 §5.7).
inline constexpr std::size_t kDerivedFactsListed = 64;
inline constexpr std::size_t kCrossCheckLines = 12;

// The budgets one operation's protected computation runs under
// (InferenceRequest::compute_budget). Unset on a request, the protocol is not
// offered at all and the request runs exactly as before it existed.
struct ComputeBudgetLimits {
	std::size_t ops_per_round = kOpsPerRound;
	std::size_t compute_rounds_per_call = kComputeRoundsPerCall;
	std::size_t repair_rounds_per_call = kRepairRoundsPerCall;
	std::size_t compute_rounds_per_task = kComputeRoundsPerTask;
	std::size_t ops_per_task = kOpsPerTask;
	std::size_t answer_contract_repairs = kAnswerContractRepairs;
};

} // namespace singularity::protected_api::papinian::compute
