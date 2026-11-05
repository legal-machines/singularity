#include "Node/AppUpdate/AppUpdate.hpp"

#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace singularity::node::update {

namespace {

using Json = nlohmann::json;

// A version's three numbers: "0.10.0". Nothing else is a version.
struct Parsed {
	std::vector<unsigned long long> numbers;
	bool isValid = false;
};

Parsed parse(const std::string &version) {
	Parsed parsed;
	const std::string &core = version;
	std::size_t start = 0;
	while (start <= core.size()) {
		const std::size_t dot = core.find('.', start);
		const std::string part = core.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
		if (part.empty() || part.size() > 9 || !std::all_of(part.begin(), part.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
			return Parsed{};
		}
		parsed.numbers.push_back(std::stoull(part));
		if (dot == std::string::npos) break;
		start = dot + 1;
	}
	parsed.isValid = parsed.numbers.size() == 3;
	return parsed;
}

bool isHex64(const std::string &value) {
	return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
		return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
	});
}

// The installer each system takes, as the release builds name them.
std::string installerSuffix(const std::string &os) {
	if (os == "windows") return ".exe";
	if (os == "macos") return ".dmg";
	if (os == "linux") return ".deb";
	return "";
}

// A file name the site may give: letters, digits and ". _ - ( ) +" and spaces
// inside, never a path, ending as this system's installers end.
bool isInstallerName(const std::string &name, const std::string &os) {
	if (name.empty() || name.size() > 160 || name.front() == '.' || name.front() == ' ' || name.back() == ' ') return false;
	const bool isPlain = std::all_of(name.begin(), name.end(), [](unsigned char c) {
		return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-' || c == '(' || c == ')' || c == '+' || c == ' ';
	});
	const std::string suffix = installerSuffix(os);
	return isPlain && name.find("..") == std::string::npos && !suffix.empty() && name.size() > suffix.size()
		&& name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string text(const Json &value, const char *key) {
	return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>() : std::string();
}

// What a check the member asked for says when it could not check; a check
// of its own says nothing at all - never an alarm.
constexpr const char *kCouldNotCheck = "Couldn't check for updates. Try again later.";

Check refused() {
	return Check{"refused", std::nullopt, kCouldNotCheck};
}

} // namespace

std::string this_os() {
#if defined(_WIN32)
	return "windows";
#elif defined(__APPLE__)
	return "macos";
#else
	return "linux";
#endif
}

std::string this_arch() {
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
	return "arm64";
#else
	return "x64";
#endif
}

bool is_version(const std::string &version) {
	return parse(version).isValid;
}

int compare_versions(const std::string &first, const std::string &second) {
	const Parsed a = parse(first);
	const Parsed b = parse(second);
	if (!a.isValid || !b.isValid) return a.isValid == b.isValid ? 0 : (a.isValid ? 1 : -1);
	for (std::size_t index = 0; index < 3; ++index) {
		if (a.numbers[index] != b.numbers[index]) return a.numbers[index] < b.numbers[index] ? -1 : 1;
	}
	return 0;
}

std::string signed_text(const Release &release) {
	return "singularity-release/1\n"
		"version=" + release.version + "\n"
		"os=" + release.os + "\n"
		"arch=" + release.arch + "\n"
		"fileName=" + release.file_name + "\n"
		"bytes=" + std::to_string(release.bytes) + "\n"
		"sha256=" + release.sha256 + "\n"
		"publishedAt=" + release.published_at + "\n"
		"notesSha256=" + platform::sha256_hex_digest(release.notes) + "\n"
		"url=" + release.url;
}

