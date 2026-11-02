#include "PapinianGuard/ProtectedComputation/ComputeTypes.hpp"

#include <array>
#include <mutex>
#include <utility>

namespace singularity::protected_api::papinian::compute {

namespace {

struct CodeName {
	Code code;
	const char *name;
	CodeClass kind;
};

constexpr std::array<CodeName, 49> kCodes{{
	{Code::None, "", CodeClass::Static},
	{Code::BadJson, "bad_json", CodeClass::Static},
	{Code::BadId, "bad_id", CodeClass::Static},
	{Code::DuplicateId, "duplicate_id", CodeClass::Static},
	{Code::BadLabel, "bad_label", CodeClass::Static},
	{Code::BindReused, "bind_reused", CodeClass::Static},
	{Code::UnknownOp, "unknown_op", CodeClass::Static},
	{Code::Arity, "arity", CodeClass::Static},
	{Code::UnknownSymbol, "unknown_symbol", CodeClass::Static},
	{Code::UnknownLabel, "unknown_label", CodeClass::Static},
	{Code::DependencyFailed, "dependency_failed", CodeClass::Static},
	{Code::TypeMismatch, "type_mismatch", CodeClass::Static},
	{Code::CurrencyMismatch, "currency_mismatch", CodeClass::Static},
	{Code::BasisMismatch, "basis_mismatch", CodeClass::Static},
	{Code::PerPeriod, "per_period", CodeClass::Static},
	{Code::UnitRequired, "unit_required", CodeClass::Static},
	{Code::CalendarRequired, "calendar_required", CodeClass::Static},
	{Code::UnknownCalendar, "unknown_calendar", CodeClass::Static},
	{Code::RuleConflict, "rule_conflict", CodeClass::Static},
	{Code::DefinitionNotFound, "definition_not_found", CodeClass::Static},
	{Code::RuleUnknown, "rule_unknown", CodeClass::Static},
	{Code::UnsafeLiteral, "unsafe_literal", CodeClass::Static},
	{Code::ConstantNotInMaterial, "constant_not_in_material", CodeClass::Static},
	{Code::ConstantOnly, "constant_only", CodeClass::Static},
	{Code::ConstantDepth, "constant_depth", CodeClass::Static},
	{Code::TooManyConstants, "too_many_constants", CodeClass::Static},
	{Code::SameOperand, "same_operand", CodeClass::Static},
	{Code::RoundedObservation, "rounded_observation", CodeClass::Static},
	{Code::ExternalOperand, "external_operand", CodeClass::Static},
	{Code::GroupKeyType, "group_key_type", CodeClass::Static},
	{Code::QueryLiteral, "query_literal", CodeClass::Static},
	{Code::DepthExceeded, "depth_exceeded", CodeClass::Static},
	{Code::TooManyOps, "too_many_ops", CodeClass::Static},
	{Code::RoundBudget, "round_budget", CodeClass::Static},
	{Code::TaskBudget, "task_budget", CodeClass::Static},
	{Code::DisclosureBudget, "disclosure_budget", CodeClass::Static},
	{Code::NotEvaluable, "not_evaluable", CodeClass::Static},
	{Code::AmbiguousOperand, "ambiguous_operand", CodeClass::Static},
	{Code::UnknownColumn, "unknown_column", CodeClass::Static},
	{Code::BadPredicate, "bad_predicate", CodeClass::Static},
	{Code::TableTooLarge, "table_too_large", CodeClass::Static},
	{Code::CrossTable, "cross_table", CodeClass::Static},
	{Code::WithheldByPolicy, "withheld_by_policy", CodeClass::Static},
	{Code::PrecisionInsufficient, "precision_insufficient", CodeClass::Observation},
	{Code::DivisionByZero, "division_by_zero", CodeClass::Observation},
	{Code::EmptySet, "empty_set", CodeClass::Observation},
	{Code::EvaluationFailed, "evaluation_failed", CodeClass::Observation},
	{Code::BoundedOperand, "bounded_operand", CodeClass::Static},
	{Code::MaterialChanged, "material_changed", CodeClass::Static},
}};

std::string period_word(semantic::RateBasis basis) {
	switch (basis) {
		case semantic::RateBasis::PerDay: return "day";
		case semantic::RateBasis::PerMonth: return "month";
		case semantic::RateBasis::PerYear: return "year";
		case semantic::RateBasis::None: break;
	}
	return {};
}

std::string unit_words(semantic::DurationUnit unit) {
	switch (unit) {
		case semantic::DurationUnit::Day: return "days";
		case semantic::DurationUnit::Week: return "weeks";
		case semantic::DurationUnit::Month: return "months";
		case semantic::DurationUnit::Year: return "years";
		case semantic::DurationUnit::BusinessDay: return "business days";
		case semantic::DurationUnit::Quarter: return "quarters";
		case semantic::DurationUnit::Hour: return "hours";
		case semantic::DurationUnit::Minute: return "minutes";
	}
	return "days";
}

struct Registry {
	std::mutex mutex;
	std::vector<std::string> families;
	std::map<std::string, OpEntry, std::less<>> entries;
};

Registry &registry() {
	static Registry instance;
	return instance;
}

} // namespace

const char *to_string(Code code) {
	for (const CodeName &entry : kCodes)
		if (entry.code == code) return entry.name;
	return "";
}

std::optional<Code> code_from_string(std::string_view text) {
	if (text.empty()) return std::nullopt;
	for (const CodeName &entry : kCodes)
		if (text == entry.name) return entry.code;
	return std::nullopt;
}

CodeClass class_of(Code code) {
	for (const CodeName &entry : kCodes)
		if (entry.code == code) return entry.kind;
	return CodeClass::Static;
}

const char *to_string(OpStatus status) {
	switch (status) {
		case OpStatus::Ok: return "ok";
		case OpStatus::Bounded: return "bounded";
		case OpStatus::Unknown: return "unknown";
		case OpStatus::Error: return "error";
		case OpStatus::Refused: return "refused";
		case OpStatus::NotServed: return "not_served";
	}
	return "error";
}

const char *to_string(Carrier carrier) {
	switch (carrier) {
		case Carrier::FinalField: return "final_field";
		case Carrier::NativeTool: return "native_tool";
		case Carrier::TextualTool: return "textual_tool";
	}
	return "final_field";
}

const char *to_string(Grounding grounding) {
	switch (grounding) {
		case Grounding::Material: return "material";
		case Grounding::Safe: return "safe";
		case Grounding::Request: return "request";
		case Grounding::Lawyer: return "lawyer";
	}
	return "safe";
}

const char *to_string(PayloadSegment::Kind kind) {
	switch (kind) {
		case PayloadSegment::Kind::Source: return "source";
		case PayloadSegment::Kind::Request: return "request";
		case PayloadSegment::Kind::LawyerAnswer: return "lawyer_answer";
		case PayloadSegment::Kind::ModelAuthored: return "model_authored";
		case PayloadSegment::Kind::LmBlock: return "lm_block";
		case PayloadSegment::Kind::Control: return "control";
	}
	return "control";
}

std::string written(const ArgType &type) {
	switch (type.kind) {
		case TypeKind::Money: {
			std::string inside = type.currency.empty() ? std::string("?") : type.currency;
			const std::string period = period_word(type.basis);
			if (!period.empty()) inside += "/" + period;
			return "MONEY(" + inside + ")";
		}
		case TypeKind::Number: return "NUMBER";
		case TypeKind::Count: return "COUNT";
		case TypeKind::Percentage: return "PERCENTAGE";
		case TypeKind::Rate: {
			const std::string period = period_word(type.basis);
			return period.empty() ? std::string("RATE") : "RATE(" + period + ")";
		}
		case TypeKind::Date: return type.precision.empty() ? std::string("DATE") : "DATE(" + type.precision + ")";
		case TypeKind::Duration: return "DURATION(" + unit_words(type.unit) + ")";
		case TypeKind::Boolean: return "BOOLEAN";
		case TypeKind::Order: return "ORDER";
		case TypeKind::Sign: return "SIGN";
		case TypeKind::Schedule: return "SCHEDULE(" + type.key + "\xE2\x86\x92" + type.element + ")";
		case TypeKind::Entity: return type.entity.empty() ? std::string("ENTITY") : "ENTITY(" + type.entity + ")";
		case TypeKind::Table: return "TABLE";
		case TypeKind::Column: return "COLUMN(" + type.element + ")";
		case TypeKind::Row: return type.table.empty() ? std::string("ROW") : "ROW([[" + type.table + "]])";
		case TypeKind::RowSet: return type.table.empty() ? std::string("ROW_SET") : "ROW_SET([[" + type.table + "]])";
		case TypeKind::GroupSet: return "GROUP_SET(" + type.key + ")";
		case TypeKind::Calendar: return "CALENDAR";
		case TypeKind::Word: return "WORD";
	}
	return "NUMBER";
}

void register_family(std::string_view family, std::vector<OpEntry> entries) {
	Registry &shared = registry();
	std::lock_guard<std::mutex> lock(shared.mutex);
	bool known = false;
	for (const std::string &name : shared.families) known = known || name == family;
	if (!known) shared.families.emplace_back(family);
	for (OpEntry &entry : entries) {
		std::string name = entry.name;
		shared.entries[std::move(name)] = std::move(entry);
	}
}

const OpEntry *find_op(std::string_view op) {
	Registry &shared = registry();
	std::lock_guard<std::mutex> lock(shared.mutex);
	const auto found = shared.entries.find(op);
	return found == shared.entries.end() ? nullptr : &found->second;
}

std::vector<std::string> registered_families() {
	Registry &shared = registry();
	std::lock_guard<std::mutex> lock(shared.mutex);
	return shared.families;
}

void ensure_families_registered() {
	static std::once_flag once;
	std::call_once(once, [] {
		register_arithmetic_family();
		register_temporal_family();
		register_table_family();
	});
}

} // namespace singularity::protected_api::papinian::compute
