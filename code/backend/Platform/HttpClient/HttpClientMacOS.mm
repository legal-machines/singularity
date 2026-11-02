#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/Trace/Trace.hpp"

#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

// One streamed response, collected for the thread waiting on it: what arrived
// since that thread last looked, the whole body, the status and how it ended.
// Its callbacks run on the session's delegate queue; the waiting thread reads
// under the same lock and is woken by `arrived`.
@interface LMStreamedResponse : NSObject <NSURLSessionDataDelegate> {
@public
  std::mutex lock;
  std::string pending;
  std::string whole;
  NSInteger status;
  NSString *retryAfter;
  NSError *failure;
  bool finished;
  dispatch_semaphore_t arrived;
}
@end

@implementation LMStreamedResponse

- (instancetype)init {
  if ((self = [super init])) {
    status = 0;
    retryAfter = nil;
    failure = nil;
    finished = false;
    arrived = dispatch_semaphore_create(0);
  }
  return self;
}

- (void)dealloc {
  [retryAfter release];
  [failure release];
  dispatch_release(arrived);
  [super dealloc];
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
didReceiveResponse:(NSURLResponse *)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
  {
    const std::lock_guard<std::mutex> guard(lock);
    if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
      status = ((NSHTTPURLResponse *)response).statusCode;
      [retryAfter release];
      retryAfter = [[((NSHTTPURLResponse *)response) valueForHTTPHeaderField:@"Retry-After"] retain];
    }
  }
  completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)dataTask didReceiveData:(NSData *)data {
  {
    const std::lock_guard<std::mutex> guard(lock);
    [data enumerateByteRangesUsingBlock:^(const void *bytes, NSRange range, BOOL *) {
      pending.append(static_cast<const char *>(bytes), range.length);
      whole.append(static_cast<const char *>(bytes), range.length);
    }];
  }
  dispatch_semaphore_signal(arrived);
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
  {
    const std::lock_guard<std::mutex> guard(lock);
    [failure release];
    failure = [error retain];
    finished = true;
  }
  dispatch_semaphore_signal(arrived);
}

@end

