#include "Office/OfficeService/OfficeService.hpp"

#include "Office/OfficeAddins/OfficeAddins.hpp"
#include "Office/OfficeCertificate/OfficeCertificate.hpp"
#include "Office/OfficeFiles/OfficeFiles.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Events/Events.hpp"
#include "Platform/OfficeAddinRegistry/OfficeAddinRegistry.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/TrustStore/TrustStore.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace singularity::office {

std::filesystem::path office_folder() {
	return platform::computer_records_directory() / "Office";
}

namespace {

using Json = nlohmann::json;

// A certificate runs a little over two years (the most macOS accepts for a
// server certificate), and is made again a month before it ends.
constexpr int kCertificateDays = 820;
constexpr int kRenewWithinDays = 30;

#if defined(NATIVE_APP_VERSION)
constexpr const char *kAppVersion = NATIVE_APP_VERSION;
#else
constexpr const char *kAppVersion = "0.1.0";
#endif

struct State {
	std::mutex mutex;
	Carrier carrier;
	bool has_carrier = false;
	std::unique_ptr<OfficeServer> server;
	std::shared_ptr<OfficeClients> clients;
	std::string problem;
	// The latest place the person asked to see, and when.
	std::optional<RevealRequest> reveal;
	std::chrono::steady_clock::time_point revealed_at;

	std::shared_ptr<OfficeClients> paired() {
		if (!clients) clients = std::make_shared<OfficeClients>(office_folder());
		return clients;
	}
};

State &state() {
	static State *kept = new State();
	return *kept;
}

// Whether an id names a Matter open on this computer: what a pane's call may
// name, checked here, never taken from the pane.
bool is_matter_here(const std::string &matter_id) {
	try {
		return matters::load_matter(node::require_active_workspace_path(), matter_id).has_value();
	} catch (const std::exception &) {
		return false;
	}
}

std::filesystem::path documents_manifest_path() {
	return office_folder() / "singularity-word-excel.xml";
}

std::filesystem::path outlook_manifest_path() {
	return office_folder() / "singularity-outlook.xml";
}

std::filesystem::path connection_path() {
	return office_folder() / "connection.json";
}

std::string now_utc() {
	const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm parts{};
#if defined(_WIN32)
	gmtime_s(&parts, &seconds);
#else
	gmtime_r(&seconds, &parts);
#endif
	char text[32] = {};
	std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
	return text;
}

Json read_connection() {
	std::ifstream input(connection_path(), std::ios::binary);
	if (!input) return Json::object();
	std::stringstream text;
	text << input.rdbuf();
	const Json value = Json::parse(text.str(), nullptr, false);
	return value.is_object() ? value : Json::object();
}

void write_connection(bool connected, const std::string &connected_at) {
	platform::ensure_record_directory(office_folder());
	platform::write_file_durably(connection_path(), Json{{"connected", connected}, {"connectedAt", connected_at}}.dump(2));
}

std::string ends_of(const LocalCertificate &certificate) {
	const std::string &end = certificate.not_after;
	return end.size() >= 8 ? end.substr(0, 4) + "-" + end.substr(4, 2) + "-" + end.substr(6, 2) : std::string();
}

std::string certificate_word(platform::trust_store::TrustState state) {
	switch (state) {
		case platform::trust_store::TrustState::trusted: return "trusted";
		case platform::trust_store::TrustState::unsupported: return "unsupported";
		default: return "not_trusted";
	}
}

// The manifests, written for this version; Word and Excel's registered.
bool write_and_register_manifests() {
	platform::ensure_record_directory(office_folder());
	const std::string version = manifest_version(kAppVersion);
	platform::write_file_durably(documents_manifest_path(), documents_manifest(version));
	platform::write_file_durably(outlook_manifest_path(), outlook_manifest(version));
	return platform::office_addins::register_manifest(kDocumentsAddinId, documents_manifest_path());
}

std::filesystem::path panes_folder() {
	return platform::frontend_entry_path().parent_path() / "office";
}

// Starts serving with the certificate; the problem in a sentence when it cannot.
std::string serve_locked(State &kept, const LocalCertificate &certificate) {
	if (!kept.has_carrier) return "Singularity has not finished opening; try again in a moment.";
	if (kept.server && kept.server->is_listening()) return {};
	Gate gate;
	gate.clients = kept.paired();
	gate.is_matter_here = is_matter_here;
	// Singularity's window shows the person the code and asks them.
	gate.pairing_requested = [] { platform::events::publish("office.pairingRequested", nlohmann::json::object()); };
	// What reached the connection, for the person's and an auditor's eyes.
	gate.access_log = office_folder() / "access.log";
	kept.server = std::make_unique<OfficeServer>(kept.carrier, panes_folder(), gate);
	try {
		kept.server->start(kOfficePort, certificate.certificate_pem, certificate.key_pem);
	} catch (const std::exception &error) {
		kept.server.reset();
		return error.what();
	}
	return {};
}

} // namespace

