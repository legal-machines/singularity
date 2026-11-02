#include "Office/OfficeConnection/OfficeConnection.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace singularity::office {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
// A pane hands over a file it files into a Matter (an Outlook attachment, the
// workbook or document open in Office) as Base64 in its call.
constexpr std::size_t kMaxBodyBytes = 48 * 1024 * 1024;
constexpr std::size_t kMaxConnections = 32;
constexpr int kIdleMilliseconds = 15000;
constexpr std::size_t kMaxQueuedEvents = 256;

std::string lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return text;
}

std::string trimmed(const std::string &text) {
	const auto first = text.find_first_not_of(" \t");
	if (first == std::string::npos) return {};
	const auto last = text.find_last_not_of(" \t");
	return text.substr(first, last - first + 1);
}

bool is_token(const std::string &text) {
	if (text.empty()) return false;
	return std::all_of(text.begin(), text.end(), [](unsigned char character) {
		return std::isalnum(character) || character == '-' || character == '_' || character == '.';
	});
}

// The events a paired pane receives: what changed in a Matter and its
// colleagues' work. Never the connection's own (a pairing asked for).
bool is_pane_event(const std::string &name) {
	return name == "matter.changed" || name == "agent.changed" || name == "operation.changed";
}

// ---- Sockets, on each system -------------------------------------------------------

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
void close_socket(SocketHandle socket) {
	closesocket(socket);
}
void end_socket(SocketHandle socket) {
	shutdown(socket, SD_BOTH);
}
bool timed_out() {
	const int error = WSAGetLastError();
	return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
}
void set_receive_timeout(SocketHandle socket, int milliseconds) {
	const DWORD value = static_cast<DWORD>(milliseconds);
	setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&value), sizeof(value));
	setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&value), sizeof(value));
}
void start_sockets() {
	static std::once_flag once;
	std::call_once(once, [] {
		WSADATA data{};
		WSAStartup(MAKEWORD(2, 2), &data);
	});
}
#else
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
void close_socket(SocketHandle socket) {
	::close(socket);
}
void end_socket(SocketHandle socket) {
	::shutdown(socket, SHUT_RDWR);
}
bool timed_out() {
	return errno == EAGAIN || errno == EWOULDBLOCK;
}
void set_receive_timeout(SocketHandle socket, int milliseconds) {
#if defined(SO_NOSIGPIPE)
	// A pane that closed its stream ends a write with an error, not a signal.
	int quiet = 1;
	setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &quiet, sizeof(quiet));
#endif
	timeval value{};
	value.tv_sec = milliseconds / 1000;
	value.tv_usec = (milliseconds % 1000) * 1000;
	setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
	setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}
void start_sockets() {
}
#endif

int send_to(void *context, const unsigned char *bytes, size_t length) {
	const SocketHandle socket = *static_cast<SocketHandle *>(context);
	const int chunk = static_cast<int>(std::min<size_t>(length, 64 * 1024));
#if defined(_WIN32)
	const int sent = ::send(socket, reinterpret_cast<const char *>(bytes), chunk, 0);
#else
#if defined(MSG_NOSIGNAL)
	const int flags = MSG_NOSIGNAL;
#else
	const int flags = 0;
#endif
	const int sent = static_cast<int>(::send(socket, bytes, static_cast<size_t>(chunk), flags));
#endif
	if (sent < 0) return timed_out() ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_SEND_FAILED;
	return sent;
}

int receive_from(void *context, unsigned char *bytes, size_t length) {
	const SocketHandle socket = *static_cast<SocketHandle *>(context);
	const int chunk = static_cast<int>(std::min<size_t>(length, 64 * 1024));
#if defined(_WIN32)
	const int received = ::recv(socket, reinterpret_cast<char *>(bytes), chunk, 0);
#else
	const int received = static_cast<int>(::recv(socket, bytes, static_cast<size_t>(chunk), 0));
#endif
	if (received == 0) return MBEDTLS_ERR_NET_CONN_RESET;
	if (received < 0) return timed_out() ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
	return received;
}

// ---- One connection's TLS ------------------------------------------------------------

// Everything a connection uses is its own: nothing of Mbed TLS is shared
// between threads (cmake/mbedtls/lm-mbedtls-config.h).
struct Tls {
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_ssl_config config;
	mbedtls_ssl_context ssl;
	mbedtls_x509_crt certificate;
	mbedtls_pk_context key;
	SocketHandle socket = kNoSocket;

