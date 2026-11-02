#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace singularity::protected_api::papinian {

// Finds every occurrence of many literal patterns in one pass over the text
// (Aho-Corasick), where the obvious way is to search the whole text once per
// pattern. A request carries hundreds of names the Project and this operation
// already know, and every one of them used to be looked for separately in
// every passage: the cost was the number of names multiplied by the length of
// the request, and it grew with both. One pass costs the length of the text
// plus the matches found.
//
// It answers exactly what the repeated search answered: every occurrence of
// every pattern, including occurrences that overlap or nest inside one another
// ("Holt" inside "Holt Capital"), in the order they stand in the text, each
// with the tag its pattern was added under. What to do with an overlap is the
// caller's decision, as it was before.
class PatternScanner final {
public:
	// A pattern may be added more than once under different tags; every tag is
	// reported. An empty pattern is ignored: it matches nothing.
	void add(std::string_view pattern, std::size_t tag) {
		if (pattern.empty()) return;
		std::size_t node = 0;
		for (const char character : pattern) {
			const auto byte = static_cast<unsigned char>(character);
			const auto found = nodes_[node].next.find(byte);
			if (found != nodes_[node].next.end()) {
				node = found->second;
				continue;
			}
			// Growing the table can move it, so nothing already taken from it may
			// be held across the push.
			nodes_.push_back(Node{});
			const std::size_t created = nodes_.size() - 1;
			nodes_[node].next.emplace(byte, created);
			node = created;
		}
		nodes_[node].matches.push_back({pattern.size(), tag});
		built_ = false;
	}

	[[nodiscard]] bool empty() const noexcept { return nodes_.size() == 1; }

	// Breadth-first failure links, and each node inherits the matches of the
	// longest proper suffix that is itself a pattern, so a scan reports nested
	// patterns without walking the failure chain per byte.
	void build() {
		if (built_) return;
		std::vector<std::size_t> queue;
		queue.reserve(nodes_.size());
		for (auto &[byte, child] : nodes_[0].next) {
			nodes_[child].fail = 0;
			queue.push_back(child);
		}
		for (std::size_t head = 0; head < queue.size(); ++head) {
			const std::size_t node = queue[head];
			// Copying the failure node's matches keeps the scan a single step per
			// byte; a node has few matches, and the table is built once.
			const std::size_t fail = nodes_[node].fail;
			const std::vector<Match> inherited = nodes_[fail].matches;
			nodes_[node].matches.insert(nodes_[node].matches.end(), inherited.begin(), inherited.end());
			for (auto &[byte, child] : nodes_[node].next) {
				std::size_t candidate = nodes_[node].fail;
				for (;;) {
					const auto found = nodes_[candidate].next.find(byte);
					if (found != nodes_[candidate].next.end() && found->second != child) {
						nodes_[child].fail = found->second;
						break;
					}
					if (candidate == 0) {
						nodes_[child].fail = 0;
						break;
					}
					candidate = nodes_[candidate].fail;
				}
				queue.push_back(child);
			}
		}
		built_ = true;
	}

	// `found(position, length, tag)` for every occurrence, left to right.
	void scan(std::string_view text,
			const std::function<void(std::size_t, std::size_t, std::size_t)> &found) const {
		if (nodes_.size() == 1) return;
		std::size_t node = 0;
		for (std::size_t index = 0; index < text.size(); ++index) {
			const auto byte = static_cast<unsigned char>(text[index]);
			for (;;) {
				const auto next = nodes_[node].next.find(byte);
				if (next != nodes_[node].next.end()) {
					node = next->second;
					break;
				}
				if (node == 0) break;
				node = nodes_[node].fail;
			}
			for (const Match &match : nodes_[node].matches)
				found(index + 1 - match.length, match.length, match.tag);
		}
	}

private:
	struct Match {
		std::size_t length = 0;
		std::size_t tag = 0;
	};
	struct Node {
		std::unordered_map<unsigned char, std::size_t> next;
		std::vector<Match> matches;
		std::size_t fail = 0;
	};
	std::vector<Node> nodes_{Node{}};
	bool built_ = false;
};

} // namespace singularity::protected_api::papinian
