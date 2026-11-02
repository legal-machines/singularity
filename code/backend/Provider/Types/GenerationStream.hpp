#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace singularity::providers {

// Shows a generation while the provider writes it. Installed for one piece of
// work (GenerationStreamScope) on the thread that does it; every generation
// that work asks of a provider able to stream is streamed to it: `begin` as
// each request is sent (a retried request begins again), then `text` with
// each piece of the model's own output, in order. What `text` is told is the
// provider's output before restoration - the installer's alone to read, never
// a credential, a header or anything else of the exchange.
struct GenerationStream {
	std::function<void()> begin;
	std::function<void(std::string_view piece)> text;
};

// The stream the generations made on this thread are shown to, or null.
inline std::shared_ptr<GenerationStream> &current_generation_stream() {
	thread_local std::shared_ptr<GenerationStream> stream;
	return stream;
}

// Shows every generation this thread asks for while the scope lives.
class GenerationStreamScope {
public:
	explicit GenerationStreamScope(std::shared_ptr<GenerationStream> stream)
			: previous_(current_generation_stream()) {
		current_generation_stream() = std::move(stream);
	}
	~GenerationStreamScope() { current_generation_stream() = previous_; }

	GenerationStreamScope(const GenerationStreamScope &) = delete;
	GenerationStreamScope &operator=(const GenerationStreamScope &) = delete;

private:
	std::shared_ptr<GenerationStream> previous_;
};

} // namespace singularity::providers
