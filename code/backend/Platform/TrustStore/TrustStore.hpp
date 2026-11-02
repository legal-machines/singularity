#pragma once

#include <string>

// The system's trust in the one certificate Singularity makes for its own
// panes on this computer (Office/OfficeCertificate, D-105): a self-signed
// server certificate for localhost, trusted as itself - never an authority,
// so it vouches for nothing else. Nothing else is ever added or removed.
//
// Windows: the current user's Trusted Root store, where Windows itself asks
// the person in its security warning, and nothing is trusted unless they say
// yes; Singularity's window owns that warning while it is open, so it stands
// in front of the window. The certificate is still no authority (it signs
// nothing). Trusted People, which Chromium (WebView2) reads as trusted
// leaves, is not enough:
// software that inspects encrypted connections - an antivirus such as
// Kaspersky's, which re-signs every connection a browser makes, localhost's
// included - judges a server only by the roots, and blocks the pane
// otherwise (found on 2026-10-24, docs/OFFICE.md). Singularity's window
// says what is added before it asks Windows, and offers its removal.
// macOS: the login keychain, with trust set for this user; macOS asks for the
// person's password. Linux: no Office runs there, so nothing is offered.
namespace singularity::platform::trust_store {

enum class TrustState {
	trusted,
	not_trusted,
	// This system keeps no such list Singularity can use.
	unsupported,
};

TrustState trust_state(const std::string &certificate_der);

struct TrustOutcome {
	TrustState state = TrustState::not_trusted;
	// The person said no when the system asked.
	bool declined = false;
	// What went wrong, in a sentence, when it was neither trusted nor declined.
	std::string problem;
};

// Asks the system to trust the certificate for this user. Blocks while the
// system asks the person, where it asks. Never throws.
TrustOutcome trust_for_this_user(const std::string &certificate_der);

// Removes the certificate from the list, when the person removes Singularity
// from Office. Never throws.
bool forget(const std::string &certificate_der);

} // namespace singularity::platform::trust_store
