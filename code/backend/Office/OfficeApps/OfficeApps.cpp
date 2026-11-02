#include "Office/OfficeApps/OfficeApps.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"

#include <zip.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
// shellapi.h depends on types and macros declared by windows.h.
#include <shellapi.h>
#elif defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;
#endif

namespace singularity::office {
namespace {

using Parts = std::vector<std::pair<std::string, std::string>>;

constexpr const char *kXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
constexpr const char *kRelationshipsNamespace = "http://schemas.openxmlformats.org/package/2006/relationships";
constexpr const char *kRelationship = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr const char *kDrawing = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr const char *kPresentation = "http://schemas.openxmlformats.org/presentationml/2006/main";
constexpr const char *kRelationships = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
// The one web extension in each of Singularity's files, by an id of its own.
constexpr const char *kWebExtensionId = "{5E1B7C2A-9D44-4F0E-8B6A-2C7D3E9F1A40}";

std::string escaped(const std::string &text) {
	std::string written;
	for (const char character : text) {
		switch (character) {
			case '&': written += "&amp;"; break;
			case '<': written += "&lt;"; break;
			case '>': written += "&gt;"; break;
			case '"': written += "&quot;"; break;
			default: written += character;
		}
	}
	return written;
}

std::string relationship(const std::string &id, const std::string &type, const std::string &target) {
	return "  <Relationship Id=\"" + id + "\" Type=\"" + type + "\" Target=\"" + target + "\"/>\n";
}

std::string relationships(const std::string &entries) {
	return std::string(kXml) + "<Relationships xmlns=\"" + kRelationshipsNamespace + "\">\n" + entries + "</Relationships>\n";
}

std::string content_type(const std::string &part, const std::string &type) {
	return "  <Override PartName=\"/" + part + "\" ContentType=\"" + type + "\"/>\n";
}

std::string content_types(const std::string &overrides) {
	return std::string(kXml) + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
		+ "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
		+ "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n" + overrides + "</Types>\n";
}

// What the file says it is, line by line, under its title.
std::vector<std::string> lines_of(OfficeApp app) {
	const std::string kind = app == OfficeApp::excel ? "workbook" : app == OfficeApp::powerpoint ? "presentation" : "document";
	return {
		"Singularity's pane is on the right. It asks to connect by itself: check that Singularity shows the same code, "
		"then press Allow in Singularity.",
		"Then open a " + kind + " from one of your Matters. Its Matter, its team and its work open in the pane.",
		"Singularity made this " + kind + " to open its pane. You can close it at any time.",
	};
}

std::string title_of(OfficeApp app) {
	return std::string("Singularity in ") + name_of(app);
}

// Singularity's add-in in the package, under the application's own folder of
// it ("word", "xl", "ppt"): its web extension, shown in a pane as the file
// opens, and opening with it each time after (the docs' "autoopen").
void add_web_extension(Parts &parts, const std::string &folder, const std::string &addin_id, const std::string &version) {
	parts.emplace_back(folder + "/webextensions/webextension1.xml", std::string(kXml)
		+ "<we:webextension xmlns:we=\"http://schemas.microsoft.com/office/webextensions/webextension/2010/11\" id=\""
		+ kWebExtensionId + "\">\n"
		+ "  <we:reference id=\"" + escaped(addin_id) + "\" version=\"" + escaped(version)
		+ "\" store=\"developer\" storeType=\"Registry\"/>\n"
		+ "  <we:alternateReferences/>\n"
		+ "  <we:properties>\n"
		+ "    <we:property name=\"Office.AutoShowTaskpaneWithDocument\" value=\"true\"/>\n"
		+ "  </we:properties>\n"
		+ "  <we:bindings/>\n"
		+ "  <we:snapshot xmlns:r=\"" + kRelationships + "\"/>\n"
		+ "</we:webextension>\n");
	parts.emplace_back(folder + "/webextensions/taskpanes.xml", std::string(kXml)
		+ "<wetp:taskpanes xmlns:wetp=\"http://schemas.microsoft.com/office/webextensions/taskpanes/2010/11\">\n"
		+ "  <wetp:taskpane dockstate=\"right\" visibility=\"1\" width=\"350\" row=\"1\">\n"
		+ "    <wetp:webextensionref xmlns:r=\"" + kRelationships + "\" r:id=\"rId1\"/>\n"
		+ "  </wetp:taskpane>\n"
		+ "</wetp:taskpanes>\n");
	parts.emplace_back(folder + "/webextensions/_rels/taskpanes.xml.rels",
		relationships(relationship("rId1", "http://schemas.microsoft.com/office/2011/relationships/webextension", "webextension1.xml")));
}

std::string web_extension_types(const std::string &folder) {
	return content_type(folder + "/webextensions/taskpanes.xml", "application/vnd.ms-office.webextensiontaskpanes+xml")
		+ content_type(folder + "/webextensions/webextension1.xml", "application/vnd.ms-office.webextension+xml");
}

std::string package_relationships(const std::string &main_part, const std::string &folder) {
	return relationships(relationship("rId1", std::string(kRelationship) + "officeDocument", main_part)
		+ relationship("rId2", "http://schemas.microsoft.com/office/2011/relationships/webextensiontaskpanes",
			folder + "/webextensions/taskpanes.xml"));
}

Parts word_parts(const std::string &addin_id, const std::string &version) {
	Parts parts;
	parts.emplace_back("[Content_Types].xml", content_types(
		content_type("word/document.xml", "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml")
		+ content_type("word/styles.xml", "application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml")
		+ web_extension_types("word")));
	parts.emplace_back("_rels/.rels", package_relationships("word/document.xml", "word"));
	std::ostringstream document;
	document << kXml << "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\n"
	         << "  <w:body>\n"
	         << "    <w:p><w:pPr><w:spacing w:after=\"240\"/></w:pPr><w:r><w:rPr><w:b/><w:sz w:val=\"32\"/><w:szCs w:val=\"32\"/></w:rPr>"
	         << "<w:t>" << escaped(title_of(OfficeApp::word)) << "</w:t></w:r></w:p>\n";
	for (const std::string &line : lines_of(OfficeApp::word)) {
		document << "    <w:p><w:r><w:t xml:space=\"preserve\">" << escaped(line) << "</w:t></w:r></w:p>\n";
	}
	document << "    <w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\""
	         << " w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr>\n"
	         << "  </w:body>\n"
	         << "</w:document>\n";
	parts.emplace_back("word/document.xml", document.str());
	parts.emplace_back("word/_rels/document.xml.rels", relationships(relationship("rId1", std::string(kRelationship) + "styles", "styles.xml")));
	parts.emplace_back("word/styles.xml", std::string(kXml)
		+ "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\n"
		+ "  <w:docDefaults>\n"
		+ "    <w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:eastAsia=\"Calibri\" w:hAnsi=\"Calibri\" w:cs=\"Calibri\"/>"
		+ "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/><w:lang w:val=\"en-US\"/></w:rPr></w:rPrDefault>\n"
		+ "    <w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"259\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault>\n"
		+ "  </w:docDefaults>\n"
		+ "</w:styles>\n");
	add_web_extension(parts, "word", addin_id, version);
	return parts;
}

std::string cell(const std::string &reference, const std::string &text) {
	return "<c r=\"" + reference + "\" t=\"inlineStr\"><is><t>" + escaped(text) + "</t></is></c>";
}

Parts excel_parts(const std::string &addin_id, const std::string &version) {
	Parts parts;
	parts.emplace_back("[Content_Types].xml", content_types(
		content_type("xl/workbook.xml", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml")
		+ content_type("xl/worksheets/sheet1.xml", "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml")
		+ web_extension_types("xl")));
	parts.emplace_back("_rels/.rels", package_relationships("xl/workbook.xml", "xl"));
	parts.emplace_back("xl/workbook.xml", std::string(kXml)
		+ "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"" + kRelationships + "\">\n"
		+ "  <sheets><sheet name=\"Singularity\" sheetId=\"1\" r:id=\"rId1\"/></sheets>\n"
		+ "</workbook>\n");
	parts.emplace_back("xl/_rels/workbook.xml.rels",
		relationships(relationship("rId1", std::string(kRelationship) + "worksheet", "worksheets/sheet1.xml")));
	// The title in the first row, each line a row below, with a row between.
	const std::vector<std::string> lines = lines_of(OfficeApp::excel);
	std::ostringstream sheet;
	sheet << kXml << "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\n"
	      << "  <sheetData>\n"
	      << "    <row r=\"1\">" << cell("A1", title_of(OfficeApp::excel)) << "</row>\n";
	for (std::size_t index = 0; index < lines.size(); ++index) {
		const std::string row = std::to_string(index + 3);
		sheet << "    <row r=\"" << row << "\">" << cell("A" + row, lines[index]) << "</row>\n";
	}
	sheet << "  </sheetData>\n"
	      << "</worksheet>\n";
	parts.emplace_back("xl/worksheets/sheet1.xml", sheet.str());
	add_web_extension(parts, "xl", addin_id, version);
	return parts;
}

// The group a slide's shapes stand in, as PowerPoint writes it.
constexpr const char *kShapeTree =
	"<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
	"<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>";

std::string text_box(int id, const std::string &name, long long top, long long height, const std::string &paragraphs) {
	return "      <p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + name + "\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>"
		+ "<p:spPr><a:xfrm><a:off x=\"838200\" y=\"" + std::to_string(top) + "\"/><a:ext cx=\"7772400\" cy=\"" + std::to_string(height)
		+ "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>"
		+ "<p:txBody><a:bodyPr wrap=\"square\" rtlCol=\"0\"/><a:lstStyle/>" + paragraphs + "</p:txBody></p:sp>\n";
}

// The theme a deck needs: Office's own colours and fonts, and its plain fills,
// lines and effects.
std::string theme() {
	const std::string fill = "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>";
	const auto line = [&fill](int width) { return "<a:ln w=\"" + std::to_string(width) + "\">" + fill + "</a:ln>"; };
	const std::string effect = "<a:effectStyle><a:effectLst/></a:effectStyle>";
	return std::string(kXml) + "<a:theme xmlns:a=\"" + kDrawing + "\" name=\"Office Theme\">\n"
		+ "  <a:themeElements>\n"
		+ "    <a:clrScheme name=\"Office\">"
		+ "<a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
		+ "<a:dk2><a:srgbClr val=\"44546A\"/></a:dk2><a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2>"
		+ "<a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1><a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2>"
		+ "<a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3><a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4>"
		+ "<a:accent5><a:srgbClr val=\"5B9BD5\"/></a:accent5><a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6>"
		+ "<a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink><a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink>"
		+ "</a:clrScheme>\n"
		+ "    <a:fontScheme name=\"Office\">"
		+ "<a:majorFont><a:latin typeface=\"Calibri Light\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
		+ "<a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont>"
		+ "</a:fontScheme>\n"
		+ "    <a:fmtScheme name=\"Office\">"
		+ "<a:fillStyleLst>" + fill + fill + fill + "</a:fillStyleLst>"
		+ "<a:lnStyleLst>" + line(6350) + line(12700) + line(19050) + "</a:lnStyleLst>"
		+ "<a:effectStyleLst>" + effect + effect + effect + "</a:effectStyleLst>"
		+ "<a:bgFillStyleLst>" + fill + fill + fill + "</a:bgFillStyleLst>"
		+ "</a:fmtScheme>\n"
		+ "  </a:themeElements>\n"
		+ "  <a:objectDefaults/>\n"
		+ "  <a:extraClrSchemeLst/>\n"
		+ "</a:theme>\n";
}

Parts powerpoint_parts(const std::string &addin_id, const std::string &version) {
	const std::string namespaces = std::string(" xmlns:a=\"") + kDrawing + "\" xmlns:r=\"" + kRelationships + "\" xmlns:p=\"" + kPresentation + "\"";
	Parts parts;
	parts.emplace_back("[Content_Types].xml", content_types(
		content_type("ppt/presentation.xml", "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml")
		+ content_type("ppt/slideMasters/slideMaster1.xml", "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml")
		+ content_type("ppt/slideLayouts/slideLayout1.xml", "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml")
		+ content_type("ppt/slides/slide1.xml", "application/vnd.openxmlformats-officedocument.presentationml.slide+xml")
		+ content_type("ppt/theme/theme1.xml", "application/vnd.openxmlformats-officedocument.theme+xml")
		+ web_extension_types("ppt")));
	parts.emplace_back("_rels/.rels", package_relationships("ppt/presentation.xml", "ppt"));
	parts.emplace_back("ppt/presentation.xml", std::string(kXml) + "<p:presentation" + namespaces + ">\n"
		+ "  <p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>\n"
		+ "  <p:sldIdLst><p:sldId id=\"256\" r:id=\"rId2\"/></p:sldIdLst>\n"
		+ "  <p:sldSz cx=\"12192000\" cy=\"6858000\"/>\n"
		+ "  <p:notesSz cx=\"6858000\" cy=\"9144000\"/>\n"
		+ "</p:presentation>\n");
	parts.emplace_back("ppt/_rels/presentation.xml.rels", relationships(
		relationship("rId1", std::string(kRelationship) + "slideMaster", "slideMasters/slideMaster1.xml")
		+ relationship("rId2", std::string(kRelationship) + "slide", "slides/slide1.xml")
		+ relationship("rId3", std::string(kRelationship) + "theme", "theme/theme1.xml")));
	parts.emplace_back("ppt/slideMasters/slideMaster1.xml", std::string(kXml) + "<p:sldMaster" + namespaces + ">\n"
		+ "  <p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg><p:spTree>" + kShapeTree + "</p:spTree></p:cSld>\n"
		+ "  <p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\""
		+ " accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>\n"
		+ "  <p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>\n"
		+ "  <p:txStyles><p:titleStyle/><p:bodyStyle/><p:otherStyle/></p:txStyles>\n"
		+ "</p:sldMaster>\n");
	parts.emplace_back("ppt/slideMasters/_rels/slideMaster1.xml.rels", relationships(
		relationship("rId1", std::string(kRelationship) + "slideLayout", "../slideLayouts/slideLayout1.xml")
		+ relationship("rId2", std::string(kRelationship) + "theme", "../theme/theme1.xml")));
	parts.emplace_back("ppt/slideLayouts/slideLayout1.xml", std::string(kXml) + "<p:sldLayout" + namespaces + " type=\"blank\" preserve=\"1\">\n"
		+ "  <p:cSld name=\"Blank\"><p:spTree>" + kShapeTree + "</p:spTree></p:cSld>\n"
		+ "  <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>\n"
		+ "</p:sldLayout>\n");
	parts.emplace_back("ppt/slideLayouts/_rels/slideLayout1.xml.rels",
		relationships(relationship("rId1", std::string(kRelationship) + "slideMaster", "../slideMasters/slideMaster1.xml")));
	std::string body;
	for (const std::string &line : lines_of(OfficeApp::powerpoint)) {
		body += "<a:p><a:pPr><a:spcAft><a:spcPts val=\"1200\"/></a:spcAft></a:pPr><a:r><a:rPr lang=\"en-US\" sz=\"2000\" dirty=\"0\"/><a:t>"
			+ escaped(line) + "</a:t></a:r></a:p>";
	}
	parts.emplace_back("ppt/slides/slide1.xml", std::string(kXml) + "<p:sld" + namespaces + ">\n"
		+ "  <p:cSld>\n"
		+ "    <p:spTree>" + kShapeTree + "\n"
		+ text_box(2, "Title", 1325563, 923330, "<a:p><a:r><a:rPr lang=\"en-US\" sz=\"3600\" b=\"1\" dirty=\"0\"/><a:t>"
			+ escaped(title_of(OfficeApp::powerpoint)) + "</a:t></a:r></a:p>")
		+ text_box(3, "Text", 2448000, 2400000, body)
		+ "    </p:spTree>\n"
		+ "  </p:cSld>\n"
		+ "  <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>\n"
		+ "</p:sld>\n");
	parts.emplace_back("ppt/slides/_rels/slide1.xml.rels",
		relationships(relationship("rId1", std::string(kRelationship) + "slideLayout", "../slideLayouts/slideLayout1.xml")));
	parts.emplace_back("ppt/theme/theme1.xml", theme());
	add_web_extension(parts, "ppt", addin_id, version);
	return parts;
}

} // namespace

std::optional<OfficeApp> office_app_of(const std::string &word) {
	if (word == "word") return OfficeApp::word;
	if (word == "excel") return OfficeApp::excel;
	if (word == "powerpoint") return OfficeApp::powerpoint;
	if (word == "outlook") return OfficeApp::outlook;
	return std::nullopt;
}

const char *word_of(OfficeApp app) {
	switch (app) {
		case OfficeApp::word: return "word";
		case OfficeApp::excel: return "excel";
		case OfficeApp::powerpoint: return "powerpoint";
		default: return "outlook";
	}
}

const char *name_of(OfficeApp app) {
	switch (app) {
		case OfficeApp::word: return "Word";
		case OfficeApp::excel: return "Excel";
		case OfficeApp::powerpoint: return "PowerPoint";
		default: return "Outlook";
	}
}

const char *windows_program_of(OfficeApp app) {
	switch (app) {
		case OfficeApp::word: return "WINWORD.EXE";
		case OfficeApp::excel: return "EXCEL.EXE";
		case OfficeApp::powerpoint: return "POWERPNT.EXE";
		default: return "OUTLOOK.EXE";
	}
}

const char *mac_bundle_of(OfficeApp app) {
	switch (app) {
		case OfficeApp::word: return "com.microsoft.Word";
		case OfficeApp::excel: return "com.microsoft.Excel";
		// PowerPoint's identifier is spelt so (Platform/OfficeAddinRegistry).
		case OfficeApp::powerpoint: return "com.microsoft.Powerpoint";
		default: return "com.microsoft.Outlook";
	}
}

const char *mac_app_folder_of(OfficeApp app) {
	switch (app) {
		case OfficeApp::word: return "Microsoft Word.app";
		case OfficeApp::excel: return "Microsoft Excel.app";
		case OfficeApp::powerpoint: return "Microsoft PowerPoint.app";
		default: return "Microsoft Outlook.app";
	}
}

bool is_installed_in(OfficeApp app, const std::filesystem::path &applications) {
	std::error_code error;
	return std::filesystem::is_directory(applications / mac_app_folder_of(app), error) && !error;
}

std::optional<std::filesystem::path> program_at(const std::string &registered) {
	const auto first = registered.find_first_not_of(" \t");
	if (first == std::string::npos) return std::nullopt;
	std::string text = registered.substr(first, registered.find_last_not_of(" \t") - first + 1);
	if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
	if (text.empty() || text.find('"') != std::string::npos) return std::nullopt;
	const std::filesystem::path program = std::filesystem::u8path(text);
	std::error_code error;
	if (!program.is_absolute() || !std::filesystem::is_regular_file(program, error) || error) return std::nullopt;
	return program;
}

std::string starter_file_name(OfficeApp app) {
	switch (app) {
		case OfficeApp::word: return "Singularity in Word.docx";
		case OfficeApp::excel: return "Singularity in Excel.xlsx";
		case OfficeApp::powerpoint: return "Singularity in PowerPoint.pptx";
		default: return {};
	}
}

bool is_starter_file(const std::filesystem::path &file, const std::filesystem::path &folder) {
	for (const OfficeApp app : {OfficeApp::word, OfficeApp::excel, OfficeApp::powerpoint}) {
		std::error_code error;
		if (std::filesystem::equivalent(file, folder / std::filesystem::u8path(starter_file_name(app)), error) && !error) return true;
	}
	return false;
}

std::vector<std::pair<std::string, std::string>> starter_parts(OfficeApp app, const std::string &addin_id,
	const std::string &version) {
	switch (app) {
		case OfficeApp::word: return word_parts(addin_id, version);
		case OfficeApp::excel: return excel_parts(addin_id, version);
		case OfficeApp::powerpoint: return powerpoint_parts(addin_id, version);
		default: return {};
	}
}

std::filesystem::path write_starter_file(OfficeApp app, const std::filesystem::path &folder, const std::string &addin_id,
	const std::string &version) {
	const std::string name = starter_file_name(app);
	if (name.empty()) throw std::runtime_error("Outlook opens without a file of Singularity's.");
	const std::filesystem::path file = folder / std::filesystem::u8path(name);
	std::error_code error;
	// Open in its application: it is brought forward as it is.
	if (std::filesystem::is_regular_file(file, error) && platform::office_document_is_open(file)) return file;
	const std::string failed = std::string("The file Singularity opens ") + name_of(app) + " with could not be written.";
	platform::ensure_record_directory(folder);
	const Parts parts = starter_parts(app, addin_id, version);
	const std::filesystem::path staged = platform::staged_sibling_path(file);
	int code = 0;
	zip_t *archive = zip_open(staged.u8string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &code);
	if (archive == nullptr) throw std::runtime_error(failed);
	for (const auto &[part, text] : parts) {
		zip_source_t *source = zip_source_buffer(archive, text.data(), text.size(), 0);
		if (source == nullptr || zip_file_add(archive, part.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
			if (source != nullptr) zip_source_free(source);
			zip_discard(archive);
			std::filesystem::remove(staged, error);
			throw std::runtime_error(failed);
		}
	}
	if (zip_close(archive) != 0) {
		zip_discard(archive);
		std::filesystem::remove(staged, error);
		throw std::runtime_error(failed);
	}
	if (platform::replace_file(staged, file)) {
		std::filesystem::remove(staged, error);
		// Held by a program that marks nothing beside it: the one there stands.
		if (std::filesystem::is_regular_file(file, error)) return file;
		throw std::runtime_error(failed);
	}
	return file;
}

#if defined(_WIN32)

namespace {

std::wstring wide(const std::string &text) {
	return std::filesystem::u8path(text).wstring();
}

// The default value of a program's key in App Paths, its variables expanded.
std::optional<std::wstring> app_paths_value(HKEY root, REGSAM view, const std::wstring &program) {
	const std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + program;
	HKEY opened = nullptr;
	if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | view, &opened) != ERROR_SUCCESS) return std::nullopt;
	std::optional<std::wstring> found;
	DWORD type = 0;
	DWORD size = 0;
	if (RegQueryValueExW(opened, nullptr, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)
		&& size > 0) {
		std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, L'\0');
		if (RegQueryValueExW(opened, nullptr, nullptr, &type, reinterpret_cast<BYTE *>(value.data()), &size) == ERROR_SUCCESS) {
			std::wstring text(value.data());
			if (type == REG_EXPAND_SZ) {
				const DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
				std::vector<wchar_t> expanded(needed + 1, L'\0');
				if (needed > 0 && ExpandEnvironmentStringsW(text.c_str(), expanded.data(), needed) > 0) text = expanded.data();
			}
			found = text;
		}
	}
	RegCloseKey(opened);
	return found;
}

// The new Outlook is a packaged application: it starts through its alias in
// this user's WindowsApps folder, as Windows' own Run box starts it.
std::optional<std::filesystem::path> packaged_alias(const char *program) {
	wchar_t local[4096] = {};
	const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
	if (length == 0 || length >= std::size(local)) return std::nullopt;
	const std::filesystem::path alias = std::filesystem::path(local) / L"Microsoft" / L"WindowsApps" / wide(program);
	// The alias is a link no program reads through: its attributes say it is there.
	if (GetFileAttributesW(alias.c_str()) == INVALID_FILE_ATTRIBUTES) return std::nullopt;
	return alias;
}

std::optional<std::filesystem::path> installed_program(OfficeApp app) {
	if (auto program = registered_program(windows_program_of(app))) return program;
	if (app != OfficeApp::outlook) return std::nullopt;
	if (auto alias = packaged_alias("olk.exe")) return alias;
	return registered_program("olk.exe");
}

} // namespace

