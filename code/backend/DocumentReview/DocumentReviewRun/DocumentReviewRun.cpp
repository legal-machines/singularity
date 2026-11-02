#include "DocumentReview/DocumentReviewRun/DocumentReviewRun.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"
#include "DocumentReview/DocumentReviewActions/DocumentReviewActions.hpp"
#include "DocumentReview/ColleagueWork/ColleagueWork.hpp"
#include "DocumentReview/AssignmentRun/AssignmentRun.hpp"
#include "DocumentReview/MaterialCheck/MaterialCheck.hpp"
#include "DocumentIntelligence/ChangeAccount/ChangeAccount.hpp"
#include "DocumentIntelligence/ContextPreparation/ContextPreparation.hpp"
#include "DocumentIntelligence/IntegrityCheck/IntegrityCheck.hpp"
#include "DocumentIntelligence/LegalStructure/LegalStructure.hpp"
#include "DocumentIntelligence/ReviewResult/ReviewResult.hpp"
#include "DocumentIntelligence/RevisionWriter/RevisionWriter.hpp"
#include "DocumentIntelligence/SourceIntake/SourceIntake.hpp"
#include "Agent/AgentProduction/AgentProduction.hpp"
#include "Chat/ChatActions/ChatActions.hpp"
#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"
#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "Context/ProjectContextActions/ProjectContextActions.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "ExternalResearch/ContextBudget/ContextBudget.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"
#include "Tool/OperationAuthority/OperationAuthority.hpp"
#include "WorkProduct/Playbook/Playbook.hpp"
#include "WorkProduct/WorkProductActions/WorkProductActions.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <regex>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

namespace singularity::document_review {
namespace {

using Json = nlohmann::json;
namespace intelligence = document_intelligence;

// How much a review's answer may be, its thinking included, comes from the
// model, never from a number kept here: the most the model writes in one
// answer, as its provider (or Singularity, for a local model) reports it
// (routing::model_capability). A provider charges for what the model writes,
// not for the allowance; a fixed allowance below the model's own limit is what
// cut off reviews of longer agreements (the document review benchmark,
// 2026-10-24). With no figure reported, no limit is sent and the provider's own
// applies.

std::string lowered(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

std::string extensionOf(const std::string& path) {
	return lowered(std::filesystem::u8path(path).extension().u8string());
}

bool isReviewableExtension(const std::string& extension) {
	return extension == ".docx" || extension == ".pdf" || extension == ".txt" || extension == ".md";
}

long long millisecondsSince(const std::chrono::steady_clock::time_point& start) {
	return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count());
}

matters::Matter loadMatter(const std::string& matterId) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::optional<matters::Matter> matter = matters::load_matter(node::require_active_workspace_path(), matterId);
	if (matter.has_value() == false) throw ReviewRefused("This Matter is not open in Singularity on this computer.");
	return *matter;
}

// "s-10.1" is Section 10.1 to a lawyer; a comment is "a comment".
std::string placeWords(const intelligence::LegalDocument& document, const std::string& blockId) {
	const intelligence::LegalBlock* block = intelligence::findBlock(document, blockId);
	if (block == nullptr) return blockId;
	if (block->number.empty() == false) {
		std::string number = block->number;
		while (number.empty() == false && number.back() == '.') number.pop_back();
		const bool isSection = block->kind == intelligence::BlockKind::Clause || block->kind == intelligence::BlockKind::Heading;
		return isSection == true && number.empty() == false && std::isdigit(static_cast<unsigned char>(number.front())) != 0
			? "Section " + number : block->number;
	}
	if (block->kind == intelligence::BlockKind::Comment) return "a comment";
	return block->path.empty() == false ? block->path : blockId;
}

// Items in a sentence, each named once: a decision raised on nine blocks of an
// agreement's opening read "Opening, Opening, ... and Opening" (2026-10-31).
std::string listedWords(const std::vector<std::string>& items) {
	std::vector<std::string> once;
	for (const std::string& item : items) {
		if (std::find(once.begin(), once.end(), item) == once.end()) once.push_back(item);
	}
	std::string words;
	for (std::size_t index = 0; index < once.size(); ++index) {
		if (index > 0) words += index + 1 == once.size() ? " and " : ", ";
		words += once[index];
	}
	return words;
}

// A count as a colleague says it in a sentence: "two", "eleven" as "11".
std::string countWords(std::size_t count) {
	static const char* words[] = {"no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
	return count <= 10 ? std::string(words[count]) : std::to_string(count);
}

std::string capitalised(std::string words) {
	if (words.empty() == false) words[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(words[0])));
	return words;
}

// Sentences said one after another, each ending as a sentence does.
std::string joinedSentences(const std::vector<std::string>& sentences) {
	std::string words;
	for (std::string sentence : sentences) {
		while (sentence.empty() == false && std::isspace(static_cast<unsigned char>(sentence.back())) != 0) sentence.pop_back();
		if (sentence.empty() == true) continue;
		const char last = sentence.back();
		if (last != '.' && last != '?' && last != '!' && last != '"' && last != ')') sentence += ".";
		if (words.empty() == false) words += " ";
		words += sentence;
	}
	return words;
}

// ---- The source ----------------------------------------------------------------

struct SourceChoice {
	const matters::MatterDocument* document = nullptr;
	// When none could be chosen: what the colleague asks.
	std::string question;
};

// The documents a review can be of: the Matter's own, readable, of a kind it
// reviews, and never a draft a review wrote.
std::vector<const matters::MatterDocument*> reviewableDocuments(const matters::Matter& matter) {

	std::vector<std::string> written;
	for (const DocumentReview& review : matter.document_reviews) {
		for (const ReviewVersion& version : review.versions) {
			if (version.resultDocumentId.empty() == false) written.push_back(version.resultDocumentId);
		}
	}
	std::vector<const matters::MatterDocument*> documents;
	for (const matters::MatterDocument& document : matter.documents) {
		if (document.available == false) continue;
		const std::string path = matters::placed_name(document);
		if (isReviewableExtension(extensionOf(path)) == false) continue;
		if (std::find(written.begin(), written.end(), document.id) != written.end()) continue;
		documents.push_back(&document);
	}
	return documents;

}

// The version of the same agreement the Matter held before a clean copy of it
// (D-111): a Word file of the same name before its " - " ("Harborline Mutual
// NDA - Harborline comments 10-24" is a version of "Harborline Mutual NDA"),
// added before the copy, a colleague's draft included - the other side may
// have worked on the draft it was sent - and, of those, the one the copy
// changes least: the one the other side worked from. The latest added where
// two are as close.
struct EarlierVersion {
	std::string documentId;
	std::string name;
	std::string addedAt;
	std::vector<intelligence::ClauseChange> changes;
};

std::optional<EarlierVersion> earlierVersionOf(const matters::Matter& matter, const matters::MatterDocument& source,
	const std::filesystem::path& folder, const intelligence::LegalDocument& copy) {

	const auto familyOf = [](const matters::MatterDocument& document) {
		std::string stem = lowered(std::filesystem::u8path(matters::placed_name(document)).stem().u8string());
		const std::size_t dash = stem.find(" - ");
		if (dash != std::string::npos) stem.erase(dash);
		while (stem.empty() == false && stem.back() == ' ') stem.pop_back();
		return stem;
	};
	const std::string family = familyOf(source);
	if (family.size() < 4) return std::nullopt;
	std::optional<EarlierVersion> closest;
	for (const matters::MatterDocument& candidate : matter.documents) {
		if (candidate.id == source.id || candidate.available == false || familyOf(candidate) != family) continue;
		if (candidate.added_at.empty() == false && source.added_at.empty() == false && candidate.added_at > source.added_at) continue;
		if (lowered(extensionOf(matters::placed_name(candidate))) != ".docx") continue;
		if (documents::registry::is_relative_path_safe(folder, candidate.relative_path) == false) continue;
		const std::filesystem::path path = (folder / std::filesystem::u8path(candidate.relative_path)).lexically_normal();
		const intelligence::StructureReading read = intelligence::readDocxStructure(intelligence::takeInSource(path));
		if (read.isRead == false) continue;
		std::vector<intelligence::ClauseChange> changes = intelligence::clauseChanges(read.document, copy);
		const bool isCloser = closest.has_value() == false || changes.size() < closest->changes.size()
			|| (changes.size() == closest->changes.size() && candidate.added_at > closest->addedAt);
		if (isCloser == false) continue;
		closest = EarlierVersion{candidate.id, candidate.name, candidate.added_at, std::move(changes)};
	}
	return closest;

}

SourceChoice chooseSource(const matters::Matter& matter, const std::string& chatId, const std::string& instruction,
	const std::string& requestMessageId) {

	SourceChoice choice;
	const std::vector<const matters::MatterDocument*> all = reviewableDocuments(matter);
	// A document the request hands over, as a file on the message, before all.
	for (const matters::MatterTask& message : matter.tasks) {
		if (message.id != requestMessageId || message.attachment_document_ids.empty() == true) continue;
		for (const std::string& attached : message.attachment_document_ids) {
			for (const matters::MatterDocument* document : all) {
				if (document->id == attached) {
					choice.document = document;
					return choice;
				}
			}
		}
	}
	// A colleague's own drafts are its work, not documents to review, unless the
	// request hands one over or the thread works on it.
	std::set<std::string> drafts;
	for (const DocumentReview& review : matter.document_reviews) {
		for (const ReviewVersion& version : review.versions) {
			if (version.resultDocumentId.empty() == false) drafts.insert(version.resultDocumentId);
		}
	}
	std::vector<const matters::MatterDocument*> documents;
	for (const matters::MatterDocument* document : all) {
		if (drafts.count(document->id) == 0) documents.push_back(document);
	}
	// The thread's own document, where it has one.
	for (const matters::MatterChat& chat : matter.chats) {
		if (chat.id != chatId || chat.working_document_id.empty() == true) continue;
		for (const matters::MatterDocument* document : all) {
			if (document->id == chat.working_document_id) {
				choice.document = document;
				return choice;
			}
		}
	}
	// The one the request names, by its file name or the name without its kind.
	const std::string request = lowered(instruction);
	std::vector<const matters::MatterDocument*> named;
	for (const matters::MatterDocument* document : documents) {
		const std::filesystem::path path = std::filesystem::u8path(matters::placed_name(*document));
		const std::string stem = lowered(path.stem().u8string());
		if (stem.size() >= 4 && request.find(stem) != std::string::npos) named.push_back(document);
	}
	if (named.size() == 1) {
		choice.document = named.front();
		return choice;
	}
	// Or as a lawyer names it in passing: "Harborline's NDA", "the Corvane
	// markup" - by the words of its name that no other document's name has.
	// Words every candidate shares ("NDA" among NDAs) tell nothing apart.
	const auto wordsOf = [](const std::string& text) {
		std::set<std::string> words;
		std::string word;
		for (const char character : text + " ") {
			if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
				word += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
				continue;
			}
			if (word.size() >= 3) words.insert(word);
			word.clear();
		}
		return words;
	};
	if (named.empty() == true && documents.size() > 1) {
		std::map<std::string, int> owners;
		std::vector<std::set<std::string>> nameWords;
		for (const matters::MatterDocument* document : documents) {
			nameWords.push_back(wordsOf(std::filesystem::u8path(matters::placed_name(*document)).stem().u8string()));
			for (const std::string& word : nameWords.back()) ++owners[word];
		}
		const std::set<std::string> asked = wordsOf(instruction);
		std::vector<const matters::MatterDocument*> meant;
		for (std::size_t index = 0; index < documents.size(); ++index) {
			const bool isMeant = std::any_of(nameWords[index].begin(), nameWords[index].end(), [&](const std::string& word) {
				return owners[word] == 1 && std::isdigit(static_cast<unsigned char>(word.front())) == 0 && asked.count(word) > 0;
			});
			if (isMeant == true) meant.push_back(documents[index]);
		}
		if (meant.size() == 1) {
			choice.document = meant.front();
			return choice;
		}
	}
	if (documents.size() == 1) {
		choice.document = documents.front();
		return choice;
	}
	if (documents.empty() == true) {
		choice.question = "I don't see a document in this Matter I can review: add the agreement to the Matter's "
			"documents (Word, PDF or text), then ask me again.";
		return choice;
	}
	std::vector<std::string> names;
	for (const matters::MatterDocument* document : documents) names.push_back(document->name);
	std::string choices;
	for (std::size_t index = 0; index < names.size(); ++index) {
		if (index > 0) choices += index + 1 == names.size() ? " or " : ", ";
		choices += names[index];
	}
	choice.question = "Which document should I review: " + choices + "?";
	return choice;

}

// ---- Which review a request is about (D-115) ---------------------------------------

bool isOpen(const DocumentReview& review) {
	return review.versions.empty() == false && review.versions.back().state != "accepted";
}

// The review of `sourceId` this colleague has open in the thread: the next
// version of it, where an assignment works on that document again.
std::optional<DocumentReview> openReviewOf(const matters::Matter& matter, const std::string& chatId,
	const std::string& agentId, const std::string& sourceId) {
	std::optional<DocumentReview> latest;
	for (const DocumentReview& review : matter.document_reviews) {
		if (review.chatId != chatId || review.agentId != agentId || review.sourceDocumentId != sourceId) continue;
		if (isOpen(review) == false) continue;
		if (latest.has_value() == false || review.createdAt > latest->createdAt) latest = review;
	}
	return latest;
}

// The review a request is about, where a thread holds several: the one the
// lawyer's message returns, the one it tries again, else the latest this
// colleague has open there.
std::optional<DocumentReview> reviewAskedAbout(const matters::Matter& matter, const std::string& chatId,
	const std::string& agentId, const std::string& requestMessageId) {
	std::optional<DocumentReview> latest;
	for (const DocumentReview& review : matter.document_reviews) {
		if (review.chatId != chatId || review.agentId != agentId || review.versions.empty() == true) continue;
		if (requestMessageId.empty() == false) {
			for (const ReviewDecision& decision : review.decisions) {
				if (decision.messageId == requestMessageId) return review;
			}
			if (review.versions.back().requestMessageId == requestMessageId && isOpen(review) == true) return review;
		}
		if (latest.has_value() == false || review.createdAt > latest->createdAt) latest = review;
	}
	if (latest.has_value() == false || isOpen(*latest) == false) return std::nullopt;
	return latest;
}

// The documents a part of an assignment rests on, read as the colleague reads
// them: each at most kMostSupportingCharacters, all at most
// kMostSupportingTotal, the rest left out and said to be.
constexpr std::size_t kMostSupportingCharacters = 40000;
constexpr std::size_t kMostSupportingTotal = 120000;

std::vector<intelligence::SupportingMaterial> supportingOf(const matters::Matter& matter, const std::filesystem::path& folder,
	const std::vector<std::string>& ids, const std::string& sourceId) {
	std::vector<intelligence::SupportingMaterial> material;
	std::size_t total = 0;
	for (const std::string& id : ids) {
		if (id == sourceId || total >= kMostSupportingTotal) continue;
		for (const matters::MatterDocument& document : matter.documents) {
			if (document.id != id || document.available == false) continue;
			if (folder.empty() == true || documents::registry::is_relative_path_safe(folder, document.relative_path) == false) break;
			const std::filesystem::path path = (folder / std::filesystem::u8path(document.relative_path)).lexically_normal();
			const DocumentWords words = wordsOfDocument(path, std::min(kMostSupportingCharacters, kMostSupportingTotal - total));
			intelligence::SupportingMaterial entry;
			entry.name = document.name;
			entry.text = words.problem.empty() == false ? "(" + words.problem + ")" : words.text;
			total += entry.text.size();
			material.push_back(std::move(entry));
			break;
		}
	}
	return material;
}

// The clauses an instruction names by the agreement's own numbers ("2.1",
// "Section 8.1"), as a lawyer says them.
std::vector<std::string> placesNamedIn(const std::string& instruction, const intelligence::LegalDocument& document) {
	static const std::regex numbered("(?:^|[^0-9.])([0-9]{1,2}(?:\\.[0-9]{1,2}){1,3})(?![0-9])");
	std::vector<std::string> places;
	for (std::sregex_iterator found(instruction.begin(), instruction.end(), numbered), end; found != end; ++found) {
		const std::string number = (*found)[1].str();
		for (const intelligence::LegalBlock& block : document.blocks) {
			std::string own = block.number;
			while (own.empty() == false && own.back() == '.') own.pop_back();
			if (own != number) continue;
			const std::string place = "Section " + number;
			if (std::find(places.begin(), places.end(), place) == places.end()) places.push_back(place);
		}
	}
	return places;
}

// ---- Reading it, once per content ----------------------------------------------

std::filesystem::path structureCachePath(const std::string& sha256) {
	std::string version = intelligence::legalStructureVersion();
	std::replace(version.begin(), version.end(), '/', '-');
	return platform::computer_records_directory() / "DocumentIntelligence" / "structures" / (sha256 + "." + version + ".json");
}

// The source's legal structure, read again only when its bytes change: the
// reading is kept by the SHA-256 of the file and the structure's version.
intelligence::StructureReading readStructure(const std::filesystem::path& path, const intelligence::SourceIntake& intake,
	bool& isFromCache) {

	isFromCache = false;
	const std::filesystem::path cache = structureCachePath(intake.identity.sha256);
	{
		std::ifstream input(cache, std::ios::binary);
		if (input) {
			std::stringstream text;
			text << input.rdbuf();
			const Json value = Json::parse(text.str(), nullptr, false);
			if (value.is_object() == true) {
				intelligence::StructureReading reading;
				reading.isRead = true;
				reading.document = intelligence::legalDocumentFromJson(value);
				if (reading.document.source.sha256 == intake.identity.sha256) {
					isFromCache = true;
					return reading;
				}
			}
		}
	}
	intelligence::StructureReading reading;
	if (intake.identity.format == "docx") {
		reading = intelligence::readDocxStructure(intake);
	} else {
		documents::DocumentExtractionRequest request;
		request.path = path;
		const documents::DocumentExtractionResult extracted = documents::LocalDocumentExtractor().extract(request);
		if (extracted.status != documents::ExtractionStatus::ready) {
			reading.problem = "Singularity could not read the document's text.";
			return reading;
		}
		reading = intelligence::readTextStructure(intake.identity, extracted.text);
	}
	if (reading.isRead == true) {
		try {
			std::filesystem::create_directories(cache.parent_path());
			platform::write_file_durably(cache, intelligence::legalDocumentToJson(reading.document).dump());
		} catch (const std::exception&) {
			// A reading not kept is read again next time; nothing else depends on it.
		}
	}
	return reading;

}

// What the structure showed, without the document's words.
// The capability snapshot a review relied on, kept with what it prepared.
Json capabilityToJson(const routing::ModelCapability& capability) {
	const auto number = [](const auto& value) { return value.has_value() == true ? Json(*value) : Json(); };
	return Json{{"source", capability.source}, {"contextWindowTokens", number(capability.context_window_tokens)},
		{"maxOutputTokens", number(capability.max_output_tokens)}, {"inputPricePerToken", number(capability.input_price_per_token)},
		{"outputPricePerToken", number(capability.output_price_per_token)}, {"discoveredAt", capability.discovered_at}};
}

std::string structureSummary(const intelligence::LegalDocument& document) {

	std::size_t clauses = 0;
	for (const intelligence::LegalBlock& block : document.blocks) {
		if (block.kind == intelligence::BlockKind::Clause) ++clauses;
	}
	Json missing = Json::array();
	Json present = Json::array();
	for (const intelligence::ExhibitStatus& exhibit : document.exhibits) {
		std::vector<std::string> from;
		for (const std::string& id : exhibit.referencedIn) from.push_back(placeWords(document, id));
		if (exhibit.isPresent == true) present.push_back(Json{{"label", exhibit.label}, {"title", exhibit.title}});
		else missing.push_back(Json{{"label", exhibit.label}, {"referencedIn", from}});
	}
	Json references = Json::array();
	for (const intelligence::CrossReference& reference : document.references) {
		if (reference.kind != "section" || reference.isResolved == true) continue;
		references.push_back(Json{{"label", reference.label}, {"referencedIn", placeWords(document, reference.fromBlockId)}});
	}
	Json instructions = Json::array();
	for (const intelligence::EmbeddedInstruction& instruction : document.embeddedInstructions) {
		const intelligence::LegalBlock* block = intelligence::findBlock(document, instruction.blockId);
		instructions.push_back(Json{{"blockId", instruction.blockId},
			{"author", block != nullptr ? block->author : std::string()},
			{"on", block != nullptr && block->annotates.empty() == false ? placeWords(document, block->annotates) : std::string()}});
	}
	Json properties = Json::array();
	for (const intelligence::DocumentProperty& property : document.properties) properties.push_back(property.name);
	return Json{{"structureVersion", document.structureVersion}, {"sha256", document.source.sha256},
		{"format", document.source.format}, {"title", document.title}, {"blocks", document.blocks.size()},
		{"clauses", clauses}, {"definitions", document.definitions.size()}, {"references", document.references.size()},
		{"missingExhibits", missing}, {"presentExhibits", present}, {"unresolvedReferences", references},
		{"embeddedInstructions", instructions}, {"propertiesKeptHere", properties},
		{"pendingRevisions", document.pendingRevisions}, {"limitations", document.limitations}}.dump();

}

// Two readings of a clause are the same when they differ only in the run of
// their spaces or the shape of their quotes: what a lawyer would not see.
// A space before a comma is seen ("evaluating , negotiating").
std::string asRead(const std::string& text) {
	std::string plain;
	bool isSpace = false;
	for (std::size_t at = 0; at < text.size(); ++at) {
		const unsigned char character = static_cast<unsigned char>(text[at]);
		// Typographic quotes (U+2018, U+2019, U+201C, U+201D) as plain ones.
		if (character == 0xE2 && at + 2 < text.size() && static_cast<unsigned char>(text[at + 1]) == 0x80) {
			const unsigned char last = static_cast<unsigned char>(text[at + 2]);
			if (last == 0x98 || last == 0x99 || last == 0x9C || last == 0x9D) {
				if (isSpace == true && plain.empty() == false) plain += ' ';
				isSpace = false;
				plain += last == 0x98 || last == 0x99 ? '\'' : '"';
				at += 2;
				continue;
			}
		}
		if (std::isspace(character) != 0) {
			isSpace = true;
			continue;
		}
		if (isSpace == true && plain.empty() == false) plain += ' ';
		isSpace = false;
		plain += static_cast<char>(character);
	}
	return plain;
}

