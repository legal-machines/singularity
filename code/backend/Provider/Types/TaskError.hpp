#pragma once

// Master Spec Sec.65's error taxonomy, scoped to the codes actually
// reachable today from the provider/protection request path (see
// docs/alpha/phase-05-models-routing.md WP5-B). Header-only and
// dependency-free like Provider/ModelClassification/ModelClassification.hpp, so every layer
// that already includes Provider/ProviderRegistry/ProviderRegistry.hpp (matters/, routing/)
// gets it without a new link target.

#include <stdexcept>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace singularity::providers {

// Privacy-safe response-integrity evidence. Values are protocol tokens,
// categories and JSON pointers only; rejected provider plaintext and local
// restoration values are deliberately impossible to place in this record.
struct SafeResponseDiagnostics {
  std::string issue_code;
  std::string marker_candidate;
  std::string canonical_marker;
  std::string marker_type;
  std::string json_pointer;
  std::string normalization_outcome;
  std::string repair_class;
  std::size_t repair_attempts = 0;
};

enum class TaskErrorCode {
  ProviderAuthentication,
  ProviderRateLimit,
  NetworkError,
  UnsupportedModel,
  PrivacyProtectionUnavailable,
  ProtectedPayloadLeakDetected,
  ProtectedResponseInvalid,
  ResponseIncomplete,
  // The executing model's known context capacity cannot hold what the request
  // requires (context::ContextCapacityExceeded); nothing was sent.
  ContextCapacityExceeded,
  // The Project document an approved change was bound to changed before the
  // change could be applied; nothing was changed.
  TargetChanged,
  // A Project document the change was PREPARED FROM - a passage the model was
  // given when it proposed the change - changed, moved away or became
  // unreadable before the change could be applied; the proposal no longer rests
  // on the evidence it was made from, so nothing was changed.
  EvidenceChanged,
  // The lawyer rejected the proposed change; nothing was written. A decision,
  // not a fault: running the same request again would only propose the change
  // they refused, so it is never offered as a retry.
  ChangeRejected,
  // The connected provider accepted the connection but refused THIS request
  // (a 4xx that is not authentication or rate limiting) - typically the model
  // cannot do what this request needs. Distinct from Unknown because a person
  // can act on it: the same request on another model usually works, and
  // nothing about their Project is wrong.
  ProviderRejectedRequest,
  Unknown,
};

inline const char *to_string(TaskErrorCode code) noexcept {
  switch (code) {
    case TaskErrorCode::ProviderAuthentication:
      return "provider_authentication";
    case TaskErrorCode::ProviderRateLimit:
      return "provider_rate_limit";
    case TaskErrorCode::NetworkError:
      return "network_error";
    case TaskErrorCode::UnsupportedModel:
      return "unsupported_model";
    case TaskErrorCode::ProviderRejectedRequest:
      return "provider_rejected_request";
    case TaskErrorCode::PrivacyProtectionUnavailable:
      return "privacy_protection_unavailable";
    case TaskErrorCode::ProtectedPayloadLeakDetected:
      return "protected_payload_leak_detected";
    case TaskErrorCode::ProtectedResponseInvalid:
      return "protected_response_invalid";
    case TaskErrorCode::ResponseIncomplete:
      return "response_incomplete";
    case TaskErrorCode::ContextCapacityExceeded:
      return "context_capacity_exceeded";
    case TaskErrorCode::TargetChanged:
      return "target_changed";
    case TaskErrorCode::EvidenceChanged:
      return "evidence_changed";
    case TaskErrorCode::ChangeRejected:
      return "change_rejected";
    case TaskErrorCode::Unknown:
      return "unknown";
  }
  return "unknown";
}

