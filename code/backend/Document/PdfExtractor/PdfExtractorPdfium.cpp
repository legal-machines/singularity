#include "Document/PdfExtractor/PdfExtractor.hpp"

#include "Document/PdfExtractor/PdfExtractionRules.hpp"

#include "Platform/PhraseMatch/PhraseMatch.hpp"

#include <cpp/fpdf_scopers.h>
#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <sstream>

#if defined(_WIN32)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#pragma comment(lib, "windowsapp")
#endif

// PDF text on Windows and Linux through PDFium (Chrome's PDF engine), with
// the contract PDFKit keeps on macOS (PdfExtractorMacOS.mm): the same page
// gate, decided by the same rules (PdfExtractionRules.hpp). Only how a page's
// text, images and pixels are read differs.
//
// Work on a PDF - its annotated and redacted copies - is PDFium's on every
// system, macOS included: one engine writes the same copy everywhere. On
// macOS only that part is built here; PDFKit reads.
namespace singularity::documents {
namespace {

#if !defined(__APPLE__)
using pdf::PageReading;

// The image walk is bounded, whatever the file: forms nest at most this deep,
// at most this many are entered per page, and at most this many images are
// placed. What lies past the bound is not looked at.
constexpr std::size_t kDeepestForm = 8;
constexpr std::size_t kMostFormsRead = 64;
constexpr std::size_t kMostImagesPlaced = 1024;

#endif // !__APPLE__ (the reading's bounds)

// PDFium may be entered by one thread at a time.
std::mutex &pdfium_mutex() {
	static std::mutex mutex;
	return mutex;
}

void ensure_pdfium() {
	static const bool initialized = [] {
		FPDF_LIBRARY_CONFIG config{};
		config.version = 2;
		FPDF_InitLibraryWithConfig(&config);
		return true;
	}();
	(void)initialized;
}

#if !defined(__APPLE__)
std::string current_extractor_version() {
#if defined(_WIN32)
	// A scan is read by Windows' text recognition: every PDF read before is read
	// again.
	return std::string("pdfium-text-raster-page-gate/5-ocr;pdfium-") + NATIVE_APP_PDFIUM_VERSION;
#else
	return std::string("pdfium-text-raster-page-gate/4;pdfium-") + NATIVE_APP_PDFIUM_VERSION;
#endif
}

// ---- Text recognition ----------------------------------------------------------------
//
// A page that is a picture - a scan, a photographed page, a signed copy - is read by the
// system's own text recognition on this computer: on Windows, Windows.Media.Ocr. Nothing
// leaves the computer and no model is installed for it. The page is drawn at up to 300
// dots an inch, as large as the engine takes, and read in English first, then Russian,
// then the person's own languages, until a reading holds words. What recognition reads
// is said to be recognised wherever it is read (PdfExtractionRules.hpp). Recognition
// reads at most kMostRecognizedPages pages of one document; the rest stay unread and are
// said to be.
constexpr std::size_t kMostRecognizedPages = 40;
// A reading with fewer letters and digits than this is no reading: lines of a
// form, a stamp, a signature.
constexpr std::size_t kLeastRecognizedCharacters = 20;

std::size_t letters_and_digits(const std::string &text) {
	std::size_t count = 0;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		if (byte < 0x80) {
			if ((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')) ++count;
		} else if (byte >= 0xC3 && byte <= 0xDF) {
			// The first byte of a two-byte letter: accented Latin, Greek,
			// Cyrillic. Dashes and other marks, three bytes long, are not words.
			++count;
		}
	}
	return count;
}

#if defined(_WIN32)
// A picture's words, in English first, then Russian, then the person's own
// languages, until a reading holds words.
std::optional<std::string> recognize_bitmap(const winrt::Windows::Graphics::Imaging::SoftwareBitmap &image) {
	using winrt::Windows::Media::Ocr::OcrEngine;
	std::vector<OcrEngine> engines;
	std::vector<winrt::hstring> tried;
	const auto add = [&](const OcrEngine &engine) {
		if (!engine) return;
		// Each language once: the person's own is often one already tried.
		const winrt::hstring tag = engine.RecognizerLanguage().LanguageTag();
		if (std::find(tried.begin(), tried.end(), tag) != tried.end()) return;
		tried.push_back(tag);
		engines.push_back(engine);
	};
	for (const wchar_t *tag : {L"en-US", L"ru-RU"}) {
		const winrt::Windows::Globalization::Language language(tag);
		if (OcrEngine::IsLanguageSupported(language)) add(OcrEngine::TryCreateFromLanguage(language));
	}
	add(OcrEngine::TryCreateFromUserProfileLanguages());
	std::string best;
	for (const OcrEngine &engine : engines) {
		const auto recognized = engine.RecognizeAsync(image).get();
		std::string text;
		for (const auto &line : recognized.Lines()) {
			if (!text.empty()) text += "\n";
			text += winrt::to_string(line.Text());
		}
		if (letters_and_digits(text) > letters_and_digits(best)) best = std::move(text);
		if (letters_and_digits(best) >= kLeastRecognizedCharacters) break;
	}
	if (letters_and_digits(best) < kLeastRecognizedCharacters) return std::nullopt;
	return best;
}

std::optional<std::string> recognize_page(FPDF_PAGE page) {
	try {
		using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
		using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
		using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
		using winrt::Windows::Media::Ocr::OcrEngine;
		// The extraction's own thread: in the multithreaded apartment, where a
		// recognition may be waited for. A thread already in one stays in it.
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		} catch (const winrt::hresult_error &) {
		}
		const double pageWidth = FPDF_GetPageWidthF(page);
		const double pageHeight = FPDF_GetPageHeightF(page);
		if (!std::isfinite(pageWidth) || !std::isfinite(pageHeight) || pageWidth <= 0.0 || pageHeight <= 0.0)
			return std::nullopt;
		const double largest = static_cast<double>(OcrEngine::MaxImageDimension());
		const double scale = std::min(300.0 / 72.0, largest / std::max(pageWidth, pageHeight));
		const int width = std::max(1, static_cast<int>(std::floor(pageWidth * scale)));
		const int height = std::max(1, static_cast<int>(std::floor(pageHeight * scale)));
		ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, 0));
		if (!bitmap) return std::nullopt;
		FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
		FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, width, height, 0, FPDF_ANNOT);
		const auto *bgrx = static_cast<const std::uint8_t *>(FPDFBitmap_GetBuffer(bitmap.get()));
		const int stride = FPDFBitmap_GetStride(bitmap.get());
		if (bgrx == nullptr || stride < width * 4) return std::nullopt;
		const std::uint32_t bytes = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height) * 4U;
		winrt::Windows::Storage::Streams::Buffer buffer(bytes);
		std::uint8_t *pixels = buffer.data();
		for (int y = 0; y < height; ++y) {
			const std::uint8_t *row = bgrx + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
			std::uint8_t *out = pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U;
			for (int x = 0; x < width; ++x) {
				out[x * 4] = row[x * 4];
				out[x * 4 + 1] = row[x * 4 + 1];
				out[x * 4 + 2] = row[x * 4 + 2];
				out[x * 4 + 3] = 255;
			}
		}
		buffer.Length(bytes);
		const SoftwareBitmap image = SoftwareBitmap::CreateCopyFromBuffer(buffer, BitmapPixelFormat::Bgra8, width, height,
			BitmapAlphaMode::Premultiplied);
		return recognize_bitmap(image);
	} catch (...) {
		return std::nullopt;
	}
}