std::optional<std::filesystem::path> registered_program(const std::string &program) {
	const std::wstring name = wide(program);
	const struct {
		HKEY root;
		REGSAM view;
	} places[] = {{HKEY_CURRENT_USER, 0}, {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}};
	for (const auto &place : places) {
		const auto value = app_paths_value(place.root, place.view, name);
		if (!value) continue;
		if (auto found = program_at(std::filesystem::path(*value).u8string())) return found;
	}
	return std::nullopt;
}

bool is_installed(OfficeApp app) {
	return installed_program(app).has_value();
}

OpenedApp open_application(OfficeApp app, const std::optional<std::filesystem::path> &file) {
	OpenedApp opened;
	// The program itself, by its full path, never a name searched for: a
	// program of that name elsewhere is never the one started.
	const auto program = installed_program(app);
	if (!program) {
		opened.problem = std::string(name_of(app)) + " is not installed on this computer.";
		return opened;
	}
	const std::wstring parameters = file ? L"\"" + file->wstring() + L"\"" : std::wstring();
	const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", program->c_str(),
		parameters.empty() ? nullptr : parameters.c_str(), nullptr, SW_SHOWNORMAL));
	opened.opened = result > 32;
	if (!opened.opened) opened.problem = std::string("Windows did not open ") + name_of(app) + ". Try again.";
	return opened;
}

