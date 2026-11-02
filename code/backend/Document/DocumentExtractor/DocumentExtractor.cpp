#include "Document/DocumentExtractor/DocumentExtractor.hpp"

#include "Document/CsvExtractor/CsvExtractor.hpp"
#include "Document/DocxExtractor/DocxExtractor.hpp"
#include "Document/PdfExtractor/PdfExtractor.hpp"
#include "Document/PptxExtractor/PptxExtractor.hpp"
#include "Document/XlsxExtractor/XlsxExtractor.hpp"

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace singularity::documents {
namespace {

DocumentExtractionResult result_with_diagnostic(DocumentFormat format,
                                                ExtractionStatus status,
                                                std::string code,
                                                std::string message) {
  DocumentExtractionResult result;
  result.format = format;
  result.status = status;
  result.diagnostics.push_back({std::move(code), std::move(message)});
  return result;
}

bool is_supported_plain_text_format(DocumentFormat format) {
  return format == DocumentFormat::plain_text ||
         format == DocumentFormat::markdown ||
			 format == DocumentFormat::json ||
			 format == DocumentFormat::xml || format == DocumentFormat::html;
}

const char *plain_text_extractor_version(DocumentFormat format) {
	switch (format) {
		case DocumentFormat::markdown: return "markdown/1";
		case DocumentFormat::json: return "json-validated-text/2";
		case DocumentFormat::xml: return "xml-validated-text/2";
		case DocumentFormat::html: return "html-tolerant-text/2";
		default: return "plain-text/1";
	}
}

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool is_valid_utf8(const std::string &value) {
  const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = bytes[index];
    if (first <= 0x7f) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0;
    unsigned char minimum_second = 0x80;
    unsigned char maximum_second = 0xbf;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation_count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation_count = 2;
      if (first == 0xe0) {
        minimum_second = 0xa0;
      } else if (first == 0xed) {
        maximum_second = 0x9f;
      }
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation_count = 3;
      if (first == 0xf0) {
        minimum_second = 0x90;
      } else if (first == 0xf4) {
        maximum_second = 0x8f;
      }
    } else {
      return false;
    }

    if (index + continuation_count >= value.size()) {
      return false;
    }
    if (bytes[index + 1] < minimum_second ||
        bytes[index + 1] > maximum_second) {
      return false;
    }
    for (std::size_t offset = 2; offset <= continuation_count; ++offset) {
      if (bytes[index + offset] < 0x80 || bytes[index + offset] > 0xbf) {
        return false;
      }
    }
    index += continuation_count + 1;
  }
  return true;
}

bool appears_binary(const std::string &value) {
  std::size_t disallowed_controls = 0;
  for (const unsigned char c : value) {
    if (c == 0) {
      return true;
    }
    if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f' &&
        c != '\b') {
      ++disallowed_controls;
    }
  }
  return !value.empty() && disallowed_controls * 100 > value.size();
}

DocumentFormat known_container_format(const std::string &value) {
  if (value.size() >= 5 && value.compare(0, 5, "%PDF-") == 0) {
    return DocumentFormat::pdf;
  }
  if (value.size() >= 4 &&
      static_cast<unsigned char>(value[0]) == 0x50 &&
      static_cast<unsigned char>(value[1]) == 0x4b &&
      static_cast<unsigned char>(value[2]) == 0x03 &&
      static_cast<unsigned char>(value[3]) == 0x04) {
    return DocumentFormat::docx;
  }
  return DocumentFormat::unknown;
}

std::string normalize_text(std::string value) {
  if (value.size() >= 3 &&
      static_cast<unsigned char>(value[0]) == 0xef &&
      static_cast<unsigned char>(value[1]) == 0xbb &&
      static_cast<unsigned char>(value[2]) == 0xbf) {
    value.erase(0, 3);
  }

  std::string normalized;
  normalized.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\r') {
      normalized.push_back('\n');
      if (index + 1 < value.size() && value[index + 1] == '\n') {
        ++index;
      }
    } else {
      normalized.push_back(value[index]);
    }
  }
  return normalized;
}

