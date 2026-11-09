#include "WorkProduct/ReviewTable/ReviewTable.hpp"

#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"
#include "Context/ProjectContextActions/ProjectContextActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/PhraseMatch/PhraseMatch.hpp"
#include "Platform/WordAlignment/WordAlignment.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"
#include "Tool/OperationAuthority/OperationAuthority.hpp"
#include "WorkProduct/ReviewTableExport/ReviewTableExport.hpp"
#include "WorkProduct/WorkProductActions/WorkProductActions.hpp"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace singularity::review_table {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMostColumns = 24;
constexpr std::size_t kMostDocuments = 500;

const std::set<std::string> &answer_kinds() {
	static const std::set<std::string> kinds{"text", "date", "amount", "yes_no", "party", "duration", "clause"};
	return kinds;
}

std::string trimmed(const std::string &value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

// Loads the Matter under the persistence lock, applies `change` and saves it.
template <typename Change>
auto change_matter(const std::string &project_id, Change &&change) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	std::optional<matters::Matter> matter = matters::load_matter(workspace, project_id);
	if (!matter) throw std::runtime_error("This Matter is no longer in the Workspace.");
	auto result = change(*matter);
	matters::save_matter(workspace, *matter);
	return result;
}

matters::Matter load(const std::string &project_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	std::optional<matters::Matter> matter = matters::load_matter(node::require_active_workspace_path(), project_id);
	if (!matter) throw std::runtime_error("This Matter is no longer in the Workspace.");
	return *matter;
}

matters::ReviewTable &table_in(matters::Matter &matter, const std::string &table_id) {
	const auto found = std::find_if(matter.review_tables.begin(), matter.review_tables.end(),
		[&](const matters::ReviewTable &table) { return table.id == table_id; });
	if (found == matter.review_tables.end()) throw std::runtime_error("This Review Table is no longer in the Matter.");
	return *found;
}

const matters::MatterDocument *document_in(const matters::Matter &matter, const std::string &document_id) {
	for (const matters::MatterDocument &document : matter.documents)
		if (document.id == document_id) return &document;
	return nullptr;
}

std::string document_name(const matters::MatterDocument &document) {
	const std::filesystem::path path = std::filesystem::u8path(document.relative_path);
	return path.filename().empty() ? document.relative_path : path.filename().u8string();
}

std::string row_id(const std::string &table_id, const std::string &document_id) {
	return table_id + ":" + document_id;
}

const matters::ReviewRow *row_in(const matters::Matter &matter, const std::string &table_id, const std::string &document_id) {
	const std::string id = row_id(table_id, document_id);
	for (const matters::ReviewRow &row : matter.review_rows)
		if (row.id == id) return &row;
	return nullptr;
}

// Whether the document changed since the row read it only by the change
// prepared from the row (note_change), approved and written: its latest change
// is Singularity's write in that change's operation, from the very version the
// row read. A version anyone else made since - the counterparty's, a sync's, a
// later request's - is a new document, and the row is stale.
bool changed_only_by_its_change(const matters::Matter &matter, const matters::ReviewRow &row,
		const matters::MatterDocument &document) {
	if (row.change_operation_id.empty() || row.document_fingerprint == document.fingerprint) return false;
	for (auto event = matter.document_events.rbegin(); event != matter.document_events.rend(); ++event) {
		if (event->document_id != document.id || event->kind != documents::DocumentEventKind::Changed) continue;
		return event->actor == "singularity" && event->operation_id == row.change_operation_id
			&& event->previous_fingerprint == row.document_fingerprint && event->fingerprint == document.fingerprint;
	}
	return false;
}

// Whether a row answers every column the table asks now, from the document as
// it reads now.
std::string row_status(const matters::Matter &matter, const matters::ReviewTable &table, const std::string &document_id) {
	const matters::MatterDocument *document = document_in(matter, document_id);
	if (document == nullptr || !document->available) return "unavailable";
	const matters::ReviewRow *row = row_in(matter, table.id, document_id);
	if (row == nullptr) return "missing";
	if (row->state != "answered") return "failed";
	if (row->document_fingerprint != document->fingerprint && !changed_only_by_its_change(matter, *row, *document))
		return "stale";
	for (const matters::ReviewColumn &column : table.columns) {
		const bool answered = std::any_of(row->cells.begin(), row->cells.end(),
			[&](const matters::ReviewCell &cell) { return cell.column_id == column.id; });
		if (!answered) return "missing";
	}
	return "current";
}

// Runs of a table on this computer, live in this process.
struct LiveRun {
	std::atomic<bool> stop{false};
	std::shared_ptr<platform::HttpCancelToken> token = std::make_shared<platform::HttpCancelToken>();
};

std::mutex &live_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::map<std::string, std::shared_ptr<LiveRun>> &live_runs() {
	static auto *runs = new std::map<std::string, std::shared_ptr<LiveRun>>();
	return *runs;
}

std::shared_ptr<LiveRun> live_run(const std::string &project_id, const std::string &table_id) {
	const std::lock_guard<std::mutex> lock(live_mutex());
	const auto found = live_runs().find(project_id + "\n" + table_id);
	return found == live_runs().end() ? nullptr : found->second;
}

ReviewTableView view_of(const matters::Matter &matter, const matters::ReviewTable &table) {
	ReviewTableView view;
	view.table = table;
	// A run this computer began that is not running in this process was cut off.
	if (table.run_state == "running" && table.run_device_id == platform::current_device_id()
			&& live_run(matter.id, table.id) == nullptr)
		view.table.run_state = "interrupted";
	bool everyCostKnown = true;
	bool anyBilled = false;
	for (const matters::ReviewDocument &document : table.documents) {
		RowStatus status{document.document_id, row_status(matter, table, document.document_id)};
		if (status.status == "current") {
			const matters::ReviewRow *row = row_in(matter, table.id, document.document_id);
			const matters::MatterDocument *current = document_in(matter, document.document_id);
			status.change_written = row != nullptr && current != nullptr && row->document_fingerprint != current->fingerprint;
		}
		if (status.status != "current" && status.status != "unavailable") ++view.to_answer;
		view.statuses.push_back(std::move(status));
		if (const matters::ReviewRow *row = row_in(matter, table.id, document.document_id)) {
			view.rows.push_back(*row);
			view.input_tokens += row->input_tokens;
			view.output_tokens += row->output_tokens;
			if (row->input_tokens + row->output_tokens > 0 && row->destination != "local") {
				anyBilled = true;
				if (!row->cost_micros) everyCostKnown = false;
				else {
					view.cost_micros = view.cost_micros.value_or(0) + *row->cost_micros;
					view.cost_currency = row->cost_currency;
				}
			}
		}
	}
	if (!anyBilled || !everyCostKnown) view.cost_micros.reset();
	return view;
}

