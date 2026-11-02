#include "Node/LegalMachinesConnection/LegalMachinesConnection.hpp"

#include <chrono>
#include <exception>

namespace singularity::node {

LegalMachinesConnection check_legal_machines_connection(const StatusFetcher &fetch) {
	LegalMachinesConnection connection;
	const auto started = std::chrono::steady_clock::now();
	const auto finish = [&] {
		connection.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started).count();
		return connection;
	};
	try {
		connection.http_status = fetch(std::string(kLegalMachinesApiBase) + "/status", kConnectionTimeoutSeconds);
		// The service answered: whatever it said about this path, it is there.
		connection.reachable = connection.http_status > 0 && connection.http_status < 500;
		connection.state = connection.reachable ? "connected" : "unreachable";
		connection.detail = connection.reachable ? "Connected to Legal Machines."
			: "Legal Machines did not answer properly. Singularity works on this computer as usual.";
	} catch (const platform::HttpTransportError &error) {
		connection.state = "offline";
		connection.detail = error.timed_out ? "Legal Machines did not answer in time. Singularity works on this computer as usual."
			: "Legal Machines cannot be reached from this computer right now. Singularity works on it as usual.";
	} catch (const std::exception &) {
		connection.state = "offline";
		connection.detail = "Legal Machines cannot be reached from this computer right now. Singularity works on it as usual.";
	}
	return finish();
}

} // namespace singularity::node
