#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace singularity::routing::live_answer {

// The answer a model is writing, read from its output as the output arrives
// (providers::GenerationStream): the words of the answer's "response" field
// when the model writes the professional answer's JSON, or the output itself
// when it writes plain text. Source handles are left out - the finished answer
// shows its sources. Display only: it is replaced by the finished answer, and
// nothing is kept of it.
class Reader {
public:
	// A new generation begins: what the next one writes is read afresh.
	void begin();
	// The next piece of the output, in order.
	void add(std::string_view piece);
	// The answer as far as it is written; empty until it has a word.
	[[nodiscard]] std::string answer() const;

private:
	enum class Mode { Unknown, Json, Plain };
	void read(char character);
	void take(char character);

	Mode mode_ = Mode::Unknown;
	std::string plain_;
	std::string written_;
	// The JSON reading: how deep, inside a string, an escape, the key a string
	// at the top level may be, and whether the value read now is the answer.
	int depth_ = 0;
	bool inString_ = false;
	bool escaped_ = false;
	std::string unicode_;
	std::uint32_t highSurrogate_ = 0;
	bool readingKey_ = false;
	std::string key_;
	std::string lastKey_;
	bool expectingValue_ = false;
	bool inAnswer_ = false;
};

// Restores the symbols protection wrote into a model's output as it arrives,
// for showing it: each "[[Symbol]]" whose original `values` holds is passed on
// as that original, written for a JSON string; a symbol still being written
// waits for its close. Display only - the finished answer goes through the full
// restoration and its checks.
std::function<void(std::string_view)> restoring(std::function<void(std::string_view)> next,
	std::map<std::string, std::string> values);

} // namespace singularity::routing::live_answer
