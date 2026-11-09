#pragma once

#include "PapinianGuard/ProtectedPreview/ProtectedPreview.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace singularity::protected_api::papinian {

// Everything that must stay identical between a preview and a later reveal
// for the frozen ProtectionResult below to still honestly describe the
// current request/source/provider/model/policy state - never an original
// protected value. Plain fields, not a hash: a reveal call needs the recipe
// below (PreviewSessionStore::recipe_for()) up front to know which documents'
// current versions to compare, and which chat (if any) to resolve AI Setup
// against, before it can even build the rest of this struct to compare
// against the stored one.
struct PreviewSessionFingerprint {
	std::string policy_mode;
	// Sorted, so two callers that agree on the active set but built it in a
	// different order still compare equal.
	std::vector<std::string> policy_categories;
	std::vector<std::string> policy_custom_terms;
	// The effective Reasoning provider/model - never Node-default-only, so
	// a Project/Chat AI Setup override is honestly reflected here instead
	// of only ever comparing against the Node default.
	std::string provider_id;
	std::string model_id;
	// The exact documents this preview's context actually drew from, and the
	// version each had at preview time (Document/Types/MatterDocument.hpp's
	// own change-tracking field) - a later edit bumps a document's version,
	// so a mismatch here means the frozen mapping below may no longer
	// honestly describe that document's current content.
	std::map<std::string, int> document_versions;

	[[nodiscard]] bool matches(const PreviewSessionFingerprint &other) const;
};

// Everything one "What leaves this device?" preview needs to answer later,
// on-demand "Reveal original locally" calls - held only in native process
// memory for PreviewSessionStore's bounded lifetime, never persisted,
// logged, or placed in any Matter/Chat/Usage/Routing Log record, and never
// serialized to React/native JSON as a whole. Only the opaque token
// PreviewSessionStore::create() returns may cross that boundary.
struct PreviewSessionState {
	std::string matter_id;
	// The chat (if any) this preview's effective AI Setup resolution was
	// computed against - not a protected value, just an id, kept so reveal
	// can recompute the identical effective_reasoning_target() call rather
	// than silently falling back to no chat context.
	std::optional<std::string> chat_id;
	PreviewSessionFingerprint fingerprint;
	ProtectionResult protection;
	OriginSources sources;
};

// The non-sensitive inputs a caller needs to rebuild a fresh
// PreviewSessionFingerprint for comparison before calling look_up() - never
// gated behind the fingerprint check itself, since none of it is a
// protected value: document ids and a chat id carry nothing sensitive, only
// whether `token` currently names a real, unexpired session at all.
struct PreviewSessionRecipe {
	std::vector<std::string> document_ids;
	std::optional<std::string> chat_id;
};

// The ephemeral, native-memory-only binding between one opaque preview token
// and the exact state it was minted for (Phase 11 "ephemeral native preview
// session"). This is not a replacement for the Restoration Map's own
// invariants - the ProtectionResult a session carries stays exactly as
// sensitive as it always was; this only decides which one, if any, a reveal
// call may see, and for how long.
//
// A session's normal lifecycle is explicit invalidation, not TTL: the
// caller destroys it the moment the preview it backs stops being "the
// current one" - the preview is closed/cancelled, the request/composer
// state that produced it changes, a real send happens for the same matter,
// or the Project is closed (see invalidate()/invalidate_for_matter() below,
// and their bridge-level callers). The TTL/capacity bounds below exist only
// as a fallback against a caller that never explicitly cleans up (a crashed
// renderer, a dropped close event) - not as the intended everyday path.
class PreviewSessionStore {
public:
	explicit PreviewSessionStore(std::chrono::steady_clock::duration ttl = std::chrono::minutes(10),
			std::size_t max_sessions = 200);

	// Generates a fresh, cryptographically random 256-bit token, stores
	// `state` under it, and returns the token - the only value that may cross
	// into React/native JSON for this preview. Sweeps every expired session
	// in this store first (fallback cleanup, no background thread required),
	// and evicts the oldest session if already at capacity.
	[[nodiscard]] std::string create(PreviewSessionState state);

	// The non-sensitive recipe `token`'s session needs a caller to rebuild a
	// fresh PreviewSessionFingerprint before calling look_up(). Never gated
	// behind the fingerprint check itself: nothing here carries a protected
	// value, only whether `token` currently names a real, unexpired session
	// for `matter_id` at all.
	[[nodiscard]] std::optional<PreviewSessionRecipe> recipe_for(
			const std::string &token, const std::string &matter_id);

	// std::nullopt when `token` is unknown, expired, was minted for a
	// different matter, or `current_fingerprint` no longer matches the one
	// `token` was created with - every reason is indistinguishable to the
	// caller, which must fail closed identically ("no longer available"),
	// never guess or silently resolve against newly recomputed state.
	[[nodiscard]] std::optional<PreviewSessionState> look_up(
			const std::string &token, const std::string &matter_id,
			const PreviewSessionFingerprint &current_fingerprint);

	// Explicit, immediate destruction of one session - the normal way a
	// session's life ends: the preview it backs was closed or cancelled, or
	// the request/composer state that produced it changed. A no-op (not an
	// error) when `token` is unknown/already gone, or was minted for a
	// different `matter_id`.
	void invalidate(const std::string &token, const std::string &matter_id);

	// Explicit, immediate destruction of every session for `matter_id` - a
	// real send happening for that matter (its outstanding previews no
	// longer describe a pending, un-sent request), or the Project closing.
	void invalidate_for_matter(const std::string &matter_id);

private:
	struct Entry {
		PreviewSessionState state;
		std::chrono::steady_clock::time_point expires_at;
		std::chrono::steady_clock::time_point created_at;
	};

	// Caller already holds mutex_.
	void expire_stale_sessions_locked();

	std::mutex mutex_;
	std::unordered_map<std::string, Entry> sessions_;
	std::chrono::steady_clock::duration ttl_;
	std::size_t max_sessions_;
};

// The one process-wide store every preview/reveal bridge call shares.
// Cleared automatically on process exit; never written to disk.
[[nodiscard]] PreviewSessionStore &production_preview_session_store();

} // namespace singularity::protected_api::papinian