// The changes a version proposed, as the next version is told them, with how
// each clause reads in that version's draft where Word shows it otherwise
// than proposed, or where the change never reached the draft (D-102): the
// next version starts from what the lawyer read, not from what was meant.
std::string previousChangesOf(const ReviewVersion& version, const intelligence::LegalDocument* draft = nullptr) {

	const Json result = Json::parse(version.result, nullptr, false);
	if (result.is_object() == false) return std::string();
	// In the form the answer takes (review-preparation/6): each clause as the
	// version had it read. A version written before kept quoted words.
	Json changes = Json::array();
	for (const Json& change : result.value("changes", Json::array())) {
		const std::string action = change.value("action", std::string());
		const std::string proposed = change.value("proposedText", std::string());
		Json item = Json{{"positionId", change.value("positionId", std::string())}, {"clause", change.value("blockId", std::string())}};
		const std::string writeState = change.value("writeState", std::string());
		if (writeState != "written") {
			const std::string note = change.value("writeNote", std::string()).empty() == false
				? change.value("writeNote", std::string()) : change.value("checkNote", std::string());
			item["inTheDraft"] = "Not in the draft. " + note;
		} else if (draft != nullptr && (action == "rewrite" || action == "table")) {
			const intelligence::LegalBlock* block = intelligence::findBlock(*draft, change.value("blockId", std::string()));
			if (block != nullptr && asRead(block->text) != asRead(proposed)) item["inTheDraft"] = block->text;
		}
		// A table is answered as any clause is: as it should read.
		if (action == "rewrite" || action == "table") {
			item["action"] = "rewrite";
			item["text"] = proposed;
		} else if (action == "insert_after") {
			item["action"] = "add_after";
			item["text"] = proposed;
		} else if (proposed == intelligence::kOmittedClause) {
			item["action"] = "strike";
			item["text"] = "";
		} else {
			item["action"] = action;
			item["currentText"] = change.value("currentText", std::string());
			item["proposedText"] = proposed;
		}
		changes.push_back(std::move(item));
	}
	return changes.dump(1);

}

std::vector<intelligence::ReviewPosition> positionsOf(const playbooks::Playbook& playbook) {

	std::vector<intelligence::ReviewPosition> positions;
	for (const playbooks::Position& source : playbook.positions) {
		intelligence::ReviewPosition position;
		position.id = source.id;
		position.topic = source.topic;
		position.question = source.question;
		position.standard = source.standard;
		position.standardClause = source.standard_clause;
		for (const playbooks::Fallback& fallback : source.fallbacks) {
			position.fallbacks.push_back(fallback.clause.empty() == true ? fallback.position
				: fallback.position + " Clause: \"" + fallback.clause + "\"");
		}
		position.redLine = source.red_line;
		position.severity = source.severity;
		position.guidance = source.guidance;
		positions.push_back(position);
	}
	return positions;

}

// ---- The model -----------------------------------------------------------------

// ---- What the colleague says -----------------------------------------------------

// What an earlier version of the review already raised: its open points' kinds
// and places, from each version's validated result.
std::vector<intelligence::UnresolvedPoint> raisedBefore(const DocumentReview& review, int versionNumber) {
	std::vector<intelligence::UnresolvedPoint> raised;
	for (const ReviewVersion& version : review.versions) {
		if (version.number >= versionNumber || version.result.empty() == true) continue;
		const Json result = Json::parse(version.result, nullptr, false);
		if (result.is_object() == false || result.contains("unresolved") == false || result.at("unresolved").is_array() == false) continue;
		for (const Json& entry : result.at("unresolved")) {
			if (entry.is_object() == false) continue;
			intelligence::UnresolvedPoint point;
			point.kind = entry.value("kind", std::string());
			if (entry.contains("blockIds") == true && entry.at("blockIds").is_array() == true) {
				for (const Json& id : entry.at("blockIds")) {
					if (id.is_string() == true) point.blockIds.push_back(id.get<std::string>());
				}
			}
			raised.push_back(point);
		}
	}
	return raised;
}

bool wasRaised(const std::vector<intelligence::UnresolvedPoint>& raised, const std::string& kind,
	const std::vector<std::string>& blockIds) {
	for (const intelligence::UnresolvedPoint& point : raised) {
		if (point.kind != kind) continue;
		if (point.blockIds.empty() == true && blockIds.empty() == true) return true;
		for (const std::string& id : blockIds) {
			if (std::find(point.blockIds.begin(), point.blockIds.end(), id) != point.blockIds.end()) return true;
		}
	}
	return false;
}

// ---- What the colleague says, from what happened (D-101) --------------------------

// A point the review found off the firm's position that the draft does not
// change: how it is named to the lawyer, the names a message may call it by,
// whether a change was proposed for it that could not be written, and why.
struct UnchangedPoint {
	std::string label;
	std::vector<std::string> names;
	bool isProposed = false;
	std::string why;
	// Off the firm's position and unchanged in the last version too: said to
	// the lawyer then, so said again only where the lawyer asks about it now.
	bool isSaidBefore = false;
};

// A clause as the drafts read it in Word, every tracked change accepted: in
// the last version's draft, and in this one's (D-102).
struct ClauseReading {
	std::string place;
	std::string before;
	std::string now;
};

// A revision measured against the draft the lawyer read: the clauses the
// lawyer's words name, the others that now read differently, and whether the
// revision changed anything at all - a revision that changed nothing is no
// version to accept, and the colleague's own words on why stand with it.
struct DraftComparison {
	int previousVersion = 0;
	bool isUnchanged = false;
	std::string why;
	std::vector<ClauseReading> named;
	std::vector<ClauseReading> otherChanged;
};

// What happened when the review's work was done, as Singularity recorded it
// once the draft was written and read back (D-101, layer 4): the authority on
// what the colleague may say. Nothing in it is what the model meant to do.
struct HandInFacts {
	int version = 1;
	std::string lawyer;
	std::string request;
	std::string sourceName;
	std::string playbookName;
	// The draft attached, by its name; or, when there is none, why:
	// "not_word", "nothing_to_change" or "none_written".
	std::string draftName;
	std::string noDraft;
	// What the draft changes, clause by clause, and what the last version
	// changed that this one no longer does.
	std::vector<std::string> written;
	std::vector<std::string> dropped;
	std::vector<UnchangedPoint> unchanged;
	std::vector<std::string> draftProblems;
	// Comments in the draft's margin, as read back from it, and those meant for
	// it that are not in it, each with why (D-117).
	std::vector<std::string> commented;
	std::vector<std::string> notCommented;
	std::vector<std::string> calls;
	std::vector<std::string> unassessed;
	std::vector<std::string> lacking;
	// What the document lacks that no earlier version said: an exhibit or a
	// Section, by its name.
	std::vector<std::string> freshlyLacking;
	// Changes the review proposed that could not be used at all, each with why:
	// none of them is in the draft.
	std::vector<std::string> setAside;
	std::vector<std::string> notes;
	std::vector<std::string> meets;
	// The colleague's own view of the agreement, written as it reviewed it,
	// before the draft existed.
	std::string view;
	// For a revision: the drafts compared (D-102), and how many of its written
	// changes read as they did in the last version's draft.
	std::optional<DraftComparison> comparison;
	std::size_t carriedOver = 0;
	// What an earlier version's draft showed otherwise than meant that this
	// draft reads as meant: settled, though the thread still speaks of it.
	std::vector<std::string> resolved;
};

// A change, as a lawyer says what it does: the words it strikes and the words
// it puts in their place, each a phrase - “evaluating and negotiating” becomes
// “evaluating, negotiating and consummating”, never “ ” becomes
// “, negotiating ” as the redline's pieces would say it (a live run, 25 Oct
// 2026).
std::string whatItDoes(const intelligence::ProposedChange& change) {
	if (change.action == "table") {
		std::vector<std::string> said;
		for (const intelligence::TableEdit& edit : change.tableEdits) {
			if (edit.kind == "cell") said.push_back("“" + edit.currentText + "” becomes “" + edit.proposedText + "”");
			else if (edit.kind == "add_row") said.push_back("adds the row “" + edit.proposedText + "”");
			else if (edit.kind == "remove_row") said.push_back("strikes the row “" + edit.currentText + "”");
		}
		if (said.empty() == false) return listedWords(said);
	}
	if (change.action == "rewrite") {
		std::vector<std::string> said;
		for (const intelligence::WordEdit& edit : intelligence::wordEdits(change.currentText, change.proposedText)) {
			if (edit.removed.empty() == true) said.push_back("adds “" + edit.added + "”");
			else if (edit.added.empty() == true) said.push_back("strikes “" + edit.removed + "”");
			else said.push_back("“" + edit.removed + "” becomes “" + edit.added + "”");
		}
		if (said.empty() == false) return listedWords(said);
	}
	if (change.action == "rewrite" && change.redline.empty() == false) {
		std::vector<std::string> edits;
		std::string struck;
		for (const intelligence::RedlinePiece& piece : change.redline) {
			if (piece.kind == "struck") {
				struck += piece.text;
			} else if (piece.kind == "inserted") {
				edits.push_back(struck.empty() == true ? "adds “" + piece.text + "”"
					: "“" + struck + "” becomes “" + piece.text + "”");
				struck.clear();
			} else if (struck.empty() == false) {
				edits.push_back("strikes “" + struck + "”");
				struck.clear();
			}
		}
		if (struck.empty() == false) edits.push_back("strikes “" + struck + "”");
		if (edits.empty() == false) return listedWords(edits);
	}
	if (change.action == "insert_after") return "adds after it: “" + change.proposedText + "”";
	if (change.proposedText == intelligence::kOmittedClause) return "struck whole, as “" + intelligence::kOmittedClause + "”";
	if (change.action == "delete") return "strikes “" + change.currentText + "”";
	return "“" + change.currentText + "” becomes “" + change.proposedText + "”";
}

// "Section 5.2 (payment terms)": a place, and what it is about.
std::string labelOf(const std::vector<std::string>& places, const std::string& topic) {
	if (places.empty() == true) return topic;
	if (topic.empty() == true) return listedWords(places);
	std::string about = topic;
	// A topic's first word is lowered inside a sentence, unless it is a name
	// written in capitals (NDA) or a defined word the Playbook capitalises inside.
	if (about.size() > 1 && std::isupper(static_cast<unsigned char>(about[0])) != 0
		&& std::isupper(static_cast<unsigned char>(about[1])) == 0) {
		about[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(about[0])));
	}
	return listedWords(places) + " (" + about + ")";
}

HandInFacts gatherFacts(const ReviewRunInput& input, const DocumentReview& review, int versionNumber,
	const intelligence::LegalDocument& document, const intelligence::ValidatedReview& validated,
	const intelligence::RevisionOutcome& revision, bool isDocx, const intelligence::IntegrityReport& integrity,
	const DraftComparison* comparison = nullptr) {

	HandInFacts facts;
	if (comparison != nullptr) facts.comparison = *comparison;
	facts.version = versionNumber;
	facts.lawyer = input.givenByName;
	facts.request = input.instruction;
	facts.sourceName = review.sourceFileName;
	facts.playbookName = review.playbookName;
	facts.view = validated.summary;
	std::map<std::string, std::string> topics;
	for (const intelligence::PositionAssessment& assessment : validated.assessments) topics[assessment.positionId] = assessment.topic;
	const auto placeOf = [&](const std::string& clauseNumber, const std::string& blockId) {
		return clauseNumber.empty() == false ? "Section " + clauseNumber : placeWords(document, blockId);
	};

	if (revision.isWritten == true) facts.draftName = revision.path.filename().u8string();

	// What the last version's draft changed, clause by clause, to say what
	// this one does differently.
	std::map<std::string, Json> before;
	for (const ReviewVersion& earlier : review.versions) {
		if (earlier.number != versionNumber - 1 || earlier.result.empty() == true) continue;
		const Json result = Json::parse(earlier.result, nullptr, false);
		if (result.is_object() == false) continue;
		for (const Json& change : result.value("changes", Json::array())) {
			if (change.is_object() == false || change.value("writeState", std::string()) != "written") continue;
			before[change.value("blockId", std::string())] = change;
		}
	}
	const std::string previous = std::to_string(versionNumber - 1);
	// What the last version left off the firm's position, unchanged.
	std::set<std::string> offBefore;
	for (const ReviewVersion& earlier : review.versions) {
		if (earlier.number != versionNumber - 1 || earlier.result.empty() == true) continue;
		const Json result = Json::parse(earlier.result, nullptr, false);
		if (result.is_object() == false) continue;
		std::set<std::string> writtenPositions;
		std::set<std::string> writtenBlocks;
		for (const Json& change : result.value("changes", Json::array())) {
			if (change.is_object() == false) continue;
			// A change the last version could not write was said then, too.
			if (change.value("writeState", std::string()) != "written") {
				offBefore.insert(change.value("positionId", std::string()));
				continue;
			}
			writtenPositions.insert(change.value("positionId", std::string()));
			writtenBlocks.insert(change.value("blockId", std::string()));
		}
		for (const Json& assessment : result.value("assessments", Json::array())) {
			if (assessment.is_object() == false) continue;
			const std::string state = assessment.value("assessment", std::string());
			if (state != "deviates" && state != "missing") continue;
			const std::string positionId = assessment.value("positionId", std::string());
			bool isTouched = writtenPositions.count(positionId) > 0;
			for (const Json& block : assessment.value("blockIds", Json::array())) {
				if (block.is_string() == true && writtenBlocks.count(block.get<std::string>()) > 0) isTouched = true;
			}
			if (isTouched == false) offBefore.insert(positionId);
		}
	}

	std::set<std::string> changedHere;
	for (const intelligence::ProposedChange& change : validated.changes) {
		const std::string topic = topics.count(change.positionId) > 0 ? topics[change.positionId] : std::string();
		const std::string place = placeOf(change.clauseNumber, change.blockId);
		// Written or not, this version says what becomes of the clause.
		changedHere.insert(change.blockId);
		if (change.writeState == "written") {
			std::string since;
			if (versionNumber > 1) {
				const auto earlier = before.find(change.blockId);
				since = earlier == before.end() ? std::string(" (new in this version)")
					: earlier->second.value("proposedText", std::string()) == change.proposedText
						? " (as in version " + previous + ")" : " (revised since version " + previous + ")";
			}
			if (versionNumber > 1 && since == " (as in version " + previous + ")" && change.writeNote.empty() == true) {
				++facts.carriedOver;
				continue;
			}
			facts.written.push_back(labelOf({place}, topic) + since + ": " + whatItDoes(change)
				+ (change.explanation.empty() == true ? std::string() : ". Why: " + change.explanation));
			// Word shows it otherwise than meant: the draft itself needs a look.
			if (change.writeNote.empty() == false) facts.draftProblems.push_back(labelOf({place}, topic) + ": " + change.writeNote);
			continue;
		}
		UnchangedPoint point;
		point.label = labelOf({place}, topic);
		point.names = {place};
		if (topic.empty() == false) point.names.push_back(topic);
		point.isProposed = true;
		point.why = change.writeNote.empty() == false ? change.writeNote : change.checkNote;
		point.isSaidBefore = offBefore.count(change.positionId) > 0;
		facts.unchanged.push_back(point);
	}
	for (const intelligence::MarginComment& comment : validated.comments) {
		const std::string place = placeOf(comment.clauseNumber, comment.blockId);
		if (comment.writeState == "written") facts.commented.push_back(place + ": “" + comment.text + "”");
		else facts.notCommented.push_back(place + ": “" + comment.text + "”. " + comment.writeNote);
	}
	for (const auto& [blockId, change] : before) {
		if (changedHere.count(blockId) > 0) continue;
		const std::string place = placeOf(change.value("clauseNumber", std::string()), blockId);
		const std::string explanation = change.value("explanation", std::string());
		facts.dropped.push_back(place + ": version " + previous + " changed it" + (explanation.empty() == true ? std::string()
			: " (" + explanation + ")") + "; this version leaves it as the source has it");
	}

	// A position found off the Playbook that the draft leaves as it was, with
	// no call to the lawyer about it (one rewrite of a clause carries
	// everything that clause needs).
	std::set<std::string> usedSetAside;
	for (const intelligence::PositionAssessment& assessment : validated.assessments) {
		if (assessment.assessment != "deviates" && assessment.assessment != "missing") continue;
		const bool isTouched = std::any_of(validated.changes.begin(), validated.changes.end(),
			[&](const intelligence::ProposedChange& change) {
				return change.positionId == assessment.positionId || (change.writeState == "written"
					&& std::find(assessment.blockIds.begin(), assessment.blockIds.end(), change.blockId) != assessment.blockIds.end());
			});
		const bool isAsked = std::any_of(validated.unresolved.begin(), validated.unresolved.end(),
			[&](const intelligence::UnresolvedPoint& point) {
				if (point.kind != "needs_lawyer") return false;
				return std::any_of(point.blockIds.begin(), point.blockIds.end(), [&](const std::string& id) {
					return std::find(assessment.blockIds.begin(), assessment.blockIds.end(), id) != assessment.blockIds.end();
				});
			});
		if (isTouched == true || isAsked == true) continue;
		std::vector<std::string> places;
		for (const std::string& id : assessment.blockIds) places.push_back(placeWords(document, id));
		// A change proposed for it that was set aside says why itself.
		const auto setAside = std::find_if(validated.setAside.begin(), validated.setAside.end(),
			[&](const std::string& entry) { return entry.rfind("A change to " + assessment.topic + " ", 0) == 0; });
		UnchangedPoint point;
		point.label = labelOf(places, assessment.topic);
		point.names = places;
		point.names.push_back(assessment.topic);
		point.isProposed = setAside != validated.setAside.end();
		if (point.isProposed == true) usedSetAside.insert(*setAside);
		point.why = joinedSentences({assessment.explanation, point.isProposed == true ? *setAside : std::string()});
		point.isSaidBefore = offBefore.count(assessment.positionId) > 0;
		facts.unchanged.push_back(point);
	}

	// Any other change that could not be used: the colleague is told, so it
	// never takes a change it meant to make for one the draft holds.
	for (const std::string& entry : validated.setAside) {
		if (entry.rfind("A change", 0) == 0 && usedSetAside.count(entry) == 0) facts.setAside.push_back(entry);
	}

	if (revision.isWritten == true && integrity.isChecked == true) {
		for (const intelligence::IntegrityCheckResult& check : integrity.checks) {
			for (const intelligence::IntegrityFinding& finding : check.findings) {
				if (finding.severity == "problem" || finding.severity == "attention") facts.draftProblems.push_back(finding.text);
			}
		}
	} else if (revision.isWritten == true && integrity.problem.empty() == false) {
		facts.draftProblems.push_back(integrity.problem);
	}

	const std::vector<intelligence::UnresolvedPoint> raised = raisedBefore(review, versionNumber);
	const std::string earlierWords = " (said in an earlier version of this review)";
	std::set<std::string> exhibitsSaid;
	for (const intelligence::UnresolvedPoint& point : validated.unresolved) {
		std::vector<std::string> places;
		for (const std::string& id : point.blockIds) places.push_back(placeWords(document, id));
		const bool isPlaced = places.empty() == true || point.description.find(places.front()) != std::string::npos;
		if (point.kind == "needs_lawyer") {
			// A call that came with neither its question nor its place is said with
			// the position's own words on what is at stake.
			std::string words = point.description;
			if (words.find('?') == std::string::npos && isPlaced == false) {
				for (const intelligence::PositionAssessment& assessment : validated.assessments) {
					const bool isSame = std::any_of(point.blockIds.begin(), point.blockIds.end(), [&](const std::string& id) {
						return std::find(assessment.blockIds.begin(), assessment.blockIds.end(), id) != assessment.blockIds.end();
					});
					if (isSame == false || assessment.explanation.empty() == true) continue;
					words = joinedSentences({assessment.explanation, words});
					break;
				}
			}
			const bool isNamed = places.empty() == true || words.find(places.front()) != std::string::npos;
			facts.calls.push_back(isNamed == true ? words : listedWords(places) + ": " + words);
			continue;
		}
		if (point.kind == "missing_document") {
			bool isExhibit = false;
			for (const intelligence::ExhibitStatus& exhibit : document.exhibits) {
				if (exhibit.isPresent == true) continue;
				const bool isNamed = point.description.find(exhibit.label) != std::string::npos;
				const bool isFromThere = std::any_of(point.blockIds.begin(), point.blockIds.end(), [&](const std::string& id) {
					return std::find(exhibit.referencedIn.begin(), exhibit.referencedIn.end(), id) != exhibit.referencedIn.end();
				});
				if (isNamed == false && isFromThere == false) continue;
				isExhibit = true;
				if (exhibitsSaid.insert(exhibit.label).second == false) continue;
				std::vector<std::string> from;
				for (const std::string& id : exhibit.referencedIn) from.push_back(placeWords(document, id));
				const bool isOld = wasRaised(raised, "missing_document", exhibit.referencedIn);
				facts.lacking.push_back(exhibit.label + " is not in the document, though " + listedWords(from)
					+ (from.size() == 1 ? " refers" : " refer") + " to it, so it was not reviewed"
					+ (isOld == true ? earlierWords : std::string()));
				if (isOld == false) facts.freshlyLacking.push_back(exhibit.label);
			}
			if (isExhibit == true) continue;
		}
		if (point.kind == "unresolved_reference" && point.isConfirmedByStructure == true) {
			bool isSaid = false;
			for (const intelligence::CrossReference& reference : document.references) {
				if (reference.kind != "section" || reference.isResolved == true) continue;
				if (point.description.find(reference.label) == std::string::npos) continue;
				isSaid = true;
				const std::string from = capitalised(placeWords(document, reference.fromBlockId));
				const bool isMended = std::any_of(validated.changes.begin(), validated.changes.end(),
					[&](const intelligence::ProposedChange& change) {
						return change.writeState == "written" && change.blockId == reference.fromBlockId
							&& change.currentText.find(reference.label) != std::string::npos
							&& change.proposedText.find(reference.label) == std::string::npos;
					});
				const bool isOld = wasRaised(raised, "unresolved_reference", {reference.fromBlockId});
				facts.lacking.push_back(from + " refers to " + reference.label + ", which the agreement does not have"
					+ (isMended == true ? std::string("; the draft changes that reference") : std::string("; the draft leaves it"))
					+ (isOld == true ? earlierWords : std::string()));
				if (isOld == false) facts.freshlyLacking.push_back(reference.label);
				break;
			}
			if (isSaid == true) continue;
		}
		if (point.kind == "embedded_instruction") {
			const intelligence::LegalBlock* note = nullptr;
			for (const std::string& id : point.blockIds) {
				const intelligence::LegalBlock* block = intelligence::findBlock(document, id);
				if (block != nullptr && block->kind == intelligence::BlockKind::Comment) note = block;
			}
			if (note != nullptr) {
				const std::string on = placeWords(document, note->annotates);
				facts.notes.push_back("A margin comment " + (note->author.empty() == true ? std::string("by the other side")
					: "by " + note->author) + " on " + on + " is addressed to AI review tools; it was read as a comment, not "
					"followed as an instruction, and " + on + " was reviewed like any other clause"
					+ (wasRaised(raised, "embedded_instruction", point.blockIds) == true ? earlierWords : std::string()));
				continue;
			}
		}
		facts.notes.push_back(isPlaced == true ? point.description : point.description + " (" + listedWords(places) + ")");
	}
	for (const intelligence::PositionAssessment& assessment : validated.assessments) {
		if (assessment.assessment == "not_assessed") facts.unassessed.push_back(assessment.topic);
		if (assessment.assessment == "conforms") facts.meets.push_back(assessment.topic);
	}
	// Problems an earlier draft had that this one does not.
	if (revision.isWritten == true) {
		std::set<std::string> troubled;
		for (const ReviewVersion& earlier : review.versions) {
			if (earlier.number >= versionNumber || earlier.result.empty() == true) continue;
			const Json result = Json::parse(earlier.result, nullptr, false);
			if (result.is_object() == false) continue;
			for (const Json& change : result.value("changes", Json::array())) {
				if (change.is_object() == true && change.value("writeState", std::string()) == "written"
					&& change.value("writeNote", std::string()).empty() == false) {
					troubled.insert(change.value("blockId", std::string()));
				}
			}
		}
		for (const std::string& blockId : troubled) {
			const bool isMeantNow = std::any_of(validated.changes.begin(), validated.changes.end(),
				[&](const intelligence::ProposedChange& change) {
					return change.blockId == blockId && change.writeState == "written" && change.writeNote.empty() == true;
				});
			if (isMeantNow == true) {
				facts.resolved.push_back(placeWords(document, blockId) + ": an earlier draft showed it otherwise than meant; "
					"this draft, read back from the saved Word file with every tracked change accepted, reads exactly as meant");
			}
		}
	}
	if (revision.isWritten == false) {
		facts.noDraft = comparison != nullptr && comparison->isUnchanged == true ? "unchanged"
			: isDocx == false ? "not_word"
			// Nothing proposed at all, and a decision put to the lawyer: held for it.
			: validated.changes.empty() == true && validated.comments.empty() == true && validated.setAside.empty() == true
				&& facts.unchanged.empty() == true && facts.calls.empty() == false
				? "held_for_decision"
			: validated.changes.empty() == true && facts.unchanged.empty() == true ? "nothing_to_change" : "none_written";
	}
	return facts;

}

