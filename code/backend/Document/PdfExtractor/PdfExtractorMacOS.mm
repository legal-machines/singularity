#include "Document/PdfExtractor/PdfExtractor.hpp"

#include "Document/PdfExtractor/PdfExtractionRules.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <PDFKit/PDFKit.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace singularity::documents {
namespace {

// The page gate's rules are shared with PDFium (PdfExtractionRules.hpp).
using pdf::assemble_reading;
using pdf::kPictureShareOfPage;
using pdf::kRasterLongEdge;
using pdf::PageReading;

std::mutex &pdfkit_mutex() {
  static std::mutex mutex;
  return mutex;
}

DocumentExtractionResult failure(ExtractionStatus status, std::string code,
                                 std::string message) {
  DocumentExtractionResult result;
  result.format = DocumentFormat::pdf;
  result.status = status;
  result.diagnostics.push_back({std::move(code), std::move(message)});
  return result;
}

std::string current_extractor_version() {
  const auto major =
      [NSProcessInfo processInfo].operatingSystemVersion.majorVersion;
  return "pdfkit-text-raster-page-gate/4;macos-" +
         std::to_string(static_cast<long long>(major));
}

DocumentExtractionResult pdfkit_failure(ExtractionStatus status,
                                        std::string code,
                                        std::string message) {
  auto result = failure(status, std::move(code), std::move(message));
  result.extractor_version = current_extractor_version();
  return result;
}

std::string normalize_text(NSString *value) {
  if (value == nil || value.length == 0) {
    return {};
  }
  NSString *normalized =
      [value stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"];
  normalized =
      [normalized stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];
  NSData *utf8 = [normalized dataUsingEncoding:NSUTF8StringEncoding
                           allowLossyConversion:NO];
  if (utf8 == nil) {
    return {};
  }
  return std::string(static_cast<const char *>(utf8.bytes), utf8.length);
}

std::size_t meaningful_character_count(NSString *value) {
  if (value == nil) {
    return 0;
  }
  NSMutableCharacterSet *ignored =
      [[NSCharacterSet whitespaceAndNewlineCharacterSet]
          mutableCopy];
  [ignored formUnionWithCharacterSet:[NSCharacterSet controlCharacterSet]];
  const auto range = [value rangeOfCharacterFromSet:[ignored invertedSet]];
  if (range.location == NSNotFound) {
    [ignored release];
    return 0;
  }

  std::size_t count = 0;
  for (NSUInteger index = 0; index < value.length;) {
    const NSRange composed =
        [value rangeOfComposedCharacterSequenceAtIndex:index];
    NSString *character = [value substringWithRange:composed];
    if ([character rangeOfCharacterFromSet:[ignored invertedSet]].location !=
        NSNotFound) {
      ++count;
    }
    index = NSMaxRange(composed);
  }
  [ignored release];
  return count;
}

// A page drawn small in grey, the way a reader glances at it. Row 0 of the
// pixels is the top of the page; `fromPage` takes a point of the page (PDF user
// space) to the bitmap, whose origin is its bottom-left corner.
struct PageRaster {
	std::size_t width = 0;
	std::size_t height = 0;
	std::vector<unsigned char> pixels;
	CGAffineTransform fromPage = CGAffineTransformIdentity;
};

std::optional<PageRaster> render_page(PDFPage *page) {

	const CGRect bounds = [page boundsForBox:kPDFDisplayBoxCropBox];
	if (CGRectIsEmpty(bounds) == true || std::isfinite(bounds.size.width) == false
			|| std::isfinite(bounds.size.height) == false || bounds.size.width <= 0.0 || bounds.size.height <= 0.0) {
		return std::nullopt;
	}

	PageRaster raster;
	const double scale = static_cast<double>(kRasterLongEdge) / std::max(bounds.size.width, bounds.size.height);
	raster.width = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(bounds.size.width * scale)));
	raster.height = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(bounds.size.height * scale)));
	raster.pixels.assign(raster.width * raster.height, 255);

	CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceGray();
	CGContextRef context = CGBitmapContextCreate(raster.pixels.data(), raster.width, raster.height, 8, raster.width,
			colorSpace, static_cast<CGBitmapInfo>(kCGImageAlphaNone));
	CGColorSpaceRelease(colorSpace);
	if (context == nullptr) return std::nullopt;

	CGContextSetGrayFillColor(context, 1.0, 1.0);
	CGContextFillRect(context, CGRectMake(0, 0, static_cast<CGFloat>(raster.width), static_cast<CGFloat>(raster.height)));
	CGContextSetInterpolationQuality(context, kCGInterpolationLow);

	if (CGPDFPageRef pageRef = page.pageRef; pageRef != nullptr) {
		const CGRect target = CGRectMake(0, 0, static_cast<CGFloat>(raster.width), static_cast<CGFloat>(raster.height));
		raster.fromPage = CGPDFPageGetDrawingTransform(pageRef, kCGPDFCropBox, target, 0, true);
		CGContextConcatCTM(context, raster.fromPage);
		CGContextDrawPDFPage(context, pageRef);
	} else {
		raster.fromPage = CGAffineTransformTranslate(CGAffineTransformMakeScale(scale, scale), -bounds.origin.x, -bounds.origin.y);
		CGContextConcatCTM(context, raster.fromPage);
		[page drawWithBox:kPDFDisplayBoxCropBox toContext:context];
	}
	CGContextFlush(context);
	CGContextRelease(context);
	return raster;

}

