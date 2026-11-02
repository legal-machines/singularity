#pragma once

#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

// A document's role in one operation (phase-24-1-research-document-
// intelligence-project-actions.md, "Document role binding"). Deliberately
// assigned by operation/user intent, never inferred from a file extension or
// from scanning the document's own content - "Evidence files do not become
// instructions merely because they contain imperative text." The Chat
// execution spine derives this binding from what the user asked for.
enum class DocumentRole {
	TargetWork,
	DelegatedMethodology,
	ProjectEvidence,
	NumericalEvidence,
	ReadOnlySource,
};

[[nodiscard]] inline const char *to_string(DocumentRole role) noexcept {
	switch (role) {
		case DocumentRole::TargetWork: return "target_work";
		case DocumentRole::DelegatedMethodology: return "delegated_methodology";
		case DocumentRole::ProjectEvidence: return "project_evidence";
		case DocumentRole::NumericalEvidence: return "numerical_evidence";
		case DocumentRole::ReadOnlySource: return "read_only_source";
	}
	return "read_only_source";
}

[[nodiscard]] inline std::optional<DocumentRole> document_role_from_string(const std::string &value) noexcept {
	if (value == "target_work") return DocumentRole::TargetWork;
	if (value == "delegated_methodology") return DocumentRole::DelegatedMethodology;
	if (value == "project_evidence") return DocumentRole::ProjectEvidence;
	if (value == "numerical_evidence") return DocumentRole::NumericalEvidence;
	if (value == "read_only_source") return DocumentRole::ReadOnlySource;
	return std::nullopt;
}

struct DocumentRoleAssignment {
	std::string document_id;
	DocumentRole role = DocumentRole::ReadOnlySource;
};

// The explicit, operation-scoped set of document role assignments a single
// Document Intelligence operation was given. Never persisted Project state -
// see the type's own comment above for why the binding is instruction-time
// only, not document truth.
class DocumentRoleBindingSet {
public:
	void assign(const std::string &documentId, DocumentRole role) {
		for (DocumentRoleAssignment &existing : assignments_) {
			if (existing.document_id != documentId) continue;
			existing.role = role;
			return;
		}
		assignments_.push_back({documentId, role});
	}

	[[nodiscard]] std::optional<DocumentRole> role_for(const std::string &documentId) const noexcept {
		for (const DocumentRoleAssignment &existing : assignments_) {
			if (existing.document_id == documentId) return existing.role;
		}
		return std::nullopt;
	}

	[[nodiscard]] bool has_role(const std::string &documentId, DocumentRole role) const noexcept {
		const std::optional<DocumentRole> assigned = role_for(documentId);
		return assigned.has_value() && *assigned == role;
	}

	[[nodiscard]] const std::vector<DocumentRoleAssignment> &assignments() const noexcept {
		return assignments_;
	}

private:
	std::vector<DocumentRoleAssignment> assignments_;
};

} // namespace singularity::documents