bool is_blank_line(const std::string &line) {
  return std::all_of(line.begin(), line.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
}

std::string trim_ascii_whitespace(std::string value) {
  const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
  const auto first = std::find_if(value.begin(), value.end(), not_space);
  const auto last = std::find_if(value.rbegin(), value.rend(), not_space).base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

void inspect_structure(DocumentExtractionResult &result) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < result.text.size()) {
    const auto end = result.text.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(result.text.substr(start));
      break;
    }
    lines.push_back(result.text.substr(start, end - start));
    start = end + 1;
  }
  if (!result.text.empty() && result.text.back() == '\n') {
    lines.emplace_back();
  }

  result.structure.line_count = lines.size();
  bool inside_paragraph = false;
  for (const auto &line : lines) {
    const auto blank = is_blank_line(line);
    if (!blank && !inside_paragraph) {
      ++result.structure.paragraph_count;
    }
    inside_paragraph = !blank;
  }

  if (result.format != DocumentFormat::markdown) {
    return;
  }
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const auto &line = lines[index];
    std::size_t marker_count = 0;
    while (marker_count < line.size() && marker_count < 6 &&
           line[marker_count] == '#') {
      ++marker_count;
    }
    if (marker_count > 0 && marker_count < line.size() &&
        std::isspace(static_cast<unsigned char>(line[marker_count])) != 0) {
      const auto title = trim_ascii_whitespace(line.substr(marker_count + 1));
      if (!title.empty()) {
        result.structure.headings.push_back(
            {index + 1, static_cast<unsigned int>(marker_count), title});
      }
    }
  }
}

// ---- An email file (.eml) ---------------------------------------------------------
//
// Read as its text: who wrote to whom, when and about what, the names of the
// files it carries, and its own words - never the attached files' bytes, which
// are documents of their own once saved.

std::string email_trimmed(const std::string &value) {
  const std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string email_unbase64(const std::string &text) {
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char character : text) {
    int value = -1;
    if (character >= 'A' && character <= 'Z') value = character - 'A';
    else if (character >= 'a' && character <= 'z') value = character - 'a' + 26;
    else if (character >= '0' && character <= '9') value = character - '0' + 52;
    else if (character == '+') value = 62;
    else if (character == '/') value = 63;
    else if (character == '=') break;
    if (value < 0) continue;
    buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((buffer >> bits) & 0xFF);
    }
  }
  return out;
}

std::string email_unquoted_printable(const std::string &text) {
  std::string out;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '=' && index + 1 < text.size()) {
      if (text[index + 1] == '\r' || text[index + 1] == '\n') {
        index += text[index + 1] == '\r' && index + 2 < text.size() && text[index + 2] == '\n' ? 2 : 1;
        continue;
      }
      if (index + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[index + 1])) != 0
          && std::isxdigit(static_cast<unsigned char>(text[index + 2])) != 0) {
        out += static_cast<char>(std::stoi(text.substr(index + 1, 2), nullptr, 16));
        index += 2;
        continue;
      }
    }
    out += text[index];
  }
  return out;
}

std::string email_decoded_words(const std::string &text) {
  std::string out;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t start = text.find("=?", at);
    const std::size_t charset = start == std::string::npos ? std::string::npos : text.find('?', start + 2);
    const std::size_t encoding = charset == std::string::npos ? std::string::npos : text.find('?', charset + 1);
    const std::size_t end = encoding == std::string::npos ? std::string::npos : text.find("?=", encoding + 1);
    if (end == std::string::npos) {
      out += text.substr(at);
      break;
    }
    const std::string between = text.substr(at, start - at);
    if (email_trimmed(between).empty() == false || out.empty() == true) out += between;
    const std::string payload = text.substr(encoding + 1, end - encoding - 1);
    if (std::toupper(static_cast<unsigned char>(text[charset + 1])) == 'B') {
      out += email_unbase64(payload);
    } else {
      std::string spaced = payload;
      std::replace(spaced.begin(), spaced.end(), '_', ' ');
      out += email_unquoted_printable(spaced);
    }
    at = end + 2;
  }
  return out;
}