namespace singularity::platform {
namespace {

// One session for the life of Singularity: its connections, TLS sessions and
// HTTP/2 streams are kept from one request to the next, so a request to a
// provider already reached starts without a new handshake (a fresh session
// per request paid one every time). Ephemeral: no cookies, no cache, nothing
// stored on disk. The session is never invalidated; each request bounds
// itself (perform).
NSURLSession *shared_session() {
  static NSURLSession *session = [] {
    NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.HTTPShouldSetCookies = NO;
    configuration.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    configuration.HTTPCookieStorage = nil;
    configuration.URLCache = nil;
    configuration.URLCredentialStorage = nil;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    // Ceilings only: every request sets its own, shorter ones.
    configuration.timeoutIntervalForRequest = 3600;
    configuration.timeoutIntervalForResource = 4 * 3600;
    return [[NSURLSession sessionWithConfiguration:configuration] retain];
  }();
  return session;
}

// Synchronous by design: every caller already runs on the bridge's
// background worker pool (see Bridge/NativeBridge/NativeBridge.cpp's
// ExecutionPolicy::Worker), so blocking here is the simplest correct
// option rather than threading a callback through the bridge's own
// request/response plumbing a second time. The semaphore is bounded by
// NSURLSessionConfiguration's own timeout, so this cannot hang forever, and a
// cancelled HttpCancelToken ends it at once.
HttpResponse perform(NSMutableURLRequest *request, int timeout_seconds) {
  @autoreleasepool {
    // Diagnostic trace: where the request went and what it cost. Host, path and
    // sizes only - never a header, a query string or a body.
    trace::Span span("http", request.HTTPMethod != nil ? [request.HTTPMethod UTF8String] : "REQUEST");
    if (span.active()) {
      span.add("host", request.URL.host != nil ? std::string([request.URL.host UTF8String]) : std::string());
      span.add("path", request.URL.path != nil ? std::string([request.URL.path UTF8String]) : std::string());
      span.add("requestBytes", static_cast<std::size_t>(request.HTTPBody.length));
      span.add("timeoutSeconds", timeout_seconds);
    }
    // The longest silence the request may keep; the whole request is bounded
    // below.
    request.timeoutInterval = timeout_seconds;
    NSURLSession *session = shared_session();

    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

    __block NSData *response_data = nil;
    __block NSInteger status_code = 0;
    __block NSError *request_error = nil;
    __block NSString *retry_after = nil;

    // A request whose work was already stopped is never sent.
    const std::shared_ptr<HttpCancelToken> cancel_token = current_http_cancel_token();
    if (cancel_token != nullptr && cancel_token->is_cancelled()) {
      span.add("outcome", "cancelled");
      throw HttpTransportError("The request was stopped before it was sent.", false, true);
    }

    NSURLSessionDataTask *task = [session
        dataTaskWithRequest:request
          completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            response_data = [data retain];
            request_error = [error retain];
            if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
              status_code = ((NSHTTPURLResponse *)response).statusCode;
              retry_after = [[((NSHTTPURLResponse *)response) valueForHTTPHeaderField:@"Retry-After"] retain];
            }
            dispatch_semaphore_signal(semaphore);
          }];
    [task resume];
    // Stop cancels the connection itself; the completion handler then runs with
    // NSURLErrorCancelled and the wait below ends at once. The task outlives
    // the abort: clear_abort() runs before this function returns.
    if (cancel_token != nullptr && cancel_token->set_abort([task] { [task cancel]; }) == false) [task cancel];
    // The whole request, as curl's CURLOPT_TIMEOUT bounds it on Linux: a provider
    // that keeps the connection alive with a trickle of bytes (a model thinking
    // for sixteen minutes, live 26 Sep 2026) is still stopped at its timeout.
    bool is_past_timeout = false;
    if (dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(timeout_seconds) * NSEC_PER_SEC)) != 0) {
      is_past_timeout = true;
      [task cancel];
      dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
    }
    if (cancel_token != nullptr) cancel_token->clear_abort();

    if (request_error != nullptr) {
      std::string message([request_error.localizedDescription UTF8String]);
      // localizedDescription above is the user's OWN language - never match
      // English prose against it. NSURLErrorTimedOut is the stable,
      // locale-independent fact, so carry it typed (see HttpTransportError).
      const bool timed_out = is_past_timeout || ([request_error.domain isEqualToString:NSURLErrorDomain]
                             && request_error.code == NSURLErrorTimedOut);
      const bool cancelled = is_past_timeout == false && [request_error.domain isEqualToString:NSURLErrorDomain]
                             && request_error.code == NSURLErrorCancelled && cancel_token != nullptr
                             && cancel_token->is_cancelled();
      [response_data release];
      [request_error release];
      [retry_after release];
      span.add("outcome", cancelled ? "cancelled" : timed_out ? "timed_out" : "transport_error");
      if (cancelled) throw HttpTransportError("The request was stopped.", false, true);
      throw HttpTransportError(message, timed_out);
    }
    [request_error release];

    HttpResponse result;
    result.status = static_cast<int>(status_code);
    if (response_data != nullptr) {
      result.body = std::string(reinterpret_cast<const char *>(response_data.bytes),
                                response_data.length);
    }
    [response_data release];
    const char *retry_after_text = retry_after != nil ? [retry_after UTF8String] : nullptr;
    if (retry_after_text != nullptr) result.retry_after = retry_after_text;
    [retry_after release];
    span.add("status", result.status);
    span.add("responseBytes", result.body.size());
    return result;
  }
}

