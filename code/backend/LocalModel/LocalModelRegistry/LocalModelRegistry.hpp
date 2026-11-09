#pragma once

#include "LocalModel/ExecutionCompatibility/ExecutionCompatibility.hpp"
#include "LocalModel/ResourceBenchmark/ResourceBenchmark.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace singularity::hardware {

// What the adaptive scheduler needs to know about a model's own architecture to
// pick an execution strategy without the scheduler itself ever having to
// special-case a specific model. A future dense/MoE local model
// (Qwen/Llama/Mistral/Gemma-class) enters through this same generic shape - a
// new registration with these fields filled in truthfully, never a scheduler/UI
// rewrite. Every field is a real, known fact about the model's own
// package/runtime - never a benchmarked or estimated figure, and every optional
// field that does not apply to a given architecture_kind is left unset rather
// than guessed.
enum class ModelArchitectureKind {
  // The honest default: no registration has classified this model's
  // architecture (yet, or at all - an unrecognized package format). A
  // scheduler must treat Unknown as "cannot safely reason about this
  // model's execution strategy", never silently fall back to any other
  // kind - see assess_compatibility()'s own model-name-blind contract.
  Unknown,
  // A non-generative encoder used for extraction/classification (e.g.
  // Papinian's GLiNER2 span/entity model) - no autoregressive decode, no
  // expert routing, so MoE-specific strategies never apply to it.
  EncoderExtraction,
  // A dense (non-MoE) autoregressive transformer inspected from an installed
  // artifact and executed through a generic runtime adapter.
  DenseTransformer,
  // A Mixture-of-Experts autoregressive transformer. Expert
  // residency/caching strategies apply only to this kind, and only when
  // the runtime that loads it genuinely exposes per-expert structure - see
  // ModelPackageManager/ModelLifecycle for what is actually bundled today.
  MixtureOfExperts,
};

const char *to_string(ModelArchitectureKind kind);

struct ModelExecutionProfile {
  // Defaults to Unknown, not a guessed real architecture - a registration
  // that never sets this is a bug the compatibility/UI layer can detect,
  // not a silent misclassification.
  ModelArchitectureKind architecture_kind = ModelArchitectureKind::Unknown;
  // A short, plain description of the real architecture, e.g.
  // "Encoder-based span/entity extraction (GLiNER2)".
  std::string architecture_description;
  // The numeric precision this package's own artifacts actually ship in,
  // e.g. "fp16" - read from the model's own package format, never guessed.
  std::string precision;
  // Set only when this model applies quantization beyond its base
  // precision (e.g. "int4", "int8"); std::nullopt when it does not -
  // Papinian ships plain fp16 with no separate quantization step.
  std::optional<std::string> quantization;
  // The real, known size of this model's own weights/state on disk, in
  // bytes - what the package itself weighs, not what running it costs (see
  // ResourceRequirement below for that). std::nullopt only if genuinely
  // unknown before install.
  std::optional<std::uint64_t> model_state_bytes;
  // Whether inference carries reusable state across steps that a scheduler
  // must budget for beyond the one-shot ResourceRequirement - e.g. an
  // autoregressive model's KV cache. False for a stateless single-request
  // architecture like Papinian's encoder extraction.
  bool requires_runtime_state = false;
  // Only meaningful when architecture_kind == MixtureOfExperts;
  // std::nullopt for every other kind, including every model registered
  // today.
  std::optional<int> expert_count;
  std::optional<int> active_experts_per_token;
  // This model's own real, inspected native maximum context length (e.g.
  // GGUF's context_length, read from its own package metadata) -
  // std::nullopt when genuinely undiscoverable. This is the
  // model's own advertised ceiling, NEVER feasibility-checked against
  // current machine memory - informational/display only (e.g. a Model
  // Library card). Budget/generation code must use LocalModelLiveState::
  // effective_context_tokens below instead, which is real and load-checked.
  std::optional<std::uint64_t> native_context_length;
};

// What Settings needs to introduce one local module to a user - branded,
// never the underlying third-party checkpoint/architecture name (that stays
// internal to the module's own runtime code, e.g.
// LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp's SemanticModelIdentity).
struct LocalModelDescriptor {
  // Stable, never-shown-to-users identifier - e.g. "papinian-ner".
  std::string id;
  std::string display_name;
  std::string purpose;
  // The short human-facing category shown on a Model Library card, e.g.
  // "Anonymization" - the AI Setup role this model fills, not a sentence.
  // Distinct from `purpose` above, which stays a fuller technical
  // description for Details/logging.
  std::string role;
  // The product family this model belongs to, e.g. "Papinian" - shared
  // geometric/material visual language across a family's cards without
  // collapsing every model into an identical row.
  std::string family;
  // Who provides/develops this model package, e.g. "Legal Machines" - the
  // card's "Provider identity" line. Never the upstream Hugging
  // Face/third-party checkpoint name; that stays internal to the module's
  // own runtime code, same boundary this file's top comment already draws.
  std::string developer;
  std::string version;
  // Where this module's package lives on disk, for install/disk-size
  // checks. Not necessarily where it's actually loaded from at runtime
  // (that's the module's own concern) - just what a Settings "Storage" row
  // measures.
  std::filesystem::path install_path;
  // This model's own ordered backend preference - see
  // ExecutionCompatibility.hpp's choose_execution_plan().
  std::vector<ExecutionBackend> preferred_backends;
  ResourceRequirement resource_requirement;
  ModelExecutionProfile execution_profile;
  // Whether Singularity's core functionality depends on this module (Papinian:
  // true, since Protected API fails closed without it) vs. an optional
  // module a user could run without.
  bool required = true;
};