using EmailHeaders = std::vector<std::pair<std::string, std::string>>;

std::pair<EmailHeaders, std::string> email_split(const std::string &text) {
  EmailHeaders headers;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    if (line.empty() == false && line.back() == '\r') line.pop_back();
    at = end + 1;
    if (line.empty() == true) break;
    if ((line[0] == ' ' || line[0] == '\t') && headers.empty() == false) {
      headers.back().second += " " + email_trimmed(line);
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos)
      headers.push_back({lowercase_ascii(email_trimmed(line.substr(0, colon))), email_trimmed(line.substr(colon + 1))});
  }
  return {headers, at < text.size() ? text.substr(at) : std::string()};
}

std::string email_header(const EmailHeaders &headers, const std::string &name) {
  for (const auto &[key, value] : headers) {
    if (key == name) return value;
  }
  return {};
}

std::string email_parameter(const std::string &header, const std::string &name) {
  const std::string lower = lowercase_ascii(header);
  std::size_t at = 0;
  while ((at = lower.find(name, at)) != std::string::npos) {
    const std::size_t equals = at + name.size();
    if ((at > 0 && std::isalnum(static_cast<unsigned char>(lower[at - 1])) != 0) || equals >= lower.size()
        || (lower[equals] != '=' && lower[equals] != '*')) {
      at = equals;
      continue;
    }
    std::size_t start = lower.find('=', equals) + 1;
    if (start < header.size() && header[start] == '"') {
      const std::size_t end = header.find('"', start + 1);
      return email_decoded_words(header.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1));
    }
    const std::size_t end = header.find(';', start);
    std::string value = email_trimmed(header.substr(start, end == std::string::npos ? std::string::npos : end - start));
    const std::size_t quote = value.find("''");
    if (quote != std::string::npos) value = value.substr(quote + 2);
    std::string decoded;
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (value[index] == '%' && index + 2 < value.size() && std::isxdigit(static_cast<unsigned char>(value[index + 1])) != 0
          && std::isxdigit(static_cast<unsigned char>(value[index + 2])) != 0) {
        decoded += static_cast<char>(std::stoi(value.substr(index + 1, 2), nullptr, 16));
        index += 2;
        continue;
      }
      decoded += value[index];
    }
    return email_decoded_words(decoded);
  }
  return {};
}

std::string email_body_of(const EmailHeaders &headers, const std::string &body) {
  const std::string encoding = lowercase_ascii(email_header(headers, "content-transfer-encoding"));
  if (encoding == "base64") return email_unbase64(body);
  if (encoding == "quoted-printable") return email_unquoted_printable(body);
  return body;
}

void email_parts(const std::string &body, const std::string &boundary, int depth, std::string &plain,
                 std::vector<std::string> &attachments) {
  if (depth > 4 || boundary.empty() == true) return;
  const std::string marker = "--" + boundary;
  std::size_t at = body.find(marker);
  while (at != std::string::npos) {
    at += marker.size();
    if (body.compare(at, 2, "--") == 0) break;
    const std::size_t line = body.find('\n', at);
    if (line == std::string::npos) break;
    const std::size_t next = body.find(marker, line);
    const std::string content = body.substr(line + 1, next == std::string::npos ? std::string::npos : next - line - 1);
    const auto [headers, rest] = email_split(content);
    const std::string type = lowercase_ascii(email_header(headers, "content-type"));
    std::string name = email_parameter(email_header(headers, "content-disposition"), "filename");
    if (name.empty() == true) name = email_parameter(email_header(headers, "content-type"), "name");
    if (type.rfind("multipart/", 0) == 0) {
      email_parts(rest, email_parameter(email_header(headers, "content-type"), "boundary"), depth + 1, plain, attachments);
    } else if (name.empty() == false) {
      attachments.push_back(name);
    } else if (plain.empty() == true && (type.empty() == true || type.rfind("text/plain", 0) == 0)) {
      plain = email_body_of(headers, rest);
    }
    at = next;
  }
}

