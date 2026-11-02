#pragma once

#include <filesystem>
#include <optional>
#include <string>

// The certificate Singularity's Office connection presents on this
// computer's loopback (D-105). Word, Excel and Outlook load a pane only over
// HTTPS, so the panes Singularity serves from this computer need a
// certificate the system trusts for localhost.
//
// It is one self-signed server certificate, for localhost and 127.0.0.1 and
// for serving only: it is no authority and signs nothing, so trusting it
// trusts Singularity's own panes on this computer and nothing else. The
// system is asked to trust that one certificate as itself (Windows' Trusted
// People store), never to trust a new authority (docs/OFFICE.md).
namespace singularity::office {

struct LocalCertificate {
	// The server's certificate, for localhost and 127.0.0.1, and its private key.
	std::string certificate_pem;
	std::string key_pem;
	// The same certificate as the DER bytes a system trust store takes.
	std::string certificate_der;
	// When it ends: "YYYYMMDDhhmmss", UTC.
	std::string not_after;
};

// A new certificate, valid from a day ago for `days` days. Throws
// std::runtime_error when it cannot be made.
LocalCertificate make_local_certificate(int days);

// Kept in `folder`: the certificate, and its key, readable by this user alone
// (Platform/PrivateFile).
void save_local_certificate(const std::filesystem::path &folder, const LocalCertificate &certificate);
std::optional<LocalCertificate> load_local_certificate(const std::filesystem::path &folder);

// Whether the certificate still runs for more than `days` days from now.
bool runs_for_more_than(const LocalCertificate &certificate, int days);

// The UTC moment `days` from now, as certificates write it: "YYYYMMDDhhmmss".
std::string certificate_time_in_days(int days);

} // namespace singularity::office
