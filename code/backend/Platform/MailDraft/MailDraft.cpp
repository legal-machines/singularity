#include "Platform/MailDraft/MailDraft.hpp"

#if !defined(__APPLE__)
#include "Platform/OpenExternal/OpenExternal.hpp"

namespace singularity::platform {

bool open_mail_draft(const std::string &, const std::string &, const std::string &,
                     const std::vector<std::filesystem::path> &, const std::filesystem::path &eml) {
  return open_in_default_application(eml.u8string());
}

} // namespace singularity::platform
#endif
