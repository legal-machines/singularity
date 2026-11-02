#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace singularity::protected_api::papinian {

// Where a restored amount or date stands in a text the lawyer's computer
// already holds (a stored answer), and which fact of the material it is: the
// fact's key, a digest of where and in what role the material states it
// (FactProvenance, PapinianGuardService.hpp). Never a symbol, never the value,
// never the raw->symbol mapping: an offset, a length and a digest. The next
// turn's protection reads it to send that occurrence as the same fact again.
struct FactAnchor {
	std::size_t start = 0;
	std::size_t length = 0;
	std::string key;
};

namespace fact_anchor_marks {

// A fact anchor written into a text the next turn sends: ⟪writing|fact:key⟫ in
// prose, ⟦writing|fact:key⟧ where the writing already stands as a statement's
// operand. Local only: protection replaces the whole mark with the fact's
// symbol, and a send nothing protects writes the writing back alone.
inline constexpr std::string_view kOpen = "\xE2\x9F\xAA";          // ⟪ U+27EA
inline constexpr std::string_view kClose = "\xE2\x9F\xAB";         // ⟫ U+27EB
inline constexpr std::string_view kOperandOpen = "\xE2\x9F\xA6";   // ⟦ U+27E6
inline constexpr std::string_view kOperandClose = "\xE2\x9F\xA7";  // ⟧ U+27E7
inline constexpr std::string_view kKey = "|fact:";

// Whether `key` is a fact key as Singularity writes one: hexadecimal digits.
inline bool is_key(std::string_view key) {
	return !key.empty() && key.size() <= 64 && std::all_of(key.begin(), key.end(), [](char character) {
		return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
	});
}

} // namespace fact_anchor_marks

// `anchors` over `from`, moved onto `to`: each anchor's writing, and which
// occurrence of that writing it is in `from`, found again in `to`. An anchor
// whose writing `to` does not hold that many times is dropped - that
// occurrence is then protected as a fact of its own, still masked.
//
// By rank, so only for a `to` that keeps every writing of `from`, in order,
// and adds none: `from` unwrapped from its {"type":"final","text":...}
// envelope, or unescaped. Where Singularity takes words out or writes its own in
// (a figure left out, a note, a citation's label), an occurrence taken out
// would pass its rank to the next one - another fact of the same writing -
// so such an edit carries anchors by position (carry_fact_anchors).
inline std::vector<FactAnchor> relocate_fact_anchors(const std::string &from, const std::vector<FactAnchor> &anchors,
		const std::string &to) {
	std::vector<FactAnchor> moved;
	for (const FactAnchor &anchor : anchors) {
		if (anchor.length == 0 || anchor.key.empty() || anchor.start + anchor.length > from.size()) continue;
		const std::string writing = from.substr(anchor.start, anchor.length);
		std::size_t ordinal = 0;
		for (std::size_t at = from.find(writing); at != std::string::npos && at < anchor.start; at = from.find(writing, at + 1))
			++ordinal;
		std::size_t found = to.find(writing);
		for (std::size_t skipped = 0; found != std::string::npos && skipped < ordinal; ++skipped)
			found = to.find(writing, found + 1);
		if (found == std::string::npos) continue;
		moved.push_back(FactAnchor{found, anchor.length, anchor.key});
	}
	std::sort(moved.begin(), moved.end(), [](const FactAnchor &left, const FactAnchor &right) {
		return left.start < right.start;
	});
	// Two anchors can never claim one place.
	std::vector<FactAnchor> kept;
	for (FactAnchor &anchor : moved) {
		if (!kept.empty() && anchor.start < kept.back().start + kept.back().length) continue;
		kept.push_back(std::move(anchor));
	}
	return kept;
}