std::string email_message_text(const std::string &raw) {
  const auto [headers, body] = email_split(raw);
  std::string text;
  for (const char *field : {"from", "to", "cc", "date", "subject"}) {
    const std::string value = email_decoded_words(email_header(headers, field));
    if (value.empty() == true) continue;
    std::string label = field;
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    text += label + ": " + value + "\n";
  }
  if (email_trimmed(email_header(headers, "x-unsent")) == "1") text += "(A draft, not sent)\n";
  std::string plain;
  std::vector<std::string> attachments;
  const std::string type = email_header(headers, "content-type");
  if (lowercase_ascii(type).rfind("multipart/", 0) == 0) {
    email_parts(body, email_parameter(type, "boundary"), 0, plain, attachments);
  } else {
    plain = email_body_of(headers, body);
  }
  if (attachments.empty() == false) {
    text += "Attachments: ";
    for (std::size_t index = 0; index < attachments.size(); ++index) text += (index == 0 ? "" : ", ") + attachments[index];
    text += "\n";
  }
  return text + "\n" + plain;
}

// ---- A picture ------------------------------------------------------------------------

bool is_picture_extension(const std::string &extension) {
  return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tif"
      || extension == ".tiff" || extension == ".bmp" || extension == ".gif";
}

// A picture's words, read by this computer's text recognition and said to be
// recognised: a word or a figure may differ from the picture.
DocumentExtractionResult extract_picture(const DocumentExtractionRequest &request) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(request.path, error) || error)
    return result_with_diagnostic(DocumentFormat::unknown, ExtractionStatus::unreadable, "unreadable_file",
        "The picture is missing or is not a readable regular file.");
  const auto size = std::filesystem::file_size(request.path, error);
  if (error || size > request.maximum_input_bytes)
    return result_with_diagnostic(DocumentFormat::unknown, ExtractionStatus::unreadable, "input_too_large",
        "The picture exceeds the configured local extraction limit.");
  const std::optional<std::string> recognized = recognize_image_text(request.path);
  if (!recognized.has_value() || !is_valid_utf8(*recognized))
    return result_with_diagnostic(DocumentFormat::unknown, ExtractionStatus::ocr_required, "picture_not_read",
        "A picture whose words this computer could not read.");
  DocumentExtractionResult result;
  result.format = DocumentFormat::plain_text;
  result.extractor_version = "picture-text-recognition/1";
  result.text = normalize_text(*recognized);
  result.structure.byte_count = result.text.size();
  inspect_structure(result);
  result.diagnostics.push_back({"recognized_text", "The picture's words were read by text recognition; a word or a "
      "figure may differ from the picture."});
  result.status = ExtractionStatus::ready;
  return result;
}

} // namespace

DocumentFormat detect_document_format(const std::filesystem::path &path) {
  const auto extension = lowercase_ascii(path.extension().string());
  if (extension == ".txt") {
    return DocumentFormat::plain_text;
  }
  if (extension == ".md" || extension == ".markdown") {
    return DocumentFormat::markdown;
  }
  if (extension == ".pdf") {
    return DocumentFormat::pdf;
  }
  if (extension == ".docx") {
    return DocumentFormat::docx;
  }
  if (extension == ".pptx") {
    return DocumentFormat::pptx;
  }
  if (extension == ".xlsx") {
    return DocumentFormat::xlsx;
  }
  if (extension == ".csv") {
    return DocumentFormat::csv;
  }
	if (extension == ".tsv") return DocumentFormat::tsv;
	if (extension == ".json") return DocumentFormat::json;
	if (extension == ".xml") return DocumentFormat::xml;
	if (extension == ".html" || extension == ".htm") return DocumentFormat::html;
  return DocumentFormat::unknown;
}

