#include "LocalInference/PlanEvaluation/PlanEvaluation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>

namespace singularity::local_inference {
namespace {

using Json = nlohmann::json;

std::optional<double> measurement_score(
		const PlanMeasurement &measurement,
		const WorkloadProfile &workload) {
	if (!measurement.safe || !measurement.correctness_passed) return std::nullopt;
	if (!workload.model_already_loaded && workload.intent == WorkloadIntent::Interactive &&
		measurement.request_ms) {
		return measurement.startup_ms + *measurement.request_ms;
	}
	if (workload.priority == WorkloadPriority::Throughput) {
		if (workload.phase_bias == WorkloadPhaseBias::PrefillHeavy &&
			measurement.prefill_tokens_per_second && *measurement.prefill_tokens_per_second > 0.0)
			return 1.0 / *measurement.prefill_tokens_per_second;
		if (measurement.decode_tokens_per_second && *measurement.decode_tokens_per_second > 0.0)
			return 1.0 / *measurement.decode_tokens_per_second;
	}
	if (measurement.request_ms) return measurement.request_ms;
	return measurement.time_to_first_token_ms;
}

Json optional_double(const std::optional<double> &value) {
	return value ? Json(*value) : Json(nullptr);
}

Json optional_uint(const std::optional<std::uint64_t> &value) {
	return value ? Json(*value) : Json(nullptr);
}

PlanMeasurement measurement_from_json(const Json &value) {
	PlanMeasurement result;
	result.plan_identity = value.value("planIdentity", "");
	result.startup_ms = value.value("startupMs", 0.0);
	if (value.contains("timeToFirstTokenMs") && !value["timeToFirstTokenMs"].is_null())
		result.time_to_first_token_ms = value["timeToFirstTokenMs"].get<double>();
	if (value.contains("prefillTokensPerSecond") && !value["prefillTokensPerSecond"].is_null())
		result.prefill_tokens_per_second = value["prefillTokensPerSecond"].get<double>();
	if (value.contains("decodeTokensPerSecond") && !value["decodeTokensPerSecond"].is_null())
		result.decode_tokens_per_second = value["decodeTokensPerSecond"].get<double>();
	if (value.contains("requestMs") && !value["requestMs"].is_null())
		result.request_ms = value["requestMs"].get<double>();
	if (value.contains("peakMemoryBytes") && !value["peakMemoryBytes"].is_null())
		result.peak_memory_bytes = value["peakMemoryBytes"].get<std::uint64_t>();
	if (value.contains("maximumResponsivenessDelayMs") && !value["maximumResponsivenessDelayMs"].is_null())
		result.maximum_responsiveness_delay_ms = value["maximumResponsivenessDelayMs"].get<double>();
	result.sample_count = value.value("sampleCount", 1ULL);
	result.relative_score_uncertainty = value.value("relativeScoreUncertainty", 0.0);
	result.correctness_passed = value.value("correctnessPassed", false);
	result.safe = value.value("safe", false);
	result.measured_at_epoch_seconds = value.value("measuredAtEpochSeconds", 0LL);
	return result;
}

} // namespace

std::string plan_measurement_cache_key(
		const std::string &machine_runtime_identity,
		const ModelExecutionProfile &model,
		const std::string &strategy_version,
		const WorkloadProfile &workload) {
	std::ostringstream key;
	key << machine_runtime_identity << '|' << model.artifact_identity << '|'
		<< strategy_version << '|' << to_string(workload.intent) << '|'
		<< to_string(workload.phase_bias) << '|' << workload.concurrency << '|'
		<< (workload.reusable_prefix_available ? "prefix" : "no-prefix") << '|'
		<< (workload.model_already_loaded ? "resident" : "cold-load");
	return key.str();
}

std::optional<PlanMeasurement> load_plan_measurement(
		const std::filesystem::path &cache_path,
		const std::string &cache_key,
		const std::string &plan_identity) {
	try {
		std::ifstream input(cache_path);
		if (!input) return std::nullopt;
		Json root;
		input >> root;
		if (!root.contains(cache_key) || !root[cache_key].contains(plan_identity)) return std::nullopt;
		return measurement_from_json(root[cache_key][plan_identity]);
	} catch (...) {
		return std::nullopt;
	}
}

void store_plan_measurement(
		const std::filesystem::path &cache_path,
		const std::string &cache_key,
		const PlanMeasurement &measurement) {
	try {
		Json root = Json::object();
		std::ifstream input(cache_path);
		if (input) input >> root;
		root[cache_key][measurement.plan_identity] = Json{
			{"planIdentity", measurement.plan_identity},
			{"startupMs", measurement.startup_ms},
			{"timeToFirstTokenMs", optional_double(measurement.time_to_first_token_ms)},
			{"prefillTokensPerSecond", optional_double(measurement.prefill_tokens_per_second)},
			{"decodeTokensPerSecond", optional_double(measurement.decode_tokens_per_second)},
			{"requestMs", optional_double(measurement.request_ms)},
			{"peakMemoryBytes", optional_uint(measurement.peak_memory_bytes)},
			{"maximumResponsivenessDelayMs", optional_double(measurement.maximum_responsiveness_delay_ms)},
			{"sampleCount", measurement.sample_count},
			{"relativeScoreUncertainty", measurement.relative_score_uncertainty},
			{"correctnessPassed", measurement.correctness_passed},
			{"safe", measurement.safe},
			{"measuredAtEpochSeconds", measurement.measured_at_epoch_seconds},
		};
		std::error_code error;
		std::filesystem::create_directories(cache_path.parent_path(), error);
		std::ofstream output(cache_path, std::ios::trunc);
		if (output) output << root.dump(2);
	} catch (...) {
		// Probe evidence is an optimization. A failed cache write cannot change
		// execution safety and is retried on a later run.
	}
}

PlanSelection select_candidate_plan(
		const std::vector<CandidatePlan> &candidates,
		const WorkloadProfile &workload,
		const std::optional<std::string> &current_plan_identity,
		const PlanStabilityPolicy &stability) {
	PlanSelection selection;
	double best_score = std::numeric_limits<double>::infinity();
	for (std::size_t index = 0; index < candidates.size(); ++index) {
		if (candidates[index].plan.feasibility == ExecutionFeasibility::Unsupported ||
			candidates[index].plan.feasibility == ExecutionFeasibility::NotSafelyRunnable) continue;
		if (candidates[index].cached_measurement) {
			const auto score = measurement_score(*candidates[index].cached_measurement, workload);
			if (score && *score < best_score) {
				best_score = *score;
				selection.selected_candidate = index;
				selection.used_real_measurement = true;
			}
		}
	}
	if (selection.selected_candidate) {
		selection.confidence = std::max(0.0, 1.0 -
			candidates[*selection.selected_candidate].cached_measurement->relative_score_uncertainty);
		if (current_plan_identity &&
			candidates[*selection.selected_candidate].plan.plan_identity != *current_plan_identity) {
			for (std::size_t index = 0; index < candidates.size(); ++index) {
				if (candidates[index].plan.plan_identity != *current_plan_identity ||
					!candidates[index].cached_measurement) continue;
				const auto current_score = measurement_score(*candidates[index].cached_measurement, workload);
				if (!current_score || *current_score <= 0.0) break;
				const double gain = (*current_score - best_score) / *current_score;
				const double uncertainty = std::max({stability.uncertainty_margin_fraction,
					candidates[index].cached_measurement->relative_score_uncertainty,
					candidates[*selection.selected_candidate].cached_measurement->relative_score_uncertainty});
				const double required_gain = stability.minimum_material_gain_fraction +
					stability.switching_cost_fraction + uncertainty;
				if (gain <= required_gain) {
					selection.selected_candidate = index;
					selection.confidence = std::max(0.0, 1.0 - uncertainty);
					selection.reason = "Kept the current safe plan because measured benefit did not exceed switching cost, uncertainty and hysteresis.";
					return selection;
				}
				break;
			}
		}
		selection.reason = "Selected from cached real-model evidence for this machine, runtime, artifact and workload class.";
		return selection;
	}
	for (std::size_t index = 0; index < candidates.size(); ++index) {
		if (candidates[index].plan.feasibility == ExecutionFeasibility::Unsupported ||
			candidates[index].plan.feasibility == ExecutionFeasibility::NotSafelyRunnable) continue;
		if (!selection.selected_candidate || candidates[index].predicted_rank < best_score) {
			best_score = candidates[index].predicted_rank;
			selection.selected_candidate = index;
		}
	}
	selection.confidence = selection.selected_candidate ? 0.35 : 0.0;
	selection.reason = selection.selected_candidate
		? "Selected from safety-filtered prediction; a short real-model probe can raise confidence."
		: "No safe supported candidate exists.";
	return selection;
}

} // namespace singularity::local_inference
