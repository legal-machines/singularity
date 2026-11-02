#pragma once

// UTF-8 <-> UTF-16 at the Win32 boundary. Singularity keeps every string
// UTF-8; only the Windows implementation files of the platform seams convert,
// and only through these functions. Windows-only header, inline so that every
// target compiling one of those files has it without a source of its own.

#if !defined(_WIN32)
#error "Platform/WindowsText is Windows-only"
#endif

#include <windows.h>

#include <climits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace singularity::platform::windows {

// Throws std::invalid_argument for input that is not valid UTF-8, so a
// malformed name can never be silently re-interpreted in a code page.
inline std::wstring utf8_to_wide(std::string_view utf8) {
	if (utf8.empty()) return {};
	if (utf8.size() > static_cast<std::size_t>(INT_MAX)) throw std::invalid_argument("Text is too long.");
	const int length = static_cast<int>(utf8.size());
	const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
	if (characters <= 0) throw std::invalid_argument("Text is not valid UTF-8.");
	std::wstring wide(static_cast<std::size_t>(characters), L'\0');
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, wide.data(), characters) <= 0)
		throw std::invalid_argument("Text is not valid UTF-8.");
	return wide;
}

// Unpaired surrogates (possible in Win32 names) become U+FFFD.
inline std::string wide_to_utf8(std::wstring_view wide) {
	if (wide.empty()) return {};
	if (wide.size() > static_cast<std::size_t>(INT_MAX)) throw std::invalid_argument("Text is too long.");
	const int length = static_cast<int>(wide.size());
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return {};
	std::string utf8(static_cast<std::size_t>(bytes), '\0');
	if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, utf8.data(), bytes, nullptr, nullptr) <= 0) return {};
	return utf8;
}

// The system's message for a Win32 error code, as UTF-8 without the trailing
// line break; "Windows error <code>" when the system has none.
inline std::string error_message(unsigned long code) {
	wchar_t *buffer = nullptr;
	const DWORD length = FormatMessageW(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
	std::string message;
	if (length > 0 && buffer != nullptr) message = wide_to_utf8(std::wstring_view(buffer, length));
	if (buffer != nullptr) LocalFree(buffer);
	while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' ||
		message.back() == '.'))
		message.pop_back();
	return message.empty() ? "Windows error " + std::to_string(code) : message;
}

} // namespace singularity::platform::windows
