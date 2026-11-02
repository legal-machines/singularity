#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace singularity::platform {

// The lowercased host an https URL names, with userinfo and port removed
// ("https://openrouter.ai@example.com/v1" names example.com), or empty when the
// URL is not https or names no host. A credential is sent only to the host its
// connection names, so callers compare this, never a URL prefix.
inline std::string https_host(const std::string &url) {
	std::string lowered = url;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	const std::string scheme = "https://";
	if (lowered.rfind(scheme, 0) != 0) return {};
	const std::size_t end = lowered.find_first_of("/?#", scheme.size());
	std::string authority = lowered.substr(scheme.size(), end == std::string::npos ? std::string::npos : end - scheme.size());
	if (const std::size_t at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
	if (authority.empty() || authority.front() == '[') return {};
	return authority.substr(0, authority.find(':'));
}

} // namespace singularity::platform
