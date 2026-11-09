#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/HttpClient/HttpClientSeam.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winhttp.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <fstream>

namespace singularity::platform {
namespace {

std::wstring utf8_to_wide(const std::string &value) {
  if (value.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), size);
  return result;
}

class WinHttpHandle final {
public:
  explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
  ~WinHttpHandle() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
    }
  }
  WinHttpHandle(const WinHttpHandle &) = delete;
  WinHttpHandle &operator=(const WinHttpHandle &) = delete;

  operator HINTERNET() const { return handle_; }
  bool valid() const { return handle_ != nullptr; }
  // Gives up ownership of a handle closed elsewhere (a Stop's abort), so it is
  // never closed twice.
  void release() { handle_ = nullptr; }

private:
  HINTERNET handle_;
};

// Registers a Stop's abort for one WinHTTP request while it runs. Closing the
// request handle is how WinHTTP cancels a synchronous request from another
// thread: WinHttpSendRequest, WinHttpReceiveResponse or WinHttpReadData then
// fail at once.
class RequestAbortScope final {
public:
  RequestAbortScope(const std::shared_ptr<HttpCancelToken> &token, WinHttpHandle &request)
      : token_(token), request_(request) {
    if (token_ == nullptr) return;
    const HINTERNET handle = request_;
    bool *closed = &isClosed_;
    if (token_->set_abort([handle, closed] {
          WinHttpCloseHandle(handle);
          *closed = true;
        }) == false) {
      WinHttpCloseHandle(handle);
      isClosed_ = true;
    }
  }
  ~RequestAbortScope() {
    if (token_ != nullptr) token_->clear_abort();
    if (isClosed_ == true) request_.release();
  }
  RequestAbortScope(const RequestAbortScope &) = delete;
  RequestAbortScope &operator=(const RequestAbortScope &) = delete;

private:
  std::shared_ptr<HttpCancelToken> token_;
  WinHttpHandle &request_;
  bool isClosed_ = false;
};

} // namespace

// Where a body goes as it arrives, instead of into the answer.
using BodySink = std::function<void(const char *data, std::size_t size)>;

HttpResponse perform(const std::string &url, const std::vector<HttpHeader> &headers,
                    const wchar_t *method, const std::string *body, int timeout_seconds,
                    const BodySink *sink = nullptr, int stall_seconds = 0) {
  // A cancelled token stops a request before it is sent; a Stop while it runs
  // closes its request handle (RequestAbortScope), which ends the blocked
  // WinHTTP call at once.
  const std::shared_ptr<HttpCancelToken> cancel_token = current_http_cancel_token();
  if (cancel_token != nullptr && cancel_token->is_cancelled()) {
    throw HttpTransportError("The request was stopped before it was sent.", false, true);
  }
  const auto wide_url = utf8_to_wide(url);

  wchar_t host_buffer[256]{};
  wchar_t path_buffer[2048]{};
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.lpszHostName = host_buffer;
  components.dwHostNameLength = static_cast<DWORD>(std::size(host_buffer));
  components.lpszUrlPath = path_buffer;
  components.dwUrlPathLength = static_cast<DWORD>(std::size(path_buffer));
  components.dwSchemeLength = static_cast<DWORD>(-1);

  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
    throw std::runtime_error("Invalid URL: " + url);
  }
  const bool use_https = components.nScheme == INTERNET_SCHEME_HTTPS;

  WinHttpHandle session(WinHttpOpen(L"Singularity", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.valid()) {
    throw std::runtime_error("Unable to start an HTTP session");
  }

  const DWORD timeout_ms = static_cast<DWORD>(timeout_seconds) * 1000;
  // A long transfer may take its whole time, but a wait for the next bytes
  // never longer than `stall_seconds`: a stalled download fails, not hangs.
  const DWORD receive_ms = stall_seconds > 0 ? static_cast<DWORD>(stall_seconds) * 1000 : timeout_ms;
  WinHttpSetTimeouts(session, static_cast<int>(timeout_ms), static_cast<int>(timeout_ms),
                     static_cast<int>(timeout_ms), static_cast<int>(receive_ms));

  WinHttpHandle connection(
      WinHttpConnect(session, components.lpszHostName, components.nPort, 0));
  if (!connection.valid()) {
    throw std::runtime_error("Unable to connect to host");
  }

  WinHttpHandle request(WinHttpOpenRequest(
      connection, method, components.lpszUrlPath, nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, use_https ? WINHTTP_FLAG_SECURE : 0));
  if (!request.valid()) {
    throw std::runtime_error("Unable to create the HTTP request");
  }

  std::wstring header_block;
  for (const auto &header : headers) {
    header_block += utf8_to_wide(header.name) + L": " + utf8_to_wide(header.value) + L"\r\n";
  }

  // Declared after `request`, so the abort is cleared before the handle is
  // closed by its owner.
  const RequestAbortScope abort_scope(cancel_token, request);

  const void *request_data = WINHTTP_NO_REQUEST_DATA;
  DWORD request_data_length = 0;
  if (body != nullptr) {
    request_data = body->data();
    request_data_length = static_cast<DWORD>(body->size());
  }

  BOOL sent = WinHttpSendRequest(
      request, header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
      static_cast<DWORD>(-1L), const_cast<void *>(request_data), request_data_length,
      request_data_length, 0);
  if (sent) {
    sent = WinHttpReceiveResponse(request, nullptr);
  }
  if (cancel_token != nullptr && cancel_token->is_cancelled()) {
    throw HttpTransportError("The request was stopped.", false, true);
  }
  if (!sent) {
    // ERROR_WINHTTP_TIMEOUT is the locale-independent timeout fact; the
    // message text itself is only ever diagnostic (see HttpTransportError).
    const DWORD failure = GetLastError();
    throw HttpTransportError("HTTP request failed (error " +
                                 std::to_string(failure) + ")",
                             failure == ERROR_WINHTTP_TIMEOUT);
  }

  DWORD status_code = 0;
  DWORD status_code_size = sizeof(status_code);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                     WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_code_size,
                     WINHTTP_NO_HEADER_INDEX);

  // Retry-After is the one response header kept (HttpResponse::retry_after).
  std::string retry_after;
  wchar_t retry_after_buffer[128]{};
  DWORD retry_after_size = sizeof(retry_after_buffer);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, retry_after_buffer,
                          &retry_after_size, WINHTTP_NO_HEADER_INDEX)) {
    const std::size_t characters = retry_after_size / sizeof(wchar_t);
    for (std::size_t index = 0; index < characters && retry_after_buffer[index] != L'\0'; ++index)
      if (retry_after_buffer[index] < 0x80) retry_after.push_back(static_cast<char>(retry_after_buffer[index]));
  }

  // The body, to its end. A read that fails before the end is a failed
  // transfer, never a shorter answer: a body cut off half-way must not pass
  // for the whole of it (a model package cut off read as "damaged").
  const auto stopped = [&cancel_token]() {
    const DWORD failure = GetLastError();
    if (cancel_token != nullptr && cancel_token->is_cancelled()) {
      throw HttpTransportError("The request was stopped.", false, true);
    }
    throw HttpTransportError("The answer stopped before its end (error " + std::to_string(failure) + ")",
                             failure == ERROR_WINHTTP_TIMEOUT);
  };
  std::string response_body;
  std::uint64_t received = 0;
  std::vector<char> buffer;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) stopped();
    if (available == 0) break;
    buffer.resize(available);
    DWORD read = 0;
    if (!WinHttpReadData(request, buffer.data(), available, &read)) stopped();
    if (read == 0) break;
    if (sink != nullptr && status_code >= 200 && status_code < 300) (*sink)(buffer.data(), read);
    else response_body.append(buffer.data(), read);
    received += read;
  }
  // A length the server announced is the length that must arrive.
  wchar_t length_buffer[32]{};
  DWORD length_size = sizeof(length_buffer);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_buffer,
                          &length_size, WINHTTP_NO_HEADER_INDEX)) {
    const std::uint64_t announced = std::wcstoull(length_buffer, nullptr, 10);
    if (announced != received) {
      throw HttpTransportError("The answer ended after " + std::to_string(received) + " of " +
                                   std::to_string(announced) + " bytes", false);
    }
  }

  // A Stop while the body was read closed the request: what arrived is
  // incomplete and is never handed on as the answer.
  if (cancel_token != nullptr && cancel_token->is_cancelled()) {
    throw HttpTransportError("The request was stopped.", false, true);
  }

  HttpResponse result;
  result.status = static_cast<int>(status_code);
  result.body = std::move(response_body);
  result.retry_after = std::move(retry_after);
  return result;
}