	Tls() {
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		mbedtls_ssl_config_init(&config);
		mbedtls_ssl_init(&ssl);
		mbedtls_x509_crt_init(&certificate);
		mbedtls_pk_init(&key);
	}
	~Tls() {
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&config);
		mbedtls_pk_free(&key);
		mbedtls_x509_crt_free(&certificate);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
	}
	Tls(const Tls &) = delete;
	Tls &operator=(const Tls &) = delete;

	bool set_up(const std::string &certificate_pem, const std::string &key_pem) {
		static const char personal[] = "singularity-office-connection";
		if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, reinterpret_cast<const unsigned char *>(personal),
				sizeof(personal) - 1) != 0) return false;
		if (mbedtls_x509_crt_parse(&certificate, reinterpret_cast<const unsigned char *>(certificate_pem.c_str()),
				certificate_pem.size() + 1) != 0) return false;
		if (mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char *>(key_pem.c_str()), key_pem.size() + 1, nullptr, 0,
				mbedtls_ctr_drbg_random, &drbg) != 0) return false;
		if (mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
			return false;
		}
		mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
		mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
		if (mbedtls_ssl_conf_own_cert(&config, &certificate, &key) != 0) return false;
		if (mbedtls_ssl_setup(&ssl, &config) != 0) return false;
		mbedtls_ssl_set_bio(&ssl, &socket, send_to, receive_from, nullptr);
		return true;
	}

	bool handshake() {
		for (;;) {
			const int result = mbedtls_ssl_handshake(&ssl);
			if (result == 0) return true;
			if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
		}
	}

	bool write_all(const std::string &bytes) {
		std::size_t written = 0;
		while (written < bytes.size()) {
			const int result = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char *>(bytes.data() + written),
				bytes.size() - written);
			if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
			if (result <= 0) return false;
			written += static_cast<std::size_t>(result);
		}
		return true;
	}

	// More bytes of the request, or false when the connection ended or idled.
	bool read_more(std::string &buffer) {
		unsigned char chunk[16 * 1024];
		for (;;) {
			const int result = mbedtls_ssl_read(&ssl, chunk, sizeof(chunk));
			if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
			if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
			if (result <= 0) return false;
			buffer.append(reinterpret_cast<const char *>(chunk), static_cast<std::size_t>(result));
			return true;
		}
	}
};

// ---- Responses ---------------------------------------------------------------------------

const char *reason_of(int status) {
	switch (status) {
		case 200: return "OK";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 413: return "Payload Too Large";
		default: return "Internal Server Error";
	}
}

// What a pane page may load: its own files, and Office's own script, which
// Word, Excel and Outlook require from Microsoft's address. Outlook on macOS
// starts Office's script only with Microsoft's MicrosoftAjax.js, which that
// script loads from Microsoft's Ajax CDN: that one file is allowed, not the
// CDN, which also serves libraries a page could be turned with.
constexpr const char *kPanePolicy =
	"default-src 'self'; "
	"script-src 'self' https://appsforoffice.microsoft.com https://ajax.aspnetcdn.com/ajax/3.5/MicrosoftAjax.js; "
	"style-src 'self' 'unsafe-inline'; "
	"img-src 'self' data: https://appsforoffice.microsoft.com; "
	"font-src 'self' data:; "
	"connect-src 'self' https://appsforoffice.microsoft.com; "
	"frame-src 'self' https://appsforoffice.microsoft.com; "
	"object-src 'none'; base-uri 'none'; form-action 'none'; "
	"frame-ancestors 'self' https://*.office.com https://*.office365.com https://*.officeapps.live.com "
	"https://outlook.live.com https://*.outlook.com https://*.sharepoint.com https://*.microsoft.com";

// How long a client may keep an answer: data and the pages that show it
// never; the panes' pictures, fonts and scripts for an hour - Office keeps
// an add-in's icons only when it may.
enum class Keep { never, an_hour };

// An answer. For a HEAD request it says the length of what it would send,
// and sends nothing.
std::string response(int status, const std::string &content_type, const std::string &body, bool keep_alive,
	bool is_page = false, Keep keep = Keep::never, bool head_only = false) {
	std::ostringstream text;
	text << "HTTP/1.1 " << status << ' ' << reason_of(status) << "\r\n"
	     << "Content-Type: " << content_type << "\r\n"
	     << "Content-Length: " << body.size() << "\r\n"
	     << "Cache-Control: " << (keep == Keep::an_hour ? "private, max-age=3600" : "no-store") << "\r\n"
	     << "X-Content-Type-Options: nosniff\r\n"
	     << "Referrer-Policy: no-referrer\r\n";
	if (is_page) text << "Content-Security-Policy: " << kPanePolicy << "\r\n";
	text << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n\r\n";
	if (!head_only) text << body;
	return text.str();
}

// The status an answer begins with: "HTTP/1.1 200 OK" is 200.
int status_of(const std::string &reply) {
	if (reply.size() < 12 || reply.compare(0, 9, "HTTP/1.1 ") != 0) return 0;
	return std::atoi(reply.substr(9, 3).c_str());
}

std::string plain(int status, const std::string &words, bool keep_alive) {
	return response(status, "text/plain; charset=utf-8", words + "\n", keep_alive);
}