#elif defined(__APPLE__)

bool is_installed(OfficeApp app) {
	if (is_installed_in(app, "/Applications")) return true;
	const char *home = std::getenv("HOME");
	return home != nullptr && *home != '\0' && is_installed_in(app, std::filesystem::path(home) / "Applications");
}

OpenedApp open_application(OfficeApp app, const std::optional<std::filesystem::path> &file) {
	OpenedApp opened;
	if (!is_installed(app)) {
		opened.problem = std::string(name_of(app)) + " is not installed on this Mac.";
		return opened;
	}
	// open -b: the application by its bundle identifier, with the file, as
	// arguments only; no shell reads any of it.
	std::string program = "/usr/bin/open";
	std::string by_bundle = "-b";
	std::string bundle = mac_bundle_of(app);
	std::string target = file ? file->u8string() : std::string();
	std::vector<char *> arguments{program.data(), by_bundle.data(), bundle.data()};
	if (file) arguments.push_back(target.data());
	arguments.push_back(nullptr);
	pid_t child = 0;
	if (posix_spawn(&child, program.c_str(), nullptr, nullptr, arguments.data(), environ) != 0) {
		opened.problem = std::string("macOS did not open ") + name_of(app) + ". Try again.";
		return opened;
	}
	// "open" answers at once, with whether macOS found the application.
	int status = 0;
	pid_t waited = -1;
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	opened.opened = waited != child || (WIFEXITED(status) && WEXITSTATUS(status) == 0);
	if (!opened.opened) opened.problem = std::string("macOS did not open ") + name_of(app) + ". Try again.";
	return opened;
}

#else

bool is_installed(OfficeApp) {
	return false;
}

OpenedApp open_application(OfficeApp, const std::optional<std::filesystem::path> &) {
	OpenedApp opened;
	opened.problem = "Word, Excel, PowerPoint and Outlook do not run on this system.";
	return opened;
}

#endif

} // namespace singularity::office
