#include "Node/WorkLogShare/WorkLogShare.hpp"

#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <list>
#include <sstream>
#include <stdexcept>

namespace singularity::node::work_log_share {

namespace {

using Json = nlohmann::json;

// Legal Machines' keys, as their mailboxes publish them (the mail
// repository's server/openpgp), held to the fingerprints pinned beside them.
struct PinnedKey {
	const char *address;
	const char *primary_fingerprint;
	const char *subkey_fingerprint;
	const char *armored;
};

// diagnostics@legalmachines.org: Legal Machines' mailbox for work logs, whose
// key the owner makes in Seal. Pinned here once it is published; until then
// no log is sent (recipients_ready()).
constexpr PinnedKey kDiagnostics {"diagnostics@legalmachines.org", "", "", ""};

// claude@legalmachines.org: Legal Machines' Claude, which reads the logs to
// find what to fix (the owner, 2026-11-05: "diagnostics@, and claude@ too").
constexpr PinnedKey kClaude {"claude@legalmachines.org", "DCB772989B9E1ABB2740FAC53C9292119B9DF195",
	"0FA9BB63EDDB593F41A5113BFEC5AA3F2EF73CE8", R"(-----BEGIN PGP PUBLIC KEY BLOCK-----

mDMEatZoWxYJKwYBBAHaRw8BAQdA51vSw6wE/lJh1B+t6PSEW1oyLvt5JehzVlk0
Xv+5pT+0IUNsYXVkZSA8Y2xhdWRlQGxlZ2FsbWFjaGluZXMub3JnPojZBBMWCgCL
BYJq1mhbBYkFo5qAAwsJBwkQPJKSEZud8ZVFFAAAAAAAHAAgc2FsdEBub3RhdGlv
bnMub3BlbnBncGpzLm9yZ025JxGL5fhZ2NAVcc5GYb/aFIeWVm4b+BJIxDE4pP5t
BRUKCA4MBBYAAgECGQECmwMCHgEWIQTct3KYm54auydA+sU8kpIRm53xlQAA974B
APJOhJkevyRSO7Ah42aIo9in4c1x5ANQDap9IsiVAhbbAP0WFNTmEycpS+LZuPO1
T0aduKtcX5XKXOPf1w1Lj9mkA7g4BGrWaFsSCisGAQQBl1UBBQEBB0CuBDULSzin
s9GdJpxtvKRe311Ql7fBp6qoLoIwwLXcVwMBCAeIxAQYFgoAdgWCatZoWwWJBaOa
gAkQPJKSEZud8ZVFFAAAAAAAHAAgc2FsdEBub3RhdGlvbnMub3BlbnBncGpzLm9y
Z+TOQK2EWLRxq9Vk56IsMFwiGcJI/EhYHYfhmBAREVMoApsMFiEE3LdymJueGrsn
QPrFPJKSEZud8ZUAAIrAAP0cibLFwDLie8kXhX3B/0RKUr1hMna/JKQIgxBnEnDQ
6gEAqzvYBvG2QdiVnVQj9pcUGueDCTAL/HxphMKVAj8J+wQ=
=2ToE
-----END PGP PUBLIC KEY BLOCK-----
)"};

constexpr PinnedKey kPinned[] = {kDiagnostics, kClaude};

std::string base64_of(const std::string &bytes) {
	static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((bytes.size() + 2) / 3 * 4);
	std::size_t at = 0;
	while (at + 3 <= bytes.size()) {
		const unsigned value = (static_cast<unsigned char>(bytes[at]) << 16) | (static_cast<unsigned char>(bytes[at + 1]) << 8)
			| static_cast<unsigned char>(bytes[at + 2]);
		out += alphabet[(value >> 18) & 63];
		out += alphabet[(value >> 12) & 63];
		out += alphabet[(value >> 6) & 63];
		out += alphabet[value & 63];
		at += 3;
	}
	if (at < bytes.size()) {
		const unsigned first = static_cast<unsigned char>(bytes[at]);
		const unsigned second = at + 1 < bytes.size() ? static_cast<unsigned char>(bytes[at + 1]) : 0;
		const unsigned value = (first << 16) | (second << 8);
		out += alphabet[(value >> 18) & 63];
		out += alphabet[(value >> 12) & 63];
		out += at + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

// Base64 in lines of 76, as a MIME part carries it.
std::string mime_base64(const std::string &bytes) {
	const std::string text = base64_of(bytes);
	std::string out;
	for (std::size_t at = 0; at < text.size(); at += 76) out += text.substr(at, 76) + "\r\n";
	return out;
}

std::string random_hex(std::size_t bytes) {
	std::string raw(bytes, '\0');
	platform::fill_secure_random(reinterpret_cast<std::uint8_t *>(raw.data()), raw.size());
	static const char *digits = "0123456789abcdef";
	std::string out;
	for (const char character : raw) {
		out += digits[static_cast<unsigned char>(character) >> 4];
		out += digits[static_cast<unsigned char>(character) & 0x0F];
	}
	return out;
}

// One line of words for a header or a summary: no line breaks, no control
// characters, at most `most` bytes, never cut inside a letter.
std::string one_line(const std::string &text, std::size_t most) {
	std::string out;
	for (const char character : text) out += static_cast<unsigned char>(character) < 0x20 ? ' ' : character;
	if (out.size() > most) {
		std::size_t cut = most;
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
		out = out.substr(0, cut) + "...";
	}
	return out;
}

// A header's value: plain ASCII as it is, anything else as RFC 2047 words of
// at most 45 bytes each (75 characters encoded), never cut inside a letter.
std::string header_value(const std::string &text) {
	const bool plain = std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
	if (plain == true) return text;
	std::string out;
	std::size_t at = 0;
	while (at < text.size()) {
		std::size_t end = std::min(text.size(), at + 45);
		while (end < text.size() && end > at && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
		out += (out.empty() == true ? "" : " ") + std::string("=?UTF-8?B?") + base64_of(text.substr(at, end - at)) + "?=";
		at = end;
	}
	return out;
}

std::optional<std::vector<openpgp::Recipient>> &recipients_for_testing() {
	static std::optional<std::vector<openpgp::Recipient>> given;
	return given;
}

std::string mail_date(std::time_t when) {
	std::tm parts {};
#if defined(_WIN32)
	gmtime_s(&parts, &when);
#else
	gmtime_r(&when, &parts);
#endif
	static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	std::ostringstream out;
	out << days[parts.tm_wday] << ", " << std::setw(2) << std::setfill('0') << parts.tm_mday << ' ' << months[parts.tm_mon] << ' '
		<< (parts.tm_year + 1900) << ' ' << std::setw(2) << parts.tm_hour << ':' << std::setw(2) << parts.tm_min << ':' << std::setw(2)
		<< parts.tm_sec << " +0000";
	return out.str();
}

// A body kept as text: JSON where it is JSON, so a reader sees its shape.
Json readable(const std::string &text) {
	if (text.empty() == true) return Json(nullptr);
	Json value = Json::parse(text, nullptr, false);
	return value.is_discarded() == true ? Json(text) : value;
}

std::string dump(const Json &value) { return value.dump(2, ' ', false, Json::error_handler_t::replace); }

std::string two_digits(std::size_t number, std::size_t width) {
	std::string text = std::to_string(number);
	return std::string(text.size() < width ? width - text.size() : 0, '0') + text;
}

} // namespace

void set_recipients_for_testing(std::optional<std::vector<openpgp::Recipient>> given) {
	recipients_for_testing() = std::move(given);
}

std::vector<openpgp::Recipient> recipients() {
	if (recipients_for_testing().has_value() == true) return *recipients_for_testing();
	std::vector<openpgp::Recipient> found;
	for (const PinnedKey &pinned : kPinned) {
		if (*pinned.armored == '\0') continue;
		const std::optional<openpgp::Recipient> recipient = openpgp::recipient_from_key(pinned.armored, pinned.subkey_fingerprint);
		if (recipient.has_value() == false || recipient->primary_fingerprint != pinned.primary_fingerprint) continue;
		found.push_back(*recipient);
	}
	return found;
}

bool recipients_ready() {
	if (recipients_for_testing().has_value() == true) return recipients_for_testing()->empty() == false;
	return recipients().size() == std::size(kPinned);
}

Preview preview(const std::string &matter_id, const std::string &chat_id) {
	Preview shown;
	const std::vector<work_log::Call> calls = work_log::calls_of(matter_id, chat_id);
	shown.calls = calls.size();
	for (const work_log::Call &call : calls) {
		(call.is_protected == true ? shown.protected_calls : shown.unprotected_calls) += 1;
		if (call.failure.empty() == false) shown.failed_calls += 1;
		shown.bytes += call.request.size() + call.answer.size();
	}
	if (calls.empty() == false) {
		shown.from = calls.front().at;
		shown.to = calls.back().at;
	}
	if (work_log::is_kept() == false) {
		shown.reason = "This computer keeps no work log: only a design partner's computers do.";
	} else if (recipients_ready() == false) {
		shown.reason = "Work logs can't be sent from this build yet: Legal Machines' keys for them are not in it.";
	} else if (calls.empty() == true) {
		shown.reason = "Nothing of this conversation's work is kept on this computer yet.";
	} else {
		shown.can_send = true;
	}
	return shown;
}

std::string zip_of(const Request &request, const std::vector<work_log::Call> &calls, const std::string &sent_at) {
	Json log {
		{"kind", "singularity-work-log"},
		{"version", 1},
		{"sentAt", sent_at},
		{"from", {{"organisation", request.facts.organisation}, {"seat", request.facts.seat},
			{"devicePublicKey", request.facts.device_public_key}}},
		{"app", {{"version", request.facts.app_version}, {"build", request.facts.build}, {"platform", request.facts.platform}}},
		{"chat", {{"matterId", request.matter_id}, {"chatId", request.chat_id}, {"title", request.chat_title}}},
		{"note", request.note},
	};
	Json messages = Json::array();
	for (const Message &message : request.messages)
		messages.push_back({{"author", message.author}, {"at", message.at}, {"text", message.text}});
	log["messages"] = messages;
	Json listed = Json::array();
	std::list<std::string> kept;
	std::vector<std::pair<std::string, const std::string *>> files;
	for (std::size_t index = 0; index < calls.size(); ++index) {
		const work_log::Call &call = calls[index];
		const std::string name = "calls/" + two_digits(index + 1, 4) + ".json";
		listed.push_back({{"number", index + 1}, {"at", call.at}, {"milliseconds", call.milliseconds}, {"provider", call.provider_id},
			{"model", call.model_id}, {"purpose", call.purpose}, {"protected", call.is_protected}, {"inputTokens", call.input_tokens},
			{"outputTokens", call.output_tokens}, {"failure", call.failure}, {"file", name}});
		kept.push_back(dump(Json{{"request", readable(call.request)}, {"answer", readable(call.answer)}, {"failure", call.failure}}));
		files.emplace_back(name, &kept.back());
	}
	log["calls"] = listed;
	kept.push_back(dump(log));
	files.insert(files.begin(), {"log.json", &kept.back()});

	const std::filesystem::path file = std::filesystem::temp_directory_path() / ("singularity-work-log-" + random_hex(8) + ".zip");
	int error = 0;
	zip_t *archive = zip_open(file.u8string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
	if (archive == nullptr) throw std::runtime_error("The work log could not be packed on this computer.");
	for (const auto &[name, bytes] : files) {
		zip_source_t *source = zip_source_buffer(archive, bytes->data(), bytes->size(), 0);
		if (source == nullptr || zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
			if (source != nullptr) zip_source_free(source);
			zip_discard(archive);
			std::error_code ignored;
			std::filesystem::remove(file, ignored);
			throw std::runtime_error("The work log could not be packed on this computer.");
		}
	}
	if (zip_close(archive) != 0) {
		zip_discard(archive);
		std::error_code ignored;
		std::filesystem::remove(file, ignored);
		throw std::runtime_error("The work log could not be packed on this computer.");
	}
	std::string zipped;
	{
		std::ifstream in(file, std::ios::binary);
		std::ostringstream bytes;
		bytes << in.rdbuf();
		zipped = bytes.str();
	}
	std::error_code ignored;
	std::filesystem::remove(file, ignored);
	return zipped;
}

std::string letter_of(const Request &request, const std::vector<work_log::Call> &calls, const Signer &sign,
	const std::string &sent_at) {
	const std::string zipped = zip_of(request, calls, sent_at);
	const std::string digest = platform::sha256_hex_digest(zipped);
	// This computer's word that it sent this log, inside the seal.
	const std::string statement = "singularity-work-log\nsha256:" + digest + "\norganisation:" + request.facts.organisation + "\nseat:"
		+ request.facts.seat + "\nsent:" + sent_at + "\n";
	const Json signature {
		{"algorithm", "Ed25519"},
		{"signed", statement},
		{"signature", sign ? sign(statement) : std::string()},
		{"devicePublicKey", request.facts.device_public_key},
	};
	std::size_t protectedCalls = 0, failedCalls = 0;
	for (const work_log::Call &call : calls) {
		if (call.is_protected == true) protectedCalls += 1;
		if (call.failure.empty() == false) failedCalls += 1;
	}
	const std::string stamp = sent_at.substr(0, std::min<std::size_t>(sent_at.size(), 10));
	const std::string about = one_line(request.note.empty() == false ? request.note : request.chat_title, 80);
	std::ostringstream summary;
	summary << "A work log sent from Singularity by a design partner (D-257).\r\n\r\n"
			<< "From: seat " << request.facts.seat << " of " << request.facts.organisation << "\r\n"
			<< "Sent: " << sent_at << "\r\n"
			<< "Singularity " << request.facts.app_version << ", " << request.facts.build << " build, " << request.facts.platform << "\r\n"
			<< "Conversation: " << one_line(request.chat_title, 200) << "\r\n"
			<< "Model calls: " << calls.size() << " (" << protectedCalls << " sent under client data protection, " << failedCalls
			<< " failed)\r\n";
	if (calls.empty() == false) summary << "From " << calls.front().at << " to " << calls.back().at << "\r\n";
	summary << "\r\nWhat the member says went wrong:\r\n" << (request.note.empty() == true ? std::string("(nothing written)") : request.note)
			<< "\r\n\r\nAttached: the log (log.json, and each call in calls/), and this computer's signature of its SHA-256 " << digest
			<< " (signature.json).\r\n";
	const std::string boundary = "singularity-log-" + random_hex(12);
	const std::string zipName = "singularity-work-log-" + stamp + ".zip";
	std::string letter;
	letter += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"; protected-headers=\"v1\"\r\n";
	letter += "From: Singularity <noreply@legalmachines.org>\r\n";
	letter += "To: diagnostics@legalmachines.org, claude@legalmachines.org\r\n";
	letter += "Subject: " + header_value("Work log from " + one_line(request.facts.organisation, 80) + (about.empty() == true ? "" : ": " + about))
		+ "\r\n";
	letter += "Date: " + mail_date(std::time(nullptr)) + "\r\n";
	letter += "Message-ID: <" + random_hex(16) + "@legalmachines.org>\r\n";
	letter += "MIME-Version: 1.0\r\n\r\n";
	letter += "--" + boundary + "\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
	letter += mime_base64(summary.str());
	letter += "--" + boundary + "\r\nContent-Type: application/zip; name=\"" + zipName + "\"\r\nContent-Disposition: attachment; filename=\""
		+ zipName + "\"\r\nContent-Transfer-Encoding: base64\r\n\r\n";
	letter += mime_base64(zipped);
	letter += "--" + boundary
		+ "\r\nContent-Type: application/json; name=\"signature.json\"\r\nContent-Disposition: attachment; filename=\"signature.json\"\r\n"
		  "Content-Transfer-Encoding: base64\r\n\r\n";
	letter += mime_base64(dump(signature));
	letter += "--" + boundary + "--\r\n";
	return letter;
}

Sent send(const Request &request, const Signer &sign, const Poster &post) {
	if (work_log::is_kept() == false) throw std::runtime_error("This computer keeps no work log: only a design partner's computers do.");
	if (recipients_ready() == false)
		throw std::runtime_error("Work logs can't be sent from this build yet: Legal Machines' keys for them are not in it.");
	const std::vector<openpgp::Recipient> sealedTo = recipients();
	std::vector<work_log::Call> calls = work_log::calls_of(request.matter_id, request.chat_id);
	if (calls.empty() == true) throw std::runtime_error("Nothing of this conversation's work is kept on this computer yet.");
	const std::string sentAt = work_log::now_text();
	const auto when = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
	// Too large for the intake, its earliest calls are left out, a quarter at a
	// time, and the log says how many.
	std::string armored;
	std::size_t leftOut = 0;
	for (;;) {
		Request trimmed = request;
		if (leftOut > 0)
			trimmed.note = (request.note.empty() == true ? std::string() : request.note + "\n\n") + "(The " + std::to_string(leftOut)
				+ " earliest model calls were left out to keep the log within 24 MB.)";
		armored = openpgp::armored(openpgp::seal(sealedTo, letter_of(trimmed, calls, sign, sentAt), "", when, openpgp::secure_random()));
		if (armored.size() <= kMostArmoredBytes) break;
		if (calls.size() <= 1) throw std::runtime_error("This conversation's work log is too large to send, even its last call alone.");
		const std::size_t drop = std::max<std::size_t>(1, calls.size() / 4);
		calls.erase(calls.begin(), calls.begin() + static_cast<std::ptrdiff_t>(drop));
		leftOut += drop;
	}
	const auto [status, body] = post(kIntakeUrl, "application/pgp-encrypted", armored);
	const Json answer = Json::parse(body, nullptr, false);
	const std::string said = answer.is_object() == true && answer.contains("error") == true && answer.at("error").is_string() == true
		? answer.at("error").get<std::string>()
		: std::string();
	if (status == 401) throw std::runtime_error("Sign in to Singularity again, then send the work log.");
	if (status == 403 || status == 413 || status == 429 || status == 400)
		throw std::runtime_error(said.empty() == true ? std::string("Legal Machines did not take the work log.") : said);
	if (status < 200 || status >= 300 || answer.is_object() == false)
		throw std::runtime_error("Legal Machines isn't answering. Try sending the work log again in a moment.");
	Sent sent;
	sent.sha256 = answer.value("sha256", std::string());
	sent.recipients = answer.value("recipients", 0);
	sent.bytes = armored.size();
	return sent;
}

} // namespace singularity::node::work_log_share
