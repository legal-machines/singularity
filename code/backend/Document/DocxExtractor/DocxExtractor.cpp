#include "Document/DocxExtractor/DocxExtractor.hpp"

#include "Document/OfficeTextEdit/OfficeTextEdit.hpp"
#include "Document/Types/DocumentChangeDescription.hpp"
#include "Platform/PhraseMatch/PhraseMatch.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"

#include <pugixml.hpp>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace singularity::documents {
namespace {

// The model's reading: the body with Word's own numbers, its tables, the parts
// around it and every tracked change still pending, read as pending.
constexpr std::string_view kExtractorVersion = "docx-wordprocessingml/3";
// The body alone, as models read it until 25 Sep 2026 (extract_docx_body_reading).
constexpr std::string_view kBodyReadingVersion = "docx-wordprocessingml/1";
// The body alone with every revision accepted (extract_docx_accepted_reading).
constexpr std::string_view kAcceptedReadingVersion = "docx-wordprocessingml-accepted/1";
constexpr zip_int64_t kMaximumArchiveEntries = 4096;
constexpr std::uint64_t kMaximumArchiveUncompressedBytes =
    512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumXmlBytes = 48ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumDocumentXmlBytes = 40ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumStylesXmlBytes = 8ULL * 1024ULL * 1024ULL;

class DocxError final : public std::runtime_error {
public:
  DocxError(std::string error_code, std::string public_message)
      : std::runtime_error(public_message), code(std::move(error_code)) {}

  std::string code;
};

class ZipArchive final {
public:
  explicit ZipArchive(const std::filesystem::path &path) {
    int error_code = 0;
    archive_ = zip_open(path.u8string().c_str(), ZIP_RDONLY, &error_code);
    if (archive_ == nullptr) {
      throw DocxError("docx_invalid_archive",
                      "The DOCX package could not be opened as a ZIP archive.");
    }
  }

  ZipArchive(const ZipArchive &) = delete;
  ZipArchive &operator=(const ZipArchive &) = delete;

  ~ZipArchive() {
    if (archive_ != nullptr) {
      zip_discard(archive_);
    }
  }

  [[nodiscard]] zip_t *get() const noexcept { return archive_; }

private:
  zip_t *archive_ = nullptr;
};

struct ArchiveInventory {
  zip_uint64_t document_index = 0;
  std::optional<zip_uint64_t> styles_index;
  // word/numbering.xml, where the package has one: what a list's own format
  // is (a bullet or a number) is stated there and nowhere else - the numbers
  // a person sees and a model reads are counted from it.
  std::optional<zip_uint64_t> numbering_index;
  std::uint64_t document_size = 0;
  std::uint64_t styles_size = 0;
  std::uint64_t numbering_size = 0;
};

// What a paragraph style says about the text in it, for the reading a person
// looks at. Each mark is three-valued: the style says bold, the style says
// not bold, or the style says nothing and the one it is based on decides.
struct StyleRunFormat {
  int bold = -1;
  int italic = -1;
  int underline = -1;
  // Struck through (once or twice), set in capitals, set in small capitals.
  int strike = -1;
  int caps = -1;
  int small_caps = -1;
  // "superscript", "subscript" or "baseline"; "" where it says nothing.
  std::string vert_align;
  std::string color_hex;
  unsigned int half_points = 0;
};

struct StyleInfo {
  std::string based_on;
  std::optional<unsigned int> outline_level;
  StyleRunFormat run_format;
};

using StyleMap = std::unordered_map<std::string, StyleInfo>;

DocumentExtractionResult failure(std::string code, std::string message,
                                 std::string_view version = kExtractorVersion) {
  DocumentExtractionResult result;
  result.format = DocumentFormat::docx;
  result.extractor_version = std::string(version);
  result.status = ExtractionStatus::unreadable;
  result.diagnostics.push_back({std::move(code), std::move(message)});
  return result;
}

std::string_view local_name(std::string_view qualified_name) {
  const auto separator = qualified_name.find(':');
  return separator == std::string_view::npos
             ? qualified_name
             : qualified_name.substr(separator + 1);
}

bool is_element(const pugi::xml_node &node, std::string_view name) {
  return node.type() == pugi::node_element && local_name(node.name()) == name;
}

pugi::xml_node child_element(const pugi::xml_node &parent,
                             std::string_view name) {
  for (const auto &child : parent.children()) {
    if (is_element(child, name)) {
      return child;
    }
  }
  return {};
}

std::string attribute_value(const pugi::xml_node &node,
                            std::string_view name) {
  for (const auto &attribute : node.attributes()) {
    if (local_name(attribute.name()) == name) {
      return attribute.value();
    }
  }
  return {};
}

std::optional<unsigned int> parse_outline_level(const pugi::xml_node &node) {
  const auto value = attribute_value(node, "val");
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(), [](const unsigned char byte) {
        return std::isdigit(byte) != 0;
      })) {
    return std::nullopt;
  }
  try {
    const auto parsed = std::stoul(value);
    // WordprocessingML uses levels 0..8 for headings. Level 9 denotes body
    // text and must not be presented as a heading.
    if (parsed <= 8) {
      return static_cast<unsigned int>(parsed + 1);
    }
  } catch (const std::exception &) {
  }
  return std::nullopt;
}

ArchiveInventory inspect_archive(zip_t *archive,
                                 std::uint64_t maximum_archive_bytes) {
  const auto entry_count = zip_get_num_entries(archive, 0);
  if (entry_count < 0 || entry_count > kMaximumArchiveEntries) {
    throw DocxError("docx_archive_entry_limit",
                    "The DOCX package contains too many archive entries.");
  }

  ArchiveInventory inventory;
  bool found_document = false;
  bool found_styles = false;
  std::uint64_t total_compressed = 0;
  std::uint64_t total_uncompressed = 0;
  std::uint64_t total_xml = 0;

  for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entry_count);
       ++index) {
    zip_stat_t entry{};
    zip_stat_init(&entry);
    if (zip_stat_index(archive, index, ZIP_FL_ENC_GUESS, &entry) != 0 ||
        (entry.valid & ZIP_STAT_NAME) == 0 || entry.name == nullptr) {
      throw DocxError("docx_invalid_archive_entry",
                      "A DOCX archive entry could not be inspected safely.");
    }
    if ((entry.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 &&
        entry.encryption_method != ZIP_EM_NONE) {
      throw DocxError("docx_encrypted",
                      "Encrypted DOCX packages are not supported.");
    }
    if ((entry.valid & ZIP_STAT_SIZE) == 0 ||
        (entry.valid & ZIP_STAT_COMP_SIZE) == 0) {
      throw DocxError("docx_unknown_entry_size",
                      "A DOCX archive entry has no reliable size metadata.");
    }
    if (entry.comp_size > maximum_archive_bytes -
                              std::min(maximum_archive_bytes, total_compressed)) {
      throw DocxError("docx_compressed_size_limit",
                      "The DOCX package exceeds the compressed-data limit.");
    }
    total_compressed += entry.comp_size;
    if (entry.size > kMaximumArchiveUncompressedBytes -
                         std::min(kMaximumArchiveUncompressedBytes,
                                  total_uncompressed)) {
      throw DocxError("docx_uncompressed_size_limit",
                      "The DOCX package expands beyond the safety limit.");
    }
    total_uncompressed += entry.size;

    const std::string_view name(entry.name);
    if (name == "word/document.xml") {
      if (found_document) {
        throw DocxError("docx_duplicate_document_xml",
                        "The DOCX package contains duplicate document XML.");
      }
      found_document = true;
      inventory.document_index = index;
      inventory.document_size = entry.size;
    } else if (name == "word/styles.xml") {
      if (found_styles) {
        throw DocxError("docx_duplicate_styles_xml",
                        "The DOCX package contains duplicate style XML.");
      }
      found_styles = true;
      inventory.styles_index = index;
      inventory.styles_size = entry.size;
    } else if (name == "word/numbering.xml") {
      // A second numbering part is not a reason to refuse a document that
      // reads perfectly well without lists: the first one is used and the
      // rest ignored, so a list simply reads as an indented paragraph.
      if (!inventory.numbering_index.has_value()
          && entry.size <= kMaximumStylesXmlBytes) {
        inventory.numbering_index = index;
        inventory.numbering_size = entry.size;
      }
    }

    if (name.size() >= 4 && name.substr(name.size() - 4) == ".xml") {
      if (entry.size > kMaximumXmlBytes - std::min(kMaximumXmlBytes, total_xml)) {
        throw DocxError("docx_xml_size_limit",
                        "The DOCX package contains excessive XML data.");
      }
      total_xml += entry.size;
    }
  }

  if (!found_document) {
    throw DocxError("docx_missing_document_xml",
                    "The DOCX package does not contain word/document.xml.");
  }
  if (inventory.document_size > kMaximumDocumentXmlBytes ||
      inventory.styles_size > kMaximumStylesXmlBytes) {
    throw DocxError("docx_xml_part_size_limit",
                    "A required DOCX XML part exceeds the safety limit.");
  }
  return inventory;
}

std::string read_entry(zip_t *archive, zip_uint64_t index,
                       std::uint64_t expected_size,
                       std::uint64_t maximum_size) {
  if (expected_size > maximum_size ||
      expected_size > static_cast<std::uint64_t>(
                          std::numeric_limits<std::size_t>::max())) {
    throw DocxError("docx_xml_part_size_limit",
                    "A required DOCX XML part exceeds the read limit.");
  }

  zip_file_t *raw_file = zip_fopen_index(archive, index, ZIP_FL_UNCHANGED);
  if (raw_file == nullptr) {
    throw DocxError("docx_xml_read_failed",
                    "A required DOCX XML part could not be opened.");
  }

  std::string bytes(static_cast<std::size_t>(expected_size), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto read = zip_fread(raw_file, bytes.data() + offset,
                                static_cast<zip_uint64_t>(bytes.size() - offset));
    if (read < 0) {
      zip_fclose(raw_file);
      throw DocxError("docx_xml_read_failed",
                      "A required DOCX XML part could not be read completely.");
    }
    if (read == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read);
  }
  const auto close_result = zip_fclose(raw_file);
  if (close_result != 0 || offset != bytes.size()) {
    throw DocxError("docx_xml_read_failed",
                    "A required DOCX XML part could not be read completely.");
  }
  return bytes;
}

pugi::xml_document parse_xml(const std::string &bytes,
                             const char *diagnostic_code,
                             const char *public_message) {
  if (bytes.find("<!DOCTYPE") != std::string::npos ||
      bytes.find("<!ENTITY") != std::string::npos) {
    throw DocxError("docx_unsafe_xml",
                    "DTD and entity declarations are not accepted in DOCX XML.");
  }
  pugi::xml_document document;
  // pugi::parse_default alone drops any node_pcdata that consists only of
  // whitespace (pugixml's own documented behavior - off by default "usually
  // results in slower parsing", not a safety default). A real Word document
  // very commonly splits a sentence across several <w:r> runs at an
  // arbitrary point, including runs whose entire <w:t> content is a single
  // space; a live design-partner acceptance run against a real client
  // document proved that parsing this DOCX and then serializing it back out
  // (needed even to write a single OTHER cell - see apply_docx_table_cell_
  // patch() below, which round-trips the whole document.xml through this
  // same parser) silently deleted that space, at every such run in the
  // entire document, not only the one cell actually being edited. parse_ws_
  // pcdata keeps those whitespace-only text nodes in the tree so they
  // survive an unrelated edit's parse/serialize round-trip unchanged.
  const auto parsed = document.load_buffer(bytes.data(), bytes.size(),
                                           pugi::parse_default | pugi::parse_ws_pcdata,
                                           pugi::encoding_utf8);
  if (!parsed) {
    throw DocxError(diagnostic_code, public_message);
  }
  return document;
}

// OOXML's on/off: an element that is present with no value, or with "true",
// "1" or "on", is on; "false", "0" and "off" are the ways a document turns
// something a style gave it back off again.
bool on_off_is_on(const pugi::xml_node &node) {
  const std::string value = attribute_value(node, "val");
  return value.empty() || value == "true" || value == "1" || value == "on";
}

// A digit string as OOXML writes a measurement, or 0 where it is not one.
unsigned int parse_measure(const std::string &value) {
  if (value.empty() || !std::all_of(value.begin(), value.end(), [](const unsigned char byte) {
        return std::isdigit(byte) != 0;
      })) {
    return 0;
  }
  try {
    const unsigned long parsed = std::stoul(value);
    return parsed > 100000UL ? 0U : static_cast<unsigned int>(parsed);
  } catch (const std::exception &) {
    return 0;
  }
}

// The marks a <w:rPr> declares, whether it stands on a run or inside a style.
// Silence is kept as silence (-1), because a run that says nothing must be
// allowed to take what its style says rather than a default invented here.
StyleRunFormat read_run_format(const pugi::xml_node &runProperties) {
  StyleRunFormat format;
  if (!runProperties) return format;
  const auto bold = child_element(runProperties, "b");
  if (bold) format.bold = on_off_is_on(bold) ? 1 : 0;
  const auto italic = child_element(runProperties, "i");
  if (italic) format.italic = on_off_is_on(italic) ? 1 : 0;
  const auto underline = child_element(runProperties, "u");
  if (underline) {
    const std::string value = attribute_value(underline, "val");
    format.underline = (value == "none" || value == "0" || value == "false") ? 0 : 1;
  }
  const auto color = child_element(runProperties, "color");
  if (color) {
    const std::string value = attribute_value(color, "val");
    // "auto" means "whatever the reader uses" - a colour Singularity must not
    // turn into a real one.
    if (!value.empty() && value != "auto") format.color_hex = value;
  }
  const auto size = child_element(runProperties, "sz");
  if (size) format.half_points = parse_measure(attribute_value(size, "val"));
  // A double strike is still a strike: a reader shows it struck through.
  const auto strike = child_element(runProperties, "strike");
  const auto doubleStrike = child_element(runProperties, "dstrike");
  if (strike || doubleStrike)
    format.strike = ((strike && on_off_is_on(strike)) || (doubleStrike && on_off_is_on(doubleStrike))) ? 1 : 0;
  const auto caps = child_element(runProperties, "caps");
  if (caps) format.caps = on_off_is_on(caps) ? 1 : 0;
  const auto smallCaps = child_element(runProperties, "smallCaps");
  if (smallCaps) format.small_caps = on_off_is_on(smallCaps) ? 1 : 0;
  const auto vertAlign = child_element(runProperties, "vertAlign");
  if (vertAlign) format.vert_align = attribute_value(vertAlign, "val");
  return format;
}

void collect_styles(const pugi::xml_node &node, StyleMap &styles) {
  if (is_element(node, "style") && attribute_value(node, "type") == "paragraph") {
    const auto style_id = attribute_value(node, "styleId");
    if (!style_id.empty()) {
      StyleInfo info;
      const auto based_on = child_element(node, "basedOn");
      if (based_on) {
        info.based_on = attribute_value(based_on, "val");
      }
      const auto properties = child_element(node, "pPr");
      if (properties) {
        const auto outline = child_element(properties, "outlineLvl");
        if (outline) {
          info.outline_level = parse_outline_level(outline);
        }
      }
      info.run_format = read_run_format(child_element(node, "rPr"));
      styles[style_id] = std::move(info);
    }
  }
  for (const auto &child : node.children()) {
    if (child.type() == pugi::node_element) {
      collect_styles(child, styles);
    }
  }
}

std::optional<unsigned int>
resolve_style_level(const std::string &style_id, const StyleMap &styles) {
  std::unordered_set<std::string> visited;
  auto current = style_id;
  while (!current.empty() && visited.insert(current).second) {
    const auto found = styles.find(current);
    if (found == styles.end()) {
      return std::nullopt;
    }
    if (found->second.outline_level.has_value()) {
      return found->second.outline_level;
    }
    current = found->second.based_on;
  }
  return std::nullopt;
}

// Which reading of the body: as it reads with every tracked change accepted,
// or the reading models were given until 25 Sep 2026, kept exactly as it was
// so a passage cited from it is found again.
enum class BodyReading {
  Accepted,
  Frozen,
};

// The words a reader sees in `node`, every tracked change accepted. Where Word
// keeps the same content twice, only one copy is read, as the writer reads it
// (OfficeTextEdit): a drawing, not the older picture it falls back to
// (mc:Fallback), and a moved passage at the place it was moved to, not also at
// the place it left (w:moveFrom). The frozen reading reads both.
void append_visible_text(const pugi::xml_node &node, std::string &output,
                         BodyReading reading = BodyReading::Accepted) {
  const auto name = local_name(node.name());
  if (name == "del" || name == "delText") {
    return;
  }
  if ((name == "Fallback" || name == "moveFrom") && reading == BodyReading::Accepted) {
    return;
  }
  if (name == "t") {
    output += node.child_value();
    return;
  }
  if (name == "tab") {
    output.push_back('\t');
    return;
  }
  if (name == "br" || name == "cr") {
    output.push_back('\n');
    return;
  }
  if (name == "noBreakHyphen") {
    output += "\xE2\x80\x91";
    return;
  }
  if (name == "softHyphen") {
    output += "\xC2\xAD";
    return;
  }
  for (const auto &child : node.children()) {
    if (child.type() == pugi::node_element) {
      append_visible_text(child, output, reading);
    }
  }
}

std::string trim_ascii(std::string value) {
  const auto non_space = [](const unsigned char byte) {
    return std::isspace(byte) == 0;
  };
  const auto first = std::find_if(value.begin(), value.end(), non_space);
  const auto last = std::find_if(value.rbegin(), value.rend(), non_space).base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

// is_template_placeholder() is now the single canonical definition in
// Document/Types/DocumentSemantics.hpp (a third call site - DirectDocumentWork.cpp -
// needed the exact same rule; see that header's own comment for the full
// rationale/history) - normal unqualified lookup here still finds it via the
// enclosing singularity::documents namespace. Used below to let apply_docx_
// table_cell_patch() safely relax its own mixed-paragraph-styles refusal
// specifically when the CURRENT cell content is already a recognized,
// disposable template blank - never for a cell holding any real content.

std::string single_line(std::string text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool pending_space = false;
  for (const unsigned char byte : text) {
    if (byte == '\r' || byte == '\n') {
      pending_space = !normalized.empty();
    } else {
      if (pending_space && byte != ' ' && byte != '\t') {
        normalized.push_back(' ');
      }
      pending_space = false;
      normalized.push_back(static_cast<char>(byte));
    }
  }
  return trim_ascii(std::move(normalized));
}

std::optional<unsigned int> paragraph_heading_level(const pugi::xml_node &node,
                                                    const StyleMap &styles) {
  const auto properties = child_element(node, "pPr");
  if (!properties) {
    return std::nullopt;
  }
  const auto direct_outline = child_element(properties, "outlineLvl");
  if (direct_outline) {
    return parse_outline_level(direct_outline);
  }
  const auto style = child_element(properties, "pStyle");
  return style ? resolve_style_level(attribute_value(style, "val"), styles)
               : std::nullopt;
}

void collect_cell_text(const pugi::xml_node &node, std::string &output,
                       std::size_t &paragraph_count, BodyReading reading = BodyReading::Accepted) {
  if (is_element(node, "del")) {
    return;
  }
  if (is_element(node, "p")) {
    std::string paragraph;
    append_visible_text(node, paragraph, reading);
    paragraph = single_line(std::move(paragraph));
    if (!paragraph.empty()) {
      if (!output.empty()) {
        output.push_back(' ');
      }
      output += paragraph;
    }
    ++paragraph_count;
    return;
  }
  for (const auto &child : node.children()) {
    if (child.type() == pugi::node_element) {
      collect_cell_text(child, output, paragraph_count, reading);
    }
  }
}

// Office Structure Law: Singularity may know a cell's FORMAL formatting (is its
// text bold, is it shaded, how many grid columns does it span) the same way
// it already knows a cell's row/column position - this is generic OOXML
// structure, never legal meaning. It must never read what the text SAYS to
// decide this. See DocumentCellRole's own comment in DocumentSemantics.hpp
// for the real client document evidence this combination is grounded in.

// Whether a run's own direct formatting marks it bold - <w:b/> (implicitly
// true) or <w:b w:val="..."/> where val is anything OOXML treats as true
// (absent, "true", "1", "on"); an explicit "false"/"0"/"off" is not bold.
// Only ever reads the run's OWN <w:rPr>, never a paragraph/style default -
// real documents commonly apply this kind of label/title emphasis directly
// on the run, exactly as the evidence this heuristic is grounded in does.
bool run_properties_are_bold(const pugi::xml_node &runProperties) {
  const auto bold = child_element(runProperties, "b");
  return bold && on_off_is_on(bold);
}

void collect_bold_evidence(const pugi::xml_node &node, bool &foundText, bool &allBold) {
  if (is_element(node, "del")) return;
  if (is_element(node, "r")) {
    const auto textNode = child_element(node, "t");
    if (textNode && *textNode.child_value() != '\0') {
      foundText = true;
      const auto runProperties = child_element(node, "rPr");
      if (!runProperties || !run_properties_are_bold(runProperties)) allBold = false;
    }
    return;
  }
  for (const auto &child : node.children())
    if (child.type() == pugi::node_element) collect_bold_evidence(child, foundText, allBold);
}

// True only when the cell has at least one visible text run and EVERY such
// run is directly bold. A cell with no text at all (an empty placeholder) is
// deliberately not "bold" by this definition - there is nothing to judge. A
// cell mixing bold and non-bold text (ordinary body prose with one
// emphasized word) is deliberately not "bold" either, so it is never
// mistaken for a genuine label/title cell.
bool cell_visible_text_is_bold(const pugi::xml_node &cellNode) {
  bool foundText = false;
  bool allBold = true;
  collect_bold_evidence(cellNode, foundText, allBold);
  return foundText && allBold;
}

// The real, deliberate shading fill color of this cell, if any - "auto" and
// an absent <w:shd> both mean "no deliberate shading", the common case for
// an ordinary body cell.
std::optional<std::string> cell_shading_fill(const pugi::xml_node &cellNode) {
  const auto tcPr = child_element(cellNode, "tcPr");
  if (!tcPr) return std::nullopt;
  const auto shd = child_element(tcPr, "shd");
  if (!shd) return std::nullopt;
  const std::string fill = attribute_value(shd, "fill");
  if (fill.empty() || fill == "auto") return std::nullopt;
  return fill;
}

// The complete real <w:shd> shading evidence (plain RGB fill and/or a
// theme-based fill+shade, e.g. the design partner's own real "76923C"/"accent3"/"BF"
// Low-severity green) - DirectDocumentWork.cpp's own document-native
// severity presentation reads this from every real cell, never inventing a
// color scheme of its own. All three fields empty means genuinely no
// deliberate shading declared, same as cell_shading_fill()'s own contract.
struct CellShadingEvidence { std::string fill_hex, theme_fill, theme_fill_shade; };
CellShadingEvidence cell_shading_evidence(const pugi::xml_node &cellNode) {
  CellShadingEvidence evidence;
  const auto tcPr = child_element(cellNode, "tcPr");
  if (!tcPr) return evidence;
  const auto shd = child_element(tcPr, "shd");
  if (!shd) return evidence;
  const std::string fill = attribute_value(shd, "fill");
  if (!fill.empty() && fill != "auto") evidence.fill_hex = fill;
  evidence.theme_fill = attribute_value(shd, "themeFill");
  evidence.theme_fill_shade = attribute_value(shd, "themeFillShade");
  return evidence;
}

std::size_t cell_grid_span(const pugi::xml_node &cellNode) {
  const auto tcPr = child_element(cellNode, "tcPr");
  if (!tcPr) return 1;
  const auto span = child_element(tcPr, "gridSpan");
  if (!span) return 1;
  const std::string value = attribute_value(span, "val");
  try {
    const unsigned long parsed = value.empty() ? 1 : std::stoul(value);
    return parsed == 0 ? 1 : static_cast<std::size_t>(parsed);
  } catch (const std::exception &) {
    return 1;
  }
}

// True for a vertical-merge CONTINUATION cell only - <w:vMerge/> with no
// value, or w:val="continue" - never the "restart" cell that begins a new
// merge (that one is a normal, independently real cell whose content simply
// continues downward into any continuation cells beneath it).
bool cell_is_vertical_merge_continuation(const pugi::xml_node &cellNode) {
  const auto tcPr = child_element(cellNode, "tcPr");
  if (!tcPr) return false;
  const auto vMerge = child_element(tcPr, "vMerge");
  if (!vMerge) return false;
  const std::string value = attribute_value(vMerge, "val");
  return value.empty() || value == "continue";
}

// Deep structural equality for two (possibly absent) formatting nodes -
// used only to decide whether every paragraph in a target cell shares the
// SAME paragraph-level formatting (see apply_docx_table_cell_patch()'s own
// "mixed paragraph styles" check below). Two absent nodes are equivalent
// (neither has a pPr - a plain, unstyled paragraph); one absent and one
// present are never equivalent.
bool xml_nodes_equivalent(const pugi::xml_node &a, const pugi::xml_node &b) {
  if (!a && !b) return true;
  if (!a || !b) return false;
  std::ostringstream serializedA, serializedB;
  a.print(serializedA, "", pugi::format_raw);
  b.print(serializedB, "", pugi::format_raw);
  return serializedA.str() == serializedB.str();
}

// A paragraph's own style and numbering, as they are compared: its pPr less
// the paragraph mark's run formatting (rPr) and a recorded change to it
// (pPrChange). A tracked change Singularity wrote marks the paragraph marks it
// merges as deleted there - the paragraph keeps its style, and a later change
// to the same cell must not read it as another style.
std::string paragraph_style_signature(const pugi::xml_node &paragraph) {
  pugi::xml_node properties;
  for (const auto &child : paragraph.children())
    if (is_element(child, "pPr")) properties = child;
  if (!properties) return std::string();
  pugi::xml_document copy;
  pugi::xml_node own = copy.append_copy(properties);
  std::vector<pugi::xml_node> dropped;
  for (const auto &child : own.children())
    if (is_element(child, "rPr") || is_element(child, "pPrChange")) dropped.push_back(child);
  for (const pugi::xml_node &child : dropped) own.remove_child(child);
  if (!own.first_child()) return std::string();
  std::ostringstream serialized;
  own.print(serialized, "", pugi::format_raw);
  return serialized.str();
}

// Formal structural role from generic OOXML formatting evidence alone - see
// this function group's own top comment. Grounded in a real client
// document's own convention: a title/banner cell used flat neutral shading
// with non-bold text on a MERGED (multi-column) cell; a severity/status
// cell used bold text on a saturated shaded fill in a SINGLE-column cell;
// column-label cells used bold text with no shading; ordinary body prose
// used neither. Shading and bold are signals, never a complete rule on
// their own - a shaded cell whose column span does not match either known
// shaded pattern (single-column-and-bold for status/metadata, multi-column-
// and-not-bold for a title/banner) is genuinely ambiguous and resolves to
// Unknown rather than guessing one of the two, since this document's own
// evidence never actually produced that combination. Office Structure Law:
// Unknown is never treated as editable body content (see this file's own
// caller, editable = cell.role == DocumentCellRole::Body) - so widening
// Unknown's reach here only ever narrows what ordinary drafting may touch,
// never broadens it. Bold-without-shading stays HeaderLabel regardless of
// span (a real column label is single-column; a real section-spanning
// label like "…" spans every column - both are
// genuine headers, and both are already non-editable via the same
// Unknown-adjacent rule, so no span gate is needed there). Unshaded and
// non-bold is ordinary body content: this document's own real title/
// header/status cells are never both unshaded AND non-bold, so this
// default never lets a real structural cell slip through as editable.
DocumentCellRole cell_structural_role(const pugi::xml_node &cellNode) {
  const bool bold = cell_visible_text_is_bold(cellNode);
  const bool shaded = cell_shading_fill(cellNode).has_value();
  const std::size_t gridSpan = cell_grid_span(cellNode);
  if (shaded && bold) return gridSpan == 1 ? DocumentCellRole::MetadataOrSeverity : DocumentCellRole::Unknown;
  if (shaded) return gridSpan >= 2 ? DocumentCellRole::TitleOrBanner : DocumentCellRole::Unknown;
  if (bold) return DocumentCellRole::HeaderLabel;
  return DocumentCellRole::Body;
}

struct OutputBuilder {
  std::vector<std::string> lines;
  std::vector<DocumentHeading> headings;
  std::size_t paragraph_count = 0;
  std::size_t next_line = 1;
};

void append_table(const pugi::xml_node &table, OutputBuilder &output, BodyReading reading) {
  // Keep tables as distinct blocks. This makes a table boundary unambiguous
  // in bounded prompt context while every row remains deterministically TSV.
  if (!output.lines.empty() && !output.lines.back().empty()) {
    output.lines.emplace_back();
    ++output.next_line;
  }
  bool emitted_row = false;
  for (const auto &row : table.children()) {
    // A row struck as a revision is gone once it is accepted; the frozen
    // reading still reads it, as it always did.
    if (!is_element(row, "tr") || (reading == BodyReading::Accepted && row_is_deleted_revision(row))) {
      continue;
    }
    std::string rendered_row;
    bool first_cell = true;
    for (const auto &cell : row.children()) {
      if (!is_element(cell, "tc")) {
        continue;
      }
      if (!first_cell) {
        rendered_row.push_back('\t');
      }
      first_cell = false;
      std::string rendered_cell;
      collect_cell_text(cell, rendered_cell, output.paragraph_count, reading);
      rendered_row += rendered_cell;
    }
    output.lines.push_back(std::move(rendered_row));
    ++output.next_line;
    emitted_row = true;
  }
  if (emitted_row) {
    output.lines.emplace_back();
    ++output.next_line;
  }
}

void append_blocks(const pugi::xml_node &node, const StyleMap &styles,
                   OutputBuilder &output, BodyReading reading) {
  if (is_element(node, "del")) {
    return;
  }
  if (reading == BodyReading::Accepted && (is_element(node, "moveFrom") || is_element(node, "Fallback"))) {
    return;
  }
  if (is_element(node, "p")) {
    std::string text;
    append_visible_text(node, text, reading);
    const auto level = paragraph_heading_level(node, styles);
    const auto line = output.next_line;
    const auto rendered_line_count =
        1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    output.lines.push_back(std::move(text));
    output.next_line += rendered_line_count;
    ++output.paragraph_count;
    if (level.has_value()) {
      const auto title = single_line(output.lines.back());
      if (!title.empty()) {
        output.headings.push_back({line, *level, title});
      }
    }
    return;
  }
  if (is_element(node, "tbl")) {
    append_table(node, output, reading);
    return;
  }
  for (const auto &child : node.children()) {
    if (child.type() == pugi::node_element) {
      append_blocks(child, styles, output, reading);
    }
  }
}

std::string join_lines(const std::vector<std::string> &lines) {
  std::string result;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (index != 0) {
      result.push_back('\n');
    }
    result += lines[index];
  }
  return result;
}