Check check(const std::string &current_version, const std::string &os, const std::string &arch,
            const std::vector<std::string> &release_keys, const std::string &latest_url, const Fetch &fetch) {
	platform::HttpResponse response;
	try {
		response = fetch(latest_url + "?os=" + os + "&arch=" + arch + "&version=" + current_version, kCheckTimeoutSeconds);
	} catch (const std::exception &) {
		return Check{"offline", std::nullopt, kCouldNotCheck};
	}
	if (response.status == 204) return Check{"current", std::nullopt, "This is the latest version of Singularity."};
	if (response.status < 200 || response.status >= 300) {
		return Check{"offline", std::nullopt, kCouldNotCheck};
	}
	Json value;
	try {
		value = Json::parse(response.body);
	} catch (const std::exception &) {
		return refused();
	}
	if (!value.is_object()) return refused();
	Release release;
	release.version = text(value, "version");
	release.published_at = text(value, "publishedAt");
	release.notes = text(value, "notes");
	release.os = text(value, "os");
	release.arch = text(value, "arch");
	release.file_name = text(value, "fileName");
	release.sha256 = text(value, "sha256");
	release.url = text(value, "url");
	release.signature = text(value, "signature");
	if (value.contains("bytes") && value.at("bytes").is_number_unsigned()) release.bytes = value.at("bytes").get<std::uint64_t>();
	// Signed by Legal Machines, or nothing is offered: the site alone never
	// decides what a firm installs.
	const std::string words = signed_text(release);
	const bool isSigned = std::any_of(release_keys.begin(), release_keys.end(), [&](const std::string &key) {
		return !key.empty() && platform::verify_device_signature(key, words, release.signature);
	});
	if (!isSigned) return refused();
	if (!parse(release.version).isValid || release.os != os || release.arch != arch || !isInstallerName(release.file_name, os)
		|| release.bytes == 0 || release.bytes > kMostInstallerBytes || !isHex64(release.sha256)
		|| release.url.rfind(kDownloadPrefix, 0) != 0) {
		return refused();
	}
	if (compare_versions(release.version, current_version) <= 0) {
		return Check{"current", std::nullopt, "This is the latest version of Singularity."};
	}
	return Check{"available", release, "Singularity " + release.version + " is available."};
}

std::string ready_words(const std::string &os) {
	if (os == "windows") return "It installs when Singularity restarts, with Windows' permission.";
	if (os == "linux") return "It installs when Singularity restarts, with your password.";
	return "It installs when Singularity restarts.";
}

std::string file_version(const std::string &name) {
	// The first part between dashes, spaces or brackets that is a version.
	std::string part;
	for (std::size_t at = 0; at <= name.size(); ++at) {
		const char c = at < name.size() ? name[at] : '-';
		if (c == '-' || c == ' ' || c == '(' || c == ')' || c == '_') {
			if (parse(part).isValid) return part;
			part.clear();
		} else {
			part += c;
		}
	}
	return "";
}

std::filesystem::path download(const Release &release, const std::filesystem::path &folder, const Download &fetch,
                               const platform::DownloadProgress &progress) {
	if (!isInstallerName(release.file_name, release.os) || !isHex64(release.sha256) || release.bytes == 0
		|| release.url.rfind(kDownloadPrefix, 0) != 0) {
		throw std::runtime_error("This version cannot be downloaded: what the site names is not an installer of Singularity.");
	}
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	const auto isWhole = [&](const std::filesystem::path &path) {
		std::error_code sizeError;
		return std::filesystem::is_regular_file(path, sizeError) && std::filesystem::file_size(path, sizeError) == release.bytes
			&& platform::sha256_hex_digest_of_file(path) == release.sha256;
	};
	// Its own name, or the first free "name (2).exe" beside another file.
	std::filesystem::path target = folder / std::filesystem::u8path(release.file_name);
	if (std::filesystem::exists(target, error)) {
		if (isWhole(target)) return target;
		const std::string stem = std::filesystem::u8path(release.file_name).stem().u8string();
		const std::string extension = std::filesystem::u8path(release.file_name).extension().u8string();
		for (int next = 2; std::filesystem::exists(target, error); ++next) {
			target = folder / std::filesystem::u8path(stem + " (" + std::to_string(next) + ")" + extension);
		}
	}
	const std::filesystem::path partial = std::filesystem::u8path(target.u8string() + ".part");
	std::filesystem::remove(partial, error);
	int status = 0;
	try {
		status = fetch(release.url, partial, progress);
	} catch (...) {
		std::filesystem::remove(partial, error);
		throw;
	}
	if (status < 200 || status >= 300) {
		std::filesystem::remove(partial, error);
		throw std::runtime_error("Legal Machines' site did not hand over the installer. Try again later.");
	}
	if (!isWhole(partial)) {
		std::filesystem::remove(partial, error);
		throw std::runtime_error("What arrived is not the installer Legal Machines signed, so it was removed. Try again later.");
	}
	std::filesystem::rename(partial, target, error);
	if (error) {
		std::filesystem::remove(partial, error);
		throw std::runtime_error("The installer could not be kept in its folder. Make room there, then try again.");
	}
	return target;
}

} // namespace singularity::node::update