std::optional<std::string> read_file(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) return std::nullopt;
	std::ostringstream bytes;
	bytes << input.rdbuf();
	return bytes.str();
}

// One subscriber's events, waiting to be written to its stream.
struct EventQueue {
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<std::string> events;
	bool ended = false;
	// The kind of pane that reads it (pane_event_reaches).
	ClientKind kind = ClientKind::word;
};

} // namespace

// ---- The protocol ----------------------------------------------------------------------

ParseState parse_request(const std::string &bytes, HttpRequest &request, std::size_t &consumed, std::size_t max_body) {
	const std::size_t end = bytes.find("\r\n\r\n");
	if (end == std::string::npos) return bytes.size() > kMaxHeaderBytes ? ParseState::too_large : ParseState::incomplete;
	if (end > kMaxHeaderBytes) return ParseState::too_large;

	HttpRequest parsed;
	std::size_t line_end = bytes.find("\r\n");
	const std::string request_line = bytes.substr(0, line_end);
	const std::size_t first_space = request_line.find(' ');
	const std::size_t second_space = first_space == std::string::npos ? std::string::npos : request_line.find(' ', first_space + 1);
	if (first_space == std::string::npos || second_space == std::string::npos) return ParseState::invalid;
	parsed.method = request_line.substr(0, first_space);
	parsed.target = request_line.substr(first_space + 1, second_space - first_space - 1);
	const std::string version = request_line.substr(second_space + 1);
	if (version != "HTTP/1.1" && version != "HTTP/1.0") return ParseState::invalid;
	if (!std::all_of(parsed.method.begin(), parsed.method.end(), [](unsigned char character) { return std::isupper(character); })
		|| parsed.method.empty()) {
		return ParseState::invalid;
	}
	if (parsed.target.empty() || parsed.target.front() != '/') return ParseState::invalid;
	parsed.path = parsed.target.substr(0, parsed.target.find('?'));

	std::size_t position = line_end + 2;
	while (position < end) {
		const std::size_t next = bytes.find("\r\n", position);
		const std::string line = bytes.substr(position, next - position);
		position = next + 2;
		if (line.empty()) break;
		if (line.front() == ' ' || line.front() == '\t') return ParseState::invalid;
		const std::size_t colon = line.find(':');
		if (colon == std::string::npos) return ParseState::invalid;
		const std::string name = lower(line.substr(0, colon));
		if (!is_token(name)) return ParseState::invalid;
		const std::string value = trimmed(line.substr(colon + 1));
		if (parsed.headers.count(name) != 0) {
			// One Host, one length: a second of either is not ours to choose between.
			if (name == "host" || name == "content-length") return ParseState::invalid;
			parsed.headers[name] += ", " + value;
		} else {
			parsed.headers[name] = value;
		}
	}
	if (parsed.headers.count("transfer-encoding") != 0) return ParseState::invalid;

	std::size_t length = 0;
	if (const auto found = parsed.headers.find("content-length"); found != parsed.headers.end()) {
		const std::string &digits = found->second;
		if (digits.empty() || digits.size() > 10
			|| !std::all_of(digits.begin(), digits.end(), [](unsigned char character) { return std::isdigit(character); })) {
			return ParseState::invalid;
		}
		const unsigned long long value = std::stoull(digits);
		if (value > max_body) return ParseState::too_large;
		length = static_cast<std::size_t>(value);
	}
	const std::size_t body_start = end + 4;
	if (bytes.size() < body_start + length) return ParseState::incomplete;
	parsed.body = bytes.substr(body_start, length);
	consumed = body_start + length;
	request = std::move(parsed);
	return ParseState::complete;
}

std::optional<std::string> refusal_of(const HttpRequest &request, int port) {
	const auto header = [&request](const char *name) -> std::string {
		const auto found = request.headers.find(name);
		return found == request.headers.end() ? std::string() : found->second;
	};
	const std::string suffix = ":" + std::to_string(port);
	const std::string host = lower(header("host"));
	if (host != "localhost" + suffix && host != "127.0.0.1" + suffix) {
		return "This address is not Singularity's on this computer.";
	}
	if (request.path.rfind("/api/", 0) != 0) return std::nullopt;

	// A browser says where a call comes from: only the pane's own origin may
	// call, never another page, even one open on this computer.
	const std::string site = lower(header("sec-fetch-site"));
	if (!site.empty() && site != "same-origin") return "Only Singularity's own pane may call it.";
	const std::string origin = lower(header("origin"));
	if (!origin.empty() && origin != "https://localhost" + suffix && origin != "https://127.0.0.1" + suffix) {
		return "Only Singularity's own pane may call it.";
	}
	if (request.method == "POST") {
		// A header no page can send to another site without asking it first.
		if (header("x-lm-office") != "1") return "Only Singularity's own pane may call it.";
		if (lower(header("content-type")).rfind("application/json", 0) != 0) return "A call is JSON.";
	}
	return std::nullopt;
}