// A picture file - a photographed page, a scan saved as an image - read the
// same way: decoded by Windows, turned as its camera turned it, made no larger
// than the engine reads, then recognised.
std::optional<std::string> recognize_image_file(const std::filesystem::path &path) {
	try {
		using namespace winrt::Windows::Graphics::Imaging;
		using winrt::Windows::Media::Ocr::OcrEngine;
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		} catch (const winrt::hresult_error &) {
		}
		std::vector<char> bytes;
		{
			std::ifstream input(path, std::ios::binary);
			if (!input) return std::nullopt;
			bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}
		if (bytes.empty()) return std::nullopt;
		winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
		{
			winrt::Windows::Storage::Streams::DataWriter writer(stream);
			writer.WriteBytes(winrt::array_view<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(bytes.data()),
				reinterpret_cast<const std::uint8_t *>(bytes.data()) + bytes.size()));
			writer.StoreAsync().get();
			writer.DetachStream();
		}
		stream.Seek(0);
		const BitmapDecoder decoder = BitmapDecoder::CreateAsync(stream).get();
		const double largest = static_cast<double>(OcrEngine::MaxImageDimension());
		const double longest = static_cast<double>(std::max(decoder.OrientedPixelWidth(), decoder.OrientedPixelHeight()));
		BitmapTransform transform;
		if (longest > largest && longest > 0.0) {
			const double scale = largest / longest;
			transform.ScaledWidth(std::max<std::uint32_t>(1, static_cast<std::uint32_t>(decoder.PixelWidth() * scale)));
			transform.ScaledHeight(std::max<std::uint32_t>(1, static_cast<std::uint32_t>(decoder.PixelHeight() * scale)));
		}
		SoftwareBitmap image = decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Straight,
			transform, ExifOrientationMode::RespectExifOrientation, ColorManagementMode::DoNotColorManage).get();
		// Laid on its ground first: what shows through reads as black, and a
		// logo's black lettering on nothing vanished.
		const std::uint32_t width = static_cast<std::uint32_t>(image.PixelWidth());
		const std::uint32_t height = static_cast<std::uint32_t>(image.PixelHeight());
		const std::uint32_t length = width * height * 4U;
		if (length > 0) {
			winrt::Windows::Storage::Streams::Buffer pixels(length);
			image.CopyToBuffer(pixels);
			std::uint8_t *bgra = pixels.data();
			bool hasAlpha = false;
			for (std::uint32_t index = 3; index < length && hasAlpha == false; index += 4) hasAlpha = bgra[index] != 255;
			if (hasAlpha) {
				const unsigned int ground = picture::is_drawn_light(bgra, length / 4U, 2, 1, 0, 3, false) ? 0U : 255U;
				for (std::uint32_t index = 0; index < length; index += 4) {
					const unsigned int alpha = bgra[index + 3];
					for (std::uint32_t channel = 0; channel < 3; ++channel)
						bgra[index + channel] = static_cast<std::uint8_t>((bgra[index + channel] * alpha + ground * (255U - alpha)) / 255U);
					bgra[index + 3] = 255;
				}
				image = SoftwareBitmap::CreateCopyFromBuffer(pixels, BitmapPixelFormat::Bgra8, static_cast<int>(width),
					static_cast<int>(height), BitmapAlphaMode::Premultiplied);
			}
		}
		return recognize_bitmap(image);
	} catch (...) {
		return std::nullopt;
	}
}

// A picture redrawn by Windows's own imaging: decoded, turned as its camera
// turned it, made no larger than any provider reads, and - where it shows
// through - laid on its ground and written as PNG where it fits, as JPEG
// otherwise. `kept` is the picture as it is, where it could go so: one none of
// whose pixels shows through goes so.
std::optional<ModelPicture> redrawn_picture(const std::filesystem::path &path, const std::optional<ModelPicture> &kept) {
	try {
		using namespace winrt::Windows::Graphics::Imaging;
		using winrt::Windows::Storage::Streams::Buffer;
		using winrt::Windows::Storage::Streams::DataWriter;
		using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;
		using winrt::Windows::Storage::Streams::InputStreamOptions;
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		} catch (const winrt::hresult_error &) {
		}
		std::vector<char> bytes;
		{
			std::ifstream input(path, std::ios::binary);
			if (!input) return std::nullopt;
			bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}
		if (bytes.empty()) return std::nullopt;
		InMemoryRandomAccessStream stream;
		{
			DataWriter writer(stream);
			writer.WriteBytes(winrt::array_view<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(bytes.data()),
				reinterpret_cast<const std::uint8_t *>(bytes.data()) + bytes.size()));
			writer.StoreAsync().get();
			writer.DetachStream();
		}
		stream.Seek(0);
		const BitmapDecoder decoder = BitmapDecoder::CreateAsync(stream).get();
		const double longest = static_cast<double>(std::max(decoder.OrientedPixelWidth(), decoder.OrientedPixelHeight()));
		BitmapTransform transform;
		if (longest > static_cast<double>(picture::kRedrawnLongEdge)) {
			const double scale = static_cast<double>(picture::kRedrawnLongEdge) / longest;
			transform.ScaledWidth(std::max<std::uint32_t>(1, static_cast<std::uint32_t>(decoder.PixelWidth() * scale)));
			transform.ScaledHeight(std::max<std::uint32_t>(1, static_cast<std::uint32_t>(decoder.PixelHeight() * scale)));
			transform.InterpolationMode(BitmapInterpolationMode::Fant);
		}
		const SoftwareBitmap drawn = decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Straight,
			transform, ExifOrientationMode::RespectExifOrientation, ColorManagementMode::ColorManageToSRgb).get();
		const std::uint32_t width = static_cast<std::uint32_t>(drawn.PixelWidth());
		const std::uint32_t height = static_cast<std::uint32_t>(drawn.PixelHeight());
		const std::uint32_t length = width * height * 4U;
		if (width == 0 || height == 0) return std::nullopt;
		Buffer pixels(length);
		drawn.CopyToBuffer(pixels);
		std::vector<std::uint8_t> bgra(pixels.data(), pixels.data() + length);
		bool hasAlpha = false;
		for (std::uint32_t index = 3; index < length && hasAlpha == false; index += 4) hasAlpha = bgra[index] != 255;
		if (kept.has_value() && hasAlpha == false) return kept;
		const auto encoded = [&](const winrt::guid &kind, const std::vector<std::uint8_t> &data,
				BitmapAlphaMode alpha) -> std::optional<std::string> {
			InMemoryRandomAccessStream written;
			const bool isJpeg = kind == BitmapEncoder::JpegEncoderId();
			BitmapPropertySet options;
			if (isJpeg)
				options.Insert(L"ImageQuality", BitmapTypedValue(winrt::box_value(static_cast<float>(picture::kRedrawnJpegQuality)),
					winrt::Windows::Foundation::PropertyType::Single));
			BitmapEncoder encoder = BitmapEncoder::CreateAsync(kind, written, options).get();
			encoder.SetPixelData(BitmapPixelFormat::Bgra8, alpha, width, height, 96.0, 96.0,
				winrt::array_view<const std::uint8_t>(data.data(), data.data() + data.size()));
			encoder.FlushAsync().get();
			const std::uint32_t size = static_cast<std::uint32_t>(written.Size());
			if (size == 0) return std::nullopt;
			written.Seek(0);
			Buffer read(size);
			const auto taken = written.ReadAsync(read, size, InputStreamOptions::None).get();
			return std::string(reinterpret_cast<const char *>(taken.data()), taken.Length());
		};
		if (hasAlpha) {
			// Laid on its ground: white, as a page shows it, or black under a light
			// drawing; then sharp as PNG where it fits.
			const unsigned int ground = picture::is_drawn_light(bgra.data(), length / 4U, 2, 1, 0, 3, false) ? 0U : 255U;
			for (std::uint32_t index = 0; index < length; index += 4) {
				const unsigned int alpha = bgra[index + 3];
				for (std::uint32_t channel = 0; channel < 3; ++channel)
					bgra[index + channel] = static_cast<std::uint8_t>((bgra[index + channel] * alpha + ground * (255U - alpha)) / 255U);
				bgra[index + 3] = 255;
			}
			std::optional<std::string> png = encoded(BitmapEncoder::PngEncoderId(), bgra, BitmapAlphaMode::Ignore);
			if (png.has_value() && png->size() <= picture::kMostBytesAsItIs) return ModelPicture{"image/png", std::move(*png)};
		}
		std::optional<std::string> jpeg = encoded(BitmapEncoder::JpegEncoderId(), bgra, BitmapAlphaMode::Ignore);
		if (jpeg.has_value() && jpeg->size() <= picture::kMostBytesAsItIs) return ModelPicture{"image/jpeg", std::move(*jpeg)};
		return std::nullopt;
	} catch (...) {
		return std::nullopt;
	}
}
#else
// Linux: no text recognition yet; a scan's pages stay unread and are said to be.
std::optional<std::string> recognize_page(FPDF_PAGE) { return std::nullopt; }
std::optional<std::string> recognize_image_file(const std::filesystem::path &) { return std::nullopt; }
// Nor a picture redrawn: one goes as it is, or not at all.
std::optional<ModelPicture> redrawn_picture(const std::filesystem::path &, const std::optional<ModelPicture> &) {
	return std::nullopt;
}
#endif

