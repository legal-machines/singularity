#pragma once

#include <optional>
#include <string>
#include <vector>

namespace singularity::protected_api::papinian {

enum class EntityType {
  Person,
  LegalEntity,
  Organisation,
  Location,
  Address,
  Email,
  Phone,
  BankAccount,
  TaxId,
  CaseNumber,
  ContractIdentifier,
  Court,
  Judge,
  Date,
  MoneyAmount,
  // A rate or a share: "3%", "…", "0.001 (shown as a
  // percentage)". Typed apart from money so a model is never told a rate is
  // an amount; Standard protects both under its monetary-amounts category.
  Percentage,
  // A protected plain quantity: an operand Singularity worked out a statement
  // with, or a value it derived for the model without showing it.
  Number,
};

const std::vector<EntityType> &all_entity_types();
const char *to_string(EntityType type);

// Normalizes a third-party model label and maps it to Papinian Guard's stable
// taxonomy. Unknown labels are ignored rather than leaking model vocabulary
// into the rest of Singularity.
std::optional<EntityType> entity_type_from_model_label(const std::string &label);

// Higher values win semantic overlaps where the model emits both a general
// and a legal-specific label for the same text (for example JUDGE + PERSON).
int semantic_specificity(EntityType type);

} // namespace singularity::protected_api::papinian