bool pane_event_reaches(const std::string &name, ClientKind kind) {
	if (name == "office.reveal") return kind == ClientKind::word || kind == ClientKind::excel || kind == ClientKind::powerpoint;
	return is_pane_event(name);
}

bool is_carried_method(const std::string &method) {
	return may_call(ClientKind::word, method) || may_call(ClientKind::excel, method) || may_call(ClientKind::outlook, method)
		|| may_call(ClientKind::powerpoint, method);
}

std::optional<std::string> matter_named_by(const std::string &json_params) {
	const Json params = Json::parse(json_params, nullptr, false);
	if (!params.is_object()) return std::nullopt;
	for (const char *key : {"projectId", "matterId"}) {
		if (params.contains(key) && params.at(key).is_string() && !params.at(key).get<std::string>().empty()) {
			return params.at(key).get<std::string>();
		}
	}
	return std::nullopt;
}

std::optional<std::filesystem::path> static_file_for(const std::filesystem::path &root, const std::string &path) {
	static const std::string prefix = "/office/";
	if (path.rfind(prefix, 0) != 0) return std::nullopt;
	std::string relative = path.substr(prefix.size());
	if (relative.empty()) relative = "index.html";
	if (relative.find_first_of("\\:%") != std::string::npos || relative.find('\0') != std::string::npos) return std::nullopt;
	std::stringstream segments(relative);
	std::string segment;
	std::filesystem::path file = root;
	while (std::getline(segments, segment, '/')) {
		if (segment.empty() || segment == "." || segment == "..") return std::nullopt;
		file /= std::filesystem::u8path(segment);
	}
	if (!relative.empty() && relative.back() == '/') return std::nullopt;
	std::error_code error;
	if (!std::filesystem::is_regular_file(file, error)) return std::nullopt;
	const auto inside = std::filesystem::weakly_canonical(file, error);
	const auto base = std::filesystem::weakly_canonical(root, error);
	if (error) return std::nullopt;
	const std::string base_text = base.generic_u8string();
	const std::string inside_text = inside.generic_u8string();
	if (inside_text.rfind(base_text + "/", 0) != 0) return std::nullopt;
	return file;
}

std::string content_type_of(const std::filesystem::path &file) {
	const std::string extension = lower(file.extension().u8string());
	if (extension == ".html") return "text/html; charset=utf-8";
	if (extension == ".js") return "text/javascript; charset=utf-8";
	if (extension == ".css") return "text/css; charset=utf-8";
	if (extension == ".json") return "application/json; charset=utf-8";
	if (extension == ".png") return "image/png";
	if (extension == ".svg") return "image/svg+xml";
	if (extension == ".ico") return "image/x-icon";
	if (extension == ".webp") return "image/webp";
	if (extension == ".woff2") return "font/woff2";
	if (extension == ".woff") return "font/woff";
	return "application/octet-stream";
}