DocumentExtractionResult failure(ExtractionStatus status, std::string code, std::string message) {
	DocumentExtractionResult result;
	result.format = DocumentFormat::pdf;
	result.status = status;
	result.diagnostics.push_back({std::move(code), std::move(message)});
	return result;
}

DocumentExtractionResult pdfium_failure(ExtractionStatus status, std::string code, std::string message) {
	auto result = failure(status, std::move(code), std::move(message));
	result.extractor_version = current_extractor_version();
	return result;
}

#endif // !__APPLE__ (version, recognition and failures)

void append_utf8(std::string &out, std::uint32_t code) {
	if (code < 0x80) {
		out.push_back(static_cast<char>(code));
	} else if (code < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (code >> 6)));
		out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
	} else if (code < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (code >> 12)));
		out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (code >> 18)));
		out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
	}
}

bool is_space_or_control(std::uint32_t code) {
	return code <= 0x20 || (code >= 0x7F && code <= 0xA0) || code == 0x1680 || (code >= 0x2000 && code <= 0x200B)
		|| code == 0x2028 || code == 0x2029 || code == 0x202F || code == 0x205F || code == 0x3000
		|| code == 0xFEFF;
}

// PDFium's UTF-16 as normalized UTF-8: line breaks become "\n" (PDFium writes
// "\r\n" between lines), an unpaired surrogate becomes U+FFFD. `meaningful`
// counts the characters that are neither space nor control.
std::string utf8_text(const std::vector<unsigned short> &utf16, std::size_t length, std::size_t *meaningful) {
	std::string text;
	text.reserve(length);
	for (std::size_t index = 0; index < length; ++index) {
		std::uint32_t code = utf16[index];
		if (code >= 0xD800 && code <= 0xDBFF && index + 1 < length && utf16[index + 1] >= 0xDC00
				&& utf16[index + 1] <= 0xDFFF) {
			code = 0x10000 + ((code - 0xD800) << 10) + (utf16[index + 1] - 0xDC00u);
			++index;
		} else if (code >= 0xD800 && code <= 0xDFFF) {
			code = 0xFFFD;
		}
		if (code == '\r') {
			if (index + 1 < length && utf16[index + 1] == '\n') continue;
			code = '\n';
		}
		if (code == 0) continue;
		if (meaningful != nullptr && !is_space_or_control(code)) ++*meaningful;
		append_utf8(text, code);
	}
	return text;
}

#if !defined(__APPLE__)
PageReading read_page_text(FPDF_TEXTPAGE textPage) {
	PageReading reading;
	const int count = FPDFText_CountChars(textPage);
	if (count <= 0) return reading;
	std::vector<unsigned short> utf16(static_cast<std::size_t>(count) + 1, 0);
	const int written = FPDFText_GetText(textPage, 0, count, utf16.data());
	const std::size_t length = written > 0 ? static_cast<std::size_t>(written - 1) : 0;
	reading.text = utf8_text(utf16, length, &reading.meaningful);
	return reading;
}

// Whether the text inside a rectangle of the page holds anything but space.
bool rectangle_has_meaningful_text(FPDF_TEXTPAGE textPage, double left, double top, double right, double bottom) {
	const int length = FPDFText_GetBoundedText(textPage, left, top, right, bottom, nullptr, 0);
	if (length <= 0) return false;
	std::vector<unsigned short> utf16(static_cast<std::size_t>(length) + 1, 0);
	const int written = FPDFText_GetBoundedText(textPage, left, top, right, bottom, utf16.data(), length);
	std::size_t meaningful = 0;
	(void)utf8_text(utf16, written > 0 ? static_cast<std::size_t>(written) : 0, &meaningful);
	return meaningful > 0;
}

// A page drawn small in grey, row 0 at the top, the way a reader glances at it.
struct PageRaster {
	std::size_t width = 0;
	std::size_t height = 0;
	std::vector<unsigned char> pixels;
};

std::optional<PageRaster> render_page(FPDF_PAGE page) {
	const double pageWidth = FPDF_GetPageWidthF(page);
	const double pageHeight = FPDF_GetPageHeightF(page);
	if (!std::isfinite(pageWidth) || !std::isfinite(pageHeight) || pageWidth <= 0.0 || pageHeight <= 0.0)
		return std::nullopt;
	PageRaster raster;
	const double scale = static_cast<double>(pdf::kRasterLongEdge) / std::max(pageWidth, pageHeight);
	raster.width = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(pageWidth * scale)));
	raster.height = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(pageHeight * scale)));
	const int width = static_cast<int>(raster.width);
	const int height = static_cast<int>(raster.height);
	ScopedFPDFBitmap bitmap(FPDFBitmap_Create(width, height, 0));
	if (!bitmap) return std::nullopt;
	FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
	FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, width, height, 0, 0);
	const auto *bgrx = static_cast<const unsigned char *>(FPDFBitmap_GetBuffer(bitmap.get()));
	const int stride = FPDFBitmap_GetStride(bitmap.get());
	if (bgrx == nullptr || stride <= 0) return std::nullopt;
	// Luminance, as the macOS adapter renders into device grey.
	raster.pixels.resize(raster.width * raster.height);
	for (std::size_t y = 0; y < raster.height; ++y) {
		const unsigned char *row = bgrx + y * static_cast<std::size_t>(stride);
		for (std::size_t x = 0; x < raster.width; ++x) {
			const unsigned char *pixel = row + x * 4;
			raster.pixels[y * raster.width + x] =
				static_cast<unsigned char>((pixel[2] * 299 + pixel[1] * 587 + pixel[0] * 114) / 1000);
		}
	}
	return raster;
}