enum class LocalModelLoadState { NotInstalled, Loading, Ready, Unloaded, Error };

struct LocalModelLiveState {
  LocalModelLoadState load_state = LocalModelLoadState::NotInstalled;
  // Populated only when load_state == Error.
  std::string load_error;
  // Populated once an execution plan has actually been chosen (Ready, or an
  // Error that happened after planning) - absent while still Loading or
  // NotInstalled.
  std::optional<ExecutionPlan> execution_plan;
  // Real, measured evidence from this module's own most recent warm-up on
  // its chosen backend, compared against that backend's own previously
  // recorded baseline - std::nullopt before a first successful warm-up in
  // this process. See LocalModel/ResourceBenchmark/ResourceBenchmark.hpp.
  std::optional<BenchmarkComparison> benchmark;
  // The real context window (in tokens) this model's current load
  // requested and was granted, after local inference's real
  // hardware-memory-feasibility checking on this machine right now - never
  // this model's own advertised/marketing maximum
  // (LocalModelDescriptor::execution_profile.native_context_length) when
  // that would not safely fit.
  // ACTUAL GRANTED CONTEXT - the final authority. Populated only
  // while this model is actually loaded (Ready); std::nullopt otherwise.
  std::optional<std::uint64_t> effective_context_tokens;
  // PLANNED / FEASIBLE CONTEXT: what local inference's planner
  // would request for this model on this machine right now, after the same
  // feasibility check a real load performs, read without loading. This is
  // what pre-load budgeting uses; effective_context_tokens wins once a load
  // is actually granted. std::nullopt when it cannot be planned yet - budget
  // resolution must treat that as genuinely unknown (ContextBudget.hpp's
  // resolve_local_context_window_tokens()), never substitute a constant.
  std::optional<std::uint64_t> planned_context_tokens;
};

// The three context-capability figures of one module, kept distinct by
// construction: native (the package's maximum, informational), planned
// (feasibility-checked for this machine now, no load needed) and granted (what
// the current load actually got - the final authority, present only while the
// module is Ready).
struct ModelContextCapability {
  std::optional<std::uint64_t> native_context_tokens;
  std::optional<std::uint64_t> planned_context_tokens;
  std::optional<std::uint64_t> granted_context_tokens;
};

ModelContextCapability resolve_model_context_capability(const LocalModelDescriptor &descriptor,
                                                        const LocalModelLiveState &live);

struct LocalModelStatus {
  LocalModelDescriptor descriptor;
  bool installed = false;
  std::uint64_t disk_bytes = 0;
  LocalModelLiveState live;
  // Pre-install-aware, so a Model Library card can show "Recommended for
  // this computer" / "Will run, but slowly" / "Not recommended" even before
  // the model is ever downloaded - see ExecutionCompatibility.hpp's
  // assess_compatibility().
  CompatibilityAssessment compatibility;
  // What loading this specific, already-installed model right now would
  // cost against the live runtime memory budget - see ExecutionCompatibility.hpp's
  // assess_load_impact(). Distinct from `compatibility` above, which never
  // looks at currently-available memory.
  LoadImpact load_impact;
};

// Sums regular-file sizes under `path` (non-recursive-symlink-following,
// matching how a package directory is actually laid out - see
// LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp's package verification, which
// itself rejects symlinks). Returns 0 for a missing or non-directory path
// rather than throwing - a model that isn't installed simply has no size.
std::uint64_t directory_size_bytes(const std::filesystem::path &path);

