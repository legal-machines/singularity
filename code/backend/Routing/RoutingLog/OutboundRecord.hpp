#pragma once

// The outbound record: what really left this device, one entry per external
// call. The calls are noted where they are made - the provider boundary
// (ProtectedInference.cpp) and the research tools (ResearchTools.cpp) - under
// the operation they belong to, and the whole list is written with that
// operation's Routing Log entry (RoutingLog.cpp). Header-only so the two
// boundaries note a call without linking the log itself.
//
// Held in process memory until written: markers, categories, provider and
// model names, a page address and, for a protected model request, its
// passages exactly as they left - never a value a marker stands for, never
// the text a request was built from, never a request namespace.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "PapinianGuard/Types/PrivacyMetadata.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Routing/ProtectedInference/SourceMaterialSpans.hpp"

namespace singularity::routing::outbound {

// An operation that never reaches the Routing Log (a preview, Context
// upkeep) must not keep its calls for ever: the oldest operations go first.
constexpr std::size_t kKeptOperations = 256;
// A runaway loop cannot grow one operation's list without end either.
constexpr std::size_t kKeptCallsPerOperation = 512;
// The passages kept as they left: one passage, one operation, and everything
// held here at once each have a ceiling. What does not fit is not kept, and a
// passage not kept is later said to be not kept - never guessed.
constexpr std::size_t kKeptPassageBytes = 256 * 1024;
constexpr std::size_t kKeptPassageBytesPerOperation = 4 * 1024 * 1024;
constexpr std::size_t kKeptPassageBytesInMemory = 32 * 1024 * 1024;

// The calls noted under one key, and where and when the latest was made: an
// attempt runs on one thread from start to end, so a call noted on the thread
// that writes the attempt's entry, since the attempt started, is the
// attempt's own (RunCalls below).
struct NotedCalls {
	std::vector<protected_api::papinian::OutboundCall> calls;
	std::thread::id thread;
	std::chrono::steady_clock::time_point last;
	// Noted under an operation's own id, not a single request's namespace: a
	// Context or Memory call is a namespace of its own and belongs to no
	// attempt's entry.
	bool isOperation = false;
	// The bytes of sent passages these calls hold.
	std::size_t passageBytes = 0;
};

struct Registry {
	std::mutex mutex;
	std::map<std::string, NotedCalls> calls;
	std::deque<std::string> order;
	std::size_t passageBytes = 0;
	// Per operation: every operation id already listed as requested by one of
	// its calls, or met before its first call (an earlier attempt's), so each
	// is listed once and never on a call that did not ask it.
	std::map<std::string, std::set<std::string>> requested;
};

inline Registry &registry() {
	static Registry value;
	return value;
}

inline std::string utc_now() {
	const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm parts{};
#if defined(_WIN32)
	gmtime_s(&parts, &now);
#else
	gmtime_r(&now, &parts);
#endif
	char text[32];
	std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
	return text;
}

// A fresh id for one protection table's calls (OutboundCall::symbol_scope):
// sixteen hexadecimal digits from the system's secure source
// (Platform/SecureRandom), naming nothing but the table. A marker names a value
// only within its own table, so two tables must never share an id.
inline std::string new_symbol_scope() {
	return platform::secure_random_hex(8);
}

// The calls an operation no longer holds count no more against what is held.
inline void forget_passage_bytes(Registry &kept, const NotedCalls &noted) {
	kept.passageBytes -= noted.passageBytes <= kept.passageBytes ? noted.passageBytes : kept.passageBytes;
}

// Where each model call that left is written into this computer's work record
// (D-252, Usage/WorkRecord): installed once by the app, which then gives the
// call its place there (record_chain, record_index). None in a test that
// installs none, so a call is then noted as it was.
inline std::function<void(protected_api::papinian::OutboundCall &)> &work_record_sink() {
	static std::function<void(protected_api::papinian::OutboundCall &)> sink;
	return sink;
}

// Notes one call under its operation. Never throws: a record that cannot be
// kept must not undo a request that already left. `isOperation` is false for
// a call keyed by its own request namespace, which no attempt's entry can
// claim as its own.
inline void record_call(const std::string &operationKey, protected_api::papinian::OutboundCall call,
		bool isOperation = true) {
	if (operationKey.empty() == true) return;
	try {
		if (call.time.empty() == true) call.time = utc_now();
		// Its place in this computer's record, before it is noted: the record is
		// of the call as it is billed.
		if (call.kind == "model" && work_record_sink()) {
			try {
				work_record_sink()(call);
			} catch (...) {
			}
		}
		Registry &kept = registry();
		const std::lock_guard<std::mutex> lock(kept.mutex);
		auto found = kept.calls.find(operationKey);
		if (found == kept.calls.end()) {
			while (kept.order.size() >= kKeptOperations) {
				const auto oldest = kept.calls.find(kept.order.front());
				if (oldest != kept.calls.end()) {
					forget_passage_bytes(kept, oldest->second);
					kept.calls.erase(oldest);
				}
				kept.requested.erase(kept.order.front());
				kept.order.pop_front();
			}
			kept.order.push_back(operationKey);
			found = kept.calls.emplace(operationKey, NotedCalls()).first;
		}
		NotedCalls &noted = found->second;
		noted.thread = std::this_thread::get_id();
		noted.last = std::chrono::steady_clock::now();
		if (isOperation == true) noted.isOperation = true;
		if (noted.calls.size() >= kKeptCallsPerOperation) return;

		/* - - - - - - */

		// The passages that fit this operation's ceiling are kept; room for them
		// in memory is made by letting go of the oldest operations' passages
		// first - their markers and counts stay.
		std::vector<protected_api::papinian::OutboundPassage> fitting;
		std::size_t bytes = 0;
		for (protected_api::papinian::OutboundPassage &passage : call.passages) {
			if (noted.passageBytes + bytes + passage.sent.size() > kKeptPassageBytesPerOperation) continue;
			bytes += passage.sent.size();
			fitting.push_back(std::move(passage));
		}
		call.passages = std::move(fitting);
		for (const std::string &key : kept.order) {
			if (kept.passageBytes + bytes <= kKeptPassageBytesInMemory) break;
			if (key == operationKey) continue;
			NotedCalls &older = kept.calls.at(key);
			if (older.passageBytes == 0) continue;
			forget_passage_bytes(kept, older);
			older.passageBytes = 0;
			for (protected_api::papinian::OutboundCall &olderCall : older.calls) olderCall.passages.clear();
		}
		if (kept.passageBytes + bytes > kKeptPassageBytesInMemory) {
			call.passages.clear();
			bytes = 0;
		}
		noted.passageBytes += bytes;
		kept.passageBytes += bytes;
		noted.calls.push_back(std::move(call));
	} catch (...) {
	}
}

// The operations the model asked Singularity to work out in the response of the
// operation's latest model call under `symbolScope` (DESIGN4 §11.2: "local
// operations requested" belong to the call whose RESPONSE asked them): added
// to that call - refused and erroring ones too, and ones no later call ever
// returned - each marked with the call's payload digest. Called before the
// next call is noted, with every operation the table holds: one already listed
// by any call of the operation is skipped, and one met before the operation's
// first model call (an earlier attempt's) is only remembered, never listed on
// a call that did not ask it. Returns that call's digest, or "". Never throws.
inline std::string note_operations_requested(const std::string &operationKey, const std::string &symbolScope,
		const std::vector<protected_api::papinian::OutboundOperation> &operations) {
	if (operationKey.empty() == true || operations.empty() == true) return {};
	try {
		Registry &kept = registry();
		const std::lock_guard<std::mutex> lock(kept.mutex);
		std::set<std::string> &known = kept.requested[operationKey];
		const auto found = kept.calls.find(operationKey);
		protected_api::papinian::OutboundCall *asking = nullptr;
		if (found != kept.calls.end())
			for (auto call = found->second.calls.rbegin(); call != found->second.calls.rend(); ++call) {
				if (call->kind != "model" || call->symbol_scope != symbolScope) continue;
				asking = &*call;
				break;
			}
		if (asking != nullptr)
			for (const protected_api::papinian::OutboundOperation &asked : asking->operations_requested) known.insert(asked.op_id);
		for (protected_api::papinian::OutboundOperation operation : operations) {
			if (operation.op_id.empty() == true || known.insert(operation.op_id).second == false) continue;
			if (asking == nullptr) continue;
			operation.asked_in_reply_to = asking->payload_sha256;
			asking->operations_requested.push_back(std::move(operation));
		}
		return asking == nullptr ? std::string() : asking->payload_sha256;
	} catch (...) {
	}
	return {};
}

// Every operation id the operation's calls already list as requested (or met
// before its first call). Never throws.
inline std::set<std::string> requested_ops(const std::string &operationKey) {
	try {
		Registry &kept = registry();
		const std::lock_guard<std::mutex> lock(kept.mutex);
		const auto found = kept.requested.find(operationKey);
		if (found != kept.requested.end()) return found->second;
	} catch (...) {
	}
	return {};
}

// The payload digest of the call of the operation whose response asked for
// the operation `opId`, or "" when none of its calls did. Never throws.
inline std::string asked_in(const std::string &operationKey, const std::string &opId) {
	try {
		Registry &kept = registry();
		const std::lock_guard<std::mutex> lock(kept.mutex);
		const auto found = kept.calls.find(operationKey);
		if (found == kept.calls.end()) return {};
		for (const protected_api::papinian::OutboundCall &call : found->second.calls)
			for (const protected_api::papinian::OutboundOperation &asked : call.operations_requested)
				if (asked.op_id == opId) return call.payload_sha256;
	} catch (...) {
	}
	return {};
}

// Every call noted under the operation so far, in the order made.
inline std::vector<protected_api::papinian::OutboundCall> calls_for(const std::string &operationKey) {
	Registry &kept = registry();
	const std::lock_guard<std::mutex> lock(kept.mutex);
	const auto found = kept.calls.find(operationKey);
	if (found == kept.calls.end()) return {};
	return found->second.calls;
}

// The operation's calls, handed over once to be written: what is written is
// no longer held here, and a later run of the same operation starts a new list.
inline std::vector<protected_api::papinian::OutboundCall> take_calls(const std::string &operationKey) {
	Registry &kept = registry();
	const std::lock_guard<std::mutex> lock(kept.mutex);
	const auto found = kept.calls.find(operationKey);
	if (found == kept.calls.end()) {
		kept.requested.erase(operationKey);
		return {};
	}
	std::vector<protected_api::papinian::OutboundCall> taken = std::move(found->second.calls);
	forget_passage_bytes(kept, found->second);
	kept.calls.erase(found);
	kept.requested.erase(operationKey);
	for (auto position = kept.order.begin(); position != kept.order.end(); ++position) {
		if (*position != operationKey) continue;
		kept.order.erase(position);
		break;
	}
	return taken;
}

// The calls an attempt made that its entry was not handed by key: a request
// that failed after it left, a web search a local answer ran. The attempt ran
// on this thread since `since`, so every operation noted here since then is
// its own - an earlier attempt on this thread, paused for the lawyer, noted
// its calls before. When only single-request calls were noted in that time
// (a Context or Memory upkeep this thread ran, or an attempt with no
// operation of its own), whose they are cannot be said: the attempt's calls
// are then unknown, never "nothing".
struct RunCalls {
	std::vector<std::string> keys;
	bool isKnown = true;
};

inline RunCalls run_calls(std::chrono::steady_clock::time_point since) {
	RunCalls run;
	std::vector<std::string> requests;
	bool isOperationFound = false;
	Registry &kept = registry();
	const std::lock_guard<std::mutex> lock(kept.mutex);
	for (const std::string &key : kept.order) {
		const NotedCalls &noted = kept.calls.at(key);
		if (noted.thread != std::this_thread::get_id() || noted.last < since || noted.calls.empty() == true) continue;
		if (noted.isOperation == true) isOperationFound = true;
		(noted.isOperation == true ? run.keys : requests).push_back(key);
	}
	if (isOperationFound == true) run.keys.insert(run.keys.end(), requests.begin(), requests.end());
	else if (requests.empty() == false) run.isKnown = false;
	return run;
}

// Every protection marker written in `text`, each once, in the base form the
// request mapping names it by: "[[E3.short]]" and "[[E3]]" are both "[[E3]]".
inline std::vector<std::string> markers_in(const std::string &text) {
	std::vector<std::string> found;
	std::set<std::string> seen;
	std::size_t cursor = 0;
	while (true) {
		const std::size_t opened = text.find("[[", cursor);
		if (opened == std::string::npos) break;
		const std::size_t closed = text.find("]]", opened + 2);
		if (closed == std::string::npos) break;
		const std::string inside = text.substr(opened + 2, closed - opened - 2);
		const std::size_t part = inside.find('.');
		const std::string name = part == std::string::npos ? inside : inside.substr(0, part);
		bool isMarker = name.empty() == false && name.size() <= 32;
		for (const char letter : name) {
			const bool isAllowed = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')
				|| (letter >= '0' && letter <= '9') || letter == '_';
			if (isAllowed == false) isMarker = false;
		}
		if (isMarker == true) {
			const std::string marker = "[[" + name + "]]";
			if (seen.insert(marker).second == true) found.push_back(marker);
			cursor = closed + 2;
			continue;
		}
		cursor = opened + 2;
	}
	return found;
}

/* - - - - - - */

// The passages of one protected model request exactly as they left: every
// source passage ("[SOURCE:DOC_n:CHUNK_m]\n<text>\n[/SOURCE]", in a rendering's
// SOURCE_MATERIAL or a typed request's sourceMaterial) and the lawyer's
// instruction (a rendering's CURRENT_REQUEST, a typed request's
// currentRequest), and every record of what the Project remembers that
// carried a marker (a rendering's PROJECT_CONTEXT, one record a line; a typed
// request's projectContext) - a document's name, a data room change, a
// Context item. Each is cut from `sent` - the data plane that was sent -
// and paired with the same place in `built`, the data plane the request was
// built from, whose text is only digested (`digest`) and never kept. A place
// whose passages do not pair one for one is left out, never guessed.
namespace sent_form {

struct Cut {
	std::string opening;
	std::string text;
};

inline std::vector<Cut> source_passages(const std::string &value) {
	static constexpr std::string_view kClosing = "\n[/SOURCE]";
	std::vector<Cut> cuts;
	std::size_t cursor = 0;
	while (true) {
		const std::size_t at = value.find("[SOURCE:DOC_", cursor);
		if (at == std::string::npos) break;
		const std::size_t opening = source_material::opening_marker_length(value, at, value.size());
		if (opening == 0) {
			cursor = at + 1;
			continue;
		}
		const std::size_t textStart = at + opening;
		const std::size_t closing = value.find(kClosing, textStart);
		if (closing == std::string::npos) break;
		cuts.push_back({value.substr(at, opening), value.substr(textStart, closing - textStart)});
		cursor = closing + kClosing.size();
	}
	return cuts;
}

// The title above each document's passages ("[DOCUMENT:DOC_n]\n<title>\n
// [/DOCUMENT]"), as source_passages() cuts the passages.
inline std::vector<Cut> document_titles(const std::string &value) {
	std::vector<Cut> cuts;
	std::size_t cursor = 0;
	while (true) {
		const std::size_t at = value.find("[DOCUMENT:DOC_", cursor);
		if (at == std::string::npos) break;
		const std::size_t opening = source_material::document_header_opening_length(value, at, value.size());
		if (opening == 0) {
			cursor = at + 1;
			continue;
		}
		const std::size_t textStart = at + opening;
		const std::size_t closing = value.find(source_material::kDocumentHeaderClosing, textStart);
		if (closing == std::string::npos) break;
		cuts.push_back({value.substr(at, opening), value.substr(textStart, closing - textStart)});
		cursor = closing + source_material::kDocumentHeaderClosing.size();
	}
	return cuts;
}

// A rendering's section, the first time it opens: the assembler escapes every
// "<<<LM_" its content holds, so no text inside can open one of its own.
inline bool section_content(const std::string &value, const std::string &name, std::string &content) {
	const std::string opening = "<<<LM_SECTION:" + name + ">>>\n";
	const std::string closing = "\n<<<LM_END_SECTION:" + name + ">>>";
	const std::size_t at = value.find(opening);
	if (at == std::string::npos) return false;
	const std::size_t end = value.find(closing, at + opening.size());
	if (end == std::string::npos) return false;
	content = value.substr(at + opening.size(), end - at - opening.size());
	return true;
}

struct Collector {
	const std::function<std::string(const std::string &)> &digest;
	std::vector<protected_api::papinian::OutboundPassage> passages;
	std::set<std::pair<std::string, std::string>> seen;