void install_carrier(Carrier carrier) {
	State &kept = state();
	const std::lock_guard<std::mutex> lock(kept.mutex);
	kept.carrier = std::move(carrier);
	kept.has_carrier = true;
}

OfficeStatus office_status() {
	OfficeStatus status;
	status.supported = platform::office_addins::is_supported();
	status.port = kOfficePort;
	const Json connection = read_connection();
	status.connected = connection.value("connected", false);
	status.connected_at = connection.value("connectedAt", std::string());
	if (const auto certificate = load_local_certificate(office_folder())) {
		status.certificate = certificate_word(platform::trust_store::trust_state(certificate->certificate_der));
		status.certificate_ends = ends_of(*certificate);
	}
	status.documents_registered = platform::office_addins::is_registered(kDocumentsAddinId, documents_manifest_path());
	std::error_code error;
	if (std::filesystem::exists(outlook_manifest_path(), error)) status.outlook_manifest = outlook_manifest_path().u8string();
	State &kept = state();
	const std::lock_guard<std::mutex> lock(kept.mutex);
	status.listening = kept.server && kept.server->is_listening();
	status.problem = kept.problem;
	return status;
}

OfficeStatus connect_office() {
	State &kept = state();
	if (!platform::office_addins::is_supported()) {
		const std::lock_guard<std::mutex> lock(kept.mutex);
		kept.problem = "Word, Excel, PowerPoint and Outlook do not run on this system.";
		return OfficeStatus{};
	}
	std::string problem;
	try {
		std::optional<LocalCertificate> certificate = load_local_certificate(office_folder());
		if (!certificate || !runs_for_more_than(*certificate, kRenewWithinDays)) {
			certificate = make_local_certificate(kCertificateDays);
			save_local_certificate(office_folder(), *certificate);
		}
		// The person agreed in Singularity's window before this was called; the
		// system asks too, here (on Windows in front of Singularity's window,
		// Platform/TrustStore), and nothing is held meanwhile.
		const auto trust = platform::trust_store::trust_for_this_user(certificate->certificate_der);
		if (trust.state != platform::trust_store::TrustState::trusted) {
#if defined(__APPLE__)
			constexpr const char *kDeclined = "The certificate for Singularity's panes was not trusted, so Word, Excel, PowerPoint "
				"and Outlook are not connected. Try again, and enter your Mac's password when macOS asks.";
#else
			constexpr const char *kDeclined = "The certificate for Singularity's panes was not installed, so Word, Excel, PowerPoint "
				"and Outlook are not connected. Try again, and choose Yes when Windows asks.";
#endif
			problem = trust.declined ? std::string(kDeclined) : trust.problem;
		} else {
			const bool registered = write_and_register_manifests();
			const std::lock_guard<std::mutex> lock(kept.mutex);
			problem = serve_locked(kept, *certificate);
			if (problem.empty() && !registered) problem = "Word, Excel and PowerPoint could not be told about Singularity's add-in.";
			if (problem.empty() || registered) write_connection(true, now_utc());
		}
	} catch (const std::exception &error) {
		problem = error.what();
	}
	{
		const std::lock_guard<std::mutex> lock(kept.mutex);
		kept.problem = problem;
	}
	return office_status();
}

OpenedApp open_office_app(OfficeApp app) {
	OpenedApp refused;
	if (!platform::office_addins::is_supported()) {
		refused.problem = "Word, Excel, PowerPoint and Outlook do not run on this system.";
		return refused;
	}
	{
		// The panes come from this connection: without it, a pane would open on nothing.
		State &kept = state();
		const std::lock_guard<std::mutex> lock(kept.mutex);
		if (!kept.server || !kept.server->is_listening()) {
			refused.problem = std::string("Singularity is not connected to ") + name_of(app) + " on this computer yet. Connect it first.";
			return refused;
		}
	}
	std::optional<std::filesystem::path> file;
	if (!starter_file_name(app).empty()) {
		try {
			file = write_starter_file(app, office_folder(), kDocumentsAddinId, manifest_version(kAppVersion));
		} catch (const std::exception &) {
			refused.problem = std::string("Singularity could not prepare ") + name_of(app) + " on this computer. Try again.";
			return refused;
		}
	}
	return open_application(app, file);
}

OfficeStatus disconnect_office() {
	State &kept = state();
	{
		const std::lock_guard<std::mutex> lock(kept.mutex);
		if (kept.server) kept.server->stop();
		kept.server.reset();
		kept.problem.clear();
		kept.paired()->revoke_all();
	}
	(void)platform::office_addins::unregister_manifest(kDocumentsAddinId);
	// The system no longer trusts the panes' certificate either: removing Singularity
	// from Office leaves nothing of it behind in the system.
	if (const auto certificate = load_local_certificate(office_folder())) {
		(void)platform::trust_store::forget(certificate->certificate_der);
	}
	try {
		write_connection(false, std::string());
	} catch (const std::exception &) {
	}
	return office_status();
}

