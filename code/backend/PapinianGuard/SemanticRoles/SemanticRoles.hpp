#pragma once

#include "Context/BusinessCalendar/BusinessCalendar.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeTypes.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTableTypes.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// Semantic roles (DESIGN4 §5, owner §6): what each protected fact IS in the
// matter - "Base Rent", «Ставка неустойки (по договору)», "Paid" @ July 2026 -
// in the material's own words, plus a closed universal role class (LIMIT,
// PRICE, BALANCE, DEADLINE ... PUBLIC). Read locally from defined terms,
// labels, table headers and clause captions, never from a value; sent only in
// words the call's source material already carries (the words rule). Legal
// knowledge decides roles only; arithmetic stays universal.
//
// Pure. Declared by the seam (lane S); lane R writes the bodies and the
// lexicons (role_lexicon_en.txt, role_lexicon_ru.txt, query_lexicon_en.txt,
// query_lexicon_ru.txt beside this header: legal-general words, no names).
namespace singularity::protected_api::papinian::roles {

struct RoleLabel {
	// The material's own words, as written.
	std::string words;
	enum class Source { DefinedTerm, LabelColon, TableHeader, WorkbookHeader, ClauseCaption, NounPhrase, Request };
	Source source = Source::NounPhrase;
	// Closed universal class (DESIGN4 §5.2) or "".
	std::string role_class;
	// Where the words stand, built only from Singularity's own handles and bare
	// references: "DOC_2 §4.1", "DOC_3 B12". Never a sheet's, a file's or a
	// party's name - those are the material's words (a sheet is often named
	// after a party), and a location is not a label.
	std::string location;
	// Local provenance.
	std::size_t passage = 0;
	std::size_t offset = 0;
	std::size_t length = 0;
};

// Each fact's role, by fact key.
[[nodiscard]] std::map<std::string, RoleLabel> assign_roles(
	const std::vector<singularity::context::business_calendar::RulePassage> &raw_passages,
	const std::vector<RequestMappingEntry> &rows, const std::vector<tables::ProtectedTable> &tables,
	semantic::Language language);

// The closed class a label's words name (EN and RU stems), or "".
[[nodiscard]] std::string role_class_of(std::string_view words, semantic::Language language);

// The computational type a role repairs (DESIGN4 §5.3): a day count typed as
// money is a DURATION or a COUNT; a cap percentage with no period is a
// PERCENTAGE. Nothing when the role changes nothing. Masking never changes.
[[nodiscard]] std::optional<compute::ArgType> repair_type(const RequestMappingEntry &row, const RoleLabel &label);

// The words rule (DESIGN4 §5.5): whether every word of `label` stands in the
// call's protected source-material spans.
[[nodiscard]] bool sendable(std::string_view label, const std::vector<std::string> &source_spans);

// The same rule against the spans' words, folded once (spans_words()): a
// legend asks it for each of hundreds of labels, and folding every span again
// for each cost seconds on every call of a protected answer.
[[nodiscard]] std::unordered_set<std::string> spans_words(const std::vector<std::string> &source_spans);
[[nodiscard]] bool sendable_in_words(std::string_view label, const std::unordered_set<std::string> &spans_words);

// "same role, two sources" pairs (DESIGN4 §5.7): value-free, at most
// kCrossCheckLines, never saying whether the figures agree.
struct CrossCheck {
	std::string role_class;
	// The two sides as the line names them (symbols or table columns).
	std::vector<std::string> sides;
	std::string line;
	// The material's words the line quotes, one per side: what the words rule
	// judges before the line is sent.
	std::vector<std::string> labels;
};

[[nodiscard]] std::vector<CrossCheck> cross_checks(const std::map<std::string, RoleLabel> &roles,
	const std::vector<tables::ProtectedTable> &tables, const OperationProtectedSymbolTable &table);

// ---------------------------------------------------------------------------
// How the payload writes roles and tables (DESIGN4 §5.4, §6.4). Strings only;
// the caller masks every material word through the operation's frozen
// mapping and keeps a label only when sendable() holds.

// The computational type of a row as the legend writes it (compute::written):
// "MONEY(USD/month)", "DATE(day)", "ENTITY(person)"; a role's repair applied.
[[nodiscard]] std::string type_of_row(const RequestMappingEntry &row, const std::optional<RoleLabel> &role = std::nullopt);

// The label's words in the quotes its language takes: "Base Rent", «Премия».
[[nodiscard]] std::string quoted_label(std::string_view words);

// One symbol's line: `[[Amount3]] MONEY(USD/month) PRICE "Base Rent" · DOC_1 §4.1`.
// `words` empty: the class alone; `role_class` empty too: the type alone.
// A request figure (RoleLabel::Source::Request) reads "your figure".
[[nodiscard]] std::string symbol_line(const std::string &symbol, const std::string &type, const std::string &role_class,
	const std::string &words, const std::string &location, bool request_figure = false,
	semantic::Language language = semantic::Language::English);

// The tables block (DESIGN4 §6.4) of the tables the evidence touched: for each
// described table its header line, one line of its described columns, its
// formula shapes and the cell-address line; `label` gives each material string
// (title, sheet, header) as it may be sent - empty when it may not, and then
// the line names the column by letter and type only. Tables not described are
// summarised per document by `more_sheets` (ControlText TablesMoreSheets).
[[nodiscard]] std::vector<std::string> table_block(const std::vector<tables::ProtectedTable> &tables,
	const std::function<std::string(const std::string &)> &label);

} // namespace singularity::protected_api::papinian::roles