// Marks, in a mask the size of the raster, the pixels a rectangle of the page
// covers, with `pad` more pixels around it.
void mark_on_raster(const PageRaster &raster, CGRect onPage, double pad, std::vector<unsigned char> &mask) {

	const CGRect onRaster = CGRectApplyAffineTransform(onPage, raster.fromPage);
	if (CGRectIsNull(onRaster) == true) return;
	pdf::mark_rectangle(raster.width, raster.height, CGRectGetMinX(onRaster), CGRectGetMinY(onRaster),
			CGRectGetMaxX(onRaster), CGRectGetMaxY(onRaster), pad, mask);

}

// A page with no text of its own that still shows something: a scan, a
// drawing, a stamp.
bool has_material_ink(const PageRaster &raster) {
	return pdf::has_material_ink(raster.pixels, raster.width, raster.height);
}

// Where a page paints raster images - a scanned sheet, a photographed page, a
// pasted signature - as rectangles of the page, found by walking its content
// stream: the images themselves (Do of an Image XObject, inline images) and
// the forms that hold them, each placed by the transformation in force.
//
// The walk is bounded, whatever the file: a form is never entered again while
// it is being read (a form that draws itself), and a page's forms are read at
// most kMostFormsRead times and kMostFormBytes in all (a form drawn forty times
// by a form drawn forty times). What lies past the bound is not looked at: the
// page keeps the text it has, and every other PDF waits on this one only as
// long as an ordinary page takes.
struct ImagePlacements {
	CGAffineTransform ctm = CGAffineTransformIdentity;
	std::vector<CGAffineTransform> saved;
	std::vector<CGRect> images;
	CGPDFOperatorTableRef operators = nullptr;
	std::vector<CGPDFStreamRef> formsOpen;
	std::size_t formsRead = 0;
	std::size_t formBytesRead = 0;
};

constexpr std::size_t kDeepestForm = 8;
constexpr std::size_t kMostFormsRead = 64;
constexpr std::size_t kMostFormBytes = 8 * 1024 * 1024;
constexpr std::size_t kMostImagesPlaced = 1024;

void place_image(ImagePlacements &placements) {
	if (placements.images.size() >= kMostImagesPlaced) return;
	placements.images.push_back(CGRectApplyAffineTransform(CGRectMake(0, 0, 1, 1), placements.ctm));
}

void on_save(CGPDFScannerRef, void *info) {
	ImagePlacements &placements = *static_cast<ImagePlacements*>(info);
	placements.saved.push_back(placements.ctm);
}

void on_restore(CGPDFScannerRef, void *info) {
	ImagePlacements &placements = *static_cast<ImagePlacements*>(info);
	if (placements.saved.empty() == true) return;
	placements.ctm = placements.saved.back();
	placements.saved.pop_back();
}

void on_transform(CGPDFScannerRef scanner, void *info) {
	ImagePlacements &placements = *static_cast<ImagePlacements*>(info);
	CGPDFReal values[6];
	for (int index = 5; index >= 0; --index) {
		if (CGPDFScannerPopNumber(scanner, &values[index]) == false) return;
	}
	const CGAffineTransform matrix = CGAffineTransformMake(values[0], values[1], values[2], values[3], values[4], values[5]);
	placements.ctm = CGAffineTransformConcat(matrix, placements.ctm);
}