bool has_non_whitespace(const std::string &text) {
  return std::any_of(text.begin(), text.end(), [](const unsigned char byte) {
    return std::isspace(byte) == 0;
  });
}

// Every <w:tbl> in document order, nested tables included - the one traversal
// both extract_docx_tables() (read) and apply_docx_table_cell_patch() (write)
// share, so a locator's "table-<n>" ordinal always names the same physical
// table on both sides.
void collect_tbl_nodes(const pugi::xml_node &node, std::vector<pugi::xml_node> &tables) {
  for (const auto &child : node.children()) {
    if (child.type() != pugi::node_element) {
      continue;
    }
    if (is_element(child, "tbl")) {
      tables.push_back(child);
    }
    collect_tbl_nodes(child, tables);
  }
}

// Product-owner correction: whether a table's first row (or any row) is a
// semantic column-header legend, a title/severity banner, or ordinary data
// is legal/document judgment - a live design-partner acceptance run proved a
// real risk register puts a title/severity banner in row 0 and its real
// column labels ("Risk description"/"Recommendations") in row 1, which the
// previous "row 0 is always the header" assumption could never recognize,
// leaving nothing in the whole document editable. column_key is therefore a
// stable, purely positional identity (never derived from any one row's own
// text), and every row - including what used to be silently consumed as
// "the header" and never exposed at all - becomes a real, addressable
// DocumentTableRow. The model reads every row's real text (compile_
// connected_document_turn_data() exposes all of them) and decides for
// itself which row is a legend and which column position holds what, the
// same way a lawyer reading the table would - Singularity never encodes that
// judgment as a row-index rule. apply_docx_table_cell_patch() below mirrors
// this exact positional scheme for the write path.
DocumentTable build_document_table_from_node(const pugi::xml_node &tableNode, const std::string &tableId) {
  DocumentTable table;
  table.table_id = tableId;

  std::vector<pugi::xml_node> rowNodes;
  for (const auto &row : tableNode.children()) {
    if (is_element(row, "tr") && !row_is_deleted_revision(row)) {
      rowNodes.push_back(row);
    }
  }
  if (rowNodes.empty()) {
    return table;
  }

  // A vertical-merge CONTINUATION cell (see cell_is_vertical_merge_
  // continuation()'s own comment) is never a cell of its row: its real
  // content lives on the anchor cell above it in the same grid column, so it
  // must never become its own independently editable field. It still takes
  // its column, so the cells after it keep theirs, and every cell gets a
  // clean, purely positional column_key (see build_document_table_from_
  // node()'s own long-standing comment on why that is never derived from
  // any one row's own text).
  //
  // Each cell keeps the position it stands at among its row's cells, a
  // continuation counted in its place - so the value under "Status" is the
  // value read as Status in a row continuing a merged cell too. A cell merged
  // across columns is one position, as it is one cell (see the comment above).
  struct PlacedCell {
    std::size_t column = 0;
    pugi::xml_node node;
  };
  std::vector<std::vector<PlacedCell>> rowCellNodes;
  std::size_t maxColumns = 0;
  for (const auto &rowNode : rowNodes) {
    std::vector<PlacedCell> cells;
    std::size_t column = 0;
    for (const auto &cell : rowNode.children()) {
      if (!is_element(cell, "tc")) continue;
      if (!cell_is_vertical_merge_continuation(cell)) cells.push_back(PlacedCell{column, cell});
      ++column;
    }
    maxColumns = std::max(maxColumns, column);
    rowCellNodes.push_back(std::move(cells));
  }
  for (std::size_t column = 0; column < maxColumns; ++column) {
    table.column_headers.push_back("column-" + std::to_string(column + 1));
  }

  // What the table's own first row calls each column, at each cell's own
  // position. Read as text only - no judgment about whether the row IS a
  // header; a caller naming a label that the first row happens not to be
  // still resolves by position.
  for (const PlacedCell &cell : rowCellNodes.front()) {
    std::string label;
    std::size_t paragraphCount = 0;
    collect_cell_text(cell.node, label, paragraphCount);
    if (table.column_labels.size() < cell.column) table.column_labels.resize(cell.column);
    table.column_labels.push_back(trim_ascii(single_line(label)));
  }

  std::unordered_set<std::string> usedKeys;
  for (std::size_t rowIndex = 0; rowIndex < rowNodes.size(); ++rowIndex) {
    DocumentTableRow row;
    row.row_index = rowIndex;
    std::vector<std::string> cellTexts;
    for (const PlacedCell &cell : rowCellNodes[rowIndex]) {
      std::string cellText;
      std::size_t paragraphCount = 0;
      collect_cell_text(cell.node, cellText, paragraphCount);
      cellTexts.push_back(std::move(cellText));
    }
    const std::string firstCell = cellTexts.empty() ? std::string() : cellTexts.front();
    row.row_key = derive_table_row_key(firstCell, rowIndex, usedKeys);
    for (std::size_t index = 0; index < cellTexts.size(); ++index) {
      const pugi::xml_node &cellNode = rowCellNodes[rowIndex][index].node;
      const std::size_t column = rowCellNodes[rowIndex][index].column;
      DocumentTableCell cell;
      cell.column_index = column;
      cell.column_key = table.column_headers[column];
      cell.text = cellTexts[index];
      cell.grid_span = cell_grid_span(cellNode);
      cell.role = cell_structural_role(cellNode);
      const CellShadingEvidence shading = cell_shading_evidence(cellNode);
      cell.shading_fill_hex = shading.fill_hex;
      cell.shading_theme_fill = shading.theme_fill;
      cell.shading_theme_fill_shade = shading.theme_fill_shade;
      row.cells.push_back(std::move(cell));
    }
    table.rows.push_back(std::move(row));
  }
  return table;
}

// Opens an EXISTING archive read-write (libzip's default zip_open() mode,
// unlike ZipArchive above which always forces ZIP_RDONLY). commit() must be
// called to actually persist changes via zip_close(); the destructor
// zip_discard()s an archive that was never committed, so a thrown exception
// between open and commit never corrupts the temporary package it is
// operating on.
class WritableZipArchive final {
public:
  explicit WritableZipArchive(const std::filesystem::path &path) {
    int errorCode = 0;
    archive_ = zip_open(path.u8string().c_str(), 0, &errorCode);
    if (archive_ == nullptr) {
      throw DocxError("docx_invalid_archive", "The DOCX package could not be reopened for mutation.");
    }
  }
  WritableZipArchive(const WritableZipArchive &) = delete;
  WritableZipArchive &operator=(const WritableZipArchive &) = delete;
  ~WritableZipArchive() {
    if (archive_ != nullptr && committed_ == false) {
      zip_discard(archive_);
    }
  }

  [[nodiscard]] zip_t *get() const noexcept { return archive_; }

  void commit() {
    if (zip_close(archive_) != 0) {
      throw DocxError("docx_write_failed", "The mutated DOCX package could not be finalized.");
    }
    committed_ = true;
  }

private:
  zip_t *archive_ = nullptr;
  bool committed_ = false;
};

} // namespace

const char *docx_extractor_version() noexcept { return kExtractorVersion.data(); }

const char *docx_body_reading_version() noexcept { return kBodyReadingVersion.data(); }

namespace {

// The body alone, paragraph by paragraph and table by table, read as `reading`
// says.
DocumentExtractionResult read_docx_body(const DocumentExtractionRequest &request, BodyReading reading,
                                        std::string_view version) {
  try {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(request.path, filesystem_error) ||
        filesystem_error) {
      return failure("unreadable_file",
                     "The DOCX document is missing or is not a regular file.", version);
    }
    const auto archive_size =
        std::filesystem::file_size(request.path, filesystem_error);
    if (filesystem_error) {
      return failure("unreadable_file",
                     "The DOCX document size could not be read.", version);
    }
    if (archive_size > request.maximum_input_bytes) {
      return failure("input_too_large",
                     "The DOCX document exceeds the configured extraction limit.", version);
    }

    ZipArchive archive(request.path);
    const auto inventory = inspect_archive(archive.get(),
                                           request.maximum_input_bytes);
    const auto document_bytes =
        read_entry(archive.get(), inventory.document_index,
                   inventory.document_size, kMaximumDocumentXmlBytes);
    auto document = parse_xml(document_bytes, "docx_malformed_document_xml",
                              "word/document.xml is malformed.");

    StyleMap styles;
    bool styles_missing = !inventory.styles_index.has_value();
    if (inventory.styles_index.has_value()) {
      const auto styles_bytes =
          read_entry(archive.get(), *inventory.styles_index,
                     inventory.styles_size, kMaximumStylesXmlBytes);
      auto styles_document = parse_xml(styles_bytes, "docx_malformed_styles_xml",
                                       "word/styles.xml is malformed.");
      collect_styles(styles_document, styles);
    }

    pugi::xml_node body;
    for (const auto &child : document.children()) {
      if (is_element(child, "document")) {
        body = child_element(child, "body");
        break;
      }
    }
    if (!body) {
      return failure("docx_missing_body",
                     "word/document.xml has no WordprocessingML body.", version);
    }

    OutputBuilder output;
    append_blocks(body, styles, output, reading);

    DocumentExtractionResult result;
    result.format = DocumentFormat::docx;
    result.extractor_version = std::string(version);
    result.text = join_lines(output.lines);
    result.structure.byte_count = result.text.size();
    result.structure.line_count =
        result.text.empty()
            ? 0
            : 1 + static_cast<std::size_t>(
                      std::count(result.text.begin(), result.text.end(), '\n'));
    result.structure.paragraph_count = output.paragraph_count;
    result.structure.headings = std::move(output.headings);
    if (styles_missing) {
      result.diagnostics.push_back(
          {"docx_styles_missing",
           "The DOCX package has no styles.xml; style-based headings are unavailable."});
    }
    if (!has_non_whitespace(result.text)) {
      result.status = ExtractionStatus::empty;
      result.diagnostics.push_back(
          {"empty_document", "The document contains no extractable text."});
    } else {
      result.status = ExtractionStatus::ready;
    }
    return result;
  } catch (const DocxError &error) {
    return failure(error.code, error.what(), version);
  } catch (const std::exception &) {
    return failure("docx_extraction_failed",
                   "The DOCX document could not be extracted safely.", version);
  }
}

} // namespace

DocumentExtractionResult
extract_docx_body_reading(const DocumentExtractionRequest &request) {
  return read_docx_body(request, BodyReading::Frozen, kBodyReadingVersion);
}

DocumentExtractionResult
extract_docx_accepted_reading(const DocumentExtractionRequest &request) {
  return read_docx_body(request, BodyReading::Accepted, kAcceptedReadingVersion);
}

std::vector<DocumentParagraph> extract_docx_paragraphs(const DocumentExtractionRequest &request) {
  try {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(request.path, filesystem_error) || filesystem_error) return {};
    ZipArchive archive(request.path);
    const auto inventory = inspect_archive(archive.get(), request.maximum_input_bytes);
    const auto document_bytes = read_entry(archive.get(), inventory.document_index,
                                           inventory.document_size, kMaximumDocumentXmlBytes);
    auto document = parse_xml(document_bytes, "docx_malformed_document_xml",
                              "word/document.xml is malformed.");
    StyleMap styles;
    if (inventory.styles_index.has_value()) {
      const auto styles_bytes = read_entry(archive.get(), *inventory.styles_index,
                                           inventory.styles_size, kMaximumStylesXmlBytes);
      auto styles_document = parse_xml(styles_bytes, "docx_malformed_styles_xml", "word/styles.xml is malformed.");
      collect_styles(styles_document, styles);
    }

    pugi::xml_node body;
    for (const auto &child : document.children()) {
      if (is_element(child, "document")) { body = child_element(child, "body"); break; }
    }
    if (!body) return {};

    std::vector<DocumentParagraph> paragraphs;
    std::string section = "Body";
    // Only the body's own paragraphs: a paragraph inside a table cell belongs
    // to the cell, and is addressed as one.
    for (const auto &child : body.children()) {
      if (!is_element(child, "p")) continue;
      std::string text;
      append_visible_text(child, text);
      text = trim_ascii(single_line(std::move(text)));
      const std::optional<unsigned int> level = paragraph_heading_level(child, styles);
      DocumentParagraph paragraph;
      paragraph.heading_level = level;
      paragraph.text = text;
      paragraph.semantic_path = section;
      if (level.has_value() && !text.empty()) section = text;
      paragraphs.push_back(std::move(paragraph));
    }
    return paragraphs;
  } catch (const std::exception &) {
    return {};
  }
}

std::vector<DocumentTable> extract_docx_tables(const DocumentExtractionRequest &request) {
  try {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(request.path, filesystem_error) || filesystem_error) {
      return {};
    }
    ZipArchive archive(request.path);
    const auto inventory = inspect_archive(archive.get(), request.maximum_input_bytes);
    const auto document_bytes = read_entry(archive.get(), inventory.document_index,
                                           inventory.document_size, kMaximumDocumentXmlBytes);
    auto document = parse_xml(document_bytes, "docx_malformed_document_xml",
                              "word/document.xml is malformed.");

    pugi::xml_node body;
    for (const auto &child : document.children()) {
      if (is_element(child, "document")) {
        body = child_element(child, "body");
        break;
      }
    }
    if (!body) {
      return {};
    }

    std::vector<pugi::xml_node> tableNodes;
    collect_tbl_nodes(body, tableNodes);
    std::vector<DocumentTable> tables;
    for (std::size_t index = 0; index < tableNodes.size(); ++index) {
      tables.push_back(build_document_table_from_node(tableNodes[index], "table-" + std::to_string(index)));
    }
    return tables;
  } catch (const std::exception &) {
    return {};
  }
}

// Everything that happens after a mutation is decided and before the lawyer's
// file changes: a staged copy of the package is written, reopened and read
// back structurally, checked against what was approved, and only then put in
// place atomically. A failure at any step leaves the original file untouched.
//
// The package's own properties say who changed it last and when - the lawyer,
// at `tracked.when` - and nothing else about them changes: the document's
// author, title and creation stay the firm's. A package that has no
// properties part is not given one.
bool commit_mutated_docx(const std::filesystem::path &documentPath, const std::string &newDocumentXml,
    const std::function<bool(const std::filesystem::path &)> &verify_staged, const TrackedChange &tracked,
    DocxPatchApplyResult &result) {
  const std::filesystem::path temporaryPath = platform::staged_sibling_path(documentPath);
  try {
    // Its bytes only, in a file Singularity may write (never copy_file, which
    // gives a read-only document a read-only stage); a copy that fails leaves
    // no stage behind.
    platform::stage_copy_of_file(documentPath, temporaryPath);
  } catch (const std::exception &) {
    platform::discard_staged_file(temporaryPath);
    result.blocker = DocumentPatchBlocker::AtomicReplaceFailed;
    result.detail = "A temporary DOCX package could not be created.";
    return false;
  }

  try {
    WritableZipArchive writeArchive(temporaryPath);
    const zip_int64_t documentIndex =
        zip_name_locate(writeArchive.get(), "word/document.xml", ZIP_FL_ENC_GUESS);
    if (documentIndex < 0) {
      throw DocxError("docx_write_failed", "word/document.xml could not be located for mutation.");
    }
    zip_source_t *source =
        zip_source_buffer(writeArchive.get(), newDocumentXml.data(), newDocumentXml.size(), 0);
    if (source == nullptr
        || zip_file_replace(writeArchive.get(), static_cast<zip_uint64_t>(documentIndex), source,
                            ZIP_FL_ENC_UTF_8) != 0) {
      if (source != nullptr) {
        zip_source_free(source);
      }
      throw DocxError("docx_write_failed", "word/document.xml could not be replaced in the DOCX package.");
    }
    // Kept alive until the archive is committed: libzip reads the buffer then.
    std::string coreXml;
    const zip_int64_t coreIndex = zip_name_locate(writeArchive.get(), "docProps/core.xml", ZIP_FL_ENC_GUESS);
    if (coreIndex >= 0) {
      std::string existing;
      zip_stat_t stat;
      zip_stat_init(&stat);
      if (zip_stat_index(writeArchive.get(), static_cast<zip_uint64_t>(coreIndex), 0, &stat) == 0
          && (stat.valid & ZIP_STAT_SIZE) != 0 && stat.size <= kMaximumStylesXmlBytes) {
        zip_file_t *file = zip_fopen_index(writeArchive.get(), static_cast<zip_uint64_t>(coreIndex), 0);
        if (file != nullptr) {
          existing.resize(static_cast<std::size_t>(stat.size));
          const zip_int64_t read = existing.empty() ? 0 : zip_fread(file, &existing[0], stat.size);
          zip_fclose(file);
          if (read != static_cast<zip_int64_t>(stat.size)) existing.clear();
        }
      }
      coreXml = existing.empty() ? std::string() : core_properties_modified(existing, tracked.author, tracked.when);
    }
    if (!coreXml.empty()) {
      zip_source_t *coreSource = zip_source_buffer(writeArchive.get(), coreXml.data(), coreXml.size(), 0);
      if (coreSource == nullptr
          || zip_file_replace(writeArchive.get(), static_cast<zip_uint64_t>(coreIndex), coreSource,
                              ZIP_FL_ENC_UTF_8) != 0) {
        // Who changed a document last is worth saying, never worth refusing
        // the change for: the part is left as it was.
        if (coreSource != nullptr) zip_source_free(coreSource);
      }
    }
    writeArchive.commit();
  } catch (const std::exception &) {
    platform::discard_staged_file(temporaryPath);
    result.blocker = DocumentPatchBlocker::AtomicReplaceFailed;
    result.detail = "The mutated DOCX package could not be written.";
    return false;
  }

  DocumentExtractionRequest revalidationRequest;
  revalidationRequest.path = temporaryPath;
  revalidationRequest.format_hint = DocumentFormat::docx;
  // Whether the package still reads as a Word document at all: its body is
  // enough to say so, and it is what every writer here rewrote.
  const DocumentExtractionResult revalidated = extract_docx_accepted_reading(revalidationRequest);
  if (revalidated.status != ExtractionStatus::ready && revalidated.status != ExtractionStatus::empty) {
    platform::discard_staged_file(temporaryPath);
    result.blocker = DocumentPatchBlocker::OutputValidationFailed;
    result.detail = "The rewritten DOCX package failed structural validation on reopen.";
    return false;
  }
  if (verify_staged && !verify_staged(temporaryPath)) {
    platform::discard_staged_file(temporaryPath);
    result.blocker = DocumentPatchBlocker::OutputValidationFailed;
    result.detail = "The rewritten DOCX package does not hold exactly the approved change.";
    return false;
  }

  if (platform::office_document_is_open(documentPath)) {
    platform::discard_staged_file(temporaryPath);
    result.blocker = DocumentPatchBlocker::AtomicReplaceFailed;
    result.detail = platform::office_document_open_message(documentPath.filename().string()) + " Nothing was written.";
    return false;
  }
  try {
    platform::commit_staged_file(temporaryPath, documentPath, platform::StagedCommitMode::ReplaceExisting);
  } catch (const std::exception &) {
    platform::discard_staged_file(temporaryPath);
    result.blocker = DocumentPatchBlocker::AtomicReplaceFailed;
    result.detail = "The validated DOCX package could not replace the Project file atomically.";
    return false;
  }

  return true;
}

// The package's own XML declaration kept, and the tree written back the way it
// came: a mutation rewrites what it changed, never the whole file's shape.
std::string serialize_document_xml(const pugi::xml_document &document, const std::string &originalBytes) {
  std::string declaration;
  if (originalBytes.rfind("<?xml", 0) == 0) {
    const auto declarationEnd = originalBytes.find("?>");
    if (declarationEnd != std::string::npos) declaration = originalBytes.substr(0, declarationEnd + 2);
  }
  std::ostringstream serialized;
  document.save(serialized, "", pugi::format_raw | pugi::format_no_declaration, pugi::encoding_utf8);
  return declaration + serialized.str();
}

