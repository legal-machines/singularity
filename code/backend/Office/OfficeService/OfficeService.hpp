#pragma once

#include "Office/OfficeApps/OfficeApps.hpp"
#include "Office/OfficeClients/OfficeClients.hpp"
#include "Office/OfficeConnection/OfficeConnection.hpp"
#include "Platform/SystemPermissions/SystemPermissions.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Singularity in Word, Excel and Outlook on this computer: whether the
// person connected them, and what that took - the certificate the system
// trusts for the panes, the add-ins Office knows, and the connection that
// serves the panes while Singularity is open.
//
// Nothing here starts on its own the first time: the person connects Office
// from Singularity's window, and the system asks them to trust the
// certificate. From then on the connection opens whenever Singularity does.
namespace singularity::office {

struct OfficeStatus {
	// Word, Excel and Outlook can be connected on this system at all.
	bool supported = false;
	// The person connected them on this computer.
	bool connected = false;
	// The panes are being served now.
	bool listening = false;
	int port = 0;
	// "none", "trusted", "not_trusted" or "unsupported".
	std::string certificate = "none";
	// When the certificate ends, "YYYY-MM-DD".
	std::string certificate_ends;
	// Word and Excel find the add-in.
	bool documents_registered = false;
	// The Outlook add-in's manifest, for Outlook's "Add from file".
	std::string outlook_manifest;
	std::string connected_at;
	// What stands in the way, in a sentence, when something does.
	std::string problem;
	// "waiting" when the last Connect found Word, Excel and PowerPoint never
	// opened on this Mac, so Office has no folder for the add-in yet; "opened"
	// once one of them has been since, and Connect can add it now; "none"
	// otherwise. A first opening is a step, not a failure.
	std::string first_opening = "none";
};

// The bridge's carrier for the connection; installed once the window's bridge is.
void install_carrier(Carrier carrier);

// Singularity's Office folder on this computer: the certificate, the
// manifests, the panes' pairings and their access log, and the files it opens
// Word, Excel and PowerPoint with (Office/OfficeApps).
std::filesystem::path office_folder();

OfficeStatus office_status();

// Makes the certificate when there is none or it ends within a month, asks
// the system to trust it, writes and registers the add-ins and starts serving
// the panes. Blocks while the system asks the person. Never throws.
OfficeStatus connect_office();

// The certificate the panes are reached with, as onboarding asks it before
// Office is connected, so connecting asks nothing more. Not needed where
// neither Word, Excel, PowerPoint nor Outlook is installed; allowed once
// this user's system trusts it; denied when the person said no in this run.
// Never asks, never throws.
platform::system_permissions::Permission office_certificate_permission();

// Makes the certificate as connect_office does, when there is none or it ends
// within a month, and asks the system to trust it for this user: Windows' own
// security warning, or the Mac's password. Says what came of it. Blocks while
// the system asks the person. Never throws.
platform::system_permissions::Permission ask_office_certificate_trust();

// Opens where the person sees the certificates their account trusts: on
// Windows the current user's Certificates, whose Trusted Root Certification
// Authorities lists Singularity's. False where it cannot.
bool open_office_certificate_settings();

// Opens the application the person is connecting, from Singularity's window
// only: Word, Excel and PowerPoint with Singularity's own file, whose pane
// opens and asks to connect by itself, Outlook as it is. Only once Office is
// connected here and its panes are served. Never throws.
OpenedApp open_office_app(OfficeApp app);

// Stops serving the panes, removes the add-ins from Word and Excel and every
// pane's pairing. The certificate stays trusted, and is used again on the
// next connection.
OfficeStatus disconnect_office();

// The panes waiting for the person's answer, and the person's answer - from
// Singularity's window only (Office/OfficeClients).
std::vector<PendingPairing> office_pairings();
bool decide_office_pairing(const std::string &pairing_id, bool allow);

// The panes paired on this computer, and removing one.
std::vector<PairedClient> office_clients();
bool remove_office_client(const std::string &client_id);

// A place in a Matter's document the person asked to see in Word or Excel from
// Singularity's window - a message's place, a colleague's change: the document
// opens, or comes forward, in its application, and its open pane goes to the
// place ("office.reveal"). A pane opened for that document within two minutes
// goes there too. Kept in memory only.
struct RevealRequest {
	std::string id;
	std::string matter_id;
	std::string document_id;
	std::string section;
	std::string heading;
	std::string quote;
	std::string requested_at;
};

// Throws std::invalid_argument when it names no place, std::runtime_error when
// the document cannot be opened on this computer.
RevealRequest reveal_in_office(const std::string &matter_id, const std::string &document_id, const std::string &section,
	const std::string &heading, const std::string &quote);

// The latest request for a document, while it is two minutes old or less.
std::optional<RevealRequest> pending_reveal(const std::string &document_id);

// At launch: serves the panes again when the person connected Office here.
// Never throws.
void start_office_if_connected();

// At closing: stops serving. Never throws.
void stop_office() noexcept;

} // namespace singularity::office
