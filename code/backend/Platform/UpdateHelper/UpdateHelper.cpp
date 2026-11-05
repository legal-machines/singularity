#include "Platform/UpdateHelper/UpdateHelper.hpp"

#include <algorithm>
#include <cstddef>
#include <cwctype>

namespace singularity::platform::update_helper {
namespace {

bool is_hex64(const std::string &text) {
	return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	});
}

// A version as the release list names one: digits, letters, dots and dashes.
bool is_version(const std::string &text) {
	return !text.empty() && text.size() <= 32 && std::all_of(text.begin(), text.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '+';
	});
}

char lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string narrow_ascii(const std::wstring &text) {
	std::string out;
	out.reserve(text.size());
	for (const wchar_t c : text) {
		if (c > 0x7f) return {};
		out.push_back(static_cast<char>(c));
	}
	return out;
}

// A full Windows path: a drive and a backslash, or a share; never one
// relative to whatever folder the updater runs in.
bool is_full_path(const std::wstring &path) {
	if (path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) return true;
	return path.size() >= 3 && path[0] == L'\\' && path[1] == L'\\';
}

// A path the updater takes: no quote and no control character, so it can be
// quoted on a command line whole.
bool fits(const std::wstring &value) {
	return !value.empty() && value.size() < 32768 && value.find(L'"') == std::wstring::npos
		&& std::none_of(value.begin(), value.end(), [](wchar_t c) { return c < 0x20; });
}

} // namespace

std::optional<Request> request_of(const std::vector<std::wstring> &arguments) {
	static const wchar_t *const names[] = {L"--pid", L"--installer", L"--sha256", L"--version", L"--app", L"--relaunch", L"--folder"};
	constexpr std::size_t kParts = sizeof names / sizeof names[0];
	Request request;
	bool seen[kParts] = {};
	if (arguments.size() != kParts * 2) return std::nullopt;
	for (std::size_t at = 0; at < arguments.size(); at += 2) {
		const std::wstring &name = arguments[at];
		const std::wstring &value = arguments[at + 1];
		std::size_t which = kParts;
		for (std::size_t each = 0; each < kParts; ++each)
			if (name == names[each]) which = each;
		if (which == kParts || seen[which]) return std::nullopt;
		seen[which] = true;
		switch (which) {
		case 0: {
			if (value.empty() || value.size() > 10
				|| !std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
				return std::nullopt;
			const unsigned long long pid = std::stoull(value);
			if (pid == 0 || pid > 0xffffffffULL) return std::nullopt;
			request.process_id = static_cast<unsigned long>(pid);
			break;
		}
		case 1:
			if (!fits(value) || !is_full_path(value)) return std::nullopt;
			request.installer = value;
			break;
		case 2: {
			const std::string hex = narrow_ascii(value);
			if (!is_hex64(hex)) return std::nullopt;
			request.sha256.resize(hex.size());
			std::transform(hex.begin(), hex.end(), request.sha256.begin(), lower);
			break;
		}
		case 3: {
			const std::string version = narrow_ascii(value);
			if (!is_version(version)) return std::nullopt;
			request.version = version;
			break;
		}
		case 4:
			if (!fits(value) || !is_full_path(value)) return std::nullopt;
			request.application = value;
			break;
		case 5:
			if (value != L"yes" && value != L"no") return std::nullopt;
			request.relaunch = value == L"yes";
			break;
		case 6:
			if (!fits(value) || !is_full_path(value)) return std::nullopt;
			request.folder = value;
			break;
		}
	}
	return request;
}

std::vector<std::wstring> arguments_of(const Request &request) {
	return {L"--pid", std::to_wstring(request.process_id),
		L"--installer", request.installer,
		L"--sha256", std::wstring(request.sha256.begin(), request.sha256.end()),
		L"--version", std::wstring(request.version.begin(), request.version.end()),
		L"--app", request.application,
		L"--relaunch", request.relaunch ? L"yes" : L"no",
		L"--folder", request.folder};
}

std::wstring command_line_of(const std::wstring &program, const std::vector<std::wstring> &arguments) {
	// No value holds a quote (request_of refuses one), so a quoted argument
	// needs only the backslashes before its closing quote doubled.
	const auto quoted = [](const std::wstring &value) {
		std::wstring out = L"\"";
		std::size_t backslashes = 0;
		for (const wchar_t c : value) {
			backslashes = c == L'\\' ? backslashes + 1 : 0;
			out.push_back(c);
		}
		out.append(backslashes, L'\\');
		out.push_back(L'"');
		return out;
	};
	std::wstring line = quoted(program);
	for (const auto &argument : arguments) {
		line.push_back(L' ');
		line += quoted(argument);
	}
	return line;
}

std::wstring installer_parameters_of(const Request &request) {
	// request_of took the application as a full path, so it has a folder.
	const std::size_t slash = request.application.find_last_of(L"\\/");
	return L"/S /D=" + request.application.substr(0, slash);
}

bool same_sha256(const std::string &expected, const std::string &actual) {
	if (!is_hex64(expected) || !is_hex64(actual)) return false;
	for (std::size_t at = 0; at < expected.size(); ++at)
		if (lower(expected[at]) != lower(actual[at])) return false;
	return true;
}

std::string refusal_sentence(const std::string &version) {
	return "Singularity " + version + " was not installed: Windows was not given permission. It asks again when Singularity next closes.";
}

std::string failure_sentence(const std::string &version, const std::string &why) {
	return "Singularity " + version + " could not install itself: " + why + ".";
}

std::string window_heading(const Request &request) {
	return "Installing Singularity " + request.version;
}

std::string window_words(const Request &request, bool permitted) {
	// One line each, so that neither wraps and the window keeps its size.
	const std::string permission = permitted ? "Windows was given permission." : "Windows asks you for permission.";
	const std::string then = request.relaunch ? "Singularity opens again by itself when it is installed."
	                                          : "This window closes when it is installed.";
	return permission + "\n" + then;
}

std::string by_hand_words(const std::string &failure) {
	return failure + " Run its installer, in the folder that opens now, to install it.";
}

} // namespace singularity::platform::update_helper