std::string pane_transport_script() {
	return R"js((() => {
  // Singularity's Office connection (Office/OfficeConnection): the window's
  // own page script runs on it, so a pane's window.nativeApp is the window's.
  // A pane is paired once with Singularity's window; its token stays in this
  // origin's storage, and every call carries it with a moment and a nonce.
  const KEY = 'lm-office-token';
  const delivery = '__native' + '_event';
  const unreachable = {
    code: 'native.transport_failed',
    message: 'Singularity on this computer did not answer.',
    userMessage: 'Singularity on this computer did not answer. Open it, and try again.',
    retryable: true,
  };
  const notPaired = {
    code: 'office.not_paired',
    message: 'This pane is not connected to Singularity.',
    userMessage: 'Connect this pane to Singularity first.',
    retryable: false,
  };
  const read = () => { try { return localStorage.getItem(KEY) || ''; } catch (_) { return ''; } };
  const keep = (value) => { try { if (value) localStorage.setItem(KEY, value); else localStorage.removeItem(KEY); } catch (_) {} };
  const nonce = () => Array.from(crypto.getRandomValues(new Uint8Array(16)), (byte) => byte.toString(16).padStart(2, '0')).join('');
  const plain = { 'Content-Type': 'application/json', 'X-LM-Office': '1' };
  const signed = () => ({ ...plain, Authorization: 'Bearer ' + read(), 'X-LM-Time': String(Date.now()), 'X-LM-Nonce': nonce() });
  // The pane's own notice that its pairing ended, which the pane listens for.
  const unpairedNotice = 'lm-office-' + 'unpaired';
  const unpaired = () => { keep(''); window.dispatchEvent(new CustomEvent(unpairedNotice)); };
  const post = async (path, body, headers) => {
    try {
      return await fetch(path, { method: 'POST', headers, body: JSON.stringify(body), cache: 'no-store', credentials: 'same-origin' });
    } catch (_) {
      throw unreachable;
    }
  };
  const pause = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
  // The request this origin made and still waits on, kept beside the token: a
  // pane opened again waits on it instead of asking anew, which Singularity
  // refuses while one of its kind stands.
  const asked = (client) => 'lm-office-pairing-' + client;
  const readAsked = (client) => {
    try {
      const kept = JSON.parse(localStorage.getItem(asked(client)) || 'null');
      return kept && kept.until > Date.now() ? kept : null;
    } catch (_) { return null; }
  };
  const keepAsked = (client, value) => {
    try { if (value) localStorage.setItem(asked(client), JSON.stringify(value)); else localStorage.removeItem(asked(client)); } catch (_) {}
  };
  const waitOn = (client, request) => async () => {
    for (;;) {
      await pause(1200);
      // Allowed in another pane of this origin meanwhile: its token is this one's.
      if (read() !== '') { keepAsked(client, null); return 'allowed'; }
      const finished = await post('/api/pair/finish', { pairingId: request.pairingId, secret: request.secret }, plain);
      if (!finished.ok) throw unreachable;
      const outcome = await finished.json();
      if (outcome.state === 'allowed') { keep(outcome.token); keepAsked(client, null); return 'allowed'; }
      if (outcome.state !== 'waiting') { keepAsked(client, null); return outcome.state; }
    }
  };
  window.__nativeInvoke = async (method, params) => {
    if (read() === '') throw notPaired;
    const response = await post('/api/call', { method, params: params ?? {} }, signed());
    if (response.status === 401) { unpaired(); throw notPaired; }
    if (!response.ok) throw unreachable;
    const answer = await response.json();
    if (answer.status === 0) return answer.result;
    throw answer.error;
  };
  window.__lmOffice = {
    isPaired: () => read() !== '',
    forget: () => keep(''),
    // Asks Singularity to pair this pane: the code to show, and a wait()
    // that ends when the person answered in Singularity's window.
    pair: async (client, label) => {
      const kept = readAsked(client);
      if (kept) return { state: 'started', code: kept.code, wait: waitOn(client, kept) };
      const response = await post('/api/pair/start', { client, label }, plain);
      if (!response.ok) throw unreachable;
      const started = await response.json();
      if (started.state !== 'started') return { state: started.state };
      // Kept a little short of the three minutes the request lasts.
      const request = { pairingId: started.pairingId, secret: started.secret, code: started.code, until: Date.now() + 170000 };
      keepAsked(client, request);
      return { state: 'started', code: started.code, wait: waitOn(client, request) };
    },
    // The window's events, while this pane is paired.
    listen: () => {
      let stopped = false;
      (async () => {
        while (!stopped) {
          if (read() === '') { await pause(2000); continue; }
          try {
            const response = await post('/api/events', {}, signed());
            if (response.status === 401) { unpaired(); continue; }
            if (response.ok && response.body) {
              const reader = response.body.getReader();
              const decoder = new TextDecoder();
              let buffer = '';
              for (;;) {
                const { value, done } = await reader.read();
                if (done) break;
                buffer += decoder.decode(value, { stream: true });
                let end;
                while ((end = buffer.indexOf('\n\n')) >= 0) {
                  const block = buffer.slice(0, end);
                  buffer = buffer.slice(end + 2);
                  const data = block.split('\n').filter((line) => line.startsWith('data: ')).map((line) => line.slice(6)).join('\n');
                  if (data) {
                    try { window.dispatchEvent(new CustomEvent(delivery, { detail: JSON.parse(data) })); } catch (_) {}
                  }
                }
              }
            }
          } catch (_) {}
          await pause(1500);
        }
      })();
      return () => { stopped = true; };
    },
  };
})();
)js";
}

// ---- The server ----------------------------------------------------------------------------

struct OfficeServer::Impl {
	Carrier carrier;
	std::filesystem::path content_root;
	Gate gate;
	int port = 0;
	std::string certificate_pem;
	std::string key_pem;

	SocketHandle listener = kNoSocket;
	std::thread acceptor;
	std::atomic<bool> stopping{false};
	std::atomic<bool> listening{false};

	std::mutex connections_mutex;
	std::condition_variable connections_ended;
	std::set<SocketHandle> connections;

	std::mutex queues_mutex;
	std::vector<std::shared_ptr<EventQueue>> queues;
	int subscription = -1;

	std::mutex log_mutex;

