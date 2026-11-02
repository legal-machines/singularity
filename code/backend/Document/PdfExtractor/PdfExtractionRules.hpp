#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// What every platform's PDF adapter decides the same way, so a PDF reads the
// same on macOS (PDFKit) and on Windows and Linux (PDFium): when a page is a
// picture Singularity did not read, how the pages are put together into one
// reading, and what the document is. The adapters differ only in how they
// read a page's text, find its images and draw it.
namespace singularity::documents::pdf {

// The long edge, in pixels, of the grey thumbnail a page is judged by.
inline constexpr std::size_t kRasterLongEdge = 256;
// The share of a page's pixels that is ink before the page shows something.
inline constexpr double kMaterialInkRatio = 0.0015;
// A raster image covering at least this share of a page is the page itself,
// or a large part of it - never a logo or a pasted initial.
inline constexpr double kPictureShareOfPage = 0.25;
// How many times the ink of a picture must outweigh the ink of the text read
// over it before the page counts as a picture that was not read.
inline constexpr double kUnreadOutweighsRead = 4.0;

inline bool is_ink(unsigned char pixel) { return pixel < 238; }

// The pixels a glance at the page weighs: all but a thin frame at its edge,
// where a crop or a scanner leaves its own marks.
inline std::size_t raster_margin(std::size_t width, std::size_t height) {
	return std::min<std::size_t>(2, std::min(width, height) / 4);
}

// Marks, in a mask of the raster (row 0 at the top), the pixels a rectangle
// covers, `pad` pixels more around it. The rectangle is in raster pixels with
// the origin at the bottom-left, as a page's own coordinates run.
inline void mark_rectangle(std::size_t width, std::size_t height, double minX, double minY, double maxX,
		double maxY, double pad, std::vector<unsigned char> &mask) {
	minX -= pad;
	minY -= pad;
	maxX += pad;
	maxY += pad;
	if (!std::isfinite(minX) || !std::isfinite(minY) || !std::isfinite(maxX) || !std::isfinite(maxY)) return;
	const double limitX = static_cast<double>(width);
	const double limitY = static_cast<double>(height);
	const auto first = [](double value, double limit) {
		return static_cast<std::size_t>(std::clamp(std::floor(value), 0.0, limit));
	};
	const auto last = [](double value, double limit) {
		return static_cast<std::size_t>(std::clamp(std::ceil(value), 0.0, limit));
	};
	const std::size_t left = first(minX, limitX);
	const std::size_t right = last(maxX, limitX);
	const std::size_t bottom = first(minY, limitY);
	const std::size_t top = last(maxY, limitY);
	for (std::size_t y = bottom; y < top; ++y) {
		const std::size_t row = height - 1 - y;
		for (std::size_t x = left; x < right; ++x) mask[row * width + x] = 1;
	}
}

// A page with no text of its own that still shows something: a scan, a
// drawing, a stamp. `pixels` is the grey raster, row 0 at the top.
inline bool has_material_ink(const std::vector<unsigned char> &pixels, std::size_t width, std::size_t height) {
	const std::size_t margin = raster_margin(width, height);
	std::size_t inspected = 0;
	std::size_t ink = 0;
	for (std::size_t y = margin; y < height - margin; ++y) {
		for (std::size_t x = margin; x < width - margin; ++x) {
			++inspected;
			if (is_ink(pixels[y * width + x])) ++ink;
		}
	}
	return inspected > 0 && static_cast<double>(ink) / static_cast<double>(inspected) >= kMaterialInkRatio;
}

// A page with text of its own is a picture Singularity did not read when raster
// images fill a good part of it and what they show beyond that text outweighs
// the text many times over: a scanned signature page under its Bates stamp, a
// scanned page under a production legend. `picture` marks where the page's
// raster images lie, `readText` where the text that was read lies; the ink of
// that text is never counted as unread, so a cover title, a divider page, a
// logo beside a title, a rule under it or the blank paper it is set on are
// read, and so is a scan whose text layer was read (its signatures and stamps
// are a little ink beside the text over them).
inline bool picture_outweighs_its_text(const std::vector<unsigned char> &pixels, std::size_t width,
		std::size_t height, const std::vector<unsigned char> &picture, const std::vector<unsigned char> &readText) {
	const std::size_t margin = raster_margin(width, height);
	std::size_t inspected = 0;
	std::size_t covered = 0;
	std::size_t unreadInk = 0;
	std::size_t readInk = 0;
	for (std::size_t y = margin; y < height - margin; ++y) {
		for (std::size_t x = margin; x < width - margin; ++x) {
			const std::size_t at = y * width + x;
			++inspected;
			if (picture[at] != 0) ++covered;
			if (!is_ink(pixels[at])) continue;
			if (readText[at] != 0) ++readInk;
			else if (picture[at] != 0) ++unreadInk;
		}
	}
	if (inspected == 0) return false;
	const double share = static_cast<double>(covered) / static_cast<double>(inspected);
	const double ink = static_cast<double>(unreadInk) / static_cast<double>(inspected);
	return share >= kPictureShareOfPage && ink >= kMaterialInkRatio
		&& static_cast<double>(unreadInk) >= kUnreadOutweighsRead * static_cast<double>(readInk);
}

// One page as its adapter read it, before the document's reading is put
// together.
struct PageReading {
	std::string text;
	std::size_t meaningful = 0;
	// The page shows content - a scan, a signed page, a stamp - that its text,
	// if it has any, does not stand for: what is on it was not read.
	bool isImage = false;
	// The page is a picture whose words were read by text recognition on this
	// computer (Windows' own OCR): read, and said to be read so, since a
	// recognised word or figure may differ from the scan.
	bool isRecognized = false;
};

// A page read for AI: text of its own, and no picture beyond it.
inline bool is_read_page(const PageReading &page) { return !page.isImage && page.meaningful > 0; }

// Consecutive unread pages, as first and last 1-based page numbers.
inline std::vector<std::pair<std::size_t, std::size_t>> unread_page_runs(const std::vector<PageReading> &pages) {
	std::vector<std::pair<std::size_t, std::size_t>> runs;
	for (std::size_t index = 0; index < pages.size(); ++index) {
		if (!pages[index].isImage) continue;
		const std::size_t number = index + 1;
		if (!runs.empty() && runs.back().second + 1 == number) {
			runs.back().second = number;
			continue;
		}
		runs.emplace_back(number, number);
	}
	return runs;
}

inline std::string page_span(const std::pair<std::size_t, std::size_t> &run) {
	if (run.first == run.second) return std::to_string(run.first);
	return std::to_string(run.first) + "–" + std::to_string(run.second);
}

// The plain sentence a lawyer reads about the pages Singularity could not
// read: "Page 14 is an image and was not read.", "Pages 3, 14-15 and 20 are
// images and were not read." Every unread page is named, however many there
// are: the lawyer must be able to find each one without asking the model.
inline std::string unread_pages_sentence(const std::vector<std::pair<std::size_t, std::size_t>> &runs) {
	std::size_t unread = 0;
	for (const auto &run : runs) unread += run.second - run.first + 1;
	if (unread == 1) return "Page " + page_span(runs.front()) + " is an image and was not read.";
	std::string places;
	for (std::size_t index = 0; index < runs.size(); ++index) {
		if (index > 0) places += index + 1 == runs.size() ? " and " : ", ";
		places += page_span(runs[index]);
	}
	return "Pages " + places + " are images and were not read.";
}

// Consecutive pages read by text recognition, as first and last 1-based numbers.
inline std::vector<std::pair<std::size_t, std::size_t>> recognized_page_runs(const std::vector<PageReading> &pages) {
	std::vector<std::pair<std::size_t, std::size_t>> runs;
	for (std::size_t index = 0; index < pages.size(); ++index) {
		if (!pages[index].isRecognized) continue;
		const std::size_t number = index + 1;
		if (!runs.empty() && runs.back().second + 1 == number) {
			runs.back().second = number;
			continue;
		}
		runs.emplace_back(number, number);
	}
	return runs;
}

// "Page 1 is a scan, read by text recognition on this computer." - the
// lawyer and the model both know a recognised word or figure may differ from
// the page.
inline std::string recognized_pages_sentence(const std::vector<std::pair<std::size_t, std::size_t>> &runs) {
	std::size_t recognized = 0;
	for (const auto &run : runs) recognized += run.second - run.first + 1;
	if (recognized == 1) return "Page " + page_span(runs.front()) + " is a scan, read by text recognition on this computer.";
	std::string places;
	for (std::size_t index = 0; index < runs.size(); ++index) {
		if (index > 0) places += index + 1 == runs.size() ? " and " : ", ";
		places += page_span(runs[index]);
	}
	return "Pages " + places + " are scans, read by text recognition on this computer.";
}

inline void inspect_text_structure(DocumentExtractionResult &result) {
	if (result.text.empty()) return;
	result.structure.line_count = 1;
	bool in_paragraph = false;
	std::size_t line_start = 0;
	for (std::size_t index = 0; index <= result.text.size(); ++index) {
		if (index != result.text.size() && result.text[index] != '\n') continue;
		const bool blank = std::all_of(result.text.begin() + static_cast<std::ptrdiff_t>(line_start),
			result.text.begin() + static_cast<std::ptrdiff_t>(index), [](unsigned char character) {
				return character == ' ' || character == '\t' || character == '\f' || character == '\v';
			});
		if (!blank && !in_paragraph) ++result.structure.paragraph_count;
		in_paragraph = !blank;
		if (index != result.text.size()) {
			++result.structure.line_count;
			line_start = index + 1;
		}
	}
}

// Puts the document's reading together page by page and decides what it is.
// A PDF with text is ready with the text it has: an executed agreement whose
// signature pages are scans is still read, and its unread pages are said in
// plain words - once to the lawyer (the `pages_not_read` diagnostic, which the
// Documents page shows), and inside the reading itself, where the model meets
// them at their own place: "[Pages 14-15 are images and were not read.]" opens
// the first page of each unread run. A PDF with no page read is a scan Singularity
// cannot read at all.
inline void assemble_reading(const std::vector<PageReading> &pages, DocumentExtractionResult &result) {
	bool hasReadPage = false;
	for (const PageReading &page : pages)
		if (is_read_page(page)) hasReadPage = true;
	const auto runs = unread_page_runs(pages);
	const bool isPartlyRead = hasReadPage && !runs.empty();

	std::size_t runIndex = 0;
	for (std::size_t index = 0; index < pages.size(); ++index) {
		const std::size_t number = index + 1;
		std::string pageText;
		if (isPartlyRead && runIndex < runs.size() && runs[runIndex].first == number) {
			pageText = "[" + unread_pages_sentence({runs[runIndex]}) + "]";
			if (!pages[index].text.empty()) pageText += "\n\n" + pages[index].text;
			++runIndex;
		} else if (pages[index].isRecognized) {
			// Where the model meets a recognised page, it is told so (D-114).
			pageText = "[" + recognized_pages_sentence({{number, number}}) + "]\n\n" + pages[index].text;
		} else {
			pageText = pages[index].text;
		}
		if (index > 0 && !result.text.empty()) result.text.append("\n\n");
		const std::size_t offset = result.text.size();
		result.text.append(pageText);
		result.structure.page_boundaries.push_back({number, offset, pageText.size()});
	}
	result.structure.byte_count = result.text.size();
	inspect_text_structure(result);

	if (hasReadPage) {
		result.status = ExtractionStatus::ready;
		if (isPartlyRead) result.diagnostics.push_back({"pages_not_read", unread_pages_sentence(runs)});
		const auto recognized = recognized_page_runs(pages);
		if (!recognized.empty()) result.diagnostics.push_back({"pages_recognized", recognized_pages_sentence(recognized)});
		return;
	}
	if (!runs.empty()) {
		result.status = ExtractionStatus::ocr_required;
		result.diagnostics.push_back({"ocr_required",
			"The pages of this PDF are images with no text, so Singularity could not read them."});
		return;
	}
	result.status = ExtractionStatus::empty;
	result.diagnostics.push_back({"empty_document", "The PDF contains no extractable text."});
}

} // namespace singularity::documents::pdf