std::vector<matters::ReviewColumn> columns_from(const std::vector<NewColumn> &requested) {
	std::vector<matters::ReviewColumn> columns;
	for (const NewColumn &entry : requested) {
		matters::ReviewColumn column;
		column.id = matters::generate_review_column_id();
		column.question = trimmed(entry.question);
		column.heading = trimmed(entry.heading).empty() ? column.question : trimmed(entry.heading);
		column.answer_kind = answer_kinds().count(entry.answer_kind) > 0 ? entry.answer_kind : "text";
		column.playbook_id = entry.playbook_id;
		column.position_id = entry.position_id;
		column.position = trimmed(entry.position);
		column.law = trimmed(entry.law);
		if (column.question.empty()) throw std::runtime_error("Write a question for every column.");
		columns.push_back(std::move(column));
	}
	return columns;
}

std::vector<matters::ReviewDocument> documents_from(const matters::Matter &matter, const std::vector<std::string> &ids) {
	std::vector<matters::ReviewDocument> documents;
	std::set<std::string> seen;
	for (const std::string &id : ids) {
		if (!seen.insert(id).second) continue;
		const matters::MatterDocument *document = document_in(matter, id);
		if (document == nullptr) throw std::runtime_error("One of the chosen documents is no longer in the Matter.");
		documents.push_back(matters::ReviewDocument{id, document_name(*document)});
	}
	return documents;
}

void require_shape(const matters::ReviewTable &table) {
	if (table.documents.empty()) throw std::runtime_error("Choose at least one document for the table.");
	if (table.columns.empty()) throw std::runtime_error("Ask at least one question for the table.");
	if (table.columns.size() > kMostColumns)
		throw std::runtime_error("A Review Table asks at most " + std::to_string(kMostColumns) + " questions.");
	if (table.documents.size() > kMostDocuments)
		throw std::runtime_error("A Review Table reads at most " + std::to_string(kMostDocuments) + " documents.");
}

// What a model's answer says about one column, read leniently: a value that
// is not of its kind is kept as written, never invented.
struct AnsweredCell {
	std::string column_id;
	bool found = false;
	std::string answer;
	std::string value;
	std::string quote;
	std::string passage;
	std::string basis;
	std::string confidence;
	std::string reasoning;
	std::string assessment;
	std::string assessment_reason;
};

std::optional<Json> json_object_in(const std::string &text) {
	Json parsed = Json::parse(text, nullptr, false);
	if (parsed.is_object()) return parsed;
	const auto open = text.find('{');
	const auto close = text.rfind('}');
	if (open == std::string::npos || close == std::string::npos || close <= open) return std::nullopt;
	parsed = Json::parse(text.substr(open, close - open + 1), nullptr, false);
	if (parsed.is_object()) return parsed;
	return std::nullopt;
}

std::vector<AnsweredCell> answered_cells(const Json &answer) {
	std::vector<AnsweredCell> cells;
	if (!answer.contains("cells") || !answer.at("cells").is_array()) return cells;
	const auto text = [](const Json &value, const char *key) {
		return value.contains(key) && value.at(key).is_string() ? trimmed(value.at(key).get<std::string>()) : std::string();
	};
	for (const Json &entry : answer.at("cells")) {
		if (!entry.is_object()) continue;
		AnsweredCell cell;
		cell.column_id = text(entry, "columnId");
		cell.found = entry.contains("found") && entry.at("found").is_boolean() && entry.at("found").get<bool>();
		cell.answer = text(entry, "answer");
		cell.value = text(entry, "value");
		cell.quote = text(entry, "quote");
		cell.passage = text(entry, "passage");
		cell.basis = text(entry, "basis");
		cell.confidence = text(entry, "confidence");
		cell.reasoning = text(entry, "reasoning");
		cell.assessment = text(entry, "assessment");
		cell.assessment_reason = text(entry, "assessmentReason");
		cells.push_back(std::move(cell));
	}
	return cells;
}

