#include "Platform/HttpClient/HttpClient.hpp"

#ifdef SINGULARITY_HAVE_CURL
#include <curl/curl.h>
#endif

#include <cctype>
#include <exception>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace singularity::platform {

#ifdef SINGULARITY_HAVE_CURL
namespace {

// The body as it arrives: kept whole, and told piece by piece to a receiver
// while the response is a 2xx one. A receiver that throws ends the transfer;
// what it threw is rethrown once curl returns.
struct BodyWatch {
  CURL *curl = nullptr;
  std::string body;
  const HttpBodyReceiver *receive = nullptr;
  std::exception_ptr failure;
};

size_t write_callback(char *pointer, size_t size, size_t member_count, void *user_data) {
  auto *watch = static_cast<BodyWatch *>(user_data);
  const size_t length = size * member_count;
  watch->body.append(pointer, length);
  if (watch->receive != nullptr && *watch->receive) {
    long status = 0;
    curl_easy_getinfo(watch->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 200 && status < 300) {
      try {
        (*watch->receive)(std::string_view(pointer, length));
      } catch (...) {
        watch->failure = std::current_exception();
        return 0;
      }
    }
  }
  return length;
}

// Handed each response header line by curl. Only Retry-After is kept; a new
// status line (a redirect followed) starts the headers of the next response.
size_t header_callback(char *pointer, size_t size, size_t member_count, void *user_data) {
  auto *retry_after = static_cast<std::string *>(user_data);
  const std::string line(pointer, size * member_count);
  if (line.rfind("HTTP/", 0) == 0) retry_after->clear();
  const std::string name = "retry-after:";
  if (line.size() > name.size()) {
    bool isRetryAfter = true;
    for (std::size_t index = 0; index < name.size() && isRetryAfter == true; ++index)
      isRetryAfter = std::tolower(static_cast<unsigned char>(line[index])) == name[index];
    if (isRetryAfter == true) {
      std::size_t begin = name.size();
      std::size_t end = line.size();
      while (begin < end && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
      while (end > begin && (line[end - 1] == '\r' || line[end - 1] == '\n' || line[end - 1] == ' ')) --end;
      *retry_after = line.substr(begin, end - begin);
    }
  }
  return size * member_count;
}

// Asked by curl while the transfer runs: a cancelled token ends it at once
// (CURLE_ABORTED_BY_CALLBACK).
int cancel_callback(void *user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto *token = static_cast<const HttpCancelToken *>(user_data);
  return token != nullptr && token->is_cancelled() ? 1 : 0;
}

// Asked by curl while a download runs: reports the bytes received so far and
// ends the transfer at once when its token is cancelled.
struct DownloadWatch {
  const HttpCancelToken *token;
  const DownloadProgress *progress;
};

int download_watch_callback(void *user_data, curl_off_t, curl_off_t received, curl_off_t, curl_off_t) {
  const auto *watch = static_cast<const DownloadWatch *>(user_data);
  if (watch->token != nullptr && watch->token->is_cancelled()) return 1;
  if (watch->progress != nullptr && *watch->progress && received > 0) {
    (*watch->progress)(static_cast<std::uint64_t>(received));
  }
  return 0;
}

} // namespace

HttpResponse perform(const std::string &url, const std::vector<HttpHeader> &headers,
                    const char *method, const std::string *body, int timeout_seconds,
                    const HttpBodyReceiver *receive = nullptr) {
  const std::shared_ptr<HttpCancelToken> cancel_token = current_http_cancel_token();
  if (cancel_token != nullptr && cancel_token->is_cancelled()) {
    throw HttpTransportError("The request was stopped before it was sent.", false, true);
  }
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    throw std::runtime_error("Unable to initialize the HTTP client");
  }

  struct curl_slist *header_list = nullptr;
  for (const auto &header : headers) {
    header_list = curl_slist_append(header_list, (header.name + ": " + header.value).c_str());
  }

