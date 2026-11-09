#pragma once

// A web source's site icon: the small picture a site names itself by, shown
// beside a source a colleague's words cite and in the list of what its work
// searched, as ChatGPT shows its sources. This computer asks each site for
// its own icon, over https only and with nothing of the firm's in the
// request: no cookie, no credential, only the site's own address. No other
// service learns which sites a colleague's work cited.
//
// What a site's home page declares, which of it is best, which bytes are a
// picture, and the asking itself over a Get its caller supplies (the
// bridge's is platform::https_get). Header-only: it holds no state, and the
// window keeps what it is given while it is open.

#include "Platform/HttpClient/HttpsHost.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace singularity::research::site_icon {

struct Icon {
	std::string type; // "image/png", "image/x-icon", "image/svg+xml", ...
	std::string bytes;
};

// A site's icon is a few kilobytes; a larger file is not what stands at 16
// points, and the window would be handed it whole.
inline constexpr std::size_t kMaxIconBytes = 100 * 1024;
// The most sites one ask names: one message's sources.
inline constexpr std::size_t kMaxSites = 24;
// How much of a home page is read for the icons its head declares.
inline constexpr std::size_t kMaxHeadBytes = 512 * 1024;

// The body of a successful https GET of `url`, or nothing.
using Get = std::function<std::optional<std::string>(const std::string &url)>;

namespace detail {

inline std::string lowered(std::string_view text) {
	std::string out(text);
	std::transform(out.begin(), out.end(), out.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return out;
}

inline bool starts_with(std::string_view text, std::string_view head) {
	return text.substr(0, head.size()) == head;
}

inline bool is_space(char character) {
	return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f';
}

inline std::string trimmed(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && is_space(text[begin])) ++begin;
	while (end > begin && is_space(text[end - 1])) --end;
	return std::string(text.substr(begin, end - begin));
}

inline std::string base64(std::string_view bytes) {
	static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((bytes.size() + 2) / 3 * 4);
	std::size_t index = 0;
	for (; index + 2 < bytes.size(); index += 3) {
		const unsigned value = (static_cast<unsigned char>(bytes[index]) << 16)
			| (static_cast<unsigned char>(bytes[index + 1]) << 8) | static_cast<unsigned char>(bytes[index + 2]);
		out += kAlphabet[(value >> 18) & 63];
		out += kAlphabet[(value >> 12) & 63];
		out += kAlphabet[(value >> 6) & 63];
		out += kAlphabet[value & 63];
	}
	if (index < bytes.size()) {
		unsigned value = static_cast<unsigned char>(bytes[index]) << 16;
		if (index + 1 < bytes.size()) value |= static_cast<unsigned char>(bytes[index + 1]) << 8;
		out += kAlphabet[(value >> 18) & 63];
		out += kAlphabet[(value >> 12) & 63];
		out += index + 1 < bytes.size() ? kAlphabet[(value >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

inline std::optional<std::string> unbase64(std::string_view text) {
	std::string out;
	unsigned value = 0;
	int bits = 0;
	for (const char character : text) {
		int digit = -1;
		if (character >= 'A' && character <= 'Z') digit = character - 'A';
		else if (character >= 'a' && character <= 'z') digit = character - 'a' + 26;
		else if (character >= '0' && character <= '9') digit = character - '0' + 52;
		else if (character == '+' || character == '-') digit = 62;
		else if (character == '/' || character == '_') digit = 63;
		else if (character == '=' || is_space(character)) continue;
		else return std::nullopt;
		value = (value << 6) | static_cast<unsigned>(digit);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += static_cast<char>((value >> bits) & 0xFF);
		}
	}
	return out;
}

inline std::string unpercent(std::string_view text) {
	std::string out;
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (text[index] == '%' && index + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[index + 1]))
			&& std::isxdigit(static_cast<unsigned char>(text[index + 2]))) {
			out += static_cast<char>(std::stoi(std::string(text.substr(index + 1, 2)), nullptr, 16));
			index += 2;
		} else {
			out += text[index];
		}
	}
	return out;
}

// The few entities an address in an attribute carries.
inline std::string unescaped(std::string text) {
	for (const auto &[entity, character] : std::vector<std::pair<std::string, std::string>>{
		     {"&amp;", "&"}, {"&#38;", "&"}, {"&#x26;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}}) {
		for (std::size_t at = text.find(entity); at != std::string::npos; at = text.find(entity, at + 1)) {
			text.replace(at, entity.size(), character);
		}
	}
	return text;
}

} // namespace detail

// A site's host as a source's address names it, kept only when it is a
// public name a site is reached by: letters, digits and hyphens in at least
// two labels, a name and never an address, and never a name of this computer
// or of the network it stands on.
inline std::optional<std::string> public_host(std::string_view host) {
	std::string name = detail::lowered(detail::trimmed(host));
	while (!name.empty() && name.back() == '.') name.pop_back();
	if (name.empty() || name.size() > 253) return std::nullopt;
	std::vector<std::string> labels;
	for (std::size_t start = 0;;) {
		const std::size_t end = std::min(name.find('.', start), name.size());
		const std::string label = name.substr(start, end - start);
		if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return std::nullopt;
		for (const char character : label) {
			if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '-')) return std::nullopt;
		}
		labels.push_back(label);
		if (end == name.size()) break;
		start = end + 1;
	}
	if (labels.size() < 2) return std::nullopt;
	// The last label names a top-level domain: letters, or an international
	// name's xn-- form. Never digits, so never an IPv4 address.
	const std::string &top = labels.back();
	const bool isWord = std::all_of(top.begin(), top.end(), [](unsigned char character) { return std::isalpha(character); });
	if (!isWord && !detail::starts_with(top, "xn--")) return std::nullopt;
	static const std::vector<std::string> kLocal = {
		"localhost", "local", "localdomain", "internal", "intranet", "lan", "home", "corp", "private", "arpa", "test",
		"invalid", "example", "onion"};
	if (std::find(kLocal.begin(), kLocal.end(), top) != kLocal.end()) return std::nullopt;
	return name;
}

