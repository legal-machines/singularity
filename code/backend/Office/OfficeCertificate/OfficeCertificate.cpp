#include "Office/OfficeCertificate/OfficeCertificate.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"

#include <mbedtls/asn1.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace singularity::office {
namespace {

constexpr const char *kCertificateFile = "localhost-certificate.pem";
constexpr const char *kKeyFile = "localhost-key.pem";

// Its name says what it is to anyone who finds it in their system's list of
// trusted certificates.
constexpr const char *kServerName = "CN=localhost,OU=Singularity panes on this computer,O=Legal Machines";

void check(int result, const char *what) {
	if (result != 0) {
		char reason[160] = {};
		std::snprintf(reason, sizeof(reason), "%s failed (Mbed TLS -0x%04X).", what, static_cast<unsigned>(-result));
		throw std::runtime_error(reason);
	}
}

std::tm utc_of(std::chrono::system_clock::time_point moment) {
	const std::time_t seconds = std::chrono::system_clock::to_time_t(moment);
	std::tm parts{};
#if defined(_WIN32)
	gmtime_s(&parts, &seconds);
#else
	gmtime_r(&seconds, &parts);
#endif
	return parts;
}

std::string certificate_time(std::chrono::system_clock::time_point moment) {
	const std::tm parts = utc_of(moment);
	char text[16] = {};
	std::snprintf(text, sizeof(text), "%04d%02d%02d%02d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
		parts.tm_hour, parts.tm_min, parts.tm_sec);
	return text;
}

// The random generator for one making, seeded from the system's entropy.
struct Random {
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	Random() {
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		static const char personal[] = "singularity-office-certificate";
		check(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
			reinterpret_cast<const unsigned char *>(personal), sizeof(personal) - 1), "Seeding the random generator");
	}
	~Random() {
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
	}
	Random(const Random &) = delete;
	Random &operator=(const Random &) = delete;
};

struct Key {
	mbedtls_pk_context pk;
	Key() { mbedtls_pk_init(&pk); }
	~Key() { mbedtls_pk_free(&pk); }
	Key(const Key &) = delete;
	Key &operator=(const Key &) = delete;
};

void generate(Key &key, Random &random) {
	check(mbedtls_pk_setup(&key.pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)), "Setting up a key");
	check(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key.pk), mbedtls_ctr_drbg_random, &random.drbg),
		"Making a key");
}

struct Written {
	std::string pem;
	std::string der;
};

// The one server certificate, signed by its own key: no authority.
Written write_certificate(Key &key, const std::string &not_before, const std::string &not_after, Random &random) {
	mbedtls_x509write_cert certificate;
	mbedtls_x509write_crt_init(&certificate);
	struct Release {
		mbedtls_x509write_cert *value;
		~Release() { mbedtls_x509write_crt_free(value); }
	} release{&certificate};

	mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
	mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
	mbedtls_x509write_crt_set_subject_key(&certificate, &key.pk);
	mbedtls_x509write_crt_set_issuer_key(&certificate, &key.pk);
	check(mbedtls_x509write_crt_set_subject_name(&certificate, kServerName), "Naming the certificate");
	check(mbedtls_x509write_crt_set_issuer_name(&certificate, kServerName), "Naming its issuer");

	std::array<unsigned char, 16> serial{};
	check(mbedtls_ctr_drbg_random(&random.drbg, serial.data(), serial.size()), "Making a serial number");
	serial[0] = static_cast<unsigned char>((serial[0] & 0x3F) | 0x40);
	check(mbedtls_x509write_crt_set_serial_raw(&certificate, serial.data(), serial.size()), "Setting the serial number");
	check(mbedtls_x509write_crt_set_validity(&certificate, not_before.c_str(), not_after.c_str()), "Setting its validity");

	// No authority: it signs nothing, and serves only.
	check(mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1), "Marking the server certificate");
	check(mbedtls_x509write_crt_set_key_usage(&certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE), "Setting its key usage");
	mbedtls_asn1_sequence server_auth{};
	server_auth.buf.tag = MBEDTLS_ASN1_OID;
	server_auth.buf.p = reinterpret_cast<unsigned char *>(const_cast<char *>(MBEDTLS_OID_SERVER_AUTH));
	server_auth.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
	check(mbedtls_x509write_crt_set_ext_key_usage(&certificate, &server_auth), "Setting its use for servers");

	// localhost, and the loopback address it is reached at.
	static const char host[] = "localhost";
	static unsigned char loopback[4] = {127, 0, 0, 1};
	mbedtls_x509_san_list address{};
	address.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
	address.node.san.unstructured_name.p = loopback;
	address.node.san.unstructured_name.len = sizeof(loopback);
	address.next = nullptr;
	mbedtls_x509_san_list name{};
	name.node.type = MBEDTLS_X509_SAN_DNS_NAME;
	name.node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(const_cast<char *>(host));
	name.node.san.unstructured_name.len = sizeof(host) - 1;
	name.next = &address;
	check(mbedtls_x509write_crt_set_subject_alternative_name(&certificate, &name), "Naming localhost");
	check(mbedtls_x509write_crt_set_subject_key_identifier(&certificate), "Identifying its key");
	check(mbedtls_x509write_crt_set_authority_key_identifier(&certificate), "Identifying its issuer's key");

	Written written;
	std::vector<unsigned char> buffer(4096);
	const int length = mbedtls_x509write_crt_der(&certificate, buffer.data(), buffer.size(), mbedtls_ctr_drbg_random, &random.drbg);
	if (length < 0) check(length, "Writing the certificate");
	written.der.assign(reinterpret_cast<const char *>(buffer.data() + buffer.size() - static_cast<std::size_t>(length)),
		static_cast<std::size_t>(length));
	std::vector<unsigned char> pem(8192);
	check(mbedtls_x509write_crt_pem(&certificate, pem.data(), pem.size(), mbedtls_ctr_drbg_random, &random.drbg),
		"Writing the certificate as text");
	written.pem = reinterpret_cast<const char *>(pem.data());
	return written;
}

