#pragma once

#include "Document/Types/DocumentPatchTypes.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

enum class ExternalSupportStatus { Unspecified, Supported, NotEstablished, NotApplicable };

inline const char *to_string(ExternalSupportStatus status) noexcept {
	switch (status) {
		case ExternalSupportStatus::Unspecified: return "unspecified";
		case ExternalSupportStatus::Supported: return "supported";
		case ExternalSupportStatus::NotEstablished: return "not_established";
		case ExternalSupportStatus::NotApplicable: return "not_applicable";
	}
	return "unspecified";
}

inline std::optional<ExternalSupportStatus> external_support_status_from_string(const std::string &value) noexcept {
	if (value == "supported") return ExternalSupportStatus::Supported;
	if (value == "not_established") return ExternalSupportStatus::NotEstablished;
	if (value == "not_applicable") return ExternalSupportStatus::NotApplicable;
	if (value == "unspecified") return ExternalSupportStatus::Unspecified;
	return std::nullopt;
}

struct DocumentMethodologyEvidence {
	std::string document_id;
	DocumentLocator locator;
	std::string document_fingerprint;
	std::string note;
};

struct DocumentActionRationale {
	std::string id;
	std::string summary;
	std::vector<DocumentLocator> project_evidence;
	std::vector<DocumentMethodologyEvidence> methodology_evidence;
	std::vector<std::string> external_evidence_ids;
	ExternalSupportStatus external_support_status = ExternalSupportStatus::Unspecified;
};

// Durable evidence for one controlled Project-document mutation performed by
// a canonical MatterTask. The ToolCallEvidence named here remains the single
// permission/action audit entry; this record adds document-version, operation,
// rationale and provenance links needed to inspect what that action changed.
struct AppliedDocumentActionEvidence {
	std::string document_id;
	int previous_version = 0;
	std::string previous_fingerprint;
	int resulting_version = 0;
	std::string resulting_fingerprint;
	std::string tool_call_evidence_id;
	std::vector<DocumentPatchOperation> operations;
	std::vector<DocumentActionRationale> rationales;
};

} // namespace singularity::documents