namespace fact_anchor_detail {

// A text as the pieces an edit keeps or changes: a run of ASCII letters and
// digits, or any other one character (UTF-8), with where each starts.
struct Pieces {
	std::vector<int> ids;
	std::vector<std::size_t> starts;  // one more than ids: the text's end last
};

inline Pieces pieces_of(const std::string &text, std::unordered_map<std::string_view, int> &interned) {
	const auto word = [](unsigned char byte) {
		return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
	};
	Pieces pieces;
	for (std::size_t at = 0; at < text.size();) {
		std::size_t end = at + 1;
		const auto byte = static_cast<unsigned char>(text[at]);
		if (word(byte)) {
			while (end < text.size() && word(static_cast<unsigned char>(text[end]))) ++end;
		} else if (byte >= 0xC0) {
			while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
		}
		const std::string_view piece(text.data() + at, end - at);
		const auto [found, added] = interned.emplace(piece, static_cast<int>(interned.size()));
		(void)added;
		pieces.ids.push_back(found->second);
		pieces.starts.push_back(at);
		at = end;
	}
	pieces.starts.push_back(text.size());
	return pieces;
}

// For each piece of `a`, the piece of `b` it is kept as, or -1 where it is
// taken out: the longest run of pieces the two share is kept first, then the
// longest on either side of it, and so on (as difflib matches) - so a
// sentence kept whole stays whole, and a lone figure never pairs with another
// writing of it just because that makes the fewest edits.
inline std::vector<long> kept_pieces(const std::vector<int> &a, const std::vector<int> &b) {
	constexpr std::size_t kMostRuns = 4096;
	std::vector<long> kept(a.size(), -1);
	// Rolling hashes of every prefix, to find a shared run of a given length.
	// Static, so that the lambdas below read it without capturing it (MSVC asks
	// for a capture of a local constant once an overloaded operator* is in view).
	static constexpr std::uint64_t kBase = 1000003ULL;
	const auto prefixes = [](const std::vector<int> &pieces) {
		std::vector<std::uint64_t> hashes(pieces.size() + 1, 0);
		for (std::size_t at = 0; at < pieces.size(); ++at)
			hashes[at + 1] = hashes[at] * kBase + static_cast<std::uint64_t>(pieces[at]) + 1;
		return hashes;
	};
	const std::vector<std::uint64_t> ha = prefixes(a), hb = prefixes(b);
	std::vector<std::uint64_t> powers(std::max(a.size(), b.size()) + 1, 1);
	for (std::size_t at = 1; at < powers.size(); ++at) powers[at] = powers[at - 1] * kBase;
	const auto window = [&powers](const std::vector<std::uint64_t> &hashes, std::size_t from, std::size_t length) {
		return hashes[from + length] - hashes[from] * powers[length];
	};
	// A run of `length` pieces a[a0, a1) and b[b0, b1) share, earliest in `b`.
	const auto sharedRun = [&](std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1, std::size_t length)
			-> std::optional<std::pair<std::size_t, std::size_t>> {
		std::unordered_map<std::uint64_t, std::size_t> starts;
		for (std::size_t at = a0; at + length <= a1; ++at) starts.emplace(window(ha, at, length), at);
		for (std::size_t at = b0; at + length <= b1; ++at) {
			const auto found = starts.find(window(hb, at, length));
			if (found != starts.end() && std::equal(a.begin() + static_cast<long>(found->second),
					a.begin() + static_cast<long>(found->second + length), b.begin() + static_cast<long>(at)))
				return std::make_pair(found->second, at);
		}
		return std::nullopt;
	};
	struct Range { std::size_t a0, a1, b0, b1; };
	std::vector<Range> open{{0, a.size(), 0, b.size()}};
	for (std::size_t runs = 0; !open.empty() && runs < kMostRuns; ++runs) {
		const Range range = open.back();
		open.pop_back();
		if (range.a0 >= range.a1 || range.b0 >= range.b1) continue;
		// The longest shared run: a run of n shared pieces holds one of n - 1.
		std::size_t low = 0, high = std::min(range.a1 - range.a0, range.b1 - range.b0);
		std::pair<std::size_t, std::size_t> best{0, 0};
		while (low < high) {
			const std::size_t length = low + (high - low + 1) / 2;
			if (const auto found = sharedRun(range.a0, range.a1, range.b0, range.b1, length)) {
				low = length;
				best = *found;
			} else {
				high = length - 1;
			}
		}
		if (low == 0) continue;
		for (std::size_t at = 0; at < low; ++at) kept[best.first + at] = static_cast<long>(best.second + at);
		open.push_back({range.a0, best.first, range.b0, best.second});
		open.push_back({best.first + low, range.a1, best.second + low, range.b1});
	}
	return kept;
}

// The pieces of `a` whose place in `b` another alignment as good as `kept`
// would change: where what is taken out or written in could equally stand
// one piece earlier or later ("Pay $5. Please approve $5. Done" less one
// sentence is "Pay $5. Done" either way), the kept pieces it could slide
// over. A figure on such a piece is not known to be the one kept.
inline std::vector<bool> slidable_pieces(const std::vector<int> &a, const std::vector<int> &b,
		const std::vector<long> &kept) {
	std::vector<bool> slidable(a.size(), false);
	std::vector<std::pair<std::size_t, std::size_t>> matched;
	for (std::size_t at = 0; at < kept.size(); ++at)
		if (kept[at] >= 0) matched.emplace_back(at, static_cast<std::size_t>(kept[at]));
	for (std::size_t gap = 0; gap <= matched.size(); ++gap) {
		const bool hasPrevious = gap > 0, hasNext = gap < matched.size();
		const std::size_t a0 = hasPrevious ? matched[gap - 1].first + 1 : 0;
		const std::size_t a1 = hasNext ? matched[gap].first : a.size();
		const std::size_t b0 = hasPrevious ? matched[gap - 1].second + 1 : 0;
		const std::size_t b1 = hasNext ? matched[gap].second : b.size();
		if (a0 == a1 && b0 == b1) continue;
		// Earlier: the kept piece before the gap equals the gap's last, in each
		// text that has one.
		for (std::size_t step = 0; hasPrevious && step < gap; ++step) {
			const auto [ia, ib] = matched[gap - 1 - step];
			if (ia + step + 1 != a0 || ib + step + 1 != b0) break;
			if ((a1 > a0 && a[ia] != a[a1 - 1 - step]) || (b1 > b0 && b[ib] != b[b1 - 1 - step])) break;
			slidable[ia] = true;
		}
		// Later: the kept piece after the gap equals the gap's first.
		for (std::size_t step = 0; hasNext && gap + step < matched.size(); ++step) {
			const auto [ia, ib] = matched[gap + step];
			if (ia != a1 + step || ib != b1 + step) break;
			if ((a1 > a0 && a[ia] != a[a0 + step]) || (b1 > b0 && b[ib] != b[b0 + step])) break;
			slidable[ia] = true;
		}
	}
	return slidable;
}

} // namespace fact_anchor_detail