// The numbers `text` states, in digits ("2,000,000" is 2000000) or in words
// ("twenty-four", "two million"), each as its digits.
std::set<std::string> numbers_in(const std::string &text) {
	static const std::map<std::string, long long> small = {{"zero", 0}, {"one", 1}, {"two", 2}, {"three", 3},
		{"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11},
		{"twelve", 12}, {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17},
		{"eighteen", 18}, {"nineteen", 19}, {"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50}, {"sixty", 60},
		{"seventy", 70}, {"eighty", 80}, {"ninety", 90}};
	static const std::map<std::string, long long> scales = {{"thousand", 1000}, {"million", 1000000},
		{"billion", 1000000000}};
	std::set<std::string> found;
	long long total = 0;
	long long current = 0;
	bool isCounting = false;
	const auto close = [&]() {
		if (isCounting == true) found.insert(std::to_string(total + current));
		total = 0;
		current = 0;
		isCounting = false;
	};
	for (const platform::AlignedWord &word : platform::aligned_words(text)) {
		if (word.text.empty() == false && std::all_of(word.text.begin(), word.text.end(),
				[](unsigned char character) { return std::isdigit(character) != 0; })) {
			close();
			// "0012" and "12" are one number
			const std::size_t first = word.text.find_first_not_of('0');
			found.insert(first == std::string::npos ? std::string("0") : word.text.substr(first));
			continue;
		}
		const auto smallNumber = small.find(word.text);
		if (smallNumber != small.end()) {
			current += smallNumber->second;
			isCounting = true;
			continue;
		}
		if (word.text == "hundred" && isCounting == true) {
			current *= 100;
			continue;
		}
		const auto scale = scales.find(word.text);
		if (scale != scales.end() && isCounting == true) {
			total += current * scale->second;
			current = 0;
			continue;
		}
		if (word.text == "and" && isCounting == true) continue;
		close();
	}
	close();
	return found;
}

// Whether every word the alignment skipped or changed, on either side, may
// differ without changing what the clause says. An alignment exists for a party
// or a figure written another way, so only these may differ: an article, a
// figure word (the numbers themselves are compared apart), a word of a party's
// role ("the other party", "the Supplier"), or a name or defined term written
// with a capital letter after the text's first word. A lower-case word that
// carries meaning - "indirect", "solely", "prior", "written" - may not: a quote
// that drops one says less than the document. Text outside ASCII has no capital
// this test can read, so it never counts as a name here, and such a quote reads
// as worded differently: the safe side.
bool differences_keep_meaning(const std::string &quote, const std::string &found) {
	static const std::set<std::string> interchangeable = {"a", "an", "the", "s",
		"party", "parties", "other", "supplier", "customer", "company", "seller", "buyer", "vendor", "client",
		"licensor", "licensee", "discloser", "recipient", "disclosing", "receiving", "lessor", "lessee", "landlord",
		"tenant", "contractor", "consultant", "provider", "purchaser"};
	const std::vector<platform::AlignedWord> quoted = platform::aligned_words(quote);
	const std::vector<platform::AlignedWord> stated = platform::aligned_words(found);
	std::map<std::string, int> balance;
	for (const platform::AlignedWord &word : quoted) ++balance[word.text];
	for (const platform::AlignedWord &word : stated) --balance[word.text];
	const auto may_differ = [](const std::string &text, const std::vector<platform::AlignedWord> &words,
			const std::string &value) {
		if (interchangeable.count(value) > 0 || platform::figure_word(value)) return true;
		if (!value.empty() && std::all_of(value.begin(), value.end(),
				[](unsigned char character) { return std::isdigit(character) != 0; }))
			return true;
		// A name or a defined term: every place the word stands in this text is
		// written with a capital letter, and none is the text's first word.
		bool seen = false;
		for (std::size_t index = 0; index < words.size(); ++index) {
			if (words[index].text != value) continue;
			seen = true;
			const unsigned char initial = static_cast<unsigned char>(text[words[index].start]);
			if (index == 0 || initial >= 0x80 || std::isupper(initial) == 0) return false;
		}
		return seen;
	};
	for (const auto &[value, surplus] : balance) {
		if (surplus > 0 && !may_differ(quote, quoted, value)) return false;
		if (surplus < 0 && !may_differ(found, stated, value)) return false;
	}
	return true;
}

// Whether `found`, the document's words an alignment took for `quote`, say what
// the quote says. The alignment exists for a name or a figure written another
// way: a party named in full where the document has its term, a sum the
// document writes in words. It skips words to get there, so on its own it
// would also take "The Seller shall be liable" for "The Seller shall not be
// liable", or USD 2,000,000 for $1,000,000. It holds only while every number
// the quote states is in the document's words, and no word that turns a
// clause's meaning - a negation, an exception, a modal - is in one and not the
// other.
bool alignment_holds(const std::string &quote, const std::string &found) {
	const std::set<std::string> quoted = numbers_in(quote);
	if (quoted.empty() == false) {
		const std::set<std::string> stated = numbers_in(found);
		for (const std::string &number : quoted)
			if (stated.count(number) == 0) return false;
	}
	// "non" is the first half of a hyphenated "non-exclusive", "non-assignable" or
	// "non-refundable", which the alignment reads as two words.
	static const std::set<std::string> meaning = {"not", "no", "non", "never", "neither", "nor", "without", "except",
		"unless", "cannot", "t", "nothing", "none", "only", "may", "shall", "must", "will", "should", "might",
		"excluding", "including", "notwithstanding", "save"};
	std::multiset<std::string> quoteWords;
	std::multiset<std::string> foundWords;
	for (const platform::AlignedWord &word : platform::aligned_words(quote))
		if (meaning.count(word.text) > 0) quoteWords.insert(word.text);
	for (const platform::AlignedWord &word : platform::aligned_words(found))
		if (meaning.count(word.text) > 0) foundWords.insert(word.text);
	if (quoteWords != foundWords) return false;
	return differences_keep_meaning(quote, found);
}

// Where `quote` stands in the document: in the passage the model named, else
// in any passage, else across two passages that meet. Singularity's own
// finding - never the model's word for it. A quote whose words differ from the
// document's - Papinian Guard restores a name or a figure in one form where
// the document has another - is found by its words in order, and kept as the
// document's own words; it counts as found only while those words say what
// the quote says (alignment_holds), and otherwise reads as worded differently.
matters::ReviewEvidence locate(const std::string &quote, const std::string &passage,
		const std::vector<documents::DocumentChunk> &chunks) {
	matters::ReviewEvidence evidence;
	evidence.quote = quote;
	if (quote.empty()) return evidence;
	const auto found_in = [&](std::size_t index) {
		const documents::DocumentChunk &chunk = chunks[index];
		const auto at = platform::find_phrase(chunk.text, quote);
		if (!at) return false;
		evidence.verified = true;
		evidence.chunk_id = chunk.chunk_id;
		evidence.start = chunk.source_start + at->first;
		evidence.length = at->second;
		if (chunk.page) evidence.page = static_cast<int>(*chunk.page);
		evidence.heading = chunk.heading.value_or(std::string());
		return true;
	};
	if (passage.size() > 1 && (passage[0] == 'P' || passage[0] == 'p')) {
		try {
			const std::size_t index = std::stoul(passage.substr(1));
			if (index >= 1 && index <= chunks.size() && found_in(index - 1)) return evidence;
		} catch (const std::exception &) {
		}
	}
	for (std::size_t index = 0; index < chunks.size(); ++index)
		if (found_in(index)) return evidence;
	for (std::size_t index = 0; index + 1 < chunks.size(); ++index) {
		const std::string joined = chunks[index].text + " " + chunks[index + 1].text;
		const auto at = platform::find_phrase(joined, quote);
		if (!at) continue;
		evidence.verified = true;
		evidence.chunk_id = chunks[index].chunk_id;
		evidence.start = chunks[index].source_start + at->first;
		evidence.length = at->second;
		if (chunks[index].page) evidence.page = static_cast<int>(*chunks[index].page);
		evidence.heading = chunks[index].heading.value_or(std::string());
		return evidence;
	}
	// Not word for word: the passage the quote means, by its words in order -
	// in the passage the model named first.
	std::vector<std::size_t> order;
	if (passage.size() > 1 && (passage[0] == 'P' || passage[0] == 'p')) {
		try {
			const std::size_t named = std::stoul(passage.substr(1));
			if (named >= 1 && named <= chunks.size()) order.push_back(named - 1);
		} catch (const std::exception &) {
		}
	}
	for (std::size_t index = 0; index < chunks.size(); ++index)
		if (order.empty() || order.front() != index) order.push_back(index);
	for (const std::size_t index : order) {
		const documents::DocumentChunk &chunk = chunks[index];
		const auto at = platform::align_words(chunk.text, quote);
		if (!at) continue;
		evidence.quote = chunk.text.substr(at->first, at->second);
		evidence.verified = alignment_holds(quote, evidence.quote);
		evidence.aligned = true;
		evidence.chunk_id = chunk.chunk_id;
		evidence.start = chunk.source_start + at->first;
		evidence.length = at->second;
		if (chunk.page) evidence.page = static_cast<int>(*chunk.page);
		evidence.heading = chunk.heading.value_or(std::string());
		return evidence;
	}
	return evidence;
}

std::string failure_reason(providers::TaskErrorCode code, const std::string &model, bool is_local) {
	switch (code) {
		case providers::TaskErrorCode::PrivacyProtectionUnavailable:
			return "Protection is not ready on this computer, so this document was not sent. Run the table again once "
				"protection is ready.";
		case providers::TaskErrorCode::ProtectedPayloadLeakDetected:
			return "Protection could not hide everything it should in this document, so it was not sent.";
		case providers::TaskErrorCode::ProviderAuthentication:
			return "The model's provider refused the saved key. Check the connection in Settings, then run the table again.";
		case providers::TaskErrorCode::ProviderRateLimit:
			return "The model's provider is busy. Run the table again in a few minutes; answered documents are kept.";
		case providers::TaskErrorCode::NetworkError:
			return "The model could not be reached. Run the table again when the connection is back; answered documents "
				"are kept.";
		case providers::TaskErrorCode::ContextCapacityExceeded:
			return "This document is longer than " + model + " can read at once, so nothing was sent. Choose a model "
				"that reads longer documents " + std::string(is_local ? "in Models" : "in AI Setup")
				+ ", then run the table again.";
		case providers::TaskErrorCode::UnsupportedModel:
		case providers::TaskErrorCode::ProviderRejectedRequest:
			// A table asks the model the Matter's AI Setup gives (model_for), so
			// that is where another is chosen.
			return model + " refused this request. Choose another model " + std::string(is_local
				? "on this computer in Models" : "for this Matter's reasoning in AI Setup") + ", then run the table again.";
		default:
			return "This document could not be answered. Run the table again to try it once more.";
	}
}

// The model a run asks: the one named for this run, else the Matter's
// reasoning model as its AI Setup gives it now - never the model an earlier
// run asked, which the table keeps only to say what answered it (and each row
// keeps its own). A model retired, disconnected or uninstalled is then put
// right where the lawyer chooses models, and a Matter moved to another model
// moves its tables with it. Kept to this computer's own model when the
// table's work stays here.
routing::ModelTarget model_for(const std::string &project_id, const std::optional<routing::ModelTarget> &named,
		bool local_only) {
	std::optional<routing::ModelTarget> model = named;
	if (!model) {
		chat::OperationSetupRequest setup;
		setup.project_id = project_id;
		model = chat::resolve_operation_setup(setup).primary_model;
	}
	if (local_only) {
		if (model && model->provider_id == "local") return *model;
		if (named)
			throw std::runtime_error("This table works only on this computer, so it is never sent to a model outside it. "
				"Choose a model on this computer in Models, then run it again.");
		const std::optional<std::string> local = routing::preferred_local_generative_model();
		if (!local)
			throw std::runtime_error("This table works only on this computer, and no model is installed on it, so nothing "
				"was sent. Install a model in Models, then run it again.");
		return routing::ModelTarget{"local", *local};
	}
	if (!model)
		throw std::runtime_error("No model is set for this Matter's reasoning. Choose one in AI Setup, then run the table.");
	return *model;
}

// The Matter's record of the run, for Usage and the Guard: one operation, its
// calls logged as they are made.
void record_call(const std::string &project_id, const std::string &run_task_id, const routing::ModelTarget &model,
		const routing::InferenceResult *result, const std::optional<providers::TaskErrorCode> &error, long long latency_ms) {
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		std::optional<matters::Matter> matter = matters::load_matter(workspace, project_id);
		if (!matter) return;
		const auto task = std::find_if(matter->tasks.begin(), matter->tasks.end(),
			[&](const matters::MatterTask &entry) { return entry.id == run_task_id; });
		if (task != matter->tasks.end() && result != nullptr) {
			task->usage = providers::accumulate_usage(task->usage, result->usage);
			task->protected_api_applied = task->protected_api_applied || result->protected_api_applied;
			task->privacy_metadata = result->privacy_metadata;
			matters::save_matter(workspace, *matter);
		}
		routing::RoutingLogEntry entry;
		entry.matter_id = project_id;
		entry.operation_id = run_task_id;
		entry.task_id = run_task_id;
		entry.time = workspaces::current_timestamp();
		entry.provider_id = model.provider_id;
		entry.model_id = model.model_id;
		entry.execution_kind = model.provider_id == "local" ? "local" : "external";
		entry.legal_task_preset_id = "review-table";
		entry.success = result != nullptr;
		entry.latency_ms = latency_ms;
		if (result != nullptr) {
			entry.privacy_metadata = result->privacy_metadata;
			entry.usage = result->usage;
			entry.finish_reason = result->finish_reason;
		}
		if (error) entry.error_code = error;
		// A failed external call carries no protection result: its record says
		// the mode this computer's policy held it to, never an empty record's "off".
		if (result == nullptr && model.provider_id != "local")
			entry.privacy_metadata.mode = protected_api::to_string(
				platform::load_app_settings().local_node_setup.protected_api_policy.mode);
		routing::append_routing_log_entry(workspace, entry);
	} catch (...) {
	}
}

std::string markdown_cell(std::string text) {
	for (char &character : text)
		if (character == '\n' || character == '\r') character = ' ';
	std::string escaped;
	for (const char character : text) {
		if (character == '|') escaped += "\\|";
		else escaped += character;
	}
	return escaped;
}

// The table as a Work Product reads it: a Markdown table, and the verified
// quotes as its sources.
void keep_as_work_product(const std::string &project_id, const std::string &table_id) {
	const matters::Matter matter = load(project_id);
	const auto found = std::find_if(matter.review_tables.begin(), matter.review_tables.end(),
		[&](const matters::ReviewTable &table) { return table.id == table_id; });
	if (found == matter.review_tables.end()) return;
	const matters::ReviewTable &table = *found;
	std::string content = "| Document |";
	std::string rule = "| --- |";
	for (const matters::ReviewColumn &column : table.columns) {
		content += " " + markdown_cell(column.heading) + " |";
		rule += " --- |";
	}
	content += "\n" + rule + "\n";
	std::vector<context::ResolvedSourceReference> sources;
	for (const matters::ReviewDocument &document : table.documents) {
		content += "| " + markdown_cell(document.name) + " |";
		const matters::ReviewRow *row = row_in(matter, table.id, document.document_id);
		for (const matters::ReviewColumn &column : table.columns) {
			std::string shown;
			if (row != nullptr)
				for (const matters::ReviewCell &cell : row->cells) {
					if (cell.column_id != column.id) continue;
					shown = cell.state == "not_found" ? "Not addressed" : cell.answer;
					if (cell.state == "answered" && (cell.evidence.empty() || !cell.evidence.front().verified))
						shown += " (quote not found in the document)";
					for (const matters::ReviewEvidence &evidence : cell.evidence) {
						if (!evidence.verified) continue;
						context::ResolvedSourceReference source;
						source.document_id = document.document_id;
						source.chunk_id = evidence.chunk_id;
						source.document_name = document.name;
						if (evidence.page) source.page = static_cast<std::size_t>(*evidence.page);
						if (!evidence.heading.empty()) source.heading = evidence.heading;
						const bool known = std::any_of(sources.begin(), sources.end(), [&](const auto &entry) {
							return entry.document_id == source.document_id && entry.chunk_id == source.chunk_id;
						});
						if (!known) sources.push_back(std::move(source));
					}
				}
			content += " " + markdown_cell(shown) + " |";
		}
		content += "\n";
	}
	if (!table.work_product_id.empty()) {
		for (const matters::WorkProduct &kept : matter.work_products)
			if (kept.id == table.work_product_id) {
				if (kept.content == content && kept.title == table.title) return;
				work_product::UpdateWorkProductCommand update;
				update.project_id = project_id;
				update.work_product_id = kept.id;
				update.expected_revision = kept.revision;
				update.title = table.title;
				update.content = content;
				(void)work_product::update(std::move(update));
				return;
			}
	}
	work_product::CreateWorkProductCommand command;
	command.project_id = project_id;
	command.type_id = matters::kWorkProductTypeReviewTable;
	command.title = table.title;
	command.content = content;
	command.source_refs = std::move(sources);
	command.origin.kind = matters::WorkProductOriginKind::ReviewTable;
	command.origin.reference_id = table.id;
	const matters::WorkProductObject created = work_product::create(std::move(command));
	change_matter(project_id, [&](matters::Matter &current) {
		table_in(current, table_id).work_product_id = created.id;
		return true;
	});
}

} // namespace

