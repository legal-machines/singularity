#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace singularity::routing::source_material {

struct Span {
	std::size_t from = 0;
	std::size_t to = 0;
	bool machine_owned = false;
};

// Length of a source passage's opening marker line at `at`, exactly as
// SourceMarkerRegistry::format_chunk writes it ("[SOURCE:DOC_<n>:CHUNK_<n>]\n"),
// or 0 when the text there is anything else. Deliberately strict: only this
// exact shape, whose tokens carry no Project data, may pass through unscored.
inline std::size_t opening_marker_length(const std::string &value, std::size_t at, std::size_t end) {
	const auto digits = [&](std::size_t position) {
		std::size_t cursor = position;
		while (cursor < end && cursor - position < 7 && value[cursor] >= '0' && value[cursor] <= '9') ++cursor;
		return cursor - position;
	};
	static constexpr std::string_view kPrefix = "[SOURCE:DOC_";
	static constexpr std::string_view kMiddle = ":CHUNK_";
	if (at > end || end - at < kPrefix.size() || value.compare(at, kPrefix.size(), kPrefix) != 0) return 0;
	std::size_t cursor = at + kPrefix.size();
	std::size_t count = digits(cursor);
	if (count == 0 || count > 6) return 0;
	cursor += count;
	if (end - cursor < kMiddle.size() || value.compare(cursor, kMiddle.size(), kMiddle) != 0) return 0;
	cursor += kMiddle.size();
	count = digits(cursor);
	if (count == 0 || count > 6) return 0;
	cursor += count;
	if (end - cursor < 2 || value[cursor] != ']' || value[cursor + 1] != '\n') return 0;
	return cursor + 2 - at;
}

// A passage's text exactly as a send presents it to detection: the chunk's own
// text with LM boundary tokens rendered as literals, the way PromptAssembler's
// append_section() escapes what it is given. Preparing a document's detections
// ahead of a send hashes this, and a send scores the span this produces, so the
// two are the same bytes and the prepared detections are found. One definition,
// because the moment the two drift apart every prepared detection silently
// misses and the model runs again on every turn.
inline std::string as_prompt_text(std::string text) {
	std::size_t marker = 0;
	while ((marker = text.find("<<<LM_", marker)) != std::string::npos) {
		text.replace(marker, 6, "<<<LM_LITERAL_");
		marker += 14;
	}
	return text;
}

// A document's passages are read under the document they come from, named
// once above them: "[DOCUMENT:DOC_<n>]\n<folder/file name>\n[/DOCUMENT]\n\n".
// Without it a clause of the buyer's draft and the same clause of the sellers'
// draft read alike, and a live run (11 Oct 2026) reported the sellers' escrow
// as the buyer's. The marker lines are Singularity's own and carry no Project
// data; the title between them is Project data, scored and protected like a
// passage, and the same bytes on every request, so its detections are
// prepared with the document's passages (PrivacyActions).
inline constexpr std::string_view kDocumentHeaderClosing = "\n[/DOCUMENT]\n\n";

// The title as a header writes it: one line, and never a bracket that could
// read as a marker of Singularity's own.
inline std::string document_header_title(std::string title) {
	for (char &character : title) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte < 0x20 || byte == 0x7f) character = ' ';
		else if (character == '[') character = '(';
		else if (character == ']') character = ')';
	}
	return title;
}

// The header above a document's passages, for its handle ("DOC_3").
inline std::string document_header(const std::string &document_token, const std::string &title) {
	return "[DOCUMENT:" + document_token + "]\n" + document_header_title(title) + std::string(kDocumentHeaderClosing);
}

