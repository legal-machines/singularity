#pragma once

#include "Platform/AppPaths/AppPaths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace singularity::local_model {

// How fast a local model really runs ON THIS MACHINE.
//
// Everything that depended on this figure asserted it instead of measuring it.
// The one that mattered was the generation ceiling: it was calculated from
// "~104 tokens a second of prefill", a number taken on an unthrottled run,
// while a real governed run on the same hardware measured 17.5 - six times
// slower. A genuine turn was then killed at the ceiling as if it had stopped
// converging, after several minutes of real work.
//
// So it is measured, from the generations Singularity actually performs, and kept:
// the SLOWEST rate seen for a model, because a ceiling exists for the worst
// case and an average would kill the worst case again. Nothing here estimates;
// with nothing measured yet, callers keep whatever default they had.
//
// Never throws. A store that cannot be read or written degrades to "nothing
// measured", which is exactly the first-launch state.
struct LocalThroughput {
	// Tokens per second, as measured. Zero means never measured.
	double slowest_prefill_tokens_per_second = 0.0;
	double slowest_decode_tokens_per_second = 0.0;
	// The longest a generation has actually taken and COMPLETED on this machine.
	// A ceiling is a statement about the worst case, and this is the worst case
	// that really happened, rather than one calculated from a benchmark.
	double slowest_completed_generation_seconds = 0.0;
	std::uint64_t generations_seen = 0;
};

namespace local_throughput_detail {

inline std::mutex &guard() {
	static std::mutex mutex;
	return mutex;
}

inline std::filesystem::path store_path() {
	std::error_code error;
	const std::filesystem::path directory = platform::application_data_directory();
	std::filesystem::create_directories(directory, error);
	return directory / "local-throughput.json";
}

inline nlohmann::json read_all() {
	std::ifstream file(store_path());
	if (!file.is_open()) return nlohmann::json::object();
	const nlohmann::json parsed = nlohmann::json::parse(file, nullptr, false);
	return parsed.is_object() ? parsed : nlohmann::json::object();
}

} // namespace local_throughput_detail

// What has been measured for `model_id`, or nothing.
[[nodiscard]] inline std::optional<LocalThroughput> observed_local_throughput(const std::string &model_id) {
	if (model_id.empty()) return std::nullopt;
	try {
		const std::lock_guard<std::mutex> lock(local_throughput_detail::guard());
		const nlohmann::json all = local_throughput_detail::read_all();
		if (!all.contains(model_id) || !all.at(model_id).is_object()) return std::nullopt;
		const nlohmann::json &entry = all.at(model_id);
		LocalThroughput observed;
		observed.slowest_prefill_tokens_per_second = entry.value("slowestPrefillTokensPerSecond", 0.0);
		observed.slowest_decode_tokens_per_second = entry.value("slowestDecodeTokensPerSecond", 0.0);
		observed.slowest_completed_generation_seconds =
			entry.value("slowestCompletedGenerationSeconds", 0.0);
		observed.generations_seen = entry.value("generationsSeen", std::uint64_t{0});
		if (observed.slowest_prefill_tokens_per_second <= 0.0
				&& observed.slowest_decode_tokens_per_second <= 0.0)
			return std::nullopt;
		return observed;
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

// Records one real generation. A phase with no tokens or no time is not a
// measurement and is ignored rather than recorded as infinitely fast.
inline void record_local_generation_throughput(const std::string &model_id,
		std::uint64_t evaluated_prompt_tokens, double prompt_ms,
		std::uint64_t generated_tokens, double generation_ms) {
	if (model_id.empty()) return;
	const double prefill = evaluated_prompt_tokens > 0 && prompt_ms > 0.0
		? static_cast<double>(evaluated_prompt_tokens) / (prompt_ms / 1000.0) : 0.0;
	const double decode = generated_tokens > 0 && generation_ms > 0.0
		? static_cast<double>(generated_tokens) / (generation_ms / 1000.0) : 0.0;
	if (prefill <= 0.0 && decode <= 0.0) return;
	try {
		const std::lock_guard<std::mutex> lock(local_throughput_detail::guard());
		nlohmann::json all = local_throughput_detail::read_all();
		nlohmann::json entry = all.contains(model_id) && all.at(model_id).is_object()
			? all.at(model_id) : nlohmann::json::object();
		const double storedPrefill = entry.value("slowestPrefillTokensPerSecond", 0.0);
		const double storedDecode = entry.value("slowestDecodeTokensPerSecond", 0.0);
		if (prefill > 0.0)
			entry["slowestPrefillTokensPerSecond"] = storedPrefill <= 0.0 ? prefill : std::min(storedPrefill, prefill);
		if (decode > 0.0)
			entry["slowestDecodeTokensPerSecond"] = storedDecode <= 0.0 ? decode : std::min(storedDecode, decode);
		const double seconds = (prompt_ms + generation_ms) / 1000.0;
		if (seconds > entry.value("slowestCompletedGenerationSeconds", 0.0))
			entry["slowestCompletedGenerationSeconds"] = seconds;
		entry["generationsSeen"] = entry.value("generationsSeen", std::uint64_t{0}) + 1;
		all[model_id] = std::move(entry);
		std::ofstream file(local_throughput_detail::store_path(), std::ios::trunc);
		if (file.is_open()) file << all.dump();
	} catch (const std::exception &) {
		// A machine that cannot record its own speed keeps the default ceiling.
	}
}

// The slowest generation that has completed on this machine, across every
// local model it has run. The generation ceiling is a property of the machine,
// not of one model: whichever model was slowest is the one the ceiling has to
// survive. Zero when nothing has been measured yet.
[[nodiscard]] inline double slowest_completed_local_generation_seconds() {
	try {
		const std::lock_guard<std::mutex> lock(local_throughput_detail::guard());
		const nlohmann::json all = local_throughput_detail::read_all();
		double slowest = 0.0;
		for (const auto &[model, entry] : all.items()) {
			(void)model;
			if (!entry.is_object()) continue;
			slowest = std::max(slowest, entry.value("slowestCompletedGenerationSeconds", 0.0));
		}
		return slowest;
	} catch (const std::exception &) {
		return 0.0;
	}
}

} // namespace singularity::local_model