nlohmann::json row_contract(const matters::ReviewTable &table) {
	const Json text{{"type", "string"}};
	const Json cell{{"type", "object"},
		{"properties", Json{{"columnId", text}, {"found", Json{{"type", "boolean"}}}, {"answer", text}, {"value", text},
			{"quote", text}, {"passage", text}, {"basis", Json{{"type", "string"}, {"enum", {"stated", "inferred"}}}},
			{"confidence", Json{{"type", "string"}, {"enum", {"high", "medium", "low"}}}}, {"reasoning", text},
			{"assessment", Json{{"type", "string"}, {"enum", {"conforms", "deviates", "missing", "not_applicable"}}}},
			{"assessmentReason", text}}},
		{"required", {"columnId", "found", "answer", "value", "quote", "passage", "basis", "confidence", "reasoning",
			"assessment", "assessmentReason"}},
		{"additionalProperties", false}};
	return Json{{"type", "object"},
		{"properties", Json{{"cells", Json{{"type", "array"}, {"maxItems", table.columns.size()}, {"items", cell}}}}},
		{"required", {"cells"}}, {"additionalProperties", false}};
}

std::string row_prompt(const matters::ReviewTable &table, const std::string &document_name,
		const std::vector<std::string> &passages) {
	bool anyPosition = false;
	Json questions = Json::array();
	for (const matters::ReviewColumn &column : table.columns) {
		Json question{{"columnId", column.id}, {"question", column.question}, {"answerKind", column.answer_kind}};
		if (!column.position.empty()) {
			question["firmPosition"] = column.position;
			anyPosition = true;
		}
		if (!column.law.empty()) question["lawTheFirmCites"] = column.law;
		questions.push_back(std::move(question));
	}
	std::string prompt =
		"You are reviewing one document of a legal matter for a review table: the same questions are asked of every "
		"document in the table. Answer each question below from this document alone.\n\n"
		"Rules:\n"
		"- Use only the document text below. Never fill a gap from general knowledge of the parties, the market or "
		"the law.\n"
		"- For every question you answer, copy into \"quote\" the exact words of the document the answer rests on: one "
		"continuous passage, copied character for character, at most about 60 words, and put its label (P1, P2, ...) "
		"in \"passage\".\n"
		"- When the document does not address the question, set \"found\" to false, leave \"quote\" and \"passage\" "
		"empty, and say in \"answer\" that the document does not address it.\n"
		"- When two parts of the document say different things, answer with both and quote the one that controls; say "
		"so in \"reasoning\".\n"
		"- \"value\" is the answer in a normal form for its kind: date as YYYY-MM-DD; amount as an ISO currency code "
		"and a number, like \"USD 2000000\"; yes_no as \"yes\" or \"no\"; duration as a number and \"days\", "
		"\"months\" or \"years\"; party as the party's name; clause and text as a short phrase.\n"
		"- \"basis\" is \"stated\" when the quoted words say the answer, \"inferred\" when you concluded it from them.\n"
		"- \"confidence\" is \"high\", \"medium\" or \"low\". \"reasoning\" is one short sentence.\n";
	if (anyPosition)
		prompt +=
			"- Some questions carry the firm's position (\"firmPosition\"): the firm's own policy for such contracts - "
			"not law, and not a fact of this document. For those, set \"assessment\" to \"conforms\" when the document "
			"meets the position, \"deviates\" when it departs from it, or \"missing\" when the document has no such "
			"term, and give the reason in \"assessmentReason\" in one sentence. For questions without a position, set "
			"\"assessment\" to \"not_applicable\" and leave \"assessmentReason\" empty.\n"
			"- \"lawTheFirmCites\" is the law the firm cites for its position, in the firm's words. Use it only to "
			"understand the position; never add law of your own, and never treat it as a fact of this document.\n";
	else
		prompt += "- Set \"assessment\" to \"not_applicable\" and leave \"assessmentReason\" empty.\n";
	prompt += "- Answer every question once, with its columnId.\n\nQuestions:\n" + questions.dump(1) + "\n\nDocument: "
		+ document_name + "\n\n";
	for (std::size_t index = 0; index < passages.size(); ++index)
		prompt += "[P" + std::to_string(index + 1) + "] " + passages[index] + "\n\n";
	return prompt;
}

