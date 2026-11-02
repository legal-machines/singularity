#include "Office/OfficeClients/OfficeClients.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>

namespace singularity::office {
namespace {

using Json = nlohmann::json;

constexpr int kTokenDays = 90;
constexpr auto kPairingLasts = std::chrono::minutes(3);
constexpr auto kQuietAfterDecline = std::chrono::seconds(30);
constexpr std::size_t kMostPending = 2;
constexpr long long kMomentToleranceMilliseconds = 120 * 1000;
constexpr auto kNonceKept = std::chrono::minutes(5);

const std::set<std::string> &common_methods() {
	static const std::set<std::string> methods = {
		"matter.listSummaries", "matter.listDocuments",
		"chat.list", "chat.create", "chat.rename", "chat.conversation", "chat.post",
		"chat.editMessage", "chat.deleteMessage", "chat.react",
		"agent.list", "organisation.get", "project.sharedActivity",
		"documentReview.list", "documentReview.get",
		"office.describeFile", "office.fileIntoMatter",
	};
	return methods;
}

const std::set<std::string> &word_methods() {
	static const std::set<std::string> methods = {
		"matter.readDocumentView", "chat.closeThread", "chat.reopenThread",
		"documentReview.decide", "documentReview.retry", "office.openDocument",
	};
	return methods;
}

const std::set<std::string> &excel_methods() {
	static const std::set<std::string> methods = {
		"matter.readDocumentView", "documentReview.decide", "reviewTable.list", "reviewTable.get", "office.openDocument",
	};
	return methods;
}

// A deck's pane (D-112): its Matter, its thread and the slide's words, and the
// deck opened from a place in the thread.
const std::set<std::string> &powerpoint_methods() {
	static const std::set<std::string> methods = {"matter.readDocumentView", "office.openDocument"};
	return methods;
}

const std::set<std::string> &outlook_methods() {
	static const std::set<std::string> methods = {"office.documentContent", "correspondence.suggest", "correspondence.file",
		"correspondence.move", "correspondence.list", "correspondence.saveAttachment", "office.emailDraft",
		"documentReview.assignments"};
	return methods;
}

std::string utc(std::chrono::system_clock::time_point moment) {
	const std::time_t seconds = std::chrono::system_clock::to_time_t(moment);
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

std::string now_utc() {
	return utc(std::chrono::system_clock::now());
}

std::string six_digits() {
	std::uint8_t bytes[4] = {};
	platform::fill_secure_random(bytes, sizeof(bytes));
	const std::uint32_t value = (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16)
		| (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
	std::string code = std::to_string(value % 1000000);
	while (code.size() < 6) code.insert(code.begin(), '0');
	return code;
}

// Compares in time that does not depend on where two hashes differ.
bool same_text(const std::string &one, const std::string &other) {
	if (one.size() != other.size()) return false;
	unsigned char difference = 0;
	for (std::size_t index = 0; index < one.size(); ++index) {
		difference |= static_cast<unsigned char>(one[index] ^ other[index]);
	}
	return difference == 0;
}

Json client_json(const PairedClient &client) {
	return Json{{"id", client.id}, {"kind", word_of(client.kind)}, {"label", client.label}, {"pairedAt", client.paired_at},
		{"lastUsedAt", client.last_used_at}, {"expiresAt", client.expires_at}, {"tokenSha256", client.token_sha256}};
}

} // namespace

std::optional<ClientKind> client_kind_of(const std::string &word) {
	if (word == "word") return ClientKind::word;
	if (word == "excel") return ClientKind::excel;
	if (word == "outlook") return ClientKind::outlook;
	if (word == "powerpoint") return ClientKind::powerpoint;
	return std::nullopt;
}

const char *word_of(ClientKind kind) {
	switch (kind) {
		case ClientKind::word: return "word";
		case ClientKind::excel: return "excel";
		case ClientKind::powerpoint: return "powerpoint";
		default: return "outlook";
	}
}

bool may_call(ClientKind kind, const std::string &method) {
	if (common_methods().count(method) != 0) return true;
	switch (kind) {
		case ClientKind::word: return word_methods().count(method) != 0;
		case ClientKind::excel: return excel_methods().count(method) != 0;
		case ClientKind::powerpoint: return powerpoint_methods().count(method) != 0;
		default: return outlook_methods().count(method) != 0;
	}
}

OfficeClients::OfficeClients(std::filesystem::path folder) : folder_(std::move(folder)) {}

void OfficeClients::load_locked() {
	if (loaded_) return;
	loaded_ = true;
	std::ifstream input(folder_ / "clients.json", std::ios::binary);
	if (!input) return;
	std::stringstream text;
	text << input.rdbuf();
	const Json value = Json::parse(text.str(), nullptr, false);
	if (!value.is_object() || !value.contains("clients") || !value.at("clients").is_array()) return;
	for (const Json &entry : value.at("clients")) {
		const auto kind = client_kind_of(entry.value("kind", std::string()));
		if (!kind) continue;
		PairedClient client;
		client.id = entry.value("id", std::string());
		client.kind = *kind;
		client.label = entry.value("label", std::string());
		client.paired_at = entry.value("pairedAt", std::string());
		client.last_used_at = entry.value("lastUsedAt", std::string());
		client.expires_at = entry.value("expiresAt", std::string());
		client.token_sha256 = entry.value("tokenSha256", std::string());
		if (!client.id.empty() && client.token_sha256.size() == 64) clients_.push_back(client);
	}
}

void OfficeClients::save_locked() {
	platform::ensure_record_directory(folder_);
	(void)platform::restrict_to_owner(folder_);
	Json list = Json::array();
	for (const PairedClient &client : clients_) list.push_back(client_json(client));
	platform::write_file_durably(folder_ / "clients.json", Json{{"clients", list}}.dump(2));
	(void)platform::restrict_to_owner(folder_ / "clients.json");
}

void OfficeClients::forget_expired_locked() {
	const auto now = std::chrono::steady_clock::now();
	for (auto entry = pending_.begin(); entry != pending_.end();) {
		if (entry->second.expires < now || entry->second.collected) entry = pending_.erase(entry);
		else ++entry;
	}
	const std::string today = now_utc();
	const auto before = clients_.size();
	clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
		[&today](const PairedClient &client) { return !client.expires_at.empty() && client.expires_at < today; }), clients_.end());
	if (clients_.size() != before) save_locked();
	for (auto &[client, seen] : nonces_) {
		for (auto nonce = seen.begin(); nonce != seen.end();) {
			if (now - nonce->second > kNonceKept) nonce = seen.erase(nonce);
			else ++nonce;
		}
	}
}

std::optional<PairingStart> OfficeClients::begin_pairing(ClientKind kind, const std::string &label) {
	const std::lock_guard<std::mutex> lock(mutex_);
	load_locked();
	forget_expired_locked();
	const auto now = std::chrono::steady_clock::now();
	if (now < quiet_until_) return std::nullopt;
	std::size_t waiting = 0;
	for (const auto &[id, pending] : pending_) {
		if (pending.state == "waiting") ++waiting;
		// One question at a time from each kind of client.
		if (pending.state == "waiting" && pending.request.kind == kind) return std::nullopt;
	}
	if (waiting >= kMostPending) return std::nullopt;
	PairingStart start;
	start.id = "pair_" + platform::secure_random_hex(12);
	start.code = six_digits();
	start.secret = platform::secure_random_hex(32);
	Pending pending;
	pending.request.id = start.id;
	pending.request.kind = kind;
	std::string shown = label.substr(0, 80);
	shown.erase(std::remove_if(shown.begin(), shown.end(), [](unsigned char character) { return character < 0x20; }), shown.end());
	pending.request.label = shown.empty() ? std::string(word_of(kind)) : shown;
	pending.request.code = start.code;
	pending.request.requested_at = now_utc();
	pending.secret_sha256 = platform::sha256_hex_digest(start.secret);
	pending.expires = now + kPairingLasts;
	pending_.emplace(start.id, pending);
	return start;
}

std::vector<PendingPairing> OfficeClients::pending() {
	const std::lock_guard<std::mutex> lock(mutex_);
	forget_expired_locked();
	std::vector<PendingPairing> waiting;
	for (const auto &[id, pending] : pending_) {
		if (pending.state == "waiting") waiting.push_back(pending.request);
	}
	return waiting;
}

bool OfficeClients::decide_pairing(const std::string &pairing_id, bool allow) {
	const std::lock_guard<std::mutex> lock(mutex_);
	forget_expired_locked();
	const auto found = pending_.find(pairing_id);
	if (found == pending_.end() || found->second.state != "waiting") return false;
	found->second.state = allow ? "allowed" : "declined";
	if (!allow) quiet_until_ = std::chrono::steady_clock::now() + kQuietAfterDecline;
	return true;
}

PairingOutcome OfficeClients::finish_pairing(const std::string &pairing_id, const std::string &secret) {
	const std::lock_guard<std::mutex> lock(mutex_);
	load_locked();
	forget_expired_locked();
	PairingOutcome outcome;
	const auto found = pending_.find(pairing_id);
	if (found == pending_.end()) {
		outcome.state = "unknown";
		return outcome;
	}
	if (!same_text(platform::sha256_hex_digest(secret), found->second.secret_sha256)) {
		outcome.state = "unknown";
		return outcome;
	}
	outcome.state = found->second.state;
	if (found->second.state != "allowed") {
		if (found->second.state == "declined") pending_.erase(found);
		return outcome;
	}
	PairedClient client;
	client.id = "client_" + platform::secure_random_hex(12);
	client.kind = found->second.request.kind;
	client.label = found->second.request.label;
	client.paired_at = now_utc();
	client.last_used_at = client.paired_at;
	client.expires_at = utc(std::chrono::system_clock::now() + std::chrono::hours(24) * kTokenDays);
	const std::string token = "lmo_" + platform::secure_random_hex(32);
	client.token_sha256 = platform::sha256_hex_digest(token);
	clients_.push_back(client);
	save_locked();
	found->second.collected = true;
	outcome.token = token;
	outcome.client_id = client.id;
	return outcome;
}

std::optional<PairedClient> OfficeClients::authorize(const std::string &token) {
	if (token.rfind("lmo_", 0) != 0 || token.size() != 68) return std::nullopt;
	const std::lock_guard<std::mutex> lock(mutex_);
	load_locked();
	forget_expired_locked();
	const std::string hashed = platform::sha256_hex_digest(token);
	for (PairedClient &client : clients_) {
		if (same_text(client.token_sha256, hashed)) {
			const std::string now = now_utc();
			// The use is written at most once a minute.
			if (client.last_used_at.substr(0, 16) != now.substr(0, 16)) {
				client.last_used_at = now;
				save_locked();
			}
			return client;
		}
	}
	return std::nullopt;
}

bool OfficeClients::is_fresh(const std::string &client_id, const std::string &moment_milliseconds, const std::string &nonce) {
	if (moment_milliseconds.empty() || moment_milliseconds.size() > 15 || nonce.size() < 16 || nonce.size() > 64) return false;
	if (!std::all_of(moment_milliseconds.begin(), moment_milliseconds.end(), [](unsigned char character) { return std::isdigit(character); })) {
		return false;
	}
	if (!std::all_of(nonce.begin(), nonce.end(), [](unsigned char character) { return std::isalnum(character) || character == '-'; })) {
		return false;
	}
	const long long moment = std::stoll(moment_milliseconds);
	const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	if (moment < now - kMomentToleranceMilliseconds || moment > now + kMomentToleranceMilliseconds) return false;
	const std::lock_guard<std::mutex> lock(mutex_);
	auto &seen = nonces_[client_id];
	if (seen.count(nonce) != 0) return false;
	seen.emplace(nonce, std::chrono::steady_clock::now());
	return true;
}

std::vector<PairedClient> OfficeClients::clients() {
	const std::lock_guard<std::mutex> lock(mutex_);
	load_locked();
	forget_expired_locked();
	return clients_;
}

bool OfficeClients::revoke(const std::string &client_id) {
	const std::lock_guard<std::mutex> lock(mutex_);
	load_locked();
	const auto before = clients_.size();
	clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
		[&client_id](const PairedClient &client) { return client.id == client_id; }), clients_.end());
	if (clients_.size() == before) return false;
	nonces_.erase(client_id);
	save_locked();
	return true;
}

void OfficeClients::revoke_all() {
	const std::lock_guard<std::mutex> lock(mutex_);
	load_locked();
	clients_.clear();
	pending_.clear();
	nonces_.clear();
	save_locked();
}

} // namespace singularity::office
