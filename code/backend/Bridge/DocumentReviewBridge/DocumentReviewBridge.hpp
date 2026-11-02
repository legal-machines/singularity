#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::document_review_bridge {

// documentReview.* (DocumentReview/): parses, calls the domain, serializes.
nlohmann::json list(const nlohmann::json& params);
nlohmann::json get(const nlohmann::json& params);
nlohmann::json decide(const nlohmann::json& params);
nlohmann::json retry(const nlohmann::json& params);
// documentReview.assignments: the assignments colleagues carried out, each step
// with where its review stands and its email with whether it is ready to send;
// documentReview.leaveOut: the version waiting, made again without the changes
// the lawyer leaves out (D-119).
nlohmann::json assignments(const nlohmann::json& params);
nlohmann::json leaveOut(const nlohmann::json& params);
// documentReview.answerDecision: the lawyer's answer to one of an assignment's
// decisions, recorded and said to the colleague in the thread (D-120).
nlohmann::json answerDecision(const nlohmann::json& params);
// documentReview.decideWritten: the lawyer's decision on a document a colleague
// wrote (D-141), accepted or returned as a revised draft is (D-143); the
// assignment as it reads then.
nlohmann::json decideWritten(const nlohmann::json& params);

// documentReview.saveCopy: a copy of a version's revised draft, saved where
// the lawyer chooses. The file is found and checked before the dialog opens,
// so a draft that is gone never asks where to put it; the copy is made once
// the lawyer chose (NativeBridge opens the dialog on the window's thread).
struct PreparedCopy {
	std::filesystem::path source;
	std::string defaultName;
};
PreparedCopy prepareCopy(const nlohmann::json& params);
nlohmann::json finishCopy(const PreparedCopy& prepared, const std::optional<std::filesystem::path>& destination);

} // namespace singularity::bridge::document_review_bridge
