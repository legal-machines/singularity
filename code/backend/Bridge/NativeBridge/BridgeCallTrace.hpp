#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "Platform/Trace/Trace.hpp"

namespace singularity::bridge {

// The one diagnostic record a native call leaves when SINGULARITY_TRACE_FILE is
// set: which method, how long it waited and ran, how large its request and
// answer were, and how it ended (a stable code). Never its parameters or its
// answer - a sign-in's password, a document's text and a session token all
// stay out by construction, because nothing but these figures is handed in.
inline void record_bridge_call(std::string_view method, const char *policy, double queued_ms, double handler_ms,
                               std::size_t request_bytes, std::size_t result_bytes, const std::string &outcome,
                               double total_ms) {
	platform::trace::record("bridge", method,
		{{"policy", policy},
		 {"queuedMs", queued_ms},
		 {"handlerMs", handler_ms},
		 {"requestBytes", request_bytes},
		 {"resultBytes", result_bytes},
		 {"outcome", outcome}},
		total_ms);
}

} // namespace singularity::bridge