// `anchors` over `from`, carried onto `to`, a text Singularity made of it - words
// taken out (a figure it could not work out, a sentence about an approval),
// its own written in (the sentence saying why, a note, a citation's label):
// each anchor goes where the edit keeps that very occurrence - the texts
// matched by their longest shared runs (kept_pieces) - never by its writing's
// rank. An anchor is kept only where its writing and what stands right beside
// it on either side are kept together, and only where no alignment as good
// could keep another occurrence in its place (slidable_pieces), so an
// occurrence taken out never passes its fact to another writing of the same
// figure. One not kept is dropped - that figure is then protected as a fact
// of its own, still masked. An edit Singularity makes itself and can follow is
// better carried by position as it is made.
inline std::vector<FactAnchor> carry_fact_anchors(const std::string &from, const std::vector<FactAnchor> &anchors,
		const std::string &to) {
	if (from.empty() || anchors.empty()) return {};
	if (const std::size_t whole = to.find(from); whole != std::string::npos) {
		std::vector<FactAnchor> moved;
		for (const FactAnchor &anchor : anchors)
			if (anchor.length > 0 && !anchor.key.empty() && anchor.start + anchor.length <= from.size())
				moved.push_back(FactAnchor{anchor.start + whole, anchor.length, anchor.key});
		std::sort(moved.begin(), moved.end(), [](const FactAnchor &left, const FactAnchor &right) {
			return left.start < right.start;
		});
		return moved;
	}
	std::unordered_map<std::string_view, int> interned;
	const fact_anchor_detail::Pieces a = fact_anchor_detail::pieces_of(from, interned);
	const fact_anchor_detail::Pieces b = fact_anchor_detail::pieces_of(to, interned);
	const std::vector<long> kept = fact_anchor_detail::kept_pieces(a.ids, b.ids);
	const std::vector<bool> slidable = fact_anchor_detail::slidable_pieces(a.ids, b.ids, kept);
	const auto pieceAt = [&](std::size_t offset) {
		return static_cast<std::size_t>(std::upper_bound(a.starts.begin(), a.starts.end() - 1, offset) - a.starts.begin()) - 1;
	};
	std::vector<FactAnchor> moved;
	for (const FactAnchor &anchor : anchors) {
		if (anchor.length == 0 || anchor.key.empty() || anchor.start + anchor.length > from.size()) continue;
		const std::size_t first = pieceAt(anchor.start);
		const std::size_t last = pieceAt(anchor.start + anchor.length - 1);
		// The writing's own pieces kept, one after another.
		bool whole = kept[first] >= 0 && !slidable[first];
		for (std::size_t piece = first + 1; whole && piece <= last; ++piece)
			whole = kept[piece] >= 0 && kept[piece] == kept[piece - 1] + 1 && !slidable[piece];
		if (!whole) continue;
		// And what stands beside it: the piece it starts or ends inside, or the
		// one right before and right after, kept beside it - unless the writing
		// begins or ends `from` itself.
		const bool before = anchor.start > a.starts[first] || first == 0
			|| (kept[first - 1] >= 0 && kept[first - 1] == kept[first] - 1);
		const bool after = anchor.start + anchor.length < a.starts[last + 1] || last + 1 >= a.ids.size()
			|| (kept[last + 1] >= 0 && kept[last + 1] == kept[last] + 1);
		if (!before || !after) continue;
		const std::size_t start = b.starts[static_cast<std::size_t>(kept[first])] + (anchor.start - a.starts[first]);
		if (start + anchor.length > to.size() || to.compare(start, anchor.length, from, anchor.start, anchor.length) != 0)
			continue;
		moved.push_back(FactAnchor{start, anchor.length, anchor.key});
	}
	std::sort(moved.begin(), moved.end(), [](const FactAnchor &left, const FactAnchor &right) {
		return left.start < right.start;
	});
	std::vector<FactAnchor> distinct;
	for (FactAnchor &anchor : moved) {
		if (!distinct.empty() && anchor.start < distinct.back().start + distinct.back().length) continue;
		distinct.push_back(std::move(anchor));
	}
	return distinct;
}