	// One line for each answer: what reached the connection, for the person's
	// and an auditor's eyes. Never a body, a query or a header but the
	// client's own name for itself.
	void record(const HttpRequest &request, int status) {
		if (gate.access_log.empty()) return;
		const auto agent = request.headers.find("user-agent");
		std::string client = agent == request.headers.end() ? std::string("-") : agent->second.substr(0, 120);
		for (char &character : client) {
			if (static_cast<unsigned char>(character) < 0x20 || character == '"') character = ' ';
		}
		const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm parts{};
#if defined(_WIN32)
		gmtime_s(&parts, &seconds);
#else
		gmtime_r(&seconds, &parts);
#endif
		char moment[32] = {};
		std::strftime(moment, sizeof(moment), "%Y-%m-%dT%H:%M:%SZ", &parts);
		const std::lock_guard<std::mutex> lock(log_mutex);
		std::error_code error;
		if (std::filesystem::file_size(gate.access_log, error) > 512 * 1024 && !error) {
			std::filesystem::rename(gate.access_log, gate.access_log.string() + ".1", error);
		}
		std::ofstream log(gate.access_log, std::ios::binary | std::ios::app);
		log << moment << ' ' << request.method << ' ' << request.path << ' ' << status << " \"" << client << "\"\n";
	}

	// Sends an answer and records it.
	bool deliver(Tls &tls, const HttpRequest &request, const std::string &reply) {
		record(request, status_of(reply));
		return tls.write_all(reply);
	}

