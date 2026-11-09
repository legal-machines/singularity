#pragma once

#include <string>

// Singularity's add-ins for Word, Excel and Outlook: their manifests, which
// tell the Office application where the pane comes from - Singularity on this
// computer, at https://localhost on one fixed port - and which button opens it.
// One pane serves the three; it knows from Office which one it is in.
namespace singularity::office {

// The port Singularity's Office connection listens on, on 127.0.0.1 only. It
// is fixed because a manifest names it.
inline constexpr int kOfficePort = 47621;

// Word and Excel share one add-in; Outlook's is a mail add-in of its own.
inline constexpr const char *kDocumentsAddinId = "68524925-cfe2-4c58-a695-d8afd89e66fc";
inline constexpr const char *kOutlookAddinId = "9a5ca4ac-ac2d-437e-afa1-554f3f31d7ac";

// "https://localhost:47621": the one origin the panes come from.
std::string office_origin();

// The manifest of the Word and Excel add-in, and of the Outlook add-in, for
// this version of Singularity ("1.0.0.0" form).
std::string documents_manifest(const std::string &version);
std::string outlook_manifest(const std::string &version);

// Singularity's version as Office writes one: four numbers, "0.1.0" becoming
// "0.1.0.0".
std::string manifest_version(const std::string &app_version);

} // namespace singularity::office