std::vector<PendingPairing> office_pairings() {
	State &kept = state();
	std::shared_ptr<OfficeClients> clients;
	{
		const std::lock_guard<std::mutex> lock(kept.mutex);
		clients = kept.paired();
	}
	return clients->pending();
}

bool decide_office_pairing(const std::string &pairing_id, bool allow) {
	State &kept = state();
	std::shared_ptr<OfficeClients> clients;
	{
		const std::lock_guard<std::mutex> lock(kept.mutex);
		clients = kept.paired();
	}
	return clients->decide_pairing(pairing_id, allow);
}

std::vector<PairedClient> office_clients() {
	State &kept = state();
	std::shared_ptr<OfficeClients> clients;
	{
		const std::lock_guard<std::mutex> lock(kept.mutex);
		clients = kept.paired();
	}
	return clients->clients();
}

bool remove_office_client(const std::string &client_id) {
	State &kept = state();
	std::shared_ptr<OfficeClients> clients;
	{
		const std::lock_guard<std::mutex> lock(kept.mutex);
		clients = kept.paired();
	}
	return clients->revoke(client_id);
}

namespace {

// A place's words on one line, at most `most` bytes, cut at a character.
std::string kept_words(const std::string &value, std::size_t most) {
	std::string line;
	bool space = false;
	for (const char character : value) {
		if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
			space = !line.empty();
			continue;
		}
		if (space) line.push_back(' ');
		space = false;
		line.push_back(character);
	}
	if (line.size() <= most) return line;
	std::size_t end = most;
	while (end > 0 && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80) --end;
	return line.substr(0, end);
}

} // namespace

RevealRequest reveal_in_office(const std::string &matter_id, const std::string &document_id, const std::string &section,
	const std::string &heading, const std::string &quote) {
	RevealRequest request;
	request.matter_id = matter_id;
	request.document_id = document_id;
	request.section = kept_words(section, 40);
	request.heading = kept_words(heading, 200);
	request.quote = kept_words(quote, 1200);
	if (request.section.empty() && request.quote.empty()) throw std::invalid_argument("Name the section or the words to show.");
	request.id = "reveal_" + platform::secure_random_hex(8);
	request.requested_at = now_utc();
	// Opens it, or brings it forward where it is open: only a document inside
	// the Matter's folder, and only one Singularity opens (OfficeFiles).
	open_document(matter_id, document_id);
	{
		State &kept = state();
		const std::lock_guard<std::mutex> lock(kept.mutex);
		kept.reveal = request;
		kept.revealed_at = std::chrono::steady_clock::now();
	}
	platform::events::publish("office.reveal", Json{{"revealId", request.id}, {"matterId", request.matter_id},
		{"documentId", request.document_id}, {"section", request.section}, {"heading", request.heading}, {"quote", request.quote},
		{"requestedAt", request.requested_at}});
	return request;
}

std::optional<RevealRequest> pending_reveal(const std::string &document_id) {
	State &kept = state();
	const std::lock_guard<std::mutex> lock(kept.mutex);
	if (!kept.reveal || kept.reveal->document_id != document_id) return std::nullopt;
	if (std::chrono::steady_clock::now() - kept.revealed_at > std::chrono::minutes(2)) return std::nullopt;
	return kept.reveal;
}

void start_office_if_connected() {
	try {
		if (!read_connection().value("connected", false)) return;
		const auto certificate = load_local_certificate(office_folder());
		State &kept = state();
		if (!certificate) {
			const std::lock_guard<std::mutex> lock(kept.mutex);
			kept.problem = "The certificate for Singularity's panes is missing; connect Word, Excel, PowerPoint and Outlook again.";
			return;
		}
		if (platform::trust_store::trust_state(certificate->certificate_der) != platform::trust_store::TrustState::trusted) {
			const std::lock_guard<std::mutex> lock(kept.mutex);
			kept.problem = "The system no longer trusts the certificate for Singularity's panes; connect again.";
			return;
		}
		(void)write_and_register_manifests();
		const std::lock_guard<std::mutex> lock(kept.mutex);
		kept.problem = serve_locked(kept, *certificate);
	} catch (const std::exception &error) {
		State &kept = state();
		const std::lock_guard<std::mutex> lock(kept.mutex);
		kept.problem = error.what();
	}
}

void stop_office() noexcept {
	try {
		State &kept = state();
		const std::lock_guard<std::mutex> lock(kept.mutex);
		if (kept.server) kept.server->stop();
	} catch (...) {
	}
}

} // namespace singularity::office
