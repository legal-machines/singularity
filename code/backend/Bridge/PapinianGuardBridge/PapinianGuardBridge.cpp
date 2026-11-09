#include "Bridge/PapinianGuardBridge/PapinianGuardBridge.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "PapinianGuard/Taxonomy/Taxonomy.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Privacy/PrivacyActions/PrivacyActions.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace singularity::bridge::papinian_guard_bridge {
namespace {

using Json = nlohmann::json;

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

Json serialize_policy(const protected_api::Policy &policy) {
  const auto effective = protected_api::effective_categories(policy);
  Json categories = Json::array();
  for (const auto category : protected_api::all_categories()) {
    categories.push_back({{"id", protected_api::to_string(category)},
                          {"enabled", std::find(effective.begin(), effective.end(), category) !=
                                          effective.end()}});
  }
  return Json{{"mode", protected_api::to_string(policy.mode)},
              {"categories", std::move(categories)},
              {"customTerms", policy.custom_terms}};
}

bool is_known_category(const std::string &value) {
  return std::any_of(protected_api::all_categories().begin(),
                     protected_api::all_categories().end(),
                     [&](const protected_api::Category category) {
                       return value == protected_api::to_string(category);
                     });
}

} // namespace

nlohmann::json get_policy(const nlohmann::json &) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  return serialize_policy(platform::load_app_settings().local_node_setup.protected_api_policy);
}

nlohmann::json save_policy(const nlohmann::json &params) {
  if (!params.is_object()) throw std::runtime_error("Protected API policy must be an object.");
  if (!params.contains("mode") || !params.at("mode").is_string())
    throw std::runtime_error("Protected API mode is required.");
  const auto mode_value = params.at("mode").get<std::string>();
  if (mode_value != "standard" && mode_value != "off" && mode_value != "custom")
    throw std::runtime_error("Unknown Protected API mode.");

  protected_api::Policy policy;
  policy.mode = protected_api::mode_from_string(mode_value);
  if (params.contains("enabledCategories")) {
    const auto &categories = params.at("enabledCategories");
    if (!categories.is_array())
      throw std::runtime_error("Protected API enabledCategories must be an array.");
    std::unordered_set<std::string> seen;
    for (const auto &category : categories) {
      if (!category.is_string())
        throw std::runtime_error("Protected API category identifiers must be strings.");
      const auto value = category.get<std::string>();
      if (!is_known_category(value))
        throw std::runtime_error("Unknown Protected API category: " + value);
      if (seen.insert(value).second)
        policy.custom_categories.push_back(protected_api::category_from_string(value));
    }
  }
  if (params.contains("customTerms")) {
    const auto &terms = params.at("customTerms");
    if (!terms.is_array()) throw std::runtime_error("Protected API customTerms must be an array.");
    std::unordered_set<std::string> seen;
    for (const auto &term : terms) {
      if (!term.is_string()) throw std::runtime_error("Protected API custom terms must be strings.");
      const auto value = trim(term.get<std::string>());
      if (!value.empty() && seen.insert(value).second) policy.custom_terms.push_back(value);
    }
  }
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  settings.local_node_setup.protected_api_policy = policy;
  platform::save_app_settings(settings);
  return serialize_policy(settings.local_node_setup.protected_api_policy);
}

nlohmann::json preview(const nlohmann::json &params) {
  std::string text;
  if (params.contains("text") && params.at("text").is_string())
    text = params.at("text").get<std::string>();
  if (trim(text).empty()) {
    text = "Contact John Smith at john.smith@example.com or +1 415 555 0138 regarding the "
           "$12,400,000 payment (ref A/2024/00931) - details at https://example.com/matter/412.";
  }
  protected_api::Policy policy;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    policy = platform::load_app_settings().local_node_setup.protected_api_policy;
  }
  routing::require_papinian_model_ready(policy);
  const auto &guard = routing::production_guard();
  const auto protection = guard.protect(text, policy, routing::generate_request_namespace());
  const auto restoration = guard.restore(protection.protected_text, protection);
  return Json{{"original", text}, {"transformed", protection.protected_text},
              {"restored", restoration.text}};
}

nlohmann::json preview_passage(const nlohmann::json &params) {
  const std::string projectId = params.at("projectId").get<std::string>();
  std::string text = params.at("text").get<std::string>();
  // A passage, not a document: a cited chunk is a few thousand characters.
  constexpr std::size_t kMaximumPassageBytes = 64 * 1024;
  if (text.size() > kMaximumPassageBytes) throw std::invalid_argument("This passage is too long to preview.");

  protected_api::Policy policy;
  protected_api::papinian::OperationProtectedSymbolTable symbols;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    policy = platform::load_app_settings().local_node_setup.protected_api_policy;
    const auto workspace_path = node::require_active_workspace_path();
    auto matter = matters::load_matter(workspace_path, projectId);
    if (!matter) throw std::runtime_error("This Project no longer exists.");
    // The same Project identities a real request protects with.
    symbols.project_entity_symbols = privacy::project_entity_symbols(*matter);
    for (const privacy::ProjectEntity &entity : privacy::list_active_entities(*matter)) {
      symbols.project_entity_aliases[entity.id] = privacy::protection_values(*matter, entity);
      symbols.project_entity_types[entity.id] = entity.category;
      if (!entity.locked_aliases.empty())
        symbols.project_entity_curated_aliases[entity.id] =
            std::set<std::string>(entity.locked_aliases.begin(), entity.locked_aliases.end());
    }
  }
  if (policy.mode == protected_api::Mode::Off || trim(text).empty()) {
    return Json{{"protectionMode", "off"}, {"replacements", Json::array()}, {"protectedText", text}};
  }
  routing::require_papinian_model_ready(policy);
  const auto protection = routing::production_guard().protect(
      text, policy, routing::generate_request_namespace(), &symbols);

  Json replacements = Json::array();
  for (const auto &entry : protection.mapping) {
    Json forms = Json::array();
    for (const std::string &form : entry.every_form()) {
      if (!form.empty()) forms.push_back(form);
    }
    replacements.push_back(Json{
        {"forms", std::move(forms)},
        {"placeholder", entry.placeholder},
        {"category", entry.type ? protected_api::papinian::to_string(*entry.type) : "other"}});
  }
  // The passage as it would go out, each symbol where its words stood.
  return Json{{"protectionMode", "on"}, {"replacements", std::move(replacements)},
              {"protectedText", protection.protected_text}};
}

nlohmann::json preview_document(const nlohmann::json &params) {
  const auto preview = privacy_actions::preview_document_protection(params.at("projectId").get<std::string>(),
                                                                     params.at("documentId").get<std::string>());
  Json passages = Json::array();
  for (const auto &passage : preview.passages) {
    passages.push_back(Json{{"text", passage.text},
                            {"protectedText", passage.protected_text},
                            {"page", passage.page ? Json(*passage.page) : Json(nullptr)},
                            {"heading", passage.heading ? Json(*passage.heading) : Json(nullptr)},
                            {"withheld", passage.is_withheld}});
  }
  Json replacements = Json::array();
  for (const auto &replacement : preview.replacements)
    replacements.push_back(Json{{"placeholder", replacement.placeholder}, {"category", replacement.category},
                                {"forms", replacement.forms}});
  return Json{{"protectionMode", preview.is_protected ? "on" : "off"},
              {"isRead", preview.is_read},
              {"name", preview.name},
              {"protectedName", preview.protected_name},
              {"passages", std::move(passages)},
              {"passageCount", preview.passage_count},
              {"replacements", std::move(replacements)}};
}

} // namespace singularity::bridge::papinian_guard_bridge
