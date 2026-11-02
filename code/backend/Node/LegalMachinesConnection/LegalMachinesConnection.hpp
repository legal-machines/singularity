#pragma once

#include "Platform/HttpClient/HttpClient.hpp"

#include <functional>
#include <string>

namespace singularity::node {

// The Legal Machines service Singularity talks to: the model catalog, the signed-in
// session and, later, everything an account brings. One address, named once.
inline constexpr const char *kLegalMachinesApiBase = "https://legalmachines.org/singularity/api";

// Whether this installation can reach Legal Machines right now - the first
// thing Singularity checks as it opens. Today that is reachability: any answer from
// the service is a connection, and nothing but the request itself is sent. It
// is the one place a signed-in session handshake will be added (BB-05), so the
// startup screen's "connecting to Legal Machines" never has to change.
//
// Never throws and never waits longer than kConnectionTimeoutSeconds: being
// offline is an ordinary state of a local-first product, not an error, and Singularity
// opens either way.
inline constexpr int kConnectionTimeoutSeconds = 4;

struct LegalMachinesConnection {
	bool reachable = false;
	// "connected", "offline" (no answer in time or no route) or "unreachable"
	// (the network answered with a failure).
	std::string state;
	int http_status = 0;
	long long elapsed_ms = 0;
	// A sentence for a person, never a transport error string.
	std::string detail;
};

// The transport, injectable so the rule above is tested without a network: it
// returns the HTTP status of a GET to `url`, or throws for a transport failure
// (platform::HttpTransportError, whose timed_out says whether time ran out).
using StatusFetcher = std::function<int(const std::string &url, int timeout_seconds)>;

[[nodiscard]] LegalMachinesConnection check_legal_machines_connection(const StatusFetcher &fetch);

// The same check over this platform's own HTTPS client.
[[nodiscard]] inline LegalMachinesConnection check_legal_machines_connection() {
	return check_legal_machines_connection([](const std::string &url, int timeout_seconds) {
		return platform::https_get(url, {}, timeout_seconds).status;
	});
}

} // namespace singularity::node