// Length of a header's opening line at `at` ("[DOCUMENT:DOC_<n>]\n"), or 0.
inline std::size_t document_header_opening_length(const std::string &value, std::size_t at, std::size_t end) {
	static constexpr std::string_view kPrefix = "[DOCUMENT:DOC_";
	if (at > end || end - at < kPrefix.size() || value.compare(at, kPrefix.size(), kPrefix) != 0) return 0;
	std::size_t cursor = at + kPrefix.size();
	const std::size_t digitsFrom = cursor;
	while (cursor < end && cursor - digitsFrom < 7 && value[cursor] >= '0' && value[cursor] <= '9') ++cursor;
	if (cursor == digitsFrom || cursor - digitsFrom > 6) return 0;
	if (end - cursor < 2 || value[cursor] != ']' || value[cursor + 1] != '\n') return 0;
	return cursor + 2 - at;
}

// SOURCE_MATERIAL data in [from, to) is a run of passages, each
// "<marker>\n<text>\n[/SOURCE]\n\n", a document's run headed by its title
// (document_header()). Each title and each passage's text becomes its own
// scored span - the same bytes whether a passage is sent alone or with others,
// so its detections can be prepared ahead of time and reused. Only the exact
// marker lines are machine-owned; anything that does not parse as a passage is
// one scored span, exactly as the whole section was before. Empty spans are
// omitted.
// A section that grows by appending - the conversation carried forward, the
// earlier turns kept in full - is scored record by record rather than whole.
// Scored whole, one new line makes the entire section new text: a long session
// re-scores everything it has already said on every turn, and the cost of a
// turn climbs with the length of the conversation while the data room costs
// nothing. Cut at the boundary the section itself is built on, each record is
// the same bytes it was last turn and is found already scored.
//
// `record_separator` is what the builder puts between records: "\n" where each
// record is one line, "\n\n" where each record is a block of text. Only the
// separators pass through unscored - they carry nothing - and a section with no
// separator in it is one scored span, exactly as it was before.
inline std::vector<Span> record_spans(const std::string &value, std::size_t from, std::size_t to,
		std::string_view record_separator) {
	std::vector<Span> spans;
	const auto add = [&](std::size_t start, std::size_t end, bool machineOwned) {
		if (end > start) spans.push_back({start, end, machineOwned});
	};
	std::size_t cursor = from;
	while (cursor < to) {
		const std::size_t separator = value.find(record_separator, cursor);
		if (separator == std::string::npos || separator + record_separator.size() > to) break;
		add(cursor, separator, false);
		add(separator, separator + record_separator.size(), true);
		cursor = separator + record_separator.size();
	}
	add(cursor, to, false);
	return spans;
}

inline std::vector<Span> passage_spans(const std::string &value, std::size_t from, std::size_t to) {
	static constexpr std::string_view kClosing = "\n[/SOURCE]\n\n";
	std::vector<Span> spans;
	const auto add = [&](std::size_t start, std::size_t end, bool machineOwned) {
		if (end > start) spans.push_back({start, end, machineOwned});
	};
	std::size_t cursor = from;
	while (cursor < to) {
		if (const std::size_t header = document_header_opening_length(value, cursor, to); header != 0) {
			const std::size_t titleStart = cursor + header;
			const std::size_t closing = value.find(kDocumentHeaderClosing, titleStart);
			if (closing == std::string::npos || closing + kDocumentHeaderClosing.size() > to) break;
			add(cursor, titleStart, true);
			add(titleStart, closing, false);
			add(closing, closing + kDocumentHeaderClosing.size(), true);
			cursor = closing + kDocumentHeaderClosing.size();
			continue;
		}
		const std::size_t opening = opening_marker_length(value, cursor, to);
		if (opening == 0) break;
		const std::size_t textStart = cursor + opening;
		const std::size_t closing = value.find(kClosing, textStart);
		if (closing == std::string::npos || closing + kClosing.size() > to) break;
		add(cursor, textStart, true);
		add(textStart, closing, false);
		add(closing, closing + kClosing.size(), true);
		cursor = closing + kClosing.size();
	}
	add(cursor, to, false);
	return spans;
}

} // namespace singularity::routing::source_material