std::string key_pem(Key &key) {
	std::vector<unsigned char> pem(4096);
	check(mbedtls_pk_write_key_pem(&key.pk, pem.data(), pem.size()), "Writing the key");
	return reinterpret_cast<const char *>(pem.data());
}

std::optional<std::string> read_text(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) return std::nullopt;
	std::stringstream text;
	text << input.rdbuf();
	return text.str();
}

// A PEM certificate's DER bytes and its end, or nullopt when it cannot be read.
struct Parsed {
	std::string der;
	std::string not_after;
};

std::optional<Parsed> parse_pem(const std::string &pem) {
	mbedtls_x509_crt certificate;
	mbedtls_x509_crt_init(&certificate);
	const int result = mbedtls_x509_crt_parse(&certificate, reinterpret_cast<const unsigned char *>(pem.c_str()), pem.size() + 1);
	if (result != 0) {
		mbedtls_x509_crt_free(&certificate);
		return std::nullopt;
	}
	Parsed parsed;
	parsed.der.assign(reinterpret_cast<const char *>(certificate.raw.p), certificate.raw.len);
	char end[16] = {};
	const mbedtls_x509_time &to = certificate.valid_to;
	std::snprintf(end, sizeof(end), "%04d%02d%02d%02d%02d%02d", to.year, to.mon, to.day, to.hour, to.min, to.sec);
	parsed.not_after = end;
	mbedtls_x509_crt_free(&certificate);
	return parsed;
}

} // namespace

std::string certificate_time_in_days(int days) {
	return certificate_time(std::chrono::system_clock::now() + std::chrono::hours(24) * days);
}

LocalCertificate make_local_certificate(int days) {
	if (days <= 0) throw std::invalid_argument("A certificate must run for at least a day.");
	Random random;
	const auto now = std::chrono::system_clock::now();
	const std::string not_before = certificate_time(now - std::chrono::hours(24));
	const std::string not_after = certificate_time(now + std::chrono::hours(24) * days);
	Key key;
	generate(key, random);
	const Written written = write_certificate(key, not_before, not_after, random);
	LocalCertificate made;
	made.certificate_pem = written.pem;
	made.certificate_der = written.der;
	made.key_pem = key_pem(key);
	made.not_after = not_after;
	return made;
}

void save_local_certificate(const std::filesystem::path &folder, const LocalCertificate &certificate) {
	platform::ensure_record_directory(folder);
	(void)platform::restrict_to_owner(folder);
	platform::write_file_durably(folder / kKeyFile, certificate.key_pem);
	(void)platform::restrict_to_owner(folder / kKeyFile);
	platform::write_file_durably(folder / kCertificateFile, certificate.certificate_pem);
}

std::optional<LocalCertificate> load_local_certificate(const std::filesystem::path &folder) {
	const auto certificate = read_text(folder / kCertificateFile);
	const auto key = read_text(folder / kKeyFile);
	if (!certificate || !key) return std::nullopt;
	const auto parsed = parse_pem(*certificate);
	if (!parsed) return std::nullopt;
	LocalCertificate loaded;
	loaded.certificate_pem = *certificate;
	loaded.key_pem = *key;
	loaded.certificate_der = parsed->der;
	loaded.not_after = parsed->not_after;
	return loaded;
}

bool runs_for_more_than(const LocalCertificate &certificate, int days) {
	return certificate.not_after > certificate_time_in_days(days);
}

} // namespace singularity::office