// A generic catalogue of installed local AI modules. Deliberately not
// specific to Papinian: the production singleton (see routing/
// inference_service.cpp) registers Papinian plus valid installed GGUF
// artifacts; the container itself supports any number - see
// code/backend/LocalModel/LocalModelRegistry/LocalModelRegistry.test.cpp, which registers several
// synthetic descriptors to prove that.
class LocalModelRegistry {
public:
  // Called on demand (not cached) so each list_status() reflects the
  // module's current load state/execution plan, e.g. while Papinian's
  // warm-up is still in flight.
  using LiveStateProvider = std::function<LocalModelLiveState()>;
  // The same, handed the hardware snapshot the registry already detected for
  // this status query: a module whose live state includes a real planning
  // pass (planned context) plans against that ONE snapshot, so every figure in
  // one status answer describes the same machine state.
  using HardwareAwareLiveStateProvider = std::function<LocalModelLiveState(const HardwareCapabilities &)>;
  // Unloads this module if it is currently loaded; a no-op if it is not.
  // Optional - only needed for a module release_reclaimable_memory() below
  // may actually stop under severe memory pressure. Returns false when the
  // module is answering right now and was therefore not stopped, so a caller
  // never reports memory it did not free.
  using UnloadCallback = std::function<bool()>;

  // Shared by every thread that lists, loads, installs or releases models,
  // so each call holds the registry's lock only while it reads or changes
  // its entries. A provider or callback runs outside it, on the entries as
  // they stood when the call began, so it may call back into the registry
  // (or into a runtime that does).
  LocalModelRegistry() = default;
  LocalModelRegistry(LocalModelRegistry &&other) noexcept;
  LocalModelRegistry(const LocalModelRegistry &) = delete;
  LocalModelRegistry &operator=(const LocalModelRegistry &) = delete;
  LocalModelRegistry &operator=(LocalModelRegistry &&) = delete;

  void register_model(LocalModelDescriptor descriptor, LiveStateProvider live_state_provider,
                      UnloadCallback unload_callback = nullptr);
  void register_model(LocalModelDescriptor descriptor, HardwareAwareLiveStateProvider live_state_provider,
                      UnloadCallback unload_callback = nullptr);
  void register_model(LocalModelDescriptor descriptor, std::nullptr_t,
                      UnloadCallback unload_callback = nullptr);

	// Inserts a newly discovered/catalogued model or replaces the existing
	// entry with the same stable id. Runtime discovery is repeatable after an
	// install/remove operation, so refreshing must update one identity rather
	// than append duplicate cards and stale callbacks.
	void upsert_model(LocalModelDescriptor descriptor, LiveStateProvider live_state_provider,
	                  UnloadCallback unload_callback = nullptr);
	void upsert_model(LocalModelDescriptor descriptor, HardwareAwareLiveStateProvider live_state_provider,
	                  UnloadCallback unload_callback = nullptr);
	void upsert_model(LocalModelDescriptor descriptor, std::nullptr_t,
	                  UnloadCallback unload_callback = nullptr);

	// An internal in-memory catalogue primitive, not the product definition
	// of "uninstall": list_status() already derives `installed` from a live
	// std::filesystem::exists(install_path) check on every call, so deleting
	// an installed artifact's files and leaving its descriptor registered is
	// normally enough on its own for the model to correctly revert to
	// Available/Install (see Bridge/LocalModelBridge/LocalModelBridge.cpp's
	// remove()) - a catalogue-backed model must never be unregistered as
	// part of a successful uninstall. This method exists only for the
	// narrower case where a registered entry has no catalogue/install
	// identity to preserve at all (an ad-hoc discovered artifact whose own
	// existence in the registry is 1:1 with the file's existence - see
	// LocalInference/GenerativeModelService's discover_and_register());
	// there, leaving a ghost entry behind would
	// show an "Install" action that can never actually work. A required
	// module (Papinian: core Protected API dependency) must never be
	// unregistered - the caller checks LocalModelDescriptor::required
	// itself before ever reaching this call. A no-op for an unknown id.
	void unregister_model(const std::string& id);

  std::vector<LocalModelDescriptor> descriptors() const;
  std::vector<LocalModelStatus> list_status() const;
  // One module's status by id - exactly what list_status() would report for
  // it - or std::nullopt for an unknown id. Detects hardware once and asks
  // only that module's own provider: its live state may include a real
  // planning pass, which a query by id must never pay for every module.
  std::optional<LocalModelStatus> status_for(const std::string &model_id) const;

  // The emergency "severe-state memory release pass" - stops every
  // registered, currently-Ready, non-required module to reclaim memory. A
  // required module (Papinian: core Protected API dependency) is never
  // silently stopped here, under the fail-closed invariant: a
  // severe failure stops the affected local AI and preserves Project state.
  // Returns the ids of modules actually released.
  std::vector<std::string> release_reclaimable_memory() const;

private:
  struct Entry {
    LocalModelDescriptor descriptor;
    HardwareAwareLiveStateProvider live_state_provider;
    UnloadCallback unload_callback;
  };
  std::vector<Entry> snapshot() const;
  static HardwareAwareLiveStateProvider hardware_aware(LiveStateProvider provider);
  static LocalModelStatus status_for_entry(const Entry &entry, const HardwareCapabilities &hardware);

  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
};

} // namespace singularity::hardware
