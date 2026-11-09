#pragma once

#include "Matter/Types/Matter.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::work_product {

struct CreateTypeCommand {
	std::string project_id;
	std::string name;
	std::string description;
	std::optional<std::string> instructions;
	std::optional<std::string> expected_structure;
};

struct CreateWorkProductCommand {
	std::string project_id;
	std::string type_id;
	std::string title;
	std::string content;
	std::optional<std::string> document;
	std::vector<context::ResolvedSourceReference> source_refs;
	// The typed rows this work is made of, where the operation produced any.
	std::vector<matters::Finding> findings;
	std::vector<matters::Obligation> obligations;
	std::vector<matters::Event> events;
	std::vector<matters::Change> changes;
	matters::WorkProductOrigin origin;
};

struct UpdateWorkProductCommand {
	std::string project_id;
	std::string work_product_id;
	long long expected_revision = 0;
	std::string title;
	std::string content;
};

std::vector<matters::WorkProductType> list_types(const std::string& project_id);
std::vector<matters::WorkProductObject> list_objects(const std::string& project_id);
matters::WorkProductDetail get_detail(
		const std::string& project_id, const std::string& work_product_id);

matters::WorkProductType create_type(CreateTypeCommand command);
matters::WorkProductObject create(CreateWorkProductCommand command);
// An empty `type_id` or `title` is the result's own: decided here from the
// result (its document's kind and title, a change it wrote, its typed rows,
// else its request), the same whoever keeps it.
matters::WorkProductObject retain_task_result(const std::string& project_id,
		const std::string& task_id, const std::string& type_id, std::string title);
matters::WorkProductObject update(UpdateWorkProductCommand command);
matters::WorkProductObject update_issue_status(const std::string& project_id,
		const std::string& issue_id, const std::string& status, long long expected_revision);
matters::WorkProductObject restore_revision(const std::string& project_id,
		const std::string& work_product_id, long long revision, long long expected_revision);

// Removes one kept result together with its earlier versions, and unlinks it
// from the AI result it was kept from, so that result can be kept again.
// Resolves false when it was already gone.
bool remove(const std::string& project_id, const std::string& work_product_id);

} // namespace singularity::work_product