matters::ReviewTable create(const CreateReviewTable &command) {
	return change_matter(command.project_id, [&](matters::Matter &matter) {
		matters::ReviewTable table;
		table.id = matters::generate_review_table_id();
		table.title = trimmed(command.title).empty() ? std::string("Review table") : trimmed(command.title);
		table.created_at = workspaces::current_timestamp();
		table.updated_at = table.created_at;
		table.created_by_actor_id = platform::resolve_actor_identity().actor_id;
		table.documents = documents_from(matter, command.document_ids);
		table.columns = columns_from(command.columns);
		table.playbook_id = command.playbook_id;
		table.playbook_version = command.playbook_version;
		table.local_only = command.local_only;
		require_shape(table);
		matter.review_tables.push_back(table);
		matter.updated_at = table.updated_at;
		return table;
	});
}

matters::ReviewTable update(const UpdateReviewTable &command) {
	return change_matter(command.project_id, [&](matters::Matter &matter) {
		matters::ReviewTable &table = table_in(matter, command.table_id);
		if (table.run_state == "running" && live_run(command.project_id, command.table_id) != nullptr)
			throw std::runtime_error("The table is being answered. Stop it before changing its documents or questions.");
		if (command.title && !trimmed(*command.title).empty()) table.title = trimmed(*command.title);
		if (command.document_ids) table.documents = documents_from(matter, *command.document_ids);
		if (command.columns) {
			std::vector<matters::ReviewColumn> columns;
			for (matters::ReviewColumn column : *command.columns) {
				column.question = trimmed(column.question);
				if (column.question.empty()) throw std::runtime_error("Write a question for every column.");
				if (trimmed(column.heading).empty()) column.heading = column.question;
				if (answer_kinds().count(column.answer_kind) == 0) column.answer_kind = "text";
				// A column keeps its id only while it asks the same thing.
				const auto before = std::find_if(table.columns.begin(), table.columns.end(),
					[&](const matters::ReviewColumn &entry) { return entry.id == column.id; });
				if (before == table.columns.end() || before->question != column.question
						|| before->answer_kind != column.answer_kind || before->position != column.position)
					column.id = matters::generate_review_column_id();
				columns.push_back(std::move(column));
			}
			table.columns = std::move(columns);
		}
		if (command.local_only) table.local_only = table.local_only || *command.local_only;
		require_shape(table);
		// Rows of documents no longer in the table go with them.
		std::set<std::string> kept;
		for (const matters::ReviewDocument &document : table.documents) kept.insert(row_id(table.id, document.document_id));
		matter.review_rows.erase(std::remove_if(matter.review_rows.begin(), matter.review_rows.end(),
			[&](const matters::ReviewRow &row) { return row.table_id == table.id && kept.count(row.id) == 0; }),
			matter.review_rows.end());
		table.updated_at = workspaces::current_timestamp();
		++table.revision;
		return table;
	});
}

