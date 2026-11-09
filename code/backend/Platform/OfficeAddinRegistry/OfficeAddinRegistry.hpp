#pragma once

#include <filesystem>
#include <string>

// Where this user's Word and Excel find an add-in that is not from a store:
// Singularity's own, whose manifest Singularity writes on this computer.
//
// Windows: Office's developer add-ins, a value per add-in under
// HKEY_CURRENT_USER\Software\Microsoft\Office\16.0\WEF\Developer naming its
// manifest. macOS: the manifest copied into Word's and Excel's own "wef"
// folders. Linux: no Office runs there.
//
// Outlook takes an add-in only from the person's own mailbox (its
// "Add from file"), so it is not registered here.
namespace singularity::platform::office_addins {

bool is_supported();

// Registers the manifest under the add-in's id. Never throws.
// On macOS each application's folder is another application's data, which
// macOS guards with its own question ("access data from other apps"): it is
// entered only to place a manifest it does not hold yet, or when `again` is
// asked for (the person connecting); what was placed is remembered beside the
// manifest, so reading whether it is registered never enters them.
bool register_manifest(const std::string &addin_id, const std::filesystem::path &manifest, bool again = false);

// Whether this user's Office has the add-in registered with this manifest.
bool is_registered(const std::string &addin_id, const std::filesystem::path &manifest);

// Removes the add-in's registration. Never throws.
bool unregister_manifest(const std::string &addin_id);

} // namespace singularity::platform::office_addins