// A rectangle of the page (PDF user space) marked on a mask of the raster:
// its corners go through the page's own transformation to the raster, rotation
// included, and the shared rule marks the pixels it covers.
void mark_page_rectangle(FPDF_PAGE page, const PageRaster &raster, double left, double bottom, double right,
		double top, double pad, std::vector<unsigned char> &mask) {
	const std::array<std::pair<double, double>, 4> corners{
		{{left, bottom}, {left, top}, {right, bottom}, {right, top}}};
	double minX = 0, minY = 0, maxX = 0, maxY = 0;
	bool first = true;
	for (const auto &[x, y] : corners) {
		int deviceX = 0;
		int deviceY = 0;
		if (!FPDF_PageToDevice(page, 0, 0, static_cast<int>(raster.width), static_cast<int>(raster.height), 0, x, y,
				&deviceX, &deviceY))
			return;
		// Device rows run down from the top; the shared rule counts up from the bottom.
		const double rasterX = static_cast<double>(deviceX);
		const double rasterY = static_cast<double>(raster.height) - static_cast<double>(deviceY);
		if (first) {
			minX = maxX = rasterX;
			minY = maxY = rasterY;
			first = false;
		} else {
			minX = std::min(minX, rasterX);
			maxX = std::max(maxX, rasterX);
			minY = std::min(minY, rasterY);
			maxY = std::max(maxY, rasterY);
		}
	}
	pdf::mark_rectangle(raster.width, raster.height, minX, minY, maxX, maxY, pad, mask);
}

// An affine map of PDF space: x' = a x + c y + e, y' = b x + d y + f.
struct Matrix {
	double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

std::pair<double, double> apply(const Matrix &m, double x, double y) {
	return {m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f};
}

#endif // !__APPLE__ (page text and raster)

struct Box {
	double left = 0, bottom = 0, right = 0, top = 0;
};

#if !defined(__APPLE__)
// Where the page paints raster images - a scanned sheet, a photographed page,
// a pasted signature - as rectangles of the page: its image objects (inline
// images included) and those held by its forms, each carried out of its forms
// by their matrices. Bounded as kDeepestForm, kMostFormsRead and
// kMostImagesPlaced say.
struct ImageWalk {
	std::vector<Box> images;
	std::size_t formsRead = 0;
};

void place_image(FPDF_PAGEOBJECT object, const std::vector<Matrix> &forms, ImageWalk &walk) {
	if (walk.images.size() >= kMostImagesPlaced) return;
	float left = 0, bottom = 0, right = 0, top = 0;
	if (!FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top)) return;
	std::array<std::pair<double, double>, 4> corners{
		{{left, bottom}, {left, top}, {right, bottom}, {right, top}}};
	for (auto form = forms.rbegin(); form != forms.rend(); ++form)
		for (auto &corner : corners) corner = apply(*form, corner.first, corner.second);
	Box box{corners[0].first, corners[0].second, corners[0].first, corners[0].second};
	for (const auto &[x, y] : corners) {
		box.left = std::min(box.left, x);
		box.right = std::max(box.right, x);
		box.bottom = std::min(box.bottom, y);
		box.top = std::max(box.top, y);
	}
	if (std::isfinite(box.left) && std::isfinite(box.right) && std::isfinite(box.bottom) && std::isfinite(box.top))
		walk.images.push_back(box);
}

void walk_objects(FPDF_PAGE page, FPDF_PAGEOBJECT form, std::vector<Matrix> &forms, ImageWalk &walk) {
	const int count = form == nullptr ? FPDFPage_CountObjects(page) : FPDFFormObj_CountObjects(form);
	for (int index = 0; index < count && walk.images.size() < kMostImagesPlaced; ++index) {
		FPDF_PAGEOBJECT object = form == nullptr ? FPDFPage_GetObject(page, index)
			: FPDFFormObj_GetObject(form, static_cast<unsigned long>(index));
		if (object == nullptr) continue;
		const int type = FPDFPageObj_GetType(object);
		if (type == FPDF_PAGEOBJ_IMAGE) {
			place_image(object, forms, walk);
		} else if (type == FPDF_PAGEOBJ_FORM && forms.size() < kDeepestForm && walk.formsRead < kMostFormsRead) {
			++walk.formsRead;
			FS_MATRIX m{};
			if (!FPDFPageObj_GetMatrix(object, &m)) continue;
			forms.push_back(Matrix{m.a, m.b, m.c, m.d, m.e, m.f});
			walk_objects(page, object, forms, walk);
			forms.pop_back();
		}
	}
}

std::vector<Box> image_placements(FPDF_PAGE page) {
	ImageWalk walk;
	std::vector<Matrix> forms;
	walk_objects(page, nullptr, forms, walk);
	return walk.images;
}

#endif // !__APPLE__ (image walk)

Box page_box(FPDF_PAGE page) {
	float left = 0, bottom = 0, right = 0, top = 0;
	if (FPDFPage_GetCropBox(page, &left, &bottom, &right, &top) || FPDFPage_GetMediaBox(page, &left, &bottom, &right, &top))
		return Box{std::min(left, right), std::min(bottom, top), std::max(left, right), std::max(bottom, top)};
	return Box{0, 0, FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)};
}

#if !defined(__APPLE__)
// Whether raster images could cover a good part of the page at all: the sum of
// their areas on the page, which is never less than the part they cover. A
// page that fails this holds no scan, and is not drawn to find out.
bool could_hold_a_picture(FPDF_PAGE page, const std::vector<Box> &images) {
	const Box box = page_box(page);
	const double pageArea = (box.right - box.left) * (box.top - box.bottom);
	if (!std::isfinite(pageArea) || pageArea <= 0.0) return false;
	double covered = 0.0;
	for (const Box &image : images) {
		const double width = std::min(image.right, box.right) - std::max(image.left, box.left);
		const double height = std::min(image.top, box.top) - std::max(image.bottom, box.bottom);
		if (width > 0.0 && height > 0.0 && std::isfinite(width * height)) covered += width * height;
	}
	return covered / pageArea >= pdf::kPictureShareOfPage;
}

// Whether a page shows content that was not read. With no text at all, any
// material ink is such content; with text, only a picture beyond that text is.
// A page with text and no raster image large enough to be a scan is read, and
// is never drawn to find out.
bool page_shows_unread_content(FPDF_PAGE page, FPDF_TEXTPAGE textPage, std::size_t meaningful) {
	if (meaningful == 0) {
		const auto raster = render_page(page);
		return raster.has_value() && pdf::has_material_ink(raster->pixels, raster->width, raster->height);
	}
	const std::vector<Box> images = image_placements(page);
	if (!could_hold_a_picture(page, images)) return false;
	const auto raster = render_page(page);
	if (!raster.has_value()) return false;
	std::vector<unsigned char> picture(raster->pixels.size(), 0);
	for (const Box &image : images)
		mark_page_rectangle(page, *raster, image.left, image.bottom, image.right, image.top, 0.0, picture);
	// The text that was read, run by run: a run's rectangle holds its glyphs.
	std::vector<unsigned char> readText(raster->pixels.size(), 0);
	if (textPage != nullptr) {
		const int runs = FPDFText_CountRects(textPage, 0, -1);
		for (int index = 0; index < runs; ++index) {
			double left = 0, top = 0, right = 0, bottom = 0;
			if (!FPDFText_GetRect(textPage, index, &left, &top, &right, &bottom)) continue;
			if (!rectangle_has_meaningful_text(textPage, left, top, right, bottom)) continue;
			mark_page_rectangle(page, *raster, left, bottom, right, top, 1.5, readText);
		}
	}
	return pdf::picture_outweighs_its_text(raster->pixels, raster->width, raster->height, picture, readText);
}