void on_inline_image(CGPDFScannerRef, void *info) {
	place_image(*static_cast<ImagePlacements*>(info));
}

// Whether one more form may be read: not one already open, and not past the
// page's bound. A form whose length is not declared counts as a large one.
bool may_read_form(ImagePlacements &placements, CGPDFStreamRef stream, CGPDFDictionaryRef dictionary) {

	if (placements.formsOpen.size() >= kDeepestForm) return false;
	if (placements.formsRead >= kMostFormsRead) return false;
	if (std::find(placements.formsOpen.begin(), placements.formsOpen.end(), stream) != placements.formsOpen.end()) return false;
	CGPDFInteger length = 0;
	const std::size_t bytes = CGPDFDictionaryGetInteger(dictionary, "Length", &length) == true && length >= 0
		? static_cast<std::size_t>(length) : kMostFormBytes / 4;
	if (bytes > kMostFormBytes - placements.formBytesRead) return false;
	placements.formsRead += 1;
	placements.formBytesRead += bytes;
	return true;

}

void on_draw_object(CGPDFScannerRef scanner, void *info) {

	ImagePlacements &placements = *static_cast<ImagePlacements*>(info);
	const char *name = nullptr;
	if (CGPDFScannerPopName(scanner, &name) == false || name == nullptr) return;
	CGPDFContentStreamRef contentStream = CGPDFScannerGetContentStream(scanner);
	CGPDFObjectRef object = CGPDFContentStreamGetResource(contentStream, "XObject", name);
	CGPDFStreamRef stream = nullptr;
	if (object == nullptr || CGPDFObjectGetValue(object, kCGPDFObjectTypeStream, &stream) == false || stream == nullptr) return;
	CGPDFDictionaryRef dictionary = CGPDFStreamGetDictionary(stream);
	const char *subtype = nullptr;
	if (dictionary == nullptr || CGPDFDictionaryGetName(dictionary, "Subtype", &subtype) == false || subtype == nullptr) return;

	if (std::strcmp(subtype, "Image") == 0) {
		place_image(placements);
		return;
	}
	if (std::strcmp(subtype, "Form") != 0 || may_read_form(placements, stream, dictionary) == false) return;

	/* - - - - - - */

	CGAffineTransform matrix = CGAffineTransformIdentity;
	CGPDFArrayRef matrixValues = nullptr;
	if (CGPDFDictionaryGetArray(dictionary, "Matrix", &matrixValues) == true && CGPDFArrayGetCount(matrixValues) == 6) {
		CGPDFReal values[6];
		bool isComplete = true;
		for (size_t index = 0; index < 6; ++index) {
			if (CGPDFArrayGetNumber(matrixValues, index, &values[index]) == false) isComplete = false;
		}
		if (isComplete == true) matrix = CGAffineTransformMake(values[0], values[1], values[2], values[3], values[4], values[5]);
	}
	CGPDFDictionaryRef resources = nullptr;
	if (CGPDFDictionaryGetDictionary(dictionary, "Resources", &resources) == false) resources = nullptr;

	CGPDFContentStreamRef formStream = CGPDFContentStreamCreateWithStream(stream, resources, contentStream);
	if (formStream == nullptr) return;
	const CGAffineTransform outer = placements.ctm;
	const std::size_t outerSaved = placements.saved.size();
	placements.ctm = CGAffineTransformConcat(matrix, outer);
	placements.formsOpen.push_back(stream);
	CGPDFScannerRef formScanner = CGPDFScannerCreate(formStream, placements.operators, &placements);
	if (formScanner != nullptr) {
		CGPDFScannerScan(formScanner);
		CGPDFScannerRelease(formScanner);
	}
	CGPDFContentStreamRelease(formStream);
	placements.formsOpen.pop_back();
	placements.saved.resize(std::min(placements.saved.size(), outerSaved));
	placements.ctm = outer;

}