bool remove(const std::string &project_id, const std::string &table_id) {
	if (live_run(project_id, table_id) != nullptr)
		throw std::runtime_error("The table is being answered. Stop it before removing it.");
	return change_matter(project_id, [&](matters::Matter &matter) {
		const auto before = matter.review_tables.size();
		matter.review_tables.erase(std::remove_if(matter.review_tables.begin(), matter.review_tables.end(),
			[&](const matters::ReviewTable &table) { return table.id == table_id; }), matter.review_tables.end());
		matter.review_rows.erase(std::remove_if(matter.review_rows.begin(), matter.review_rows.end(),
			[&](const matters::ReviewRow &row) { return row.table_id == table_id; }), matter.review_rows.end());
		return matter.review_tables.size() != before;
	});
}

std::vector<matters::ReviewTable> list(const std::string &project_id) {
	const matters::Matter matter = load(project_id);
	std::vector<matters::ReviewTable> tables;
	for (const matters::ReviewTable &table : matter.review_tables) tables.push_back(view_of(matter, table).table);
	return tables;
}

ReviewTableView get(const std::string &project_id, const std::string &table_id) {
	matters::Matter matter = load(project_id);
	return view_of(matter, table_in(matter, table_id));
}

void request_stop(const std::string &project_id, const std::string &table_id) {
	if (const std::shared_ptr<LiveRun> run = live_run(project_id, table_id)) {
		run->stop.store(true);
		run->token->cancel();
	}
	change_matter(project_id, [&](matters::Matter &matter) {
		matters::ReviewTable &table = table_in(matter, table_id);
		if (table.run_state == "running") table.stop_requested = true;
		return true;
	});
}

void note_change(const std::string &project_id, const std::string &table_id, const std::string &document_id,
		const std::string &operation_id) {
	change_matter(project_id, [&](matters::Matter &matter) {
		const std::string id = row_id(table_id, document_id);
		for (matters::ReviewRow &row : matter.review_rows)
			if (row.id == id) row.change_operation_id = operation_id;
		return true;
	});
}

