#pragma once

#include "Matter/Types/Matter.hpp"

#include <string>

namespace singularity::activity {

matters::Matter add_note(const std::string &matter_id, std::string content);
matters::Matter remove_note(const std::string &matter_id, const std::string &note_id);
matters::Matter promote_finding(const std::string &matter_id, const std::string &finding_id);
matters::Matter update_issue_status(const std::string &matter_id, const std::string &issue_id,
                                    const std::string &status);

} // namespace singularity::activity
