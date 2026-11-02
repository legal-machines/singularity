#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The Office applications Singularity opens on this computer as the person
// connects them (D-191): Word, Excel and PowerPoint with a file of
// Singularity's own that carries its add-in, so that Singularity's pane opens
// with the file by itself, and Outlook as it is. Windows finds each
// application where it registered itself (App Paths); macOS opens it by its
// bundle identifier; Linux runs no desktop Office.
namespace singularity::office {

enum class OfficeApp { word, excel, powerpoint, outlook };

// "word", "excel", "powerpoint" or "outlook"; nothing for any other word.
std::optional<OfficeApp> office_app_of(const std::string &word);
const char *word_of(OfficeApp app);
// The application's name as the person knows it: "Word".
const char *name_of(OfficeApp app);

// The program Windows registers the application under in App Paths
// ("WINWORD.EXE"); Outlook's is the classic one's, which opens the new Outlook
// where the person chose it.
const char *windows_program_of(OfficeApp app);
// The application's bundle identifier on macOS ("com.microsoft.Word").
const char *mac_bundle_of(OfficeApp app);
// The application's own folder on macOS ("Microsoft Word.app"), which Office
// for Mac installs into the Applications folder.
const char *mac_app_folder_of(OfficeApp app);

// Whether the application is in `applications`, a Mac's Applications folder.
bool is_installed_in(OfficeApp app, const std::filesystem::path &applications);

// Whether the application is installed on this computer, asked before it is
// opened: App Paths on Windows; the Applications folder, the computer's or
// the person's own, on macOS; never on Linux, which runs no desktop Office.
bool is_installed(OfficeApp app);

// The program a value of App Paths names, without quotes around it; nothing
// when no such file is here.
std::optional<std::filesystem::path> program_at(const std::string &registered);

// The file Singularity opens Word, Excel or PowerPoint with ("Singularity in
// Word.docx"); empty for Outlook, which opens without one.
std::string starter_file_name(OfficeApp app);

// Whether a file is one of those files in `folder`, Singularity's Office folder.
bool is_starter_file(const std::filesystem::path &file, const std::filesystem::path &folder);

// That file's package, part by part: a page that says what the file is, and
// Singularity's add-in with its pane shown as the file opens (task pane
// visibility 1, Office.AutoShowTaskpaneWithDocument). The add-in is named as
// Microsoft's own sideloading names one registered on this computer: its id,
// its version, store "developer", store type "Registry" (docs/OFFICE.md).
std::vector<std::pair<std::string, std::string>> starter_parts(OfficeApp app, const std::string &addin_id,
	const std::string &version);

// Writes the file into `folder` and returns where it is. A file its
// application holds open is left as it is, to be brought forward. Throws
// std::runtime_error when it cannot be written.
std::filesystem::path write_starter_file(OfficeApp app, const std::filesystem::path &folder, const std::string &addin_id,
	const std::string &version);

struct OpenedApp {
	bool opened = false;
	// What stands in the way, in a sentence, when it did not open.
	std::string problem;
};

// Opens the application, with the file when one is given, once it is known to
// be installed; says in a sentence when it is not. Never throws.
OpenedApp open_application(OfficeApp app, const std::optional<std::filesystem::path> &file);

#if defined(_WIN32)
// The program App Paths registers under this name: this user's registration
// first, then this computer's, in Windows' 64-bit and 32-bit views. Nothing
// when it is not installed.
std::optional<std::filesystem::path> registered_program(const std::string &program);
#endif

} // namespace singularity::office
