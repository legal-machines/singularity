#include "Agent/TextDiff/TextDiff.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace singularity::agents {
namespace {

enum class StepKind { Same, Left, Came };

struct Step {
	StepKind kind = StepKind::Same;
	std::size_t before_index = 0;
	std::size_t after_index = 0;
};

// Each distinct paragraph text gets one number, so the comparison below
// compares numbers, not strings.
std::vector<int> numbered(const std::vector<TextBlock> &blocks, std::unordered_map<std::string, int> &numbers) {
	std::vector<int> result;
	result.reserve(blocks.size());
	for (const TextBlock &block : blocks) {
		const std::string key = normalize_block_text(block.text);
		const auto found = numbers.find(key);
		if (found != numbers.end()) {
			result.push_back(found->second);
			continue;
		}
		const int number = static_cast<int>(numbers.size());
		numbers.emplace(key, number);
		result.push_back(number);
	}
	return result;
}

// Myers' O((N+M)D) shortest edit script between before[from, before.size() -
// tail) and after[from, after.size() - tail). Returns false when the two
// differ by more than `ceiling` edits; the trace it would need grows with the
// square of that number.
bool shortest_edit(const std::vector<int> &before, const std::vector<int> &after, std::size_t from, std::size_t tail,
	std::size_t ceiling, std::vector<Step> &steps) {
	const std::int32_t n = static_cast<std::int32_t>(before.size() - from - tail);
	const std::int32_t m = static_cast<std::int32_t>(after.size() - from - tail);
	const std::int32_t most = std::min<std::int32_t>(n + m, static_cast<std::int32_t>(ceiling));
	const std::int32_t offset = n + m + 1;
	std::vector<std::int32_t> furthest(static_cast<std::size_t>(2 * offset + 2), 0);
	// trace[d] is `furthest` as it stood before step d, kept only over the
	// diagonals step d reads: -d-1 .. d+1.
	std::vector<std::vector<std::int32_t>> trace;
	std::int32_t found = -1;
	for (std::int32_t d = 0; d <= most && found < 0; ++d) {
		trace.emplace_back(furthest.begin() + (offset - d - 1), furthest.begin() + (offset + d + 2));
		for (std::int32_t k = -d; k <= d; k += 2) {
			std::int32_t x = 0;
			if (k == -d || (k != d && furthest[offset + k - 1] < furthest[offset + k + 1])) x = furthest[offset + k + 1];
			else x = furthest[offset + k - 1] + 1;
			std::int32_t y = x - k;
			while (x < n && y < m && before[from + x] == after[from + y]) {
				++x;
				++y;
			}
			furthest[offset + k] = x;
			if (x >= n && y >= m) {
				found = d;
				break;
			}
		}
	}
	if (found < 0) return false;

	// Walk back from the end along the recorded frontiers.
	std::vector<Step> reversed;
	std::int32_t x = n;
	std::int32_t y = m;
	for (std::int32_t d = found; d >= 0; --d) {
		const std::vector<std::int32_t> &previous = trace[static_cast<std::size_t>(d)];
		const auto at = [&](std::int32_t k) { return previous[static_cast<std::size_t>(k + d + 1)]; };
		const std::int32_t k = x - y;
		std::int32_t previousK = 0;
		if (k == -d || (k != d && at(k - 1) < at(k + 1))) previousK = k + 1;
		else previousK = k - 1;
		const std::int32_t previousX = d == 0 ? 0 : at(previousK);
		const std::int32_t previousY = d == 0 ? 0 : previousX - previousK;
		while (x > previousX && y > previousY) {
			reversed.push_back({StepKind::Same, from + static_cast<std::size_t>(x - 1), from + static_cast<std::size_t>(y - 1)});
			--x;
			--y;
		}
		if (d == 0) break;
		if (x == previousX) reversed.push_back({StepKind::Came, from + static_cast<std::size_t>(x), from + static_cast<std::size_t>(y - 1)});
		else reversed.push_back({StepKind::Left, from + static_cast<std::size_t>(x - 1), from + static_cast<std::size_t>(y)});
		x = previousX;
		y = previousY;
	}
	steps.assign(reversed.rbegin(), reversed.rend());
	return true;
}

void keep_quoted(std::vector<TextBlock> &blocks, std::size_t &omitted, std::size_t most) {
	if (blocks.size() <= most) return;
	omitted = blocks.size() - most;
	blocks.resize(most);
}

} // namespace

std::string normalize_block_text(const std::string &text) {
	std::string result;
	result.reserve(text.size());
	bool isPendingSpace = false;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const unsigned char character = static_cast<unsigned char>(text[index]);
		// A no-break space (U+00A0) is spacing too: Word writes it often.
		const bool isNoBreakSpace = character == 0xC2 && index + 1 < text.size()
			&& static_cast<unsigned char>(text[index + 1]) == 0xA0;
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f'
				|| character == '\v' || isNoBreakSpace == true) {
			if (isNoBreakSpace == true) ++index;
			isPendingSpace = true;
			continue;
		}
		if (isPendingSpace == true && result.empty() == false) result += ' ';
		isPendingSpace = false;
		result += static_cast<char>(character);
	}
	return result;
}

TextDiffResult diff_text_blocks(const std::vector<TextBlock> &before, const std::vector<TextBlock> &after,
	const TextDiffLimits &limits) {
	TextDiffResult result;
	std::unordered_map<std::string, int> numbers;
	const std::vector<int> left = numbered(before, numbers);
	const std::vector<int> right = numbered(after, numbers);

	// What both versions begin and end with is the same; only the middle is
	// compared.
	std::size_t from = 0;
	while (from < left.size() && from < right.size() && left[from] == right[from]) ++from;
	std::size_t tail = 0;
	while (tail < left.size() - from && tail < right.size() - from
			&& left[left.size() - 1 - tail] == right[right.size() - 1 - tail])
		++tail;
	if (from == left.size() && from == right.size()) return result;

	std::vector<Step> steps;
	if (shortest_edit(left, right, from, tail, limits.edit_ceiling, steps) == false) {
		result.is_rewritten = true;
		return result;
	}

	// Consecutive edits are one change; the unchanged paragraph before them is
	// its place in the document.
	TextChange current;
	bool isOpen = false;
	std::size_t lastSameBefore = from;
	bool hasLastSame = from > 0;
	if (hasLastSame == true) lastSameBefore = from - 1;
	const auto close = [&]() {
		if (isOpen == false) return;
		keep_quoted(current.before, current.omitted_before, limits.paragraphs_per_change);
		keep_quoted(current.after, current.omitted_after, limits.paragraphs_per_change);
		if (result.changes.size() < limits.changes_per_document) result.changes.push_back(current);
		else ++result.omitted_changes;
		current = TextChange();
		isOpen = false;
	};
	for (const Step &step : steps) {
		if (step.kind == StepKind::Same) {
			close();
			lastSameBefore = step.before_index;
			hasLastSame = true;
			continue;
		}
		if (isOpen == false) {
			isOpen = true;
			if (hasLastSame == true) current.context.push_back(before[lastSameBefore]);
		}
		if (step.kind == StepKind::Left) current.before.push_back(before[step.before_index]);
		else current.after.push_back(after[step.after_index]);
	}
	close();
	return result;
}

} // namespace singularity::agents