ReviewTableView run(const std::string &project_id, const std::string &table_id, const RunOptions &options) {
	// Nothing new begins once Singularity is closing (Platform/AppLifecycle).
	if (platform::lifecycle::is_closing())
		throw std::runtime_error("Singularity is closing, so the table is not answered now. Run it again once Singularity"
			" is open.");
	const std::string key = project_id + "\n" + table_id;
	const auto live = std::make_shared<LiveRun>();
	{
		const std::lock_guard<std::mutex> lock(live_mutex());
		if (live_runs().count(key) > 0) throw std::runtime_error("This table is already being answered.");
		live_runs()[key] = live;
	}
	struct Unregister {
		std::string key;
		~Unregister() {
			const std::lock_guard<std::mutex> lock(live_mutex());
			live_runs().erase(key);
		}
	} unregister{key};
	// Closing stops the request in flight on its connection, and the run ends
	// before its next document.
	const platform::lifecycle::CancelOnClosing closingStops(live->token);

	// Begin: which model, kept where, recorded before anything is sent.
	matters::Matter matter = load(project_id);
	matters::ReviewTable table = table_in(matter, table_id);
	require_shape(table);
	const std::string device = platform::current_device_id();
	if (table.run_state == "running" && !table.run_device_id.empty() && table.run_device_id != device)
		throw std::runtime_error("This table is being answered on another computer. Run it there, or wait until it "
			"finishes.");
	const bool localOnly = table.local_only || routing::ScopedLocalOnlyWork::is_active();
	const routing::ModelTarget model = model_for(project_id, options.model, localOnly);
	const bool isLocal = model.provider_id == "local";
	const std::string runId = matters::generate_review_run_id();
	const std::string runTaskId = matters::generate_task_id();
	tools::OperationAuthority authority;
	authority.actor_id = platform::resolve_actor_identity().actor_id;
	authority.ceiling = tools::ProjectActionMode::AskForApproval;
	authority.local_only = localOnly;
	tools::begin_operation_authority(project_id, runId, authority);
	change_matter(project_id, [&](matters::Matter &current) {
		matters::ReviewTable &entry = table_in(current, table_id);
		entry.run_state = "running";
		entry.run_id = runId;
		entry.run_device_id = device;
		entry.run_started_at = workspaces::current_timestamp();
		entry.run_finished_at.clear();
		entry.stop_requested = false;
		entry.provider_id = model.provider_id;
		entry.model_id = model.model_id;
		entry.local_only = localOnly;
		entry.run_message.clear();
		entry.updated_at = entry.run_started_at;
		matters::MatterTask task;
		task.id = runTaskId;
		task.created_at = entry.run_started_at;
		task.operation_type = "review_table";
		task.operation_role = "reasoning";
		task.agent_operation_id = runId;
		task.prompt = "Review table: " + entry.title;
		task.requested_provider_id = model.provider_id;
		task.requested_model_id = model.model_id;
		task.provider_id = model.provider_id;
		task.model_id = model.model_id;
		task.execution_kind = isLocal ? "local" : "external";
		if (isLocal) task.runtime_id = "local";
		task.local_only = localOnly;
		task.operation_device_id = device;
		task.status = matters::TaskStatus::Success;
		current.tasks.push_back(std::move(task));
		return true;
	});

	const routing::ScopedLocalOnlyWork keptHere(localOnly);
	std::size_t answered = 0;
	std::size_t failed = 0;
	bool stopped = false;
	std::string stoppedFor;
	for (const matters::ReviewDocument &document : table.documents) {
		if (live->stop.load() || platform::lifecycle::is_closing()) {
			stopped = true;
			break;
		}
		matter = load(project_id);
		const std::string status = row_status(matter, table_in(matter, table_id), document.document_id);
		if (status == "current" || status == "unavailable") continue;
		matters::ReviewTable asked = table_in(matter, table_id);
		const matters::MatterDocument *current = document_in(matter, document.document_id);
		// A row still current but for questions added since keeps its answers:
		// only the new questions are asked.
		std::vector<matters::ReviewCell> keptCells;
		if (const matters::ReviewRow *before = row_in(matter, table_id, document.document_id);
				status == "missing" && before != nullptr && before->state == "answered" && current != nullptr
				&& before->document_fingerprint == current->fingerprint) {
			std::vector<matters::ReviewColumn> missing;
			for (const matters::ReviewColumn &column : asked.columns) {
				const auto answered = std::find_if(before->cells.begin(), before->cells.end(),
					[&](const matters::ReviewCell &cell) { return cell.column_id == column.id; });
				if (answered == before->cells.end()) missing.push_back(column);
				else keptCells.push_back(*answered);
			}
			asked.columns = std::move(missing);
		}
		matters::ReviewRow row;
		row.id = row_id(table_id, document.document_id);
		row.table_id = table_id;
		row.document_id = document.document_id;
		row.run_id = runId;
		row.provider_id = model.provider_id;
		row.model_id = model.model_id;
		row.destination = isLocal ? "local" : "protected";
		if (current != nullptr) {
			row.document_fingerprint = current->fingerprint;
			row.document_version = current->version;
		}

		// The document, whole, in its own order.
		std::vector<documents::DocumentChunk> chunks;
		try {
			std::vector<matters::UnavailableSourceDocument> unavailable;
			const std::vector<context::RetrievedContextChunk> retrieved = matters::retrieve_document_context(
				node::require_active_workspace_path(), matter, std::string(), {document.document_id}, &unavailable,
				matters::SelectedSourceDelivery::EveryPassage);
			for (const context::RetrievedContextChunk &entry : retrieved) chunks.push_back(entry.chunk);
			std::sort(chunks.begin(), chunks.end(),
				[](const auto &left, const auto &right) { return left.source_start < right.source_start; });
		} catch (const std::exception &error) {
			row.state = "skipped";
			row.message = std::string("Singularity could not read this document: ") + error.what();
		}
		if (row.state.empty() && chunks.empty()) {
			row.state = "skipped";
			row.message = "Singularity found no text in this document to answer from.";
		}

		if (row.state.empty()) {
			std::vector<std::string> passages;
			for (const documents::DocumentChunk &chunk : chunks) passages.push_back(chunk.text);
			routing::InferenceRequest request;
			request.call_purpose = routing::observability::CallPurpose::SubstantiveGeneration;
			request.model_target = model;
			{
				const persistence::PersistenceLock lock(persistence::persistence_mutex());
				request.workspace_id = platform::load_app_settings().local_node_setup.active_workspace_id;
			}
			if (isLocal) {
				const auto window = routing::estimate_model_context_window_tokens(model);
				request.maximum_output_tokens = window ? std::max<std::uint64_t>(1024, *window / 4) : 2048;
			} else {
				request.maximum_output_tokens = 8192;
				request.project_entity_hints = context_actions::memory_protection_hints(project_id);
			}
			request.prompt = row_prompt(asked, document.name, passages);
			request.research_mode = research::ResearchMode::Off;
			request.research_final_output_contract = providers::GenerationOutputContract{"review_table_row", row_contract(asked)};
			request.operation_symbols = std::make_shared<protected_api::papinian::OperationProtectedSymbolTable>();
			request.operation_symbols->operation_id = runId;
			const auto started = std::chrono::steady_clock::now();
			const auto elapsed = [&]() {
				return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - started).count());
			};
			try {
				routing::require_execution_ready(request);
				routing::InferenceResult result;
				{
					const platform::HttpCancelScope scope(live->token);
					result = routing::execute(request);
				}
				row.latency_ms = elapsed();
				record_call(project_id, runTaskId, model, &result, std::nullopt, row.latency_ms);
				row.input_tokens = static_cast<std::uint64_t>(result.usage.input_tokens.value_or(0));
				row.output_tokens = static_cast<std::uint64_t>(result.usage.output_tokens.value_or(0));
				row.cost_micros = result.usage.cost_micros;
				row.cost_currency = result.usage.cost_currency.value_or(std::string());
				if (!isLocal && !result.protected_api_applied) row.destination = "unprotected";
				const std::optional<Json> parsed = json_object_in(result.text);
				const std::vector<AnsweredCell> cells = parsed ? answered_cells(*parsed) : std::vector<AnsweredCell>();
				if (cells.empty()) {
					row.state = "failed";
					row.message = "The model's answer for this document could not be read. Run the table again to ask it once more.";
				} else {
					row.state = "answered";
					for (const matters::ReviewColumn &column : asked.columns) {
						matters::ReviewCell cell;
						cell.column_id = column.id;
						const auto given = std::find_if(cells.begin(), cells.end(),
							[&](const AnsweredCell &entry) { return entry.column_id == column.id; });
						if (given == cells.end()) {
							cell.state = "failed";
							cell.answer = "The model gave no answer to this question.";
							row.cells.push_back(std::move(cell));
							continue;
						}
						cell.state = given->found ? "answered" : "not_found";
						cell.answer = given->answer;
						cell.value = given->value;
						cell.confidence = given->confidence;
						cell.basis = given->basis;
						cell.reasoning = given->reasoning;
						if (!column.position.empty()) {
							cell.assessment = given->assessment == "not_applicable" ? std::string() : given->assessment;
							cell.assessment_reason = given->assessment_reason;
						}
						if (given->found) cell.evidence.push_back(locate(given->quote, given->passage, chunks));
						row.cells.push_back(std::move(cell));
					}
				}
			} catch (const providers::TaskError &error) {
				if (live->token->is_cancelled() || live->stop.load()) {
					stopped = true;
					break;
				}
				row.latency_ms = elapsed();
				record_call(project_id, runTaskId, model, nullptr, error.code(), row.latency_ms);
				row.state = "failed";
				row.message = failure_reason(error.code(), model.model_id, isLocal);
				// Nothing more can be sent this run: stop instead of failing every document.
				if (error.code() == providers::TaskErrorCode::PrivacyProtectionUnavailable
						|| error.code() == providers::TaskErrorCode::ProviderAuthentication
						|| error.code() == providers::TaskErrorCode::NetworkError) {
					stoppedFor = row.message;
				}
			} catch (const std::exception &error) {
				if (live->token->is_cancelled() || live->stop.load()) {
					stopped = true;
					break;
				}
				row.latency_ms = elapsed();
				record_call(project_id, runTaskId, model, nullptr, providers::TaskErrorCode::Unknown, row.latency_ms);
				row.state = "failed";
				row.message = error.what();
			}
		}
		if (row.state == "answered") {
			// The answers kept, in the table's own column order.
			std::vector<matters::ReviewCell> merged;
			for (const matters::ReviewColumn &column : table_in(matter, table_id).columns) {
				for (const std::vector<matters::ReviewCell> *from : {&row.cells, &keptCells})
					for (const matters::ReviewCell &cell : *from)
						if (cell.column_id == column.id
								&& std::none_of(merged.begin(), merged.end(),
									[&](const matters::ReviewCell &entry) { return entry.column_id == column.id; }))
							merged.push_back(cell);
			}
			row.cells = std::move(merged);
		}
		row.answered_at = workspaces::current_timestamp();
		if (row.state == "answered") ++answered;
		else ++failed;
		change_matter(project_id, [&](matters::Matter &current) {
			const auto existing = std::find_if(current.review_rows.begin(), current.review_rows.end(),
				[&](const matters::ReviewRow &entry) { return entry.id == row.id; });
			// A failure never replaces an answer the document still supports.
			if (existing != current.review_rows.end()) {
				if (row.state == "answered" || existing->document_fingerprint != row.document_fingerprint) *existing = row;
				else {
					existing->message = row.message;
				}
			} else {
				current.review_rows.push_back(row);
			}
			table_in(current, table_id).updated_at = row.answered_at;
			return true;
		});
		if (options.progress) options.progress(get(project_id, table_id));
		if (!stoppedFor.empty()) break;
	}

	// Finish: what the run did, in words. A run Singularity closed on is
	// interrupted, never the lawyer's Stop.
	const bool closedOn = stopped && platform::lifecycle::is_closing() && !live->stop.load();
	change_matter(project_id, [&](matters::Matter &current) {
		matters::ReviewTable &entry = table_in(current, table_id);
		entry.run_finished_at = workspaces::current_timestamp();
		entry.stop_requested = false;
		const ReviewTableView now = view_of(current, entry);
		if (closedOn) {
			entry.run_state = "interrupted";
			entry.run_message = "Singularity closed while this table was being answered. The answered documents are "
				"kept; run the table again to answer the rest.";
		} else if (stopped) {
			entry.run_state = "stopped";
			entry.run_message = "Stopped. Answered documents are kept; run the table again to answer the rest.";
		} else if (!stoppedFor.empty()) {
			entry.run_state = "stopped";
			entry.run_message = stoppedFor;
		} else if (now.to_answer == 0) {
			entry.run_state = "completed";
			entry.run_message.clear();
		} else {
			entry.run_state = answered > 0 ? "completed" : "failed";
			entry.run_message = std::to_string(now.to_answer) + (now.to_answer == 1 ? " document" : " documents")
				+ " could not be answered. Run the table again to try them once more.";
		}
		entry.updated_at = entry.run_finished_at;
		for (matters::MatterTask &task : current.tasks)
			if (task.id == runTaskId) {
				task.status = answered > 0 || failed == 0 ? matters::TaskStatus::Success : matters::TaskStatus::Error;
				task.operation_execution_state = closedOn ? "interrupted" : stopped ? "stopped" : "completed";
				task.operation_updated_at = entry.run_finished_at;
			}
		return true;
	});
	try {
		keep_as_work_product(project_id, table_id);
	} catch (const std::exception &) {
		// The table itself is kept; its Work Product is made again on the next run.
	}
	return get(project_id, table_id);
}

std::string export_xlsx(const std::string &project_id, const std::string &table_id) {
	const matters::Matter matter = load(project_id);
	const auto found = std::find_if(matter.review_tables.begin(), matter.review_tables.end(),
		[&](const matters::ReviewTable &table) { return table.id == table_id; });
	if (found == matter.review_tables.end()) throw std::runtime_error("This Review Table is no longer in the Matter.");
	const ReviewTableView view = view_of(matter, *found);
	return review_table_export::xlsx(view.table, view.rows, view.statuses);
}

std::string export_docx(const std::string &project_id, const std::string &table_id) {
	const matters::Matter matter = load(project_id);
	const auto found = std::find_if(matter.review_tables.begin(), matter.review_tables.end(),
		[&](const matters::ReviewTable &table) { return table.id == table_id; });
	if (found == matter.review_tables.end()) throw std::runtime_error("This Review Table is no longer in the Matter.");
	const ReviewTableView view = view_of(matter, *found);
	return review_table_export::docx(view.table, view.rows, view.statuses);
}

} // namespace singularity::review_table