  BodyWatch watch;
  watch.curl = curl;
  watch.receive = receive;
  std::string retry_after;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &watch);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &retry_after);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Singularity");
  if (cancel_token != nullptr) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel_token.get());
  }
  if (body != nullptr) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
  }

  const CURLcode outcome = curl_easy_perform(curl);
  if (watch.failure) {
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    std::rethrow_exception(watch.failure);
  }
  if (outcome != CURLE_OK) {
    const std::string message = curl_easy_strerror(outcome);
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    // curl_easy_strerror() is the human-facing text; CURLE_OPERATION_TIMEDOUT
    // is the locale-independent fact callers may branch on.
    if (outcome == CURLE_ABORTED_BY_CALLBACK) throw HttpTransportError("The request was stopped.", false, true);
    throw HttpTransportError(message, outcome == CURLE_OPERATION_TIMEDOUT);
  }

  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

  curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);

  HttpResponse response;
  response.status = static_cast<int>(status_code);
  response.body = std::move(watch.body);
  response.retry_after = std::move(retry_after);
  return response;
}

HttpResponse https_get(const std::string &url, const std::vector<HttpHeader> &headers,
                       int timeout_seconds) {
  return perform(url, headers, "GET", nullptr, timeout_seconds);
}

HttpResponse https_post(const std::string &url, const std::vector<HttpHeader> &headers,
                        const std::string &body, int timeout_seconds) {
  return perform(url, headers, "POST", &body, timeout_seconds);
}

HttpResponse https_post_streaming(const std::string &url, const std::vector<HttpHeader> &headers,
                                  const std::string &body, int timeout_seconds, const HttpBodyReceiver &receive) {
  return perform(url, headers, "POST", &body, timeout_seconds, &receive);
}

int https_download_file(const std::string &url,
                        const std::vector<HttpHeader> &headers,
                        const std::filesystem::path &destination,
                        int timeout_seconds,
                        const DownloadProgress &progress) {
  if (url.rfind("https://", 0) != 0) {
    throw std::runtime_error("Model downloads require a valid HTTPS URL");
  }
  const std::shared_ptr<HttpCancelToken> cancel_token = current_http_cancel_token();
  if (cancel_token != nullptr && cancel_token->is_cancelled()) {
    throw HttpTransportError("The download was stopped before it began.", false, true);
  }
  FILE *file = std::fopen(destination.string().c_str(), "wb");
  if (file == nullptr) throw std::runtime_error("Unable to create model download file");
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    std::fclose(file);
    throw std::runtime_error("Unable to initialize the HTTP client");
  }
  struct curl_slist *header_list = nullptr;
  for (const auto &header : headers) {
    header_list = curl_slist_append(header_list, (header.name + ": " + header.value).c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Singularity");
  // A connection that delivers nothing for a minute has stalled.
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
  DownloadWatch watch{cancel_token.get(), &progress};
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_watch_callback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &watch);
  const auto outcome = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  const bool close_failed = std::fclose(file) != 0;
  if (outcome != CURLE_OK || close_failed) {
    std::filesystem::remove(destination);
    if (outcome == CURLE_ABORTED_BY_CALLBACK) throw HttpTransportError("The download was stopped.", false, true);
    if (outcome != CURLE_OK) throw HttpTransportError(curl_easy_strerror(outcome), outcome == CURLE_OPERATION_TIMEDOUT);
    throw std::runtime_error("Unable to finish model download");
  }
  if (status < 200 || status >= 300) std::filesystem::remove(destination);
  return static_cast<int>(status);
}
#else

// No libcurl found at configure time (see code/backend/CMakeLists.txt) - see
// docs/ARCHITECTURE.md for this platform's known gaps. Reporting this
// honestly lets the UI disable "Connect" instead of the request silently
// failing.
HttpResponse https_get(const std::string &, const std::vector<HttpHeader> &, int) {
  throw std::runtime_error(
      "Connecting to model providers requires libcurl, which was not found on this system.");
}

HttpResponse https_post(const std::string &, const std::vector<HttpHeader> &, const std::string &, int) {
  throw std::runtime_error(
      "Connecting to model providers requires libcurl, which was not found on this system.");
}

HttpResponse https_post_streaming(const std::string &, const std::vector<HttpHeader> &, const std::string &, int,
                                  const HttpBodyReceiver &) {
  throw std::runtime_error(
      "Connecting to model providers requires libcurl, which was not found on this system.");
}

int https_download_file(const std::string &, const std::vector<HttpHeader> &,
                        const std::filesystem::path &, int, const DownloadProgress &) {
  throw std::runtime_error("Downloading local models requires libcurl, which was not found on this system.");
}

#endif

} // namespace singularity::platform