// perform(), with the body told to `receive` as it arrives (a task's own
// delegate: macOS 12, below the 13.4 Singularity requires).
HttpResponse perform_streaming(NSMutableURLRequest *request, int timeout_seconds, const HttpBodyReceiver &receive) {
  @autoreleasepool {
    trace::Span span("http", "POST");
    if (span.active()) {
      span.add("host", request.URL.host != nil ? std::string([request.URL.host UTF8String]) : std::string());
      span.add("path", request.URL.path != nil ? std::string([request.URL.path UTF8String]) : std::string());
      span.add("requestBytes", static_cast<std::size_t>(request.HTTPBody.length));
      span.add("timeoutSeconds", timeout_seconds);
      span.add("streamed", "true");
    }
    request.timeoutInterval = timeout_seconds;
    const std::shared_ptr<HttpCancelToken> cancel_token = current_http_cancel_token();
    if (cancel_token != nullptr && cancel_token->is_cancelled()) {
      span.add("outcome", "cancelled");
      throw HttpTransportError("The request was stopped before it was sent.", false, true);
    }
    LMStreamedResponse *streamed = [[LMStreamedResponse alloc] init];
    // The shared session keeps its connections; this task alone reports to
    // `streamed` as its bytes arrive.
    NSURLSessionDataTask *task = [shared_session() dataTaskWithRequest:request];
    task.delegate = streamed;
    [task resume];
    if (cancel_token != nullptr && cancel_token->set_abort([task] { [task cancel]; }) == false) [task cancel];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool is_past_timeout = false;
    std::exception_ptr refused;
    std::size_t told = 0;
    // Waits for the end, telling `receive` each piece as it comes: on this
    // thread, never under the lock the delegate queue takes.
    const auto wait_for_end = [&](bool tell) {
      for (;;) {
        std::string piece;
        bool finished = false;
        NSInteger status = 0;
        {
          const std::lock_guard<std::mutex> guard(streamed->lock);
          piece.swap(streamed->pending);
          finished = streamed->finished;
          status = streamed->status;
        }
        if (tell && !piece.empty() && status >= 200 && status < 300 && receive) {
          try {
            receive(piece);
            told += piece.size();
          } catch (...) {
            refused = std::current_exception();
            tell = false;
            [task cancel];
          }
        }
        if (finished) return;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline && is_past_timeout == false) {
          // The whole request is bounded, as https_post bounds it.
          is_past_timeout = true;
          [task cancel];
        }
        const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
        const int64_t wait = is_past_timeout ? static_cast<int64_t>(NSEC_PER_SEC)
            : std::min<int64_t>(static_cast<int64_t>(NSEC_PER_SEC), std::max<int64_t>(left, 1));
        dispatch_semaphore_wait(streamed->arrived, dispatch_time(DISPATCH_TIME_NOW, wait));
      }
    };
    wait_for_end(true);
    if (cancel_token != nullptr) cancel_token->clear_abort();
    span.add("toldBytes", told);
    if (refused) {
      [streamed release];
      span.add("outcome", "refused_by_reader");
      std::rethrow_exception(refused);
    }
    NSError *request_error = streamed->failure;
    if (request_error != nil) {
      const std::string message([request_error.localizedDescription UTF8String]);
      const bool timed_out = is_past_timeout || ([request_error.domain isEqualToString:NSURLErrorDomain]
                             && request_error.code == NSURLErrorTimedOut);
      const bool cancelled = is_past_timeout == false && [request_error.domain isEqualToString:NSURLErrorDomain]
                             && request_error.code == NSURLErrorCancelled && cancel_token != nullptr
                             && cancel_token->is_cancelled();
      [streamed release];
      span.add("outcome", cancelled ? "cancelled" : timed_out ? "timed_out" : "transport_error");
      if (cancelled) throw HttpTransportError("The request was stopped.", false, true);
      throw HttpTransportError(message, timed_out);
    }
    HttpResponse result;
    {
      const std::lock_guard<std::mutex> guard(streamed->lock);
      result.status = static_cast<int>(streamed->status);
      result.body.swap(streamed->whole);
      if (streamed->retryAfter != nil) result.retry_after = [streamed->retryAfter UTF8String];
    }
    [streamed release];
    span.add("status", result.status);
    span.add("responseBytes", result.body.size());
    return result;
  }
}

} // namespace

HttpResponse https_get(const std::string &url, const std::vector<HttpHeader> &headers,
                       int timeout_seconds) {
  @autoreleasepool {
    NSURL *ns_url = [NSURL URLWithString:@(url.c_str())];
    if (ns_url == nullptr) {
      throw std::runtime_error("Invalid URL: " + url);
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:ns_url];
    request.HTTPMethod = @"GET";
    for (const auto &header : headers) {
      [request setValue:@(header.value.c_str()) forHTTPHeaderField:@(header.name.c_str())];
    }

    return perform(request, timeout_seconds);
  }
}

HttpResponse https_post(const std::string &url, const std::vector<HttpHeader> &headers,
                        const std::string &body, int timeout_seconds) {
  @autoreleasepool {
    NSURL *ns_url = [NSURL URLWithString:@(url.c_str())];
    if (ns_url == nullptr) {
      throw std::runtime_error("Invalid URL: " + url);
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:ns_url];
    request.HTTPMethod = @"POST";
    request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
    for (const auto &header : headers) {
      [request setValue:@(header.value.c_str()) forHTTPHeaderField:@(header.name.c_str())];
    }

    return perform(request, timeout_seconds);
  }
}