DocxPatchApplyResult apply_docx_table_cell_patch(
    const std::filesystem::path &documentPath, const DocumentPatch &patch,
    const std::function<bool(const std::filesystem::path &)> &verify_staged) {
  DocxPatchApplyResult result;
  try {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(documentPath, filesystem_error) || filesystem_error) {
      result.blocker = DocumentPatchBlocker::LocatorUnresolved;
      result.detail = "The DOCX document could not be re-read for mutation.";
      return result;
    }

    // Read, then closed at once: an open package cannot be replaced on
    // Windows, and the commit below replaces this very file.
    const auto document_bytes = [&] {
      ZipArchive readArchive(documentPath);
      const auto inventory = inspect_archive(readArchive.get(), 64ULL * 1024ULL * 1024ULL);
      return read_entry(readArchive.get(), inventory.document_index, inventory.document_size,
                        kMaximumDocumentXmlBytes);
    }();
    pugi::xml_document document = parse_xml(document_bytes, "docx_malformed_document_xml",
                                            "word/document.xml is malformed.");

    pugi::xml_node body;
    for (const auto &child : document.children()) {
      if (is_element(child, "document")) {
        body = child_element(child, "body");
        break;
      }
    }
    if (!body) {
      result.blocker = DocumentPatchBlocker::LocatorUnresolved;
      result.detail = "word/document.xml has no WordprocessingML body.";
      return result;
    }

    std::vector<pugi::xml_node> tableNodes;
    collect_tbl_nodes(body, tableNodes);

    // Every cell an operation names, resolved and checked before anything is
    // written: the patch is all of its operations or none of them.
    struct ResolvedTarget {
      pugi::xml_node cell_node;
      std::string new_value;
      // Empty (the common case) means this operation's own w:tcPr/w:shd is
      // left exactly as it already was - see DocumentPatchOperation's own
      // comment for where a real, non-empty value comes from.
      std::string new_shading_fill_hex;
      std::string new_shading_theme_fill;
      std::string new_shading_theme_fill_shade;
    };
    std::vector<ResolvedTarget> resolvedTargets;

    for (const DocumentPatchOperation &operation : patch.operations) {
      if (!operation.locator.row_key.has_value() || !operation.locator.column_key.has_value()) {
        result.blocker = DocumentPatchBlocker::LocatorUnresolved;
        result.detail = "A DOCX patch operation is missing its row/column locator.";
        return result;
      }

      std::size_t tableIndex = 0;
      if (operation.locator.table_id.has_value()) {
        const std::string &tableId = *operation.locator.table_id;
        const std::string prefix = "table-";
        if (tableId.rfind(prefix, 0) != 0) {
          result.blocker = DocumentPatchBlocker::LocatorUnresolved;
          result.detail = "The DOCX patch table id is not in the expected form.";
          return result;
        }
        try {
          tableIndex = static_cast<std::size_t>(std::stoul(tableId.substr(prefix.size())));
        } catch (const std::exception &) {
          result.blocker = DocumentPatchBlocker::LocatorUnresolved;
          result.detail = "The DOCX patch table id could not be parsed.";
          return result;
        }
      } else if (tableNodes.size() != 1) {
        result.blocker = DocumentPatchBlocker::LocatorAmbiguous;
        result.detail = "The DOCX patch omitted table_id and the document has more than one table.";
        return result;
      }
      if (tableIndex >= tableNodes.size()) {
        result.blocker = DocumentPatchBlocker::LocatorUnresolved;
        result.detail = "The DOCX patch table id does not resolve to a real table.";
        return result;
      }

      std::vector<pugi::xml_node> rowNodes;
      for (const auto &row : tableNodes[tableIndex].children()) {
        if (is_element(row, "tr") && !row_is_deleted_revision(row)) {
          rowNodes.push_back(row);
        }
      }
      if (rowNodes.empty()) {
        result.blocker = DocumentPatchBlocker::LocatorUnresolved;
        result.detail = "The DOCX patch target table has no rows.";
        return result;
      }

      // Mirrors build_document_table_from_node()'s own purely positional
      // column_key scheme ("column-<1-based position>") - see that
      // function's own comment. column_key is never matched against any
      // row's text here; a locator this old (schema versions before this
      // correction) that still carries real header text simply fails to
      // parse and resolves closed below, exactly like any other unresolved
      // locator.
      const std::string &columnKey = *operation.locator.column_key;
      const std::string columnPrefix = "column-";
      std::optional<std::size_t> columnIndex;
      if (columnKey.rfind(columnPrefix, 0) == 0) {
        try {
          columnIndex = static_cast<std::size_t>(std::stoul(columnKey.substr(columnPrefix.size())) - 1);
        } catch (const std::exception &) {
          columnIndex.reset();
        }
      }
      if (!columnIndex.has_value()) {
        result.blocker = DocumentPatchBlocker::LocatorUnresolved;
        result.detail = "The DOCX patch column could not be resolved.";
        return result;
      }

      std::unordered_set<std::string> usedKeys;
      std::optional<std::size_t> matchedRow;
      for (std::size_t rowIndex = 0; rowIndex < rowNodes.size(); ++rowIndex) {
        // Mirrors build_document_table_from_node()'s own filtering exactly -
        // a vertical-merge continuation cell must never shift the physical
        // position of the cells after it out of step with the column_key
        // values the read path already assigned.
        std::vector<pugi::xml_node> cellNodes;
        for (const auto &cell : rowNodes[rowIndex].children()) {
          if (!is_element(cell, "tc")) continue;
          if (cell_is_vertical_merge_continuation(cell)) continue;
          cellNodes.push_back(cell);
        }
        std::string firstCellText;
        if (!cellNodes.empty()) {
          std::size_t paragraphCount = 0;
          collect_cell_text(cellNodes.front(), firstCellText, paragraphCount);
        }
        const std::string rowKey = derive_table_row_key(firstCellText, rowIndex, usedKeys);
        if (rowKey != *operation.locator.row_key) {
          continue;
        }
        if (matchedRow.has_value()) {
          result.blocker = DocumentPatchBlocker::LocatorAmbiguous;
          result.detail = "More than one DOCX row resolved to the patch row key.";
          return result;
        }
        matchedRow = rowIndex;
      }
      if (!matchedRow.has_value()) {
        result.blocker = DocumentPatchBlocker::LocatorUnresolved;
        result.detail = "The DOCX patch row key could not be resolved.";
        return result;
      }

      std::vector<pugi::xml_node> targetRowCells;
      for (const auto &cell : rowNodes[*matchedRow].children()) {
        if (!is_element(cell, "tc")) continue;
        if (cell_is_vertical_merge_continuation(cell)) continue;
        targetRowCells.push_back(cell);
      }
      if (*columnIndex >= targetRowCells.size()) {
        result.blocker = DocumentPatchBlocker::LocatorUnresolved;
        result.detail = "The DOCX patch column index does not resolve to a real cell in the matched row.";
        return result;
      }
      pugi::xml_node targetCell = targetRowCells[*columnIndex];

      std::string currentText;
      std::size_t paragraphCount = 0;
      collect_cell_text(targetCell, currentText, paragraphCount);
      if (currentText != operation.expected_old_value) {
        result.blocker = DocumentPatchBlocker::OldValueMismatch;
        result.detail = "The DOCX cell's current value no longer matches the patch's expected old value.";
        return result;
      }

      std::vector<pugi::xml_node> paragraphNodes;
      for (const auto &child : targetCell.children()) {
        if (is_element(child, "p")) {
          paragraphNodes.push_back(child);
        }
      }
      if (paragraphNodes.empty()) {
        result.blocker = DocumentPatchBlocker::InvalidNewValue;
        result.detail = "The DOCX target cell has no paragraph to write into.";
        return result;
      }

      // Office Structure Law defense-in-depth: generic multi-paragraph/
      // multi-run body rendering may only ever target a structurally Body
      // cell. ConnectedDocumentAction.cpp already gates this via editable
      // field handles before a DocumentPatch is ever built - this low-level
      // writer never trusts that alone (see cell_structural_role()'s own
      // comment on why Singularity, not the schema, is the real enforcement).
      //
      // TitleOrBanner and MetadataOrSeverity are also allowed here - but
      // ONLY ConnectedDocumentAction.cpp's old editable-field-handle gate
      // (which never exposes either role as a candidate at all - matching
      // its own real product semantics where a title/severity is never a
      // free-text field) and DirectDocumentWork.cpp's own EditSurfaceResolver
      // (Document/DirectDocumentWork/DirectDocumentWork.hpp's own top
      // comment) ever construct an operation targeting one, and only once
      // that resolver already proved the WHOLE professional block is
      // genuinely available (every one of its real body fields already a
      // confirmed placeholder) - a title or severity default value inside an
      // available block is real, but replaceable, template scaffolding, not
      // an existing lawyer's professional conclusion (this session's own
      // real product-correction: a template's default severity is not a
      // structural constraint on what may be written there). The real
      // guarantee against ever touching a POPULATED block's title/severity
      // stays exactly what it already was for every field: the caller must
      // already have read this cell's own real current text and supplied it
      // as expected_old_value below - a stale or wrong assumption about
      // which block is genuinely available fails there, never here.
      const bool roleAllowsWrite = cell_structural_role(targetCell) == DocumentCellRole::Body
        || cell_structural_role(targetCell) == DocumentCellRole::TitleOrBanner
        || cell_structural_role(targetCell) == DocumentCellRole::MetadataOrSeverity;
      if (!roleAllowsWrite) {
        result.blocker = DocumentPatchBlocker::InvalidNewValue;
        result.detail = "The DOCX target cell is not classified as structurally editable body content.";
        return result;
      }

      // Safe ownership requires every paragraph in this cell to share the
      // SAME paragraph-level formatting (style/numbering) as the first -
      // a cell mixing paragraph styles (for example a heading paragraph
      // followed by a differently-styled body paragraph) has no single
      // template Singularity can safely render the model's replacement text
      // into. Fail closed rather than guessing which style should own it -
      // UNLESS the cell's own current content is already a recognized,
      // disposable template placeholder (is_template_placeholder() above):
      // a real live external-API acceptance run proved a genuinely empty
      // risk-register slot commonly uses a numbered/lettered sub-list
      // ("(1) [●].(2) [●]:(a) [●];(b) [●].") whose own numbering LEVELS
      // legitimately differ in pPr per line, purely as template scaffolding
      // - there is no real content to lose, and the mutation logic just
      // below this already always collapses onto the first paragraph and
      // discards every paragraph after it regardless, so honoring that same
      // first-paragraph style here is exactly what happens for the ordinary
      // single-style case, never a new or different code path.
      // Only paragraphs a reader still sees are compared: one whose words a
      // tracked change already struck, mark and all, holds nothing to write
      // into (a template Singularity filled once keeps its struck lines beside the
      // new words until the change is accepted).
      const auto shows_words = [](const pugi::xml_node &paragraph) {
        bool shown = false;
        std::function<void(const pugi::xml_node &)> walk = [&](const pugi::xml_node &node) {
          for (const auto &child : node.children()) {
            if (shown || is_element(child, "del")) continue;
            if (is_element(child, "t") && child.text().as_string()[0] != '\0') shown = true;
            else walk(child);
          }
        };
        walk(paragraph);
        return shown;
      };
      std::optional<std::string> templateParagraphStyle;
      bool mixedParagraphStyles = false;
      for (const auto &paragraph : paragraphNodes) {
        if (!shows_words(paragraph)) continue;
        const std::string style = paragraph_style_signature(paragraph);
        if (!templateParagraphStyle) templateParagraphStyle = style;
        if (style != *templateParagraphStyle) {
          mixedParagraphStyles = true;
          break;
        }
      }
      if (mixedParagraphStyles && !is_template_placeholder(currentText)) {
        result.blocker = DocumentPatchBlocker::InvalidNewValue;
        result.detail = "The DOCX target cell mixes paragraph styles; Singularity will not guess a single body template.";
        return result;
      }

      resolvedTargets.push_back({targetCell, operation.new_value, operation.new_shading_fill_hex,
          operation.new_shading_theme_fill, operation.new_shading_theme_fill_shade});
    }

    // Every operation resolved and matched its expected value - only now is
    // anything written, and written as Word records a change made with Track
    // Changes on: the cell's old words stay in the file, struck, beside the
    // new ones, and a new background is a recorded change to the cell's look.
    // A risk register the firm keeps is never silently overwritten; a partner
    // accepts or rejects each change in Word.
    TrackedChange tracked;
    tracked.author = patch.revision_author.empty() ? std::string("Singularity") : patch.revision_author;
    tracked.when = revision_timestamp_now();
    for (const ResolvedTarget &target : resolvedTargets) {
      replace_cell_text_tracked(target.cell_node, target.new_value, tracked);
      set_cell_shading(target.cell_node, target.new_shading_fill_hex, target.new_shading_theme_fill,
                       target.new_shading_theme_fill_shade, tracked);
    }

    const std::string newDocumentXml = serialize_document_xml(document, document_bytes);

    if (!commit_mutated_docx(documentPath, newDocumentXml, verify_staged, tracked, result)) return result;
    result.ok = true;
    return result;
  } catch (const DocxError &error) {
    result.blocker = DocumentPatchBlocker::LocatorUnresolved;
    result.detail = error.what();
    return result;
  } catch (const std::exception &error) {
    result.blocker = DocumentPatchBlocker::LocatorUnresolved;
    result.detail = error.what();
    return result;
  }
}


namespace {

// ---- Rows and the cells merged across them ---------------------------------
//
// A cell Word merges down several rows (w:vMerge) is one cell standing in each
// of them: its words live in the first row, the "restart", and every row below
// holds a continuation that draws nothing of its own. A cell merged across
// columns (w:gridSpan) stands in several columns of one row. A row added or
// removed has to leave every such cell whole: a continuation with no restart
// above it, or a row of the wrong width, is a table Word draws broken.

enum class VerticalMerge { None, Restart, Continue };

VerticalMerge vertical_merge_of(const pugi::xml_node &cellNode) {
  const auto vMerge = child_element(child_element(cellNode, "tcPr"), "vMerge");
  if (!vMerge) return VerticalMerge::None;
  return attribute_value(vMerge, "val") == "restart" ? VerticalMerge::Restart : VerticalMerge::Continue;
}

// Each grid column of a row with the cell standing in it; an empty node for a
// column the row leaves out at its start (w:gridBefore).
std::vector<pugi::xml_node> cells_by_grid_column(const pugi::xml_node &rowNode) {
  std::vector<pugi::xml_node> columns;
  const auto before = child_element(child_element(rowNode, "trPr"), "gridBefore");
  if (before) {
    try {
      columns.resize(std::stoul(attribute_value(before, "val")));
    } catch (const std::exception &) {
      columns.clear();
    }
  }
  for (const auto &child : rowNode.children()) {
    if (!is_element(child, "tc")) continue;
    for (std::size_t index = cell_grid_span(child); index > 0; --index) columns.push_back(child);
  }
  return columns;
}

VerticalMerge merge_at(const pugi::xml_node &rowNode, std::size_t column) {
  if (!rowNode) return VerticalMerge::None;
  const std::vector<pugi::xml_node> cells = cells_by_grid_column(rowNode);
  return column < cells.size() && cells[column] ? vertical_merge_of(cells[column]) : VerticalMerge::None;
}

// The table's rows as they stand: a row struck as a revision is not one.
std::vector<pugi::xml_node> live_rows(const pugi::xml_node &tableNode) {
  std::vector<pugi::xml_node> rows;
  for (const auto &child : tableNode.children())
    if (is_element(child, "tr") && !row_is_deleted_revision(child)) rows.push_back(child);
  return rows;
}

// How many columns the table's grid has; failing a grid, its widest row.
std::size_t grid_width(const pugi::xml_node &tableNode) {
  std::size_t width = 0;
  for (const auto &column : child_element(tableNode, "tblGrid").children()) if (is_element(column, "gridCol")) ++width;
  if (width > 0) return width;
  for (const pugi::xml_node &row : live_rows(tableNode)) width = std::max(width, cells_by_grid_column(row).size());
  return width;
}

// The rows a row stands for: itself and, where its first cell is merged down,
// every row that cell continues into - the item a lawyer names by that cell.
std::vector<pugi::xml_node> item_rows(const std::vector<pugi::xml_node> &rows, std::size_t index) {
  std::vector<pugi::xml_node> item{rows[index]};
  if (merge_at(rows[index], 0) != VerticalMerge::Restart) return item;
  for (std::size_t next = index + 1; next < rows.size() && merge_at(rows[next], 0) == VerticalMerge::Continue; ++next)
    item.push_back(rows[next]);
  return item;
}

// The first column whose merged cell a new row would cut, put just above
// `below`: a cell `below` continues from a row above it.
std::optional<std::size_t> merge_cut_above(const pugi::xml_node &below, std::size_t width) {
  for (std::size_t column = 0; column < width; ++column)
    if (merge_at(below, column) == VerticalMerge::Continue) return column;
  return std::nullopt;
}

// The first column whose merged cell would lose the row it starts in, were the
// rows between `above` and `below` taken out: a cell `below` continues that
// nothing left above it begins.
std::optional<std::size_t> merge_orphaned_between(const pugi::xml_node &above, const pugi::xml_node &below,
                                                  std::size_t width) {
  for (std::size_t column = 0; column < width; ++column)
    if (merge_at(below, column) == VerticalMerge::Continue && merge_at(above, column) == VerticalMerge::None)
      return column;
  return std::nullopt;
}

// A row to copy for a new one: an ordinary row of the table - one cell to a
// column, nothing merged down or across - the nearest one to `index` below the
// heading, or the heading itself where it is the table's only such row (a
// register with no entry yet). Empty where the table has none.
pugi::xml_node ordinary_row_near(const std::vector<pugi::xml_node> &rows, std::size_t index, std::size_t width) {
  const auto ordinary = [&](const pugi::xml_node &row) {
    std::size_t cells = 0;
    for (const auto &child : row.children()) {
      if (!is_element(child, "tc")) continue;
      if (cell_grid_span(child) != 1 || vertical_merge_of(child) != VerticalMerge::None) return false;
      ++cells;
    }
    return cells == width && cells_by_grid_column(row).size() == width;
  };
  for (std::size_t distance = 0; distance < rows.size(); ++distance) {
    if (index >= distance && index - distance > 0 && ordinary(rows[index - distance])) return rows[index - distance];
    if (index + distance < rows.size() && index + distance > 0 && ordinary(rows[index + distance]))
      return rows[index + distance];
  }
  return !rows.empty() && ordinary(rows.front()) ? rows.front() : pugi::xml_node();
}

// A table's rows as the structural read names them, so a row an operation
// names by its own first cell resolves to the same row here.
std::vector<std::pair<std::string, pugi::xml_node>> table_rows_by_key(const pugi::xml_node &tableNode) {
  std::vector<std::pair<std::string, pugi::xml_node>> rows;
  std::unordered_set<std::string> usedKeys;
  std::size_t rowIndex = 0;
  for (const auto &child : tableNode.children()) {
    // A row deleted as a revision is not a row of the table any more: it is
    // never named, never written beside and never copied.
    if (!is_element(child, "tr") || row_is_deleted_revision(child)) continue;
    // The first cell as the structural read counts it: a cell a vertical merge
    // continues from the row above is not a cell of this row's own.
    std::string firstCell;
    for (const auto &cell : child.children()) {
      if (!is_element(cell, "tc") || cell_is_vertical_merge_continuation(cell)) continue;
      std::size_t paragraphCount = 0;
      collect_cell_text(cell, firstCell, paragraphCount);
      break;
    }
    // Named exactly as extract_docx_tables() names it, spaces and all.
    rows.emplace_back(derive_table_row_key(firstCell, rowIndex, usedKeys), child);
    ++rowIndex;
  }
  return rows;
}

// A row's cells as the structural read positions them ("column-<n>"), each at
// its own position among the row's cells: an empty node where a vertical
// merge continues the cell above, which is not a cell of this row's own but
// keeps its place, so the cells after it keep theirs.
std::vector<pugi::xml_node> positioned_cells(const pugi::xml_node &rowNode) {
  std::vector<pugi::xml_node> cells;
  for (const auto &child : rowNode.children()) {
    if (!is_element(child, "tc")) continue;
    cells.push_back(cell_is_vertical_merge_continuation(child) ? pugi::xml_node() : child);
  }
  return cells;
}

// The text of one cell of a row that is being written: the cell's own first
// paragraph and first run keep their formatting and take the new text; any
// further paragraph or run the copied row carried is emptied, so a new row
// reads as one value per cell. The text is written as the document writes it,
// without the marks a model puts round words ("**High** risk" is written
// "High risk"), as every other cell a change writes.
void write_cell_text(pugi::xml_node cell, const std::string &value) {
  const std::string text = prose_plain_text(value);
  std::vector<pugi::xml_node> paragraphs;
  for (const auto &child : cell.children()) if (is_element(child, "p")) paragraphs.push_back(child);
  if (paragraphs.empty()) paragraphs.push_back(cell.append_child("w:p"));
  for (std::size_t index = 1; index < paragraphs.size(); ++index) cell.remove_child(paragraphs[index]);
  pugi::xml_node paragraph = paragraphs.front();
  std::vector<pugi::xml_node> runs;
  for (const auto &child : paragraph.children()) if (is_element(child, "r")) runs.push_back(child);
  if (runs.empty()) runs.push_back(paragraph.append_child("w:r"));
  for (std::size_t index = 1; index < runs.size(); ++index) paragraph.remove_child(runs[index]);
  pugi::xml_node run = runs.front();
  std::vector<pugi::xml_node> pieces;
  for (const auto &child : run.children()) if (is_element(child, "t") || is_element(child, "br")) pieces.push_back(child);
  for (std::size_t index = 1; index < pieces.size(); ++index) run.remove_child(pieces[index]);
  pugi::xml_node piece = pieces.empty() ? run.append_child("w:t") : pieces.front();
  if (!is_element(piece, "t")) { run.remove_child(piece); piece = run.append_child("w:t"); }
  if (!piece.attribute("xml:space")) piece.append_attribute("xml:space") = "preserve";
  piece.text().set(text.c_str());
}

// Where a column a change names stands in a table, counted as the structural
// read counts: by the label the table's first row gives it, else by its
// positional key ("column-<n>"), which runs to the widest row - so a register
// whose first row is one merged banner still has every column a model was
// shown. `matches` is how many columns the name fits: 0 none, more than 1
// ambiguous.
struct ColumnPosition {
  std::size_t index = 0;
  std::size_t matches = 0;
};

ColumnPosition column_position(const std::vector<std::pair<std::string, pugi::xml_node>> &rows,
                               const std::string &columnKey) {
  ColumnPosition position;
  if (rows.empty()) return position;
  std::size_t index = 0;
  for (const pugi::xml_node &cell : positioned_cells(rows.front().second)) {
    if (!cell) {
      ++index;
      continue;
    }
    std::string text;
    std::size_t paragraphCount = 0;
    collect_cell_text(cell, text, paragraphCount);
    if (trim_ascii(single_line(text)) == columnKey) {
      if (position.matches == 0) position.index = index;
      ++position.matches;
    }
    ++index;
  }
  if (position.matches > 0) return position;
  const std::string positional = "column-";
  if (columnKey.rfind(positional, 0) != 0) return position;
  const std::string digits = columnKey.substr(positional.size());
  if (digits.empty() || digits.size() > 4
      || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
    return position;
  const std::size_t number = static_cast<std::size_t>(std::stoul(digits));
  std::size_t widest = 0;
  for (const auto &row : rows) widest = std::max(widest, positioned_cells(row.second).size());
  if (number >= 1 && number <= widest) {
    position.index = number - 1;
    position.matches = 1;
  }
  return position;
}

// The one cell an operation names: its table, the row whose own first cell says
// what the operation says, and the column the table itself heads that way (or
// its positional key). Empty, with a reason, when that does not resolve to
// exactly one cell.
pugi::xml_node docx_cell_for(pugi::xml_node body, const DocumentPatchOperation &operation,
    DocumentPatchBlocker &blocker, std::string &detail) {
  std::vector<pugi::xml_node> tableNodes;
  collect_tbl_nodes(body, tableNodes);
  std::size_t tableIndex = 0;
  if (operation.locator.table_id.has_value()) {
    const std::string prefix = "table-";
    const std::string &tableId = *operation.locator.table_id;
    if (tableId.rfind(prefix, 0) != 0) {
      blocker = DocumentPatchBlocker::LocatorUnresolved;
      detail = "The table this change names is not in the expected form.";
      return {};
    }
    try {
      tableIndex = static_cast<std::size_t>(std::stoul(tableId.substr(prefix.size())));
    } catch (const std::exception &) {
      blocker = DocumentPatchBlocker::LocatorUnresolved;
      detail = "The table this change names is not in the expected form.";
      return {};
    }
  } else if (tableNodes.size() != 1) {
    blocker = DocumentPatchBlocker::LocatorAmbiguous;
    detail = "The change did not say which table, and the document has more than one.";
    return {};
  }
  if (tableIndex >= tableNodes.size()) {
    blocker = DocumentPatchBlocker::LocatorUnresolved;
    detail = "The table this change names is not in the document.";
    return {};
  }
  const auto rows = table_rows_by_key(tableNodes[tableIndex]);
  const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto &entry) {
    return operation.locator.row_key && entry.first == *operation.locator.row_key;
  });
  if (row == rows.end()) {
    blocker = DocumentPatchBlocker::LocatorUnresolved;
    detail = "The row this change names is not in that table.";
    return {};
  }
  const std::vector<pugi::xml_node> cells = positioned_cells(row->second);
  const ColumnPosition column = operation.locator.column_key
      ? column_position(rows, *operation.locator.column_key) : ColumnPosition{};
  const std::size_t columnIndex = column.index;
  if (column.matches != 1 || columnIndex >= cells.size()) {
    blocker = column.matches > 1 ? DocumentPatchBlocker::LocatorAmbiguous : DocumentPatchBlocker::LocatorUnresolved;
    detail = "The column this change names does not resolve to one cell of that row.";
    return {};
  }
  if (!cells[columnIndex]) {
    blocker = DocumentPatchBlocker::LocatorUnresolved;
    detail = "In that row the column is part of a cell merged with the one above or beside it; "
      "change the merged cell where it begins.";
    return {};
  }
  return cells[columnIndex];
}

} // namespace