// The facts as the colleague reads them before it writes (layer 4).
std::string factsText(const HandInFacts& facts) {

	const auto items = [](const std::vector<std::string>& lines) {
		std::string text;
		for (const std::string& line : lines) text += "- " + line + "\n";
		return text;
	};
	const std::string lawyer = facts.lawyer.empty() == true ? std::string("the lawyer") : facts.lawyer;
	std::string text = "<what_happened>\nWhat Singularity recorded once your draft was written and read back from the "
		"saved Word file, as Word reads it. It is the authority on what the draft holds: say nothing it does not support, "
		"and never call a change made that it does not list as made.\n";
	// A document no Playbook covers is revised as the lawyer asked (D-115).
	text += "The work: version " + std::to_string(facts.version) + " of your " + (facts.playbookName.empty() == true
		? "revision of \"" + facts.sourceName + "\" as " + lawyer + " asked"
		: "review of \"" + facts.sourceName + "\" against " + facts.playbookName + ", asked for by " + lawyer) + ".\n";
	const auto quoted = [](const std::string& words) { return "\u201C" + words + "\u201D"; };
	if (facts.comparison.has_value() == true) {
		const DraftComparison& compared = *facts.comparison;
		const std::string last = "version " + std::to_string(compared.previousVersion);
		text += "What " + lawyer + " asked of you this time: " + quoted(facts.request) + "\n";
		if (compared.named.empty() == false) {
			text += "The clauses it names, as the drafts read in Word with every tracked change accepted:\n";
			for (const ClauseReading& reading : compared.named) {
				text += "- " + reading.place + ": " + (asRead(reading.before) == asRead(reading.now)
					? "reads the same as in " + last + "'s draft: " + quoted(reading.now)
					: "in " + last + "'s draft it read " + quoted(reading.before) + "; now it reads " + quoted(reading.now)) + "\n";
			}
		}
	}
	if (facts.draftName.empty() == false) {
		text += "The draft: \"" + facts.draftName + "\", attached to your message, with your changes as tracked changes "
			"nobody has accepted yet.\n";
	} else if (facts.noDraft == "unchanged") {
		text += "There is no new draft: even after a second attempt, your changes left the agreement reading exactly as "
			"version " + std::to_string(facts.comparison->previousVersion) + "'s draft does, so no new version was made, and "
			"version " + std::to_string(facts.comparison->previousVersion) + " still waits for " + lawyer + ". Your own words "
			"on it: " + quoted(facts.comparison->why) + "\n";
	} else {
		text += "There is no draft: " + std::string(facts.noDraft == "not_word"
			? "the source is not a Word document, so no tracked changes could be written into it."
			: facts.noDraft == "held_for_decision"
				? "you proposed no change yet: it waits for the decision you put to " + lawyer + " below."
			: facts.noDraft == "nothing_to_change" ? (facts.playbookName.empty() == true
				? std::string("the agreement already does what the request asks, so there was nothing to change.")
				: "your review found nothing to change against the " + facts.playbookName + ".")
			: "none of the changes you proposed could be written into the document.") + "\n";
	}
	if (facts.comparison.has_value() == true && facts.comparison->otherChanged.empty() == false) {
		text += "Other clauses that read differently from version " + std::to_string(facts.comparison->previousVersion)
			+ "'s draft:\n";
		for (const ClauseReading& reading : facts.comparison->otherChanged) {
			text += "- " + reading.place + ": " + (reading.before.empty() == true ? std::string("new: ") + quoted(reading.now)
				: "it read " + quoted(reading.before) + "; now it reads " + quoted(reading.now)) + "\n";
		}
	}
	if (facts.written.empty() == false) {
		text += std::string(facts.comparison.has_value() == true ? "Changes new or revised in this draft:\n"
			: "What the draft changes:\n") + items(facts.written);
	}
	if (facts.carriedOver > 0) {
		text += std::to_string(facts.carriedOver) + " other " + (facts.carriedOver == 1 ? "change reads" : "changes read")
			+ " exactly as in the last version's draft; the thread knows " + (facts.carriedOver == 1 ? "it" : "them")
			+ ", so say nothing of " + (facts.carriedOver == 1 ? "it" : "them") + " unless asked.\n";
	}
	if (facts.commented.empty() == false) {
		text += "Your comments in the draft's margin, as Word holds them, for whoever the draft is sent to:\n"
			+ items(facts.commented);
	}
	if (facts.notCommented.empty() == false) {
		text += "Comments you meant for the margin that are not in the draft:\n" + items(facts.notCommented);
	}
	if (facts.dropped.empty() == false) text += "What the last version changed that this draft no longer does:\n" + items(facts.dropped);
	if (facts.unchanged.empty() == false) {
		text += "What the draft does not change, though your review found it off the firm's position:\n";
		for (const UnchangedPoint& point : facts.unchanged) {
			text += "- " + point.label + ": " + (point.isProposed == true
				? std::string("the change you proposed for it is not in the draft") : std::string("you made no change to it"))
				+ (point.why.empty() == true ? std::string(".") : ". " + point.why)
				+ (point.isSaidBefore == true ? std::string(" It was so in the last version too, and the thread knows it.")
					: std::string()) + "\n";
		}
	}
	if (facts.draftProblems.empty() == false) text += "What the draft itself needs before it goes out:\n" + items(facts.draftProblems);
	if (facts.resolved.empty() == false) {
		text += "Settled: problems an earlier draft had that this one does not. Where the thread raised them, say they are "
			"settled; never that they stand:\n" + items(facts.resolved);
	}
	if (facts.calls.empty() == false) text += "Decisions only " + lawyer + " can make:\n" + items(facts.calls);
	if (facts.unassessed.empty() == false) text += "Positions you did not assess, so not reviewed:\n" + items(facts.unassessed);
	if (facts.lacking.empty() == false) text += "What the document lacks:\n" + items(facts.lacking);
	if (facts.setAside.empty() == false) {
		text += "Changes you proposed that could not be used, so they are not in the draft:\n" + items(facts.setAside);
	}
	if (facts.notes.empty() == false) text += "Other points:\n" + items(facts.notes);
	if (facts.meets.empty() == false) text += "Positions the agreement already meets: " + listedWords(facts.meets) + ".\n";
	if (facts.view.empty() == false) {
		text += "Your own view of the agreement as you reviewed it, before the draft was written (where it differs from the "
			"above, the above is right): " + facts.view + "\n";
	}
	return text + "</what_happened>\n";

}

// The hand-in said plainly from the facts: what goes to the thread when the
// colleague's own words could not be made to agree with them.
std::string plainHandIn(const HandInFacts& facts) {

	std::vector<std::string> paragraphs;
	if (facts.draftName.empty() == false) {
		const std::size_t tracked = facts.written.size() + facts.carriedOver;
		paragraphs.push_back("Version " + std::to_string(facts.version) + " is attached, with "
			+ countWords(tracked) + " tracked " + (tracked == 1 ? "change" : "changes") + ".");
	} else if (facts.noDraft == "unchanged") {
		paragraphs.push_back("I couldn't make a new draft: my changes kept leaving the agreement exactly as version "
			+ std::to_string(facts.comparison->previousVersion) + " reads, so version "
			+ std::to_string(facts.comparison->previousVersion) + " is still the one to look at."
			+ (facts.comparison->why.empty() == true ? std::string() : " " + facts.comparison->why));
	} else if (facts.noDraft == "not_word") {
		paragraphs.push_back("The source isn't a Word document, so I couldn't track changes in it.");
	} else if (facts.noDraft == "held_for_decision") {
		paragraphs.push_back("I haven't changed it yet: the change waits for your decision below, so there's no draft.");
	} else if (facts.noDraft == "nothing_to_change") {
		paragraphs.push_back(facts.playbookName.empty() == true
			? std::string("The agreement already does what you asked, so there's no new draft.")
			: "I found nothing to change against the " + facts.playbookName + ", so there's no new draft.");
	} else {
		paragraphs.push_back("None of the changes I proposed could be written into the document, so there's no draft.");
	}
	if (facts.unchanged.empty() == false) {
		std::vector<std::string> said;
		for (const UnchangedPoint& point : facts.unchanged) {
			if (point.isProposed == false) {
				said.push_back(capitalised(point.label) + " is still off our position, and "
					+ (facts.draftName.empty() == false ? std::string("the draft doesn't change it.") : std::string("I made no change to it.")));
			} else if (facts.draftName.empty() == true) {
				said.push_back("The change I proposed to " + point.label + " couldn't be used.");
			} else {
				said.push_back("The change I proposed to " + point.label + " isn't in the draft. " + point.why);
			}
		}
		paragraphs.push_back(joinedSentences(said));
	}
	if (facts.commented.empty() == false) {
		paragraphs.push_back(std::string(facts.commented.size() == 1 ? "A comment of mine is" : "My comments are")
			+ " in the margin, for whoever the draft goes to: " + joinedSentences(facts.commented));
	}
	if (facts.notCommented.empty() == false) {
		paragraphs.push_back(std::string(facts.notCommented.size() == 1 ? "A comment I meant for the margin isn't"
			: "Comments I meant for the margin aren't") + " in the draft: " + joinedSentences(facts.notCommented));
	}
	if (facts.draftProblems.empty() == false) {
		paragraphs.push_back("Before it goes out, " + countWords(facts.draftProblems.size())
			+ (facts.draftProblems.size() == 1 ? " thing" : " things") + " in the draft need" + (facts.draftProblems.size() == 1 ? "s" : "")
			+ " a look. " + joinedSentences(facts.draftProblems));
	}
	if (facts.calls.empty() == false) {
		paragraphs.push_back((facts.calls.size() == 1 ? std::string("One call for you: ") : capitalised(countWords(facts.calls.size()))
			+ " calls for you. ") + joinedSentences(facts.calls));
	}
	if (facts.unassessed.empty() == false) {
		paragraphs.push_back("I did not assess " + listedWords(facts.unassessed) + " against the Playbook, so treat "
			+ (facts.unassessed.size() == 1 ? std::string("it") : std::string("them")) + " as not reviewed.");
	}
	// A change for no position of the Playbook is the model's own slip, and
	// names nothing the lawyer knows: it is not said to the lawyer.
	std::vector<std::string> unused;
	for (std::string entry : facts.setAside) {
		if (entry.find("which is not a position of the Playbook") != std::string::npos) continue;
		while (entry.empty() == false && (entry.back() == '.' || entry.back() == ' ')) entry.pop_back();
		if (entry.empty() == false) entry[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(entry[0])));
		if (entry.empty() == false) unused.push_back(entry);
	}
	if (unused.empty() == false) {
		paragraphs.push_back(std::string(unused.size() == 1 ? "One more change I proposed couldn't be used: "
			: "These changes I proposed couldn't be used: ") + listedWords(unused) + ".");
	}
	std::vector<std::string> fresh;
	for (const std::string& line : facts.lacking) {
		if (line.find("(said in an earlier version") == std::string::npos) fresh.push_back(line);
	}
	if (fresh.empty() == false) paragraphs.push_back(joinedSentences(fresh));
	std::string text;
	for (const std::string& paragraph : paragraphs) text += (text.empty() == true ? "" : "\n\n") + paragraph;
	return text;

}

// Whether `text` holds one of `words` as a word or a phrase of its own.
bool holdsWord(const std::string& text, const std::vector<std::string>& words) {
	for (const std::string& word : words) {
		if (word.empty() == true) continue;
		for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
			const bool startsWord = at == 0 || std::isalnum(static_cast<unsigned char>(text[at - 1])) == 0;
			const std::size_t end = at + word.size();
			const bool endsWord = end >= text.size() || std::isalnum(static_cast<unsigned char>(text[end])) == 0
				|| std::isalnum(static_cast<unsigned char>(word.back())) == 0;
			if (startsWord == true && endsWord == true) return true;
		}
	}
	return false;
}

// Where the colleague's message says what the facts do not: a change it calls
// made that the draft does not hold, a point the draft leaves that it passes
// over in silence, a decision it does not ask for, a draft it calls attached
// when there is none, a report's stock reassurance. Empty when the message
// agrees with what happened. A reply to a question about the work (`isReply`)
// need not say again what its hand-in said; it must not contradict it.
std::vector<std::string> contradictions(const std::string& message, const HandInFacts& facts, bool isReply = false) {

	std::vector<std::string> issues;
	const std::string words = lowered(message);
	if (words.find_first_not_of(" \n\r\t") == std::string::npos) return {"It was empty."};
	const std::size_t first = message.find_first_not_of(" \n\r\t");
	if (message[first] == '{' || message[first] == '[' || message.find("```") != std::string::npos
		|| message.find("<what_happened>") != std::string::npos) {
		issues.push_back("It is not a message to a colleague: write the message itself, in plain words.");
	}
	if (message.find("[[") != std::string::npos) issues.push_back("It holds a placeholder in double square brackets.");
	for (const char* stock : {"nothing of the source is lost", "nothing was lost", "i checked the draft itself",
			"as an ai language model", "require further assistance", "successfully completed", "comprehensive analysis"}) {
		if (words.find(stock) != std::string::npos) issues.push_back(std::string("It says \"") + stock + "\", which a colleague would not write.");
	}

	// Sentence by sentence, and within a sentence part by part ("I updated 4.1
	// and left 5.2 as it was"): what each says of a place the draft does not
	// change. A part that names the place but says nothing of it takes what its
	// sentence says ("I updated Sections 4.1 and 5.2").
	std::vector<std::string> sentences;
	std::string sentence;
	for (std::size_t at = 0; at < words.size(); ++at) {
		sentence += words[at];
		const bool ends = words[at] == '\n' || ((words[at] == '.' || words[at] == '?' || words[at] == '!' || words[at] == ';')
			&& (at + 1 >= words.size() || words[at + 1] == ' ' || words[at + 1] == '\n'));
		if (ends == true) {
			sentences.push_back(sentence);
			sentence.clear();
		}
	}
	if (sentence.empty() == false) sentences.push_back(sentence);
	const auto partsOf = [](const std::string& line) {
		static const std::vector<std::string> joints{", and ", ", but ", " and ", " but ", ", while ", " while ", ", though ",
			" though ", ", although ", " although ", ", whereas ", " whereas ", " - ", " — ", " – ", ": "};
		std::vector<std::string> parts{line};
		for (const std::string& joint : joints) {
			std::vector<std::string> split;
			for (const std::string& part : parts) {
				std::size_t from = 0;
				for (std::size_t at = part.find(joint); at != std::string::npos; at = part.find(joint, from)) {
					split.push_back(part.substr(from, at - from));
					from = at + joint.size();
				}
				split.push_back(part.substr(from));
			}
			parts = split;
		}
		return parts;
	};
	static const std::vector<std::string> done{"updated", "changed", "fixed", "restored", "revised", "reverted", "put back",
		"brought back", "now reads", "now read", "now says", "now say", "replaced", "struck", "deleted", "removed", "added",
		"amended", "corrected", "switched", "narrowed", "extended", "shortened", "tightened", "redrafted", "rewrote",
		"rewritten", "took out", "taken out", "marked up", "is done"};
	static const std::vector<std::string> notDone{"not", "no change", "unable", "failed", "still", "left", "leave",
		"leaves", "unchanged", "yet", "couldn", "didn", "wasn", "isn", "doesn", "don", "won", "hasn", "haven", "aren",
		"weren", "can't", "cannot", "nor", "without", "instead", "as it was", "as is", "as the source", "untouched",
		"would"};
	const auto says = [](const std::string& part, const std::vector<std::string>& which) { return holdsWord(part, which); };
	// Whether a part of a sentence names a place of its own ("left 7.1 as is").
	const auto namesAPlace = [&](const std::string& part) {
		for (std::size_t at = 1; at + 1 < part.size(); ++at) {
			if (part[at] == '.' && std::isdigit(static_cast<unsigned char>(part[at - 1])) != 0
				&& std::isdigit(static_cast<unsigned char>(part[at + 1])) != 0) {
				return true;
			}
		}
		return says(part, {"section", "sections", "clause", "clauses", "exhibit", "schedule", "annex"});
	};
	// Whether a part speaks of what came before it ("but it couldn't be
	// written"), not of the rest ("and left the rest as it was").
	const auto pointsBack = [&](const std::string& part) {
		return says(part, {"it", "it's", "its", "that", "this", "which", "the change", "my change", "the edit"}) == true
			&& says(part, {"rest", "else", "other", "others", "everything", "anything"}) == false;
	};
	// The words of a topic a lawyer would use for it, by their stems:
	// "Limitation of liability" is mentioned as "the liability cap", and
	// "Non-solicitation" as "the non-solicit".
	static const std::set<std::string> plain{"about", "after", "before", "their", "there", "these", "those", "which",
		"where", "other", "under", "party", "parties", "agreement", "information", "clause", "section", "provision",
		"provisions", "terms", "between"};
	const auto holdsStem = [](const std::string& text, const std::vector<std::string>& stems) {
		for (const std::string& stem : stems) {
			for (std::size_t at = text.find(stem); at != std::string::npos; at = text.find(stem, at + 1)) {
				if (at == 0 || std::isalpha(static_cast<unsigned char>(text[at - 1])) == 0) return true;
			}
		}
		return false;
	};
	for (const UnchangedPoint& point : facts.unchanged) {
		std::vector<std::string> names;
		std::vector<std::string> topicWords;
		for (const std::string& name : point.names) {
			if (name.empty() == true || name.rfind("a ", 0) == 0) continue;
			names.push_back(lowered(name));
			// "Section 5.2" is also said as "5.2", "§ 5.2" or "clause 5.2".
			if (name.rfind("Section ", 0) == 0) names.push_back(lowered(name.substr(8)));
			if (name.rfind("Sections ", 0) == 0) names.push_back(lowered(name.substr(9)));
			if (std::isdigit(static_cast<unsigned char>(name.back())) != 0) continue;
			std::string word;
			for (const char letter : lowered(name) + " ") {
				if (std::isalpha(static_cast<unsigned char>(letter)) != 0) {
					word += letter;
					continue;
				}
				if (word.size() >= 5 && plain.count(word) == 0) topicWords.push_back(word.substr(0, 7));
				word.clear();
			}
		}
		bool isMentioned = false;
		bool isCalledDone = false;
		for (const std::string& line : sentences) {
			if (says(line, names) == false) {
				if (holdsStem(line, topicWords) == true) isMentioned = true;
				continue;
			}
			isMentioned = true;
			const bool sentenceSaysDone = says(line, done) == true && says(line, notDone) == false;
			const std::vector<std::string> parts = partsOf(line);
			for (std::size_t index = 0; index < parts.size(); ++index) {
				const std::string& part = parts[index];
				if (says(part, names) == false) continue;
				const bool partSaysDone = says(part, done);
				const bool partSaysNot = says(part, notDone);
				bool isDone = (partSaysDone == true && partSaysNot == false)
					|| (partSaysDone == false && partSaysNot == false && sentenceSaysDone == true);
				// "I changed 5.2, but it couldn't be written": a part that says no of
				// what came before it, and of no other place, speaks of this one.
				for (std::size_t other = 0; other < parts.size() && isDone == true; ++other) {
					if (other != index && says(parts[other], notDone) == true && namesAPlace(parts[other]) == false
						&& pointsBack(parts[other]) == true) {
						isDone = false;
					}
				}
				if (isDone == true) isCalledDone = true;
			}
		}
		// What the lawyer asks about now is answered, whatever was said before.
		const std::string asked = lowered(facts.request);
		const bool isAskedAbout = says(asked, names) == true || holdsStem(asked, topicWords) == true;
		if (isCalledDone == true) {
			issues.push_back("It says " + point.label + " is changed, but the draft does not change it: " + point.why);
		} else if (isMentioned == false && isReply == false && (point.isSaidBefore == false || isAskedAbout == true)) {
			issues.push_back("It does not say that the draft leaves " + point.label + " unchanged: " + point.why);
		}
	}
	if (facts.draftName.empty() == true && holdsWord(words, {"attached", "attach", "here's the draft", "here is the draft"}) == true) {
		issues.push_back("It speaks of a draft attached, but there is none.");
	}
	if (isReply == true) return issues;
	// A revision answers what was asked of it (D-102): each clause the lawyer
	// named, by what became of it.
	if (facts.comparison.has_value() == true) {
		for (const ClauseReading& reading : facts.comparison->named) {
			std::vector<std::string> names{lowered(reading.place)};
			if (reading.place.rfind("Section ", 0) == 0) names.push_back(lowered(reading.place.substr(8)));
			if (holdsWord(words, names) == false) {
				issues.push_back("It does not say what became of " + reading.place + ", which "
					+ (facts.lawyer.empty() == true ? std::string("the lawyer") : facts.lawyer) + " asked about.");
			}
		}
	}
	// What was not assessed is said with the first review; a revision re-reads
	// what it was asked about.
	if (facts.unassessed.empty() == false && facts.version == 1) {
		bool isSaid = false;
		for (const std::string& line : sentences) {
			if (holdsWord(line, notDone) == true && holdsStem(line, {"assess", "review", "check", "look", "consider"}) == true) isSaid = true;
		}
		if (isSaid == false) {
			issues.push_back("It does not say that " + listedWords(facts.unassessed) + (facts.unassessed.size() == 1
				? std::string(" was") : std::string(" were")) + " not assessed against the Playbook, so not reviewed.");
		}
	}
	for (const std::string& label : facts.freshlyLacking) {
		if (holdsWord(words, {lowered(label)}) == false) {
			issues.push_back("It does not say what the document lacks: " + label + ".");
		}
	}
	const bool asksSomething = message.find('?') != std::string::npos
		|| holdsWord(words, {"let me know", "tell me", "please confirm", "can you confirm", "your call", "your decision"}) == true;
	if (facts.calls.empty() == false && asksSomething == false) {
		issues.push_back("It asks nothing, though " + countWords(facts.calls.size()) + (facts.calls.size() == 1
			? std::string(" decision is ") : std::string(" decisions are ")) + (facts.lawyer.empty() == true
				? std::string("the lawyer") : facts.lawyer) + "'s to make.");
	}
	return issues;

}