	void accept_loop() {
		while (!stopping) {
			fd_set readable;
			FD_ZERO(&readable);
			FD_SET(listener, &readable);
			timeval wait{};
			wait.tv_usec = 400 * 1000;
#if defined(_WIN32)
			const int ready = select(0, &readable, nullptr, nullptr, &wait);
#else
			const int ready = select(listener + 1, &readable, nullptr, nullptr, &wait);
#endif
			if (stopping) break;
			if (ready <= 0) continue;
			sockaddr_in peer{};
#if defined(_WIN32)
			int peer_size = sizeof(peer);
#else
			socklen_t peer_size = sizeof(peer);
#endif
			const SocketHandle socket = ::accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_size);
			if (socket == kNoSocket) continue;
			// Loopback only: the listener is bound to it, and is checked again.
			if (peer.sin_family != AF_INET || ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
				close_socket(socket);
				continue;
			}
			{
				const std::lock_guard<std::mutex> lock(connections_mutex);
				if (connections.size() >= kMaxConnections || stopping) {
					close_socket(socket);
					continue;
				}
				connections.insert(socket);
			}
			std::thread([this, socket] {
				try {
					serve(socket);
				} catch (...) {
				}
				close_socket(socket);
				const std::lock_guard<std::mutex> lock(connections_mutex);
				connections.erase(socket);
				connections_ended.notify_all();
			}).detach();
		}
	}

	void serve(SocketHandle socket) {
		set_receive_timeout(socket, kIdleMilliseconds);
		Tls tls;
		tls.socket = socket;
		if (!tls.set_up(certificate_pem, key_pem) || !tls.handshake()) return;
		std::string buffer;
		while (!stopping) {
			HttpRequest request;
			std::size_t consumed = 0;
			ParseState state = parse_request(buffer, request, consumed, kMaxBodyBytes);
			while (state == ParseState::incomplete) {
				if (stopping || !tls.read_more(buffer)) return;
				state = parse_request(buffer, request, consumed, kMaxBodyBytes);
			}
			if (state == ParseState::too_large) {
				(void)deliver(tls, request, plain(413, "Too large.", false));
				return;
			}
			if (state == ParseState::invalid) {
				(void)tls.write_all(plain(400, "Not a request Singularity reads.", false));
				return;
			}
			buffer.erase(0, consumed);
			const bool closes = lower(request.headers.count("connection") ? request.headers["connection"] : "") == "close";
			if (!answer(tls, request, !closes) || closes) break;
		}
		mbedtls_ssl_close_notify(&tls.ssl);
	}

	// Answers one request; false when the connection ends with it.
	bool answer(Tls &tls, const HttpRequest &request, bool keep_alive) {
		if (const auto refusal = refusal_of(request, port)) {
			(void)deliver(tls, request, plain(403, *refusal, false));
			return false;
		}

		if (request.path.rfind("/api/", 0) == 0) {
			if (request.method != "POST") return deliver(tls, request, plain(405, "The connection is called with POST.", keep_alive)) && keep_alive;
			if (request.path == "/api/pair/start") return deliver(tls, request, json_reply(200, pair_start(request.body), keep_alive)) && keep_alive;
			if (request.path == "/api/pair/finish") return deliver(tls, request, json_reply(200, pair_finish(request.body), keep_alive)) && keep_alive;
			const auto client = paired_client(request);
			if (!client) return deliver(tls, request, json_reply(401, not_paired(), keep_alive)) && keep_alive;
			if (request.path == "/api/call") return deliver(tls, request, json_reply(200, call(*client, request.body), keep_alive)) && keep_alive;
			if (request.path == "/api/events") {
				record(request, 200);
				stream_events(tls, client->kind);
				return false;
			}
			return deliver(tls, request, plain(404, "Nothing here.", keep_alive)) && keep_alive;
		}
		if (request.method != "GET" && request.method != "HEAD") {
			return deliver(tls, request, plain(405, "Only reading is allowed here.", keep_alive)) && keep_alive;
		}
		const bool head_only = request.method == "HEAD";
		if (request.path == "/office/native-bridge.js") {
			const std::string script = pane_transport_script() + "\n" + carrier.page_script
				+ "\nif (window.__lmOffice) window.__lmOffice.listen();\n";
			return deliver(tls, request, response(200, "text/javascript; charset=utf-8", script, keep_alive, false, Keep::never, head_only)) && keep_alive;
		}
		// The colleagues' faces are the window's own (frontend-dist/characters).
		static const std::string faces = "/office/characters/";
		const auto file = request.path.rfind(faces, 0) == 0
			? static_file_for(content_root.parent_path() / "characters", "/office/" + request.path.substr(faces.size()))
			: static_file_for(content_root, request.path == "/office" ? "/office/" : request.path);
		if (!file) return deliver(tls, request, plain(404, "Nothing here.", keep_alive)) && keep_alive;
		const auto bytes = read_file(*file);
		if (!bytes) return deliver(tls, request, plain(404, "Nothing here.", keep_alive)) && keep_alive;
		const std::string type = content_type_of(*file);
		const bool is_page = type.rfind("text/html", 0) == 0;
		return deliver(tls, request, response(200, type, *bytes, keep_alive, is_page, is_page ? Keep::never : Keep::an_hour, head_only))
			&& keep_alive;
	}

	static std::string json_reply(int status, const std::string &body, bool keep_alive) {
		return response(status, "application/json; charset=utf-8", body, keep_alive);
	}

	static std::string refused(const std::string &code, const std::string &message, const std::string &user_message) {
		return Json{{"status", 1}, {"error", {{"code", code}, {"message", message}, {"userMessage", user_message},
			{"retryable", false}}}}.dump();
	}

	static std::string not_paired() {
		return refused("office.not_paired", "This pane is not connected to Singularity, or its connection was removed.",
			"Connect this pane to Singularity first.");
	}

	static std::string header_of(const HttpRequest &request, const char *name) {
		const auto found = request.headers.find(name);
		return found == request.headers.end() ? std::string() : found->second;
	}

	// The paired client a call comes from: its token, and a moment and a nonce
	// that make the call this one, now. Nothing when any is missing or used.
	std::optional<PairedClient> paired_client(const HttpRequest &request) const {
		if (!gate.clients) return std::nullopt;
		const std::string authorization = header_of(request, "authorization");
		if (authorization.rfind("Bearer ", 0) != 0) return std::nullopt;
		const auto client = gate.clients->authorize(authorization.substr(7));
		if (!client) return std::nullopt;
		if (!gate.clients->is_fresh(client->id, header_of(request, "x-lm-time"), header_of(request, "x-lm-nonce"))) return std::nullopt;
		return client;
	}

	std::string pair_start(const std::string &body) const {
		const Json request = Json::parse(body, nullptr, false);
		const auto kind = request.is_object() ? client_kind_of(request.value("client", std::string())) : std::nullopt;
		if (!kind || !gate.clients) return Json{{"state", "refused"}}.dump();
		const auto started = gate.clients->begin_pairing(*kind, request.value("label", std::string()));
		if (!started) return Json{{"state", "busy"}}.dump();
		if (gate.pairing_requested) gate.pairing_requested();
		return Json{{"state", "started"}, {"pairingId", started->id}, {"code", started->code}, {"secret", started->secret}}.dump();
	}

	std::string pair_finish(const std::string &body) const {
		const Json request = Json::parse(body, nullptr, false);
		if (!request.is_object() || !gate.clients) return Json{{"state", "unknown"}}.dump();
		const auto outcome = gate.clients->finish_pairing(request.value("pairingId", std::string()), request.value("secret", std::string()));
		Json answer{{"state", outcome.state}};
		if (!outcome.token.empty()) answer["token"] = outcome.token;
		return answer.dump();
	}

	std::string call(const PairedClient &client, const std::string &body) const {
		const Json request = Json::parse(body, nullptr, false);
		if (!request.is_object() || !request.contains("method") || !request.at("method").is_string()) {
			return refused("native.invalid_request", "Expected {method, params}.", "");
		}
		const std::string method = request.at("method").get<std::string>();
		const Json params = request.contains("params") && request.at("params").is_object() ? request.at("params") : Json::object();
		if (!may_call(client.kind, method)) {
			return refused("native.method_not_found", "The Office connection does not carry " + method + " for this client.", "");
		}
		const std::string params_text = params.dump();
		if (const auto matter = matter_named_by(params_text)) {
			if (!gate.is_matter_here || !gate.is_matter_here(*matter)) {
				return refused("office.matter_not_here", "That Matter is not open on this computer.",
					"That Matter is not open in Singularity on this computer.");
			}
		}
		const auto [status, json] = carrier.call(method, params_text);
		return std::string("{\"status\":") + (status == 0 ? "0,\"result\":" : "1,\"error\":") + json + "}";
	}

	void stream_events(Tls &tls, ClientKind kind) {
		auto queue = std::make_shared<EventQueue>();
		queue->kind = kind;
		{
			const std::lock_guard<std::mutex> lock(queues_mutex);
			queues.push_back(queue);
		}
		const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: keep-alive\r\n\r\n: open\n\n";
		bool open = tls.write_all(head);
		while (open && !stopping) {
			std::deque<std::string> ready;
			{
				std::unique_lock<std::mutex> lock(queue->mutex);
				queue->changed.wait_for(lock, std::chrono::seconds(15), [&queue, this] {
					return !queue->events.empty() || queue->ended || stopping;
				});
				if (queue->ended) break;
				ready.swap(queue->events);
			}
			if (ready.empty()) {
				open = tls.write_all(": still here\n\n");
				continue;
			}
			std::string text;
			for (const std::string &event : ready) text += "data: " + event + "\n\n";
			open = tls.write_all(text);
		}
		const std::lock_guard<std::mutex> lock(queues_mutex);
		queues.erase(std::remove(queues.begin(), queues.end(), queue), queues.end());
	}

	void publish(const std::string &name, const std::string &payload) {
		if (!is_pane_event(name) && name != "office.reveal") return;
		const std::string event = Json{{"name", name}, {"payload", Json::parse(payload, nullptr, false)}}.dump();
		const std::lock_guard<std::mutex> lock(queues_mutex);
		for (const auto &queue : queues) {
			if (!pane_event_reaches(name, queue->kind)) continue;
			const std::lock_guard<std::mutex> queued(queue->mutex);
			queue->events.push_back(event);
			while (queue->events.size() > kMaxQueuedEvents) queue->events.pop_front();
			queue->changed.notify_all();
		}
	}
};