inline TaskErrorCode task_error_code_from_string(const std::string &value) noexcept {
  if (value == "provider_authentication") return TaskErrorCode::ProviderAuthentication;
  if (value == "provider_rate_limit") return TaskErrorCode::ProviderRateLimit;
  if (value == "network_error") return TaskErrorCode::NetworkError;
  if (value == "unsupported_model") return TaskErrorCode::UnsupportedModel;
  if (value == "privacy_protection_unavailable") return TaskErrorCode::PrivacyProtectionUnavailable;
  if (value == "protected_payload_leak_detected") return TaskErrorCode::ProtectedPayloadLeakDetected;
  if (value == "protected_response_invalid") return TaskErrorCode::ProtectedResponseInvalid;
  if (value == "response_incomplete") return TaskErrorCode::ResponseIncomplete;
  if (value == "context_capacity_exceeded") return TaskErrorCode::ContextCapacityExceeded;
  if (value == "target_changed") return TaskErrorCode::TargetChanged;
  if (value == "evidence_changed") return TaskErrorCode::EvidenceChanged;
  if (value == "change_rejected") return TaskErrorCode::ChangeRejected;
  if (value == "provider_rejected_request") return TaskErrorCode::ProviderRejectedRequest;
  return TaskErrorCode::Unknown;
}

// Master Spec Sec.66 Retry: whether offering one-click "Retry" (same inputs,
// unchanged) is honest for this code, versus a failure that will just
// reproduce itself until the user changes something (a bad key, an
// unavailable model, a detected leak) - see WP5-D.
inline bool is_retryable(TaskErrorCode code) noexcept {
  switch (code) {
    case TaskErrorCode::ProviderRateLimit:
    case TaskErrorCode::NetworkError:
    case TaskErrorCode::Unknown:
      return true;
    case TaskErrorCode::ProviderAuthentication:
    case TaskErrorCode::UnsupportedModel:
    case TaskErrorCode::PrivacyProtectionUnavailable:
    case TaskErrorCode::ProtectedPayloadLeakDetected:
    case TaskErrorCode::ProtectedResponseInvalid:
    case TaskErrorCode::ResponseIncomplete:
    case TaskErrorCode::ContextCapacityExceeded:
    case TaskErrorCode::TargetChanged:
    case TaskErrorCode::EvidenceChanged:
    case TaskErrorCode::ChangeRejected:
    // Retrying the same request on the same model reproduces the same refusal;
    // what changes the outcome is choosing another model.
    case TaskErrorCode::ProviderRejectedRequest:
      return false;
  }
  return false;
}

// The words of a request turned down because the account behind the key has
// no credit left (ProviderRegistry's out_of_credit_message: "<provider> says
// this account does not have enough credit for this request. ..."): a billing
// matter, which waiting never fixes and another model on the same account may
// not either. Every layer that words a failure for a lawyer reads it here, so
// the thread says what to do instead of "the model turned it down"
// (2026-10-31: two runs of Griffin's reply stopped so, named as refusals).
inline constexpr const char *kOutOfCreditWords = " says this account does not have enough credit for this request.";

// The provider whose account has no credit left, as the message names it, or
// nullopt when `message` is no such message.
inline std::optional<std::string> out_of_credit_provider(const std::string &message) {
  const std::size_t at = message.find(kOutOfCreditWords);
  if (at == std::string::npos) return std::nullopt;
  return message.substr(0, at);
}

// Thrown instead of a plain std::runtime_error at every point in the
// provider/protection stack that already knows which stable, safe category a
// failure belongs to. `what()` stays the same plain-language, user-safe copy
// this codebase already threw before WP5-B; `code()` is the stable value
// persistence and the native contract key off instead of parsing that text.
class TaskError : public std::runtime_error {
public:
  TaskError(TaskErrorCode code, const std::string &message,
      std::optional<SafeResponseDiagnostics> diagnostics = std::nullopt)
      : std::runtime_error(message), diagnostics(std::move(diagnostics)), code_(code) {}

  [[nodiscard]] TaskErrorCode code() const noexcept { return code_; }

  std::optional<SafeResponseDiagnostics> diagnostics;

private:
  TaskErrorCode code_;
};

} // namespace singularity::providers
