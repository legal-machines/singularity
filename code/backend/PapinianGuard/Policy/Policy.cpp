#include "PapinianGuard/Policy/Policy.hpp"

#include <algorithm>

namespace singularity::protected_api {

const std::vector<Category> &all_categories() {
  static const std::vector<Category> categories{
      Category::MonetaryAmounts,
      Category::Emails,
      Category::PhoneNumbers,
      Category::Urls,
      Category::Identifiers,
  };
  return categories;
}

const char *to_string(Category category) {
  switch (category) {
    case Category::MonetaryAmounts:
      return "monetary_amounts";
    case Category::Emails:
      return "emails";
    case Category::PhoneNumbers:
      return "phone_numbers";
    case Category::Urls:
      return "urls";
    case Category::Identifiers:
      return "identifiers";
  }
  return "monetary_amounts";
}

Category category_from_string(const std::string &value) {
  for (const auto category : all_categories()) {
    if (value == to_string(category)) {
      return category;
    }
  }
  return Category::MonetaryAmounts;
}

const char *to_string(Mode mode) {
  switch (mode) {
    case Mode::Standard:
      return "standard";
    case Mode::Off:
      return "off";
    case Mode::Custom:
      return "custom";
  }
  return "standard";
}

Mode mode_from_string(const std::string &value) {
  if (value == "off") return Mode::Off;
  if (value == "custom") return Mode::Custom;
  return Mode::Standard;
}

std::vector<Category> effective_categories(const Policy &policy) {
  switch (policy.mode) {
    case Mode::Standard:
      return all_categories();
    case Mode::Off:
      return {};
    case Mode::Custom: {
      // Preserve all_categories()'s fixed priority order regardless of the
      // order custom_categories happens to list them in.
      std::vector<Category> result;
      for (const auto category : all_categories()) {
        if (std::find(policy.custom_categories.begin(), policy.custom_categories.end(), category) !=
            policy.custom_categories.end()) {
          result.push_back(category);
        }
      }
      return result;
    }
  }
  return {};
}

const char *to_string(RelationSensitivity sensitivity) {
  return sensitivity == RelationSensitivity::Sensitive ? "sensitive" : "ordinary";
}

namespace {

// The relation kinds that say nothing about how their operands' values stand
// to each other: a contact detail's owner, a row a sheet hides. Every other
// kind - an order, an equality, a sum, a difference, a ratio, a rate, a
// product, a period between two dates, a deadline, an expression the model
// wrote over several facts - relates what it is given, whatever its name; what
// a relation reveals, not what the operation is called, is what a party
// comparison classifies.
bool relates_nothing(const std::string &kind) {
  static const std::vector<std::string> kinds{"contact_owner", "hidden_row"};
  return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

} // namespace

RelationSensitivity classify_relation(const RelationFacts &facts, const RelationPolicy &policy) {
  if (std::find(policy.sensitive_kinds.begin(), policy.sensitive_kinds.end(), facts.kind) != policy.sensitive_kinds.end())
    return RelationSensitivity::Sensitive;
  if (policy.party_comparisons_sensitive && !relates_nothing(facts.kind)) {
    std::vector<std::string> parties;
    for (const std::string &party : facts.operand_parties)
      if (!party.empty() && std::find(parties.begin(), parties.end(), party) == parties.end()) parties.push_back(party);
    if (parties.size() >= 2) return RelationSensitivity::Sensitive;
  }
  if (facts.kind == "contact_owner"
      && std::find(facts.operand_categories.begin(), facts.operand_categories.end(), "PERSON")
          != facts.operand_categories.end())
    return RelationSensitivity::Sensitive;
  return RelationSensitivity::Ordinary;
}

bool permit_relation(const RelationFacts &facts, const Policy &policy) {
  if (policy.mode == Mode::Off) return true;
  return !(policy.relations.withhold_sensitive
      && classify_relation(facts, policy.relations) == RelationSensitivity::Sensitive);
}

Policy apply_mode_override(const Policy &base_policy, const std::optional<Mode> &mode_override) {
  if (!mode_override) return base_policy;
  Policy overridden = base_policy;
  overridden.mode = *mode_override;
  return overridden;
}

} // namespace singularity::protected_api