std::vector<CGRect> image_placements(CGPDFPageRef pageRef) {

	ImagePlacements placements;
	placements.operators = CGPDFOperatorTableCreate();
	if (placements.operators == nullptr) return {};
	CGPDFOperatorTableSetCallback(placements.operators, "q", on_save);
	CGPDFOperatorTableSetCallback(placements.operators, "Q", on_restore);
	CGPDFOperatorTableSetCallback(placements.operators, "cm", on_transform);
	CGPDFOperatorTableSetCallback(placements.operators, "Do", on_draw_object);
	// Core Graphics hands an inline image (BI ... ID ... EI) to its closing EI.
	CGPDFOperatorTableSetCallback(placements.operators, "EI", on_inline_image);

	CGPDFContentStreamRef contentStream = CGPDFContentStreamCreateWithPage(pageRef);
	if (contentStream != nullptr) {
		CGPDFScannerRef scanner = CGPDFScannerCreate(contentStream, placements.operators, &placements);
		if (scanner != nullptr) {
			CGPDFScannerScan(scanner);
			CGPDFScannerRelease(scanner);
		}
		CGPDFContentStreamRelease(contentStream);
	}
	CGPDFOperatorTableRelease(placements.operators);
	return placements.images;

}

// Whether raster images could cover a good part of the page at all: the sum of
// their areas on the page, which is never less than the part they cover. A page
// that fails this holds no scan, and is not drawn to find out.
bool could_hold_a_picture(CGPDFPageRef pageRef, const std::vector<CGRect> &images) {

	const CGRect page = CGPDFPageGetBoxRect(pageRef, kCGPDFCropBox);
	const double pageArea = static_cast<double>(page.size.width) * static_cast<double>(page.size.height);
	if (std::isfinite(pageArea) == false || pageArea <= 0.0) return false;
	double covered = 0.0;
	for (const CGRect &image : images) {
		const CGRect onPage = CGRectIntersection(CGRectStandardize(image), page);
		if (CGRectIsNull(onPage) == true) continue;
		const double area = static_cast<double>(onPage.size.width) * static_cast<double>(onPage.size.height);
		if (std::isfinite(area) == true) covered += area;
	}
	return covered / pageArea >= kPictureShareOfPage;

}

// A page with text of its own is a picture Singularity did not read when a raster
// image fills a good part of it and what the image shows beyond that text
// outweighs the text many times over: a scanned signature page under its Bates
// stamp, a scanned page under a production legend ("CONFIDENTIAL - SUBJECT TO
// PROTECTIVE ORDER ABC000001"), a scan under a "Schedule 1" heading. The ink of
// the text PDFKit read is never counted as unread, so a cover title or an
// "EXHIBIT A" divider page, read in full, is not named as unread; neither is a
// logo beside it, a rule drawn under it, the blank paper it is set on, nor a
// scan whose text layer PDFKit read (its signatures and stamps are a little
// ink beside the text read over them).
bool is_picture_beyond_its_text(PDFPage *page, const std::vector<CGRect> &images, const PageRaster &raster) {

	std::vector<unsigned char> picture(raster.pixels.size(), 0);
	for (const CGRect &image : images) mark_on_raster(raster, image, 0.0, picture);

	// The text PDFKit read, line by line: a line's bounds hold its glyphs, and
	// a page is only a few dozen lines however many characters it has.
	std::vector<unsigned char> readText(raster.pixels.size(), 0);
	const NSUInteger characterCount = static_cast<NSUInteger>(std::max<NSInteger>(0, page.numberOfCharacters));
	if (characterCount > 0) {
		PDFSelection *all = [page selectionForRange:NSMakeRange(0, characterCount)];
		for (PDFSelection *line in all.selectionsByLine) {
			if (meaningful_character_count(line.string) == 0) continue;
			mark_on_raster(raster, NSRectToCGRect([line boundsForPage:page]), 1.5, readText);
		}
	}

	return pdf::picture_outweighs_its_text(raster.pixels, raster.width, raster.height, picture, readText);

}

// Whether a page shows content that was not read. With no text at all, any
// material ink is such content (a scan, a drawing, a stamp); with text, only a
// picture beyond that text is. A page with text and no raster image large
// enough to be a scan is read, and is never drawn to find out.
bool page_shows_unread_content(PDFPage *page, std::size_t meaningful) {

	if (meaningful == 0) {
		const std::optional<PageRaster> raster = render_page(page);
		return raster.has_value() == true && has_material_ink(*raster);
	}
	CGPDFPageRef pageRef = page.pageRef;
	if (pageRef == nullptr) return false;
	const std::vector<CGRect> images = image_placements(pageRef);
	if (could_hold_a_picture(pageRef, images) == false) return false;
	const std::optional<PageRaster> raster = render_page(page);
	return raster.has_value() == true && is_picture_beyond_its_text(page, images, *raster);

}

} // namespace

