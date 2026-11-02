#include "DocumentReview/MaterialCheck/MaterialCheck.hpp"
#include "DocumentReview/ColleagueWork/ColleagueWork.hpp"
#include "DocumentReview/Types/DocumentReview.hpp"
#include "DocumentIntelligence/LegalStructure/LegalStructure.hpp"
#include "DocumentIntelligence/SlideWriter/SlideWriter.hpp"
#include "DocumentIntelligence/SourceIntake/SourceIntake.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Document/XlsxWorkbookView/XlsxWorkbookView.hpp"
#include "Matter/Types/Matter.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace singularity::document_review {
namespace {

namespace intelligence = document_intelligence;
using Json = nlohmann::json;

std::string lowered(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

std::string trimmed(const std::string& text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::vector<std::string> cellsOf(const std::string& line) {
	std::vector<std::string> cells;
	std::size_t from = 0;
	for (std::size_t at = line.find(" | "); ; at = line.find(" | ", from)) {
		cells.push_back(trimmed(line.substr(from, at == std::string::npos ? std::string::npos : at - from)));
		if (at == std::string::npos) break;
		from = at + 3;
	}
	return cells;
}

// The places a heading names: "Section 3.12(a) - Material Contracts" is the
// place "3.12(a)"; "Schedule 1" is "schedule:1".
std::vector<std::string> placesNamedBy(const std::string& heading) {
	std::vector<std::string> keys;
	for (const intelligence::StatedValue& value : intelligence::valuesIn(heading)) {
		if (value.kind != "reference") continue;
		// "REF:<kind>:<number>"
		const std::size_t first = value.key.find(':');
		const std::size_t second = value.key.find(':', first + 1);
		if (first == std::string::npos || second == std::string::npos) continue;
		const std::string kind = value.key.substr(first + 1, second - first - 1);
		std::string number = value.key.substr(second + 1);
		const std::size_t item = number.find(":item:");
		if (item != std::string::npos) number = number.substr(0, item);
		const bool isDotted = number.find('.') != std::string::npos || number.find('(') != std::string::npos;
		keys.push_back(isDotted == true || kind == "section" ? number : kind + ":" + number);
	}
	return keys;
}

bool isWholeNumber(const std::string& text) {
	return text.empty() == false
		&& std::all_of(text.begin(), text.end(), [](unsigned char character) { return std::isdigit(character) != 0; });
}

// A line that heads what follows by the place it names, though Word draws no
// number for it: "Section 3.12(a) - Material Contracts" in a disclosure
// schedule. Short, no sentence, and the place first.
bool isPlaceHeading(const std::string& text) {
	const std::string line = trimmed(text);
	if (line.empty() == true || line.size() > 120 || line.find(". ") != std::string::npos || line.back() == '.'
			|| line.back() == ';')
		return false;
	for (const intelligence::StatedValue& value : intelligence::valuesIn(line)) {
		if (value.kind == "reference" && value.start == 0) return true;
	}
	return false;
}

// Whose tracked changes nobody has accepted a block holds: "Dana Whitfield
// (Corvane)", "Dana Whitfield (Corvane) and Unicorn"; "someone" unnamed.
std::string authorsOf(const std::vector<intelligence::PendingChange>& changes) {
	std::vector<std::string> authors;
	for (const intelligence::PendingChange& change : changes) {
		const std::string author = trimmed(change.author).empty() == true ? std::string("someone") : trimmed(change.author);
		if (std::find(authors.begin(), authors.end(), author) == authors.end()) authors.push_back(author);
	}
	std::string joined;
	for (std::size_t index = 0; index < authors.size(); ++index)
		joined += (index == 0 ? "" : index + 1 == authors.size() ? " and " : ", ") + authors[index];
	return joined.empty() == true ? std::string("someone") : joined;
}

// Whether a block's words, as they stand, are agreed (Statement::stand): a
// block no open change touches is; one inserted whole, or holding words someone
// inserted, is not yet. `line` narrows a table to one of its rows.
std::string standOf(const intelligence::LegalBlock& block, const std::string& line = std::string()) {
	if (block.isPendingInsertion == true)
		return "inserted whole in " + authorsOf(block.pendingChanges) + "'s tracked change, which nobody has accepted";
	if (block.pendingChanges.empty() == true) return "agreed";
	if (line.empty() == false) {
		std::vector<intelligence::PendingChange> touching;
		for (const intelligence::PendingChange& change : block.pendingChanges) {
			const std::string text = trimmed(change.text);
			if (change.kind == "inserted" && text.size() >= 2 && line.find(text) != std::string::npos) touching.push_back(change);
		}
		if (touching.empty() == true) return "agreed";
		return "it reads so with " + authorsOf(touching) + "'s tracked changes, which nobody has accepted";
	}
	return "it reads so with " + authorsOf(block.pendingChanges) + "'s tracked changes, which nobody has accepted";
}

void readAgreement(const intelligence::LegalDocument& document, DocumentFacts& facts, const std::string& stem) {

	std::string heading;
	// The place the last heading names, which the numbered items under it fill.
	std::size_t listing = std::string::npos;
	std::set<std::string> numbered;
	for (const intelligence::LegalBlock& block : document.blocks) {
		if (block.section != "body" && block.section.rfind("exhibit", 0) != 0) continue;
		std::string number = block.number;
		while (number.empty() == false && number.back() == '.') number.pop_back();
		const bool isNamedHeading = number.empty() == true && block.kind == intelligence::BlockKind::Paragraph
			&& isPlaceHeading(block.text);
		const bool isHeading = block.kind == intelligence::BlockKind::Heading || block.kind == intelligence::BlockKind::ExhibitHeading
			|| block.kind == intelligence::BlockKind::Title || isNamedHeading == true;
		// An item of the list a lettered or dotted place holds - item 1 of
		// Section 3.12(a) - is no section of the document.
		const bool isItem = isHeading == false && listing != std::string::npos && isWholeNumber(number) == true
			&& isWholeNumber(facts.places.places[listing].key) == false;
		if (number.empty() == false && isItem == false && numbered.insert(number).second == true) {
			facts.places.places.push_back(intelligence::FactPlace{number, "Section " + number
				+ (isHeading == true ? " (" + trimmed(block.text) + ")" : std::string()), {}});
		}
		if (isHeading == true) {
			heading = trimmed(block.text);
			listing = std::string::npos;
			for (const std::string& key : placesNamedBy(block.text)) {
				const auto known = std::find_if(facts.places.places.begin(), facts.places.places.end(),
					[&](const intelligence::FactPlace& place) { return place.key == key; });
				if (known != facts.places.places.end()) {
					listing = static_cast<std::size_t>(known - facts.places.places.begin());
					continue;
				}
				facts.places.places.push_back(intelligence::FactPlace{key, heading, {}});
				listing = facts.places.places.size() - 1;
			}
			continue;
		}
		if (listing != std::string::npos && number.empty() == false && block.kind != intelligence::BlockKind::Table)
			facts.places.places[listing].items.push_back(trimmed(block.text));
		const std::string under = isItem == true ? heading : block.heading.empty() == false ? block.heading : heading;
		if (block.kind == intelligence::BlockKind::Table) {
			std::vector<std::string> lines;
			std::size_t from = 0;
			while (from <= block.text.size()) {
				std::size_t end = block.text.find('\n', from);
				if (end == std::string::npos) end = block.text.size();
				const std::string line = trimmed(block.text.substr(from, end - from));
				if (line.empty() == false) lines.push_back(line);
				from = end + 1;
				if (end >= block.text.size()) break;
			}
			for (std::size_t index = 1; index < lines.size(); ++index) {
				intelligence::Statement statement;
				statement.place = (under.empty() == true ? std::string("A table") : under) + ", " + cellsOf(lines[index]).front();
				statement.label = lines.front();
				statement.subject = under.empty() == true ? stem : under;
				statement.text = lines[index];
				statement.stand = standOf(block, lines[index]);
				facts.statements.push_back(std::move(statement));
			}
			continue;
		}
		intelligence::Statement statement;
		statement.place = isItem == true ? heading + ", item " + number : number.empty() == false ? "Section " + number
			: under.empty() == false ? under : std::string("A paragraph");
		statement.label = under;
		statement.subject = stem;
		statement.text = trimmed(block.text);
		if (statement.text.empty() == true) continue;
		statement.stand = standOf(block);
		facts.statements.push_back(statement);
		// Its words before the changes nobody has accepted are what it read
		// before them, and may be what a source still gives.
		const std::string before = trimmed(block.originalText);
		if (before.empty() == false && before != statement.text) {
			statement.place += ", before its pending changes";
			statement.text = before;
			statement.stand = "agreed words, which " + authorsOf(block.pendingChanges)
				+ "'s tracked changes nobody has accepted would change";
			facts.statements.push_back(std::move(statement));
		}
	}

}

void readWorkbook(const std::filesystem::path& path, DocumentFacts& facts) {

	documents::DocumentExtractionRequest request;
	request.path = path;
	const documents::XlsxWorkbookViewResult view = documents::read_xlsx_workbook_view(request);
	if (view.status != documents::ExtractionStatus::ready) {
		facts.problem = "The workbook could not be read.";
		return;
	}
	for (const documents::SheetRender& sheet : view.workbook.sheets) {
		// The header: the first row with three words or more across it.
		std::size_t headerRow = std::string::npos;
		for (std::size_t index = 0; index < sheet.rows.size() && headerRow == std::string::npos; ++index) {
			std::size_t texts = 0;
			for (const documents::SheetRenderCell& cell : sheet.rows[index].cells) {
				if (cell.kind == documents::SheetCellKind::Text && trimmed(cell.text).empty() == false) ++texts;
			}
			if (texts >= 3) headerRow = index;
		}
		std::map<std::size_t, std::string> header;
		if (headerRow != std::string::npos) {
			for (const documents::SheetRenderCell& cell : sheet.rows[headerRow].cells) header[cell.column] = trimmed(cell.text);
		}
		for (std::size_t index = 0; index < sheet.rows.size(); ++index) {
			if (index == headerRow) continue;
			const documents::SheetRenderRow& row = sheet.rows[index];
			const bool isUnder = headerRow != std::string::npos && index > headerRow;
			// What the row is about: its first words, as its own first cells say.
			std::string subject;
			std::size_t named = 0;
			for (const documents::SheetRenderCell& cell : row.cells) {
				if (named >= 3 || cell.kind != documents::SheetCellKind::Text || trimmed(cell.text).empty() == true) continue;
				subject += (subject.empty() == true ? std::string() : " | ") + trimmed(cell.text);
				++named;
			}
			std::string left;
			for (const documents::SheetRenderCell& cell : row.cells) {
				const std::string text = trimmed(cell.text);
				if (text.empty() == true && cell.formula.has_value() == false) continue;
				intelligence::Statement statement;
				statement.place = sheet.name + "!" + cell.ref;
				statement.label = isUnder == true && header.count(cell.column) > 0 ? header[cell.column] : left;
				statement.subject = isUnder == true ? subject : std::string();
				statement.text = text;
				statement.standing = "summary";
				statement.isCalculated = cell.formula.has_value();
				if (cell.kind == documents::SheetCellKind::Text) left = text;
				facts.statements.push_back(std::move(statement));
			}
		}
	}

}

void readDeck(const std::filesystem::path& path, DocumentFacts& facts) {
	for (const intelligence::SlideParagraphs& slide : intelligence::readSlideParagraphs(path)) {
		const std::string title = slide.paragraphs.empty() == true ? std::string() : trimmed(slide.paragraphs.front());
		for (std::size_t index = 0; index < slide.paragraphs.size(); ++index) {
			intelligence::Statement statement;
			statement.place = "Slide " + std::to_string(slide.slideNumber);
			statement.subject = index == 0 ? std::string() : title;
			statement.text = trimmed(slide.paragraphs[index]);
			statement.standing = "summary";
			if (statement.text.empty() == false) facts.statements.push_back(std::move(statement));
		}
	}
	if (facts.statements.empty() == true) facts.problem = "The deck's slides could not be read.";
}

std::string fileKeyOf(const std::filesystem::path& path) {
	try {
		return platform::sha256_hex_digest_of_file(path) + lowered(path.extension().u8string());
	} catch (const std::exception&) {
		return std::string();
	}
}

std::string extensionOf(const std::string& name) {
	return lowered(std::filesystem::u8path(name).extension().u8string());
}

} // namespace

DocumentFacts factsOfDocument(const std::filesystem::path& path, const std::string& documentId, const std::string& name) {

	DocumentFacts facts;
	facts.places.documentId = documentId;
	facts.places.documentName = name;
	const std::string extension = lowered(path.extension().u8string());
	const std::string stem = std::filesystem::u8path(name).stem().u8string();
	try {
		if (extension == ".docx") {
			const intelligence::StructureReading read = intelligence::readDocxStructure(intelligence::takeInSource(path));
			if (read.isRead == false) facts.problem = read.problem;
			else readAgreement(read.document, facts, stem);
		} else if (extension == ".xlsx" || extension == ".xlsm") {
			readWorkbook(path, facts);
		} else if (extension == ".pptx") {
			readDeck(path, facts);
		} else {
			const DocumentWords words = wordsOfDocument(path, 200000);
			if (words.problem.empty() == false) {
				facts.problem = words.problem;
			} else {
				facts.statements = intelligence::statementsOfLines(words.text, documentId, name,
					extension == ".eml" || extension == ".msg" ? "summary" : "source");
			}
		}
	} catch (const std::exception& error) {
		facts.problem = error.what();
	}
	for (intelligence::Statement& statement : facts.statements) {
		statement.documentId = documentId;
		statement.documentName = name;
		intelligence::readValues(statement);
	}
	return facts;

}

DocumentFacts factsOfAgreement(const intelligence::LegalDocument& document, const std::string& documentId,
	const std::string& name) {
	DocumentFacts facts;
	facts.places.documentId = documentId;
	facts.places.documentName = name;
	readAgreement(document, facts, std::filesystem::u8path(name).stem().u8string());
	for (intelligence::Statement& statement : facts.statements) {
		statement.documentId = documentId;
		statement.documentName = name;
		intelligence::readValues(statement);
	}
	return facts;
}

const DocumentFacts& FactsReading::of(const std::filesystem::path& path, const std::string& documentId, const std::string& name) {
	const std::string key = fileKeyOf(path) + "\n" + documentId + "\n" + name;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto found = read_.find(key);
		if (found != read_.end()) return found->second;
	}
	DocumentFacts read = factsOfDocument(path, documentId, name);
	const std::lock_guard<std::mutex> lock(mutex_);
	return read_.emplace(key, std::move(read)).first->second;
}

MatterFacts matterFacts(const matters::Matter& matter, const std::string& workedOnDocumentId,
	const std::vector<std::string>& assignmentDraftIds, FactsReading& reading) {

	// Which draft stands for which original: this assignment's own, else the
	// latest one a lawyer accepted.
	std::map<std::string, std::string> standsFor;
	std::map<std::string, std::string> standsSince;
	std::set<std::string> drafts;
	std::string workedOnSource = workedOnDocumentId;
	for (const DocumentReview& review : matter.document_reviews) {
		for (const ReviewVersion& version : review.versions) {
			if (version.resultDocumentId.empty() == true) continue;
			drafts.insert(version.resultDocumentId);
			if (version.resultDocumentId == workedOnDocumentId) workedOnSource = review.sourceDocumentId;
			const bool isOurs = std::find(assignmentDraftIds.begin(), assignmentDraftIds.end(), version.resultDocumentId)
				!= assignmentDraftIds.end();
			const bool isAccepted = version.state == "accepted";
			if (isOurs == false && isAccepted == false) continue;
			const std::string rank = (isOurs == true ? "1" : "0") + version.createdAt;
			if (standsSince[review.sourceDocumentId] < rank) {
				standsSince[review.sourceDocumentId] = rank;
				standsFor[review.sourceDocumentId] = version.resultDocumentId;
			}
		}
	}
	// The emails colleagues drafted are their work, never a source.
	std::set<std::string> emails;
	for (const AssignmentRecord& record : matter.assignment_records) {
		if (record.email.documentId.empty() == false) emails.insert(record.email.documentId);
	}
	// The document worked on, and every version of it.
	std::set<std::string> family{workedOnDocumentId, workedOnSource};
	for (const DocumentReview& review : matter.document_reviews) {
		if (review.sourceDocumentId != workedOnSource) continue;
		for (const ReviewVersion& version : review.versions) family.insert(version.resultDocumentId);
	}

	MatterFacts facts;
	const std::filesystem::path folder = std::filesystem::u8path(matter.directory_path.value_or(std::string()));
	for (const matters::MatterDocument& document : matter.documents) {
		if (document.available == false || family.count(document.id) > 0 || emails.count(document.id) > 0) continue;
		const std::string name = document.name.empty() == true ? matters::placed_name(document) : document.name;
		if (std::filesystem::u8path(name).filename().u8string().rfind("~$", 0) == 0) continue;
		if (drafts.count(document.id) > 0) {
			bool isStanding = false;
			for (const auto& [source, draft] : standsFor) {
				if (draft == document.id && family.count(source) == 0) isStanding = true;
			}
			if (isStanding == false) continue;
		} else if (standsFor.count(document.id) > 0) {
			continue;
		}
		if (folder.empty() == true || documents::registry::is_relative_path_safe(folder, document.relative_path) == false) continue;
		const std::filesystem::path path = (folder / std::filesystem::u8path(document.relative_path)).lexically_normal();
		const std::string extension = extensionOf(name);
		if (extension != ".docx" && extension != ".xlsx" && extension != ".xlsm" && extension != ".pptx" && extension != ".pdf"
				&& extension != ".txt" && extension != ".md" && extension != ".eml" && extension != ".csv")
			continue;
		const DocumentFacts& read = reading.of(path, document.id, name);
		if (read.problem.empty() == false) continue;
		facts.sources.insert(facts.sources.end(), read.statements.begin(), read.statements.end());
		facts.documents.push_back(read.places);
	}
	return facts;

}

FactsCheck checkAgainstMatter(const std::vector<intelligence::Statement>& statements, const MatterFacts& facts) {

	FactsCheck check;
	check.checks = intelligence::checkFacts(statements, facts.sources);
	for (intelligence::FactCheck& reference : intelligence::checkReferences(statements, facts.documents))
		check.checks.push_back(std::move(reference));
	for (const intelligence::FactCheck& one : check.checks) {
		if (one.state == "agrees") ++check.agreeing;
		else if (one.state == "differs") ++check.differing;
		else if (one.state == "unclear") ++check.unclear;
		else ++check.unsourced;
	}
	return check;

}

std::vector<const intelligence::FactCheck*> differences(const FactsCheck& check, std::size_t most) {
	std::vector<const intelligence::FactCheck*> found;
	for (const intelligence::FactCheck& one : check.checks) {
		if (one.state == "differs" || one.state == "unclear") found.push_back(&one);
	}
	std::stable_sort(found.begin(), found.end(), [](const intelligence::FactCheck* left, const intelligence::FactCheck* right) {
		if ((left->state == "differs") != (right->state == "differs")) return left->state == "differs";
		return left->strength > right->strength;
	});
	if (found.size() > most) found.resize(most);
	return found;
}

std::string differencesText(const FactsCheck& check, std::size_t most) {
	std::string text;
	for (const intelligence::FactCheck* one : differences(check, most))
		text += "- " + std::string(one->state == "unclear" ? "Unclear: " : "") + intelligence::checkWords(*one) + "\n";
	return text;
}

Json factsCheckToJson(const FactsCheck& check) {
	Json listed = Json::array();
	for (const intelligence::FactCheck* one : differences(check, 40)) {
		Json evidence = Json::array();
		for (const intelligence::FactEvidence& given : one->evidence) {
			evidence.push_back(Json{{"documentId", given.documentId}, {"documentName", given.documentName}, {"place", given.place},
				{"quote", given.quote}, {"value", given.value}, {"isSame", given.isSame}});
		}
		listed.push_back(Json{{"place", one->place}, {"label", one->label}, {"value", one->value.text}, {"kind", one->value.kind},
			{"state", one->state}, {"words", intelligence::checkWords(*one)}, {"evidence", evidence}});
	}
	return Json{{"agreeing", check.agreeing}, {"differing", check.differing}, {"unclear", check.unclear},
		{"unsourced", check.unsourced}, {"differences", listed}};
}

Json originsToJson(const std::vector<intelligence::ValueOrigin>& origins) {
	Json listed = Json::array();
	for (const intelligence::ValueOrigin& origin : origins) {
		listed.push_back(Json{{"value", origin.value.text}, {"kind", origin.value.kind}, {"state", origin.state},
			{"documentName", origin.evidence.documentName}, {"place", origin.evidence.place}, {"quote", origin.evidence.quote}});
	}
	return listed;
}

} // namespace singularity::document_review