#endif // !__APPLE__ (page gate)

} // namespace

#if !defined(__APPLE__)
std::string pdf_extractor_version() { return current_extractor_version(); }

std::optional<std::string> recognize_image_text(const std::filesystem::path &path) { return recognize_image_file(path); }

// What Windows's text recognition reads in a picture, word by word; nothing
// on Linux, which reads none yet.
std::vector<std::string> picture_word_readings(const std::filesystem::path &path) {
	std::vector<std::string> words;
	const std::optional<std::string> text = recognize_image_file(path);
	if (text.has_value() == false) return words;
	std::istringstream pieces(*text);
	for (std::string piece; pieces >> piece;)
		if (std::find(words.begin(), words.end(), piece) == words.end()) words.push_back(piece);
	return words;
}

// A picture as a model reads it: as it is where every provider takes it,
// otherwise redrawn where this system can (redrawn_picture). One that shows
// through is laid on its ground where this system redraws; Linux, which redraws
// none, sends it as it is.
std::optional<ModelPicture> picture_for_model(const std::filesystem::path &path) {
	const std::optional<ModelPicture> kept = picture::as_it_is(path);
	if (kept.has_value() && picture::shows_through(kept->bytes) == false) return kept;
	if (std::optional<ModelPicture> drawn = redrawn_picture(path, kept)) return drawn;
	return kept;
}

bool pdf_text_recognition_available() {
#if defined(_WIN32)
	try {
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		} catch (const winrt::hresult_error &) {
		}
		return winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages().Size() > 0;
	} catch (...) {
		return false;
	}
#else
	return false;
#endif
}

DocumentExtractionResult extract_pdf_document(const DocumentExtractionRequest &request) {
	std::error_code error;
	if (!std::filesystem::is_regular_file(request.path, error) || error)
		return failure(ExtractionStatus::unreadable, "unreadable_file",
			"The PDF is missing or is not a readable regular file.");
	const auto size = std::filesystem::file_size(request.path, error);
	if (error)
		return failure(ExtractionStatus::unreadable, "unreadable_file", "The PDF size could not be read.");
	if (size > request.maximum_input_bytes)
		return failure(ExtractionStatus::unreadable, "input_too_large",
			"The PDF exceeds the configured local extraction limit.");

	// Read whole and handed over in memory: no file name, in any script, ever
	// reaches PDFium.
	std::vector<char> bytes;
	{
		std::ifstream input(request.path, std::ios::binary);
		if (!input)
			return failure(ExtractionStatus::unreadable, "unreadable_file", "The PDF could not be opened.");
		bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		if (bytes.size() != size)
			return failure(ExtractionStatus::unreadable, "unreadable_file", "The PDF could not be read completely.");
	}

	const std::lock_guard<std::mutex> lock(pdfium_mutex());
	ensure_pdfium();
	ScopedFPDFDocument document(FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr));
	if (!document) {
		const unsigned long reason = FPDF_GetLastError();
		if (reason == FPDF_ERR_PASSWORD || reason == FPDF_ERR_SECURITY)
			return pdfium_failure(ExtractionStatus::unreadable, "encrypted_pdf",
				"Encrypted PDFs must be unlocked before local extraction.");
		return pdfium_failure(ExtractionStatus::unreadable, "malformed_pdf", "PDFium could not parse this PDF.");
	}
	// Like PDFKit's isEncrypted: a PDF under any security handler is refused,
	// even one that opens without a password.
	if (FPDF_GetSecurityHandlerRevision(document.get()) != -1)
		return pdfium_failure(ExtractionStatus::unreadable, "encrypted_pdf",
			"Encrypted PDFs must be unlocked before local extraction.");

	const int pageCount = FPDF_GetPageCount(document.get());
	if (pageCount <= 0)
		return pdfium_failure(ExtractionStatus::unreadable, "malformed_pdf", "The PDF contains no readable page tree.");

	// The pages are read from their copy in a new document. PDFium reads a
	// stream of the file afresh each time a form draws it, and its guard against
	// a form that draws itself compares where the stream's bytes lie: read
	// afresh, they never lie in the same place twice, and a form that draws
	// itself forty times would be drawn 40^40 times. The copy holds each stream
	// in memory once, so the guard holds for a form stored uncompressed. A
	// compressed one is still decoded afresh at each drawing and is not bounded
	// here; neither are forms nested deep and wide. Those wait for PDFs to be
	// read in a process of their own, under limits.
	ScopedFPDFDocument copy(FPDF_CreateNewDocument());
	if (!copy || !FPDF_ImportPagesByIndex(copy.get(), document.get(), nullptr, 0, 0)
			|| FPDF_GetPageCount(copy.get()) != pageCount)
		return pdfium_failure(ExtractionStatus::unreadable, "malformed_pdf",
			"PDFium could not read a declared PDF page.");
	document.reset();
	std::vector<char>().swap(bytes);

	DocumentExtractionResult result;
	result.format = DocumentFormat::pdf;
	result.extractor_version = current_extractor_version();
	result.structure.page_count = static_cast<std::size_t>(pageCount);

	std::vector<PageReading> pages;
	pages.reserve(static_cast<std::size_t>(pageCount));
	std::size_t recognizedPages = 0;
	for (int index = 0; index < pageCount; ++index) {
		ScopedFPDFPage page(FPDF_LoadPage(copy.get(), index));
		if (!page)
			return pdfium_failure(ExtractionStatus::unreadable, "malformed_pdf",
				"PDFium could not read a declared PDF page.");
		ScopedFPDFTextPage textPage(FPDFText_LoadPage(page.get()));
		PageReading reading;
		if (textPage) reading = read_page_text(textPage.get());
		reading.isImage = page_shows_unread_content(page.get(), textPage.get(), reading.meaningful);
		// A page that is a picture with no text of its own is read by text
		// recognition; a picture beside text it does not stand for - a
		// signature under a typed page - keeps its text and its note.
		if (reading.isImage && reading.meaningful == 0 && recognizedPages < kMostRecognizedPages) {
			if (const std::optional<std::string> recognized = recognize_page(page.get())) {
				++recognizedPages;
				reading.text = *recognized;
				reading.meaningful = letters_and_digits(*recognized);
				reading.isImage = false;
				reading.isRecognized = true;
			}
		}
		pages.push_back(std::move(reading));
	}

	pdf::assemble_reading(pages, result);
	return result;
}

#endif // !__APPLE__ (reading)

// ---- Work on a PDF, always in a copy ---------------------------------------

