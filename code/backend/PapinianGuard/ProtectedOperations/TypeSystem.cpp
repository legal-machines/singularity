#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"

// The typing table's dispatch (DESIGN4 §2.5): each op's typer is its family's
// (register_family) - arithmetic, comparison, rate and logic rows in
// ArithmeticOps.cpp, temporal rows in TemporalOps.cpp, table rows in
// TableOps.cpp - and the static type of an operand, which is read from what a
// value IS (its kind, currency, period, unit, precision) and never from how
// large it is (DESIGN4 §8.4 L1).
namespace singularity::protected_api::papinian::operations {

namespace {

namespace temporal = singularity::context::temporal;

std::string precision_word(const temporal::TemporalValue &date) {
	switch (date.precision) {
		case temporal::Precision::Day: return date.first_day == date.last_day ? "day" : "range";
		case temporal::Precision::Month: return "month";
		case temporal::Precision::Quarter: return "quarter";
		case temporal::Precision::Year: return "year";
	}
	return "range";
}

} // namespace

compute::ArgType type_of_value(const semantic::Value &value) {
	compute::ArgType type;
	switch (value.kind) {
		case semantic::Kind::Money:
			type.kind = compute::TypeKind::Money;
			type.currency = value.currency.empty() ? std::string("?") : value.currency;
			type.basis = value.basis;
			break;
		case semantic::Kind::Number: type.kind = compute::TypeKind::Number; break;
		case semantic::Kind::Count: type.kind = compute::TypeKind::Count; break;
		case semantic::Kind::Percentage:
			type.kind = value.basis == semantic::RateBasis::None ? compute::TypeKind::Percentage : compute::TypeKind::Rate;
			type.basis = value.basis;
			break;
		case semantic::Kind::Duration:
			type.kind = compute::TypeKind::Duration;
			type.unit = value.unit;
			break;
		case semantic::Kind::Date:
			type.kind = compute::TypeKind::Date;
			type.precision = precision_word(value.date);
			break;
		case semantic::Kind::DayOfYear:
			type.kind = compute::TypeKind::Date;
			type.precision = "day of year";
			break;
		case semantic::Kind::Boolean: type.kind = compute::TypeKind::Boolean; break;
	}
	return type;
}

std::optional<compute::ResultType> type_of(std::string_view op, const std::vector<compute::ArgType> &arguments,
		const compute::OpOptions &options, compute::TypeError &error) {
	compute::ensure_families_registered();
	const compute::OpEntry *entry = compute::find_op(op);
	if (entry == nullptr || entry->typer == nullptr) {
		error.code = compute::Code::UnknownOp;
		return std::nullopt;
	}
	return entry->typer(arguments, options, error);
}

compute::ArgType arg_type_of(const RequestMappingEntry &row) {
	if (row.value) return type_of_value(*row.value);
	compute::ArgType type;
	type.kind = compute::TypeKind::Entity;
	if (!row.type) return type;
	switch (*row.type) {
		case EntityType::Person:
		case EntityType::Judge: type.entity = "person"; break;
		case EntityType::Organisation:
		case EntityType::LegalEntity:
		case EntityType::Court: type.entity = "organisation"; break;
		case EntityType::Location:
		case EntityType::Address: type.entity = "place"; break;
		default: type.entity = "identifier"; break;
	}
	return type;
}

} // namespace singularity::protected_api::papinian::operations