DocxPatchApplyResult apply_docx_document_patch(
    const std::filesystem::path &documentPath, const DocumentPatch &patch,
    const std::function<bool(const std::filesystem::path &)> &verify_staged,
    const std::vector<std::vector<std::string>> &equivalent_writings) {
  DocxPatchApplyResult result;
  try {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(documentPath, filesystem_error) || filesystem_error) {
      result.blocker = DocumentPatchBlocker::LocatorUnresolved;
      result.detail = "The DOCX document could not be re-read for mutation.";
      return result;
    }
    // Read, then closed at once: an open package cannot be replaced on
    // Windows, and the commit below replaces this very file.
    std::string document_bytes;
    // Which paragraphs Word numbers itself, from the package's own styles and
    // numbering: a paragraph inserted like one of them is numbered by Word.
    std::string stylesXml;
    std::string numberingXml;
    {
      ZipArchive readArchive(documentPath);
      const auto inventory = inspect_archive(readArchive.get(), 64ULL * 1024ULL * 1024ULL);
      document_bytes = read_entry(readArchive.get(), inventory.document_index,
                                  inventory.document_size, kMaximumDocumentXmlBytes);
      try {
        if (inventory.styles_index.has_value())
          stylesXml = read_entry(readArchive.get(), *inventory.styles_index, inventory.styles_size, kMaximumStylesXmlBytes);
        if (inventory.numbering_index.has_value())
          numberingXml = read_entry(readArchive.get(), *inventory.numbering_index, inventory.numbering_size,
                                    kMaximumStylesXmlBytes);
      } catch (const DocxError &) {
        // Without them, a paragraph's own numbering is still known.
      }
    }
    pugi::xml_document document = parse_xml(document_bytes, "docx_malformed_document_xml",
                                            "word/document.xml is malformed.");
    pugi::xml_node body;
    for (const auto &child : document.children()) {
      if (is_element(child, "document")) { body = child_element(child, "body"); break; }
    }
    if (!body) {
      result.blocker = DocumentPatchBlocker::LocatorUnresolved;
      result.detail = "word/document.xml has no WordprocessingML body.";
      return result;
    }

    // Every change Singularity writes into a document that already existed is a
    // tracked change - its tables' rows and cells included.
    TrackedChange tracked;
    tracked.author = patch.revision_author.empty() ? std::string("Singularity") : patch.revision_author;
    tracked.when = revision_timestamp_now();

    // How the package's paragraphs are numbered and which are headings, read
    // once for every operation.
    TextEditRequest packageReading;
    read_docx_numbering_into(packageReading, stylesXml, numberingXml);

    std::size_t operationNumber = 0;
    // Which change failed, in the words of the change itself: a patch of
    // several is refused as a whole, and a lawyer is entitled to know which
    // one could not be made.
    const auto refuse = [&](DocumentPatchBlocker blocker, const std::string &detail,
                            const DocumentPatchOperation &operation) {
      result.blocker = blocker;
      result.detail = "Change " + std::to_string(operationNumber) + " of "
          + std::to_string(patch.operations.size()) + " (" + to_string(operation.kind) + "): " + detail;
      return result;
    };
    // Every row an operation names, found in its table AS IT STANDS before any
    // of them - as the model was told ("each against the document as it stands
    // before any of them"). Found again after an earlier change, a row named by
    // its place ("row-7", where the first column is Word's own numbering) is
    // the row that moved into that place: the second of two deletions struck
    // the row after the one the lawyer approved.
    std::map<std::size_t, pugi::xml_node> namedRows;
    {
      std::vector<pugi::xml_node> tableNodes;
      collect_tbl_nodes(body, tableNodes);
      std::size_t number = 0;
      for (const DocumentPatchOperation &operation : patch.operations) {
        ++number;
        const bool rowOperation = operation.kind == DocumentPatchOperationKind::DeleteTableRow
            || (operation.kind == DocumentPatchOperationKind::InsertTableRow
                && operation.placement != DocumentPatchPlacement::AtEnd);
        if (!rowOperation || !operation.locator.row_key) continue;
        std::size_t tableIndex = 0;
        if (operation.locator.table_id) {
          const std::string &tableId = *operation.locator.table_id;
          if (tableId.rfind("table-", 0) != 0) continue;
          try {
            tableIndex = static_cast<std::size_t>(std::stoul(tableId.substr(6)));
          } catch (const std::exception &) {
            continue;
          }
        }
        if (tableIndex >= tableNodes.size()) continue;
        for (const auto &[key, row] : table_rows_by_key(tableNodes[tableIndex]))
          if (key == *operation.locator.row_key) namedRows[number] = row;
      }
    }
    for (const DocumentPatchOperation &operation : patch.operations) {
      ++operationNumber;
      switch (operation.kind) {
        case DocumentPatchOperationKind::ReplaceText:
        case DocumentPatchOperationKind::DeleteContent:
        case DocumentPatchOperationKind::SetFormatting:
        case DocumentPatchOperationKind::InsertParagraph: {
          // A formatting change that names a cell is about that cell, whatever
          // it says: "make the priority bold" is not a search for the word.
          if (operation.kind == DocumentPatchOperationKind::SetFormatting
              && operation.locator.row_key && operation.locator.column_key) {
            DocumentPatchOperation asCell = operation;
            asCell.kind = DocumentPatchOperationKind::ReplaceCellText;
            asCell.expected_old_value.clear();
            DocumentPatchBlocker blocker = DocumentPatchBlocker::LocatorUnresolved;
            std::string detail;
            const pugi::xml_node cell = docx_cell_for(body, asCell, blocker, detail);
            if (!cell) return refuse(blocker, detail, operation);
            // Recorded as a formatting revision, like a passage written
            // differently: a partner rejects it in Word.
            apply_formatting_within(cell, operation.formatting, tracked);
            break;
          }
          TextEditRequest edit;
          edit.anchor = operation.expected_old_value;
          edit.text = operation.new_value;
          edit.formatting = operation.formatting;
          // Every change Singularity writes into a document that already existed is
          // a tracked change. The lawyer's own file comes back with a redline
          // in it, which is the only form in which a partner, a client or the
          // other side can be asked to accept work: nothing is altered behind
          // anyone's back, and Word's own accept and reject do the rest.
          edit.tracked = tracked;
          edit.numbered_automatically = packageReading.numbered_automatically;
          edit.list_place = packageReading.list_place;
          edit.is_heading = packageReading.is_heading;
          if (operation.kind == DocumentPatchOperationKind::DeleteContent) edit.kind = TextEditKind::Delete;
          else if (operation.kind == DocumentPatchOperationKind::SetFormatting) edit.kind = TextEditKind::Format;
          else if (operation.kind == DocumentPatchOperationKind::InsertParagraph)
            edit.kind = operation.placement == DocumentPatchPlacement::Before
              ? TextEditKind::InsertParagraphBefore
              : operation.placement == DocumentPatchPlacement::AtEnd
                ? TextEditKind::AppendParagraph : TextEditKind::InsertParagraphAfter;
          else edit.kind = TextEditKind::Replace;
          const TextEditOutcome outcome = apply_wordprocessing_edit(document, edit);
          if (!outcome.ok) {
            return refuse(outcome.matches > 1 ? DocumentPatchBlocker::LocatorAmbiguous
                                              : DocumentPatchBlocker::LocatorUnresolved,
                          outcome.reason, operation);
          }
          break;
        }
        case DocumentPatchOperationKind::InsertTableRow:
        case DocumentPatchOperationKind::DeleteTableRow: {
          std::vector<pugi::xml_node> tableNodes;
          collect_tbl_nodes(body, tableNodes);
          std::size_t tableIndex = 0;
          if (operation.locator.table_id.has_value()) {
            const std::string prefix = "table-";
            const std::string &tableId = *operation.locator.table_id;
            if (tableId.rfind(prefix, 0) != 0) {
              result.blocker = DocumentPatchBlocker::LocatorUnresolved;
              result.detail = "The DOCX patch table id is not in the expected form.";
              return result;
            }
            try {
              tableIndex = static_cast<std::size_t>(std::stoul(tableId.substr(prefix.size())));
            } catch (const std::exception &) {
              result.blocker = DocumentPatchBlocker::LocatorUnresolved;
              result.detail = "The DOCX patch table id is not in the expected form.";
              return result;
            }
          } else if (tableNodes.size() != 1) {
            result.blocker = DocumentPatchBlocker::LocatorAmbiguous;
            result.detail = "The operation omitted the table and the document has more than one.";
            return result;
          }
          if (tableIndex >= tableNodes.size()) {
            result.blocker = DocumentPatchBlocker::LocatorUnresolved;
            result.detail = "The operation's table does not exist in the document.";
            return result;
          }
          pugi::xml_node tableNode = tableNodes[tableIndex];
          const auto rows = table_rows_by_key(tableNode);
          if (rows.empty()) {
            result.blocker = DocumentPatchBlocker::LocatorUnresolved;
            result.detail = "The operation's table has no rows.";
            return result;
          }
          // The row it names, as found before any change of this patch.
          const auto named = namedRows.find(operationNumber);
          const auto byName = [&](const auto &entry) {
            if (named != namedRows.end()) return entry.second == named->second;
            return operation.locator.row_key && entry.first == *operation.locator.row_key;
          };
          if (operation.kind == DocumentPatchOperationKind::DeleteTableRow) {
            const auto found = std::find_if(rows.begin(), rows.end(), byName);
            if (found == rows.end()) {
              result.blocker = DocumentPatchBlocker::LocatorUnresolved;
              result.detail = named != namedRows.end()
                ? "The row this operation removes was already removed by another change of this patch."
                : "The row this operation removes is not in the table.";
              return result;
            }
            if (found == rows.begin()) {
              result.blocker = DocumentPatchBlocker::InvalidNewValue;
              result.detail = "A table's header row is never removed by an operation.";
              return result;
            }
            // The item the row names goes whole: where its first cell is merged
            // down, the rows that cell continues into go with it. A cell of
            // another column merged from it into a row that stays would be left
            // with no row to begin in, so that is refused, never drawn broken.
            const std::vector<pugi::xml_node> live = live_rows(tableNode);
            const std::size_t width = grid_width(tableNode);
            const std::size_t at = static_cast<std::size_t>(
              std::find(live.begin(), live.end(), found->second) - live.begin());
            const std::vector<pugi::xml_node> item = at < live.size()
              ? item_rows(live, at) : std::vector<pugi::xml_node>{found->second};
            const pugi::xml_node above = at > 0 && at < live.size() ? live[at - 1] : pugi::xml_node();
            const std::size_t after = at + item.size();
            const pugi::xml_node below = after < live.size() ? live[after] : pugi::xml_node();
            if (const auto column = merge_orphaned_between(above, below, width)) {
              result.blocker = DocumentPatchBlocker::InvalidNewValue;
              result.detail = "That row begins a cell merged down into the row below it (column "
                + std::to_string(*column + 1) + "), so removing it alone would break the table. "
                "Change that cell first, or remove the rows it spans together.";
              return result;
            }
            // Struck, not removed: each row stays in the file as a deletion the
            // lawyer can reject.
            for (const pugi::xml_node &row : item) track_deleted_row(row, tracked);
            break;
          }
          // insertTableRow: the new row is a copy of an ordinary row of the
          // table, so it is written in the table's own formatting, and then each
          // named column takes its value.
          const auto anchor = operation.placement == DocumentPatchPlacement::AtEnd
            ? std::prev(rows.end())
            : std::find_if(rows.begin(), rows.end(), byName);
          if (anchor == rows.end()) {
            result.blocker = DocumentPatchBlocker::LocatorUnresolved;
            result.detail = "The row this operation writes beside is not in the table.";
            return result;
          }
          // Where it goes: before the named row, or after the whole item the
          // named row stands for - never between an item's merged rows - and
          // never across a cell another column merges down past that place.
          const std::vector<pugi::xml_node> live = live_rows(tableNode);
          const std::size_t width = grid_width(tableNode);
          const std::size_t at = static_cast<std::size_t>(
            std::find(live.begin(), live.end(), anchor->second) - live.begin());
          if (at >= live.size()) {
            result.blocker = DocumentPatchBlocker::LocatorUnresolved;
            result.detail = "The row this operation writes beside is not in the table.";
            return result;
          }
          pugi::xml_node below;
          pugi::xml_node last = live[at];
          if (operation.placement == DocumentPatchPlacement::Before) {
            below = live[at];
          } else {
            const std::vector<pugi::xml_node> item = item_rows(live, at);
            last = item.back();
            const std::size_t after = at + item.size();
            below = after < live.size() ? live[after] : pugi::xml_node();
          }
          if (const auto column = merge_cut_above(below, width)) {
            result.blocker = DocumentPatchBlocker::InvalidNewValue;
            result.detail = "A row there would cut a cell merged down across those rows (column "
              + std::to_string(*column + 1) + "). Add it before or after the merged rows.";
            return result;
          }
          const pugi::xml_node model = ordinary_row_near(live, at, width);
          if (!model) {
            result.blocker = DocumentPatchBlocker::UnsupportedFormatCapability;
            result.detail = "The table has no row of one cell to each column to write a new row like, so no row was added.";
            return result;
          }
          pugi::xml_node created = operation.placement == DocumentPatchPlacement::Before
            ? tableNode.insert_copy_before(model, below)
            : tableNode.insert_copy_after(model, last);
          // The copy is of the row as it reads: a revision the row still carries
          // is not part of the new one. A copy of the heading row - a register
          // that has no entry yet - is an ordinary row, not a second heading.
          // Then it is emptied for the new entry: of the row it copies it keeps
          // the look, never a table, link, bookmark or comment of that row.
          accept_revisions_within(created);
          make_ordinary_row(created);
          empty_row_for_entry(created);
          // A column is named as the structural read names it - by the table's
          // own label for it, or by its position - and found among the new
          // row's cells as that read positions them.
          const std::vector<pugi::xml_node> cells = positioned_cells(created);
          for (const DocumentPatchCellValue &value : operation.cells) {
            const ColumnPosition column = column_position(rows, value.column_key);
            const std::size_t columnIndex = column.index;
            if (column.matches != 1) {
              result.blocker = column.matches > 1 ? DocumentPatchBlocker::LocatorAmbiguous
                                                  : DocumentPatchBlocker::LocatorUnresolved;
              result.detail = "The operation writes a column the table does not resolve: " + value.column_key;
              return result;
            }
            if (columnIndex >= cells.size() || !cells[columnIndex]) {
              result.blocker = DocumentPatchBlocker::LocatorUnresolved;
              result.detail = "The table's row does not have a cell for column " + value.column_key;
              return result;
            }
            write_cell_text(cells[columnIndex], value.value);
            // The row is itself an insertion: how its new words are written is
            // part of it, not a change of its own.
            apply_formatting_within(cells[columnIndex], operation.formatting);
          }
          track_inserted_row(created, tracked);
          break;
        }
        case DocumentPatchOperationKind::ReplaceCellText: {
          // One cell of a table, named by the table, the row's own first cell
          // and the column's own heading. This is how a model changes a value
          // in a register without quoting the whole row back.
          DocumentPatchBlocker blocker = DocumentPatchBlocker::LocatorUnresolved;
          std::string detail;
          const pugi::xml_node cell = docx_cell_for(body, operation, blocker, detail);
          if (!cell) return refuse(blocker, detail, operation);
          std::string current;
          std::size_t paragraphCount = 0;
          collect_cell_text(cell, current, paragraphCount);
          // Words are replaced only by a change that names them: an empty cell
          // may be filled without quoting it, a cell with words never.
          if (!platform::phrases_equal(current, operation.expected_old_value, equivalent_writings))
            return refuse(DocumentPatchBlocker::OldValueMismatch, operation.expected_old_value.empty()
                ? "That cell holds words this change did not name, so nothing was changed."
                : "That cell no longer holds what this change expected, so nothing was changed.", operation);
          replace_cell_text_tracked(cell, operation.new_value, tracked);
          // "Change that cell and make it bold": the change to the words and
          // the change to how they are written are one instruction - the new
          // words are written that way, and words the cell keeps are recorded
          // as written differently.
          apply_formatting_within(cell, operation.formatting, tracked);
          set_cell_shading(cell, operation.new_shading_fill_hex, operation.new_shading_theme_fill,
                           operation.new_shading_theme_fill_shade, tracked);
          break;
        }
      }
    }

    const std::string newDocumentXml = serialize_document_xml(document, document_bytes);
    if (!commit_mutated_docx(documentPath, newDocumentXml, verify_staged, tracked, result)) return result;
    result.ok = true;
    return result;
  } catch (const DocxError &error) {
    result.blocker = DocumentPatchBlocker::LocatorUnresolved;
    result.detail = error.what();
    return result;
  } catch (const std::exception &error) {
    result.blocker = DocumentPatchBlocker::LocatorUnresolved;
    result.detail = error.what();
    return result;
  }
}


// ---------------------------------------------------------------------------
// The reading a lawyer LOOKS at (Document/Types/DocumentRenderTypes.hpp).
//
// Nothing below is ever written back and no locator resolves against it. It
// exists so that a person who is told their document changed can be shown
// their document - with its pages, its headings, its numbering, its tables,
// its notes, its comments and its own tracked changes - instead of a
// transcript of it.
// ---------------------------------------------------------------------------
namespace {

constexpr std::size_t kListLevels = 9;

// ---- Measurements -----------------------------------------------------------

// A whole number as OOXML writes one ("1", "-2"), or nothing where it is not.
std::optional<int> parse_int(const std::string &value) {
  if (value.empty()) return std::nullopt;
  std::size_t index = 0;
  bool negative = false;
  if (value[0] == '-' || value[0] == '+') {
    negative = value[0] == '-';
    index = 1;
  }
  if (index >= value.size()) return std::nullopt;
  long long parsed = 0;
  for (; index < value.size(); ++index) {
    const unsigned char byte = static_cast<unsigned char>(value[index]);
    if (std::isdigit(byte) == 0) return std::nullopt;
    parsed = parsed * 10 + (byte - '0');
    if (parsed > 100000000LL) return std::nullopt;
  }
  return static_cast<int>(negative ? -parsed : parsed);
}

// A length in twips as OOXML writes one: a plain number of twips ("1440",
// "-360"), or - in a document written to the strict schema - a number with
// its unit ("2.54cm", "1in", "72pt"). Read without the machine's locale, so a
// decimal point is always a point. Nothing where it is neither.
std::optional<int> parse_twips(const std::string &value) {
  if (value.empty()) return std::nullopt;
  std::size_t index = 0;
  bool negative = false;
  if (value[0] == '-' || value[0] == '+') {
    negative = value[0] == '-';
    index = 1;
  }
  double number = 0.0;
  double scale = 0.0;
  bool digits = false;
  for (; index < value.size(); ++index) {
    const char character = value[index];
    if (character >= '0' && character <= '9') {
      digits = true;
      if (scale == 0.0) {
        number = number * 10.0 + (character - '0');
      } else {
        number += (character - '0') * scale;
        scale /= 10.0;
      }
      if (number > 1.0e7) return std::nullopt;
      continue;
    }
    if (character == '.' && scale == 0.0) {
      scale = 0.1;
      continue;
    }
    break;
  }
  if (!digits) return std::nullopt;
  const std::string unit = value.substr(index);
  double twips = number;
  if (unit == "in") twips = number * 1440.0;
  else if (unit == "cm") twips = number * 1440.0 / 2.54;
  else if (unit == "mm") twips = number * 1440.0 / 25.4;
  else if (unit == "pt") twips = number * 20.0;
  else if (unit == "pc" || unit == "pi") twips = number * 240.0;
  else if (!unit.empty()) return std::nullopt;
  // Far beyond any page Word lays out: not a length a reader can use.
  if (twips > 1.0e8) return std::nullopt;
  const long long rounded = static_cast<long long>(twips + 0.5);
  return static_cast<int>(negative ? -rounded : rounded);
}

// OOXML's three-valued toggle on a properties element: 1 on, 0 turned off,
// -1 where the element says nothing and something further out decides.
int toggle_of(const pugi::xml_node &properties, std::string_view name) {
  const auto element = child_element(properties, name);
  if (!element) return -1;
  return on_off_is_on(element) ? 1 : 0;
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
  return value;
}

// ---- Where a paragraph stands ----------------------------------------------

// A paragraph's indentation as one declaration states it. Each part is decided
// separately: a paragraph that moves only its left edge keeps the hanging
// indent its list level gives it.
struct IndentParts {
  std::optional<int> left;
  // Negative for a hanging indent.
  std::optional<int> first_line;
  std::optional<int> right;
};

IndentParts read_indent(const pugi::xml_node &indent) {
  IndentParts parts;
  if (!indent) return parts;
  parts.left = parse_twips(attribute_value(indent, "left"));
  if (!parts.left) parts.left = parse_twips(attribute_value(indent, "start"));
  parts.right = parse_twips(attribute_value(indent, "right"));
  if (!parts.right) parts.right = parse_twips(attribute_value(indent, "end"));
  const auto hanging = parse_twips(attribute_value(indent, "hanging"));
  if (hanging) parts.first_line = -*hanging;
  else parts.first_line = parse_twips(attribute_value(indent, "firstLine"));
  return parts;
}

void fill_missing(IndentParts &into, const IndentParts &from) {
  if (!into.left) into.left = from.left;
  if (!into.first_line) into.first_line = from.first_line;
  if (!into.right) into.right = from.right;
}

struct SpacingParts {
  std::optional<int> before;
  std::optional<int> after;
};

SpacingParts read_spacing(const pugi::xml_node &spacing) {
  SpacingParts parts;
  if (!spacing) return parts;
  // Word's "auto" spacing - the gap a web page puts between paragraphs - is
  // fourteen points, whatever number stands beside it.
  const auto automatic = [&](const char *name) {
    const std::string value = attribute_value(spacing, name);
    return value == "1" || value == "true" || value == "on";
  };
  parts.before = automatic("beforeAutospacing") ? std::optional<int>(280)
                                                : parse_twips(attribute_value(spacing, "before"));
  parts.after = automatic("afterAutospacing") ? std::optional<int>(280)
                                              : parse_twips(attribute_value(spacing, "after"));
  return parts;
}

// A paragraph's alignment in the four words a reader knows. A justification
// Word distributes differently for other scripts is still justified text.
std::string normalized_alignment(const std::string &value) {
  if (value == "center" || value == "right" || value == "both" || value == "left") return value;
  if (value == "start") return "left";
  if (value == "end") return "right";
  if (value == "distribute" || value == "thaiDistribute" || value.find("Kashida") != std::string::npos)
    return "both";
  return {};
}

// ---- Styles, as a reader needs them ----------------------------------------

// What a paragraph style says about where its paragraphs stand and whether
// they are in a list. Silence stays silence, so the style it is based on - and
// then the document's defaults - can speak.
struct ParagraphStyleLayout {
  std::string based_on;
  std::string name;
  bool names_list = false;
  std::string num_id;
  std::optional<unsigned int> list_level;
  IndentParts indent;
  SpacingParts spacing;
  int keep_next = -1;
  int page_break_before = -1;
  std::string alignment;
};

// A character style ("Strong", "Emphasis", "Footnote Reference"): marks a run
// takes when it names the style and does not say otherwise itself.
struct CharacterStyle {
  std::string based_on;
  StyleRunFormat format;
};

// Which edges of a table draw a line - top, left, bottom, right, and the
// lines between rows and between columns: 1 a line, 0 declared as none, -1
// not declared here.
using TableEdges = std::array<int, 6>;

struct TableStyle {
  std::string based_on;
  TableEdges edges{{-1, -1, -1, -1, -1, -1}};
  // The paragraph spacing the table style gives the paragraphs in its cells
  // ("Table Grid" sets none after, where the document default sets ten
  // points): above the document's defaults, below the paragraph's own style.
  SpacingParts spacing;
};

struct LayoutStyles {
  std::unordered_map<std::string, ParagraphStyleLayout> paragraphs;
  std::unordered_map<std::string, CharacterStyle> characters;
  std::unordered_map<std::string, TableStyle> tables;
  // A numbering style (w:type="numbering") and the list instance it names:
  // how a list defined through a style (w:numStyleLink) is reached.
  std::unordered_map<std::string, std::string> numbering_styles;
  // The style a paragraph or a table that names none is written in.
  std::string default_paragraph;
  std::string default_table;
  // The document's own defaults (w:docDefaults), under every style.
  IndentParts default_indent;
  SpacingParts default_spacing;
};

TableEdges read_table_edges(const pugi::xml_node &borders) {
  TableEdges edges{{-1, -1, -1, -1, -1, -1}};
  if (!borders) return edges;
  for (const auto &edge : borders.children()) {
    if (edge.type() != pugi::node_element) continue;
    const std::string_view name = local_name(edge.name());
    std::size_t index = edges.size();
    if (name == "top") index = 0;
    else if (name == "left" || name == "start") index = 1;
    else if (name == "bottom") index = 2;
    else if (name == "right" || name == "end") index = 3;
    else if (name == "insideH") index = 4;
    else if (name == "insideV") index = 5;
    if (index == edges.size()) continue;
    const std::string value = attribute_value(edge, "val");
    edges[index] = value.empty() || value == "nil" || value == "none" ? 0 : 1;
  }
  return edges;
}

void collect_layout_styles(const pugi::xml_node &node, LayoutStyles &styles) {
  for (const auto &child : node.children()) {
    if (child.type() != pugi::node_element) continue;
    if (is_element(child, "docDefaults")) {
      const auto defaults = child_element(child_element(child, "pPrDefault"), "pPr");
      styles.default_indent = read_indent(child_element(defaults, "ind"));
      styles.default_spacing = read_spacing(child_element(defaults, "spacing"));
      continue;
    }
    if (!is_element(child, "style")) {
      collect_layout_styles(child, styles);
      continue;
    }
    const std::string id = attribute_value(child, "styleId");
    if (id.empty()) continue;
    const std::string type = attribute_value(child, "type");
    const std::string declaredDefault = attribute_value(child, "default");
    const bool isDefault = declaredDefault == "1" || declaredDefault == "true" || declaredDefault == "on";
    const auto basedOn = child_element(child, "basedOn");
    const std::string basedOnId = basedOn ? attribute_value(basedOn, "val") : std::string();
    const auto properties = child_element(child, "pPr");
    if (type == "paragraph") {
      ParagraphStyleLayout layout;
      layout.based_on = basedOnId;
      const auto name = child_element(child, "name");
      if (name) layout.name = attribute_value(name, "val");
      const auto numbering = child_element(properties, "numPr");
      if (numbering) {
        const auto numId = child_element(numbering, "numId");
        if (numId) {
          layout.names_list = true;
          layout.num_id = attribute_value(numId, "val");
        }
        const auto level = child_element(numbering, "ilvl");
        const auto parsed = level ? parse_int(attribute_value(level, "val")) : std::nullopt;
        if (parsed && *parsed >= 0 && *parsed < static_cast<int>(kListLevels))
          layout.list_level = static_cast<unsigned int>(*parsed);
      }
      layout.indent = read_indent(child_element(properties, "ind"));
      layout.spacing = read_spacing(child_element(properties, "spacing"));
      layout.keep_next = toggle_of(properties, "keepNext");
      layout.page_break_before = toggle_of(properties, "pageBreakBefore");
      const auto alignment = child_element(properties, "jc");
      if (alignment) layout.alignment = normalized_alignment(attribute_value(alignment, "val"));
      styles.paragraphs[id] = std::move(layout);
      if (isDefault) styles.default_paragraph = id;
    } else if (type == "character") {
      CharacterStyle style;
      style.based_on = basedOnId;
      style.format = read_run_format(child_element(child, "rPr"));
      styles.characters[id] = std::move(style);
    } else if (type == "table") {
      TableStyle style;
      style.based_on = basedOnId;
      style.edges = read_table_edges(child_element(child_element(child, "tblPr"), "tblBorders"));
      style.spacing = read_spacing(child_element(properties, "spacing"));
      styles.tables[id] = std::move(style);
      if (isDefault) styles.default_table = id;
    } else if (type == "numbering") {
      const auto numId = child_element(child_element(properties, "numPr"), "numId");
      if (numId) styles.numbering_styles[id] = attribute_value(numId, "val");
    }
  }
}

// Every style in a paragraph style's chain, nearest first, until `visit`
// returns false or the chain ends.
template <typename Visit>
void each_paragraph_style(const std::string &styleId, const LayoutStyles &styles, Visit visit) {
  std::unordered_set<std::string> visited;
  std::string current = styleId;
  while (!current.empty() && visited.insert(current).second) {
    const auto found = styles.paragraphs.find(current);
    if (found == styles.paragraphs.end()) return;
    if (!visit(current, found->second)) return;
    current = found->second.based_on;
  }
}

// A character style's marks, read down its based-on chain.
StyleRunFormat resolve_character_format(const std::string &styleId, const LayoutStyles &styles) {
  StyleRunFormat resolved;
  std::unordered_set<std::string> visited;
  std::string current = styleId;
  while (!current.empty() && visited.insert(current).second) {
    const auto found = styles.characters.find(current);
    if (found == styles.characters.end()) break;
    const StyleRunFormat &format = found->second.format;
    if (resolved.bold < 0) resolved.bold = format.bold;
    if (resolved.italic < 0) resolved.italic = format.italic;
    if (resolved.underline < 0) resolved.underline = format.underline;
    if (resolved.strike < 0) resolved.strike = format.strike;
    if (resolved.caps < 0) resolved.caps = format.caps;
    if (resolved.small_caps < 0) resolved.small_caps = format.small_caps;
    if (resolved.vert_align.empty()) resolved.vert_align = format.vert_align;
    if (resolved.color_hex.empty()) resolved.color_hex = format.color_hex;
    if (resolved.half_points == 0) resolved.half_points = format.half_points;
    current = found->second.based_on;
  }
  return resolved;
}

// Whether a table draws any line at all. The table's own declaration decides
// each edge it names; its table style - and the styles that one is built on -
// decide the rest. A table that names no style is in the document's default
// table style, which in Word draws nothing.
bool table_has_borders(const pugi::xml_node &properties, const LayoutStyles &styles) {
  TableEdges edges = read_table_edges(child_element(properties, "tblBorders"));
  const auto styleElement = child_element(properties, "tblStyle");
  std::string current = styleElement ? attribute_value(styleElement, "val") : styles.default_table;
  std::unordered_set<std::string> visited;
  while (!current.empty() && visited.insert(current).second) {
    const auto found = styles.tables.find(current);
    if (found == styles.tables.end()) break;
    for (std::size_t index = 0; index < edges.size(); ++index)
      if (edges[index] < 0) edges[index] = found->second.edges[index];
    current = found->second.based_on;
  }
  return std::any_of(edges.begin(), edges.end(), [](const int edge) { return edge == 1; });
}

// A paragraph style's marks, read down its based-on chain: the nearest style
// that actually says something about a mark decides it.
StyleRunFormat resolve_style_run_format(const std::string &styleId, const StyleMap &styles) {
  StyleRunFormat resolved;
  std::unordered_set<std::string> visited;
  std::string current = styleId;
  while (!current.empty() && visited.insert(current).second) {
    const auto found = styles.find(current);
    if (found == styles.end()) break;
    const StyleRunFormat &format = found->second.run_format;
    if (resolved.bold < 0) resolved.bold = format.bold;
    if (resolved.italic < 0) resolved.italic = format.italic;
    if (resolved.underline < 0) resolved.underline = format.underline;
    if (resolved.strike < 0) resolved.strike = format.strike;
    if (resolved.caps < 0) resolved.caps = format.caps;
    if (resolved.small_caps < 0) resolved.small_caps = format.small_caps;
    if (resolved.vert_align.empty()) resolved.vert_align = format.vert_align;
    if (resolved.color_hex.empty()) resolved.color_hex = format.color_hex;
    if (resolved.half_points == 0) resolved.half_points = format.half_points;
    current = found->second.based_on;
  }
  return resolved;
}

// ---- Numbering --------------------------------------------------------------
//
// Word stores no computed numbers anywhere. A list paragraph names a list
// instance (w:numId) and a level; the instance names an abstract list, which
// says for each level where it starts, how its number is written and what
// text surrounds it ("%1.%2", "(%3)"). The numbers themselves exist only when
// the document is laid out, and they are counted here the way Word counts
// them, so "Section 4.2(b)" in the text and the "(b)" in the margin agree.

// One level of a list.
struct ListLevel {
  int start = 0;
  std::string format = "decimal";
  // Digits a number is padded to, for a zero-padded format ("001, 002, ...").
  std::size_t pad = 0;
  std::string text;
  // w:lvlRestart: restart after a use of this (1-based) level or any above
  // it; 0 never. Absent: restart after any level above this one.
  std::optional<int> restart;
  // Legal numbering (w:isLgl): every number shown in arabic numerals.
  bool legal = false;
  // A bullet drawn from a symbol font, or as a picture: its character is a
  // glyph of that font, not the letter it happens to be.
  bool symbol_font = false;
  bool picture = false;
  // The paragraph style this level belongs to, where a style carries the list.
  std::string style;
  IndentParts indent;
};

struct AbstractList {
  std::array<std::optional<ListLevel>, kListLevels> levels;
  // A list defined by a numbering style: the levels are in the list that
  // style names.
  std::string num_style_link;
};

struct ListInstance {
  std::string abstract_id;
  // "Restart numbering": a level that starts again at its own value, or a
  // level the instance redefines outright.
  std::array<std::optional<int>, kListLevels> start_override;
  std::array<std::optional<ListLevel>, kListLevels> level_override;

