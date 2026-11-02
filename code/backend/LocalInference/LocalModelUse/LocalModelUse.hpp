#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace singularity::local_inference {

// Which local models have a call running right now. Stopping a model's
// runtime while one of its calls is in flight would end that call (or reload
// a model the user just unloaded), so every teardown asks here first:
// unloading, reinstalling, removing or releasing memory never interrupts a
// generation a lawyer is waiting for - it refuses, truthfully, until that
// call finishes.
//
// Counted rather than flagged: one model can legitimately serve more than one
// call, and each call releases only its own hold (RAII), so an exception or an
// early return can never leave a model permanently locked.
class LocalModelUseRegistry final {
public:
	class Hold final {
	public:
		Hold() = default;
		Hold(LocalModelUseRegistry *registry, std::string model_id)
			: registry_(registry), model_id_(std::move(model_id)) {}
		Hold(const Hold &) = delete;
		Hold &operator=(const Hold &) = delete;
		Hold(Hold &&other) noexcept { *this = std::move(other); }
		Hold &operator=(Hold &&other) noexcept {
			if (this != &other) {
				release();
				registry_ = other.registry_;
				model_id_ = std::move(other.model_id_);
				other.registry_ = nullptr;
			}
			return *this;
		}
		~Hold() { release(); }

	private:
		void release() noexcept {
			if (registry_ == nullptr) return;
			registry_->end_use(model_id_);
			registry_ = nullptr;
		}

		LocalModelUseRegistry *registry_ = nullptr;
		std::string model_id_;
	};

	[[nodiscard]] Hold begin_use(const std::string &model_id) {
		const std::lock_guard<std::mutex> lock(mutex_);
		++counts_[model_id];
		return Hold(this, model_id);
	}

	[[nodiscard]] bool in_use(const std::string &model_id) const {
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto found = counts_.find(model_id);
		return found != counts_.end() && found->second > 0;
	}

	[[nodiscard]] bool any_in_use() const {
		const std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &entry : counts_)
			if (entry.second > 0) return true;
		return false;
	}

	[[nodiscard]] std::vector<std::string> models_in_use() const {
		const std::lock_guard<std::mutex> lock(mutex_);
		std::vector<std::string> result;
		for (const auto &entry : counts_)
			if (entry.second > 0) result.push_back(entry.first);
		return result;
	}

private:
	void end_use(const std::string &model_id) noexcept {
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto found = counts_.find(model_id);
		if (found == counts_.end()) return;
		if (--found->second <= 0) counts_.erase(found);
	}

	mutable std::mutex mutex_;
	std::map<std::string, int> counts_;
};

} // namespace singularity::local_inference