const char *to_string(DocumentFormat format) noexcept {
  switch (format) {
  case DocumentFormat::plain_text:
    return "plain_text";
  case DocumentFormat::markdown:
    return "markdown";
  case DocumentFormat::pdf:
    return "pdf";
  case DocumentFormat::docx:
    return "docx";
  case DocumentFormat::pptx:
    return "pptx";
  case DocumentFormat::xlsx:
    return "xlsx";
  case DocumentFormat::csv:
    return "csv";
	case DocumentFormat::tsv:
		return "tsv";
	case DocumentFormat::json:
		return "json";
	case DocumentFormat::xml:
		return "xml";
	case DocumentFormat::html:
		return "html";
  case DocumentFormat::unknown:
    return "unknown";
  }
  return "unknown";
}

std::optional<DocumentFormat> document_format_from_string(const std::string &value) noexcept {
	if (value == "plain_text" || value == "txt") return DocumentFormat::plain_text;
	if (value == "markdown" || value == "md") return DocumentFormat::markdown;
	if (value == "pdf") return DocumentFormat::pdf;
	if (value == "docx") return DocumentFormat::docx;
	if (value == "pptx") return DocumentFormat::pptx;
	if (value == "xlsx") return DocumentFormat::xlsx;
	if (value == "csv") return DocumentFormat::csv;
	if (value == "tsv") return DocumentFormat::tsv;
	if (value == "json") return DocumentFormat::json;
	if (value == "xml") return DocumentFormat::xml;
	if (value == "html") return DocumentFormat::html;
	if (value == "unknown") return DocumentFormat::unknown;
	return std::nullopt;
}

const char *to_string(ExtractionStatus status) noexcept {
  switch (status) {
  case ExtractionStatus::ready:
    return "ready";
  case ExtractionStatus::empty:
    return "empty";
  case ExtractionStatus::ocr_required:
    return "ocr_required";
  case ExtractionStatus::unsupported:
    return "unsupported";
  case ExtractionStatus::unreadable:
    return "unreadable";
  }
  return "unsupported";
}

std::string local_extractor_version(DocumentFormat format) {
  switch (format) {
  case DocumentFormat::plain_text:
	case DocumentFormat::markdown:
	case DocumentFormat::json:
	case DocumentFormat::xml:
	case DocumentFormat::html:
    return plain_text_extractor_version(format);
  case DocumentFormat::pdf:
    return pdf_extractor_version();
  case DocumentFormat::docx:
    return docx_extractor_version();
  case DocumentFormat::pptx:
    return pptx_extractor_version();
  case DocumentFormat::xlsx:
    return xlsx_extractor_version();
  case DocumentFormat::csv:
    return csv_extractor_version();
	case DocumentFormat::tsv:
		return tsv_extractor_version();
  case DocumentFormat::unknown:
    return {};
  }
  return {};
}

std::optional<DocumentExtractionResult>
extract_earlier_reading(const DocumentExtractionRequest &request, const std::string &extractor_version) {
  if (extractor_version == xlsx_cells_reading_version()) return extract_xlsx_cells_reading(request);
  if (extractor_version == xlsx_workbook_v2_reading_version()) return extract_xlsx_workbook_v2_reading(request);
  return std::nullopt;
}

std::string legacy_reading_version(DocumentFormat format) {
  if (format == DocumentFormat::xlsx) return xlsx_cells_reading_version();
  return local_extractor_version(format);
}