std::string pdf_extractor_version() { return current_extractor_version(); }

// Text recognition of a scan (D-114) is Windows' for now; on macOS, Vision is
// the Mac's to bring in, and a scan's pages stay unread and are said to be.
bool pdf_text_recognition_available() { return false; }

namespace {

// A reading with fewer letters and digits than this is no reading: lines of a
// form, a stamp, a signature (the same bound as Windows', PdfExtractorPdfium).
constexpr std::size_t kLeastRecognizedCharacters = 20;
// The longest edge a picture is read at: a phone's photograph of a page, 12 or
// 48 megapixels, keeps every word legible at it, and reads in a second or two.
constexpr int kRecognizedLongEdge = 4096;

std::size_t letters_and_digits(const std::string &text) {
	std::size_t count = 0;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		if (byte < 0x80) {
			if ((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')) ++count;
		} else if (byte >= 0xC3 && byte <= 0xDF) {
			// The first byte of a two-byte letter: accented Latin, Greek, Cyrillic.
			++count;
		}
	}
	return count;
}

// The languages a picture is read in, as on Windows: English first, then
// Russian, then the person's own, each once and only where Vision reads it.
NSArray<NSString *> *recognition_languages(VNRecognizeTextRequest *request) {
	NSError *error = nil;
	NSArray<NSString *> *supported = [request supportedRecognitionLanguagesAndReturnError:&error] ?: @[];
	NSMutableArray<NSString *> *wanted = [NSMutableArray arrayWithObjects:@"en-US", @"ru-RU", nil];
	for (NSString *preferred in [NSLocale preferredLanguages]) [wanted addObject:preferred];
	NSMutableArray<NSString *> *languages = [NSMutableArray array];
	for (NSString *language in wanted) {
		NSString *found = nil;
		for (NSString *candidate in supported) {
			if ([candidate caseInsensitiveCompare:language] == NSOrderedSame) { found = candidate; break; }
		}
		if (found == nil) {
			// "fr-CA" is read as Vision's "fr-FR": the language, not the country.
			NSString *prefix = [[language componentsSeparatedByString:@"-"] firstObject];
			for (NSString *candidate in supported) {
				if ([[[candidate componentsSeparatedByString:@"-"] firstObject] caseInsensitiveCompare:prefix] == NSOrderedSame) { found = candidate; break; }
			}
		}
		if (found != nil && [languages containsObject:found] == false) [languages addObject:found];
	}
	return languages;
}

// The lines as a page is read: from the top, and left to right where two
// stand side by side. Vision's boxes count up from the picture's bottom left.
std::string text_in_reading_order(NSArray<VNRecognizedTextObservation *> *observations) {
	struct Found {
		double top = 0;
		double left = 0;
		double height = 0;
		std::string words;
	};
	std::vector<Found> lines;
	for (VNRecognizedTextObservation *observation in observations) {
		VNRecognizedText *best = [[observation topCandidates:1] firstObject];
		if (best == nil || best.string.length == 0) continue;
		const CGRect box = observation.boundingBox;
		lines.push_back(Found{1.0 - CGRectGetMidY(box), CGRectGetMinX(box), CGRectGetHeight(box), std::string(best.string.UTF8String)});
	}
	std::stable_sort(lines.begin(), lines.end(), [](const Found &one, const Found &other) { return one.top < other.top; });
	std::string text;
	std::size_t start = 0;
	while (start < lines.size()) {
		// One row: every line whose middle is within half a line of the first's.
		std::size_t end = start + 1;
		while (end < lines.size() && lines[end].top - lines[start].top < 0.5 * std::max(lines[start].height, 0.001)) ++end;
		std::stable_sort(lines.begin() + static_cast<std::ptrdiff_t>(start), lines.begin() + static_cast<std::ptrdiff_t>(end),
				[](const Found &one, const Found &other) { return one.left < other.left; });
		if (text.empty() == false) text += "\n";
		for (std::size_t index = start; index < end; ++index) {
			if (index > start) text += "  ";
			text += lines[index].words;
		}
		start = end;
	}
	return text;
}

} // namespace

// A picture's words, read on this computer by macOS's own text recognition
// (Vision), as Windows reads them with its own (D-114, D-115): nothing leaves
// the computer and no model is installed for it. The picture is opened by
// ImageIO, so a phone's HEIC photograph reads as a PNG, JPEG or TIFF does, and
// is turned as its file says it is shown.
std::optional<std::string> recognize_image_text(const std::filesystem::path &path) {
	@autoreleasepool {
		NSString *file = [NSString stringWithUTF8String:path.u8string().c_str()];
		if (file == nil) return std::nullopt;
		CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:file], nullptr);
		if (source == nullptr) return std::nullopt;
		NSDictionary *drawing = @{
			(id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
			(id)kCGImageSourceCreateThumbnailWithTransform: @YES,
			(id)kCGImageSourceThumbnailMaxPixelSize: @(kRecognizedLongEdge),
		};
		CGImageRef image = CGImageSourceGetCount(source) > 0
				? CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)drawing)
				: nullptr;
		CFRelease(source);
		if (image == nullptr) return std::nullopt;

		VNRecognizeTextRequest *request = [[VNRecognizeTextRequest alloc] init];
		request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
		request.usesLanguageCorrection = YES;
		NSArray<NSString *> *languages = recognition_languages(request);
		if (languages.count > 0) request.recognitionLanguages = languages;
		if (@available(macOS 13.0, *)) request.automaticallyDetectsLanguage = YES;

		VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
		NSError *error = nil;
		const BOOL isRead = [handler performRequests:@[request] error:&error];
		CGImageRelease(image);
		if (isRead == NO) return std::nullopt;
		std::string text = text_in_reading_order(request.results ?: @[]);
		if (letters_and_digits(text) < kLeastRecognizedCharacters) return std::nullopt;
		return text;
	}
}