// What the bytes are, by their own first bytes - never by what a server says
// they are - or empty when they are no picture a page draws.
inline std::string image_type(std::string_view bytes) {
	if (detail::starts_with(bytes, "\x89PNG\r\n\x1a\n")) return "image/png";
	if (detail::starts_with(bytes, std::string_view("\0\0\1\0", 4)) && bytes.size() > 6) return "image/x-icon";
	if (detail::starts_with(bytes, "GIF87a") || detail::starts_with(bytes, "GIF89a")) return "image/gif";
	if (detail::starts_with(bytes, "\xFF\xD8\xFF")) return "image/jpeg";
	if (bytes.size() >= 12 && detail::starts_with(bytes, "RIFF") && bytes.substr(8, 4) == "WEBP") return "image/webp";
	// A drawing: XML whose first element is <svg>.
	std::string_view text = bytes;
	if (detail::starts_with(text, "\xEF\xBB\xBF")) text.remove_prefix(3);
	const std::string head = detail::lowered(text.substr(0, 2048));
	const std::size_t start = head.find_first_not_of(" \t\r\n");
	if (start == std::string::npos || head[start] != '<') return "";
	const std::size_t svg = head.find("<svg");
	if (svg == std::string::npos || head.find("<html") != std::string::npos) return "";
	return "image/svg+xml";
}

// An icon a page's head declares.
struct Declared {
	std::string address; // https://... or data:...
	int size = 0;        // its largest side in pixels, 0 when it does not say
	bool scales = false; // a drawing, sharp at every size
	bool touch = false;  // an apple-touch-icon: square, its own ground, 180 pixels when it does not say
	bool ico = false;
};

// How good an icon is to show at 16 points on a sharp screen: the smallest
// that is at least 32 pixels is best - sharp, and few bytes - then a drawing,
// then the larger of those under 32 pixels.
inline int quality_of(const Declared &icon) {
	if (icon.scales && icon.size == 0) return 2000;
	const int size = icon.size > 0 ? icon.size : icon.touch ? 180 : icon.ico ? 24 : 32;
	if (size >= 32) return 3000 - std::min(size, 2048);
	return size;
}