// `writings` - each a restored writing and its fact key, in the order they
// stand - found one after another in `text`: where a restored answer that was
// assembled from several fields (a structured result) holds each of them.
inline std::vector<FactAnchor> anchors_in_order(const std::string &text,
		const std::vector<std::pair<std::string, std::string>> &writings) {
	std::vector<FactAnchor> anchors;
	std::size_t cursor = 0;
	for (const auto &[writing, key] : writings) {
		if (writing.empty() || key.empty()) continue;
		const std::size_t found = text.find(writing, cursor);
		if (found == std::string::npos) continue;
		anchors.push_back(FactAnchor{found, writing.size(), key});
		cursor = found + writing.size();
	}
	return anchors;
}

// `text` with each anchor written as its mark: ⟪writing|fact:key⟫, or, where the
// writing is a statement's whole operand ("⟦$2.8 million⟧"), the key inside
// the operand's own marks. Anchors are in `text`'s coordinates; one that
// overlaps another, or a mark already there, is left as plain text.
inline std::string mark_fact_anchors(const std::string &text, std::vector<FactAnchor> anchors) {
	namespace marks = fact_anchor_marks;
	std::sort(anchors.begin(), anchors.end(), [](const FactAnchor &left, const FactAnchor &right) {
		return left.start < right.start;
	});
	std::string marked;
	marked.reserve(text.size() + anchors.size() * 32);
	std::size_t cursor = 0;
	for (const FactAnchor &anchor : anchors) {
		if (anchor.start < cursor || anchor.length == 0 || anchor.start + anchor.length > text.size()
				|| !marks::is_key(anchor.key))
			continue;
		const std::string_view writing(text.data() + anchor.start, anchor.length);
		if (writing.find(marks::kKey) != std::string_view::npos || writing.find(marks::kOpen) != std::string_view::npos
				|| writing.find(marks::kOperandOpen) != std::string_view::npos)
			continue;
		const std::size_t end = anchor.start + anchor.length;
		const bool operand = anchor.start >= marks::kOperandOpen.size()
			&& text.compare(anchor.start - marks::kOperandOpen.size(), marks::kOperandOpen.size(), marks::kOperandOpen) == 0
			&& text.compare(end, marks::kOperandClose.size(), marks::kOperandClose) == 0;
		marked.append(text, cursor, anchor.start - cursor);
		if (!operand) marked += marks::kOpen;
		marked.append(writing);
		marked += marks::kKey;
		marked += anchor.key;
		if (!operand) marked += marks::kClose;
		cursor = end;
	}
	marked.append(text, cursor, std::string::npos);
	return marked;
}

// A text with its local fact-anchor marks (⟪writing|fact:key⟫, ⟦writing|fact:key⟧)
// written back as the writing alone (and ⟦writing⟧): for a send nothing
// protects - protection off, or a model on this computer.
inline std::string strip_fact_anchor_marks(const std::string &text) {
	namespace marks = fact_anchor_marks;
	if (text.find(marks::kKey) == std::string::npos) return text;
	std::string result;
	result.reserve(text.size());
	std::size_t cursor = 0;
	while (cursor < text.size()) {
		const std::size_t key = text.find(marks::kKey, cursor);
		if (key == std::string::npos) break;
		// The mark's opening before the key and its closing after it.
		const std::size_t prose = text.rfind(marks::kOpen, key);
		const std::size_t operand = text.rfind(marks::kOperandOpen, key);
		const bool inOperand = operand != std::string::npos && operand >= cursor
			&& (prose == std::string::npos || prose < cursor || operand > prose);
		const std::size_t open = inOperand ? operand : prose;
		const std::string_view close = inOperand ? marks::kOperandClose : marks::kClose;
		const std::size_t end = text.find(close, key);
		if (open == std::string::npos || open < cursor || end == std::string::npos) {
			result.append(text, cursor, key + marks::kKey.size() - cursor);
			cursor = key + marks::kKey.size();
			continue;
		}
		result.append(text, cursor, open - cursor);
		const std::size_t writingStart = open + (inOperand ? marks::kOperandOpen.size() : marks::kOpen.size());
		if (inOperand) result += marks::kOperandOpen;
		result.append(text, writingStart, key - writingStart);
		if (inOperand) result += marks::kOperandClose;
		cursor = end + close.size();
	}
	result.append(text, cursor, std::string::npos);
	return result;
}

} // namespace singularity::protected_api::papinian