// A message as it goes into the thread. The interface shows the file a
// message hands over, so a written stand-in for it ("[attached: ...]") is
// taken out; and a colleague does not open a message on its reader's name.
std::string polished(std::string message, const std::string& reader) {
	static const std::regex attached("\\s*\\[(?:attached|attachment|file)s?:[^\\]\\n]*\\]", std::regex::icase);
	message = std::regex_replace(message, attached, "");
	const std::size_t opening = message.find_first_not_of(" \n\r\t");
	message = opening == std::string::npos ? std::string() : message.substr(opening);
	if (reader.empty() == false && message.rfind("@" + reader, 0) == 0) {
		std::size_t at = reader.size() + 1;
		while (at < message.size() && (message[at] == ',' || message[at] == ':' || message[at] == ' ' || message[at] == '-')) ++at;
		message = message.substr(at);
		if (message.empty() == false) {
			message[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(message[0])));
		}
	}
	while (message.empty() == false && std::isspace(static_cast<unsigned char>(message.back())) != 0) message.pop_back();
	return message;
}

// What the colleague's message is written with: the model the review ran on,
// asked as the review was and protected as it was.
struct HandInCall {
	routing::ModelTarget model;
	routing::ModelCapability capability;
	std::optional<std::uint64_t> allowance;
	bool isLocal = false;
	std::string matterId;
	std::string chatId;
	std::string agentId;
	std::string agentName;
	std::string agentBrief;
	std::string answering;
	// How the thread marks the message it answers.
	std::string answeringLabel = " (the request you have just done)";
	std::string operationId;
	std::string taskId;
	std::shared_ptr<platform::HttpCancelToken> token;
	const std::atomic<bool>* cancelled = nullptr;
};

struct HandIn {
	std::string text;
	// "colleague": its own words, which agree with what happened; "plain":
	// Singularity's, said from what happened, where its words could not be
	// made to agree or could not be had.
	std::string writtenBy = "plain";
	Json record = Json::object();
	std::optional<long long> costMicros;
	std::string costSource;
};

// The Matter's thread as the colleague reads it before it writes (layer 3):
// its messages so far, oldest first, each by its author, with the files it
// handed over. Where the model says how much it reads at once, the oldest
// messages give way first, so what must be said now always fits.
std::string threadText(const matters::Matter& matter, const HandInCall& call, std::optional<std::size_t> room) {

	std::map<std::string, std::string> documentNames;
	for (const matters::MatterDocument& document : matter.documents) documentNames[document.id] = document.name;
	const auto chat = std::find_if(matter.chats.begin(), matter.chats.end(),
		[&](const matters::MatterChat& entry) { return entry.id == call.chatId; });
	const bool isOwnThread = chat != matter.chats.end() && chat->agent_id == call.agentId;
	std::vector<const matters::MatterTask*> messages;
	for (const matters::MatterTask& task : matter.tasks) {
		if (task.chat_id != std::optional<std::string>(call.chatId)) continue;
		messages.push_back(&task);
	}
	std::stable_sort(messages.begin(), messages.end(),
		[](const matters::MatterTask* one, const matters::MatterTask* other) { return one->created_at < other->created_at; });
	const auto handed = [&](const matters::MatterTask& task) {
		std::vector<std::string> names;
		for (const std::string& id : task.attachment_document_ids) {
			names.push_back(documentNames.count(id) > 0 ? "\u201C" + documentNames[id] + "\u201D" : std::string("a document"));
		}
		return names.empty() == true ? std::string() : std::string("\n(This message handed over ") + listedWords(names) + ".)";
	};
	const auto placed = [&](const matters::MatterTask& task) {
		return task.place.empty() == true ? std::string() : "\n(It is about " + chat::place_words(matter, task.place) + ".)";
	};
	std::vector<std::string> lines;
	for (const matters::MatterTask* task : messages) {
		const std::string author = task->author_name.empty() == true ? std::string("A member") : task->author_name;
		const std::string now = task->id == call.answering ? call.answeringLabel : std::string();
		if (task->thread_event == "closed") {
			lines.push_back(author + " closed the thread, concluding: " + task->prompt);
		} else if (task->thread_event == "reopened") {
			lines.push_back(author + " reopened the thread.");
		} else if (task->operation_type == "member_message") {
			if (task->deleted_at.empty() == false) continue;
			lines.push_back(author + now + ": " + task->prompt + handed(*task) + placed(*task));
		} else if (task->operation_type == "agent_message") {
			if (task->deleted_at.empty() == false) continue;
			const bool isMine = task->agent_id == call.agentId;
			lines.push_back((isMine == true ? "You" : author) + ": " + task->response_text.value_or(std::string())
				+ (isMine == true ? std::string() : handed(*task)) + placed(*task));
		} else {
			if (task->prompt.empty() == false) lines.push_back(author + now + ": " + task->prompt);
			if (task->response_text.has_value() == true && task->response_text->empty() == false) {
				lines.push_back((isOwnThread == true ? std::string("You") : std::string("The colleague")) + ": " + *task->response_text);
			}
		}
	}
	// The newest first into the room there is, then said oldest first.
	std::vector<std::string> kept;
	std::size_t used = 0;
	for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
		const std::size_t cost = research::estimate_tokens(*line);
		if (room.has_value() == true && used + cost > *room) break;
		used += cost;
		kept.push_back(*line);
	}
	std::reverse(kept.begin(), kept.end());
	std::string text = "<matter_thread>\nThe thread of the " + matter.name + " Matter in which this work was asked for, "
		"oldest first. \"You\" is you.\n";
	if (kept.size() < lines.size()) text += "(Its " + std::to_string(lines.size() - kept.size()) + " earliest messages are not shown.)\n";
	for (const std::string& line : kept) text += line + "\n\n";
	return text + "</matter_thread>\n";

}

std::string handInTask(const HandInFacts& facts) {

	const std::string lawyer = facts.lawyer.empty() == true ? std::string("the lawyer") : facts.lawyer;
	std::string text = "<task>\nYou have just finished the work " + lawyer + " asked of you in this thread. Write the message "
		"you now post there" + (facts.draftName.empty() == false ? std::string(", with the draft attached under it") : std::string())
		+ ".\n";
	text += "Write it from <what_happened>: what the draft actually holds, what it leaves as it was and why, and what "
		"needs " + lawyer + ". The draft carries every change as a tracked change, and the review beside your message "
		"shows each one clause by clause with the source's words, so don't walk through them: say what matters now.\n";
	if (facts.version > 1) {
		text += "This is version " + std::to_string(facts.version) + ": say what changed since the last one, in answer to "
			"what was asked, and don't repeat what the thread already knows.\n";
	}
	text += "Write to " + lawyer + "; there is no need to open with their name. Promise no work nobody has asked you "
		"for: offer it as a question instead.\n";
	text += "Write only the message, as it will appear in the thread.\n</task>\n";
	return text;

}

// What the colleague writes after its work, asked of the model the review ran
// on, protected as the review was, and checked before anything of it is used:
// an answer that does not agree with what happened is sent back once with why.
struct AfterWork {
	// The answer that agreed, or the last one that did not ("" when none came).
	std::string text;
	bool isAgreed = false;
	// Why nothing usable came, where it did not.
	std::string why;
	Json attempts = Json::array();
	std::optional<long long> costMicros;
	std::string costSource;
};

AfterWork askAfterWork(const HandInCall& call, const std::string& prompt,
	const std::optional<providers::GenerationOutputContract>& contract,
	const std::function<std::vector<std::string>(const std::string&)>& check, const std::string& againWords) {

	AfterWork written;
	const auto isStopped = [&]() {
		return platform::lifecycle::is_closing() == true || (call.token != nullptr && call.token->is_cancelled() == true)
			|| (call.cancelled != nullptr && call.cancelled->load() == true);
	};
	if (isStopped() == true) {
		written.why = "The work was stopped before the model was asked.";
		return written;
	}
	std::string asked = prompt;
	long long spentMicros = 0;
	bool isEstimated = false;
	for (int attempt = 1; attempt <= 2; ++attempt) {
		std::optional<std::uint64_t> callAllowance = call.allowance;
		const ReviewBudget budget = call.isLocal == true ? ReviewBudget() : loadReviewBudget();
		if (call.isLocal == false && budget.limitUsd.has_value() == true) {
			// What the remainder of the limit this computer chose pays for, where
			// the whole allowance does not fit (D-118, D-132).
			const std::uint64_t affordable = callAllowance.has_value() == false ? 0 : affordableOutputTokens(asked.size(),
				*callAllowance, call.capability.input_price_per_token, call.capability.output_price_per_token,
				budget, kLeastStepOutputTokens);
			if (affordable == 0) {
				written.why = "What is left of this computer's budget for reviews could not cover it.";
				return written;
			}
			callAllowance = affordable;
		}
		routing::InferenceRequest request;
		request.call_purpose = attempt == 1 ? routing::observability::CallPurpose::SubstantiveGeneration
			: routing::observability::CallPurpose::Correction;
		request.model_target = call.model;
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			request.workspace_id = platform::load_app_settings().local_node_setup.active_workspace_id;
		}
		request.maximum_output_tokens = callAllowance;
		if (call.isLocal == false) request.project_entity_hints = context_actions::memory_protection_hints(call.matterId);
		request.prompt = asked;
		request.research_mode = research::ResearchMode::Off;
		if (contract.has_value() == true) request.research_final_output_contract = *contract;
		request.operation_symbols = std::make_shared<protected_api::papinian::OperationProtectedSymbolTable>();
		request.operation_symbols->operation_id = call.operationId;
		const auto started = std::chrono::steady_clock::now();
		routing::InferenceResult result;
		try {
			routing::require_execution_ready(request);
			if (call.token != nullptr) {
				const platform::HttpCancelScope scope(call.token);
				result = routing::execute(request);
			} else {
				result = routing::execute(request);
			}
		} catch (const providers::TaskError& error) {
			const auto* unusable = dynamic_cast<const providers::UnusableGeneration*>(&error);
			recordCall(call.matterId, call.taskId, call.model, nullptr, error.code(), millisecondsSince(started),
				unusable != nullptr ? &unusable->usage : nullptr,
				unusable != nullptr ? unusable->finish_reason : std::optional<std::string>(), "document-review", 1,
				decisionOf(call.model, request, error.code(), error.what()));
			written.attempts.push_back(Json{{"attempt", attempt}, {"failed", reasonFor(error.code(), call.model.model_id, error.what())}});
			written.why = "The model could not be asked: " + reasonFor(error.code(), call.model.model_id, error.what());
			return written;
		} catch (const platform::CredentialUnavailable&) {
			throw;
		} catch (const std::exception& error) {
			recordCall(call.matterId, call.taskId, call.model, nullptr, providers::TaskErrorCode::Unknown, millisecondsSince(started),
				nullptr, std::nullopt, "document-review", 1, decisionOf(call.model, request, providers::TaskErrorCode::Unknown,
					std::string()));
			written.attempts.push_back(Json{{"attempt", attempt}, {"failed", error.what()}});
			written.why = std::string("The model could not be asked: ") + error.what();
			return written;
		}
		const long long latency = millisecondsSince(started);
		recordCall(call.matterId, call.taskId, call.model, &result, std::nullopt, latency);
		std::optional<long long> cost = result.usage.cost_micros;
		if (cost.has_value() == false && call.isLocal == false) {
			cost = reviewCostBoundMicros(asked.size(),
				static_cast<std::uint64_t>(std::max<long long>(result.usage.output_tokens.value_or(0), 0)),
				call.capability.input_price_per_token, call.capability.output_price_per_token, loadReviewBudget());
			isEstimated = true;
		}
		if (call.isLocal == false && cost.has_value() == true) {
			recordReviewSpend(*cost, result.usage.cost_micros.has_value() == false);
			spentMicros += *cost;
			written.costMicros = spentMicros;
			written.costSource = isEstimated == true ? "estimated" : "provider_reported";
		}
		std::string text = result.text;
		while (text.empty() == false && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.pop_back();
		const std::size_t opening = text.find_first_not_of(" \n\r\t");
		text = opening == std::string::npos ? std::string() : text.substr(opening);
		const std::vector<std::string> issues = check(text);
		written.attempts.push_back(Json{{"attempt", attempt}, {"inputTokens", result.usage.input_tokens.value_or(0)},
			{"outputTokens", result.usage.output_tokens.value_or(0)}, {"latencyMilliseconds", latency},
			{"costMicros", cost.has_value() == true ? Json(*cost) : Json()},
			{"protected", result.protected_api_applied}, {"issues", issues}});
		// What an answer that was not used said, kept with the version: why it was
		// sent back is checked against what it said.
		if (issues.empty() == false) written.attempts.back()["text"] = text;
		written.text = text;
		if (issues.empty() == true) {
			written.isAgreed = true;
			return written;
		}
		if (isStopped() == true) break;
		std::string why;
		for (const std::string& issue : issues) why += "- " + issue + "\n";
		asked = prompt + "\n<your_answer>\n" + text + "\n</your_answer>\n\n<correction>\nThat answer was not used: it does "
			"not agree with what happened.\n" + why + againWords + "\n</correction>\n";
	}
	written.why = "The model's answer did not agree with what happened.";
	return written;

}

// The colleague's message for the thread, written once the work is done, from
// what happened (D-101): the model the review ran on writes it from the
// colleague's brief, the thread and the facts, and Singularity checks it
// against the facts before it is posted. A message that says what the facts
// do not is sent back once with why; one that still does not agree, or a
// message that cannot be had, gives way to the facts said plainly.
HandIn composeHandIn(const HandInCall& call, const HandInFacts& facts) {

	std::optional<matters::Matter> matter;
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = matters::load_matter(node::require_active_workspace_path(), call.matterId);
	} catch (const std::exception&) {
	}
	const std::string what = factsText(facts);
	const std::string task = handInTask(facts);
	std::optional<std::size_t> room;
	if (call.capability.context_window_tokens.has_value() == true) {
		const std::size_t fixed = research::estimate_tokens(call.agentBrief) + research::estimate_tokens(what)
			+ research::estimate_tokens(task) + static_cast<std::size_t>(call.allowance.value_or(0));
		const std::size_t window = static_cast<std::size_t>(*call.capability.context_window_tokens);
		room = window > fixed ? window - fixed : 0;
	}
	const std::string thread = matter.has_value() == true ? threadText(*matter, call, room) : std::string();
	const std::string prompt = call.agentBrief + "\n\n" + thread + "\n" + what + "\n" + task;
	const AfterWork written = askAfterWork(call, prompt, std::nullopt,
		[&](const std::string& text) { return contradictions(text, facts); },
		"Write it again from <what_happened>, as the message itself.");

	HandIn handIn;
	handIn.writtenBy = written.isAgreed == true ? "colleague" : "plain";
	handIn.text = written.isAgreed == true ? polished(written.text, facts.lawyer) : plainHandIn(facts);
	handIn.costMicros = written.costMicros;
	handIn.costSource = written.costSource;
	handIn.record["attempts"] = written.attempts;
	handIn.record["writtenBy"] = handIn.writtenBy;
	if (written.isAgreed == false) handIn.record["why"] = written.why + " The facts were said plainly.";
	return handIn;

}

// ---- A colleague asked about a review it handed in (D-101) -------------------------

// The review as its record stands (layer 4), for a colleague asked about it:
// its latest version, where that stands, the draft, what it changes and does
// not, and what is open. Read from the saved record, never from memory; and
// the same facts, as far as the record holds them, for checking the reply.
struct ReviewState {
	std::string text;
	HandInFacts facts;
	// Said plainly, where the colleague's words could not be had.
	std::string plain;
};

ReviewState reviewStateOf(const DocumentReview& review) {

	ReviewState state;
	const ReviewVersion& latest = review.versions.back();
	const ReviewDecision* decision = nullptr;
	for (const ReviewDecision& entry : review.decisions) {
		if (entry.version == latest.number) decision = &entry;
	}
	const std::string who = decision != nullptr && decision->actorName.empty() == false ? decision->actorName : std::string("a lawyer");
	std::string where;
	if (latest.state == "ready_for_review") {
		where = "is handed in and waits for a lawyer to accept it or return it with comments";
	} else if (latest.state == "returned") {
		where = "was returned by " + who + (decision != nullptr && decision->comment.empty() == false
			? " with these comments: \"" + decision->comment + "\"" : std::string());
	} else if (latest.state == "accepted") {
		where = "was accepted by " + who;
	} else if (latest.state == "failed") {
		where = "could not be finished: " + latest.stateMessage;
	} else if (latest.state == "waiting_for_information") {
		where = "waits for an answer: " + latest.stateMessage;
	} else {
		where = "is in progress";
	}
	state.text = "<review_state>\nWhat Singularity holds of the review you did in this thread, the authority on it: say "
		"nothing it does not support.\nThe review: \"" + review.sourceFileName + "\"" + (review.playbookName.empty() == true
			? std::string(", revised as the lawyer asked") : " against " + review.playbookName)
		+ ". Its latest version, "+ std::to_string(latest.number) + ", " + where + ".\n";
	state.facts.version = latest.number;
	state.facts.sourceName = review.sourceFileName;
	state.facts.playbookName = review.playbookName;
	state.facts.lawyer = review.requestedByName;
	state.facts.draftName = latest.resultFileName;
	if (latest.resultFileName.empty() == false) {
		state.text += "Its draft: \"" + latest.resultFileName + "\", attached to your message in the thread, its changes "
			"tracked changes nobody has accepted yet.\n";
	}
	const Json result = Json::parse(latest.result.empty() == true ? std::string("{}") : latest.result, nullptr, false);
	if (result.is_object() == true) {
		std::map<std::string, std::string> topics;
		for (const Json& assessment : result.value("assessments", Json::array())) {
			if (assessment.is_object() == true) topics[assessment.value("positionId", std::string())] = assessment.value("topic", std::string());
		}
		// A clause by the number the agreement gives it: from a change that names
		// it, or from Singularity's own id for a numbered clause ("s-5.2").
		std::map<std::string, std::string> numbers;
		for (const Json& change : result.value("changes", Json::array())) {
			if (change.is_object() == true && change.value("clauseNumber", std::string()).empty() == false) {
				numbers[change.value("blockId", std::string())] = change.value("clauseNumber", std::string());
			}
		}
		const auto placeOf = [&](const std::string& blockId) {
			if (numbers.count(blockId) > 0) return "Section " + numbers[blockId];
			if (blockId.rfind("s-", 0) == 0 && blockId.size() > 2 && std::isdigit(static_cast<unsigned char>(blockId[2])) != 0) {
				return "Section " + blockId.substr(2);
			}
			return blockId;
		};
		std::vector<std::string> written;
		std::vector<std::string> notWritten;
		std::set<std::string> writtenPositions;
		std::set<std::string> writtenBlocks;
		std::set<std::string> proposedPositions;
		for (const Json& change : result.value("changes", Json::array())) {
			if (change.is_object() == false) continue;
			const std::string place = placeOf(change.value("blockId", std::string()));
			const std::string topic = topics[change.value("positionId", std::string())];
			const std::string label = labelOf({place}, topic);
			if (change.value("writeState", std::string()) == "written") {
				written.push_back(label + ": " + change.value("explanation", std::string()));
				writtenPositions.insert(change.value("positionId", std::string()));
				writtenBlocks.insert(change.value("blockId", std::string()));
				continue;
			}
			proposedPositions.insert(change.value("positionId", std::string()));
			const std::string note = change.value("writeNote", std::string()).empty() == false
				? change.value("writeNote", std::string()) : change.value("checkNote", std::string());
			notWritten.push_back(label + ": the change you proposed is not in the draft. " + note);
			UnchangedPoint point;
			point.label = label;
			point.names = {place};
			if (topic.empty() == false) point.names.push_back(topic);
			point.isProposed = true;
			point.why = note;
			state.facts.unchanged.push_back(point);
		}
		// A position the review found off the firm's position that the draft
		// leaves as it was, a change set aside for it or none at all.
		const std::vector<std::string> setAside = result.value("setAside", std::vector<std::string>());
		for (const Json& assessment : result.value("assessments", Json::array())) {
			if (assessment.is_object() == false) continue;
			const std::string kind = assessment.value("assessment", std::string());
			const std::string positionId = assessment.value("positionId", std::string());
			if (kind != "deviates" && kind != "missing") continue;
			if (writtenPositions.count(positionId) > 0 || proposedPositions.count(positionId) > 0) continue;
			std::vector<std::string> places;
			bool isTouched = false;
			for (const Json& block : assessment.value("blockIds", Json::array())) {
				if (block.is_string() == false) continue;
				if (writtenBlocks.count(block.get<std::string>()) > 0) isTouched = true;
				places.push_back(placeOf(block.get<std::string>()));
			}
			if (isTouched == true) continue;
			const std::string topic = assessment.value("topic", std::string());
			const auto aside = std::find_if(setAside.begin(), setAside.end(),
				[&](const std::string& entry) { return entry.rfind("A change to " + topic + " ", 0) == 0; });
			UnchangedPoint point;
			point.label = labelOf(places, topic);
			point.names = places;
			point.names.push_back(topic);
			point.isProposed = aside != setAside.end();
			point.why = point.isProposed == true ? *aside : std::string("No change was made to it.");
			notWritten.push_back(point.label + ": " + (point.isProposed == true
				? "the change you proposed could not be used, so the draft does not change it. " + *aside
				: std::string("still off the firm's position; the draft does not change it.")));
			state.facts.unchanged.push_back(point);
		}
		std::vector<std::string> calls;
		std::vector<std::string> lacking;
		for (const Json& point : result.value("unresolved", Json::array())) {
			if (point.is_object() == false) continue;
			const std::string kind = point.value("kind", std::string());
			if (kind == "needs_lawyer") calls.push_back(point.value("description", std::string()));
			if (kind == "missing_document" || kind == "unresolved_reference") lacking.push_back(point.value("description", std::string()));
		}
		const auto items = [](const std::vector<std::string>& lines) {
			std::string text;
			for (const std::string& line : lines) text += "- " + line + "\n";
			return text;
		};
		// What an earlier draft showed otherwise than meant, and this one reads as
		// meant: settled, though the thread still speaks of it.
		std::set<std::string> troubled;
		for (const ReviewVersion& earlier : review.versions) {
			if (earlier.number >= latest.number || earlier.result.empty() == true) continue;
			const Json before = Json::parse(earlier.result, nullptr, false);
			if (before.is_object() == false) continue;
			for (const Json& change : before.value("changes", Json::array())) {
				if (change.is_object() == true && change.value("writeState", std::string()) == "written"
					&& change.value("writeNote", std::string()).empty() == false) {
					troubled.insert(change.value("blockId", std::string()));
				}
			}
		}
		std::vector<std::string> settled;
		for (const Json& change : result.value("changes", Json::array())) {
			if (change.is_object() == false || change.value("writeState", std::string()) != "written"
				|| change.value("writeNote", std::string()).empty() == false) continue;
			const std::string blockId = change.value("blockId", std::string());
			if (troubled.count(blockId) > 0) {
				settled.push_back(placeOf(blockId) + ": an earlier draft showed it otherwise than meant; this draft, read back from "
					"the saved Word file with every tracked change accepted, reads exactly as meant");
			}
		}
		if (written.empty() == false) state.text += "What the draft changes:\n" + items(written);
		if (settled.empty() == false) state.text += "Settled since earlier drafts (never say they stand):\n" + items(settled);
		if (notWritten.empty() == false) {
			state.text += "What it does not change, though your review found it off the firm's position:\n" + items(notWritten);
		}
		if (calls.empty() == false) state.text += "Decisions you asked for:\n" + items(calls);
		if (lacking.empty() == false) state.text += "What the document lacks:\n" + items(lacking);
	}
	state.text += "</review_state>\n";
	state.plain = "Version " + std::to_string(latest.number) + " " + where + "."
		+ (latest.resultFileName.empty() == false ? " Its draft is attached to my message above." : std::string());
	return state;

}