// The address `href` names on `host`'s home page, asked over https only, or
// nothing when it names no picture there.
inline std::optional<std::string> resolved(const std::string &href, const std::string &host) {
	const std::string address = detail::unescaped(detail::trimmed(href));
	const std::string lower = detail::lowered(address);
	if (address.empty()) return std::nullopt;
	if (detail::starts_with(lower, "data:image/")) return address;
	std::string url;
	if (detail::starts_with(lower, "https://")) url = address;
	else if (detail::starts_with(lower, "http://")) url = "https://" + address.substr(7);
	else if (detail::starts_with(lower, "//")) url = "https:" + address;
	else if (lower.find(':') != std::string::npos && lower.find(':') < lower.find_first_of("/?#")) return std::nullopt;
	else if (address.front() == '/') url = "https://" + host + address;
	else url = "https://" + host + "/" + address;
	// An icon kept on another site is asked of that site only when it too is a
	// public name.
	if (!public_host(platform::https_host(url))) return std::nullopt;
	return url;
}

// The icons the head of a home page declares, best first.
inline std::vector<Declared> declared_icons(std::string_view page, const std::string &host) {
	const std::string html = detail::lowered(page.substr(0, kMaxHeadBytes));
	const std::size_t headEnd = std::min(html.find("</head"), html.size());
	std::vector<Declared> icons;
	for (std::size_t at = html.find("<link"); at != std::string::npos && at < headEnd; at = html.find("<link", at + 5)) {
		// Its attributes, a quoted value whole, up to the tag's own end.
		std::map<std::string, std::string> attributes;
		std::size_t index = at + 5;
		while (index < html.size() && html[index] != '>') {
			while (index < html.size() && (detail::is_space(html[index]) || html[index] == '/')) ++index;
			const std::size_t nameStart = index;
			while (index < html.size() && !detail::is_space(html[index]) && html[index] != '=' && html[index] != '>'
			       && html[index] != '/') {
				++index;
			}
			const std::string name = html.substr(nameStart, index - nameStart);
			while (index < html.size() && detail::is_space(html[index])) ++index;
			std::string value;
			if (index < html.size() && html[index] == '=') {
				++index;
				while (index < html.size() && detail::is_space(html[index])) ++index;
				if (index < html.size() && (html[index] == '"' || html[index] == '\'')) {
					const char quote = html[index++];
					const std::size_t end = std::min(html.find(quote, index), html.size());
					// The page's own letters: an address keeps its case.
					value = std::string(page.substr(index, end - index));
					index = end + 1;
				} else {
					const std::size_t valueStart = index;
					while (index < html.size() && !detail::is_space(html[index]) && html[index] != '>') ++index;
					value = std::string(page.substr(valueStart, index - valueStart));
				}
			}
			if (!name.empty() && attributes.count(name) == 0) attributes.emplace(name, value);
			if (name.empty() && index < html.size() && html[index] != '>') ++index;
		}
		std::vector<std::string> rel;
		{
			const std::string words = detail::lowered(attributes["rel"]);
			for (std::size_t start = 0; start < words.size();) {
				const std::size_t end = std::min(words.find_first_of(" \t\r\n", start), words.size());
				if (end > start) rel.push_back(words.substr(start, end - start));
				start = end + 1;
			}
		}
		const auto has = [&](const std::string &word) { return std::find(rel.begin(), rel.end(), word) != rel.end(); };
		const bool touch = has("apple-touch-icon") || has("apple-touch-icon-precomposed");
		if (!has("icon") && !touch) continue;
		const auto address = resolved(attributes["href"], host);
		if (!address) continue;
		Declared icon;
		icon.address = *address;
		icon.touch = touch;
		const std::string type = detail::lowered(attributes["type"]);
		const std::string path = detail::lowered(address->substr(0, address->find_first_of("?#")));
		icon.scales = type == "image/svg+xml" || (path.size() > 4 && path.compare(path.size() - 4, 4, ".svg") == 0)
			|| detail::starts_with(path, "data:image/svg");
		icon.ico = path.size() > 4 && path.compare(path.size() - 4, 4, ".ico") == 0;
		const std::string sizes = detail::lowered(attributes["sizes"]);
		for (std::size_t start = 0; start < sizes.size();) {
			const std::size_t end = std::min(sizes.find_first_of(" \t\r\n", start), sizes.size());
			const std::string word = sizes.substr(start, end - start);
			if (word == "any") icon.scales = true;
			if (const std::size_t by = word.find('x'); by != std::string::npos && by > 0) {
				try {
					icon.size = std::max({icon.size, std::stoi(word.substr(0, by)), std::stoi(word.substr(by + 1))});
				} catch (...) {
				}
			}
			start = end + 1;
		}
		icons.push_back(std::move(icon));
	}
	std::stable_sort(icons.begin(), icons.end(),
		[](const Declared &left, const Declared &right) { return quality_of(left) > quality_of(right); });
	return icons;
}