namespace {

bool read_whole(const std::filesystem::path &path, std::vector<char> &bytes) {
	std::ifstream input(path, std::ios::binary);
	if (!input) return false;
	bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	return true;
}

// A page's characters as UTF-8, with the PDFium character each byte is part
// of; the line breaks PDFium writes between lines read as spaces.
struct PageChars {
	std::string text;
	std::vector<int> char_of_byte;
};

PageChars page_chars(FPDF_TEXTPAGE textPage) {
	PageChars chars;
	const int count = textPage != nullptr ? FPDFText_CountChars(textPage) : 0;
	for (int index = 0; index < count; ++index) {
		std::uint32_t code = FPDFText_GetUnicode(textPage, index);
		if (code == '\r' || code == '\n' || code == 0 || code == 0xFFFE || code == 0xFFFF) code = ' ';
		std::string utf8;
		append_utf8(utf8, code);
		for (const char byte : utf8) {
			chars.text += byte;
			chars.char_of_byte.push_back(index);
		}
	}
	return chars;
}

struct FoundQuote {
	int page = -1;
	int first = 0;
	int count = 0;
};

// Where a mark's words stand: on its page, else anywhere, and only where they
// stand once.
std::pair<std::optional<FoundQuote>, std::string> find_mark(FPDF_DOCUMENT document, const PdfMark &mark) {
	const int pageCount = FPDF_GetPageCount(document);
	std::vector<int> order;
	if (mark.page > 0 && static_cast<int>(mark.page) <= pageCount) order.push_back(static_cast<int>(mark.page) - 1);
	for (int index = 0; index < pageCount; ++index) {
		if (order.empty() == false && index == order.front()) continue;
		order.push_back(index);
	}
	std::optional<FoundQuote> found;
	std::size_t matches = 0;
	bool isOnNamedPage = false;
	for (std::size_t position = 0; position < order.size(); ++position) {
		const int index = order[position];
		ScopedFPDFPage page(FPDF_LoadPage(document, index));
		if (!page) continue;
		ScopedFPDFTextPage textPage(FPDFText_LoadPage(page.get()));
		const PageChars chars = page_chars(textPage.get());
		const platform::PhrasePlacement placement = platform::locate_phrase(chars.text, mark.quote);
		if (placement.matches == 0 || placement.length == 0) continue;
		matches += placement.matches;
		if (found.has_value() == false) {
			FoundQuote quote;
			quote.page = index;
			quote.first = chars.char_of_byte[placement.start];
			quote.count = chars.char_of_byte[placement.start + placement.length - 1] - quote.first + 1;
			found = quote;
			isOnNamedPage = position == 0 && mark.page > 0;
		}
		// Words found on the page the mark names stand there, whatever else
		// holds them.
		if (isOnNamedPage == true && placement.matches == 1) return {found, std::string()};
		if (matches > 1) break;
	}
	if (found.has_value() == false) return {std::nullopt, "These words are not in the PDF's text: “" + mark.quote + "”."};
	if (matches > 1)
		return {std::nullopt, "These words stand in more than one place in the PDF, so the mark could not be anchored: “"
			+ mark.quote + "”."};
	return {found, std::string()};
}

std::vector<FS_RECTF> quote_rects(FPDF_TEXTPAGE textPage, const FoundQuote &quote) {
	std::vector<FS_RECTF> rects;
	const int count = FPDFText_CountRects(textPage, quote.first, quote.count);
	for (int index = 0; index < count; ++index) {
		double left = 0, top = 0, right = 0, bottom = 0;
		if (!FPDFText_GetRect(textPage, index, &left, &top, &right, &bottom)) continue;
		rects.push_back(FS_RECTF{static_cast<float>(left), static_cast<float>(top), static_cast<float>(right),
			static_cast<float>(bottom)});
	}
	return rects;
}

// UTF-8 as the null-terminated UTF-16 PDFium takes.
std::vector<unsigned short> utf16_of(const std::string &text) {
	std::vector<unsigned short> out;
	for (std::size_t index = 0; index < text.size();) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		std::uint32_t code = 0xFFFD;
		std::size_t length = 1;
		if (byte < 0x80) code = byte;
		else if ((byte >> 5) == 0x6 && index + 1 < text.size()) { code = ((byte & 0x1Fu) << 6) | (text[index + 1] & 0x3Fu); length = 2; }
		else if ((byte >> 4) == 0xE && index + 2 < text.size()) {
			code = ((byte & 0x0Fu) << 12) | ((text[index + 1] & 0x3Fu) << 6) | (text[index + 2] & 0x3Fu);
			length = 3;
		} else if ((byte >> 3) == 0x1E && index + 3 < text.size()) {
			code = ((byte & 0x07u) << 18) | ((text[index + 1] & 0x3Fu) << 12) | ((text[index + 2] & 0x3Fu) << 6)
				| (text[index + 3] & 0x3Fu);
			length = 4;
		}
		if (code >= 0x10000) {
			code -= 0x10000;
			out.push_back(static_cast<unsigned short>(0xD800 + (code >> 10)));
			out.push_back(static_cast<unsigned short>(0xDC00 + (code & 0x3FF)));
		} else {
			out.push_back(static_cast<unsigned short>(code));
		}
		index += length;
	}
	out.push_back(0);
	return out;
}

std::string annotation_string(FPDF_ANNOTATION annotation, const char *key) {
	const unsigned long bytes = FPDFAnnot_GetStringValue(annotation, key, nullptr, 0);
	if (bytes <= 2) return std::string();
	std::vector<unsigned short> utf16(bytes / 2, 0);
	FPDFAnnot_GetStringValue(annotation, key, utf16.data(), bytes);
	return utf8_text(utf16, utf16.size() - 1, nullptr);
}

struct MemoryWriter : FPDF_FILEWRITE {
	std::string bytes;
};

int append_block(FPDF_FILEWRITE *self, const void *data, unsigned long size) {
	static_cast<MemoryWriter *>(self)->bytes.append(static_cast<const char *>(data), size);
	return 1;
}

std::optional<std::string> saved(FPDF_DOCUMENT document) {
	MemoryWriter writer;
	writer.version = 1;
	writer.WriteBlock = append_block;
	if (!FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL)) return std::nullopt;
	return writer.bytes;
}

std::vector<std::string> page_texts(FPDF_DOCUMENT document) {
	std::vector<std::string> texts;
	const int count = FPDF_GetPageCount(document);
	for (int index = 0; index < count; ++index) {
		ScopedFPDFPage page(FPDF_LoadPage(document, index));
		ScopedFPDFTextPage textPage(page ? FPDFText_LoadPage(page.get()) : nullptr);
		texts.push_back(page_chars(textPage.get()).text);
	}
	return texts;
}

// The copy's bytes, kept at `target` only where nothing is there: staged
// beside it, then moved into place.
std::string keep_copy(const std::string &bytes, const std::filesystem::path &target) {
	std::error_code exists;
	if (std::filesystem::exists(target, exists))
		return "A file of the copy's name is already there, so nothing was written over it.";
	const std::filesystem::path staged = target.parent_path() / std::filesystem::u8path(target.filename().u8string() + ".staging");
	{
		std::ofstream output(staged, std::ios::binary | std::ios::trunc);
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		output.flush();
		if (!output) {
			std::error_code removed;
			std::filesystem::remove(staged, removed);
			return "The copy could not be written beside the original.";
		}
	}
	std::error_code renamed;
	std::filesystem::rename(staged, target, renamed);
	if (renamed) {
		std::error_code removed;
		std::filesystem::remove(staged, removed);
		return "The copy could not be saved.";
	}
	return std::string();
}

struct OpenedPdf {
	std::vector<char> bytes;
	ScopedFPDFDocument document;
	std::string problem;
};