// What the colleague does with a message about a review it handed in: a
// change to the draft is a new version, made from that message; anything
// else is answered in the thread from the review as it stands.
struct ThreadAnswer {
	bool isRevision = true;
	std::string reply;
	Json record = Json::object();
	std::optional<long long> costMicros;
};

Json threadAnswerSchema() {
	return Json{{"type", "object"}, {"additionalProperties", false}, {"required", Json::array({"revise", "reply"})},
		{"properties", Json{{"revise", Json{{"type", "boolean"}}}, {"reply", Json{{"type", "string"}}}}}};
}

ThreadAnswer answerAboutReview(const ReviewRunInput& input, const DocumentReview& review) {

	ThreadAnswer answer;
	const ReviewState state = reviewStateOf(review);
	HandInCall call;
	try {
		call.model = modelFor(input.matterId, input.isLocalOnly, input.agentId);
	} catch (const std::exception& error) {
		// The review says why it cannot run, as it always has.
		answer.record["why"] = error.what();
		return answer;
	}
	call.isLocal = call.model.provider_id == "local";
	call.capability = routing::model_capability(call.model);
	call.allowance = call.capability.max_output_tokens;
	call.matterId = input.matterId;
	call.chatId = input.chatId;
	call.agentId = input.agentId;
	call.agentName = input.agentName;
	call.agentBrief = input.agentBrief;
	call.answering = input.requestMessageId;
	call.answeringLabel = " (the message you are answering)";
	call.operationId = "review-op-" + platform::secure_random_hex(10);
	call.cancelled = input.cancelled;

	// The call kept as the Matter's operation, for Usage, as the review's is.
	call.taskId = matters::generate_task_id();
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		std::optional<matters::Matter> current = matters::load_matter(workspace, input.matterId);
		if (current.has_value() == true) {
			matters::MatterTask operation;
			operation.id = call.taskId;
			operation.created_at = workspaces::current_timestamp();
			operation.operation_type = "document_review";
			operation.operation_role = "reasoning";
			operation.agent_operation_id = call.operationId;
			operation.agent_id = input.agentId;
			operation.author_actor_id = input.givenByActorId;
			operation.author_name = input.givenByName;
			operation.prompt = "A message about the review of " + review.sourceFileName;
			operation.requested_provider_id = call.model.provider_id;
			operation.requested_model_id = call.model.model_id;
			operation.provider_id = call.model.provider_id;
			operation.model_id = call.model.model_id;
			operation.execution_kind = call.isLocal == true ? "local" : "external";
			if (call.isLocal == true) operation.runtime_id = "local";
			operation.local_only = input.isLocalOnly;
			operation.operation_device_id = platform::current_device_id();
			operation.document_review_id = review.id;
			operation.status = matters::TaskStatus::Success;
			current->tasks.push_back(std::move(operation));
			matters::save_matter(workspace, *current);
		}
	} catch (const std::exception&) {
	}

	std::optional<matters::Matter> matter;
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = matters::load_matter(node::require_active_workspace_path(), input.matterId);
	} catch (const std::exception&) {
	}
	const std::string asker = input.givenByName.empty() == true ? std::string("a lawyer") : input.givenByName;
	const std::string task = "<message_to_you>\n" + asker + ": " + input.instruction + "\n</message_to_you>\n\n"
		"<task>\nThe message above, from " + asker + ", is to you.\nIf it asks you to change the draft in any way - to revise, add, remove, keep, restore or reword "
		"anything in it - answer with \"revise\": true and an empty \"reply\": the change is made as a new version of the review, from that message.\n"
		"Otherwise answer with \"revise\": false and, in \"reply\", the message you post in the thread in answer: answer "
		"what was asked, from <review_state> and the thread, as you would to a colleague. The thread already holds what "
		"the review found: say again only what bears on the question, and correct anything said there that "
		"<review_state> does not support.\nAnswer with the JSON object only.\n</task>\n";
	std::optional<std::size_t> room;
	if (call.capability.context_window_tokens.has_value() == true) {
		const std::size_t fixed = research::estimate_tokens(call.agentBrief) + research::estimate_tokens(state.text)
			+ research::estimate_tokens(task) + static_cast<std::size_t>(call.allowance.value_or(0));
		const std::size_t window = static_cast<std::size_t>(*call.capability.context_window_tokens);
		room = window > fixed ? window - fixed : 0;
	}
	const std::string thread = matter.has_value() == true ? threadText(*matter, call, room) : std::string();
	const std::string prompt = call.agentBrief + "\n\n" + thread + "\n" + state.text + "\n" + task;
	const auto read = [](const std::string& text) {
		const Json parsed = Json::parse(text, nullptr, false);
		return parsed.is_object() == true && parsed.contains("revise") == true && parsed.at("revise").is_boolean() == true
			? parsed : Json();
	};
	const AfterWork written = askAfterWork(call, prompt,
		providers::GenerationOutputContract{"thread_answer", threadAnswerSchema()},
		[&](const std::string& text) {
			const Json parsed = read(text);
			if (parsed.is_null() == true) return std::vector<std::string>{"It is not the JSON object asked for."};
			if (parsed.at("revise").get<bool>() == true) return std::vector<std::string>();
			return contradictions(parsed.value("reply", std::string()), state.facts, true);
		},
		"Answer again from <review_state>, with the JSON object only.");
	answer.costMicros = written.costMicros;
	answer.record["attempts"] = written.attempts;
	if (written.costMicros.has_value() == true) {
		answer.record["costMicros"] = *written.costMicros;
		answer.record["costSource"] = written.costSource;
	}
	const Json parsed = read(written.text);
	if (written.isAgreed == true) {
		answer.isRevision = parsed.at("revise").get<bool>();
		answer.reply = polished(parsed.value("reply", std::string()), input.givenByName);
		answer.record["answer"] = answer.isRevision == true ? "revise" : "reply";
		return answer;
	}
	if (written.attempts.empty() == true || parsed.is_null() == true) {
		// Nothing usable came: the message is taken as it always was, as work on
		// the draft, and the review says for itself if it cannot run.
		answer.record["answer"] = "revise";
		answer.record["why"] = written.why;
		return answer;
	}
	// A reply that still says what the review does not hold gives way to where
	// the review stands, said plainly.
	answer.isRevision = false;
	answer.reply = state.plain;
	answer.record["answer"] = "reply";
	answer.record["why"] = written.why + " Where the review stands was said plainly.";
	return answer;

}

std::string markdownCell(std::string text) {
	for (char& character : text) {
		if (character == '\n' || character == '\r') character = ' ';
	}
	std::string escaped;
	for (const char character : text) {
		if (character == '|') escaped += "\\|";
		else escaped += character;
	}
	return escaped;
}

// The work as a Work Product reads it: the summary, the changes clause by
// clause with what the source says, and what is still open.
std::string workProductContent(const DocumentReview& review, int versionNumber, const intelligence::ValidatedReview& validated) {

	std::string content = "# " + review.agentName + "'s review of " + review.sourceFileName + ", version "
		+ std::to_string(versionNumber) + "\n\n";
	content += review.playbookName.empty() == true ? std::string("Revised as the lawyer asked; no Playbook of the firm covers it.\n\n")
		: "Against " + review.playbookName + " (version " + std::to_string(review.playbookVersion) + ").\n\n";
	if (validated.summary.empty() == false) content += validated.summary + "\n\n";
	if (validated.changes.empty() == false) {
		content += "## Proposed changes\n\n| Clause | Position | Now | Proposed | Source | In the draft |\n"
			"| --- | --- | --- | --- | --- | --- |\n";
		for (const intelligence::ProposedChange& change : validated.changes) {
			content += "| " + markdownCell(change.clauseNumber.empty() == false ? change.clauseNumber : change.blockId) + " | "
				+ markdownCell(change.positionId) + " | " + markdownCell(change.currentText) + " | "
				+ markdownCell(change.action == "delete" ? std::string("(struck)") : change.proposedText) + " | "
				+ intelligence::toString(change.check) + " | " + markdownCell(change.writeState) + " |\n";
		}
		content += "\n";
	}
	if (validated.unresolved.empty() == false) {
		content += "## Still open\n\n";
		for (const intelligence::UnresolvedPoint& point : validated.unresolved) content += "- " + point.description + "\n";
	}
	return content;

}

} // namespace

bool isReviewRequest(const std::string& matterId, const std::string& chatId, const std::string& agentId,
	const std::string& instruction) {

	try {
		if (openReviewInThread(matterId, chatId, agentId).has_value() == true) return true;
	} catch (const std::exception&) {
		return false;
	}
	const std::string words = lowered(instruction);
	for (const char* signal : {"review", "revise", "revision", "mark up", "markup", "mark-up", "redline", "draft",
		"against our playbook", "against the playbook"}) {
		if (words.find(signal) != std::string::npos) return true;
	}
	return false;

}

// What a colleague says as it takes the work up, in reply to the message that
// gave it (the owner, 2026-10-24: "if the agent starts work, let it write a
// message as a message in the chat, varied, as a reply"). Said in a few words
// of what it is about to do, never of what it has done; the words vary from
// one piece of work to the next as a colleague's do.
std::string startWords(const std::string& seed, const std::string& documentName, const std::string& asker,
	int versionNumber, bool isTakenUpAgain, bool isAnswered) {

	std::string name = std::filesystem::u8path(documentName).stem().u8string();
	if (name.empty() == true) name = "the agreement";
	else name = "the " + name;
	const std::string first = asker.substr(0, asker.find(' '));
	const std::string thanks = first.empty() == true ? std::string("Thanks.") : "Thanks, " + first + ".";
	const std::string next = std::to_string(versionNumber);
	std::vector<std::string> ways;
	if (isTakenUpAgain == true) {
		ways = {"Trying again now.", "Picking this back up.", "Back on it.", "Let me have another go at this."};
	} else if (isAnswered == true) {
		ways = {thanks + " Starting on " + name + " now.", "Got it. I'll send the redline here.",
			"That's the one I'll take. On it now."};
	} else if (versionNumber > 1) {
		ways = {thanks + " Working that into version " + next + " now.", "Understood. I'll revise the draft and post version "
			+ next + " here.", "Got it. Making those changes now.", "On it. Version " + next + " will follow in this thread.",
			"Will do. I'm updating the draft now."};
	} else {
		ways = {"On it. I'll go through " + name + " against our playbook and send you a marked-up draft.",
			"Will do. I'm reviewing " + name + " now; the redline will follow here.",
			"Got it. I'll take " + name + " through the playbook and come back with a revised draft.",
			"Starting on " + name + " now. You'll have my markup in this thread.",
			thanks + " I'll review " + name + " and put my changes in a draft for you."};
	}
	return ways[std::hash<std::string>{}(seed) % ways.size()];

}

namespace {

// What the version did, as its record keeps it (D-119): what it was meant to
// do, the source and its exact version, each change proposed and what became
// of it with where its values come from, what the saved draft was checked for,
// and what is left for the lawyer.
Json stepRecordOf(const intelligence::ValidatedReview& validated, const intelligence::LegalDocument& document,
	const intelligence::IntegrityReport& integrity, const intelligence::RevisionOutcome& revision, bool isUnchanged,
	const Json& origins, const ReviewRunOutput& output, const std::filesystem::path& sourcePath, const std::string& readSha,
	const std::string& sourceId, const std::string& sourceName, const std::string& instruction) {

	Json operations = Json::array();
	std::size_t applied = 0;
	std::vector<std::string> remaining;
	for (const intelligence::ProposedChange& change : validated.changes) {
		const std::string place = change.clauseNumber.empty() == false ? "Section " + change.clauseNumber
			: placeWords(document, change.blockId);
		const bool isWritten = change.writeState == "written";
		if (isWritten == true) ++applied;
		const std::string note = change.writeNote.empty() == false ? change.writeNote : change.checkNote;
		Json operation{{"id", change.id}, {"place", place}, {"kind", change.action}, {"before", change.currentText},
			{"after", change.proposedText}, {"why", change.explanation}, {"state", isWritten == true ? "applied" : "refused"},
			{"note", note}, {"readBack", change.readBack}};
		for (const Json& entry : origins) {
			if (entry.value("change", std::string()) == change.id) operation["origins"] = entry.value("origins", Json::array());
		}
		operations.push_back(operation);
		if (isWritten == false) remaining.push_back(place + " was not written: " + (note.empty() == true ? std::string("it could "
			"not be placed in the agreement.") : note));
	}
	for (const intelligence::MarginComment& comment : validated.comments) {
		const std::string place = comment.clauseNumber.empty() == false ? "Section " + comment.clauseNumber
			: placeWords(document, comment.blockId);
		const bool isWritten = comment.writeState == "written";
		operations.push_back(Json{{"id", "comment-" + comment.blockId}, {"place", place}, {"kind", "comment"},
			{"before", std::string()}, {"after", comment.text}, {"why", std::string()},
			{"state", isWritten == true ? "applied" : "refused"}, {"note", comment.writeNote}, {"readBack", std::string()}});
		if (isWritten == false) remaining.push_back("The comment on " + place + " was not written: " + comment.writeNote);
	}
	for (const std::string& value : output.unsourced) remaining.push_back("Unsourced: " + value);
	Json checks = Json::array();
	if (revision.isWritten == true) {
		checks.push_back(Json{{"kind", "read_back"}, {"state", "passed"}, {"words", std::to_string(applied) + " of "
			+ std::to_string(validated.changes.size()) + " proposed changes written as tracked changes and read back from the "
			"saved draft"}});
	}
	for (const intelligence::IntegrityCheckResult& check : integrity.checks) {
		checks.push_back(Json{{"kind", check.name}, {"state", check.state == "failed" ? "failed" : "passed"}, {"words", check.summary}});
	}
	if (integrity.problem.empty() == false) checks.push_back(Json{{"kind", "integrity"}, {"state", "failed"}, {"words", integrity.problem}});
	std::string now;
	try {
		now = platform::sha256_hex_digest_of_file(sourcePath);
	} catch (const std::exception&) {
	}
	const bool isSame = readSha.empty() == false && now == readSha;
	checks.push_back(Json{{"kind", "original"}, {"state", isSame == true ? "passed" : "failed"}, {"words", isSame == true
		? "The original is byte for byte as it was read." : "The original changed while the review worked."}});
	const std::string statusWords = revision.isWritten == false ? std::string("No draft was made.") : isUnchanged == true
		? std::string("No new draft: it would read as the last version.") : "Revised: " + std::to_string(applied) + " of "
			+ std::to_string(validated.changes.size()) + " proposed changes written as tracked changes, read back from the saved "
			"draft" + (remaining.empty() == true ? std::string(".") : "; " + std::to_string(remaining.size())
				+ (remaining.size() == 1 ? " point" : " points") + " left for you.");
	return Json{{"intended", instruction}, {"source", Json{{"documentId", sourceId}, {"name", sourceName}, {"sha256", readSha}}},
		{"proposed", operations.size()}, {"applied", applied}, {"operations", operations}, {"checks", checks},
		{"remaining", remaining}, {"recoveries", output.recoveries}, {"calls", output.calls},
		{"status", revision.isWritten == true && isUnchanged == false ? "completed" : "unchanged"}, {"statusWords", statusWords}};

}

// Whether a document of the Matter is a colleague's own work (D-172): a
// review's draft, a document a colleague wrote, or an email it drafted.
bool isColleagueWork(const matters::Matter& matter, const std::string& documentId) {
	for (const DocumentReview& review : matter.document_reviews)
		for (const ReviewVersion& version : review.versions)
			if (version.resultDocumentId == documentId) return true;
	for (const AssignmentRecord& record : matter.assignment_records) {
		if (record.email.documentId == documentId) return true;
		for (const AssignmentStepRecord& step : record.steps)
			if (step.documentId == documentId && step.writtenSha256.empty() == false) return true;
	}
	return false;
}

// Whose writings of the parties a draft is checked against (D-172): the
// source's, unless it is a colleague's own work - a reply revised four times
// from a draft that misnamed the defendant kept "Coastline Logistics Corp."
// while every pleading writes "Coastline Logistics, Inc." (2026-10-31). Then
// the Matter's own documents decide, each read as it is.
intelligence::PartyAuthority partyAuthorityFor(const matters::Matter& matter, const matters::MatterDocument& source,
	FactsReading* facts) {
	intelligence::PartyAuthority authority;
	authority.isSourceAuthoritative = isColleagueWork(matter, source.id) == false;
	if (authority.isSourceAuthoritative == true) return authority;
	const std::filesystem::path folder = std::filesystem::u8path(matter.directory_path.value_or(std::string()));
	if (folder.empty() == true) return authority;
	FactsReading local;
	FactsReading& reading = facts != nullptr ? *facts : local;
	for (const matters::MatterDocument& other : matter.documents) {
		if (other.available == false || other.id == source.id || isColleagueWork(matter, other.id) == true) continue;
		if (documents::registry::is_relative_path_safe(folder, other.relative_path) == false) continue;
		const std::filesystem::path path = (folder / std::filesystem::u8path(other.relative_path)).lexically_normal();
		std::string words;
		try {
			if (lowered(path.extension().u8string()) == ".docx") {
				const intelligence::StructureReading read = intelligence::readDocxStructure(intelligence::takeInSource(path));
				if (read.isRead == true)
					for (const intelligence::LegalBlock& block : read.document.blocks)
						if (block.section != "comment") words += block.text + "\n";
			} else {
				const DocumentFacts& read = reading.of(path, other.id, other.name);
				for (const intelligence::Statement& statement : read.statements) words += statement.label + "\n" + statement.text + "\n";
			}
		} catch (const std::exception&) {
		}
		if (words.empty() == false) authority.texts.push_back(std::move(words));
	}
	return authority;
}

// A colleague's own draft that misnames a party is written again with the party
// named as the Matter's documents write it (D-172): in the changes the model
// proposed, and as Singularity's own rewrite of every other clause that still
// misnames it - a reply kept "Coastline Logistics Corp." in 21 places that one
// pass could not reach (2026-10-31). Each correction says why, where its record
// shows the reasoning.
void correctPartyNames(intelligence::ValidatedReview& answer, const intelligence::LegalDocument& document,
	const std::map<std::string, std::string>& corrections) {
	const auto corrected = [&](std::string text) {
		for (const auto& [wrong, right] : corrections) {
			for (std::size_t at = text.find(wrong); at != std::string::npos; at = text.find(wrong, at + right.size()))
				text.replace(at, wrong.size(), right);
		}
		return text;
	};
	std::set<std::string> rewritten;
	for (intelligence::ProposedChange& change : answer.changes) {
		if (change.action == "rewrite") rewritten.insert(change.blockId);
		if (change.isWritable == false) continue;
		const std::string fixed = corrected(change.proposedText);
		if (fixed == change.proposedText) continue;
		if (change.action == "rewrite") {
			const intelligence::ClauseRewrite made = intelligence::rewriteOfClause(document, change.currentText, fixed);
			if (made.edits.empty() == true) continue;
			change.proposedText = made.text;
			change.edits = made.edits;
			change.redline = made.redline;
		} else if (change.action == "table") {
			change.proposedText = fixed;
			for (intelligence::TableEdit& edit : change.tableEdits) {
				edit.proposedText = corrected(edit.proposedText);
				for (std::string& cell : edit.cells) cell = corrected(cell);
			}
		} else {
			change.proposedText = fixed;
		}
	}
	const std::string positionId = answer.changes.empty() == true ? std::string(intelligence::kRequestPositionId)
		: answer.changes.front().positionId;
	int number = 0;
	for (const intelligence::LegalBlock& block : document.blocks) {
		const bool isBody = block.section == "body" || block.section.rfind("exhibit", 0) == 0;
		if (isBody == false || block.kind == intelligence::BlockKind::Table || rewritten.count(block.id) > 0) continue;
		const std::string fixed = corrected(block.text);
		if (fixed == block.text) continue;
		const intelligence::ClauseRewrite made = intelligence::rewriteOfClause(document, block.text, fixed);
		if (made.edits.empty() == true) continue;
		intelligence::ProposedChange change;
		change.id = "chg-party-" + std::to_string(++number);
		change.positionId = positionId;
		change.action = "rewrite";
		change.citedClause = block.id;
		change.blockId = block.id;
		change.clauseNumber = block.number;
		change.currentText = block.text;
		change.proposedText = made.text;
		change.edits = made.edits;
		change.redline = made.redline;
		std::string named;
		for (const auto& [wrong, right] : corrections)
			if (block.text.find(wrong) != std::string::npos) named += (named.empty() == true ? "" : ", ") + std::string("\xE2\x80\x9C") + right + "\xE2\x80\x9D";
		change.explanation = "Names the party as the Matter's documents write it: " + named + ".";
		change.confidence = "high";
		change.check = intelligence::SourceCheck::Verified;
		change.isWritable = true;
		answer.changes.push_back(std::move(change));
	}
}

} // namespace

