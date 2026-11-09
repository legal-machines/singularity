#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace singularity::providers {

// The pictures a member attached to a request, sent to the model as image
// inputs: a trademark's logo, a photographed page, a diagram. Each is its
// file's bytes, in a form every provider here reads (PNG, JPEG, GIF or WebP),
// with its name for the request's record, its SHA-256, and its bytes as base64,
// which is what the body carries and what the final check holds the body to
// (ProtectedInference): a picture in the body that is none of these is never
// sent. Installed for one request (InputPicturesScope) on the thread that makes
// it, as PromptCache is; a request with none carries text only.
//
// Protection masks words, not pictures: while it is on, a picture leaves as it
// is, and only because the member pressed Send with the attachment saying so.
struct InputPicture {
	std::string name;
	// "image/png", "image/jpeg", "image/gif" or "image/webp".
	std::string media_type;
	std::string sha256;
	std::string base64;
	std::size_t bytes = 0;
};

// Standard base64 (RFC 4648, padded) of some bytes.
inline std::string base64_of(const std::string &bytes) {
	static const char *const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string encoded;
	encoded.reserve(((bytes.size() + 2) / 3) * 4);
	std::size_t index = 0;
	while (index + 2 < bytes.size()) {
		const unsigned int triple = (static_cast<unsigned char>(bytes[index]) << 16)
			| (static_cast<unsigned char>(bytes[index + 1]) << 8) | static_cast<unsigned char>(bytes[index + 2]);
		encoded += alphabet[(triple >> 18) & 0x3F];
		encoded += alphabet[(triple >> 12) & 0x3F];
		encoded += alphabet[(triple >> 6) & 0x3F];
		encoded += alphabet[triple & 0x3F];
		index += 3;
	}
	if (index < bytes.size()) {
		const bool isPair = index + 1 < bytes.size();
		const unsigned int triple = (static_cast<unsigned char>(bytes[index]) << 16)
			| (isPair ? static_cast<unsigned char>(bytes[index + 1]) << 8 : 0u);
		encoded += alphabet[(triple >> 18) & 0x3F];
		encoded += alphabet[(triple >> 12) & 0x3F];
		encoded += isPair ? alphabet[(triple >> 6) & 0x3F] : '=';
		encoded += '=';
	}
	return encoded;
}

// The pictures the generations made on this thread carry, or none.
inline std::shared_ptr<const std::vector<InputPicture>> &current_input_pictures() {
	thread_local std::shared_ptr<const std::vector<InputPicture>> pictures;
	return pictures;
}

// Gives every generation this thread asks for the pictures while the scope
// lives.
class InputPicturesScope {
public:
	explicit InputPicturesScope(std::shared_ptr<const std::vector<InputPicture>> pictures)
		: previous_(current_input_pictures()) {
		current_input_pictures() = std::move(pictures);
	}
	~InputPicturesScope() { current_input_pictures() = previous_; }
	InputPicturesScope(const InputPicturesScope &) = delete;
	InputPicturesScope &operator=(const InputPicturesScope &) = delete;

private:
	std::shared_ptr<const std::vector<InputPicture>> previous_;
};

} // namespace singularity::providers