void open_pdf(const std::filesystem::path &source, OpenedPdf &opened) {
	if (!read_whole(source, opened.bytes) || opened.bytes.empty()) {
		opened.problem = "The PDF could not be read.";
		return;
	}
	opened.document.reset(FPDF_LoadMemDocument64(opened.bytes.data(), opened.bytes.size(), nullptr));
	if (!opened.document) {
		opened.problem = "The PDF could not be opened: it may be damaged or protected.";
		return;
	}
	if (FPDF_GetSecurityHandlerRevision(opened.document.get()) != -1) {
		opened.document.reset();
		opened.problem = "The PDF is encrypted; unlock it before it is worked on.";
	}
}

} // namespace

PdfRevision write_annotated_pdf(const std::filesystem::path &source, const std::filesystem::path &target,
		const std::vector<PdfMark> &marks, const std::string &author) {
	PdfRevision revision;
	revision.path = target;
	const std::lock_guard<std::mutex> lock(pdfium_mutex());
	ensure_pdfium();
	OpenedPdf opened;
	open_pdf(source, opened);
	if (!opened.problem.empty()) {
		revision.problem = opened.problem;
		return revision;
	}
	FPDF_DOCUMENT document = opened.document.get();
	const std::vector<std::string> before = page_texts(document);
	// Highlights written on each page, with the note each carries.
	std::map<int, std::vector<std::string>> expected;
	for (const PdfMark &mark : marks) {
		PdfMarkOutcome outcome;
		const auto [found, why] = find_mark(document, mark);
		if (!found.has_value()) {
			outcome.state = why.find("more than one") != std::string::npos ? "ambiguous" : "not_found";
			outcome.note = why;
			revision.outcomes.push_back(outcome);
			continue;
		}
		ScopedFPDFPage page(FPDF_LoadPage(document, found->page));
		ScopedFPDFTextPage textPage(page ? FPDFText_LoadPage(page.get()) : nullptr);
		const std::vector<FS_RECTF> rects = textPage ? quote_rects(textPage.get(), *found) : std::vector<FS_RECTF>();
		if (!page || rects.empty()) {
			outcome.state = "not_found";
			outcome.note = "The words were found but their place on the page could not be measured.";
			revision.outcomes.push_back(outcome);
			continue;
		}
		ScopedFPDFAnnotation annotation(FPDFPage_CreateAnnot(page.get(), FPDF_ANNOT_HIGHLIGHT));
		if (!annotation) {
			outcome.state = "not_found";
			outcome.note = "The highlight could not be made on this page.";
			revision.outcomes.push_back(outcome);
			continue;
		}
		FS_RECTF box = rects.front();
		for (const FS_RECTF &rect : rects) {
			FS_QUADPOINTSF quad{rect.left, rect.top, rect.right, rect.top, rect.left, rect.bottom, rect.right, rect.bottom};
			FPDFAnnot_AppendAttachmentPoints(annotation.get(), &quad);
			box.left = std::min(box.left, rect.left);
			box.right = std::max(box.right, rect.right);
			box.top = std::max(box.top, rect.top);
			box.bottom = std::min(box.bottom, rect.bottom);
		}
		FPDFAnnot_SetRect(annotation.get(), &box);
		FPDFAnnot_SetColor(annotation.get(), FPDFANNOT_COLORTYPE_Color, 255, 214, 10, 255);
		const std::vector<unsigned short> note = utf16_of(mark.comment);
		const std::vector<unsigned short> name = utf16_of(author);
		FPDFAnnot_SetStringValue(annotation.get(), "Contents", note.data());
		FPDFAnnot_SetStringValue(annotation.get(), "T", name.data());
		annotation.reset();
		// Drawn once, so PDFium gives the highlight its appearance before it is
		// saved: every reader then shows it as drawn here.
		{
			ScopedFPDFBitmap bitmap(FPDFBitmap_Create(32, 32, 0));
			if (bitmap) FPDF_RenderPageBitmap(bitmap.get(), page.get(), 0, 0, 32, 32, 0, FPDF_ANNOT);
		}
		outcome.state = "written";
		outcome.page = static_cast<unsigned int>(found->page + 1);
		expected[found->page].push_back(mark.comment);
		++revision.written;
		revision.outcomes.push_back(outcome);
	}
	if (revision.written == 0) {
		revision.problem = "None of the passages could be found once in the PDF, so nothing was marked.";
		return revision;
	}
	const std::optional<std::string> bytes = saved(document);
	if (!bytes) {
		revision.problem = "The annotated copy could not be saved.";
		return revision;
	}
	// Read back: the copy's own pages, highlights and text.
	ScopedFPDFDocument copy(FPDF_LoadMemDocument64(bytes->data(), bytes->size(), nullptr));
	if (!copy || FPDF_GetPageCount(copy.get()) != FPDF_GetPageCount(document)) {
		revision.problem = "The annotated copy did not read back as a PDF with the original's pages, so it was not kept.";
		return revision;
	}
	if (page_texts(copy.get()) != before) {
		revision.problem = "The annotated copy's text did not read as the original's, so it was not kept.";
		return revision;
	}
	for (const auto &[pageIndex, notes] : expected) {
		ScopedFPDFPage page(FPDF_LoadPage(copy.get(), pageIndex));
		std::vector<std::string> found;
		const int count = page ? FPDFPage_GetAnnotCount(page.get()) : 0;
		for (int index = 0; index < count; ++index) {
			ScopedFPDFAnnotation annotation(FPDFPage_GetAnnot(page.get(), index));
			if (!annotation || FPDFAnnot_GetSubtype(annotation.get()) != FPDF_ANNOT_HIGHLIGHT) continue;
			found.push_back(annotation_string(annotation.get(), "Contents"));
		}
		for (const std::string &note : notes) {
			if (std::find(found.begin(), found.end(), note) == found.end()) {
				revision.problem = "A highlight did not read back on page " + std::to_string(pageIndex + 1) + ", so the copy was not kept.";
				return revision;
			}
		}
	}
	revision.problem = keep_copy(*bytes, target);
	revision.is_written = revision.problem.empty();
	return revision;
}

