#include "Routing/LiveAnswer/LiveAnswer.hpp"

#include <utility>

namespace singularity::routing::live_answer {
namespace {

void append_utf8(std::string &target, std::uint32_t code) {
	if (code <= 0x7F) {
		target.push_back(static_cast<char>(code));
	} else if (code <= 0x7FF) {
		target.push_back(static_cast<char>(0xC0 | (code >> 6)));
		target.push_back(static_cast<char>(0x80 | (code & 0x3F)));
	} else if (code <= 0xFFFF) {
		target.push_back(static_cast<char>(0xE0 | (code >> 12)));
		target.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
		target.push_back(static_cast<char>(0x80 | (code & 0x3F)));
	} else {
		target.push_back(static_cast<char>(0xF0 | (code >> 18)));
		target.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
		target.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
		target.push_back(static_cast<char>(0x80 | (code & 0x3F)));
	}
}

// What a JSON string holds `text` as.
std::string json_written(const std::string &text) {
	std::string written;
	written.reserve(text.size());
	for (const char character : text) {
		switch (character) {
			case '"': written += "\\\""; break;
			case '\\': written += "\\\\"; break;
			case '\n': written += "\\n"; break;
			case '\r': written += "\\r"; break;
			case '\t': written += "\\t"; break;
			default:
				if (static_cast<unsigned char>(character) < 0x20) written += ' ';
				else written.push_back(character);
		}
	}
	return written;
}

// The text without its source handles ("[SOURCE:DOC_2:CHUNK_1]", protection's
// "[[SOURCE:...]]"), and without the start of one still being written.
std::string without_handles(const std::string &text) {
	std::string kept;
	kept.reserve(text.size());
	std::size_t at = 0;
	while (at < text.size()) {
		const std::size_t open = text.find('[', at);
		if (open == std::string::npos) {
			kept.append(text, at, std::string::npos);
			break;
		}
		kept.append(text, at, open - at);
		const std::size_t start = text.compare(open, 2, "[[") == 0 ? open + 2 : open + 1;
		static const std::string kHandle = "SOURCE:";
		const std::string_view rest = std::string_view(text).substr(start, kHandle.size());
		const bool handle = rest == kHandle;
		// The start of a handle at the end: it is not shown until it is one, nor
		// the space before it.
		if ((!handle && start + rest.size() == text.size() && kHandle.compare(0, rest.size(), rest) == 0)
				|| (handle && text.find(']', start) == std::string::npos)) {
			while (!kept.empty() && kept.back() == ' ') kept.pop_back();
			break;
		}
		if (!handle) {
			kept.push_back('[');
			at = open + 1;
			continue;
		}
		const std::size_t close = text.find(']', start);
		if (close == std::string::npos) break;
		at = close + 1;
		if (at < text.size() && text[at] == ']') ++at;
		// "said. [SOURCE:..]" leaves no space before what follows.
		while (!kept.empty() && kept.back() == ' ' && (at >= text.size() || text[at] == ' ' || text[at] == '\n'
				|| text[at] == '.' || text[at] == ',' || text[at] == ';'))
			kept.pop_back();
	}
	return kept;
}

} // namespace

void Reader::begin() {
	*this = Reader();
}

void Reader::add(std::string_view piece) {
	for (const char character : piece) {
		if (mode_ == Mode::Unknown) {
			if (character == ' ' || character == '\n' || character == '\r' || character == '\t') continue;
			mode_ = character == '{' ? Mode::Json : Mode::Plain;
		}
		if (mode_ == Mode::Plain) plain_.push_back(character);
		else read(character);
	}
}

std::string Reader::answer() const {
	return without_handles(mode_ == Mode::Plain ? plain_ : written_);
}

void Reader::take(char character) {
	if (readingKey_) key_.push_back(character);
	else if (inAnswer_) written_.push_back(character);
}

void Reader::read(char character) {
	if (inString_) {
		if (!unicode_.empty() || (escaped_ && character == 'u')) {
			if (escaped_) {
				escaped_ = false;
				unicode_ = "u";
				return;
			}
			unicode_.push_back(character);
			if (unicode_.size() < 5) return;
			// Four hex digits; anything else is shown as a question mark, never
			// thrown at the request that is being read.
			std::uint32_t code = 0;
			for (std::size_t index = 1; index < unicode_.size(); ++index) {
				const char digit = unicode_[index];
				const int value = digit >= '0' && digit <= '9' ? digit - '0'
					: digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
					: digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
				if (value < 0) {
					code = '?';
					break;
				}
				code = code * 16 + static_cast<std::uint32_t>(value);
			}
			unicode_.clear();
			if (code >= 0xD800 && code <= 0xDBFF) {
				highSurrogate_ = code;
				return;
			}
			std::string decoded;
			if (code >= 0xDC00 && code <= 0xDFFF && highSurrogate_ != 0)
				append_utf8(decoded, 0x10000 + ((highSurrogate_ - 0xD800) << 10) + (code - 0xDC00));
			else
				append_utf8(decoded, code);
			highSurrogate_ = 0;
			for (const char byte : decoded) take(byte);
			return;
		}
		if (escaped_) {
			escaped_ = false;
			switch (character) {
				case 'n': take('\n'); break;
				case 't': take('\t'); break;
				case 'r': break;
				case 'b': case 'f': break;
				default: take(character);
			}
			return;
		}
		if (character == '\\') {
			escaped_ = true;
			return;
		}
		if (character == '"') {
			inString_ = false;
			if (readingKey_) {
				readingKey_ = false;
				lastKey_ = key_;
			}
			inAnswer_ = false;
			return;
		}
		take(character);
		return;
	}
	switch (character) {
		case '"':
			inString_ = true;
			if (depth_ == 1 && !expectingValue_) {
				readingKey_ = true;
				key_.clear();
			} else if (depth_ == 1 && expectingValue_) {
				inAnswer_ = lastKey_ == "response";
				expectingValue_ = false;
			}
			return;
		case ':':
			if (depth_ == 1) expectingValue_ = true;
			return;
		case ',':
			if (depth_ == 1) expectingValue_ = false;
			return;
		case '{':
		case '[':
			++depth_;
			if (depth_ == 2) expectingValue_ = false;
			return;
		case '}':
		case ']':
			if (depth_ > 0) --depth_;
			return;
		default:
			return;
	}
}

std::function<void(std::string_view)> restoring(std::function<void(std::string_view)> next,
		std::map<std::string, std::string> values) {
	for (auto &[symbol, original] : values) original = json_written(original);
	return [next = std::move(next), values = std::move(values), held = std::string()](std::string_view piece) mutable {
		held.append(piece.data(), piece.size());
		std::string shown;
		std::size_t at = 0;
		while (at < held.size()) {
			const std::size_t open = held.find("[[", at);
			if (open == std::string::npos) {
				// A last "[" may open a symbol the next piece finishes.
				const std::size_t end = held.back() == '[' ? held.size() - 1 : held.size();
				shown.append(held, at, end - at);
				at = end;
				break;
			}
			shown.append(held, at, open - at);
			const std::size_t close = held.find("]]", open + 2);
			if (close == std::string::npos) {
				// No symbol is this long: what opened is the text's own.
				if (held.size() - open > 96) {
					shown.append(held, open, 2);
					at = open + 2;
					continue;
				}
				at = open;
				break;
			}
			const std::string symbol = held.substr(open, close + 2 - open);
			const auto found = values.find(symbol);
			shown += found != values.end() ? found->second : symbol;
			at = close + 2;
		}
		held.erase(0, at);
		if (!shown.empty() && next) next(shown);
	};
}

} // namespace singularity::routing::live_answer