  [[nodiscard]] bool restarts() const {
    for (std::size_t level = 0; level < kListLevels; ++level)
      if (start_override[level] || level_override[level]) return true;
    return false;
  }
};

struct NumberingDefinitions {
  std::unordered_map<std::string, AbstractList> abstracts;
  std::unordered_map<std::string, ListInstance> instances;
};

bool is_symbol_font(const std::string &font) {
  const std::string name = lowercase(font);
  return name == "symbol" || name.rfind("wingdings", 0) == 0 || name == "webdings"
         || name.rfind("zapf", 0) == 0 || name == "marlett";
}

ListLevel read_list_level(const pugi::xml_node &level) {
  ListLevel result;
  const auto start = child_element(level, "start");
  if (start) result.start = parse_int(attribute_value(start, "val")).value_or(0);
  pugi::xml_node format = child_element(level, "numFmt");
  if (!format) {
    // Word writes a zero-padded format ("001, 002, 003, ...") for readers
    // that know it, with a plain one beside it for readers that do not.
    const auto alternate = child_element(level, "AlternateContent");
    const auto chosen = child_element(child_element(alternate, "Choice"), "numFmt");
    const auto fallback = child_element(child_element(alternate, "Fallback"), "numFmt");
    if (chosen && attribute_value(chosen, "val") == "custom") {
      const std::string pattern = attribute_value(chosen, "format");
      const std::string first = pattern.substr(0, pattern.find(','));
      if (!first.empty() && std::all_of(first.begin(), first.end(), [](const unsigned char byte) {
            return std::isdigit(byte) != 0;
          })) {
        result.pad = first.size();
      }
    }
    format = result.pad > 0 ? pugi::xml_node() : (fallback ? fallback : chosen);
  }
  if (format) {
    const std::string value = attribute_value(format, "val");
    if (!value.empty()) result.format = value;
  }
  const auto text = child_element(level, "lvlText");
  if (text) result.text = attribute_value(text, "val");
  const auto restart = child_element(level, "lvlRestart");
  if (restart) result.restart = parse_int(attribute_value(restart, "val"));
  const auto legal = child_element(level, "isLgl");
  result.legal = legal && on_off_is_on(legal);
  result.picture = static_cast<bool>(child_element(level, "lvlPicBulletId"));
  const auto style = child_element(level, "pStyle");
  if (style) result.style = attribute_value(style, "val");
  result.indent = read_indent(child_element(child_element(level, "pPr"), "ind"));
  const auto fonts = child_element(child_element(level, "rPr"), "rFonts");
  for (const char *slot : {"ascii", "hAnsi", "cs", "eastAsia"})
    if (is_symbol_font(attribute_value(fonts, slot))) result.symbol_font = true;
  return result;
}

std::optional<std::size_t> list_level_index(const pugi::xml_node &node) {
  const auto parsed = parse_int(attribute_value(node, "ilvl"));
  if (!parsed || *parsed < 0 || *parsed >= static_cast<int>(kListLevels)) return std::nullopt;
  return static_cast<std::size_t>(*parsed);
}

void collect_numbering(const pugi::xml_node &node, NumberingDefinitions &numbering) {
  for (const auto &child : node.children()) {
    if (child.type() != pugi::node_element) continue;
    if (is_element(child, "abstractNum")) {
      const std::string abstractId = attribute_value(child, "abstractNumId");
      if (abstractId.empty()) continue;
      AbstractList list;
      for (const auto &level : child.children()) {
        if (!is_element(level, "lvl")) continue;
        const auto index = list_level_index(level);
        if (index) list.levels[*index] = read_list_level(level);
      }
      const auto link = child_element(child, "numStyleLink");
      if (link) list.num_style_link = attribute_value(link, "val");
      numbering.abstracts[abstractId] = std::move(list);
      continue;
    }
    if (is_element(child, "num")) {
      const std::string numId = attribute_value(child, "numId");
      const auto abstractRef = child_element(child, "abstractNumId");
      if (numId.empty() || !abstractRef) continue;
      ListInstance instance;
      instance.abstract_id = attribute_value(abstractRef, "val");
      for (const auto &levelOverride : child.children()) {
        if (!is_element(levelOverride, "lvlOverride")) continue;
        const auto index = list_level_index(levelOverride);
        if (!index) continue;
        const auto startOverride = child_element(levelOverride, "startOverride");
        if (startOverride) instance.start_override[*index] = parse_int(attribute_value(startOverride, "val"));
        const auto level = child_element(levelOverride, "lvl");
        if (level) instance.level_override[*index] = read_list_level(level);
      }
      numbering.instances[numId] = std::move(instance);
      continue;
    }
    collect_numbering(child, numbering);
  }
}

// A list instance with the abstract list that holds its levels, and the
// counters it counts with. Instances of the same abstract list continue one
// another, as in Word; an instance that restarts numbering counts on its own.
struct ResolvedList {
  const ListInstance *instance = nullptr;
  const AbstractList *abstract = nullptr;
  std::string counter_key;

  [[nodiscard]] const ListLevel *level(std::size_t index) const {
    if (instance == nullptr || abstract == nullptr || index >= kListLevels) return nullptr;
    if (instance->level_override[index]) return &*instance->level_override[index];
    if (abstract->levels[index]) return &*abstract->levels[index];
    return nullptr;
  }