	void add(const std::string &kind, const std::string &chunkId, const std::string &built, const std::string &sent) {
		if (sent.empty() == true || sent.size() > kKeptPassageBytes) return;
		protected_api::papinian::OutboundPassage passage;
		passage.kind = kind;
		passage.chunk_id = chunkId;
		passage.content_sha256 = digest(built);
		if (seen.insert({passage.content_sha256, sent}).second == false) return;
		passage.sent = sent;
		passages.push_back(std::move(passage));
	}

	// What the Project remembers goes one record a line: each line that left
	// with a marker in it is a passage of its own, paired with the same line of
	// what it was built from. Lines that do not pair one for one are left out.
	void add_context_lines(const std::string &built, const std::string &sent) {
		std::vector<std::string> builtLines;
		std::vector<std::string> sentLines;
		std::size_t start = 0;
		while (start <= built.size()) {
			const std::size_t end = built.find('\n', start);
			builtLines.push_back(built.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (end == std::string::npos) break;
			start = end + 1;
		}
		start = 0;
		while (start <= sent.size()) {
			const std::size_t end = sent.find('\n', start);
			sentLines.push_back(sent.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (end == std::string::npos) break;
			start = end + 1;
		}
		if (builtLines.size() != sentLines.size()) return;
		for (std::size_t index = 0; index < builtLines.size(); ++index) {
			if (sentLines[index].find("[[") == std::string::npos || sentLines[index] == builtLines[index]) continue;
			add("context", std::string(), builtLines[index], sentLines[index]);
		}
	}

	void collect(const nlohmann::json &built, const nlohmann::json &sent, const std::string &key, const std::string &chunkId) {
		if (built.is_object() == true && sent.is_object() == true) {
			const auto recordClass = built.find("contentClass");
			if (recordClass != built.end() && *recordClass == "project_context") {
				const auto builtContent = built.find("content");
				const auto sentContent = sent.find("content");
				if (builtContent != built.end() && sentContent != sent.end() && builtContent->is_string() == true
						&& sentContent->is_string() == true)
					add_context_lines(builtContent->get_ref<const std::string &>(), sentContent->get_ref<const std::string &>());
				return;
			}
			// A typed passage names the chunk it is: its handle is the chunk id
			// and passes through Papinian Guard unchanged.
			std::string handle;
			const auto contentClass = built.find("contentClass");
			const auto named = built.find("handle");
			if (contentClass != built.end() && *contentClass == "untrusted_evidence" && named != built.end()
					&& named->is_string() == true)
				handle = named->get<std::string>();
			for (auto entry = built.begin(); entry != built.end(); ++entry) {
				const auto other = sent.find(entry.key());
				if (other == sent.end()) continue;
				collect(entry.value(), *other, entry.key(), entry.key() == "content" ? handle : std::string());
			}
			return;
		}
		if (built.is_array() == true && sent.is_array() == true) {
			if (built.size() != sent.size()) return;
			for (std::size_t index = 0; index < built.size(); ++index) collect(built.at(index), sent.at(index), key, std::string());
			return;
		}
		if (built.is_string() == false || sent.is_string() == false) return;
		const std::string &builtText = built.get_ref<const std::string &>();
		const std::string &sentText = sent.get_ref<const std::string &>();
		if (key == "currentRequest") add("instruction", std::string(), builtText, sentText);
		std::string builtRequest;
		std::string sentRequest;
		if (section_content(builtText, "CURRENT_REQUEST", builtRequest) == true
				&& section_content(sentText, "CURRENT_REQUEST", sentRequest) == true)
			add("instruction", std::string(), builtRequest, sentRequest);
		std::string builtContext;
		std::string sentContext;
		if (section_content(builtText, "PROJECT_CONTEXT", builtContext) == true
				&& section_content(sentText, "PROJECT_CONTEXT", sentContext) == true)
			add_context_lines(builtContext, sentContext);
		// A document's name as it left, where protection hid part of it.
		const std::vector<Cut> builtTitles = document_titles(builtText);
		const std::vector<Cut> sentTitles = document_titles(sentText);
		if (builtTitles.size() == sentTitles.size())
			for (std::size_t index = 0; index < builtTitles.size(); ++index)
				if (builtTitles[index].opening == sentTitles[index].opening && sentTitles[index].text.find("[[") != std::string::npos
						&& sentTitles[index].text != builtTitles[index].text)
					add("context", std::string(), builtTitles[index].text, sentTitles[index].text);
		const std::vector<Cut> builtCuts = source_passages(builtText);
		const std::vector<Cut> sentCuts = source_passages(sentText);
		if (builtCuts.size() != sentCuts.size()) return;
		for (std::size_t index = 0; index < builtCuts.size(); ++index)
			if (builtCuts[index].opening != sentCuts[index].opening) return;
		for (std::size_t index = 0; index < builtCuts.size(); ++index)
			add("source", builtCuts.size() == 1 ? chunkId : std::string(), builtCuts[index].text, sentCuts[index].text);
	}
};

} // namespace sent_form

inline std::vector<protected_api::papinian::OutboundPassage> sent_passages(const nlohmann::json &built,
		const nlohmann::json &sent, const std::function<std::string(const std::string &)> &digest) {
	sent_form::Collector collector{digest, {}, {}};
	collector.collect(built, sent, std::string(), std::string());
	return std::move(collector.passages);
}

} // namespace singularity::routing::outbound
