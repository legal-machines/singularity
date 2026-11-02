#include "PapinianGuard/Taxonomy/Taxonomy.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace singularity::protected_api::papinian {
namespace {

std::string normalize_label(const std::string &label) {
  std::string normalized;
  normalized.reserve(label.size());
  bool prior_separator = false;
  for (const unsigned char character : label) {
    if (std::isalnum(character)) {
      normalized.push_back(static_cast<char>(std::tolower(character)));
      prior_separator = false;
    } else if (!normalized.empty() && !prior_separator) {
      normalized.push_back('_');
      prior_separator = true;
    }
  }
  while (!normalized.empty() && normalized.back() == '_') normalized.pop_back();
  return normalized;
}

bool one_of(const std::string &value, std::initializer_list<const char *> choices) {
  return std::any_of(choices.begin(), choices.end(),
                     [&](const char *choice) { return value == choice; });
}

} // namespace

const std::vector<EntityType> &all_entity_types() {
  static const std::vector<EntityType> types{
      EntityType::Person,             EntityType::LegalEntity,
      EntityType::Organisation,       EntityType::Location,
      EntityType::Address,
      EntityType::Email,              EntityType::Phone,
      EntityType::BankAccount,        EntityType::TaxId,
      EntityType::CaseNumber,         EntityType::ContractIdentifier,
      EntityType::Court,              EntityType::Judge,
      EntityType::Date,               EntityType::MoneyAmount,
      EntityType::Percentage,         EntityType::Number,
  };
  return types;
}

const char *to_string(EntityType type) {
  switch (type) {
    case EntityType::Person:
      return "PERSON";
    case EntityType::LegalEntity:
      return "LEGAL_ENTITY";
    case EntityType::Organisation:
      return "ORGANISATION";
    case EntityType::Location:
      return "LOCATION";
    case EntityType::Address:
      return "ADDRESS";
    case EntityType::Email:
      return "EMAIL";
    case EntityType::Phone:
      return "PHONE";
    case EntityType::BankAccount:
      return "BANK_ACCOUNT";
    case EntityType::TaxId:
      return "TAX_ID";
    case EntityType::CaseNumber:
      return "CASE_NUMBER";
    case EntityType::ContractIdentifier:
      return "CONTRACT_IDENTIFIER";
    case EntityType::Court:
      return "COURT";
    case EntityType::Judge:
      return "JUDGE";
    case EntityType::Date:
      return "DATE";
    case EntityType::MoneyAmount:
      return "MONEY_AMOUNT";
    case EntityType::Percentage:
      return "PERCENTAGE";
    case EntityType::Number:
      return "NUMBER";
  }
  return "PERSON";
}

std::optional<EntityType> entity_type_from_model_label(const std::string &label) {
  const auto value = normalize_label(label);
  if (one_of(value, {"person", "full_name", "first_name", "middle_name", "last_name",
                     "given_name", "givenname", "surname"})) {
    return EntityType::Person;
  }
  if (one_of(value, {"legal_entity", "company", "corporation", "business"})) {
    return EntityType::LegalEntity;
  }
  if (one_of(value, {"organisation", "organization", "institution"})) {
    return EntityType::Organisation;
  }
  if (one_of(value, {"location", "place", "city", "country", "region"})) {
    return EntityType::Location;
  }
  if (one_of(value, {"address", "street_address", "street", "postal_address"})) {
    return EntityType::Address;
  }
  if (value == "email" || value == "email_address") return EntityType::Email;
  if (value == "phone" || value == "phone_number" || value == "telephone_number") {
    return EntityType::Phone;
  }
  if (one_of(value, {"bank_account", "account_number", "iban", "routing_number"})) {
    return EntityType::BankAccount;
  }
  if (one_of(value, {"tax_id", "tax_number", "inn", "tin"})) return EntityType::TaxId;
  if (one_of(value, {"case_number", "court_case_number", "docket_number"})) {
    return EntityType::CaseNumber;
  }
  if (one_of(value, {"contract_identifier", "contract_number", "agreement_number"})) {
    return EntityType::ContractIdentifier;
  }
  if (one_of(value, {"court", "tribunal"})) return EntityType::Court;
  if (one_of(value, {"judge", "justice", "magistrate"})) return EntityType::Judge;
  if (one_of(value, {"date", "sensitive_date", "document_date", "transaction_date"})) {
    return EntityType::Date;
  }
  if (one_of(value, {"money", "money_amount", "monetary_amount", "amount"})) {
    return EntityType::MoneyAmount;
  }
  if (one_of(value, {"percentage", "percent", "rate", "interest_rate", "percentages"})) {
    return EntityType::Percentage;
  }
  if (one_of(value, {"number", "quantity"})) return EntityType::Number;
  return std::nullopt;
}

int semantic_specificity(EntityType type) {
  switch (type) {
    case EntityType::Judge:
    case EntityType::Court:
      return 3;
    case EntityType::LegalEntity:
      return 2;
    case EntityType::Person:
    case EntityType::Organisation:
    case EntityType::Location:
      return 1;
    default:
      return 2;
  }
}

} // namespace singularity::protected_api::papinian