// The icon these bytes are, or nothing when they are no picture or too large.
inline std::optional<Icon> icon_of(std::string bytes) {
	if (bytes.empty() || bytes.size() > kMaxIconBytes) return std::nullopt;
	std::string type = image_type(bytes);
	if (type.empty()) return std::nullopt;
	return Icon{std::move(type), std::move(bytes)};
}

// The picture a data: address holds, base64 or percent-encoded.
inline std::optional<Icon> inline_icon(const std::string &address) {
	const std::size_t comma = address.find(',');
	if (comma == std::string::npos) return std::nullopt;
	const std::string meta = detail::lowered(address.substr(0, comma));
	const std::string data = address.substr(comma + 1);
	if (meta.size() >= 7 && meta.compare(meta.size() - 7, 7, ";base64") == 0) {
		const auto bytes = detail::unbase64(data);
		return bytes ? icon_of(*bytes) : std::nullopt;
	}
	return icon_of(detail::unpercent(data));
}

// `site`'s icon: the best its home page declares, else the /favicon.ico every
// browser asks for; then, for a site under another (tsdr.uspto.gov), the
// icon of the site it stands under. Nothing when it has none.
inline std::optional<Icon> fetch(const std::string &site, const Get &get) {
	auto host = public_host(site);
	for (int round = 0; host && round < 2; ++round) {
		std::vector<std::string> asked;
		if (const auto page = get("https://" + *host + "/")) {
			int fetched = 0;
			for (const auto &declared : declared_icons(*page, *host)) {
				if (detail::starts_with(declared.address, "data:")) {
					if (auto icon = inline_icon(declared.address)) return icon;
					continue;
				}
				if (fetched == 2) break;
				++fetched;
				asked.push_back(declared.address);
				if (const auto bytes = get(declared.address)) {
					if (auto icon = icon_of(*bytes)) return icon;
				}
			}
		}
		const std::string favicon = "https://" + *host + "/favicon.ico";
		if (std::find(asked.begin(), asked.end(), favicon) == asked.end()) {
			if (const auto bytes = get(favicon)) {
				if (auto icon = icon_of(*bytes)) return icon;
			}
		}
		// The site it stands under, while that is still a site of its own.
		const std::size_t dot = host->find('.');
		const std::string parent = host->substr(dot + 1);
		if (std::count(parent.begin(), parent.end(), '.') == 0) break;
		host = public_host(parent);
	}
	return std::nullopt;
}

// Every site's icon at once, each site asked on its own thread so the slowest
// bounds the whole: at most kMaxSites distinct sites, each by its own name.
inline std::map<std::string, std::optional<Icon>> fetch_all(const std::vector<std::string> &sites, const Get &get) {
	std::vector<std::string> distinct;
	for (const auto &site : sites) {
		if (distinct.size() == kMaxSites) break;
		if (std::find(distinct.begin(), distinct.end(), site) == distinct.end()) distinct.push_back(site);
	}
	std::vector<std::future<std::optional<Icon>>> asked;
	asked.reserve(distinct.size());
	for (const auto &site : distinct) {
		asked.push_back(std::async(std::launch::async, [site, get] { return fetch(site, get); }));
	}
	std::map<std::string, std::optional<Icon>> icons;
	for (std::size_t index = 0; index < distinct.size(); ++index) {
		try {
			icons[distinct[index]] = asked[index].get();
		} catch (...) {
			icons[distinct[index]] = std::nullopt;
		}
	}
	return icons;
}

// The icon as the window draws it: a data: address.
inline std::string data_url(const Icon &icon) {
	return "data:" + icon.type + ";base64," + detail::base64(icon.bytes);
}

} // namespace singularity::research::site_icon