HttpResponse https_post_streaming(const std::string &url, const std::vector<HttpHeader> &headers,
                                  const std::string &body, int timeout_seconds, const HttpBodyReceiver &receive) {
  @autoreleasepool {
    NSURL *ns_url = [NSURL URLWithString:@(url.c_str())];
    if (ns_url == nullptr) {
      throw std::runtime_error("Invalid URL: " + url);
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:ns_url];
    request.HTTPMethod = @"POST";
    request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
    for (const auto &header : headers) {
      [request setValue:@(header.value.c_str()) forHTTPHeaderField:@(header.name.c_str())];
    }

    return perform_streaming(request, timeout_seconds, receive);
  }
}

int https_download_file(const std::string &url,
                        const std::vector<HttpHeader> &headers,
                        const std::filesystem::path &destination,
                        int timeout_seconds,
                        const DownloadProgress &progress) {
  @autoreleasepool {
    NSURL *ns_url = [NSURL URLWithString:@(url.c_str())];
    if (ns_url == nullptr || ![[ns_url scheme] isEqualToString:@"https"]) {
      throw std::runtime_error("Model downloads require a valid HTTPS URL");
    }
    const std::shared_ptr<HttpCancelToken> cancel_token = current_http_cancel_token();
    if (cancel_token != nullptr && cancel_token->is_cancelled()) {
      throw HttpTransportError("The download was stopped before it began.", false, true);
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:ns_url];
    request.HTTPMethod = @"GET";
    for (const auto &header : headers) {
      [request setValue:@(header.value.c_str()) forHTTPHeaderField:@(header.name.c_str())];
    }

    NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    // The request interval is the longest silence the connection may keep; the
    // resource interval bounds the whole transfer, which for a model package on
    // an ordinary connection takes far longer than a minute.
    configuration.timeoutIntervalForRequest = std::min(timeout_seconds, 60);
    configuration.timeoutIntervalForResource = timeout_seconds;
    configuration.HTTPShouldSetCookies = NO;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration];
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    __block NSInteger status_code = 0;
    __block NSError *request_error = nil;
    __block NSError *move_error = nil;
    const auto destination_string = destination.string();

    NSURLSessionDownloadTask *task = [session
        downloadTaskWithRequest:request
             completionHandler:^(NSURL *location, NSURLResponse *response, NSError *error) {
               request_error = [error retain];
               if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                 status_code = ((NSHTTPURLResponse *)response).statusCode;
               }
               if (error == nullptr && status_code >= 200 && status_code < 300 && location != nullptr) {
                 NSURL *target = [NSURL fileURLWithPath:@(destination_string.c_str())];
                 [[NSFileManager defaultManager] moveItemAtURL:location toURL:target error:&move_error];
                 [move_error retain];
               }
               dispatch_semaphore_signal(semaphore);
             }];
    [task resume];
    // A download task writes to a file of its own until it finishes, so the
    // destination cannot be watched for progress: the task's own count of
    // received bytes is read instead, while waiting. The same wait notices a
    // cancelled token and cancels the task; its completion handler then runs
    // with NSURLErrorCancelled and ends the wait.
    bool isCancelRequested = false;
    while (dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC)) != 0) {
      if (isCancelRequested == false && cancel_token != nullptr && cancel_token->is_cancelled()) {
        isCancelRequested = true;
        [task cancel];
      }
      if (progress) {
        const int64_t received = task.countOfBytesReceived;
        if (received > 0) progress(static_cast<std::uint64_t>(received));
      }
    }
    [session finishTasksAndInvalidate];
    if (request_error != nullptr) {
      const std::string message([request_error.localizedDescription UTF8String]);
      const bool timed_out = [request_error.domain isEqualToString:NSURLErrorDomain]
                             && request_error.code == NSURLErrorTimedOut;
      [request_error release];
      [move_error release];
      if (isCancelRequested == true) throw HttpTransportError("The download was stopped.", false, true);
      throw HttpTransportError(message, timed_out);
    }
    if (move_error != nullptr) {
      const std::string message([move_error.localizedDescription UTF8String]);
      [move_error release];
      throw std::runtime_error("Unable to save model package: " + message);
    }
    return static_cast<int>(status_code);
  }
}

} // namespace singularity::platform