  [[nodiscard]] int start(std::size_t index) const {
    if (instance != nullptr && index < kListLevels && instance->start_override[index])
      return *instance->start_override[index];
    const ListLevel *definition = level(index);
    return definition != nullptr ? definition->start : 0;
  }
};

ResolvedList resolve_list(const std::string &numId, const NumberingDefinitions &numbering,
                          const LayoutStyles &styles) {
  ResolvedList resolved;
  const auto instance = numbering.instances.find(numId);
  if (instance == numbering.instances.end()) return resolved;
  std::string abstractId = instance->second.abstract_id;
  auto abstract = numbering.abstracts.find(abstractId);
  // A list defined through a numbering style: this abstract list only names
  // the style, the style names a list instance, and that instance's abstract
  // list holds the levels.
  for (int hops = 0; abstract != numbering.abstracts.end() && !abstract->second.num_style_link.empty()
                     && hops < 8; ++hops) {
    const auto style = styles.numbering_styles.find(abstract->second.num_style_link);
    if (style == styles.numbering_styles.end()) break;
    const auto linked = numbering.instances.find(style->second);
    if (linked == numbering.instances.end()) break;
    const auto next = numbering.abstracts.find(linked->second.abstract_id);
    if (next == numbering.abstracts.end() || next == abstract) break;
    abstractId = linked->second.abstract_id;
    abstract = next;
  }
  if (abstract == numbering.abstracts.end()) return resolved;
  resolved.instance = &instance->second;
  resolved.abstract = &abstract->second;
  resolved.counter_key = instance->second.restarts() ? "num:" + numId : "abstract:" + abstractId;
  return resolved;
}

std::string roman_numeral(int value, bool upper) {
  static const std::pair<int, const char *> kNumerals[] = {
      {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"}, {50, "l"},
      {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
  std::string numeral;
  for (const auto &[amount, letters] : kNumerals) {
    while (value >= amount) {
      numeral += letters;
      value -= amount;
    }
  }
  if (upper) {
    std::transform(numeral.begin(), numeral.end(), numeral.begin(),
                   [](const unsigned char byte) { return static_cast<char>(std::toupper(byte)); });
  }
  return numeral;
}

// Word's lettered lists: a ... z, then aa ... zz, then aaa ...
std::string letter_numeral(int value, bool upper) {
  const int index = (value - 1) % 26;
  const int repeat = std::min((value - 1) / 26 + 1, 64);
  return std::string(static_cast<std::size_t>(repeat), static_cast<char>((upper ? 'A' : 'a') + index));
}

// One level's number as that level writes it. Legal numbering writes every
// level in arabic numerals. A format Singularity does not spell out - an ordinal,
// a number in another script - is written as its plain number, which is the
// number it stands for.
std::string list_number(int value, const ListLevel &level, bool legal) {
  const std::string &format = level.format;
  if (format == "none" || format == "bullet") return {};
  if (legal) return std::to_string(value);
  if (level.pad > 0) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    if (digits.size() < level.pad) digits.insert(0, level.pad - digits.size(), '0');
    return value < 0 ? "-" + digits : digits;
  }
  if (format == "decimalZero") return value >= 0 && value < 10 ? "0" + std::to_string(value) : std::to_string(value);
  if ((format == "lowerRoman" || format == "upperRoman") && value > 0 && value < 4000)
    return roman_numeral(value, format == "upperRoman");
  if ((format == "lowerLetter" || format == "upperLetter") && value > 0)
    return letter_numeral(value, format == "upperLetter");
  return std::to_string(value);
}

// Whether every character of `text` is one a reader's own font draws: no
// control characters and nothing from the private-use area, where the symbol
// fonts keep their glyphs.
bool is_printable_text(const std::string &text) {
  bool visible = false;
  for (std::size_t index = 0; index < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    std::uint32_t codePoint = 0;
    std::size_t length = 0;
    if (lead < 0x80) { codePoint = lead; length = 1; }
    else if ((lead & 0xE0) == 0xC0) { codePoint = lead & 0x1F; length = 2; }
    else if ((lead & 0xF0) == 0xE0) { codePoint = lead & 0x0F; length = 3; }
    else if ((lead & 0xF8) == 0xF0) { codePoint = lead & 0x07; length = 4; }
    else return false;
    if (index + length > text.size()) return false;
    for (std::size_t next = 1; next < length; ++next) {
      const unsigned char continuation = static_cast<unsigned char>(text[index + next]);
      if ((continuation & 0xC0) != 0x80) return false;
      codePoint = (codePoint << 6) | (continuation & 0x3F);
    }
    index += length;
    if (codePoint < 0x20 || (codePoint >= 0x7F && codePoint < 0xA0)) return false;
    if (codePoint >= 0xE000 && codePoint <= 0xF8FF) return false;
    if (codePoint != 0x20 && codePoint != 0xA0) visible = true;
  }
  return visible;
}

// The bullet Word draws for a bullet level. A character a reader's own font
// draws is drawn as written ("o", "-", "▪"); a glyph that exists only in a
// symbol font - a private-use code point, or an ordinary letter that Symbol or
// Wingdings draws as a shape - is drawn as the ordinary bullet.
std::string bullet_marker(const ListLevel &level) {
  if (level.picture || level.symbol_font || !is_printable_text(level.text)) return "\xE2\x80\xA2";
  return level.text;
}

// Each list's counters, level by level, as the document is read in order.
struct ListCounters {
  std::array<int, kListLevels> value{};
  std::array<bool, kListLevels> used{};
};
using NumberingState = std::unordered_map<std::string, ListCounters>;

// Counts one more paragraph of `list` at `level` and returns what Word draws
// in front of it.
std::string next_list_marker(const ResolvedList &list, std::size_t level, NumberingState &state) {
  ListCounters &counters = state[list.counter_key];
  counters.value[level] = counters.used[level] ? counters.value[level] + 1 : list.start(level);
  counters.used[level] = true;
  // A deeper level starts again under a new parent - or, where the level
  // says so itself, only under a parent at or above the level it names.
  for (std::size_t deeper = level + 1; deeper < kListLevels; ++deeper) {
    const ListLevel *definition = list.level(deeper);
    const int threshold = definition != nullptr && definition->restart
                              ? *definition->restart : static_cast<int>(deeper);
    if (static_cast<int>(level) < threshold) counters.used[deeper] = false;
  }
  const ListLevel *definition = list.level(level);
  if (definition == nullptr) return {};
  if (definition->format == "bullet") return bullet_marker(*definition);
  std::string marker;
  const std::string &text = definition->text;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '%' || index + 1 >= text.size() || text[index + 1] < '1' || text[index + 1] > '9') {
      marker.push_back(text[index]);
      continue;
    }
    const std::size_t referenced = static_cast<std::size_t>(text[index + 1] - '1');
    ++index;
    const ListLevel *referencedLevel = list.level(referenced);
    if (referencedLevel == nullptr) continue;
    // A level above this one that the list has not reached yet reads as the
    // number before its start - Word's own "0.1" under no heading.
    const int value = counters.used[referenced] ? counters.value[referenced]
                      : referenced < level ? std::max(0, list.start(referenced) - 1)
                                           : list.start(referenced);
    marker += list_number(value, *referencedLevel, definition->legal);
  }
  return marker;
}

// ---- The package around the body --------------------------------------------

struct Relationship {
  std::string type;
  std::string target;
  bool external = false;
};
using Relationships = std::unordered_map<std::string, Relationship>;

// A part the reading needs beyond the body - a header, the notes, the
// comments, their relationships - or false where the package has no such part
// or it cannot be read safely. None of them is needed to read the body, so
// none of them can stop it.
bool read_optional_part(zip_t *archive, const std::string &name, pugi::xml_document &document) {
  if (name.empty()) return false;
  try {
    const zip_int64_t index = zip_name_locate(archive, name.c_str(), ZIP_FL_NOCASE | ZIP_FL_ENC_GUESS);
    if (index < 0) return false;
    zip_stat_t entry{};
    zip_stat_init(&entry);
    if (zip_stat_index(archive, static_cast<zip_uint64_t>(index), ZIP_FL_ENC_GUESS, &entry) != 0
        || (entry.valid & ZIP_STAT_SIZE) == 0 || entry.size > kMaximumStylesXmlBytes) {
      return false;
    }
    const std::string bytes = read_entry(archive, static_cast<zip_uint64_t>(index), entry.size,
                                         kMaximumStylesXmlBytes);
    document = parse_xml(bytes, "docx_malformed_part", "A document part is malformed.");
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

// A relationship's target as a part name: relative to the part that names it
// ("header1.xml" from word/document.xml is word/header1.xml), or from the
// package root when it starts with "/".
std::string resolve_part_name(const std::string &sourcePart, const std::string &target) {
  if (target.empty()) return {};
  const std::string combined = target.front() == '/'
      ? target.substr(1) : sourcePart.substr(0, sourcePart.rfind('/') + 1) + target;
  std::vector<std::string> segments;
  std::size_t start = 0;
  while (start <= combined.size()) {
    const std::size_t end = std::min(combined.find('/', start), combined.size());
    const std::string segment = combined.substr(start, end - start);
    if (segment == "..") {
      if (!segments.empty()) segments.pop_back();
    } else if (!segment.empty() && segment != ".") {
      segments.push_back(segment);
    }
    start = end + 1;
  }
  std::string name;
  for (const std::string &segment : segments) name += (name.empty() ? "" : "/") + segment;
  return name;
}

Relationships read_relationships(zip_t *archive, const std::string &partName) {
  Relationships relationships;
  const std::size_t slash = partName.rfind('/');
  const std::string directory = slash == std::string::npos ? std::string() : partName.substr(0, slash + 1);
  const std::string file = slash == std::string::npos ? partName : partName.substr(slash + 1);
  pugi::xml_document document;
  if (!read_optional_part(archive, directory + "_rels/" + file + ".rels", document)) return relationships;
  const auto root = child_element(document, "Relationships");
  for (const auto &child : root.children()) {
    if (!is_element(child, "Relationship")) continue;
    Relationship relationship;
    relationship.type = attribute_value(child, "Type");
    relationship.target = attribute_value(child, "Target");
    relationship.external = attribute_value(child, "TargetMode") == "External";
    relationships[attribute_value(child, "Id")] = std::move(relationship);
  }
  return relationships;
}

bool ends_with(const std::string &value, std::string_view suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The part the document's relationships name for a kind of content
// ("/footnotes", "/comments"), or the name Word gives it where the
// relationships say nothing.
std::string related_part(const Relationships &relationships, std::string_view typeSuffix,
                         const std::string &conventionalName) {
  std::string found;
  for (const auto &[id, relationship] : relationships) {
    if (relationship.external || !ends_with(relationship.type, typeSuffix)) continue;
    // Several of one kind is not something Word writes; the lowest id is the
    // one read, so a reading never depends on the order of a hash map.
    if (found.empty() || id < found) found = id;
  }
  if (found.empty()) return conventionalName;
  return resolve_part_name("word/document.xml", relationships.at(found).target);
}

// ---- Reading a story ----------------------------------------------------------

// The numbers the notes take, in the order their references are met.
struct NoteNumbers {
  std::unordered_map<std::string, unsigned int> number;
  std::vector<std::string> order;
};

// What one reading of a document carries from part to part.
struct DocumentReading {
  NoteNumbers footnotes;
  NoteNumbers endnotes;
  std::vector<std::string> comment_order;
  std::unordered_set<std::string> comment_seen;
};

// A field (w:fldChar ... w:fldChar) being read: its instruction, and once its
// result begins, where a HYPERLINK field leads.
struct FieldReading {
  bool in_result = false;
  std::string instruction;
  std::string link;
};

// One story of the document - the body, a header, a note - read in order.
struct StoryReading {
  const Relationships *relationships = nullptr;
  // The body's tables by their node, numbered as collect_tbl_nodes() numbers
  // them for the locator and the writer. Absent outside the body.
  const std::unordered_map<const void *, std::size_t> *table_ids = nullptr;
  NumberingState numbering;
  std::vector<std::string> open_comments;
  std::unordered_set<std::string> ranged_comments;
  std::vector<FieldReading> fields;
};

struct RenderContext {
  const StyleMap &styles;
  const LayoutStyles &layout;
  const NumberingDefinitions &numbering;
  DocumentReading &document;
  StoryReading story;
  // The table style of the table whose cells are being read; "" outside one.
  std::string table_style{};
};

// What a run inherits from what stands around it: its paragraph style's
// marks, a tracked change wrapped round it, a hyperlink it sits in.
struct InlineContext {
  StyleRunFormat paragraph_format;
  std::optional<DocumentRenderRevision> revision;
  std::string link;
};

std::optional<DocumentRenderRevisionKind> revision_kind(std::string_view name) {
  if (name == "ins" || name == "moveTo") return DocumentRenderRevisionKind::Inserted;
  if (name == "del" || name == "moveFrom") return DocumentRenderRevisionKind::Deleted;
  return std::nullopt;
}

DocumentRenderRevision read_revision(const pugi::xml_node &node, DocumentRenderRevisionKind kind) {
  DocumentRenderRevision revision;
  revision.kind = kind;
  revision.id = attribute_value(node, "id");
  revision.author = attribute_value(node, "author");
  revision.date = attribute_value(node, "date");
  revision.date_utc = attribute_value(node, "dateUtc");
  return revision;
}

// An element as XML text, for telling whether two say the same.
std::string element_xml(const pugi::xml_node &node) {
  if (!node) return {};
  std::ostringstream text;
  node.print(text, "", pugi::format_raw);
  return text.str();
}

// ---- Formatting changes ----------------------------------------------------
//
// Word records "made bold" as the run's properties as they were, kept inside
// the run's properties as they are (w:rPrChange); a paragraph's the same way
// (w:pPrChange). What the change did is the difference between the two, said
// in the words Singularity uses for a formatting change everywhere else
// (DocumentChangeDescription's formatting_words): "bold", "not italic",
// "14 pt", "aligned center".

std::optional<DocumentRenderRevision> run_format_revision(const pugi::xml_node &runProperties) {
  const auto change = child_element(runProperties, "rPrChange");
  if (!change) return std::nullopt;
  const auto was = child_element(change, "rPr");
  DocumentRenderRevision revision = read_revision(change, DocumentRenderRevisionKind::Formatted);
  const StyleRunFormat now = read_run_format(runProperties);
  const StyleRunFormat before = read_run_format(was);
  // Silence is not the mark: a run that says nothing about bold is not bold
  // here - its style may make it so, and a change to the style is not this one.
  const auto on = [](int value) { return value == 1; };
  DocumentTextFormatting formatting;
  if (on(now.bold) != on(before.bold)) formatting.bold = on(now.bold);
  if (on(now.italic) != on(before.italic)) formatting.italic = on(now.italic);
  if (on(now.underline) != on(before.underline)) formatting.underline = on(now.underline);
  if (now.color_hex != before.color_hex && !now.color_hex.empty()) formatting.color_hex = now.color_hex;
  const std::string highlightNow = attribute_value(child_element(runProperties, "highlight"), "val");
  const std::string highlightWas = attribute_value(child_element(was, "highlight"), "val");
  if (highlightNow != highlightWas) formatting.highlight = highlightNow.empty() ? std::string("none") : highlightNow;
  if (now.half_points != before.half_points && now.half_points != 0) formatting.size_points = now.half_points / 2.0;
  const auto font = [](const pugi::xml_node &properties) {
    const auto fonts = child_element(properties, "rFonts");
    const std::string ascii = attribute_value(fonts, "ascii");
    return ascii.empty() ? attribute_value(fonts, "hAnsi") : ascii;
  };
  if (font(runProperties) != font(was) && !font(runProperties).empty()) formatting.font_name = font(runProperties);
  revision.changes = change_description_detail::formatting_words(formatting);
  if (on(now.strike) != on(before.strike)) revision.changes.push_back(on(now.strike) ? "struck through" : "not struck through");
  if (on(now.caps) != on(before.caps)) revision.changes.push_back(on(now.caps) ? "capitals" : "not capitals");
  if (on(now.small_caps) != on(before.small_caps))
    revision.changes.push_back(on(now.small_caps) ? "small capitals" : "not small capitals");
  if (now.vert_align != before.vert_align) {
    revision.changes.push_back(now.vert_align == "superscript" ? "superscript"
                               : now.vert_align == "subscript" ? "subscript" : "on the line");
  }
  // A change to what the words are written in that has no word of its own -
  // a character style, a language, spacing between the letters.
  if (revision.changes.empty()) revision.changes.push_back("formatting");
  return revision;
}

std::optional<DocumentRenderRevision> paragraph_format_revision(const pugi::xml_node &properties,
                                                                const LayoutStyles &layout) {
  const auto change = child_element(properties, "pPrChange");
  if (!change) return std::nullopt;
  const auto was = child_element(change, "pPr");
  DocumentRenderRevision revision = read_revision(change, DocumentRenderRevisionKind::Formatted);
  DocumentTextFormatting formatting;
  const std::string styleNow = attribute_value(child_element(properties, "pStyle"), "val");
  const std::string styleWas = attribute_value(child_element(was, "pStyle"), "val");
  if (styleNow != styleWas) {
    // The style by the name Word shows for it ("Heading 1"), not its id.
    const std::string id = styleNow.empty() ? layout.default_paragraph : styleNow;
    const auto found = layout.paragraphs.find(id);
    const std::string name = found != layout.paragraphs.end() && !found->second.name.empty() ? found->second.name : id;
    if (!name.empty()) formatting.paragraph_style = name;
  }
  const std::string alignmentNow = normalized_alignment(attribute_value(child_element(properties, "jc"), "val"));
  const std::string alignmentWas = normalized_alignment(attribute_value(child_element(was, "jc"), "val"));
  if (alignmentNow != alignmentWas && !alignmentNow.empty())
    formatting.alignment = alignmentNow == "both" ? std::string("justify") : alignmentNow;
  const SpacingParts spacingNow = read_spacing(child_element(properties, "spacing"));
  const SpacingParts spacingWas = read_spacing(child_element(was, "spacing"));
  if (spacingNow.before != spacingWas.before && spacingNow.before) formatting.space_before_points = *spacingNow.before / 20.0;
  if (spacingNow.after != spacingWas.after && spacingNow.after) formatting.space_after_points = *spacingNow.after / 20.0;
  revision.changes = change_description_detail::formatting_words(formatting);
  if (element_xml(child_element(properties, "ind")) != element_xml(child_element(was, "ind")))
    revision.changes.push_back("indentation");
  if (element_xml(child_element(properties, "numPr")) != element_xml(child_element(was, "numPr")))
    revision.changes.push_back(child_element(properties, "numPr") ? "numbered" : "not numbered");
  if (revision.changes.empty()) revision.changes.push_back("paragraph formatting");
  return revision;
}

// A cell given another look (w:tcPrChange): its background, said as a colour
// where the document writes one; anything else about the cell - its width,
// its borders - as "cell formatting".
std::optional<DocumentRenderRevision> cell_format_revision(const pugi::xml_node &cellNode) {
  const auto properties = child_element(cellNode, "tcPr");
  const auto change = child_element(properties, "tcPrChange");
  if (!change) return std::nullopt;
  const auto was = child_element(change, "tcPr");
  DocumentRenderRevision revision = read_revision(change, DocumentRenderRevisionKind::Formatted);
  const auto fill = [](const pugi::xml_node &cellProperties) {
    const auto shading = child_element(cellProperties, "shd");
    std::string value = attribute_value(shading, "fill");
    if (value == "auto") value.clear();
    const std::string theme = attribute_value(shading, "themeFill");
    return std::make_pair(value, theme);
  };
  const auto now = fill(properties);
  const auto before = fill(was);
  if (now != before) {
    if (!now.first.empty()) revision.changes.push_back("cell shading #" + now.first);
    else if (!now.second.empty()) revision.changes.push_back("cell shading");
    else revision.changes.push_back("no cell shading");
  }
  // Everything else the cell says about itself, the change record aside.
  const auto rest = [](const pugi::xml_node &cellProperties) {
    std::string text;
    for (const auto &child : cellProperties.children()) {
      if (child.type() != pugi::node_element) continue;
      const std::string_view name = local_name(child.name());
      if (name == "tcPrChange" || name == "shd") continue;
      text += element_xml(child);
    }
    return text;
  };
  if (rest(properties) != rest(was)) revision.changes.push_back("cell formatting");
  if (revision.changes.empty()) revision.changes.push_back("cell formatting");
  return revision;
}

void note_comment(RenderContext &context, const std::string &id) {
  if (!id.empty() && context.document.comment_seen.insert(id).second) context.document.comment_order.push_back(id);
}

void open_comment(RenderContext &context, const std::string &id) {
  if (id.empty()) return;
  note_comment(context, id);
  context.story.ranged_comments.insert(id);
  auto &open = context.story.open_comments;
  if (std::find(open.begin(), open.end(), id) == open.end()) open.push_back(id);
}

void close_comment(RenderContext &context, const std::string &id) {
  auto &open = context.story.open_comments;
  open.erase(std::remove(open.begin(), open.end(), id), open.end());
}

// A link a reader may be offered: a web page or an e-mail address. Anything
// else a file can name - a script, a local program, a path on someone's
// drive - is not passed on as a link, whatever the document says.
std::string openable_link(const std::string &target) {
  std::size_t start = 0;
  while (start < target.size() && static_cast<unsigned char>(target[start]) <= 0x20) ++start;
  const std::string link = target.substr(start);
  const std::size_t colon = link.find(':');
  if (colon == std::string::npos) return {};
  const std::string scheme = lowercase(link.substr(0, colon));
  return scheme == "http" || scheme == "https" || scheme == "mailto" ? link : std::string();
}

// Where a w:hyperlink leads when it leads outside the document; "" for a link
// to a bookmark inside it.
std::string hyperlink_target(const pugi::xml_node &hyperlink, const RenderContext &context) {
  const std::string id = attribute_value(hyperlink, "id");
  if (id.empty() || context.story.relationships == nullptr) return {};
  const auto found = context.story.relationships->find(id);
  if (found == context.story.relationships->end() || !found->second.external) return {};
  const std::string anchor = attribute_value(hyperlink, "anchor");
  return openable_link(anchor.empty() ? found->second.target : found->second.target + "#" + anchor);
}

// Where a HYPERLINK field leads ("HYPERLINK \"https://...\" \o \"tip\""): its
// first argument. A field that only names a bookmark (\l) leads nowhere
// outside the document.
std::string field_hyperlink(const std::string &instruction) {
  std::vector<std::pair<std::string, bool>> tokens;
  for (std::size_t index = 0; index < instruction.size();) {
    const char character = instruction[index];
    if (character == ' ' || character == '\t') {
      ++index;
      continue;
    }
    if (character == '"') {
      const std::size_t close = instruction.find('"', index + 1);
      const std::size_t end = close == std::string::npos ? instruction.size() : close;
      tokens.emplace_back(instruction.substr(index + 1, end - index - 1), true);
      index = end + 1;
      continue;
    }
    std::size_t end = index;
    while (end < instruction.size() && instruction[end] != ' ' && instruction[end] != '\t') ++end;
    tokens.emplace_back(instruction.substr(index, end - index), false);
    index = end;
  }
  if (tokens.empty() || lowercase(tokens.front().first) != "hyperlink") return {};
  for (std::size_t index = 1; index < tokens.size(); ++index) {
    const auto &[token, quoted] = tokens[index];
    if (!quoted && !token.empty() && token.front() == '\\') {
      // \l names a bookmark, \o a tooltip, \t a frame: each takes the argument
      // after it, which is never the target.
      const std::string name = lowercase(token);
      if (name == "\\l" || name == "\\o" || name == "\\t") ++index;
      continue;
    }
    return openable_link(token);
  }
  return {};
}

std::string current_field_link(const RenderContext &context) {
  for (auto field = context.story.fields.rbegin(); field != context.story.fields.rend(); ++field)
    if (field->in_result && !field->link.empty()) return field->link;
  return {};
}

// Whether the text being read belongs to a field's instruction rather than to
// what the field shows - the result of a field nested inside another field's
// code ("IF { MERGEFIELD Party } = ..."), which Word never displays.
bool in_field_instruction(const RenderContext &context) {
  return std::any_of(context.story.fields.begin(), context.story.fields.end(),
                     [](const FieldReading &field) { return !field.in_result; });
}

std::string utf8_of(std::uint32_t codePoint) {
  std::string text;
  if (codePoint < 0x80) {
    text.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800) {
    text.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint < 0x10000) {
    text.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint < 0x110000) {
    text.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    text.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
  return text;
}

// A symbol inserted from a symbol font (w:sym): the character it stands for
// where it has an ordinary one - the ticks, crosses, check boxes and bullets
// forms are made of. A glyph with no ordinary equivalent is left out, as it is
// from every other reading, rather than shown as the letter the font never
// meant.
std::string symbol_text(const pugi::xml_node &symbol) {
  const std::string font = lowercase(attribute_value(symbol, "font"));
  const std::string code = attribute_value(symbol, "char");
  if (code.empty() || code.size() > 6 || !std::all_of(code.begin(), code.end(), [](const unsigned char byte) {
        return std::isxdigit(byte) != 0;
      })) {
    return {};
  }
  std::uint32_t value = static_cast<std::uint32_t>(std::stoul(code, nullptr, 16));
  if (value >= 0xF000 && value <= 0xF0FF) value -= 0xF000;
  else if (value >= 0x100 || !is_symbol_font(font)) return value >= 0xE000 && value <= 0xF8FF ? std::string() : utf8_of(value);
  if (font.rfind("wingdings 2", 0) == 0) {
    switch (value) {
      case 0x50: return "\xE2\x9C\x93";  // ✓
      case 0x4F: return "\xE2\x9C\x97";  // ✗
      case 0x52: return "\xE2\x98\x91";  // ☑
      case 0x54: return "\xE2\x98\x92";  // ☒
      case 0xA3: return "\xE2\x98\x90";  // ☐
      default: return {};
    }
  }
  if (font.rfind("wingdings", 0) == 0) {
    switch (value) {
      case 0x6C: return "\xE2\x97\x8F";  // ●
      case 0x6E: return "\xE2\x96\xA0";  // ■
      case 0x6F: return "\xE2\x96\xA1";  // □
      case 0x71: return "\xE2\x9D\x91";  // ❑
      case 0xA7: return "\xE2\x96\xAA";  // ▪
      case 0xD8: return "\xE2\x9E\xA2";  // ➢
      case 0xFB: return "\xE2\x9C\x97";  // ✗
      case 0xFC: return "\xE2\x9C\x93";  // ✓
      case 0xFD: return "\xE2\x98\x92";  // ☒
      case 0xFE: return "\xE2\x98\x91";  // ☑
      default: return {};
    }
  }
  if (font == "symbol") {
    switch (value) {
      case 0xB7: return "\xE2\x80\xA2";  // •
      case 0xB0: return "\xC2\xB0";      // °
      case 0xB1: return "\xC2\xB1";      // ±
      case 0xD7: return "\xC3\x97";      // ×
      case 0xA3: return "\xE2\x89\xA4";  // ≤
      case 0xB3: return "\xE2\x89\xA5";  // ≥
      default: return {};
    }
  }
  return {};
}

// The text of a text box or phonetic guide inside a run, in stretches: each
// stretch the words one tracked change inserted or struck, or words no change
// touches. A price the other side struck and rewrote inside a text box is a
// proposal like any other, and is read as one. Where the document offers the
// same content twice - a modern drawing and an older fallback picture of it -
// only the modern one is read, as the writer reads it, so no text is shown
// twice.
struct EmbeddedStretch {
  std::optional<DocumentRenderRevision> revision;
  std::string text;
};

void append_embedded_text(const pugi::xml_node &node, const std::optional<DocumentRenderRevision> &revision,
                          std::vector<EmbeddedStretch> &stretches, bool &paragraphEnded) {
  const auto add = [&](const std::string &text) {
    if (text.empty()) return;
    // A paragraph of the box begins on a line of its own.
    std::string written = text;
    if (paragraphEnded) {
      const bool anyText = std::any_of(stretches.begin(), stretches.end(),
                                       [](const EmbeddedStretch &stretch) { return !stretch.text.empty(); });
      if (anyText) written.insert(0, "\n");
      paragraphEnded = false;
    }
    const bool sameChange = !stretches.empty() && stretches.back().revision.has_value() == revision.has_value()
        && (!revision || (stretches.back().revision->kind == revision->kind
                          && stretches.back().revision->author == revision->author));
    if (sameChange) stretches.back().text += written;
    else stretches.push_back({revision, written});
  };
  for (const auto &child : node.children()) {
    if (child.type() != pugi::node_element) continue;
    const std::string_view name = local_name(child.name());
    if (name == "Fallback" || name == "instrText" || name == "rt") continue;
    if (name == "AlternateContent") {
      const auto choice = child_element(child, "Choice");
      append_embedded_text(choice ? choice : child_element(child, "Fallback"), revision, stretches, paragraphEnded);
      continue;
    }
    if (const auto kind = revision_kind(name)) {
      append_embedded_text(child, read_revision(child, *kind), stretches, paragraphEnded);
      continue;
    }
    if (name == "p") {
      paragraphEnded = true;
      append_embedded_text(child, revision, stretches, paragraphEnded);
      paragraphEnded = true;
      continue;
    }
    if (name == "t" || name == "delText") {
      add(child.child_value());
      continue;
    }
    if (name == "tab") {
      add("\t");
      continue;
    }
    if (name == "br" || name == "cr") {
      add("\n");
      continue;
    }
    append_embedded_text(child, revision, stretches, paragraphEnded);
  }
}

void render_run(const pugi::xml_node &node, RenderContext &context, const InlineContext &inherited,
                std::vector<DocumentRenderRun> &runs) {
  const auto runProperties = child_element(node, "rPr");
  const StyleRunFormat own = read_run_format(runProperties);
  const auto runStyle = child_element(runProperties, "rStyle");
  const StyleRunFormat character = runStyle
      ? resolve_character_format(attribute_value(runStyle, "val"), context.layout) : StyleRunFormat{};
  const StyleRunFormat &paragraph = inherited.paragraph_format;
  // The run's own formatting first, then the character style it names, then
  // its paragraph's style. (The paragraph mark's own formatting, w:pPr/w:rPr,
  // belongs to the pilcrow alone and is not inherited by the text.)
  const auto mark = [](int ownValue, int styleValue, int paragraphValue) {
    if (ownValue >= 0) return ownValue == 1;
    if (styleValue >= 0) return styleValue == 1;
    return paragraphValue == 1;
  };
  const auto first = [](const std::string &ownValue, const std::string &styleValue,
                        const std::string &paragraphValue) {
    return !ownValue.empty() ? ownValue : !styleValue.empty() ? styleValue : paragraphValue;
  };
  DocumentRenderRun format;
  format.bold = mark(own.bold, character.bold, paragraph.bold);
  format.italic = mark(own.italic, character.italic, paragraph.italic);
  format.underline = mark(own.underline, character.underline, paragraph.underline);
  format.strikethrough = mark(own.strike, character.strike, paragraph.strike);
  format.caps = mark(own.caps, character.caps, paragraph.caps);
  format.small_caps = mark(own.small_caps, character.small_caps, paragraph.small_caps);
  const std::string vertAlign = first(own.vert_align, character.vert_align, paragraph.vert_align);
  if (vertAlign == "superscript" || vertAlign == "subscript") format.vert_align = vertAlign;
  format.color_hex = first(own.color_hex, character.color_hex, paragraph.color_hex);
  const auto highlight = child_element(runProperties, "highlight");
  if (highlight) {
    const std::string value = attribute_value(highlight, "val");
    if (value != "none") format.highlight = value;
  }
  format.half_points = own.half_points != 0 ? own.half_points
                       : character.half_points != 0 ? character.half_points : paragraph.half_points;
  format.revision = inherited.revision;
  // Words struck or brought in by a change are that change: how they are
  // written is part of it, and is not said as a change of its own.
  if (!format.revision) format.format_revision = run_format_revision(runProperties);
  if (format.format_revision) {
    // How the run was written before the change, resolved the way its marks
    // now are: what the page drawn as it stood, every change rejected, shows.
    const auto was = child_element(child_element(runProperties, "rPrChange"), "rPr");
    const StyleRunFormat wasOwn = read_run_format(was);
    const auto wasStyle = child_element(was, "rStyle");
    const StyleRunFormat wasCharacter = wasStyle
        ? resolve_character_format(attribute_value(wasStyle, "val"), context.layout) : StyleRunFormat{};
    DocumentRenderRunMarks marks;
    marks.bold = mark(wasOwn.bold, wasCharacter.bold, paragraph.bold);
    marks.italic = mark(wasOwn.italic, wasCharacter.italic, paragraph.italic);
    marks.underline = mark(wasOwn.underline, wasCharacter.underline, paragraph.underline);
    marks.strikethrough = mark(wasOwn.strike, wasCharacter.strike, paragraph.strike);
    marks.caps = mark(wasOwn.caps, wasCharacter.caps, paragraph.caps);
    marks.small_caps = mark(wasOwn.small_caps, wasCharacter.small_caps, paragraph.small_caps);
    const std::string wasVertAlign = first(wasOwn.vert_align, wasCharacter.vert_align, paragraph.vert_align);
    if (wasVertAlign == "superscript" || wasVertAlign == "subscript") marks.vert_align = wasVertAlign;
    format.format_revision->was_run = marks;
  }
  format.comment_ids = context.story.open_comments;

  DocumentRenderRun current = format;
  const auto append = [&](const std::string &text) {
    if (text.empty() || in_field_instruction(context)) return;
    if (current.text.empty()) {
      const std::string fieldLink = current_field_link(context);
      current.link = !fieldLink.empty() ? fieldLink : inherited.link;
    }
    current.text += text;
  };
  const auto flush = [&]() {
    if (!current.text.empty()) runs.push_back(current);
    current = format;
  };
  for (const auto &child : node.children()) {
    if (child.type() != pugi::node_element) continue;
    const std::string_view name = local_name(child.name());
    if (name == "t" || name == "delText") {
      append(child.child_value());
    } else if (name == "tab" || name == "ptab") {
      append("\t");
    } else if (name == "br") {
      if (attribute_value(child, "type") == "page") {
        flush();
        DocumentRenderRun pageBreak = format;
        pageBreak.page_break = true;
        runs.push_back(std::move(pageBreak));
      } else {
        append("\n");
      }
    } else if (name == "cr") {
      append("\n");
    } else if (name == "noBreakHyphen") {
      append("\xE2\x80\x91");
    } else if (name == "softHyphen") {
      append("\xC2\xAD");
    } else if (name == "sym") {
      append(symbol_text(child));
    } else if (name == "footnoteReference" || name == "endnoteReference") {
      const bool footnote = name == "footnoteReference";
      const std::string id = attribute_value(child, "id");
      NoteNumbers &numbers = footnote ? context.document.footnotes : context.document.endnotes;
      auto found = numbers.number.find(id);
      if (found == numbers.number.end()) {
        numbers.order.push_back(id);
        found = numbers.number.emplace(id, static_cast<unsigned int>(numbers.order.size())).first;
      }
      flush();
      DocumentRenderRun reference = format;
      reference.is_note_reference = true;
      // Footnotes and endnotes are numbered separately in the package, so
      // their ids are named with their kind to stay distinct.
      reference.note_id = (footnote ? "footnote-" : "endnote-") + id;
      reference.note_number = found->second;
      runs.push_back(std::move(reference));
    } else if (name == "commentReference") {
      const std::string id = attribute_value(child, "id");
      note_comment(context, id);
      // A comment with a range is already on the text it covers; one with
      // no range is anchored here, at a point.
      if (!id.empty() && context.story.ranged_comments.count(id) == 0) {
        flush();
        DocumentRenderRun point = format;
        point.comment_ids = {id};
        runs.push_back(std::move(point));
      }
    } else if (name == "fldChar") {
      const std::string type = attribute_value(child, "fldCharType");
      auto &fields = context.story.fields;
      if (type == "begin") {
        fields.emplace_back();
      } else if (type == "separate" && !fields.empty()) {
        fields.back().in_result = true;
        fields.back().link = field_hyperlink(fields.back().instruction);
      } else if (type == "end" && !fields.empty()) {
        fields.pop_back();
      }
    } else if (name == "instrText") {
      if (!context.story.fields.empty() && !context.story.fields.back().in_result)
        context.story.fields.back().instruction += child.child_value();
    } else if (name == "AlternateContent" || name == "drawing" || name == "pict" || name == "object"
               || name == "ruby") {
      std::vector<EmbeddedStretch> stretches;
      bool paragraphEnded = false;
      append_embedded_text(child, std::nullopt, stretches, paragraphEnded);
      bool first = true;
      for (EmbeddedStretch &stretch : stretches) {
        if (stretch.text.empty()) continue;
        // A box stands on a line of its own after the words before it.
        if (first && name != "ruby" && (!current.text.empty() || !runs.empty())) stretch.text.insert(0, "\n");
        first = false;
        // Each change inside the box is read as the change it is; words no
        // change touches take what the run itself stands in.
        const std::optional<DocumentRenderRevision> revision = stretch.revision ? stretch.revision : format.revision;
        const bool sameRevision = current.revision.has_value() == revision.has_value()
            && (!revision || (current.revision->kind == revision->kind && current.revision->id == revision->id
                              && current.revision->author == revision->author));
        if (!sameRevision) {
          flush();
          current.revision = revision;
        }
        append(stretch.text);
      }
      const bool backToRun = current.revision.has_value() == format.revision.has_value()
          && (!format.revision || (current.revision->kind == format.revision->kind
                                   && current.revision->id == format.revision->id));
      if (!backToRun) flush();
    }
    // Everything else in a run - its properties, the automatic number of a
    // note or a comment standing in the note itself, a page break Word only
    // remembers from its last layout - is not text a reader is shown.
  }
  flush();
}

void render_inline(const pugi::xml_node &node, RenderContext &context, const InlineContext &inherited,
                   std::vector<DocumentRenderRun> &runs) {
  for (const auto &child : node.children()) {
    if (child.type() != pugi::node_element) continue;
    const std::string_view name = local_name(child.name());
    if (name == "r") {
      render_run(child, context, inherited, runs);
      continue;
    }
    if (const auto kind = revision_kind(name)) {
      InlineContext inner = inherited;
      inner.revision = read_revision(child, *kind);
      render_inline(child, context, inner, runs);
      continue;
    }
    if (name == "hyperlink") {
      InlineContext inner = inherited;
      inner.link = hyperlink_target(child, context);
      render_inline(child, context, inner, runs);
      continue;
    }
    if (name == "fldSimple") {
      InlineContext inner = inherited;
      const std::string link = field_hyperlink(attribute_value(child, "instr"));
      if (!link.empty()) inner.link = link;
      render_inline(child, context, inner, runs);
      continue;
    }
    if (name == "commentRangeStart") {
      open_comment(context, attribute_value(child, "id"));
      continue;
    }
    if (name == "commentRangeEnd") {
      close_comment(context, attribute_value(child, "id"));
      continue;
    }
    if (name == "AlternateContent") {
      const auto choice = child_element(child, "Choice");
      render_inline(choice ? choice : child_element(child, "Fallback"), context, inherited, runs);
      continue;
    }
    if (name == "pPr" || name == "rPr" || name == "sdtPr" || name == "sdtEndPr" || name == "Fallback") continue;
    // A smart tag, a content control, a custom XML element: containers that
    // hold ordinary runs. Their content is the paragraph's text.
    render_inline(child, context, inherited, runs);
  }
}

// The list a paragraph is in: named on the paragraph itself, else by its style
// or a style that one is built on. numId 0 says "not in a list" and ends the
// search.
struct ParagraphList {
  bool found = false;
  std::string num_id;
  std::size_t level = 0;
};

ParagraphList paragraph_list(const pugi::xml_node &properties, const std::string &styleId,
                             const RenderContext &context) {
  std::optional<std::string> numId;
  std::optional<std::size_t> level;
  const auto numbering = child_element(properties, "numPr");
  if (numbering) {
    const auto id = child_element(numbering, "numId");
    if (id) numId = attribute_value(id, "val");
    const auto ilvl = child_element(numbering, "ilvl");
    const auto parsed = ilvl ? parse_int(attribute_value(ilvl, "val")) : std::nullopt;
    if (parsed && *parsed >= 0 && *parsed < static_cast<int>(kListLevels)) level = static_cast<std::size_t>(*parsed);
  }
  each_paragraph_style(styleId, context.layout, [&](const std::string &, const ParagraphStyleLayout &style) {
    if (!numId && style.names_list) numId = style.num_id;
    if (!level && style.list_level) level = *style.list_level;
    return !numId || !level;
  });
  ParagraphList list;
  if (!numId || numId->empty() || *numId == "0") return list;
  list.found = true;
  list.num_id = *numId;
  if (level) {
    list.level = *level;
    return list;
  }
  // A style that names a list but not a level stands at the level that names
  // the style (w:lvl/w:pStyle) - how Word ties "Heading 2" to "1.1".
  const ResolvedList resolved = resolve_list(list.num_id, context.numbering, context.layout);
  for (std::size_t index = 0; index < kListLevels; ++index) {
    const ListLevel *definition = resolved.level(index);
    if (definition != nullptr && !definition->style.empty() && definition->style == styleId) {
      list.level = index;
      break;
    }
  }
  return list;
}

DocumentRenderParagraph render_paragraph(const pugi::xml_node &node, RenderContext &context,
                                         const std::optional<DocumentRenderRevision> &enclosing) {
  DocumentRenderParagraph paragraph;
  paragraph.heading_level = paragraph_heading_level(node, context.styles);
  const auto properties = child_element(node, "pPr");
  const auto style = child_element(properties, "pStyle");
  if (style) paragraph.style_id = attribute_value(style, "val");
  // A paragraph that names no style - or a style the document does not define
  // - is written in the document's default paragraph style ("Normal"), and
  // takes what that style says, as in Word.
  const std::string styleId = paragraph.style_id.empty() || context.layout.paragraphs.count(paragraph.style_id) == 0
      ? context.layout.default_paragraph : paragraph.style_id;

  const auto alignment = child_element(properties, "jc");
  if (alignment) {
    paragraph.alignment = normalized_alignment(attribute_value(alignment, "val"));
  } else {
    each_paragraph_style(styleId, context.layout, [&](const std::string &, const ParagraphStyleLayout &layout) {
      paragraph.alignment = layout.alignment;
      return paragraph.alignment.empty();
    });
  }
  const auto indent = child_element(properties, "ind");
  if (indent) {
    unsigned int twips = parse_measure(attribute_value(indent, "left"));
    if (twips == 0) twips = parse_measure(attribute_value(indent, "start"));
    // Word's own default tab is 720 twips; anything shorter than half of one
    // is the typographic breathing room of a style, not a level of nesting.
    paragraph.indent_level = twips >= 360 ? twips / 720 + (twips % 720 >= 360 ? 1 : 0) : 0;
    if (paragraph.indent_level > 8) paragraph.indent_level = 8;
  }

  IndentParts listIndent;
  const ParagraphList list = paragraph_list(properties, styleId, context);
  if (list.found) {
    paragraph.in_list = true;
    paragraph.list_id = list.num_id;
    paragraph.list_level = static_cast<unsigned int>(list.level);
    const ResolvedList resolved = resolve_list(list.num_id, context.numbering, context.layout);
    const ListLevel *definition = resolved.level(list.level);
    // A list whose format Singularity could not read is shown with a bullet: a
    // mark that claims nothing, rather than a number that would claim a
    // position in a sequence.
    paragraph.list_bullet = definition == nullptr || definition->format == "bullet" || definition->format == "none";
    if (definition != nullptr) {
      paragraph.list_marker = next_list_marker(resolved, list.level, context.story.numbering);
      listIndent = definition->indent;
    }
  }

  // Where it stands: its own indentation, else its list level's, else its
  // style's, else the document's default - each part decided on its own.
  IndentParts resolvedIndent = read_indent(indent);
  fill_missing(resolvedIndent, listIndent);
  SpacingParts spacing = read_spacing(child_element(properties, "spacing"));
  int keepNext = toggle_of(properties, "keepNext");
  int pageBreakBefore = toggle_of(properties, "pageBreakBefore");
  each_paragraph_style(styleId, context.layout, [&](const std::string &id, const ParagraphStyleLayout &layout) {
    fill_missing(resolvedIndent, layout.indent);
    if (!spacing.before) spacing.before = layout.spacing.before;
    if (!spacing.after) spacing.after = layout.spacing.after;
    if (keepNext < 0) keepNext = layout.keep_next;
    if (pageBreakBefore < 0) pageBreakBefore = layout.page_break_before;
    if (id == "Title" || lowercase(layout.name) == "title") paragraph.is_title = true;
    return true;
  });
  // Inside a table, its style speaks next - in Word's order, the document's
  // defaults, then the table style, then the paragraph's style, then the
  // paragraph itself.
  {
    std::unordered_set<std::string> visited;
    std::string current = context.table_style;
    while (!current.empty() && visited.insert(current).second && (!spacing.before || !spacing.after)) {
      const auto found = context.layout.tables.find(current);
      if (found == context.layout.tables.end()) break;
      if (!spacing.before) spacing.before = found->second.spacing.before;
      if (!spacing.after) spacing.after = found->second.spacing.after;
      current = found->second.based_on;
    }
  }
  fill_missing(resolvedIndent, context.layout.default_indent);
  if (!spacing.before) spacing.before = context.layout.default_spacing.before;
  if (!spacing.after) spacing.after = context.layout.default_spacing.after;
  paragraph.indent_left_twips = resolvedIndent.left;
  paragraph.indent_first_line_twips = resolvedIndent.first_line;
  paragraph.indent_right_twips = resolvedIndent.right;
  paragraph.spacing_before_twips = spacing.before;
  paragraph.spacing_after_twips = spacing.after;
  paragraph.keep_next = keepNext == 1;
  paragraph.page_break_before = pageBreakBefore == 1;

  // The paragraph mark's own revision: a paragraph inserted or deleted as a
  // whole. A deletion recorded on the mark outweighs an insertion there - the
  // paragraph was added and then taken out again.
  const auto markProperties = child_element(properties, "rPr");
  for (const auto &child : markProperties.children()) {
    if (child.type() != pugi::node_element) continue;
    const auto kind = revision_kind(local_name(child.name()));
    if (!kind) continue;
    if (!paragraph.revision || *kind == DocumentRenderRevisionKind::Deleted) paragraph.revision = read_revision(child, *kind);
  }
  if (!paragraph.revision) paragraph.revision = enclosing;
  if (!paragraph.revision) paragraph.format_revision = paragraph_format_revision(properties, context.layout);
  if (paragraph.format_revision) {
    // Its alignment as it stood - its own, else its former style's - for the
    // page drawn with every change rejected.
    const auto was = child_element(child_element(properties, "pPrChange"), "pPr");
    std::string wasAlignment;
    const auto wasJc = child_element(was, "jc");
    if (wasJc) {
      wasAlignment = normalized_alignment(attribute_value(wasJc, "val"));
    } else {
      const std::string wasStyle = attribute_value(child_element(was, "pStyle"), "val");
      const std::string wasStyleId = wasStyle.empty() || context.layout.paragraphs.count(wasStyle) == 0
          ? context.layout.default_paragraph : wasStyle;
      each_paragraph_style(wasStyleId, context.layout, [&](const std::string &, const ParagraphStyleLayout &layout) {
        wasAlignment = layout.alignment;
        return wasAlignment.empty();
      });
    }
    paragraph.format_revision->was_alignment = wasAlignment;
  }

  InlineContext inherited;
  inherited.paragraph_format = resolve_style_run_format(styleId, context.styles);
  inherited.revision = enclosing;
  render_inline(node, context, inherited, paragraph.runs);
  return paragraph;
}

// The rows of a table and the cells of a row, including those a content
// control or a custom XML element is wrapped round.
void collect_table_children(const pugi::xml_node &node, std::string_view name, std::vector<pugi::xml_node> &found) {
  for (const auto &child : node.children()) {
    if (is_element(child, name)) found.push_back(child);
    else if (is_element(child, "sdt")) collect_table_children(child_element(child, "sdtContent"), name, found);
    else if (is_element(child, "customXml")) collect_table_children(child, name, found);
  }
}

void render_blocks(const pugi::xml_node &container, RenderContext &context,
                   const std::optional<DocumentRenderRevision> &revision, std::vector<DocumentRenderBlock> &blocks);

DocumentRenderTable render_table(const pugi::xml_node &tableNode, RenderContext &context,
                                 const std::optional<DocumentRenderRevision> &revision) {
  DocumentRenderTable table;
  if (context.story.table_ids != nullptr) {
    // The identity a locator and the writer give this very table: counted
    // over every table in the body, a table inside a cell included, in the
    // order collect_tbl_nodes() meets them.
    const auto found = context.story.table_ids->find(tableNode.internal_object());
    if (found != context.story.table_ids->end()) table.table_id = "table-" + std::to_string(found->second);
  }
  const auto grid = child_element(tableNode, "tblGrid");
  if (grid) {
    for (const auto &column : grid.children()) {
      if (!is_element(column, "gridCol")) continue;
      table.column_widths_twips.push_back(parse_measure(attribute_value(column, "w")));
    }
  }
  table.has_borders = table_has_borders(child_element(tableNode, "tblPr"), context.layout);
  const auto tableStyle = child_element(child_element(tableNode, "tblPr"), "tblStyle");
  const std::string outerTableStyle = context.table_style;
  context.table_style = tableStyle ? attribute_value(tableStyle, "val") : context.layout.default_table;

  std::vector<pugi::xml_node> rowNodes;
  collect_table_children(tableNode, "tr", rowNodes);
  // Which cells begin a vertical merge, row by row: how far each reaches down
  // is known only once the rows below it are read.
  std::vector<std::vector<bool>> mergeStarts;
  for (const pugi::xml_node &rowNode : rowNodes) {
    DocumentRenderRow row;
    row.row_index = table.rows.size();
    const auto rowProperties = child_element(rowNode, "trPr");
    const auto header = child_element(rowProperties, "tblHeader");
    row.is_header = header && on_off_is_on(header);
    for (const auto &child : rowProperties.children()) {
      if (child.type() != pugi::node_element) continue;
      const auto kind = revision_kind(local_name(child.name()));
      if (kind && (!row.revision || *kind == DocumentRenderRevisionKind::Deleted)) row.revision = read_revision(child, *kind);
    }
    if (!row.revision) row.revision = revision;
    // Grid columns a row leaves empty before its first cell.
    const auto gridBefore = child_element(rowProperties, "gridBefore");
    std::size_t columnIndex = gridBefore ? parse_measure(attribute_value(gridBefore, "val")) : 0;
    std::vector<pugi::xml_node> cellNodes;
    collect_table_children(rowNode, "tc", cellNodes);
    std::vector<bool> starts;
    for (const pugi::xml_node &cellNode : cellNodes) {
      DocumentRenderCell cell;
      cell.column_index = columnIndex;
      cell.grid_span = cell_grid_span(cellNode);
      cell.is_merge_continuation = cell_is_vertical_merge_continuation(cellNode);
      cell.shading_fill_hex = cell_shading_evidence(cellNode).fill_hex;
      const auto verticalMerge = child_element(child_element(cellNode, "tcPr"), "vMerge");
      starts.push_back(verticalMerge && attribute_value(verticalMerge, "val") == "restart");
      render_blocks(cellNode, context, revision, cell.blocks);
      // The cell itself given another look in a tracked change - Singularity's
      // own risk shading among them: said on its words, which are the same.
      if (auto cellChange = cell_format_revision(cellNode)) {
        for (DocumentRenderBlock &block : cell.blocks) {
          if (block.kind != DocumentRenderBlockKind::Paragraph) continue;
          DocumentRenderParagraph &paragraph = block.paragraph;
          if (!paragraph.revision && !paragraph.format_revision) paragraph.format_revision = cellChange;
        }
      }
      for (const DocumentRenderBlock &block : cell.blocks)
        if (block.kind == DocumentRenderBlockKind::Paragraph) cell.paragraphs.push_back(block.paragraph);
      columnIndex += cell.grid_span;
      row.cells.push_back(std::move(cell));
    }
    mergeStarts.push_back(std::move(starts));
    table.rows.push_back(std::move(row));
  }
  context.table_style = outerTableStyle;
  for (std::size_t rowIndex = 0; rowIndex < table.rows.size(); ++rowIndex) {
    for (std::size_t cellIndex = 0; cellIndex < table.rows[rowIndex].cells.size(); ++cellIndex) {
      if (!mergeStarts[rowIndex][cellIndex]) continue;
      DocumentRenderCell &anchor = table.rows[rowIndex].cells[cellIndex];
      std::size_t span = 1;
      for (std::size_t below = rowIndex + 1; below < table.rows.size(); ++below) {
        const auto &cells = table.rows[below].cells;
        const auto continuation = std::find_if(cells.begin(), cells.end(), [&](const DocumentRenderCell &cell) {
          return cell.column_index == anchor.column_index;
        });
        if (continuation == cells.end() || !continuation->is_merge_continuation) break;
        ++span;
      }
      anchor.row_span = span;
    }
  }
  return table;
}

void render_blocks(const pugi::xml_node &container, RenderContext &context,
                   const std::optional<DocumentRenderRevision> &revision, std::vector<DocumentRenderBlock> &blocks) {
  for (const auto &child : container.children()) {
    if (child.type() != pugi::node_element) continue;
    const std::string_view name = local_name(child.name());
    if (name == "p") {
      DocumentRenderBlock block;
      block.kind = DocumentRenderBlockKind::Paragraph;
      block.paragraph = render_paragraph(child, context, revision);
      blocks.push_back(std::move(block));
      continue;
    }
    if (name == "tbl") {
      DocumentRenderBlock block;
      block.kind = DocumentRenderBlockKind::Table;
      block.table = render_table(child, context, revision);
      blocks.push_back(std::move(block));
      continue;
    }
    // A tracked change wrapped round whole paragraphs or tables.
    if (const auto kind = revision_kind(name)) {
      render_blocks(child, context, read_revision(child, *kind), blocks);
      continue;
    }
    if (name == "sdt") {
      render_blocks(child_element(child, "sdtContent"), context, revision, blocks);
      continue;
    }
    // A custom XML element holds ordinary body content.
    if (name == "customXml" || name == "sdtContent") {
      render_blocks(child, context, revision, blocks);
      continue;
    }
    if (name == "AlternateContent") {
      const auto choice = child_element(child, "Choice");
      render_blocks(choice ? choice : child_element(child, "Fallback"), context, revision, blocks);
      continue;
    }
    if (name == "commentRangeStart") open_comment(context, attribute_value(child, "id"));
    else if (name == "commentRangeEnd") close_comment(context, attribute_value(child, "id"));
  }
}

// A story outside the body - a header, a footer, a note, a comment - as the
// paragraphs a reader is shown, a table in it read cell by cell.
void append_paragraphs(const std::vector<DocumentRenderBlock> &blocks,
                       std::vector<DocumentRenderParagraph> &paragraphs) {
  for (const DocumentRenderBlock &block : blocks) {
    if (block.kind == DocumentRenderBlockKind::Paragraph) {
      paragraphs.push_back(block.paragraph);
      continue;
    }
    for (const DocumentRenderRow &row : block.table.rows)
      for (const DocumentRenderCell &cell : row.cells) append_paragraphs(cell.blocks, paragraphs);
  }
}

std::vector<DocumentRenderParagraph> render_story(const pugi::xml_node &root, RenderContext context) {
  std::vector<DocumentRenderBlock> blocks;
  render_blocks(root, context, std::nullopt, blocks);
  std::vector<DocumentRenderParagraph> paragraphs;
  append_paragraphs(blocks, paragraphs);
  return paragraphs;
}

// Every section's properties in order: those that end a section inside the
// body (on a paragraph) and then the body's own, which is the last.
std::vector<pugi::xml_node> section_properties(const pugi::xml_node &body) {
  std::vector<pugi::xml_node> sections;
  for (const auto &child : body.children()) {
    if (is_element(child, "p")) {
      const auto section = child_element(child_element(child, "pPr"), "sectPr");
      if (section) sections.push_back(section);
    } else if (is_element(child, "sectPr")) {
      sections.push_back(child);
    }
  }
  return sections;
}

std::optional<DocumentRenderPage> read_page(const pugi::xml_node &section) {
  const auto size = child_element(section, "pgSz");
  if (!size) return std::nullopt;
  const auto width = parse_twips(attribute_value(size, "w"));
  const auto height = parse_twips(attribute_value(size, "h"));
  if (!width || !height || *width <= 0 || *height <= 0) return std::nullopt;
  DocumentRenderPage page;
  page.width_twips = static_cast<unsigned int>(*width);
  page.height_twips = static_cast<unsigned int>(*height);
  const std::string orientation = attribute_value(size, "orient");
  page.orientation = orientation == "landscape" || (orientation.empty() && *width > *height) ? "landscape" : "portrait";
  // Word's own margin, an inch, where a page states its size and not its
  // margins.
  const auto margins = child_element(section, "pgMar");
  page.margin_top_twips = parse_twips(attribute_value(margins, "top")).value_or(1440);
  page.margin_right_twips = parse_twips(attribute_value(margins, "right")).value_or(1440);
  page.margin_bottom_twips = parse_twips(attribute_value(margins, "bottom")).value_or(1440);
  page.margin_left_twips = parse_twips(attribute_value(margins, "left")).value_or(1440);
  return page;
}

// The relationship id of the header or footer of one kind ("default", "first"
// for the first page, "even" for even pages) of the last section. A section
// that names none shows the one of the section before it, as in Word.
std::string story_reference(const std::vector<pugi::xml_node> &sections, std::string_view element,
                            std::string_view kind) {
  for (auto section = sections.rbegin(); section != sections.rend(); ++section) {
    for (const auto &child : section->children()) {
      if (!is_element(child, element)) continue;
      std::string type = attribute_value(child, "type");
      if (type.empty()) type = "default";
      if (type == kind) return attribute_value(child, "id");
    }
  }
  return {};
}

std::string default_story_reference(const std::vector<pugi::xml_node> &sections, std::string_view element) {
  return story_reference(sections, element, "default");
}

// A header or footer part, read as paragraphs with its own relationships.
std::vector<DocumentRenderParagraph> render_story_part(zip_t *archive, const Relationships &documentRelationships,
                                                       const std::string &relationshipId,
                                                       const RenderContext &base) {
  if (relationshipId.empty()) return {};
  const auto found = documentRelationships.find(relationshipId);
  if (found == documentRelationships.end() || found->second.external) return {};
  const std::string partName = resolve_part_name("word/document.xml", found->second.target);
  pugi::xml_document part;
  if (!read_optional_part(archive, partName, part)) return {};
  const Relationships relationships = read_relationships(archive, partName);
  RenderContext context{base.styles, base.layout, base.numbering, base.document, StoryReading{}};
  context.story.relationships = &relationships;
  for (const auto &root : part.children())
    if (root.type() == pugi::node_element) return render_story(root, context);
  return {};
}

// The notes the body refers to, in the order of their numbers. A separator
// line and its continuation are part of the notes' layout, not notes.
void render_notes(zip_t *archive, const std::string &partName, DocumentRenderNoteKind kind,
                  const NoteNumbers &numbers, const RenderContext &base, std::vector<DocumentRenderNote> &notes) {
  if (numbers.order.empty()) return;
  pugi::xml_document part;
  if (!read_optional_part(archive, partName, part)) return;
  const Relationships relationships = read_relationships(archive, partName);
  const std::string_view element = kind == DocumentRenderNoteKind::Footnote ? "footnote" : "endnote";
  std::unordered_map<std::string, pugi::xml_node> byId;
  for (const auto &root : part.children()) {
    for (const auto &note : root.children()) {
      if (!is_element(note, element)) continue;
      const std::string type = attribute_value(note, "type");
      if (type == "separator" || type == "continuationSeparator" || type == "continuationNotice") continue;
      byId.emplace(attribute_value(note, "id"), note);
    }
  }
  for (const std::string &id : numbers.order) {
    const auto found = byId.find(id);
    if (found == byId.end()) continue;
    RenderContext context{base.styles, base.layout, base.numbering, base.document, StoryReading{}};
    context.story.relationships = &relationships;
    DocumentRenderNote note;
    note.id = (kind == DocumentRenderNoteKind::Footnote ? "footnote-" : "endnote-") + id;
    note.kind = kind;
    note.number = numbers.number.at(id);
    note.paragraphs = render_story(found->second, context);
    notes.push_back(std::move(note));
  }
}

// Every comment in the package, in the order its mark stands in the
// document; a comment whose mark was not met is listed after them.
void render_comments(zip_t *archive, const std::string &partName, const RenderContext &base,
                     std::vector<DocumentRenderComment> &comments) {
  pugi::xml_document part;
  if (!read_optional_part(archive, partName, part)) return;
  const Relationships relationships = read_relationships(archive, partName);
  std::vector<pugi::xml_node> inFileOrder;
  for (const auto &root : part.children())
    for (const auto &comment : root.children())
      if (is_element(comment, "comment")) inFileOrder.push_back(comment);
  std::unordered_map<std::string, std::size_t> position;
  for (std::size_t index = 0; index < base.document.comment_order.size(); ++index)
    position.emplace(base.document.comment_order[index], index);
  std::stable_sort(inFileOrder.begin(), inFileOrder.end(), [&](const pugi::xml_node &left, const pugi::xml_node &right) {
    const auto leftFound = position.find(attribute_value(left, "id"));
    const auto rightFound = position.find(attribute_value(right, "id"));
    const std::size_t leftAt = leftFound == position.end() ? position.size() : leftFound->second;
    const std::size_t rightAt = rightFound == position.end() ? position.size() : rightFound->second;
    return leftAt < rightAt;
  });
  for (const pugi::xml_node &node : inFileOrder) {
    RenderContext context{base.styles, base.layout, base.numbering, base.document, StoryReading{}};
    context.story.relationships = &relationships;
    DocumentRenderComment comment;
    comment.id = attribute_value(node, "id");
    comment.author = attribute_value(node, "author");
    comment.initials = attribute_value(node, "initials");
    comment.date = attribute_value(node, "date");
    comment.paragraphs = render_story(node, context);
    comments.push_back(std::move(comment));
  }
}

// A header or footer beside the page's own: the one a document gives its
// first page, or its even pages - "STRICTLY CONFIDENTIAL" is often on the first
// page alone.
struct LabelledStory {
  std::string label;
  std::vector<DocumentRenderParagraph> paragraphs;
};

// The whole package read the way a person is shown it: the body, its page, the
// default header and footer, the notes the body refers to and the comments -
// and, where `otherStories` is given, each first-page and even-page header and
// footer the document has besides. Throws DocxError where the package cannot be
// read at all; `stylesMissing` says whether it had no styles part, so headings
// could only come from the paragraphs themselves.
void read_docx_package(const DocumentExtractionRequest &request, DocumentRenderModel &model, bool &stylesMissing,
                       std::vector<LabelledStory> *otherStories = nullptr) {
  ZipArchive archive(request.path);
  const auto inventory = inspect_archive(archive.get(), request.maximum_input_bytes);
  const auto document_bytes = read_entry(archive.get(), inventory.document_index,
                                         inventory.document_size, kMaximumDocumentXmlBytes);
  auto document = parse_xml(document_bytes, "docx_malformed_document_xml",
                            "word/document.xml is malformed.");
  StyleMap styles;
  LayoutStyles layout;
  stylesMissing = !inventory.styles_index.has_value();
  if (inventory.styles_index.has_value()) {
    const auto styles_bytes = read_entry(archive.get(), *inventory.styles_index,
                                         inventory.styles_size, kMaximumStylesXmlBytes);
    auto styles_document = parse_xml(styles_bytes, "docx_malformed_styles_xml",
                                     "word/styles.xml is malformed.");
    collect_styles(styles_document, styles);
    collect_layout_styles(styles_document, layout);
  }
  NumberingDefinitions numbering;
  if (inventory.numbering_index.has_value()) {
    try {
      const auto numbering_bytes = read_entry(archive.get(), *inventory.numbering_index,
                                              inventory.numbering_size, kMaximumStylesXmlBytes);
      auto numbering_document = parse_xml(numbering_bytes, "docx_malformed_numbering_xml",
                                          "word/numbering.xml is malformed.");
      collect_numbering(numbering_document, numbering);
    } catch (const DocxError &) {
      // A document whose numbering part cannot be read still reads
      // perfectly well; its lists simply carry a bullet.
    }
  }
  pugi::xml_node body;
  for (const auto &child : document.children()) {
    if (is_element(child, "document")) { body = child_element(child, "body"); break; }
  }
  if (!body) throw DocxError("docx_missing_body", "This Word document has no readable body.");

  // Every table in the body by its node, numbered exactly as the locator
  // and the writer number them, so the table a lawyer sees and the table a
  // change names are one table even where tables stand inside tables.
  std::vector<pugi::xml_node> tableNodes;
  collect_tbl_nodes(body, tableNodes);
  std::unordered_map<const void *, std::size_t> tableIds;
  for (std::size_t index = 0; index < tableNodes.size(); ++index)
    tableIds.emplace(tableNodes[index].internal_object(), index);

  const Relationships relationships = read_relationships(archive.get(), "word/document.xml");
  DocumentReading reading;
  RenderContext context{styles, layout, numbering, reading, StoryReading{}};
  context.story.relationships = &relationships;
  context.story.table_ids = &tableIds;
  render_blocks(body, context, std::nullopt, model.blocks);

  const std::vector<pugi::xml_node> sections = section_properties(body);
  if (!sections.empty()) model.page = read_page(sections.back());
  model.header = render_story_part(archive.get(), relationships,
                                   default_story_reference(sections, "headerReference"), context);
  model.footer = render_story_part(archive.get(), relationships,
                                   default_story_reference(sections, "footerReference"), context);
  if (otherStories != nullptr) {
    struct OtherStory {
      const char *element;
      const char *kind;
      const char *label;
    };
    static const OtherStory kOthers[] = {
        {"headerReference", "first", "[First-page header]"}, {"headerReference", "even", "[Even-page header]"},
        {"footerReference", "first", "[First-page footer]"}, {"footerReference", "even", "[Even-page footer]"}};
    for (const OtherStory &other : kOthers) {
      const std::string id = story_reference(sections, other.element, other.kind);
      // The page's own header, named again for its first page, is read once.
      if (id.empty() || id == default_story_reference(sections, other.element)) continue;
      std::vector<DocumentRenderParagraph> paragraphs = render_story_part(archive.get(), relationships, id, context);
      if (!paragraphs.empty()) otherStories->push_back({other.label, std::move(paragraphs)});
    }
  }
  render_notes(archive.get(), related_part(relationships, "/footnotes", "word/footnotes.xml"),
               DocumentRenderNoteKind::Footnote, reading.footnotes, context, model.notes);
  render_notes(archive.get(), related_part(relationships, "/endnotes", "word/endnotes.xml"),
               DocumentRenderNoteKind::Endnote, reading.endnotes, context, model.notes);
  render_comments(archive.get(), related_part(relationships, "/comments", "word/comments.xml"),
                  context, model.comments);
}

// ---- The model's reading ------------------------------------------------------
//
// The same reading a person is shown, written out as text for a model. Nothing
// is read a second way: the numbers are the ones the page draws, the notes and
// comments are the ones the page lists, and a tracked change is the one the
// page marks - so what a model says about a document is what a lawyer sees in
// it.

// Who made the pending changes a reading met, and how many each made, in the
// order they were met.
struct PendingRevisions {
  std::vector<std::pair<std::string, std::size_t>> by_author;

  void count(const std::string &author) {
    for (auto &entry : by_author) {
      if (entry.first == author) {
        ++entry.second;
        return;
      }
    }
    by_author.emplace_back(author, 1);
  }
};

// How a pending change opens in the text: "[inserted by Jane Roe: ", or
// "[deleted: " where the document does not say who made it.
std::string revision_opening(const DocumentRenderRevision &revision) {
  std::string opening = revision.kind == DocumentRenderRevisionKind::Inserted ? "[inserted" : "[deleted";
  if (!revision.author.empty()) opening += " by " + revision.author;
  return opening + ": ";
}

// A paragraph as a model reads it: the number or bullet Word draws in front of
// it, its words, a note's number where the note is referred to, and each
// pending change in brackets that say what it is and whose. `around` is a
// revision on what holds the paragraph - a row inserted or struck as a whole -
// that a run carrying none of its own belongs to.
std::string paragraph_reading(const DocumentRenderParagraph &paragraph,
                              const std::optional<DocumentRenderRevision> &around, PendingRevisions &pending) {
  // The paragraph's words in stretches: words one person inserted, or struck,
  // one after another are one stretch - one change - however many runs Word
  // split them into.
  struct Stretch {
    std::optional<DocumentRenderRevision> revision;
    std::string words;
  };
  std::vector<Stretch> stretches;
  for (const DocumentRenderRun &run : paragraph.runs) {
    std::string words = run.text;
    if (run.is_note_reference) {
      words = std::string(run.note_id.rfind("endnote-", 0) == 0 ? "[endnote " : "[footnote ")
          + std::to_string(run.note_number) + "]";
    }
    if (words.empty()) continue;
    const std::optional<DocumentRenderRevision> &revision = run.revision ? run.revision : around;
    const bool continues = !stretches.empty() && stretches.back().revision.has_value() == revision.has_value()
        && (!revision || (stretches.back().revision->kind == revision->kind
                          && stretches.back().revision->author == revision->author));
    if (continues) stretches.back().words += words;
    else stretches.push_back({revision, words});
  }
  std::string text;
  if (!paragraph.list_marker.empty()) text = paragraph.list_marker + " ";
  for (const Stretch &stretch : stretches) {
    if (!stretch.revision) {
      text += stretch.words;
      continue;
    }
    // The spaces around a change stand outside its brackets, as they stand
    // outside Word's marks: "the [deleted by X: six (6)] months".
    const std::size_t first = stretch.words.find_first_not_of(" \t\n");
    if (first == std::string::npos) {
      text += stretch.words;
      continue;
    }
    const std::size_t last = stretch.words.find_last_not_of(" \t\n");
    pending.count(stretch.revision->author);
    text += stretch.words.substr(0, first) + revision_opening(*stretch.revision)
        + stretch.words.substr(first, last - first + 1) + "]" + stretch.words.substr(last + 1);
  }
  return text;
}

// A cell's words on one line, its paragraphs - and a table inside it, cell by
// cell - one after another, as a row of a table reads.
void append_cell_reading(const std::vector<DocumentRenderBlock> &blocks,
                         const std::optional<DocumentRenderRevision> &around, PendingRevisions &pending,
                         std::string &out, std::size_t &paragraphCount) {
  for (const DocumentRenderBlock &block : blocks) {
    if (block.kind == DocumentRenderBlockKind::Paragraph) {
      ++paragraphCount;
      const std::string line = single_line(paragraph_reading(block.paragraph, around, pending));
      if (line.empty()) continue;
      if (!out.empty()) out.push_back(' ');
      out += line;
      continue;
    }
    for (const DocumentRenderRow &row : block.table.rows) {
      for (const DocumentRenderCell &cell : row.cells)
        append_cell_reading(cell.blocks, row.revision ? row.revision : around, pending, out, paragraphCount);
    }
  }
}

// The words of the passage each comment is attached to: the text its range
// covers, or - for a comment anchored at a single point - the paragraph it
// stands in. Only what the passage SAYS, pending deletions left out.
void collect_commented_passages(const std::vector<DocumentRenderBlock> &blocks,
                                std::unordered_map<std::string, std::string> &passages) {
  for (const DocumentRenderBlock &block : blocks) {
    if (block.kind == DocumentRenderBlockKind::Table) {
      for (const DocumentRenderRow &row : block.table.rows)
        for (const DocumentRenderCell &cell : row.cells) collect_commented_passages(cell.blocks, passages);
      continue;
    }
    const DocumentRenderParagraph &paragraph = block.paragraph;
    // A range that runs on into this paragraph reads with a space where the
    // paragraph break is, as the passage reads to a person.
    std::unordered_set<std::string> begunHere;
    for (const DocumentRenderRun &run : paragraph.runs) {
      for (const std::string &id : run.comment_ids) {
        if (run.text.empty()) {
          if (passages.count(id) == 0) passages[id] = paragraph.text();
          continue;
        }
        if (run.is_deleted()) continue;
        std::string &passage = passages[id];
        if (begunHere.insert(id).second && !passage.empty() && passage.back() != ' ') passage.push_back(' ');
        passage += run.text;
      }
    }
  }
}

// A passage short enough to name a place: its first words, cut at a space.
std::string passage_named(const std::string &text) {
  constexpr std::size_t kLongest = 120;
  const std::string line = single_line(text);
  if (line.size() <= kLongest) return line;
  std::size_t cut = line.rfind(' ', kLongest);
  if (cut == std::string::npos || cut < kLongest / 2) cut = kLongest;
  // Never inside a character: back off a UTF-8 continuation byte.
  while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
  return line.substr(0, cut) + "...";
}

// A header or footer line that only says which page this is - "1", "Page 2" -
// tells a model nothing about the document.
bool is_page_number(const std::string &line) {
  std::string rest = lowercase(line);
  for (const char *word : {"page", "of"}) {
    std::size_t at;
    while ((at = rest.find(word)) != std::string::npos) rest.erase(at, std::strlen(word));
  }
  return std::all_of(rest.begin(), rest.end(), [](const unsigned char byte) {
    return std::isdigit(byte) != 0 || std::isspace(byte) != 0 || byte == '-' || byte == '/';
  });
}

// The one line a reading opens with when the document holds tracked changes
// nobody has accepted or rejected: that they are there, whose they are, and
// how the text marks them.
std::string pending_revisions_line(const PendingRevisions &pending) {
  std::string who;
  for (std::size_t index = 0; index < pending.by_author.size(); ++index) {
    if (index > 0) who += ", ";
    const auto &[author, changes] = pending.by_author[index];
    who += std::to_string(changes) + " by " + (author.empty() ? std::string("an unnamed author") : author);
  }
  return "[This document has tracked changes that are not yet accepted or rejected in Word: " + who
      + ". Words marked [inserted by ...: ...] are proposed and not yet agreed; words marked "
        "[deleted by ...: ...] still stand until the deletion is accepted.]";
}

} // namespace

DocumentRenderModel render_docx_document(const DocumentExtractionRequest &request) {
  DocumentRenderModel model;
  model.format = DocumentFormat::docx;
  try {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(request.path, filesystem_error) || filesystem_error) {
      model.status = ExtractionStatus::unreadable;
      model.limitation = "This document is no longer at the place the Project knows it by.";
      return model;
    }
    bool stylesMissing = false;
    read_docx_package(request, model, stylesMissing);
    model.status = ExtractionStatus::ready;
    return model;
  } catch (const DocxError &error) {
    model.blocks.clear();
    model.status = ExtractionStatus::unreadable;
    model.diagnostics.push_back({error.code, error.what()});
    model.limitation = error.what();
    return model;
  } catch (const std::exception &) {
    model.blocks.clear();
    model.status = ExtractionStatus::unreadable;
    model.limitation = "This Word document could not be read.";
    return model;
  }
}

DocumentExtractionResult extract_docx_document(const DocumentExtractionRequest &request) {
  try {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(request.path, filesystem_error) || filesystem_error) {
      return failure("unreadable_file", "The DOCX document is missing or is not a regular file.");
    }
    const auto archive_size = std::filesystem::file_size(request.path, filesystem_error);
    if (filesystem_error) return failure("unreadable_file", "The DOCX document size could not be read.");
    if (archive_size > request.maximum_input_bytes) {
      return failure("input_too_large", "The DOCX document exceeds the configured extraction limit.");
    }

    DocumentRenderModel model;
    bool stylesMissing = false;
    std::vector<LabelledStory> otherStories;
    read_docx_package(request, model, stylesMissing, &otherStories);

    // ---- The body, block by block ----
    PendingRevisions pending;
    std::vector<std::string> lines;
    std::vector<DocumentHeading> headings;
    std::size_t paragraphCount = 0;
    std::size_t nextLine = 1;
    for (const DocumentRenderBlock &block : model.blocks) {
      if (block.kind == DocumentRenderBlockKind::Paragraph) {
        const DocumentRenderParagraph &paragraph = block.paragraph;
        std::string text = paragraph_reading(paragraph, std::nullopt, pending);
        const std::size_t line = nextLine;
        nextLine += 1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
        lines.push_back(std::move(text));
        ++paragraphCount;
        if (paragraph.heading_level.has_value()) {
          // A heading is named as the page shows it - its number and the words
          // it says - so "9.2 Limitation of Liability" is found by either.
          const std::string title = single_line(
              (paragraph.list_marker.empty() ? std::string() : paragraph.list_marker + " ") + paragraph.text());
          if (!title.empty()) headings.push_back({line, *paragraph.heading_level, title});
        }
        continue;
      }
      // A table stands apart from the paragraphs around it, one row to a line
      // and its cells separated by tabs.
      if (!lines.empty() && !lines.back().empty()) {
        lines.emplace_back();
        ++nextLine;
      }
      bool emittedRow = false;
      for (const DocumentRenderRow &row : block.table.rows) {
        std::string rendered;
        for (std::size_t index = 0; index < row.cells.size(); ++index) {
          if (index > 0) rendered.push_back('\t');
          std::string cell;
          append_cell_reading(row.cells[index].blocks, row.revision, pending, cell, paragraphCount);
          rendered += cell;
        }
        lines.push_back(std::move(rendered));
        ++nextLine;
        emittedRow = true;
      }
      if (emittedRow) {
        lines.emplace_back();
        ++nextLine;
      }
    }

    // ---- What stands around the body, each part labelled ----
    std::vector<std::string> parts;
    const auto story = [&](const char *label, const std::vector<DocumentRenderParagraph> &paragraphs) {
      for (const DocumentRenderParagraph &paragraph : paragraphs) {
        const std::string reading = single_line(paragraph_reading(paragraph, std::nullopt, pending));
        if (reading.empty() || is_page_number(reading)) continue;
        parts.push_back(std::string(label) + " " + reading);
      }
    };
    story("[Header]", model.header);
    for (const LabelledStory &other : otherStories)
      if (other.label.find("header") != std::string::npos) story(other.label.c_str(), other.paragraphs);
    story("[Footer]", model.footer);
    for (const LabelledStory &other : otherStories)
      if (other.label.find("footer") != std::string::npos) story(other.label.c_str(), other.paragraphs);
    for (const DocumentRenderNote &note : model.notes) {
      std::string words;
      for (const DocumentRenderParagraph &paragraph : note.paragraphs) {
        const std::string reading = single_line(paragraph_reading(paragraph, std::nullopt, pending));
        if (reading.empty()) continue;
        if (!words.empty()) words.push_back(' ');
        words += reading;
      }
      if (words.empty()) continue;
      parts.push_back(std::string(note.kind == DocumentRenderNoteKind::Footnote ? "[Footnote " : "[Endnote ")
          + std::to_string(note.number) + "] " + words);
    }
    std::unordered_map<std::string, std::string> commented;
    collect_commented_passages(model.blocks, commented);
    for (const DocumentRenderComment &comment : model.comments) {
      std::string words;
      for (const DocumentRenderParagraph &paragraph : comment.paragraphs) {
        const std::string reading = single_line(paragraph_reading(paragraph, std::nullopt, pending));
        if (reading.empty()) continue;
        if (!words.empty()) words.push_back(' ');
        words += reading;
      }
      if (words.empty()) continue;
      std::string label = "[Comment";
      if (!comment.author.empty()) label += " by " + comment.author;
      const auto passage = commented.find(comment.id);
      if (passage != commented.end() && !single_line(passage->second).empty())
        label += " on \"" + passage_named(passage->second) + "\"";
      parts.push_back(label + "] " + words);
    }

    bool hasWords = std::any_of(lines.begin(), lines.end(), [](const std::string &line) {
      return has_non_whitespace(line);
    });
    hasWords = hasWords || !parts.empty();
    if (!parts.empty()) {
      if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
      for (std::string &part : parts) lines.push_back(std::move(part));
    }
    // The pending changes are said first, before anything they affect is read.
    if (!pending.by_author.empty()) {
      lines.insert(lines.begin(), {pending_revisions_line(pending), std::string()});
      for (DocumentHeading &heading : headings) heading.line += 2;
    }

    DocumentExtractionResult result;
    result.format = DocumentFormat::docx;
    result.extractor_version = std::string(kExtractorVersion);
    result.text = join_lines(lines);
    result.structure.byte_count = result.text.size();
    result.structure.line_count = result.text.empty()
        ? 0 : 1 + static_cast<std::size_t>(std::count(result.text.begin(), result.text.end(), '\n'));
    result.structure.paragraph_count = paragraphCount;
    result.structure.headings = std::move(headings);
    if (stylesMissing) {
      result.diagnostics.push_back(
          {"docx_styles_missing", "The DOCX package has no styles.xml; style-based headings are unavailable."});
    }
    if (!hasWords) {
      result.status = ExtractionStatus::empty;
      result.diagnostics.push_back({"empty_document", "The document contains no extractable text."});
    } else {
      result.status = ExtractionStatus::ready;
    }
    return result;
  } catch (const DocxError &error) {
    return failure(error.code, error.what());
  } catch (const std::exception &) {
    return failure("docx_extraction_failed", "The DOCX document could not be extracted safely.");
  }
}

namespace {

// What a package's styles and numbering say about its paragraphs, read once and
// kept by the functions that answer about them.
struct PackageNumbering {
  StyleMap styles;
  LayoutStyles layout;
  NumberingDefinitions numbering;
  DocumentReading reading;
};

std::shared_ptr<PackageNumbering> read_package_numbering(const std::string &stylesXml,
                                                         const std::string &numberingXml) {
  auto package = std::make_shared<PackageNumbering>();
  try {
    if (!stylesXml.empty()) {
      const pugi::xml_document styles = parse_xml(stylesXml, "docx_malformed_styles_xml", "word/styles.xml is malformed.");
      collect_styles(styles, package->styles);
      collect_layout_styles(styles, package->layout);
    }
  } catch (const std::exception &) {
  }
  try {
    if (!numberingXml.empty()) {
      const pugi::xml_document numbering =
          parse_xml(numberingXml, "docx_malformed_numbering_xml", "word/numbering.xml is malformed.");
      collect_numbering(numbering, package->numbering);
    }
  } catch (const std::exception &) {
  }
  return package;
}

// The list a paragraph is in, and the level Word draws it at.
ParagraphList list_of_paragraph(PackageNumbering &package, const pugi::xml_node &node) {
  RenderContext context{package.styles, package.layout, package.numbering, package.reading, StoryReading{}};
  const auto properties = child_element(node, "pPr");
  const auto style = child_element(properties, "pStyle");
  std::string styleId = style ? attribute_value(style, "val") : std::string();
  if (styleId.empty() || package.layout.paragraphs.count(styleId) == 0) styleId = package.layout.default_paragraph;
  return paragraph_list(properties, styleId, context);
}

// Every paragraph of `body` in reading order - tables, content controls and
// tracked changes included - with what Word draws in front of each one it
// numbers, counted as the reading counts. `each` is told of every paragraph
// Word numbers; it returns true to stop.
void count_numbered_paragraphs(PackageNumbering &package, const pugi::xml_node &body,
                               const std::function<bool(const pugi::xml_node &, const std::string &)> &each) {
  NumberingState state;
  const std::function<bool(const pugi::xml_node &)> count = [&](const pugi::xml_node &container) {
    for (const auto &child : container.children()) {
      if (child.type() != pugi::node_element) continue;
      const std::string_view name = local_name(child.name());
      if (name == "p") {
        const ParagraphList met = list_of_paragraph(package, child);
        if (!met.found) continue;
        const ResolvedList list = resolve_list(met.num_id, package.numbering, package.layout);
        if (list.level(met.level) == nullptr) continue;
        if (each(child, next_list_marker(list, met.level, state))) return true;
        continue;
      }
      if (name == "AlternateContent") {
        const auto choice = child_element(child, "Choice");
        if (count(choice ? choice : child_element(child, "Fallback"))) return true;
        continue;
      }
      if (name == "tbl" || name == "tr" || name == "tc" || name == "sdt" || name == "sdtContent"
          || name == "customXml" || revision_kind(name)) {
        if (count(child)) return true;
      }
    }
    return false;
  };
  count(body);
}

pugi::xml_node body_of(const pugi::xml_node &node) {
  for (pugi::xml_node up = node; up; up = up.parent())
    if (is_element(up, "body")) return up;
  return {};
}

// What Word draws for the first item of a level of a list: each number in it
// at the value its level starts at.
std::string first_marker(const ResolvedList &list, std::size_t level) {
  const ListLevel *definition = list.level(level);
  if (definition == nullptr) return {};
  if (definition->format == "bullet") return bullet_marker(*definition);
  std::string marker;
  const std::string &text = definition->text;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '%' || index + 1 >= text.size() || text[index + 1] < '1' || text[index + 1] > '9') {
      marker.push_back(text[index]);
      continue;
    }
    const std::size_t referenced = static_cast<std::size_t>(text[index + 1] - '1');
    ++index;
    const ListLevel *referencedLevel = list.level(referenced);
    if (referencedLevel == nullptr) continue;
    marker += list_number(std::max(1, list.start(referenced)), *referencedLevel, definition->legal);
  }
  return marker;
}

// What Word draws in front of a paragraph, as docx_automatic_numbering answers.
AutomaticNumbering automatic_numbering_of(const std::shared_ptr<PackageNumbering> &package) {
  return [package](const pugi::xml_node &paragraph) -> std::optional<std::string> {
    const ParagraphList list = list_of_paragraph(*package, paragraph);
    if (!list.found) return std::nullopt;
    const ResolvedList resolved = resolve_list(list.num_id, package->numbering, package->layout);
    const ListLevel *level = resolved.level(list.level);
    // A list Singularity cannot resolve is still a list Word draws a mark for; a
    // level that draws nothing ("none") numbers nothing.
    if (level == nullptr) return std::string();
    if (level->format == "none") return std::nullopt;
    const pugi::xml_node body = body_of(paragraph);
    if (!body) {
      NumberingState state;
      return next_list_marker(resolved, list.level, state);
    }
    std::optional<std::string> marker;
    count_numbered_paragraphs(*package, body, [&](const pugi::xml_node &node, const std::string &drawn) {
      if (node != paragraph) return false;
      marker = drawn;
      return true;
    });
    return marker ? *marker : std::string();
  };
}

} // namespace

