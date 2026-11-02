#pragma once

#include <optional>
#include <string>
#include <vector>

namespace singularity::protected_api {

// A deterministic category Protected API can reliably find and replace on
// its own, with no cloud calls and no guesswork - see detector.hpp.
// Deliberately narrow: person/company/location names are handled by Papinian
// NER (protected_api/papinian/), which runs semantically rather than by
// pattern match and is not gated per-category here - it is always active
// whenever Policy::mode != Off (see PapinianGuard::protect in
// PapinianGuard/PapinianGuardService/PapinianGuardService.cpp), so it is not offered as a toggleable
// category (see docs/ARCHITECTURE.md and the onboarding Protected API step).
enum class Category {
  // The financial facts of a matter: amounts of money, the financial ratios a
  // company keeps confidential, and a party's holding - its shares, units or
  // membership interests ("Seller | Shares | 3,600"), a financial fact about
  // that party typed as the plain quantity it is. Contract terms (a threshold,
  // a period, a rate the contract sets) are not facts of this kind.
  MonetaryAmounts,
  Emails,
  PhoneNumbers,
  Urls,
  Identifiers,
};

// Every category this build knows how to detect, in the fixed priority
// order overlapping matches are resolved by (see detector.cpp). Custom
// terms (Policy::custom_terms) always take priority over all of these.
const std::vector<Category> &all_categories();

const char *to_string(Category category);
Category category_from_string(const std::string &value);

enum class Mode {
  // Every built-in category is active; no per-category picking.
  Standard,
  // Protected API is disabled - requests to external models carry the
  // original text unchanged.
  Off,
  // Exactly the categories in Policy::custom_categories are active.
  Custom,
};

const char *to_string(Mode mode);
Mode mode_from_string(const std::string &value);

// Whether a relation Singularity works out over protected facts may itself be
// confidential. Hiding the amounts does not hide everything: "[[Org1]] owes
// more than [[Org2]]" can be the secret. Raw-value sensitivity (Category) and
// relation sensitivity are separate questions, and both are recorded for every
// relation that leaves (PrivacyMetadata.hpp: OutboundRelation).
enum class RelationSensitivity { Ordinary, Sensitive };

const char *to_string(RelationSensitivity sensitivity);

struct RelationPolicy {
  // Whether a relation classified Sensitive is held back. Standard: false - it
  // is classified and recorded, then sent when the task justifies it, which is
  // the current minimum-sufficient behaviour.
  bool withhold_sensitive = false;
  // Relation kinds (closed names, ProtectedRelations / ProtectedOperations)
  // classified Sensitive by policy whatever their operands.
  std::vector<std::string> sensitive_kinds;
  // Whether comparing, ordering, adding or subtracting quantities that belong
  // to two different parties is Sensitive. A classification; under Standard it
  // does not withhold anything by itself.
  bool party_comparisons_sensitive = true;
};

struct Policy {
  Mode mode = Mode::Standard;
  // Only consulted when mode == Custom.
  std::vector<Category> custom_categories;
  // Terms the user has explicitly asked to always replace when Protected
  // API is active. Consulted whenever mode != Off - Off means nothing is
  // transformed at all, custom terms included, so turning protection off
  // is unambiguous.
  std::vector<std::string> custom_terms;
  RelationPolicy relations;
};

// What a relation says, as far as its sensitivity goes: its kind, the category
// of each operand ("MONEY_AMOUNT", "PERSON"), and the party symbol each operand
// belongs to where the material ties it to one (the Org or Person symbol in the
// same table row or sentence), or "" when none. Never a value.
struct RelationFacts {
  std::string kind;
  std::vector<std::string> operand_categories;
  std::vector<std::string> operand_parties;
};

// Sensitive when the policy names the kind; when it relates facts of two
// different parties in any way - an order, an equality, arithmetic, a period,
// an expression over them: classified by what it reveals, never by the
// operation's name - and party comparisons are sensitive; or when it links a
// contact detail to a person.
RelationSensitivity classify_relation(const RelationFacts &facts, const RelationPolicy &policy);

// Whether the relation may leave under `policy`: false means it is withheld.
bool permit_relation(const RelationFacts &facts, const Policy &policy);

// The categories actually active for `policy`, in priority order.
std::vector<Category> effective_categories(const Policy &policy);

// `base_policy` with its mode replaced by `mode_override`, when set -
// custom_categories/custom_terms are left exactly as `base_policy` already
// has them. Used by Retry (Master Spec Sec.66) to pin the Protected API
// mode a historical task attempt actually ran under, without also freezing
// category/custom-term configuration this build has never tracked per-task
// - see matters::MatterTask::protected_api_mode and native_bridge.cpp's
// retry_matter_task(). nullopt is a no-op: returns `base_policy` unchanged,
// which is what every non-Retry request already gets.
Policy apply_mode_override(const Policy &base_policy, const std::optional<Mode> &mode_override);

} // namespace singularity::protected_api