PdfRevision write_redacted_pdf(const std::filesystem::path &source, const std::filesystem::path &target,
		const std::vector<PdfMark> &marks) {
	PdfRevision revision;
	revision.path = target;
	const std::lock_guard<std::mutex> lock(pdfium_mutex());
	ensure_pdfium();
	OpenedPdf opened;
	open_pdf(source, opened);
	if (!opened.problem.empty()) {
		revision.problem = opened.problem;
		return revision;
	}
	FPDF_DOCUMENT document = opened.document.get();
	const std::vector<std::string> before = page_texts(document);
	// Every passage found first, on the pages as they are: redacting one page
	// changes nothing another passage is found by.
	std::map<int, std::vector<FS_RECTF>> areas;
	std::vector<std::string> redacted;
	for (const PdfMark &mark : marks) {
		PdfMarkOutcome outcome;
		const auto [found, why] = find_mark(document, mark);
		if (!found.has_value()) {
			outcome.state = why.find("more than one") != std::string::npos ? "ambiguous" : "not_found";
			outcome.note = why;
			revision.outcomes.push_back(outcome);
			continue;
		}
		ScopedFPDFPage page(FPDF_LoadPage(document, found->page));
		ScopedFPDFTextPage textPage(page ? FPDFText_LoadPage(page.get()) : nullptr);
		const std::vector<FS_RECTF> rects = textPage ? quote_rects(textPage.get(), *found) : std::vector<FS_RECTF>();
		if (rects.empty() || FPDFPage_GetRotation(page.get()) != 0) {
			outcome.state = "not_found";
			outcome.note = rects.empty() ? "The words were found but their place on the page could not be measured."
				: "The page is rotated; redact it in a PDF editor.";
			revision.outcomes.push_back(outcome);
			continue;
		}
		for (const FS_RECTF &rect : rects) areas[found->page].push_back(rect);
		redacted.push_back(mark.quote);
		outcome.state = "written";
		outcome.page = static_cast<unsigned int>(found->page + 1);
		++revision.written;
		revision.outcomes.push_back(outcome);
	}
	if (revision.written == 0) {
		revision.problem = "None of the passages could be found once in the PDF, so nothing was redacted.";
		return revision;
	}
	for (const auto &[pageIndex, rects] : areas) {
		ScopedFPDFPage page(FPDF_LoadPage(document, pageIndex));
		if (!page) {
			revision.problem = "A page to redact could not be opened, so nothing was kept.";
			return revision;
		}
		const auto overlaps = [&](const FS_RECTF &box) {
			return std::any_of(rects.begin(), rects.end(), [&](const FS_RECTF &rect) {
				return box.left <= rect.right + 2 && box.right >= rect.left - 2 && box.bottom <= rect.top + 2
					&& box.top >= rect.bottom - 2;
			});
		};
		// An annotation over a passage could repeat its words: it goes too.
		for (int index = FPDFPage_GetAnnotCount(page.get()) - 1; index >= 0; --index) {
			bool isOver = false;
			{
				ScopedFPDFAnnotation annotation(FPDFPage_GetAnnot(page.get(), index));
				FS_RECTF box{};
				isOver = annotation && FPDFAnnot_GetRect(annotation.get(), &box) && overlaps(box);
			}
			if (isOver) FPDFPage_RemoveAnnot(page.get(), index);
		}
		// The page drawn as it reads, at up to 200 dots an inch, the passages
		// blacked out in the drawing itself.
		const Box box = page_box(page.get());
		const double width = box.right - box.left;
		const double height = box.top - box.bottom;
		if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0) {
			revision.problem = "A page to redact has no size, so nothing was kept.";
			return revision;
		}
		const double scale = std::min(200.0 / 72.0, 5000.0 / std::max(width, height));
		const int pixelsWide = std::max(1, static_cast<int>(std::ceil(width * scale)));
		const int pixelsHigh = std::max(1, static_cast<int>(std::ceil(height * scale)));
		ScopedFPDFBitmap bitmap(FPDFBitmap_Create(pixelsWide, pixelsHigh, 0));
		if (!bitmap) {
			revision.problem = "A page to redact could not be drawn, so nothing was kept.";
			return revision;
		}
		FPDFBitmap_FillRect(bitmap.get(), 0, 0, pixelsWide, pixelsHigh, 0xFFFFFFFF);
		FPDF_RenderPageBitmap(bitmap.get(), page.get(), 0, 0, pixelsWide, pixelsHigh, 0, 0);
		for (const FS_RECTF &rect : rects) {
			int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
			FPDF_PageToDevice(page.get(), 0, 0, pixelsWide, pixelsHigh, 0, rect.left - 1.0, rect.top + 1.0, &x1, &y1);
			FPDF_PageToDevice(page.get(), 0, 0, pixelsWide, pixelsHigh, 0, rect.right + 1.0, rect.bottom - 1.0, &x2, &y2);
			const int left = std::max(0, std::min(x1, x2));
			const int top = std::max(0, std::min(y1, y2));
			const int right = std::min(pixelsWide, std::max(x1, x2) + 1);
			const int bottom = std::min(pixelsHigh, std::max(y1, y2) + 1);
			if (right > left && bottom > top) FPDFBitmap_FillRect(bitmap.get(), left, top, right - left, bottom - top, 0xFF000000);
		}
		// Everything the page drew goes; the drawing takes its place.
		for (int index = FPDFPage_CountObjects(page.get()) - 1; index >= 0; --index) {
			FPDF_PAGEOBJECT object = FPDFPage_GetObject(page.get(), index);
			if (object != nullptr && FPDFPage_RemoveObject(page.get(), object)) FPDFPageObj_Destroy(object);
		}
		FPDF_PAGEOBJECT image = FPDFPageObj_NewImageObj(document);
		FPDF_PAGE pages[] = {page.get()};
		if (image == nullptr || !FPDFImageObj_SetBitmap(pages, 1, image, bitmap.get())) {
			if (image != nullptr) FPDFPageObj_Destroy(image);
			revision.problem = "A redacted page could not be put back together, so nothing was kept.";
			return revision;
		}
		const FS_MATRIX placement{static_cast<float>(width), 0.0f, 0.0f, static_cast<float>(height),
			static_cast<float>(box.left), static_cast<float>(box.bottom)};
		FPDFPageObj_SetMatrix(image, &placement);
		FPDFPage_InsertObject(page.get(), image);
		if (!FPDFPage_GenerateContent(page.get())) {
			revision.problem = "A redacted page could not be written, so nothing was kept.";
			return revision;
		}
	}
	// Only what the pages now reach goes into the copy: the redacted pages' old
	// content, fonts and images are left behind, never carried in the file.
	ScopedFPDFDocument fresh(FPDF_CreateNewDocument());
	if (!fresh || !FPDF_ImportPagesByIndex(fresh.get(), document, nullptr, 0, 0)
			|| FPDF_GetPageCount(fresh.get()) != FPDF_GetPageCount(document)) {
		revision.problem = "The redacted copy could not be assembled, so nothing was kept.";
		return revision;
	}
	const std::optional<std::string> bytes = saved(fresh.get());
	if (!bytes) {
		revision.problem = "The redacted copy could not be saved.";
		return revision;
	}
	// Read back: not one redacted passage left in the copy's text or bytes, and
	// every page without a redaction as it was.
	ScopedFPDFDocument copy(FPDF_LoadMemDocument64(bytes->data(), bytes->size(), nullptr));
	if (!copy || FPDF_GetPageCount(copy.get()) != static_cast<int>(before.size())) {
		revision.problem = "The redacted copy did not read back with the original's pages, so it was not kept.";
		return revision;
	}
	const std::vector<std::string> after = page_texts(copy.get());
	for (std::size_t index = 0; index < after.size(); ++index) {
		for (const std::string &quote : redacted) {
			if (platform::locate_phrase(after[index], quote).matches > 0) {
				revision.problem = "A redacted passage could still be read on page " + std::to_string(index + 1)
					+ ", so the copy was not kept.";
				return revision;
			}
		}
		if (areas.count(static_cast<int>(index)) == 0 && after[index] != before[index]) {
			revision.problem = "Page " + std::to_string(index + 1) + " changed though nothing on it was redacted, so the copy "
				"was not kept.";
			return revision;
		}
	}
	for (const std::string &quote : redacted) {
		if (quote.size() >= 4 && bytes->find(quote) != std::string::npos) {
			revision.problem = "A redacted passage was still in the copy's bytes, so the copy was not kept.";
			return revision;
		}
	}
	revision.problem = keep_copy(*bytes, target);
	revision.is_written = revision.problem.empty();
	return revision;
}

} // namespace singularity::documents