DocumentExtractionResult LocalDocumentExtractor::extract(
    const DocumentExtractionRequest &request) const {
  const auto format = request.format_hint.value_or(
      detect_document_format(request.path));
  if (format == DocumentFormat::pdf) {
    return extract_pdf_document(request);
  }
  if (format == DocumentFormat::docx) {
    return extract_docx_document(request);
  }
  if (format == DocumentFormat::pptx) {
    return extract_pptx_document(request);
  }
  if (format == DocumentFormat::xlsx) {
    return extract_xlsx_document(request);
  }
  if (format == DocumentFormat::csv) {
    return extract_csv_document(request);
  }
	if (format == DocumentFormat::tsv) {
		return extract_tsv_document(request);
	}
	// An email file is read as its text (D-115): headers, the names of what it
	// carries, and its own words; a picture, by text recognition.
	const std::string extension = lowercase_ascii(request.path.extension().string());
	const bool is_email = format == DocumentFormat::unknown && extension == ".eml";
	if (format == DocumentFormat::unknown && is_picture_extension(extension)) return extract_picture(request);
  if (!is_supported_plain_text_format(format) && !is_email) {
    return result_with_diagnostic(
        format, ExtractionStatus::unsupported, "unsupported_format",
        "No local extractor is available for this document format.");
  }

  std::error_code error;
  if (!std::filesystem::is_regular_file(request.path, error) || error) {
    return result_with_diagnostic(
        format, ExtractionStatus::unreadable, "unreadable_file",
        "The document is missing or is not a readable regular file.");
  }

  const auto size = std::filesystem::file_size(request.path, error);
  if (error) {
    return result_with_diagnostic(
        format, ExtractionStatus::unreadable, "unreadable_file",
        "The document size could not be read.");
  }
  if (size > request.maximum_input_bytes) {
    return result_with_diagnostic(
        format, ExtractionStatus::unreadable, "input_too_large",
        "The document exceeds the configured local extraction limit.");
  }

  std::ifstream input(request.path, std::ios::binary);
  if (!input) {
    return result_with_diagnostic(
        format, ExtractionStatus::unreadable, "unreadable_file",
        "The document could not be opened for local extraction.");
  }
  std::string raw((std::istreambuf_iterator<char>(input)),
                  std::istreambuf_iterator<char>());
  if (input.bad()) {
    return result_with_diagnostic(
        format, ExtractionStatus::unreadable, "read_failed",
        "The document could not be read completely.");
  }
  if (is_email) {
    DocumentExtractionResult email;
    email.format = DocumentFormat::plain_text;
    email.extractor_version = "email-message-text/1";
    email.text = normalize_text(email_message_text(raw));
    if (!is_valid_utf8(email.text)) {
      return result_with_diagnostic(format, ExtractionStatus::unreadable, "unsupported_encoding",
          "The email is not written in UTF-8 text Singularity can read.");
    }
    email.structure.byte_count = email.text.size();
    inspect_structure(email);
    email.status = ExtractionStatus::ready;
    return email;
  }
  const auto container_format = known_container_format(raw);
  if (container_format != DocumentFormat::unknown) {
    return result_with_diagnostic(
        container_format, ExtractionStatus::unsupported,
        "format_content_mismatch",
        "The document content is not compatible with plain-text extraction.");
  }
  if (appears_binary(raw) || !is_valid_utf8(raw)) {
    return result_with_diagnostic(
        format, ExtractionStatus::unreadable, "unsupported_encoding",
        "The document is binary or is not valid UTF-8 text.");
  }

  DocumentExtractionResult result;
  result.format = format;
  result.extractor_version = plain_text_extractor_version(format);
  result.text = normalize_text(std::move(raw));
  result.structure.byte_count = result.text.size();
  inspect_structure(result);

  const auto has_non_whitespace =
      std::any_of(result.text.begin(), result.text.end(), [](unsigned char c) {
        return std::isspace(c) == 0;
      });
  if (!has_non_whitespace) {
    result.status = ExtractionStatus::empty;
    result.diagnostics.push_back(
        {"empty_document", "The document contains no extractable text."});
    return result;
  }

	if (format == DocumentFormat::json
			&& nlohmann::json::parse(result.text, nullptr, false).is_discarded()) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back(
			{"json_malformed", "The JSON document is not syntactically valid JSON."});
		return result;
	}
	if (format == DocumentFormat::xml) {
		pugi::xml_document document;
		const pugi::xml_parse_result parsed = document.load_buffer(
			result.text.data(), result.text.size(), pugi::parse_default, pugi::encoding_utf8);
		if (!parsed || !document.document_element()) {
			result.status = ExtractionStatus::unreadable;
			result.diagnostics.push_back(
				{"xml_malformed", "The XML document is not well formed or has no document element."});
			return result;
		}
	}

  result.status = ExtractionStatus::ready;
  return result;
}

} // namespace singularity::documents
