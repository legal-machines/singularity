#include "DocumentIntelligence/EmailDraft/EmailDraft.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace singularity::document_intelligence {
namespace {

constexpr std::uintmax_t kMostAttachedBytes = 40ULL * 1024ULL * 1024ULL;
constexpr const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::string& bytes) {
	std::string out;
	out.reserve((bytes.size() + 2) / 3 * 4);
	std::size_t index = 0;
	while (index + 2 < bytes.size()) {
		const std::uint32_t value = (static_cast<unsigned char>(bytes[index]) << 16)
			| (static_cast<unsigned char>(bytes[index + 1]) << 8) | static_cast<unsigned char>(bytes[index + 2]);
		out += kAlphabet[(value >> 18) & 63];
		out += kAlphabet[(value >> 12) & 63];
		out += kAlphabet[(value >> 6) & 63];
		out += kAlphabet[value & 63];
		index += 3;
	}
	if (index < bytes.size()) {
		std::uint32_t value = static_cast<unsigned char>(bytes[index]) << 16;
		if (index + 1 < bytes.size()) value |= static_cast<unsigned char>(bytes[index + 1]) << 8;
		out += kAlphabet[(value >> 18) & 63];
		out += kAlphabet[(value >> 12) & 63];
		out += index + 1 < bytes.size() ? kAlphabet[(value >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

std::string unbase64(const std::string& text) {
	std::string out;
	std::uint32_t buffer = 0;
	int bits = 0;
	for (const char character : text) {
		int value = -1;
		if (character >= 'A' && character <= 'Z') value = character - 'A';
		else if (character >= 'a' && character <= 'z') value = character - 'a' + 26;
		else if (character >= '0' && character <= '9') value = character - '0' + 52;
		else if (character == '+') value = 62;
		else if (character == '/') value = 63;
		else if (character == '=') break;
		if (value < 0) continue;
		buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += static_cast<char>((buffer >> bits) & 0xFF);
		}
	}
	return out;
}

// Base64 in lines of 76 characters, as MIME writes it.
std::string base64Lines(const std::string& bytes) {
	const std::string encoded = base64(bytes);
	std::string lines;
	for (std::size_t at = 0; at < encoded.size(); at += 76) lines += encoded.substr(at, 76) + "\r\n";
	return lines;
}

bool isAscii(const std::string& text) {
	return std::all_of(text.begin(), text.end(), [](char character) {
		const unsigned char byte = static_cast<unsigned char>(character);
		return byte >= 0x20 && byte < 0x7F;
	});
}

// A header's words, as an encoded word where they are not plain ASCII
// (RFC 2047), each piece a whole number of characters.
std::string headerWords(const std::string& text) {
	if (isAscii(text) == true) return text;
	std::string words;
	std::size_t at = 0;
	while (at < text.size()) {
		std::size_t end = std::min(text.size(), at + 45);
		while (end < text.size() && end > at && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
		if (words.empty() == false) words += "\r\n ";
		words += "=?UTF-8?B?" + base64(text.substr(at, end - at)) + "?=";
		at = end;
	}
	return words;
}

// A file name as a MIME parameter: quoted ASCII, or RFC 2231's UTF-8 form.
std::string nameParameter(const std::string& key, const std::string& name) {
	if (isAscii(name) == true && name.find('"') == std::string::npos && name.find('\\') == std::string::npos)
		return key + "=\"" + name + "\"";
	std::string encoded;
	static const char* hex = "0123456789ABCDEF";
	for (const char character : name) {
		const unsigned char byte = static_cast<unsigned char>(character);
		if (std::isalnum(byte) != 0 || byte == '.' || byte == '-' || byte == '_') {
			encoded += static_cast<char>(byte);
		} else {
			encoded += '%';
			encoded += hex[byte >> 4];
			encoded += hex[byte & 15];
		}
	}
	return key + "*=UTF-8''" + encoded;
}

std::string contentTypeOf(const std::string& name) {
	std::string extension = std::filesystem::u8path(name).extension().u8string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	if (extension == ".docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
	if (extension == ".xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
	if (extension == ".pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
	if (extension == ".pdf") return "application/pdf";
	if (extension == ".txt" || extension == ".md") return "text/plain";
	return "application/octet-stream";
}

std::string crlfText(const std::string& text) {
	std::string out;
	out.reserve(text.size() + text.size() / 20);
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (text[index] == '\r') continue;
		if (text[index] == '\n') {
			out += "\r\n";
			continue;
		}
		out += text[index];
	}
	return out;
}

std::string withoutCarriageReturns(const std::string& text) {
	std::string out;
	for (const char character : text) {
		if (character != '\r') out += character;
	}
	return out;
}

std::string trimmed(const std::string& text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string lowered(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return text;
}

// ---- Reading -------------------------------------------------------------------------

struct Part {
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;
};

std::string headerOf(const std::vector<std::pair<std::string, std::string>>& headers, const std::string& name) {
	for (const auto& [key, value] : headers) {
		if (lowered(key) == lowered(name)) return value;
	}
	return std::string();
}

// Headers until the first empty line, each folded line unfolded.
std::pair<std::vector<std::pair<std::string, std::string>>, std::string> splitHeaders(const std::string& text) {
	std::vector<std::pair<std::string, std::string>> headers;
	std::size_t at = 0;
	std::string current;
	while (at < text.size()) {
		std::size_t end = text.find('\n', at);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(at, end - at);
		if (line.empty() == false && line.back() == '\r') line.pop_back();
		at = end + 1;
		if (line.empty() == true) break;
		if ((line[0] == ' ' || line[0] == '\t') && headers.empty() == false) {
			headers.back().second += " " + trimmed(line);
			continue;
		}
		const std::size_t colon = line.find(':');
		if (colon == std::string::npos) continue;
		headers.push_back({trimmed(line.substr(0, colon)), trimmed(line.substr(colon + 1))});
	}
	return {headers, at < text.size() ? text.substr(at) : std::string()};
}

std::string unquotedPrintable(const std::string& text) {
	std::string out;
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (text[index] == '=' && index + 1 < text.size()) {
			if (text[index + 1] == '\r' || text[index + 1] == '\n') {
				index += text[index + 1] == '\r' && index + 2 < text.size() && text[index + 2] == '\n' ? 2 : 1;
				continue;
			}
			if (index + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[index + 1])) != 0
					&& std::isxdigit(static_cast<unsigned char>(text[index + 2])) != 0) {
				out += static_cast<char>(std::stoi(text.substr(index + 1, 2), nullptr, 16));
				index += 2;
				continue;
			}
		}
		out += text[index];
	}
	return out;
}

// Encoded words (RFC 2047) back to their UTF-8: B and Q forms.
std::string decodedWords(const std::string& text) {
	std::string out;
	std::size_t at = 0;
	while (at < text.size()) {
		const std::size_t start = text.find("=?", at);
		if (start == std::string::npos) {
			out += text.substr(at);
			break;
		}
		const std::size_t charsetEnd = text.find('?', start + 2);
		const std::size_t encodingEnd = charsetEnd == std::string::npos ? std::string::npos : text.find('?', charsetEnd + 1);
		const std::size_t end = encodingEnd == std::string::npos ? std::string::npos : text.find("?=", encodingEnd + 1);
		if (end == std::string::npos) {
			out += text.substr(at);
			break;
		}
		std::string between = text.substr(at, start - at);
		// Space between two encoded words is not part of either.
		if (trimmed(between).empty() == false || out.empty() == true) out += between;
		const char encoding = static_cast<char>(std::toupper(static_cast<unsigned char>(text[charsetEnd + 1])));
		const std::string payload = text.substr(encodingEnd + 1, end - encodingEnd - 1);
		if (encoding == 'B') {
			out += unbase64(payload);
		} else {
			std::string spaced = payload;
			std::replace(spaced.begin(), spaced.end(), '_', ' ');
			out += unquotedPrintable(spaced);
		}
		at = end + 2;
	}
	return out;
}

std::string parameterOf(const std::string& header, const std::string& name) {
	const std::string lower = lowered(header);
	// RFC 2231's extended form first.
	const std::size_t extended = lower.find(name + "*=");
	if (extended != std::string::npos) {
		std::size_t start = extended + name.size() + 2;
		std::size_t end = header.find(';', start);
		std::string value = trimmed(header.substr(start, end == std::string::npos ? std::string::npos : end - start));
		const std::size_t quote = value.find("''");
		if (quote != std::string::npos) value = value.substr(quote + 2);
		std::string decoded;
		for (std::size_t index = 0; index < value.size(); ++index) {
			if (value[index] == '%' && index + 2 < value.size() && std::isxdigit(static_cast<unsigned char>(value[index + 1])) != 0
					&& std::isxdigit(static_cast<unsigned char>(value[index + 2])) != 0) {
				decoded += static_cast<char>(std::stoi(value.substr(index + 1, 2), nullptr, 16));
				index += 2;
				continue;
			}
			decoded += value[index];
		}
		return decoded;
	}
	std::size_t at = 0;
	while ((at = lower.find(name + "=", at)) != std::string::npos) {
		if (at > 0 && std::isalnum(static_cast<unsigned char>(lower[at - 1])) != 0) {
			at += name.size();
			continue;
		}
		std::size_t start = at + name.size() + 1;
		if (start < header.size() && header[start] == '"') {
			const std::size_t end = header.find('"', start + 1);
			return decodedWords(header.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1));
		}
		const std::size_t end = header.find(';', start);
		return decodedWords(trimmed(header.substr(start, end == std::string::npos ? std::string::npos : end - start)));
	}
	return std::string();
}

std::string decodedBody(const Part& part) {
	const std::string encoding = lowered(headerOf(part.headers, "Content-Transfer-Encoding"));
	if (encoding == "base64") return unbase64(part.body);
	if (encoding == "quoted-printable") return unquotedPrintable(part.body);
	return part.body;
}

void readParts(const std::string& body, const std::string& boundary, std::vector<Part>& parts, int depth) {
	if (depth > 4) return;
	const std::string marker = "--" + boundary;
	std::size_t at = body.find(marker);
	while (at != std::string::npos) {
		at += marker.size();
		if (body.compare(at, 2, "--") == 0) break;
		const std::size_t lineEnd = body.find('\n', at);
		if (lineEnd == std::string::npos) break;
		const std::size_t next = body.find(marker, lineEnd);
		std::string content = body.substr(lineEnd + 1, next == std::string::npos ? std::string::npos : next - lineEnd - 1);
		if (content.size() >= 2 && content.compare(content.size() - 2, 2, "\r\n") == 0) content.resize(content.size() - 2);
		else if (content.empty() == false && content.back() == '\n') content.pop_back();
		auto [headers, rest] = splitHeaders(content);
		Part part{headers, rest};
		const std::string type = lowered(headerOf(headers, "Content-Type"));
		if (type.rfind("multipart/", 0) == 0) {
			readParts(rest, parameterOf(headerOf(headers, "Content-Type"), "boundary"), parts, depth + 1);
		} else {
			parts.push_back(std::move(part));
		}
		at = next;
	}
}

} // namespace

ReadEmail readEmailText(const std::string& message) {

	ReadEmail read;
	auto [headers, body] = splitHeaders(message);
	if (headers.empty() == true) return read;
	read.isRead = true;
	read.from = decodedWords(headerOf(headers, "From"));
	read.to = decodedWords(headerOf(headers, "To"));
	read.cc = decodedWords(headerOf(headers, "Cc"));
	read.date = headerOf(headers, "Date");
	read.subject = decodedWords(headerOf(headers, "Subject"));
	read.isUnsent = trimmed(headerOf(headers, "X-Unsent")) == "1";
	read.messageId = trimmed(headerOf(headers, "Message-ID"));
	read.inReplyTo = trimmed(headerOf(headers, "In-Reply-To"));
	read.references = trimmed(headerOf(headers, "References"));
	const std::string type = headerOf(headers, "Content-Type");
	if (lowered(type).rfind("multipart/", 0) == 0) {
		std::vector<Part> parts;
		readParts(body, parameterOf(type, "boundary"), parts, 0);
		for (const Part& part : parts) {
			const std::string disposition = headerOf(part.headers, "Content-Disposition");
			std::string name = parameterOf(disposition, "filename");
			if (name.empty() == true) name = parameterOf(headerOf(part.headers, "Content-Type"), "name");
			const std::string partType = lowered(headerOf(part.headers, "Content-Type"));
			if (name.empty() == false || lowered(disposition).rfind("attachment", 0) == 0) {
				read.attachments.push_back({name, decodedBody(part)});
				continue;
			}
			if (read.body.empty() == true && (partType.empty() == true || partType.rfind("text/plain", 0) == 0)) {
				read.body = withoutCarriageReturns(decodedBody(part));
			}
		}
	} else {
		read.body = withoutCarriageReturns(decodedBody(Part{headers, body}));
	}
	return read;

}

ReadEmail readEmailFile(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) return ReadEmail();
	const std::string message((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	return readEmailText(message);
}

EmailDraftOutcome writeEmailDraft(const std::filesystem::path& target, const EmailDraft& draft) {

	EmailDraftOutcome outcome;
	outcome.path = target;
	std::error_code exists;
	if (std::filesystem::exists(target, exists) == true) {
		outcome.problem = "A file of the email draft's name is already there, so nothing was written over it.";
		return outcome;
	}
	// Each attachment, read whole.
	std::vector<std::pair<std::string, std::string>> files;
	for (const EmailAttachment& attachment : draft.attachments) {
		std::error_code sized;
		const std::uintmax_t size = std::filesystem::file_size(attachment.path, sized);
		if (sized || size > kMostAttachedBytes) {
			outcome.problem = "\"" + attachment.name + "\" could not be attached: it is missing or too large for an email.";
			return outcome;
		}
		std::ifstream input(attachment.path, std::ios::binary);
		std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if (!input.eof() && input.fail()) {
			outcome.problem = "\"" + attachment.name + "\" could not be read to attach it.";
			return outcome;
		}
		files.push_back({attachment.name, std::move(bytes)});
	}

	std::string identity = draft.to + "\n" + draft.subject + "\n" + draft.body;
	for (const auto& [name, bytes] : files) identity += "\n" + name + ":" + std::to_string(bytes.size());
	const std::string boundary = "----=_Singularity_" + platform::sha256_hex_digest(identity).substr(0, 24);
	std::string message;
	if (trimmed(draft.to).empty() == false) message += "To: " + headerWords(trimmed(draft.to)) + "\r\n";
	if (trimmed(draft.cc).empty() == false) message += "Cc: " + headerWords(trimmed(draft.cc)) + "\r\n";
	message += "Subject: " + headerWords(trimmed(draft.subject)) + "\r\n";
	// Opened as a new message to edit and send, never as one received.
	message += "X-Unsent: 1\r\n";
	message += "MIME-Version: 1.0\r\n";
	message += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n";
	message += "--" + boundary + "\r\n";
	message += "Content-Type: text/plain; charset=\"utf-8\"\r\nContent-Transfer-Encoding: base64\r\n\r\n";
	message += base64Lines(crlfText(draft.body));
	for (const auto& [name, bytes] : files) {
		message += "--" + boundary + "\r\n";
		message += "Content-Type: " + contentTypeOf(name) + "; " + nameParameter("name", name) + "\r\n";
		message += "Content-Disposition: attachment; " + nameParameter("filename", name) + "\r\n";
		message += "Content-Transfer-Encoding: base64\r\n\r\n";
		message += base64Lines(bytes);
	}
	message += "--" + boundary + "--\r\n";

	// Read back before it is kept: every part as meant.
	const ReadEmail read = readEmailText(message);
	bool isMeant = read.isRead == true && read.isUnsent == true && read.subject == trimmed(draft.subject)
		&& read.to == trimmed(draft.to) && read.cc == trimmed(draft.cc)
		&& trimmed(read.body) == trimmed(withoutCarriageReturns(draft.body)) && read.attachments.size() == files.size();
	for (std::size_t index = 0; isMeant == true && index < files.size(); ++index) {
		isMeant = read.attachments[index].first == files[index].first && read.attachments[index].second == files[index].second;
	}
	if (isMeant == false) {
		outcome.problem = "The email draft did not read back as meant, so it was not kept.";
		return outcome;
	}
	const std::filesystem::path staged = target.parent_path() / std::filesystem::u8path(target.filename().u8string() + ".staging");
	{
		std::ofstream output(staged, std::ios::binary | std::ios::trunc);
		output.write(message.data(), static_cast<std::streamsize>(message.size()));
		output.flush();
		if (!output) {
			std::error_code removed;
			std::filesystem::remove(staged, removed);
			outcome.problem = "The email draft could not be written beside the Matter's documents.";
			return outcome;
		}
	}
	std::error_code renamed;
	if (std::filesystem::exists(target, exists) == true) {
		std::filesystem::remove(staged, renamed);
		outcome.problem = "A file of the email draft's name appeared meanwhile, so nothing was written over it.";
		return outcome;
	}
	std::filesystem::rename(staged, target, renamed);
	if (renamed) {
		std::error_code removed;
		std::filesystem::remove(staged, removed);
		outcome.problem = "The email draft could not be saved.";
		return outcome;
	}
	// And once more from the disk.
	const ReadEmail saved = readEmailFile(target);
	if (saved.isRead == false || saved.attachments.size() != files.size() || saved.subject != read.subject) {
		std::error_code removed;
		std::filesystem::remove(target, removed);
		outcome.problem = "The saved email draft did not read back as meant, so it was not kept.";
		return outcome;
	}
	outcome.isWritten = true;
	outcome.sha256 = platform::sha256_hex_digest_of_file(target);
	for (const auto& [name, bytes] : files) outcome.attached.push_back(name);
	return outcome;

}

} // namespace singularity::document_intelligence