namespace {

// A picture written as `type` ("public.png", "public.jpeg"), at `quality` for
// a lossy one.
std::optional<std::string> encoded_picture(CGImageRef image, CFStringRef type, std::optional<double> quality) {
	NSMutableData *data = [NSMutableData data];
	CGImageDestinationRef destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, type, 1, nullptr);
	if (destination == nullptr) return std::nullopt;
	NSDictionary *options = quality.has_value() ? @{(id)kCGImageDestinationLossyCompressionQuality: @(*quality)} : @{};
	CGImageDestinationAddImage(destination, image, (__bridge CFDictionaryRef)options);
	const bool isWritten = CGImageDestinationFinalize(destination);
	CFRelease(destination);
	if (isWritten == false || data.length == 0) return std::nullopt;
	return std::string(static_cast<const char *>(data.bytes), data.length);
}

// The picture on white paper: what shows through its transparency is white,
// as a page shows it, never JPEG's black.
CGImageRef on_white(CGImageRef image) {
	const std::size_t width = CGImageGetWidth(image);
	const std::size_t height = CGImageGetHeight(image);
	CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGContextRef context = CGBitmapContextCreate(nullptr, width, height, 8, 0, space,
		static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast));
	CGColorSpaceRelease(space);
	if (context == nullptr) return nullptr;
	const CGRect whole = CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height));
	CGContextSetRGBFillColor(context, 1, 1, 1, 1);
	CGContextFillRect(context, whole);
	CGContextDrawImage(context, whole, image);
	CGImageRef flattened = CGBitmapContextCreateImage(context);
	CGContextRelease(context);
	return flattened;
}

} // namespace

// A picture as a model reads it (D-256): as it is where every provider takes
// it, otherwise redrawn by ImageIO - a phone's HEIC or HEIF photograph, a TIFF
// or BMP scan, a picture too large or turned by its file - upright and no
// larger than any provider reads.
std::optional<ModelPicture> picture_for_model(const std::filesystem::path &path) {
	if (std::optional<ModelPicture> kept = picture::as_it_is(path)) return kept;
	@autoreleasepool {
		NSString *file = [NSString stringWithUTF8String:path.u8string().c_str()];
		if (file == nil) return std::nullopt;
		CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:file], nullptr);
		if (source == nullptr) return std::nullopt;
		if (CGImageSourceGetCount(source) == 0) {
			CFRelease(source);
			return std::nullopt;
		}
		NSDictionary *properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
		const bool hasAlpha = [properties[(id)kCGImagePropertyHasAlpha] boolValue];
		const double longest = std::max([properties[(id)kCGImagePropertyPixelWidth] doubleValue],
			[properties[(id)kCGImagePropertyPixelHeight] doubleValue]);
		const double edge = longest > 0 ? std::min<double>(picture::kRedrawnLongEdge, longest) : picture::kRedrawnLongEdge;
		NSDictionary *drawing = @{
			(id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
			(id)kCGImageSourceCreateThumbnailWithTransform: @YES,
			(id)kCGImageSourceThumbnailMaxPixelSize: @(edge),
		};
		CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)drawing);
		CFRelease(source);
		if (image == nullptr) return std::nullopt;
		std::optional<ModelPicture> drawn;
		if (hasAlpha) {
			std::optional<std::string> png = encoded_picture(image, CFSTR("public.png"), std::nullopt);
			if (png.has_value() && png->size() <= picture::kMostBytesAsItIs) drawn = ModelPicture{"image/png", std::move(*png)};
		}
		if (drawn.has_value() == false) {
			CGImageRef paper = hasAlpha ? on_white(image) : CGImageRetain(image);
			if (paper != nullptr) {
				std::optional<std::string> jpeg = encoded_picture(paper, CFSTR("public.jpeg"), picture::kRedrawnJpegQuality);
				if (jpeg.has_value() && jpeg->size() <= picture::kMostBytesAsItIs) drawn = ModelPicture{"image/jpeg", std::move(*jpeg)};
				CGImageRelease(paper);
			}
		}
		CGImageRelease(image);
		return drawn;
	}
}