ReviewRunOutput runDocumentReview(const ReviewRunInput& input) {

	ReviewRunOutput output;
	const auto now = []() { return workspaces::current_timestamp(); };
	Json stages = Json::array();
	const auto stage = [&](const std::string& name) { stages.push_back(Json{{"stage", name}, {"at", now()}}); };

	/* - - - - - - */
	// 1. Which review, and which version of it.

	std::optional<DocumentReview> open;
	matters::Matter matter;
	try {
		matter = loadMatter(input.matterId);
		// A part of an assignment works on its own document: the review of it this
		// colleague has open in the thread, or a new one (D-115). Otherwise the
		// review the request is about: the one it returns or tries again, else the
		// latest not yet accepted.
		open = input.sourceDocumentId.empty() == true
			? reviewAskedAbout(matter, input.chatId, input.agentId, input.requestMessageId)
			: openReviewOf(matter, input.chatId, input.agentId, input.sourceDocumentId);
	} catch (const std::exception& error) {
		output.reason = error.what();
		return output;
	}
	std::string instruction = input.instruction;
	std::string requestMessageId = input.requestMessageId;
	int versionNumber = 1;
	int attempt = 1;
	bool isSameVersion = false;
	// Taken up again with the same words (Try again, or Singularity closed on
	// it), or after the lawyer named the document asked about.
	bool isTakenUpAgain = false;
	bool isAnswered = false;
	// What reading a message about a handed-in review cost, when it turned out
	// to be work on the draft: counted in what this run cost.
	long long askedMicros = 0;
	if (open.has_value() == true) {
		const ReviewVersion& latest = open->versions.back();
		const bool isUnfinished = latest.state == "failed" || latest.state == "waiting_for_information"
			|| latest.state == "unchanged" || latest.state == "queued" || latest.state == "preparing"
			|| latest.state == "running" || latest.state == "producing";
		// A new message to the colleague about the review it handed in (D-101):
		// a change to the draft is a new version; a question, a status request or
		// a remark is answered from the review as it stands. A return with
		// comments is a change by its nature, and a retry or an answer to the
		// colleague's own question carries on the version it is for.
		const bool isReturnItself = latest.state == "returned" && open->decisions.empty() == false
			&& open->decisions.back().messageId == input.requestMessageId;
		const bool isNewMessage = input.requestMessageId.empty() == false && input.requestMessageId != latest.requestMessageId;
		// A part of an assignment is work by its nature, never a question.
		if (input.assignment.empty() == true
				&& (latest.state == "ready_for_review" || latest.state == "returned" || latest.state == "failed"
				|| latest.state == "unchanged") && isReturnItself == false && isNewMessage == true) {
			const ThreadAnswer answer = answerAboutReview(input, *open);
			// What the colleague made of the message is kept with the version it
			// was about, with what it cost ("How it was done").
			try {
				(void)changeReview(input.matterId, open->id, [&](DocumentReview& changed) {
					ReviewVersion& version = changed.versions.back();
					Json preparation = Json::parse(version.execution.preparation.empty() == true ? std::string("{}")
						: version.execution.preparation, nullptr, false);
					if (preparation.is_object() == false) preparation = Json::object();
					Json entry = answer.record;
					entry["messageId"] = input.requestMessageId;
					entry["at"] = now();
					preparation["answers"].push_back(entry);
					version.execution.preparation = preparation.dump();
				});
			} catch (const std::exception&) {
			}
			if (answer.isRevision == true) askedMicros = answer.costMicros.value_or(0);
			if (answer.isRevision == false) {
				output.reviewId = open->id;
				output.version = latest.number;
				output.status = "completed";
				if (answer.costMicros.has_value() == true) {
					output.cost = static_cast<double>(*answer.costMicros) / 1000000.0;
					output.currency = "USD";
				}
				try {
					// In reply to the message it answers, as the colleague's word that it
					// is on the work is: the thread, Word's pane and the clause the message
					// was about all hold the answer to it.
					chat::MessageExtras extras;
					extras.quote_message_id = input.requestMessageId;
					std::string messageId;
					try {
						messageId = chat::post_agent_message(input.matterId, input.chatId, input.agentId,
							input.agentName, answer.reply, input.isLocalOnly, extras);
					} catch (const std::invalid_argument&) {
						// A request that is no message of this thread is answered plainly.
						messageId = chat::post_agent_message(input.matterId, input.chatId, input.agentId,
							input.agentName, answer.reply, input.isLocalOnly);
					}
					linkTurn(input.matterId, input.requestMessageId, open->id);
					linkTurn(input.matterId, messageId, open->id);
					output.excerpt = answer.reply.substr(0, answer.reply.find('\n'));
				} catch (const std::exception& error) {
					output.status = "failed";
					output.reason = error.what();
				}
				return output;
			}
		}
		if (isUnfinished == true) {
			// The same version again: a retry, work Singularity closed on, or
			// the lawyer's answer to the colleague's question.
			isSameVersion = true;
			versionNumber = latest.number;
			attempt = latest.execution.attempt + 1;
			const bool isNewWords = (latest.state == "waiting_for_information" || latest.state == "unchanged")
				&& input.requestMessageId.empty() == false && input.requestMessageId != latest.requestMessageId;
			if (isNewWords == false) {
				instruction = latest.instruction;
				requestMessageId = latest.requestMessageId;
			}
			isTakenUpAgain = isNewWords == false && latest.state != "waiting_for_information";
			isAnswered = isNewWords == true && latest.state == "waiting_for_information";
		} else {
			versionNumber = latest.number + 1;
		}
	}
	output.version = versionNumber;

	/* - - - - - - */
	// 2. The source.

	const matters::MatterDocument* source = nullptr;
	const std::string sourceId = input.sourceDocumentId.empty() == false ? input.sourceDocumentId
		: open.has_value() == true ? open->sourceDocumentId : std::string();
	if (sourceId.empty() == false) {
		for (const matters::MatterDocument& document : matter.documents) {
			if (document.id == sourceId && document.available == true) source = &document;
		}
	}
	std::string question;
	if (source == nullptr) {
		const SourceChoice choice = chooseSource(matter, input.chatId, input.instruction + " " + instruction, requestMessageId);
		source = choice.document;
		question = choice.question;
	}
	// A document no Playbook covers is revised as the lawyer asks (D-115); a
	// review begun that way goes on that way.
	const bool isRequestOnly = input.isRequestOnly == true
		|| (open.has_value() == true && open->playbookId.empty() == true && open->versions.empty() == false);
	std::optional<playbooks::Playbook> playbook = isRequestOnly == true ? std::nullopt : playbooks::get(input.playbookId);

	// The record of the review, before anything is read or sent.
	DocumentReview review;
	try {
		ReviewVersion version;
		version.number = versionNumber;
		version.requestMessageId = requestMessageId;
		version.instruction = instruction;
		version.createdAt = now();
		version.state = source == nullptr ? "waiting_for_information" : "preparing";
		version.stateMessage = question;
		version.execution.runId = input.runId;
		version.execution.assignmentId = input.assignmentId;
		version.execution.attempt = attempt;
		version.execution.startedAt = version.createdAt;
		if (open.has_value() == false) {
			review.id = "review_" + platform::secure_random_hex(8);
			review.chatId = input.chatId;
			review.agentId = input.agentId;
			review.agentName = input.agentName;
			review.requestedByActorId = input.givenByActorId;
			review.requestedByName = input.givenByName;
			if (playbook.has_value() == true) {
				review.playbookId = playbook->id;
				review.playbookName = playbook->name;
				review.playbookVersion = playbook->version;
			}
			if (source != nullptr) {
				review.sourceDocumentId = source->id;
				review.sourceRelativePath = source->relative_path;
				review.sourceFileName = source->name;
				review.sourceVersion = source->version;
			}
			review.versions.push_back(version);
			review = addReview(input.matterId, review);
		} else {
			review = changeReview(input.matterId, open->id, [&](DocumentReview& changed) {
				if (changed.sourceDocumentId.empty() == true && source != nullptr) {
					changed.sourceDocumentId = source->id;
					changed.sourceRelativePath = source->relative_path;
					changed.sourceFileName = source->name;
					changed.sourceVersion = source->version;
				}
				if (isSameVersion == true) {
					changed.versions.back() = version;
				} else {
					// A version the lawyer asked to revise is no longer the one
					// awaiting review: the new one is.
					ReviewVersion& earlier = changed.versions.back();
					if (earlier.state == "ready_for_review") {
						earlier.state = "superseded";
						earlier.stateMessage = "Version " + std::to_string(version.number) + " replaced it.";
					}
					changed.versions.push_back(version);
				}
			});
		}
		linkTurn(input.matterId, requestMessageId, review.id);
	} catch (const std::exception& error) {
		output.reason = error.what();
		return output;
	}
	output.reviewId = review.id;

	// Every ending of this run is written into the version and said in the
	// thread, in the colleague's words - unless the run is a part of the
	// colleague's work (isQuiet), whose loop says what came of it in its own
	// message: a lease's part once posted "The model could not be asked: ...
	// (domain=NSCocoaErrorDomain code=257)" beside the colleague's closing.
	const auto finish = [&](const std::string& state, const std::string& message, const std::string& said) {
		try {
			review = changeReview(input.matterId, review.id, [&](DocumentReview& changed) {
				ReviewVersion& version = changed.versions.back();
				version.state = state;
				version.stateMessage = message;
				version.execution.finishedAt = now();
				stage(state);
				version.execution.stages = stages.dump();
			});
			if (said.empty() == false && input.isQuiet == false && input.isHandedInByAssignment == false) {
				const std::string messageId = chat::post_agent_message(input.matterId, input.chatId, input.agentId,
					input.agentName, said, input.isLocalOnly);
				linkTurn(input.matterId, messageId, review.id);
				output.excerpt = said.substr(0, said.find('\n'));
			}
		} catch (const std::exception&) {
		}
		return output;
	};
	const auto fail = [&](const std::string& reason) {
		output.status = "failed";
		output.reason = reason;
		const std::string about = review.sourceFileName.empty() == true ? std::string("the review")
			: "the review of " + review.sourceFileName;
		return finish("failed", reason, "I couldn't finish " + about + ". " + reason + " Nothing in the Matter was changed.");
	};

	if (source == nullptr) {
		output.status = "waiting";
		output.reason = question;
		return finish("waiting_for_information", question, question);
	}
	if (isRequestOnly == false && playbook.has_value() == false) {
		return fail("The Playbook I review against is no longer on " + computerWords() + ". Choose one in my settings.");
	}

	/* - - - - - - */
	// 3. Preparation.

	stage("preparing");
	const auto preparationStarted = std::chrono::steady_clock::now();
	const std::filesystem::path folder = std::filesystem::u8path(matter.directory_path.value_or(std::string()));
	if (folder.empty() == true || documents::registry::is_relative_path_safe(folder, source->relative_path) == false) {
		return fail("The document is not inside this Matter's folder, so it was not read.");
	}
	const std::filesystem::path sourcePath = (folder / std::filesystem::u8path(source->relative_path)).lexically_normal();
	intelligence::SourceIntake intake = intelligence::takeInSource(sourcePath);
	if (intake.isAccepted == false) return fail(intake.refusal);
	intake.identity.matterId = input.matterId;
	intake.identity.documentId = source->id;
	intake.identity.documentVersion = source->version;
	bool isFromCache = false;
	intelligence::StructureReading reading = readStructure(sourcePath, intake, isFromCache);
	if (reading.isRead == false) return fail(reading.problem.empty() == false ? reading.problem : std::string("The document could not be read."));
	intelligence::LegalDocument& document = reading.document;
	document.source = intake.identity;

	intelligence::ReviewTask task;
	task.colleagueBrief = input.agentBrief;
	task.instruction = instruction;
	if (isRequestOnly == true) {
		task.positions = {intelligence::requestPosition(instruction)};
	} else {
		task.playbookName = playbook->name;
		task.playbookVersion = playbook->version;
		task.positions = positionsOf(*playbook);
	}
	task.version = versionNumber;
	// A part of an assignment: the lawyer's words, what came before it, the
	// documents it rests on, and the clauses its instruction names (D-115).
	task.assignment = input.assignment;
	task.earlierInAssignment = input.earlierInAssignment;
	task.supporting = supportingOf(matter, folder, input.supportingDocumentIds, source->id);
	if (input.assignment.empty() == false) task.onlyPlaces = placesNamedIn(instruction, document);
	// The last version, and its draft as Word holds it (D-102): what the lawyer
	// read, what the request is about, and what this version is measured by.
	std::optional<ReviewVersion> previous;
	for (const ReviewVersion& earlier : review.versions) {
		if (versionNumber > 1 && earlier.number == versionNumber - 1) previous = earlier;
	}
	std::optional<intelligence::LegalDocument> previousDraft;
	if (previous.has_value() == true && previous->resultRelativePath.empty() == false
			&& documents::registry::is_relative_path_safe(folder, previous->resultRelativePath) == true) {
		const std::filesystem::path path = (folder / std::filesystem::u8path(previous->resultRelativePath)).lexically_normal();
		std::error_code exists;
		if (std::filesystem::exists(path, exists) == true) {
			const intelligence::StructureReading read = intelligence::readDocxStructure(intelligence::takeInSource(path));
			if (read.isRead == true) previousDraft = read.document;
		}
	}
	if (previous.has_value() == true) {
		task.previousChanges = previousChangesOf(*previous, previousDraft.has_value() == true ? &*previousDraft : nullptr);
	}
	// A clean copy - no tracked changes - is read against the version the
	// Matter held before it, so the other side's changes are found by
	// comparing the files, never left to the model to guess (D-111).
	std::optional<EarlierVersion> earlier;
	if (versionNumber == 1 && document.pendingRevisions == 0 && intake.identity.format == "docx") {
		earlier = earlierVersionOf(matter, *source, folder, document);
		if (earlier.has_value() == true) {
			task.comparedWith = earlier->name;
			task.changesAgainstEarlier = earlier->changes;
		}
	}
	const intelligence::PreparedContext prepared = intelligence::prepareReviewContext(document, task);
	Json preparation = intelligence::preparedContextSummary(prepared);
	if (earlier.has_value() == true) {
		preparation["comparedWith"] = Json{{"documentId", earlier->documentId}, {"name", earlier->name},
			{"changes", earlier->changes.size()}};
	}
	preparation["structureFromCache"] = isFromCache;
	preparation["sourceSha256"] = intake.identity.sha256;
	const long long preparationMilliseconds = millisecondsSince(preparationStarted);
	stage("prepared");
	review = changeReview(input.matterId, review.id, [&](DocumentReview& changed) {
		changed.sourceSha256 = intake.identity.sha256;
		changed.structure = structureSummary(document);
		ReviewVersion& version = changed.versions.back();
		version.state = "running";
		version.execution.preparation = preparation.dump();
		version.execution.preparationMilliseconds = preparationMilliseconds;
		version.execution.stages = stages.dump();
	});

	/* - - - - - - */
	// 4. The model.

	routing::ModelTarget model;
	try {
		model = modelFor(input.matterId, input.isLocalOnly, input.agentId);
	} catch (const std::exception& error) {
		return fail(error.what());
	}
	const bool isLocal = model.provider_id == "local";
	// What the model can do, as its provider or Singularity reports it, and the
	// allowance the request takes from it: the model's own limit, or none.
	const routing::ModelCapability capability = routing::model_capability(model);
	std::optional<std::uint64_t> allowance = capability.max_output_tokens;
	preparation["capability"] = capabilityToJson(capability);
	preparation["allowance"] = Json{{"outputTokens", allowance.has_value() == true ? Json(*allowance) : Json()},
		{"basis", allowance.has_value() == true ? "the most the model writes in one answer, as "
			+ std::string(capability.source == "local" ? "Singularity measures it on this computer" : "its provider reports it")
			: std::string("no limit is reported for this model, so none is sent and the provider's own applies")}};
	// A limit this computer chose on its spend is kept (D-118); with none
	// chosen, nothing here holds the review back (D-132).
	const ReviewBudget budget = isLocal == true ? ReviewBudget() : loadReviewBudget();
	if (isLocal == false && budget.limitUsd.has_value() == true) {
		if (allowance.has_value() == false) {
			return fail("Singularity cannot tell what this review could cost: the provider reports no limit on how much "
				"this model writes, so this computer's budget for reviews could not be kept and nothing was sent. "
				+ modelChooser(true) + " can choose a model on my page whose provider reports its limits.");
		}
		// The model's whole limit where the budget's remainder covers it, else
		// what the remainder pays for (D-118).
		const std::uint64_t affordable = affordableOutputTokens(prepared.prompt.size(), *allowance,
			capability.input_price_per_token, capability.output_price_per_token, budget, kLeastReviewOutputTokens);
		if (affordable == 0) {
			char spent[32];
			char most[32];
			std::snprintf(spent, sizeof(spent), "%.2f", static_cast<double>(budget.spentMicros) / 1000000.0);
			std::snprintf(most, sizeof(most), "%.2f", *budget.limitUsd);
			return fail(std::string("This computer's budget for reviews by external models is $") + most + ", and $" + spent
				+ " of it is spent; what is left cannot pay for a review of this agreement, so nothing was sent.");
		}
		if (affordable < *allowance) {
			allowance = affordable;
			preparation["allowance"] = Json{{"outputTokens", *allowance}, {"basis", "what is left of this computer's budget for "
				"reviews pays for, below the most the model writes in one answer"}};
		}
	}
	const std::string operationId = "review-op-" + platform::secure_random_hex(10);
	const std::string taskId = matters::generate_task_id();
	output.operationId = operationId;
	tools::OperationAuthority authority;
	authority.origin = tools::OperationOrigin::Agent;
	authority.agent_id = input.agentId;
	authority.actor_id = platform::resolve_actor_identity().actor_id;
	// What the Matter's own mode lets any member do (D-158).
	authority.local_only = input.isLocalOnly;
	tools::begin_operation_authority(input.matterId, operationId, authority);
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		std::optional<matters::Matter> current = matters::load_matter(workspace, input.matterId);
		if (current.has_value() == true) {
			matters::MatterTask operation;
			operation.id = taskId;
			operation.created_at = now();
			operation.operation_type = "document_review";
			operation.operation_role = "reasoning";
			operation.agent_operation_id = operationId;
			operation.agent_id = input.agentId;
			operation.author_actor_id = input.givenByActorId;
			operation.author_name = input.givenByName;
			// The record says what was done, never the document's words.
			operation.prompt = "Document review: " + source->name + ", version " + std::to_string(versionNumber);
			operation.requested_provider_id = model.provider_id;
			operation.requested_model_id = model.model_id;
			operation.provider_id = model.provider_id;
			operation.model_id = model.model_id;
			operation.execution_kind = isLocal == true ? "local" : "external";
			if (isLocal == true) operation.runtime_id = "local";
			operation.local_only = input.isLocalOnly;
			operation.operation_device_id = platform::current_device_id();
			operation.document_review_id = review.id;
			operation.status = matters::TaskStatus::Success;
			current->tasks.push_back(std::move(operation));
			matters::save_matter(workspace, *current);
		}
	} catch (const std::exception& error) {
		return fail(std::string("The review could not be recorded in the Matter: ") + error.what());
	}

	// The colleague says it is on it, as a colleague does: in its own words, in
	// reply to the message that gave it the work - once for a whole assignment,
	// never once a document of it.
	if (input.isQuiet == false) try {
		const std::string words = startWords(requestMessageId + "#" + std::to_string(attempt), review.sourceFileName,
			input.givenByName, versionNumber, isTakenUpAgain, isAnswered);
		std::string messageId;
		try {
			chat::MessageExtras reply;
			reply.quote_message_id = requestMessageId;
			messageId = chat::post_agent_message(input.matterId, input.chatId, input.agentId, input.agentName, words,
				input.isLocalOnly, reply);
		} catch (const std::exception&) {
			// A request that is no message of this thread is answered plainly.
			messageId = chat::post_agent_message(input.matterId, input.chatId, input.agentId, input.agentName, words,
				input.isLocalOnly);
		}
		linkTurn(input.matterId, messageId, review.id);
	} catch (const std::exception&) {
	}

	// A Stop on the colleague, or Singularity closing, abandons a call.
	const std::shared_ptr<platform::HttpCancelToken> token = std::make_shared<platform::HttpCancelToken>();
	const platform::lifecycle::CancelOnClosing closingStops(token);

	stage("running");
	ReviewExecution execution;
	execution.runId = input.runId;
	execution.assignmentId = input.assignmentId;
	execution.operationId = taskId;
	execution.providerId = model.provider_id;
	execution.modelId = model.model_id;
	execution.attempt = attempt;
	execution.startedAt = review.versions.back().execution.startedAt;
	execution.preparation = preparation.dump();
	execution.preparationMilliseconds = preparationMilliseconds;
	// What the calls cost - a failed one too: an answer the model wrote but
	// stopped short of finishing is paid for - kept in the version and counted
	// against the budget, so nothing hides what it spent. A second call of the
	// same version adds to the first.
	const auto addSpent = [&](const providers::UsageMetadata& usage, const std::optional<std::string>& finishReason,
		long long latency, std::size_t promptSize) {
		const long long in = usage.input_tokens.value_or(0);
		const long long out = usage.output_tokens.value_or(0);
		execution.inputTokens += in;
		execution.outputTokens += out;
		execution.latencyMilliseconds += latency;
		if (usage.served_by.has_value() == true && usage.served_by->empty() == false) execution.servedBy = *usage.served_by;
		execution.finishReason = finishReason.value_or(std::string());
		std::optional<long long> cost;
		bool isEstimated = false;
		if (usage.cost_micros.has_value() == true) {
			cost = usage.cost_micros;
			execution.costCurrency = usage.cost_currency.value_or("USD");
		} else if (isLocal == false && (in > 0 || out > 0)) {
			// No price came back: the bound is what is counted against the
			// budget, and it says it is an estimate.
			cost = reviewCostBoundMicros(promptSize, static_cast<std::uint64_t>(std::max<long long>(out, 0)),
				capability.input_price_per_token, capability.output_price_per_token, loadReviewBudget());
			execution.costCurrency = "USD";
			isEstimated = true;
		}
		if (cost.has_value() == false) return;
		execution.costSource = isEstimated == true || execution.costSource == "estimated" ? "estimated" : "provider_reported";
		execution.costMicros = execution.costMicros.value_or(0) + *cost;
		if (isLocal == false) recordReviewSpend(*cost, isEstimated);
	};
	routing::InferenceResult result;
	int unansweredAgain = 0;
	bool isAskedAgain = false;
	// One call of the model as the review asks it, protected as every call of
	// the colleague is. Gives the run's ending where the call could not be made;
	// a call the provider could not take for a moment is asked again, at most
	// twice, and said (D-119).
	const std::function<std::optional<ReviewRunOutput>(const std::string&, bool)> callOnce
		= [&](const std::string& prompt, bool isAgain) -> std::optional<ReviewRunOutput> {
		std::optional<std::uint64_t> callAllowance = allowance;
		const ReviewBudget again = isAgain == true && isLocal == false ? loadReviewBudget() : ReviewBudget();
		if (isAgain == true && isLocal == false && again.limitUsd.has_value() == true) {
			const std::uint64_t affordable = callAllowance.has_value() == false ? 0 : affordableOutputTokens(prompt.size(),
				*callAllowance, capability.input_price_per_token, capability.output_price_per_token, again,
				kLeastReviewOutputTokens);
			if (affordable == 0) {
				return fail("What is left of this computer's budget for reviews could not cover a second attempt, so nothing "
					"more was sent.");
			}
			callAllowance = affordable;
		}
		routing::InferenceRequest request;
		request.call_purpose = isAgain == true ? routing::observability::CallPurpose::Correction
			: routing::observability::CallPurpose::SubstantiveGeneration;
		request.model_target = model;
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			request.workspace_id = platform::load_app_settings().local_node_setup.active_workspace_id;
		}
		request.maximum_output_tokens = callAllowance;
		if (isLocal == false) request.project_entity_hints = context_actions::memory_protection_hints(input.matterId);
		request.prompt = prompt;
		request.research_mode = research::ResearchMode::Off;
		request.research_final_output_contract = providers::GenerationOutputContract{"document_review",
			intelligence::reviewAnswerSchema()};
		// The assignment's one table where it gives one (ReviewRunInput::symbols).
		request.operation_symbols = input.symbols != nullptr ? input.symbols
			: std::make_shared<protected_api::papinian::OperationProtectedSymbolTable>();
		if (request.operation_symbols->operation_id.empty() == true) request.operation_symbols->operation_id = operationId;
		request.reasoning_effort = input.revisionEffort;
		std::atomic<bool> isCallDone(false);
		std::thread watcher;
		if (input.cancelled != nullptr) {
			watcher = std::thread([&]() {
				while (isCallDone.load() == false) {
					if (input.cancelled->load() == true) {
						token->cancel();
						return;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
				}
			});
		}
		const auto endWatch = [&]() {
			isCallDone.store(true);
			if (watcher.joinable() == true) watcher.join();
		};
		const auto started = std::chrono::steady_clock::now();
		++output.calls;
		try {
			routing::require_execution_ready(request);
			const platform::HttpCancelScope scope(token);
			result = routing::execute(request);
		} catch (const providers::TaskError& error) {
			endWatch();
			const long long latency = millisecondsSince(started);
			const auto* unusable = dynamic_cast<const providers::UnusableGeneration*>(&error);
			recordCall(input.matterId, taskId, model, nullptr, error.code(), latency,
				unusable != nullptr ? &unusable->usage : nullptr,
				unusable != nullptr ? unusable->finish_reason : std::optional<std::string>(), "document-review", 1,
				decisionOf(model, request, error.code(), error.what()));
			if (unusable != nullptr) {
				addSpent(unusable->usage, unusable->finish_reason, latency, prompt.size());
				try {
					review = changeReview(input.matterId, review.id, [&](DocumentReview& changed) {
						changed.versions.back().execution = execution;
					});
				} catch (const std::exception&) {
				}
			}
			if (platform::lifecycle::is_closing() == true) {
				output.status = "interrupted";
				output.reason = "Singularity closed during the review; it begins again when Singularity opens.";
				return finish("queued", output.reason, std::string());
			}
			if (token->is_cancelled() == true || (input.cancelled != nullptr && input.cancelled->load() == true)) {
				output.status = "cancelled";
				output.reason = "The review was stopped before the model answered.";
				return finish("failed", output.reason, "You stopped the review before I finished. Nothing in the Matter was changed.");
			}
			// A moment's trouble at the provider is asked again, twice at most.
			ColleagueCallResult failed;
			failed.errorCode = error.code();
			if (isWorthAskingAgain(failed) == true && unansweredAgain < 2) {
				++unansweredAgain;
				output.recoveries.push_back("Call " + std::to_string(output.calls) + " brought no answer ("
					+ reasonFor(error.code(), model.model_id, error.what()) + "), so it was asked again.");
				isAskedAgain = true;
				return std::nullopt;
			}
			return fail(reasonFor(error.code(), model.model_id, error.what()));
		} catch (const platform::CredentialUnavailable&) {
			// The Mac is locked: nothing left, and the work waits for it to be
			// unlocked (D-169) - the colleague's loop holds and takes it up again,
			// never a failed revision (a lease part failed so, 2026-10-31).
			endWatch();
			throw;
		} catch (const std::exception& error) {
			endWatch();
			// Its record says the mode the request ran under, as every other call's
			// does (D-172): this one once read "off" for a protected lease revision.
			recordCall(input.matterId, taskId, model, nullptr, providers::TaskErrorCode::Unknown, millisecondsSince(started),
				nullptr, std::nullopt, "document-review", 1, decisionOf(model, request, providers::TaskErrorCode::Unknown,
					std::string()));
			if (platform::lifecycle::is_closing() == true) {
				output.status = "interrupted";
				output.reason = "Singularity closed during the review; it begins again when Singularity opens.";
				return finish("queued", output.reason, std::string());
			}
			return fail(std::string("The model could not be asked: ") + error.what());
		}
		endWatch();
		const long long latency = millisecondsSince(started);
		recordCall(input.matterId, taskId, model, &result, std::nullopt, latency);
		execution.destination = isLocal == true ? "local" : result.protected_api_applied == true ? "protected" : "unprotected";
		execution.isProtectionApplied = execution.isProtectionApplied || result.protected_api_applied;
		addSpent(result.usage, result.finish_reason, latency, prompt.size());
		preparation["protection"] = Json{{"mode", result.privacy_metadata.mode},
			{"protectedCount", result.privacy_metadata.protected_count},
			{"categories", result.privacy_metadata.category_counts},
			{"payloadValidated", result.privacy_metadata.final_payload_validated},
			{"payloadSha256", result.privacy_metadata.final_payload_sha256}};
		execution.preparation = preparation.dump();
		if (execution.costMicros.has_value() == true) {
			output.cost = static_cast<double>(*execution.costMicros + askedMicros) / 1000000.0;
			output.currency = execution.costCurrency;
		}
		return std::nullopt;
	};
	const auto callModel = [&](const std::string& prompt, bool isAgain) -> std::optional<ReviewRunOutput> {
		bool isRepeat = isAgain;
		while (true) {
			isAskedAgain = false;
			std::optional<ReviewRunOutput> ended = callOnce(prompt, isRepeat);
			if (ended.has_value() == true || isAskedAgain == false) return ended;
			pauseBeforeAskingAgain(unansweredAgain, input.cancelled);
			isRepeat = true;
		}
	};
	if (std::optional<ReviewRunOutput> ended = callModel(prepared.prompt, false)) return *ended;

	/* - - - - - - */
	// 5. The answer checked, the draft written and read back, the work kept.

	stage("producing");
	review = changeReview(input.matterId, review.id, [&](DocumentReview& changed) {
		ReviewVersion& version = changed.versions.back();
		version.state = "producing";
		version.execution = execution;
		version.execution.stages = stages.dump();
	});
	intelligence::ValidatedReview validated = intelligence::validateReviewAnswer(result.text, document, task.positions,
		instruction);
	// A well-formed answer is not yet a review: one that assessed none of the
	// Playbook's positions is never handed in as finding nothing (the owner,
	// 2026-10-24: never call a result correct because the model returned valid
	// JSON). One that assessed some says which it did not.
	const auto assessedIn = [](const intelligence::ValidatedReview& answer) {
		std::size_t count = 0;
		for (const intelligence::PositionAssessment& assessment : answer.assessments) {
			if (assessment.assessment != "not_assessed") ++count;
		}
		return count;
	};
	// A revision as the lawyer asked that proposes changes has answered its one
	// position, whether or not the model labelled it; so has a part of an
	// assignment held to the clauses the lawyer named, against a Playbook, that
	// proposes changes or comments for them (a live run refused Section 5.2's
	// restored sixty days for want of assessments, 25 Oct 2026).
	const bool isNarrowed = isRequestOnly == true || (input.assignment.empty() == false && task.onlyPlaces.empty() == false);
	// Its work: a change, a comment, or a decision it puts to the lawyer before
	// changing anything - what a structure check adds on its own is not it.
	const auto hasWork = [](const intelligence::ValidatedReview& answer) {
		return answer.changes.empty() == false || answer.comments.empty() == false
			|| std::any_of(answer.unresolved.begin(), answer.unresolved.end(), [](const intelligence::UnresolvedPoint& point) {
				return point.kind == "needs_lawyer" && point.foundBy != "structure";
			});
	};
	// An answer with nothing in it - no summary, assessment, change, comment or
	// point of its own - is asked for once more before it is refused (D-118):
	// live runs (25 Oct 2026) had four such answers in some twenty-five, each
	// well-formed and empty after a thousand tokens and more.
	const auto isEmpty = [&](const intelligence::ValidatedReview& answer) {
		return answer.summary.find_first_not_of(" \t\r\n") == std::string::npos && assessedIn(answer) == 0
			&& answer.changes.empty() == true && answer.comments.empty() == true
			&& std::none_of(answer.unresolved.begin(), answer.unresolved.end(),
				[](const intelligence::UnresolvedPoint& point) { return point.foundBy != "structure"; });
	};
	// Why an answer cannot be used, in the words the next request says it.
	const auto whyUnusable = [&](const intelligence::ValidatedReview& answer) -> std::string {
		if (answer.isWellFormed == false) {
			return result.finish_reason.value_or(std::string()) == "length"
				? std::string("it was cut off at its length limit before it was complete")
				: std::string("it could not be read as a review of the shape asked for");
		}
		if (isEmpty(answer) == true) return "it was empty: no summary, no assessment, no change, no comment and no point for the lawyer";
		if (task.positions.empty() == false && assessedIn(answer) == 0 && (isNarrowed == false || hasWork(answer) == false))
			return "it assessed none of the Playbook's positions";
		return std::string();
	};
	// An answer that cannot be used is asked for again, twice at most, told why
	// (D-118, D-119): one live run's review came back empty twice running.
	for (int again = 1; again <= 2; ++again) {
		const std::string why = whyUnusable(validated);
		if (why.empty() == true) break;
		output.recoveries.push_back("Answer " + std::to_string(output.calls) + " could not be used (" + why
			+ "), so it was asked again.");
		const std::string correction = "\n<correction>\nYour answer could not be used: " + why + ". "
			+ (input.givenByName.empty() == true ? std::string("The lawyer") : input.givenByName) + " asked: \""
			+ intelligence::neutralizedMaterial(instruction) + "\". Answer again with the review this asks for, in the shape "
			"asked for: the changes it needs, or why none is needed.\n</correction>\n";
		stage("again");
		if (std::optional<ReviewRunOutput> ended = callModel(prepared.prompt + correction, true)) return *ended;
		const intelligence::ValidatedReview next = intelligence::validateReviewAnswer(result.text, document, task.positions,
			instruction);
		if (next.isWellFormed == true || validated.isWellFormed == false) validated = next;
	}
	if (validated.isWellFormed == false) {
		if (result.finish_reason.value_or(std::string()) == "length") {
			return fail("The model's answer was cut off at its length limit before it was complete, so none of it was kept. "
				"Try again; if it happens again, the agreement may be too long for one answer from this model, and "
				+ modelChooser(false) + " can choose another model for me on my page.");
		}
		return fail("The model's answer could not be read as a review, so none of it was kept. Try again; if it happens "
			"again, " + modelChooser(false) + " can choose another model for me on my page.");
	}
	if (task.positions.empty() == false && assessedIn(validated) == 0 && (isNarrowed == false || hasWork(validated) == false)) {
		// What the model said is kept with the version, so it can be read.
		try {
			review = changeReview(input.matterId, review.id, [&](DocumentReview& changed) {
				changed.versions.back().result = intelligence::validatedReviewToJson(validated).dump();
			});
		} catch (const std::exception&) {
		}
		return fail("The model's answer assessed none of the Playbook's positions, so it was not taken as a review. Try "
			"again; if it happens again, " + modelChooser(false) + " can choose another model for me on my page.");
	}

	const bool isDocx = intake.identity.format == "docx";
	const std::filesystem::path relative = std::filesystem::u8path(source->relative_path);
	std::filesystem::path target;
	if (isDocx == true) {
		const std::string stem = relative.stem().u8string();
		// A later review of the same document says which it is, so its drafts
		// never read as the first review's: "... - Unicorn review 2, v1.docx".
		int reviewNumber = 1;
		for (const DocumentReview& earlier : matter.document_reviews) {
			if (earlier.id != review.id && earlier.sourceDocumentId == review.sourceDocumentId && earlier.createdAt < review.createdAt) {
				++reviewNumber;
			}
		}
		const std::string kind = reviewNumber == 1 ? std::string(" revision v") : " review " + std::to_string(reviewNumber) + ", v";
		for (int counter = 1; counter < 100; ++counter) {
			const std::string name = stem + " - " + input.agentName + kind + std::to_string(versionNumber)
				+ (counter > 1 ? " (" + std::to_string(counter) + ")" : std::string()) + ".docx";
			target = sourcePath.parent_path() / std::filesystem::u8path(name);
			std::error_code exists;
			if (std::filesystem::exists(target, exists) == false) break;
		}
	}
	// Whose writings of the parties decide (D-172), and the corrections a
	// colleague's own draft needs, worked out once.
	const intelligence::PartyAuthority partyAuthority = partyAuthorityFor(matter, *source, input.facts.get());
	const std::map<std::string, std::string> partyCorrections = intelligence::partyCorrections(document, partyAuthority);
	// The draft of an answer, written and read back as Word holds it: each
	// written change's clause as the draft now reads, against what it was meant
	// to read (D-102).
	intelligence::RevisionOutcome revision;
	std::optional<intelligence::LegalDocument> draftDocument;
	const auto writeDraft = [&](intelligence::ValidatedReview& answer) {
		revision = intelligence::RevisionOutcome();
		draftDocument.reset();
		const bool isAnyProposed = std::any_of(answer.changes.begin(), answer.changes.end(),
			[](const intelligence::ProposedChange& change) { return change.isWritable == true; });
		if (partyCorrections.empty() == false && isAnyProposed == true) correctPartyNames(answer, document, partyCorrections);
		// Each figure a change works out from the documents' values - a ratio, a
		// deadline - is worked out here, from the real values, before it is
		// written (Context/ComputedStatements): a revision wrote the statement
		// itself into the draft, and the colleague then "corrected" it to a ratio
		// it guessed, 5.0% for 2.53% (2026-10-29). One that cannot be worked out
		// is not written, and says why.
		const auto worked = [](std::string& text, std::string& why) {
			if (context::computed_statements::has_statements(text) == false) return true;
			const context::computed_statements::CheckedText computed = context::computed_statements::compute_checked(text);
			if (computed.failures.empty() == false) {
				why = "A figure in it could not be worked out by Singularity (" + computed.failures.front().statement + ": "
					+ computed.failures.front().reason + "), so it was not written.";
				return false;
			}
			text = computed.text;
			return true;
		};
		for (intelligence::ProposedChange& change : answer.changes) {
			std::string why;
			bool isWorked = worked(change.proposedText, why);
			for (intelligence::TextEdit& edit : change.edits) isWorked = worked(edit.proposedText, why) && isWorked;
			for (intelligence::TableEdit& edit : change.tableEdits) {
				isWorked = worked(edit.proposedText, why) && isWorked;
				for (std::string& cell : edit.cells) isWorked = worked(cell, why) && isWorked;
			}
			(void)worked(change.explanation, why);
			if (isWorked == false && change.isWritable == true) {
				change.isWritable = false;
				change.checkNote = why;
			}
		}
		for (intelligence::MarginComment& comment : answer.comments) {
			std::string why;
			(void)worked(comment.text, why);
		}
		const bool isAnyWritable = std::any_of(answer.changes.begin(), answer.changes.end(),
			[](const intelligence::ProposedChange& change) { return change.isWritable == true; });
		if (isDocx == false || (isAnyWritable == false && answer.comments.empty() == true)) return;
		revision = intelligence::writeRevisedDocument(sourcePath, target, answer.changes, answer.comments, input.agentName);
		if (revision.isWritten == false) return;
		const intelligence::StructureReading read = intelligence::readDocxStructure(intelligence::takeInSource(revision.path));
		if (read.isRead == false) return;
		draftDocument = read.document;
		for (intelligence::ProposedChange& change : answer.changes) {
			if (change.writeState != "written" || change.action == "insert_after") continue;
			const intelligence::LegalBlock* block = intelligence::findBlock(*draftDocument, change.blockId);
			const intelligence::LegalBlock* before = intelligence::findBlock(document, change.blockId);
			if (block == nullptr) continue;
			change.readBack = block->text;
			// The colleague answers for the whole of what it wrote: an edit its
			// explanation leaves unsaid - the other side's words it took out
			// among them - is said from the edit itself.
			if (before != nullptr) {
				const std::string unsaid = intelligence::unsaidEdits(change.explanation, intelligence::wordEdits(before->text, block->text));
				if (unsaid.empty() == false && change.explanation.find(unsaid) == std::string::npos) {
					change.explanation += (change.explanation.empty() == true ? std::string() : std::string(" ")) + unsaid;
				}
			}
			// What the clause was meant to read: the rewrite whole; a struck clause
			// as omitted; a replacement in the clause as it stood.
			std::string meant;
			if (change.action == "rewrite" || change.action == "table" || change.proposedText == intelligence::kOmittedClause) {
				meant = change.proposedText;
			} else if (change.action == "replace" && before != nullptr) {
				meant = before->text;
				const std::size_t at = meant.find(change.currentText);
				if (at != std::string::npos) meant.replace(at, change.currentText.size(), change.proposedText);
				else meant.clear();
			}
			if (meant.empty() == true) continue;
			change.writeNote = asRead(meant) == asRead(block->text) ? std::string()
				: "Written as a tracked change, but Word shows the clause as “" + block->text + "” where it was meant to "
					"read “" + meant + "”.";
		}
	};
	const auto removeDraft = [&]() {
		std::error_code removed;
		if (revision.isWritten == true) std::filesystem::remove(revision.path, removed);
		revision = intelligence::RevisionOutcome();
		draftDocument.reset();
	};
	// Whether a draft reads, clause for clause, as the last version's did.
	const auto readsAsBefore = [&]() {
		if (previousDraft.has_value() == false || draftDocument.has_value() == false) return false;
		if (previousDraft->blocks.size() != draftDocument->blocks.size()) return false;
		for (std::size_t index = 0; index < previousDraft->blocks.size(); ++index) {
			if (previousDraft->blocks[index].id != draftDocument->blocks[index].id) return false;
			if (asRead(previousDraft->blocks[index].text) != asRead(draftDocument->blocks[index].text)) return false;
		}
		return true;
	};
	// The clauses the lawyer's words name, by the numbers the agreement gives
	// them: "2.1", "Section 8.1".
	std::vector<std::string> namedBlocks;
	{
		static const std::regex numbered("(?:^|[^0-9.])([0-9]{1,2}(?:\\.[0-9]{1,2}){1,3})(?![0-9])");
		for (std::sregex_iterator found(instruction.begin(), instruction.end(), numbered), end; found != end; ++found) {
			const std::string number = (*found)[1].str();
			for (const intelligence::LegalBlock& block : document.blocks) {
				std::string own = block.number;
				while (own.empty() == false && own.back() == '.') own.pop_back();
				if (own == number && std::find(namedBlocks.begin(), namedBlocks.end(), block.id) == namedBlocks.end()) {
					namedBlocks.push_back(block.id);
				}
			}
		}
	}
	writeDraft(validated);
	// A revision whose draft would read exactly as the last version's is not a
	// result: the colleague takes it up again once, told so, with how the
	// clauses the lawyer named read now; a draft that still changes nothing is
	// never handed in as a version to accept (D-102).
	const bool canBeMeasured = isDocx == true && previous.has_value() == true && previousDraft.has_value() == true;
	bool isUnchanged = canBeMeasured == true && (revision.isWritten == false || readsAsBefore() == true);
	Json accountability = Json::object();
	if (isUnchanged == true) {
		removeDraft();
		accountability["firstAttempt"] = "read as version " + std::to_string(previous->number);
		std::string correction = "\n<correction>\nYour answer leaves the agreement exactly as version "
			+ std::to_string(previous->number) + "'s draft reads in Word, clause for clause, so it is no revision. "
			+ (input.givenByName.empty() == true ? std::string("The lawyer") : input.givenByName) + " asked: \""
			+ intelligence::neutralizedMaterial(instruction) + "\".\n";
		for (const std::string& id : namedBlocks) {
			const intelligence::LegalBlock* block = intelligence::findBlock(*previousDraft, id);
			if (block != nullptr) correction += "In version " + std::to_string(previous->number) + "'s draft, ["
				+ id + "] reads: \"" + intelligence::neutralizedMaterial(block->text) + "\"\n";
		}
		correction += "Make the change asked for, as the clause should read. Where it cannot be made, propose no change to "
			"it and say exactly why in the summary.\n</correction>\n";
		stage("again");
		if (std::optional<ReviewRunOutput> ended = callModel(prepared.prompt + correction, true)) return *ended;
		intelligence::ValidatedReview again = intelligence::validateReviewAnswer(result.text, document, task.positions,
			instruction);
		if (again.isWellFormed == true && (task.positions.empty() == true || assessedIn(again) > 0
				|| (isNarrowed == true && hasWork(again) == true))) {
			validated = again;
			writeDraft(validated);
			isUnchanged = revision.isWritten == false || readsAsBefore() == true;
			if (isUnchanged == true) removeDraft();
		}
		accountability["secondAttempt"] = isUnchanged == true
			? "read as version " + std::to_string(previous->number) : std::string("changed the draft");
	}
	preparation["accountability"] = accountability;
	execution.preparation = preparation.dump();

	// The draft is the Matter's once it is kept.
	std::string resultDocumentId;
	std::string resultRelativePath;
	if (revision.isWritten == true) {
		const std::filesystem::path relativeTarget = relative.parent_path() / target.filename();
		resultRelativePath = relativeTarget.generic_u8string();
		try {
			(void)matter::detect_affected_paths(input.matterId, {target.u8string()});
			const matters::Matter refreshed = matter::process_project_documents(input.matterId);
			for (const matters::MatterDocument& written : refreshed.documents) {
				if (written.relative_path == resultRelativePath) resultDocumentId = written.id;
			}
		} catch (const std::exception&) {
			// The draft is in the Matter's folder; the folder watch brings it in.
		}
	}
	stage("written");

	// The draft read back as Word shows it with every change accepted, and
	// checked against the source apart from the model: nothing lost, nothing
	// changed that no change accounts for, numbering, definitions and
	// cross-references.
	intelligence::IntegrityReport integrity;
	if (revision.isWritten == true) {
		if (draftDocument.has_value() == true) {
			integrity = intelligence::checkDraftIntegrity(document, *draftDocument, validated.changes, partyAuthority);
		} else {
			integrity.problem = "The revised draft could not be read back, so it was not checked.";
		}
	}

	// Where each value a written change gives comes from (D-119): a document of
	// the Matter, the Playbook, the lawyer's words, or the clause as it read
	// before the other side's changes. One that none gives is the lawyer's to
	// confirm, and said so.
	Json origins = Json::array();
	if (revision.isWritten == true && isUnchanged == false) {
		// The agreement's own clauses, and the documents it rests on: in an
		// assignment, every document of the Matter as the assignment reads them.
		std::vector<intelligence::Statement> sources = factsOfAgreement(document, source->id, source->name).statements;
		try {
			if (input.facts != nullptr) {
				const MatterFacts read = matterFacts(matter, source->id, input.assignmentDraftIds, *input.facts);
				sources.insert(sources.end(), read.sources.begin(), read.sources.end());
			} else {
				FactsReading reading;
				const std::filesystem::path folder = std::filesystem::u8path(matter.directory_path.value_or(std::string()));
				for (const std::string& id : input.supportingDocumentIds) {
					for (const matters::MatterDocument& supporting : matter.documents) {
						if (supporting.id != id || folder.empty() == true
								|| documents::registry::is_relative_path_safe(folder, supporting.relative_path) == false)
							continue;
						const DocumentFacts& read = reading.of((folder / std::filesystem::u8path(supporting.relative_path))
							.lexically_normal(), supporting.id, supporting.name);
						sources.insert(sources.end(), read.statements.begin(), read.statements.end());
					}
				}
			}
		} catch (const std::exception&) {
		}
		std::string playbookWords;
		for (const intelligence::ReviewPosition& position : task.positions) {
			playbookWords += position.standard + "\n" + position.standardClause + "\n" + position.redLine + "\n";
			for (const std::string& fallback : position.fallbacks) playbookWords += fallback + "\n";
		}
		for (const intelligence::ProposedChange& change : validated.changes) {
			if (change.writeState != "written") continue;
			const intelligence::LegalBlock* before = intelligence::findBlock(document, change.blockId);
			const std::string was = before != nullptr ? before->text : change.currentText;
			const std::string now = change.readBack.empty() == false ? change.readBack : change.proposedText;
			std::string theirs;
			for (const intelligence::LegalBlock& note : document.blocks) {
				if (note.section == "comment" && note.annotates == change.blockId) theirs += note.text + "\n";
			}
			const std::vector<intelligence::ValueOrigin> found = intelligence::originsOfNewValues(was, now,
				before != nullptr ? before->heading : std::string(), sources, {
					intelligence::GivenText{"your instruction", instruction},
					intelligence::GivenText{"the assignment", input.assignment},
					intelligence::GivenText{"the Playbook", playbookWords},
					intelligence::GivenText{"the clause before the pending changes", before != nullptr ? before->originalText
						: std::string()},
					intelligence::GivenText{"the comments in its margin", theirs}});
			const std::string place = change.clauseNumber.empty() == false ? "Section " + change.clauseNumber
				: placeWords(document, change.blockId);
			for (const intelligence::ValueOrigin& origin : found) {
				if (origin.state == "unsourced") {
					output.unsourced.push_back(place + " now gives " + origin.value.text + ", which no document of the Matter, the "
						"Playbook or your words give.");
				}
			}
			origins.push_back(Json{{"change", change.id}, {"place", place}, {"origins", originsToJson(found)}});
		}
	}

	// How the clauses read across the drafts, for the colleague's message: those
	// the lawyer named, those that read otherwise than in the last draft, and
	// how many read as they did (D-102).
	DraftComparison comparison;
	if (canBeMeasured == true) {
		comparison.previousVersion = previous->number;
		comparison.isUnchanged = isUnchanged;
		comparison.why = validated.summary;
		const auto placeOfBlock = [&](const std::string& id) { return placeWords(document, id); };
		for (const std::string& id : namedBlocks) {
			ClauseReading reading;
			reading.place = placeOfBlock(id);
			const intelligence::LegalBlock* before = intelligence::findBlock(*previousDraft, id);
			const intelligence::LegalBlock* now = draftDocument.has_value() == true ? intelligence::findBlock(*draftDocument, id) : nullptr;
			reading.before = before != nullptr ? before->text : std::string();
			reading.now = now != nullptr ? now->text : reading.before;
			comparison.named.push_back(reading);
		}
		if (draftDocument.has_value() == true) {
			for (const intelligence::LegalBlock& block : draftDocument->blocks) {
				if (std::find(namedBlocks.begin(), namedBlocks.end(), block.id) != namedBlocks.end()) continue;
				const intelligence::LegalBlock* before = intelligence::findBlock(*previousDraft, block.id);
				if (before != nullptr && asRead(before->text) == asRead(block.text)) continue;
				ClauseReading reading;
				reading.place = placeOfBlock(block.id);
				reading.before = before != nullptr ? before->text : std::string();
				reading.now = block.text;
				comparison.otherChanged.push_back(reading);
			}
		}
	}

	// The work, kept as a Work Product of the Matter: one per version.
	std::string workProductId;
	try {
		work_product::CreateWorkProductCommand command;
		command.project_id = input.matterId;
		command.type_id = matters::kWorkProductTypeDraft;
		command.title = input.agentName + "'s review of " + source->name + ", version " + std::to_string(versionNumber);
		command.content = workProductContent(review, versionNumber, validated);
		context::ResolvedSourceReference sourceReference;
		sourceReference.document_id = source->id;
		sourceReference.document_name = source->name;
		command.source_refs.push_back(sourceReference);
		for (const intelligence::ProposedChange& change : validated.changes) {
			matters::Change row;
			row.id = matters::generate_change_id();
			row.matter_id = input.matterId;
			row.category = change.positionId;
			row.description = (change.clauseNumber.empty() == false ? change.clauseNumber + ": " : std::string())
				+ change.explanation;
			context::ResolvedSourceReference reference;
			reference.document_id = source->id;
			reference.document_name = source->name;
			row.source_refs.push_back(reference);
			command.changes.push_back(row);
		}
		command.origin.kind = matters::WorkProductOriginKind::Agent;
		command.origin.reference_id = input.runId;
		workProductId = work_product::create(command).id;
	} catch (const std::exception&) {
		// The review and its draft are kept either way.
	}

	/* - - - - - - */
	// 6. Handed in: the colleague's message, written once the work is done from
	// what happened, and checked against it before it is posted (D-101).

	stage("writing_message");
	const HandInFacts facts = gatherFacts(input, review, versionNumber, document, validated, revision, isDocx, integrity,
		canBeMeasured == true ? &comparison : nullptr);
	HandIn handIn;
	if (input.isHandedInByAssignment == true) {
		// A part of an assignment hands in through the assignment's card and its
		// closing message (D-120): what happened is said from the record and kept
		// with the version, for its page; no message is posted, no model asked.
		handIn.text = plainHandIn(facts);
		handIn.record = Json{{"writtenBy", "plain"}, {"byAssignment", true}, {"text", handIn.text}};
	} else {
		HandInCall call;
		call.model = model;
		call.capability = capability;
		call.allowance = allowance;
		call.isLocal = isLocal;
		call.matterId = input.matterId;
		call.chatId = input.chatId;
		call.agentId = input.agentId;
		call.agentName = input.agentName;
		call.agentBrief = input.agentBrief;
		call.answering = input.requestMessageId;
		call.operationId = operationId;
		call.taskId = taskId;
		call.token = token;
		call.cancelled = input.cancelled;
		handIn = composeHandIn(call, facts);
	}
	const std::string said = handIn.text;
	Json handInRecord = handIn.record;
	// The message's own calls are the version's too.
	for (const Json& attempt : handInRecord.value("attempts", Json::array())) {
		if (attempt.is_object() == false || attempt.contains("failed") == true) continue;
		++output.calls;
		output.inputTokens += attempt.value("inputTokens", 0LL);
		output.outputTokens += attempt.value("outputTokens", 0LL);
	}
	if (handIn.costMicros.has_value() == true) {
		handInRecord["costMicros"] = *handIn.costMicros;
		handInRecord["costSource"] = handIn.costSource;
		const long long total = execution.costMicros.value_or(0) + *handIn.costMicros + askedMicros;
		output.cost = static_cast<double>(total) / 1000000.0;
		output.currency = execution.costCurrency.empty() == true ? std::string("USD") : execution.costCurrency;
	}
	preparation["handIn"] = handInRecord;
	execution.preparation = preparation.dump();
	std::string answerMessageId;
	if (input.isHandedInByAssignment == false) try {
		chat::MessageExtras handedOver;
		if (resultDocumentId.empty() == false) handedOver.attachment_document_ids.push_back(resultDocumentId);
		answerMessageId = chat::post_agent_message(input.matterId, input.chatId, input.agentId, input.agentName, said,
			input.isLocalOnly, handedOver);
		std::vector<std::string> products;
		if (workProductId.empty() == false) products.push_back(workProductId);
		linkTurn(input.matterId, answerMessageId, review.id, products);
	} catch (const std::exception&) {
	}
	// A revision that changed nothing is no version to accept: the last one
	// waits for the lawyer again. One that changed the draft takes its place.
	// A draft that names a party as the source never does - a mark of the text
	// around a name, another spelling, another party's capacity - is never
	// offered for acceptance (P0, 30 Oct 2026): it stays in the Matter as the
	// record of what was written, the original untouched, the version before it
	// still the one to review, and the colleague is told why.
	std::string partyProblem;
	for (const intelligence::IntegrityCheckResult& check : integrity.checks)
		if (check.name == "parties" && check.state == "failed" && check.findings.empty() == false)
			partyProblem = check.findings.front().text;
	const bool isPartyFailure = isUnchanged == false && partyProblem.empty() == false;
	const std::string ending = isUnchanged == true ? std::string("unchanged")
		: isPartyFailure == true ? std::string("failed") : std::string("ready_for_review");
	stage(ending);
	execution.finishedAt = now();
	execution.stages = stages.dump();
	review = changeReview(input.matterId, review.id, [&](DocumentReview& changed) {
		for (ReviewVersion& earlier : changed.versions) {
			if (previous.has_value() == false || earlier.number != previous->number) continue;
			if (isUnchanged == true && earlier.state == "superseded") {
				earlier.state = "ready_for_review";
				earlier.stateMessage.clear();
			} else if (isUnchanged == false && isPartyFailure == false && earlier.state == "ready_for_review") {
				earlier.state = "superseded";
				earlier.stateMessage = "Version " + std::to_string(versionNumber) + " replaced it.";
			}
		}
		ReviewVersion& version = changed.versions.back();
		version.state = ending;
		version.stateMessage = isUnchanged == true
			? "It would read exactly as version " + std::to_string(previous->number) + ", so no new draft was made."
			: isPartyFailure == true
			? partyProblem + " The draft is kept as written but not offered for acceptance; the original is unchanged."
			: std::string();
		Json result = intelligence::validatedReviewToJson(validated);
		result["integrity"] = intelligence::integrityReportToJson(integrity);
		result["trackChangesOn"] = revision.isTrackingOn;
		result["record"] = stepRecordOf(validated, document, integrity, revision, isUnchanged, origins, output, sourcePath,
			changed.sourceSha256, source->id, source->name, instruction);
		version.result = result.dump();
		version.resultDocumentId = resultDocumentId;
		version.resultRelativePath = resultRelativePath;
		version.resultFileName = revision.isWritten == true ? revision.path.filename().u8string() : std::string();
		version.resultSha256 = revision.sha256;
		version.workProductId = workProductId;
		version.answerMessageId = answerMessageId;
		version.execution = execution;
	});
	output.status = isPartyFailure == true ? std::string("failed") : std::string("completed");
	if (isPartyFailure == true) output.reason = partyProblem + " The draft was not offered for acceptance.";
	output.excerpt = said.substr(0, said.find('\n'));
	if (workProductId.empty() == false) output.workProductId = workProductId;
	// What the version holds, read back from the saved draft, for the
	// assignment it may be a part of (D-115). A draft that names a party wrong
	// is no result to hand in.
	output.sourceName = source->name;
	output.resultDocumentId = isPartyFailure == true ? std::string() : resultDocumentId;
	output.resultFileName = revision.isWritten == true && isPartyFailure == false ? revision.path.filename().u8string()
		: std::string();
	if (revision.isWritten == true && isUnchanged == false) {
		std::map<std::string, std::string> topics;
		for (const intelligence::PositionAssessment& assessment : validated.assessments) topics[assessment.positionId] = assessment.topic;
		for (const intelligence::ProposedChange& change : validated.changes) {
			if (change.writeState != "written") continue;
			const std::string place = change.clauseNumber.empty() == false ? "Section " + change.clauseNumber
				: placeWords(document, change.blockId);
			const std::string topic = topics.count(change.positionId) > 0 && change.positionId != intelligence::kRequestPositionId
				? topics[change.positionId] : std::string();
			output.writtenChanges.push_back(labelOf({place}, topic) + ": " + whatItDoes(change));
			output.writtenPlaces.push_back(place);
		}
		for (const intelligence::MarginComment& comment : validated.comments) {
			if (comment.writeState != "written") continue;
			output.writtenComments.push_back((comment.clauseNumber.empty() == false ? "Section " + comment.clauseNumber
				: placeWords(document, comment.blockId)) + ": “" + comment.text + "”");
		}
	}
	output.openPoints = facts.calls;
	// The same calls as the lawyer takes them (D-120).
	for (const intelligence::UnresolvedPoint& point : validated.unresolved) {
		if (point.kind != "needs_lawyer") continue;
		AssignmentDecision decision;
		std::vector<std::string> places;
		for (const std::string& id : point.blockIds) places.push_back(placeWords(document, id));
		decision.place = listedWords(places);
		decision.question = point.description;
		decision.why = point.why;
		decision.options = point.options;
		decision.isRecommended = point.isRecommended;
		decision.isBlocking = point.isBlocking;
		decision.reviewId = review.id;
		decision.version = versionNumber;
		decision.documentId = resultDocumentId.empty() == false ? resultDocumentId : source->id;
		decision.documentName = source->name;
		output.decisions.push_back(std::move(decision));
	}
	output.inputTokens += execution.inputTokens;
	output.outputTokens += execution.outputTokens;
	if (revision.isWritten == false && facts.noDraft == "held_for_decision") {
		output.reason = "It waits for your decision before anything in it is changed.";
	}
	return output;

}

