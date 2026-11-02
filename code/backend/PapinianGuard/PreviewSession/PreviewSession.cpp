#include "PapinianGuard/PreviewSession/PreviewSession.hpp"

#include <iomanip>
#include <random>
#include <sstream>

namespace singularity::protected_api::papinian {
namespace {

// Drawn directly from std::random_device on every word rather than seeding a
// fast PRNG once - on macOS/Windows/Linux this is backed by a real OS
// entropy source, and this token is the ONLY thing standing between a
// caller and a real protected value, so it gets the strongest portable
// randomness available rather than the lighter generate_id()-style pattern
// used elsewhere in this codebase for ordinary (non-security-boundary) ids.
std::string generate_preview_token() {
	std::random_device source;
	std::ostringstream token;
	token << std::hex << std::setfill('0');
	for (int word = 0; word < 8; ++word) token << std::setw(8) << source();
	return token.str();
}

} // namespace

bool PreviewSessionFingerprint::matches(const PreviewSessionFingerprint &other) const {
	return policy_mode == other.policy_mode && policy_categories == other.policy_categories
			&& policy_custom_terms == other.policy_custom_terms && provider_id == other.provider_id
			&& model_id == other.model_id && document_versions == other.document_versions;
}

PreviewSessionStore::PreviewSessionStore(std::chrono::steady_clock::duration ttl, std::size_t max_sessions)
		: ttl_(ttl), max_sessions_(max_sessions) {
}

void PreviewSessionStore::expire_stale_sessions_locked() {
	const auto now = std::chrono::steady_clock::now();
	for (auto entry = sessions_.begin(); entry != sessions_.end();) {
		if (entry->second.expires_at <= now) entry = sessions_.erase(entry);
		else ++entry;
	}
}

std::string PreviewSessionStore::create(PreviewSessionState state) {
	const std::lock_guard<std::mutex> lock(mutex_);
	expire_stale_sessions_locked();

	if (sessions_.size() >= max_sessions_) {
		// Evict the single oldest session rather than refusing a new preview -
		// bounded memory, never an unbounded queue of abandoned previews.
		auto oldest = sessions_.begin();
		for (auto entry = sessions_.begin(); entry != sessions_.end(); ++entry) {
			if (entry->second.created_at < oldest->second.created_at) oldest = entry;
		}
		sessions_.erase(oldest);
	}

	const auto now = std::chrono::steady_clock::now();
	std::string token;
	do {
		token = generate_preview_token();
	} while (sessions_.count(token) > 0);

	sessions_.emplace(token, Entry{std::move(state), now + ttl_, now});
	return token;
}

std::optional<PreviewSessionRecipe> PreviewSessionStore::recipe_for(
		const std::string &token, const std::string &matter_id) {
	const std::lock_guard<std::mutex> lock(mutex_);
	expire_stale_sessions_locked();

	const auto found = sessions_.find(token);
	if (found == sessions_.end() || found->second.state.matter_id != matter_id) return std::nullopt;

	PreviewSessionRecipe recipe;
	recipe.document_ids.reserve(found->second.state.fingerprint.document_versions.size());
	for (const auto &[document_id, version] : found->second.state.fingerprint.document_versions) {
		static_cast<void>(version);
		recipe.document_ids.push_back(document_id);
	}
	recipe.chat_id = found->second.state.chat_id;
	return recipe;
}

std::optional<PreviewSessionState> PreviewSessionStore::look_up(
		const std::string &token, const std::string &matter_id, const PreviewSessionFingerprint &current_fingerprint) {
	const std::lock_guard<std::mutex> lock(mutex_);
	expire_stale_sessions_locked();

	const auto found = sessions_.find(token);
	if (found == sessions_.end()) return std::nullopt;
	if (found->second.state.matter_id != matter_id) return std::nullopt;
	if (!found->second.state.fingerprint.matches(current_fingerprint)) return std::nullopt;

	return found->second.state;
}

void PreviewSessionStore::invalidate(const std::string &token, const std::string &matter_id) {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto found = sessions_.find(token);
	if (found == sessions_.end() || found->second.state.matter_id != matter_id) return;
	sessions_.erase(found);
}

void PreviewSessionStore::invalidate_for_matter(const std::string &matter_id) {
	const std::lock_guard<std::mutex> lock(mutex_);
	for (auto entry = sessions_.begin(); entry != sessions_.end();) {
		if (entry->second.state.matter_id == matter_id) entry = sessions_.erase(entry);
		else ++entry;
	}
}

PreviewSessionStore &production_preview_session_store() {
	static PreviewSessionStore store;
	return store;
}

} // namespace singularity::protected_api::papinian