// Work on a PDF (D-115) - an annotated or a redacted copy - is PDFium's on
// every system: PdfExtractorPdfium.cpp writes it on the Mac too.

DocumentExtractionResult
extract_pdf_document(const DocumentExtractionRequest &request) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(request.path, error) || error) {
    return failure(ExtractionStatus::unreadable, "unreadable_file",
                   "The PDF is missing or is not a readable regular file.");
  }
  const auto size = std::filesystem::file_size(request.path, error);
  if (error) {
    return failure(ExtractionStatus::unreadable, "unreadable_file",
                   "The PDF size could not be read.");
  }
  if (size > request.maximum_input_bytes) {
    return failure(ExtractionStatus::unreadable, "input_too_large",
                   "The PDF exceeds the configured local extraction limit.");
  }

  std::lock_guard<std::mutex> lock(pdfkit_mutex());
  @autoreleasepool {
    @try {
      const auto path_bytes = request.path.native();
      NSString *path = [[[NSString alloc]
          initWithBytes:path_bytes.data()
                 length:path_bytes.size()
               encoding:NSUTF8StringEncoding] autorelease];
      if (path == nil) {
        return failure(ExtractionStatus::unreadable, "unreadable_file",
                       "The PDF path cannot be represented locally.");
      }
      NSURL *url = [NSURL fileURLWithPath:path];
      PDFDocument *document =
          [[[PDFDocument alloc] initWithURL:url] autorelease];
      if (document == nil) {
        return pdfkit_failure(ExtractionStatus::unreadable, "malformed_pdf",
                              "PDFKit could not parse this PDF.");
      }
      if (document.isEncrypted || document.isLocked) {
        return pdfkit_failure(
            ExtractionStatus::unreadable, "encrypted_pdf",
            "Encrypted PDFs must be unlocked before local extraction.");
      }

      const NSUInteger page_count = document.pageCount;
      if (page_count == 0) {
        return pdfkit_failure(ExtractionStatus::unreadable, "malformed_pdf",
                              "The PDF contains no readable page tree.");
      }

      DocumentExtractionResult result;
      result.format = DocumentFormat::pdf;
      result.extractor_version = current_extractor_version();
      result.structure.page_count = static_cast<std::size_t>(page_count);

      std::vector<PageReading> pages;
      pages.reserve(static_cast<std::size_t>(page_count));
      for (NSUInteger index = 0; index < page_count; ++index) {
        PDFPage *page = [document pageAtIndex:index];
        if (page == nil) {
          return pdfkit_failure(
              ExtractionStatus::unreadable, "malformed_pdf",
              "PDFKit could not read a declared PDF page.");
        }

        NSString *page_string = page.string;
        PageReading reading;
        reading.meaningful = meaningful_character_count(page_string);
        reading.isImage = page_shows_unread_content(page, reading.meaningful);
        reading.text = normalize_text(page_string);
        pages.push_back(std::move(reading));
      }

      assemble_reading(pages, result);
      return result;
    } @catch (NSException *) {
      return pdfkit_failure(
          ExtractionStatus::unreadable, "malformed_pdf",
          "PDFKit rejected the PDF during local extraction.");
    }
  }
}

} // namespace singularity::documents