OfficeServer::OfficeServer(Carrier carrier, std::filesystem::path content_root, Gate gate) : impl_(std::make_unique<Impl>()) {
	impl_->carrier = std::move(carrier);
	impl_->content_root = std::move(content_root);
	impl_->gate = std::move(gate);
}

OfficeServer::~OfficeServer() {
	stop();
}

void OfficeServer::start(int port, const std::string &certificate_pem, const std::string &key_pem) {
	if (impl_->listening) return;
	start_sockets();
	impl_->port = port;
	impl_->certificate_pem = certificate_pem;
	impl_->key_pem = key_pem;
	impl_->stopping = false;

	const SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == kNoSocket) throw std::runtime_error("The Office connection could not open a socket.");
#if defined(_WIN32)
	// No other program may take the same port while Singularity holds it.
	BOOL exclusive = TRUE;
	setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
#else
	int reuse = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<unsigned short>(port));
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listener, 16) != 0) {
		close_socket(listener);
		throw std::runtime_error("Another program on this computer holds the Office connection's port, "
			+ std::to_string(port) + ". Close it, or the other Singularity that holds it, and connect again.");
	}
	impl_->listener = listener;
	if (impl_->carrier.subscribe) {
		impl_->subscription = impl_->carrier.subscribe([impl = impl_.get()](const std::string &name, const std::string &payload) {
			impl->publish(name, payload);
		});
	}
	impl_->listening = true;
	impl_->acceptor = std::thread([impl = impl_.get()] { impl->accept_loop(); });
}

void OfficeServer::stop() {
	if (!impl_ || !impl_->listening) return;
	impl_->stopping = true;
	if (impl_->acceptor.joinable()) impl_->acceptor.join();
	close_socket(impl_->listener);
	impl_->listener = kNoSocket;
	if (impl_->subscription >= 0 && impl_->carrier.unsubscribe) impl_->carrier.unsubscribe(impl_->subscription);
	impl_->subscription = -1;
	{
		const std::lock_guard<std::mutex> lock(impl_->queues_mutex);
		for (const auto &queue : impl_->queues) {
			const std::lock_guard<std::mutex> queued(queue->mutex);
			queue->ended = true;
			queue->changed.notify_all();
		}
	}
	std::unique_lock<std::mutex> lock(impl_->connections_mutex);
	for (const SocketHandle socket : impl_->connections) end_socket(socket);
	impl_->connections_ended.wait_for(lock, std::chrono::seconds(5), [this] { return impl_->connections.empty(); });
	impl_->listening = false;
}

bool OfficeServer::is_listening() const {
	return impl_ && impl_->listening;
}

} // namespace singularity::office