namespace {

// The work a colleague did in a thread, as its records keep it (D-120): each
// assignment's documents - their versions and where they stand, what each
// draft holds, what is left - its decisions with their answers, and its email.
// What the colleague's ordinary answer is given before a question about it.
std::string threadWorkRecord(const std::string& matterId, const std::string& chatId, const std::string& agentId) {
	const matters::Matter matter = loadMatter(matterId);
	std::vector<const AssignmentRecord*> records;
	for (const AssignmentRecord& record : matter.assignment_records) {
		if (record.chatId == chatId && record.agentId == agentId) records.push_back(&record);
	}
	if (records.size() > 3) records.erase(records.begin(), records.end() - 3);
	std::string text;
	for (const AssignmentRecord* record : records) {
		text += "Asked " + record->startedAt.substr(0, 10) + " by " + record->givenByName + ": \""
			+ intelligence::neutralizedMaterial(record->instruction) + "\"\n";
		for (const AssignmentStepRecord& step : record->steps) {
			const std::string name = step.label.empty() == false ? step.label : std::filesystem::u8path(step.sourceName).stem().u8string();
			text += "- " + name + " (" + step.sourceName + "): ";
			const DocumentReview* review = nullptr;
			for (const DocumentReview& entry : matter.document_reviews) {
				if (entry.id == step.reviewId) review = &entry;
			}
			if (review == nullptr || review->versions.empty() == true) {
				text += step.statusWords.empty() == false ? step.statusWords + "\n" : step.status + "\n";
				continue;
			}
			const ReviewVersion& latest = review->versions.back();
			text += "version " + std::to_string(latest.number) + ", " + (latest.state == "ready_for_review"
				? std::string("waiting for acceptance") : latest.state) + (latest.resultFileName.empty() == true ? std::string()
				: ", the draft \"" + latest.resultFileName + "\"") + ". " + step.statusWords + "\n";
			const Json result = Json::parse(latest.result, nullptr, false);
			std::size_t shown = 0;
			for (const Json& change : result.is_object() == true ? result.value("changes", Json::array()) : Json::array()) {
				if (change.is_object() == false || ++shown > 8) continue;
				const std::string place = change.value("clauseNumber", std::string());
				text += "  " + (change.value("writeState", std::string()) == "written" ? std::string("Changed") : std::string("Not written"))
					+ (place.empty() == true ? std::string() : " at " + place) + ": \""
					+ intelligence::neutralizedMaterial(change.value("currentText", std::string())) + "\" -> \""
					+ intelligence::neutralizedMaterial(change.value("proposedText", std::string())) + "\""
					+ (change.value("explanation", std::string()).empty() == true ? std::string()
						: ". Why: " + intelligence::neutralizedMaterial(change.value("explanation", std::string()))) + "\n";
			}
			std::size_t left = 0;
			for (const std::string& point : step.remaining) {
				if (++left > 6) break;
				text += "  Left: " + intelligence::neutralizedMaterial(point) + "\n";
			}
		}
		for (const AssignmentDecision& decision : record->decisions) {
			text += "- Decision" + (decision.place.empty() == true ? std::string() : " on " + decision.place) + " of "
				+ decision.documentName + ": " + intelligence::neutralizedMaterial(decision.question)
				+ (decision.state == "answered" ? " Answered by " + decision.answeredByName + ": \""
					+ intelligence::neutralizedMaterial(decision.answer) + "\"" : std::string(" Still open.")) + "\n";
		}
		if (record->email.isWanted == true) {
			text += "- The email: " + (record->email.state == "drafted" ? "drafted as \"" + record->email.name + "\", subject \""
				+ record->email.subject + "\", to " + (record->email.to.empty() == true ? std::string("no recipient yet")
				: record->email.to) + ", for the lawyer to send" : record->email.state + ": " + record->email.reason) + "\n";
		}
		text += "\n";
	}
	if (text.size() > 12000) text = text.substr(text.size() - 12000);
	return text;
}

} // namespace

