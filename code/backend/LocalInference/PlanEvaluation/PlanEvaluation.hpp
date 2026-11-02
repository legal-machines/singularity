#pragma once

#include "LocalInference/ExecutionPlanner/ExecutionPlanner.hpp"
#include "LocalInference/WorkloadProfile/WorkloadProfile.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace singularity::local_inference {

struct PlanMeasurement {
	std::string plan_identity;
	double startup_ms = 0.0;
	std::optional<double> time_to_first_token_ms;
	std::optional<double> prefill_tokens_per_second;
	std::optional<double> decode_tokens_per_second;
	std::optional<double> request_ms;
	std::optional<std::uint64_t> peak_memory_bytes;
	std::optional<double> maximum_responsiveness_delay_ms;
	std::uint64_t sample_count = 1;
	double relative_score_uncertainty = 0.0;
	bool correctness_passed = false;
	bool safe = false;
	long long measured_at_epoch_seconds = 0;
};

struct CandidatePlan {
	AdaptiveExecutionPlan plan;
	std::optional<PlanMeasurement> cached_measurement;
	double predicted_rank = 0.0;
};

struct PlanSelection {
	std::optional<std::size_t> selected_candidate;
	double confidence = 0.0;
	bool used_real_measurement = false;
	std::string reason;
};

struct PlanStabilityPolicy {
	double minimum_material_gain_fraction = 0.05;
	double switching_cost_fraction = 0.0;
	double uncertainty_margin_fraction = 0.0;
};

std::string plan_measurement_cache_key(
	const std::string &machine_runtime_identity,
	const ModelExecutionProfile &model,
	const std::string &strategy_version,
	const WorkloadProfile &workload);

std::optional<PlanMeasurement> load_plan_measurement(
	const std::filesystem::path &cache_path,
	const std::string &cache_key,
	const std::string &plan_identity);

void store_plan_measurement(
	const std::filesystem::path &cache_path,
	const std::string &cache_key,
	const PlanMeasurement &measurement);

PlanSelection select_candidate_plan(
	const std::vector<CandidatePlan> &candidates,
	const WorkloadProfile &workload,
	const std::optional<std::string> &current_plan_identity = std::nullopt,
	const PlanStabilityPolicy &stability = {});

} // namespace singularity::local_inference
