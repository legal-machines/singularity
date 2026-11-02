#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <filesystem>

namespace singularity::platform {

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpResponse {
  int status = 0;
  std::string body;
  // The response's Retry-After header as the server sent it (seconds, or an
  // HTTP date), empty when it sent none: how long a provider that turned a
  // request away (429, 503) asks to be left alone before the next one. The only
  // header kept - no other is ever read, logged or observed.
  std::string retry_after;
};

// A transport-level failure that still carries the ONE fact a caller may
// legitimately need to branch on: whether the request exceeded its own
// timeout. It derives from std::runtime_error precisely so that every
// existing `catch (const std::exception &)` site keeps behaving exactly as
// before and only a caller that genuinely needs the distinction opts in.
//
// This exists because `what()` is a LOCALIZED, human-facing string on at
// least one supported platform: HttpClientMacOS.mm surfaces NSError's
// localizedDescription, which macOS renders in the user's own language
// (a real governed local acceptance run on this Russian-locale machine
// produced a Russian sentence, not "The request timed out."). Any control
// flow that matched English prose was therefore silently locale-dependent
// and simply did not fire outside an English-locale machine. Each platform
// client sets `timed_out` from its OWN native, locale-independent signal
// (NSURLErrorTimedOut / CURLE_OPERATION_TIMEDOUT / ERROR_WINHTTP_TIMEOUT),
// never by inspecting the message text.
struct HttpTransportError : std::runtime_error {
  HttpTransportError(const std::string &message, bool request_timed_out, bool request_cancelled = false)
      : std::runtime_error(message), timed_out(request_timed_out), cancelled(request_cancelled) {}

  bool timed_out = false;
  // The request was stopped through its HttpCancelToken - a person's Stop, or
  // Singularity closing - never by the network. Nothing should retry it.
  bool cancelled = false;
};

// Lets another thread stop the requests of one piece of work while they are
// in flight: Stop takes effect on the connection itself, not minutes later
// when a long generation finally answers. A platform client registers how to
// abort its request for as long as the request runs; cancel() calls it (at
// once when the request is already running, or when it starts, since a token
// once cancelled cancels every later request too).
//
// The abort runs under the token's lock, the same lock clear_abort() takes: a
// request that completes at the moment of a Stop waits in clear_abort() for
// the abort to finish, so an abort never touches a request its client has
// already released. An abort must therefore be short and must never call back
// into the token.
class HttpCancelToken {
public:
  void cancel() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (isCancelled_ == true) return;
    isCancelled_ = true;
    if (abort_) abort_();
  }

  bool is_cancelled() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return isCancelled_;
  }

  // Returns false (and does not keep `abort`) when the token is already
  // cancelled: the caller aborts its own request instead.
  bool set_abort(std::function<void()> abort) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (isCancelled_ == true) return false;
    abort_ = std::move(abort);
    return true;
  }

  // Takes the lock cancel() runs the abort under, so once this returns the
  // request's abort is not running and can no longer run.
  void clear_abort() {
    const std::lock_guard<std::mutex> lock(mutex_);
    abort_ = nullptr;
  }

private:
  mutable std::mutex mutex_;
  bool isCancelled_ = false;
  std::function<void()> abort_;
};

// The token the requests made on this thread answer to, or null.
inline std::shared_ptr<HttpCancelToken> &current_http_cancel_token() {
  thread_local std::shared_ptr<HttpCancelToken> token;
  return token;
}

// Binds a token to every request this thread makes while the scope lives.
class HttpCancelScope {
public:
  explicit HttpCancelScope(std::shared_ptr<HttpCancelToken> token)
      : previous_(current_http_cancel_token())
  {
    current_http_cancel_token() = std::move(token);
  }
  ~HttpCancelScope() { current_http_cancel_token() = previous_; }

  HttpCancelScope(const HttpCancelScope &) = delete;
  HttpCancelScope &operator=(const HttpCancelScope &) = delete;

private:
  std::shared_ptr<HttpCancelToken> previous_;
};

// Blocking HTTPS GET. For small, infrequent requests only (model provider
// connectivity checks) - call this from a Worker-policy native bridge
// handler, never from the UI thread.
//
// Throws std::runtime_error for a transport-level failure (DNS, TLS,
// timeout, connection refused). A non-2xx HTTP response is not a thrown
// error - it comes back normally so callers can inspect `status`/`body`
// (e.g. to surface the provider's own error message).
HttpResponse https_get(const std::string &url, const std::vector<HttpHeader> &headers,
                       int timeout_seconds = 15);

// Blocking HTTPS POST with a JSON (or otherwise pre-encoded) request body.
// Same threading/error-handling contract as https_get - Worker policy only,
// never the UI thread. Used for model generation requests (see
// Provider/ProviderRegistry/ProviderRegistry.*), which take meaningfully longer than a
// connectivity check, hence the longer default timeout.
HttpResponse https_post(const std::string &url, const std::vector<HttpHeader> &headers,
                        const std::string &body, int timeout_seconds = 60);

// Told each piece of a response's body as it arrives, in order, on the thread
// that made the request.
using HttpBodyReceiver = std::function<void(std::string_view piece)>;

// https_post, read as it arrives: `receive` is told each piece of a 2xx
// response's body as it comes - a model's answer shown while it is written -
// and the response still carries the whole body. A response of any other
// status is only returned. Same timeout and cancellation as https_post; an
// exception from `receive` stops the request and is rethrown. A platform that
// cannot read a response as it arrives tells `receive` the whole body once.
HttpResponse https_post_streaming(const std::string &url, const std::vector<HttpHeader> &headers,
                                  const std::string &body, int timeout_seconds, const HttpBodyReceiver &receive);

// Told, while a download runs, how many bytes of it have arrived so far. Called
// on the downloading thread; never with a figure the transfer did not reach.
using DownloadProgress = std::function<void(std::uint64_t received_bytes)>;

// Streams an HTTPS response directly to `destination`. Returns the HTTP
// status; non-2xx responses leave no destination file. Intended for large,
// hash-pinned application assets such as local model packages.
//
// `timeout_seconds` bounds the whole transfer; a connection that stops
// delivering anything for a minute fails sooner. The thread's
// HttpCancelToken (HttpCancelScope) stops a download in flight - Singularity
// closing must never wait for a model package to finish arriving - and a
// stopped download throws HttpTransportError with `cancelled` set.
int https_download_file(const std::string &url,
                        const std::vector<HttpHeader> &headers,
                        const std::filesystem::path &destination,
                        int timeout_seconds = 600,
                        const DownloadProgress &progress = {});

} // namespace singularity::platform
