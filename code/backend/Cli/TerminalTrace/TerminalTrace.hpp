#pragma once

#include "Routing/Observability/OperationEvent.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <ostream>
#include <thread>

namespace singularity::cli {

enum class TraceLevel { Off, Safe, Forensic };

// Terminal-only observer shared by the CLI and acceptance runner. The timer
// reports elapsed time in the last observed stage; it never invents progress
// or subscribes another operation's worker thread to this sink.
class TerminalTrace {
public:
	explicit TerminalTrace(TraceLevel level, std::ostream &output,
			std::chrono::milliseconds interval = std::chrono::seconds(5),
			bool forward_to_previous = false)
		: level_(level), output_(output), interval_(interval) {
		if (level_ == TraceLevel::Off) return;
		sink_ = std::make_unique<routing::observability::ScopedEventSink>(
			[this](const routing::observability::OperationEvent &event) { render(event); },
			forward_to_previous);
		worker_ = std::thread([this] { heartbeat(); });
	}

	~TerminalTrace() {
		sink_.reset();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			isStopped_ = true;
		}
		condition_.notify_all();
		if (worker_.joinable()) worker_.join();
	}

	TerminalTrace(const TerminalTrace &) = delete;
	TerminalTrace &operator=(const TerminalTrace &) = delete;

private:
	TraceLevel level_;
	std::ostream &output_;
	std::chrono::milliseconds interval_;
	std::mutex mutex_;
	std::condition_variable condition_;
	bool isStopped_ = false;
	bool isStageKnown_ = false;
	routing::observability::OperationEventKind stage_ = routing::observability::OperationEventKind::OperationStart;
	std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point stageStarted_ = started_;
	std::unique_ptr<routing::observability::ScopedEventSink> sink_;
	std::thread worker_;

	void render(const routing::observability::OperationEvent &event) {
		std::lock_guard<std::mutex> lock(mutex_);
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count();
		output_ << "[" << routing::observability::to_string(event.kind) << "] elapsedMs=" << elapsed
			<< " " << event.message;
		for (const auto &detail : event.details) output_ << " " << detail.first << "=" << detail.second;
		output_ << '\n';
		if (level_ == TraceLevel::Forensic && event.forensic_payload)
			output_ << "    forensic: " << *event.forensic_payload << '\n';
		output_.flush();
		if (isStageKnown_ == false || stage_ != event.kind) stageStarted_ = now;
		stage_ = event.kind;
		isStageKnown_ = true;
	}

	void heartbeat() {
		std::unique_lock<std::mutex> lock(mutex_);
		while (condition_.wait_for(lock, interval_, [this] { return isStopped_; }) == false) {
			if (isStageKnown_ == false) continue;
			const auto now = std::chrono::steady_clock::now();
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count();
			const auto stageElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stageStarted_).count();
			const char *label = stage_ == routing::observability::OperationEventKind::ProviderSendStart
				? "PROVIDER_WAIT" : "STAGE_WAIT";
			output_ << "[" << label << "] elapsedMs=" << elapsed << " stage="
				<< routing::observability::to_string(stage_) << " stageElapsedMs=" << stageElapsed << '\n';
			output_.flush();
		}
	}
};

} // namespace singularity::cli