std::function<std::optional<std::string>(const pugi::xml_node &)> docx_automatic_numbering(
    const std::string &stylesXml, const std::string &numberingXml) {
  return automatic_numbering_of(read_package_numbering(stylesXml, numberingXml));
}

void read_docx_numbering_into(TextEditRequest &request, const std::string &stylesXml,
                              const std::string &numberingXml) {
  const std::shared_ptr<PackageNumbering> package = read_package_numbering(stylesXml, numberingXml);
  request.numbered_automatically = automatic_numbering_of(package);
  request.is_heading = [package](const pugi::xml_node &paragraph) {
    return paragraph_heading_level(paragraph, package->styles).has_value();
  };
  request.list_place = [package](const pugi::xml_node &paragraph) -> std::optional<ListPlace> {
    const ParagraphList list = list_of_paragraph(*package, paragraph);
    if (!list.found) return std::nullopt;
    const ResolvedList resolved = resolve_list(list.num_id, package->numbering, package->layout);
    const ListLevel *own = resolved.level(list.level);
    if (own == nullptr) return std::nullopt;
    ListPlace place;
    place.list_id = list.num_id;
    place.level = list.level;
    for (std::size_t level = 0; level < kListLevels; ++level) {
      const ListLevel *definition = resolved.level(level);
      if (definition == nullptr || definition->format == "none") place.first_drawn.emplace_back(std::nullopt);
      else place.first_drawn.emplace_back(first_marker(resolved, level));
    }
    place.text_indent_twips = own->indent.left;
    return place;
  };
}

std::vector<std::pair<pugi::xml_node, std::string>> docx_drawn_numbers(const pugi::xml_node &body,
                                                                       const std::string &stylesXml,
                                                                       const std::string &numberingXml) {
  const std::shared_ptr<PackageNumbering> package = read_package_numbering(stylesXml, numberingXml);
  std::vector<std::pair<pugi::xml_node, std::string>> drawn;
  count_numbered_paragraphs(*package, body, [&](const pugi::xml_node &node, const std::string &marker) {
    drawn.emplace_back(node, marker);
    return false;
  });
  return drawn;
}

} // namespace singularity::documents