std::string colleagueThreadRecord(const std::string& matterId, const std::string& chatId, const std::string& agentId) {
	return threadWorkRecord(matterId, chatId, agentId);
}

void installForColleagues() {

	agents::ThreadDocumentWork work;
	work.record_of = [](const std::string& matterId, const std::string& chatId, const std::string& agentId) {
		return threadWorkRecord(matterId, chatId, agentId);
	};
	// Every request a lawyer gives a colleague is the colleague's to understand
	// (D-115): its plan decides whether it is answered in words or worked on.
	work.is_requested = [](const std::string&, const std::string&, const std::string&, const std::string&) { return true; };
	// The colleague's run, handed over: its request becomes the work's input,
	// and how the work ended becomes what the run reports.
	work.run = [](const agents::Agent& agent, const agents::TaskRunInput& input, const std::string& chatId,
			const std::string& brief, agents::TaskRunOutput output) -> std::optional<agents::TaskRunOutput> {
		ReviewRunInput review;
		review.matterId = input.matter_id;
		review.chatId = chatId;
		review.agentId = agent.id;
		review.agentName = agent.name;
		review.agentBrief = brief;
		review.playbookId = agent.playbook_id;
		review.instruction = input.instruction;
		review.requestMessageId = input.reply_to_message_id;
		review.givenByActorId = input.given_by_actor_id;
		review.givenByName = input.given_by;
		review.runId = input.run_id;
		review.isLocalOnly = input.policy == agents::AgentModelPolicy::LocalOnly;
		review.cancelled = input.cancelled;
		review.isOwnChat = agent.chat_id.has_value() == true && *agent.chat_id == chatId;
		review.agentIdentity = input.identity;
		review.askerOffice = input.asker_office;
		review.askerStanding = input.asker_standing;
		review.workBegins = input.work_begins;
		review.answerGiven = input.answer_given;
		const ColleagueWorkOutcome done = runColleagueWork(review);
		if (done.status == "answer") return std::nullopt;
		// Answered in its talk, in its own words: nothing more is asked.
		if (done.status == "answered") {
			output.status = "completed";
			output.task.state = "completed";
			output.task.answer_excerpt = done.excerpt;
			if (done.cost.has_value() == true) {
				output.task.cost = done.cost;
				output.task.currency = done.currency;
			}
			return output;
		}
		output.task.operation_id = done.operationId;
		output.task.answer_excerpt = done.excerpt;
		if (done.cost.has_value() == true) {
			output.task.cost = done.cost;
			output.task.currency = done.currency;
		}
		if (done.workProductId.has_value() == true) output.work_product_id = done.workProductId;
		if (done.status == "completed") {
			output.status = "completed";
			output.task.state = "completed";
		} else if (done.status == "waiting") {
			// It asked the lawyer which document; the answer is the next request.
			output.status = "completed";
			output.task.state = "waiting_for_answer";
			output.task.message = done.reason;
		} else if (done.status == "cancelled") {
			output.status = "cancelled";
			output.task.state = "stopped";
			output.reason = done.reason;
		} else if (done.status == "interrupted") {
			output.status = "interrupted";
			output.task.state = "interrupted";
			output.reason = done.reason;
		} else {
			output.status = "failed";
			output.task.state = "failed";
			output.reason = done.reason;
			output.task.message = done.reason;
		}
		return output;
	};
	// A run the colleague begins itself, as its talk (D-129): what it says is
	// kept as Work Product, as its former answer was.
	work.run_own = [](const agents::Agent& agent, const agents::TaskRunInput& input, const std::string& chatId,
			const std::string& brief, const std::string& why, agents::TaskRunOutput output) -> std::optional<agents::TaskRunOutput> {
		ReviewRunInput review;
		review.matterId = input.matter_id;
		review.chatId = chatId;
		review.agentId = agent.id;
		review.agentName = agent.name;
		review.agentBrief = brief;
		review.playbookId = agent.playbook_id;
		review.instruction = why;
		review.givenByName = agent.responsible_name;
		review.runId = input.run_id;
		review.isLocalOnly = input.policy == agents::AgentModelPolicy::LocalOnly;
		review.cancelled = input.cancelled;
		review.isOwnChat = agent.chat_id.has_value() == true && *agent.chat_id == chatId;
		review.isOwnRun = true;
		review.agentIdentity = input.identity;
		review.workBegins = input.work_begins;
		review.answerGiven = input.answer_given;
		// A contract that has just arrived is read in this run: the files the run
		// found new or changed are brought into the Matter first (R0).
		if (input.changed_files.empty() == false) {
			try {
				std::vector<std::string> paths;
				for (const std::filesystem::path& file : input.changed_files) paths.push_back(file.u8string());
				(void)matter::detect_affected_paths(input.matter_id, paths);
				(void)matter::process_project_documents(input.matter_id);
			} catch (const std::exception&) {
			}
		}
		const ColleagueWorkOutcome done = runColleagueWork(review);
		if (done.cost.has_value() == true) {
			output.task.cost = done.cost;
			output.task.currency = done.currency;
		}
		output.task.answer_excerpt = done.excerpt;
		if (done.status == "answered" || done.status == "completed" || done.status == "waiting") {
			output.status = "completed";
			output.task.state = done.status == "waiting" ? "waiting_for_answer" : "completed";
			if (done.workProductId.has_value() == true) output.work_product_id = done.workProductId;
			return output;
		}
		if (done.status == "cancelled") {
			output.status = "cancelled";
			output.task.state = "stopped";
			output.reason = done.reason;
			return output;
		}
		if (done.status == "interrupted") {
			output.status = "interrupted";
			output.task.state = "interrupted";
			output.reason = done.reason;
			return output;
		}
		output.status = "failed";
		output.task.state = "failed";
		output.reason = done.reason;
		output.task.message = done.reason;
		return output;
	};
	agents::install_thread_document_work(std::move(work));

}

} // namespace singularity::document_review