HttpResponse https_get(const std::string &url, const std::vector<HttpHeader> &headers,
                       int timeout_seconds) {
  return perform(url, headers, L"GET", nullptr, timeout_seconds);
}

HttpResponse https_post(const std::string &url, const std::vector<HttpHeader> &headers,
                        const std::string &body, int timeout_seconds) {
  return perform(url, headers, L"POST", &body, timeout_seconds);
}

// The body arrives whole here: `receive` is told it once.
HttpResponse https_post_streaming(const std::string &url, const std::vector<HttpHeader> &headers,
                                  const std::string &body, int timeout_seconds, const HttpBodyReceiver &receive) {
  HttpResponse response = perform(url, headers, L"POST", &body, timeout_seconds);
  if (receive && response.status >= 200 && response.status < 300 && !response.body.empty()) receive(response.body);
  return response;
}

int https_download_file(const std::string &url,
                        const std::vector<HttpHeader> &headers,
                        const std::filesystem::path &destination,
                        int timeout_seconds,
                        const DownloadProgress &progress) {
  if (url.rfind("https://", 0) != 0) {
    throw std::runtime_error("Model downloads require a valid HTTPS URL");
  }
  return download_to_file(url, headers, destination, timeout_seconds, progress, kDownloadStallSeconds);
}

int download_to_file(const std::string &url, const std::vector<HttpHeader> &headers,
                     const std::filesystem::path &destination, int timeout_seconds,
                     const DownloadProgress &progress, int stall_seconds) {
  // Written to the file as it arrives, never held whole in memory, and its
  // progress reported as it goes. A refused or broken answer leaves no file.
  std::ofstream file(destination, std::ios::binary | std::ios::trunc);
  if (!file) throw std::runtime_error("Unable to save model package");
  std::uint64_t received = 0;
  const BodySink sink = [&](const char *data, std::size_t size) {
    file.write(data, static_cast<std::streamsize>(size));
    if (!file) throw std::runtime_error("Unable to save model package");
    received += size;
    if (progress) progress(received);
  };
  HttpResponse response;
  try {
    response = perform(url, headers, L"GET", nullptr, timeout_seconds, &sink, stall_seconds);
    file.close();
  } catch (...) {
    file.close();
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
    throw;
  }
  if (response.status < 200 || response.status >= 300) {
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
  }
  return response.status;
}

} // namespace singularity::platform
