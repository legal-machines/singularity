#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Routing/Observability/OperationEvent.hpp"

#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Provider/Types/GenerationStream.hpp"
#include "Provider/Types/PromptCache.hpp"
#include "Provider/Types/InputPictures.hpp"

#include <ctime>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace singularity::providers {
namespace {

using Json = nlohmann::json;

constexpr std::array<const char *, 3> kKnownProviderIds{"openai", "anthropic", "openai_compatible"};

// A real reasoning generation call - especially one carrying a large
// maximum_output_tokens for a structured multi-operation proposal - can
// legitimately take well past a connectivity-check-scale timeout. 60s was
// observed failing a genuine large structured completion; 300s stays well
// under any UI-facing operation timeout while giving real provider latency
// room to complete.
constexpr int kGenerationTimeoutSeconds = 300;

class HttpsProviderTransport final : public IProviderTransport {
public:
  platform::HttpResponse post(const std::string &url,
                              const std::vector<platform::HttpHeader> &headers,
                              const std::string &body, int timeout_seconds) override {
    return platform::https_post(url, headers, body, timeout_seconds);
  }
  platform::HttpResponse post_streaming(const std::string &url,
                                        const std::vector<platform::HttpHeader> &headers,
                                        const std::string &body, int timeout_seconds,
                                        const platform::HttpBodyReceiver &receive) override {
    return platform::https_post_streaming(url, headers, body, timeout_seconds, receive);
  }
};

// A request's body as text: a byte that is not UTF-8 in it - a letter cut in
// two by something that clipped its words - goes as U+FFFD, never failing the
// whole request (2026-11-05: "The model could not be asked:
// [json.exception.type_error.316] invalid UTF-8 byte at index 2257").
std::string body_text(const Json &body) {
  return body.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// The body as sent, with the answer first among a structured output's fields:
// a model writes them in the schema's order (OpenAI Structured Outputs), and
// the answer is what the lawyer reads while it is written - after every other
// field, alphabetically, it came last of all.
std::string answer_first(const Json &body) {
  if (!body.contains("response_format")) return body_text(body);
  const Json &format = body.at("response_format");
  if (!format.is_object() || !format.contains("json_schema") || !format.at("json_schema").is_object()
      || !format.at("json_schema").contains("schema")) return body_text(body);
  const Json &schema = format.at("json_schema").at("schema");
  if (!schema.is_object() || !schema.contains("properties") || !schema.at("properties").is_object()
      || !schema.at("properties").contains("response")) return body_text(body);
  using Ordered = nlohmann::ordered_json;
  Ordered ordered = Ordered::parse(body_text(body));
  Ordered &properties = ordered["response_format"]["json_schema"]["schema"]["properties"];
  Ordered first = Ordered::object();
  first["response"] = properties["response"];
  for (auto property = properties.begin(); property != properties.end(); ++property)
    if (property.key() != "response") first[property.key()] = property.value();
  properties = std::move(first);
  return ordered.dump();
}

// An OpenAI-compatible answer read as it is written ("stream": true): its
// server-sent events, put back together into the one response object a request
// without "stream" is answered with, so it is read exactly as that one is.
// Each piece of the model's own output is told to `text` as it arrives - never
// once the output has written `withheld` (a provider echoing the credential).
class ChatCompletionStream {
public:
  ChatCompletionStream(std::function<void(std::string_view)> text, std::string withheld)
      : text_(std::move(text)), withheld_(std::move(withheld)) {}

  void receive(std::string_view piece) {
    buffer_.append(piece.data(), piece.size());
    std::size_t start = 0;
    for (std::size_t end = buffer_.find('\n'); end != std::string::npos; end = buffer_.find('\n', start)) {
      std::string_view line(buffer_.data() + start, end - start);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      read_line(line);
      start = end + 1;
    }
    buffer_.erase(0, start);
  }

  // The response the events stand for: the answer, its tool calls, why it
  // stopped and what it cost - or the provider's error, when it sent one.
  [[nodiscard]] Json response() const {
    if (!error_.is_null() && content_.empty() && toolCalls_.empty()) return Json{{"error", error_}};
    Json message{{"role", "assistant"}, {"content", hasContent_ ? Json(content_) : Json(nullptr)}};
    if (!refusal_.empty()) message["refusal"] = refusal_;
    if (!toolCalls_.empty()) {
      Json calls = Json::array();
      for (const auto &[index, call] : toolCalls_) calls.push_back(call);
      message["tool_calls"] = std::move(calls);
    }
    Json response{{"choices", Json::array({Json{{"index", 0}, {"message", std::move(message)},
        {"finish_reason", finish_ ? Json(*finish_) : Json(nullptr)}}})}};
    if (usage_.is_object()) response["usage"] = usage_;
    if (!provider_.empty()) response["provider"] = provider_;
    return response;
  }

  // The status an error the provider sent inside the stream stands for.
  [[nodiscard]] int error_status() const {
    if (error_.is_null() || !content_.empty() || !toolCalls_.empty()) return 0;
    if (error_.is_object() && error_.contains("code") && error_.at("code").is_number_integer()) {
      const int code = error_.at("code").get<int>();
      if (code >= 400 && code <= 599) return code;
    }
    return 502;
  }

private:
  void read_line(std::string_view line) {
    // Comments (": OPENROUTER PROCESSING") keep the connection alive.
    if (line.empty() || line.front() == ':') return;
    if (line.rfind("data:", 0) != 0) return;
    line.remove_prefix(5);
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    if (line == "[DONE]") return;
    const Json chunk = Json::parse(line.begin(), line.end(), nullptr, false);
    if (chunk.is_discarded() || !chunk.is_object()) return;
    if (chunk.contains("error") && !chunk.at("error").is_null()) error_ = chunk.at("error");
    if (chunk.contains("usage") && chunk.at("usage").is_object()) usage_ = chunk.at("usage");
    // The gateway names the provider that served the request (OpenRouter).
    if (chunk.contains("provider") && chunk.at("provider").is_string()) provider_ = chunk.at("provider").get<std::string>();
    if (!chunk.contains("choices") || !chunk.at("choices").is_array()) return;
    for (const Json &choice : chunk.at("choices")) {
      if (!choice.is_object() || choice.value("index", 0) != 0) continue;
      if (choice.contains("finish_reason") && choice.at("finish_reason").is_string())
        finish_ = choice.at("finish_reason").get<std::string>();
      if (!choice.contains("delta") || !choice.at("delta").is_object()) continue;
      const Json &delta = choice.at("delta");
      if (delta.contains("content") && delta.at("content").is_string()) {
        const std::string &written = delta.at("content").get_ref<const std::string &>();
        hasContent_ = true;
        content_ += written;
        if (!withheld_.empty() && content_.find(withheld_, content_.size() > written.size() + withheld_.size()
                ? content_.size() - written.size() - withheld_.size() : 0) != std::string::npos)
          silenced_ = true;
        if (!silenced_ && text_ && !written.empty()) text_(written);
      }
      if (delta.contains("refusal") && delta.at("refusal").is_string())
        refusal_ += delta.at("refusal").get<std::string>();
      if (delta.contains("tool_calls") && delta.at("tool_calls").is_array())
        for (const Json &part : delta.at("tool_calls")) {
          if (!part.is_object()) continue;
          const int index = part.value("index", 0);
          Json &call = toolCalls_[index];
          if (call.is_null()) call = Json{{"id", ""}, {"type", "function"}, {"function", Json{{"name", ""}, {"arguments", ""}}}};
          if (part.contains("id") && part.at("id").is_string()) call["id"] = part.at("id");
          if (part.contains("type") && part.at("type").is_string()) call["type"] = part.at("type");
          if (part.contains("function") && part.at("function").is_object()) {
            const Json &function = part.at("function");
            if (function.contains("name") && function.at("name").is_string())
              call["function"]["name"] = call["function"]["name"].get<std::string>() + function.at("name").get<std::string>();
            if (function.contains("arguments") && function.at("arguments").is_string())
              call["function"]["arguments"] = call["function"]["arguments"].get<std::string>()
                  + function.at("arguments").get<std::string>();
          }
        }
    }
  }

  std::function<void(std::string_view)> text_;
  std::string withheld_;
  std::string buffer_;
  std::string content_;
  bool hasContent_ = false;
  bool silenced_ = false;
  std::string refusal_;
  std::optional<std::string> finish_;
  std::map<int, Json> toolCalls_;
  Json usage_;
  Json error_;
  std::string provider_;
};

std::string network_error_message(const std::string &provider_display_name) {
  return "Could not reach " + provider_display_name + ". Check your network connection and try again.";
}

std::string trim_trailing_slash(const std::string &value) {
  if (!value.empty() && value.back() == '/') {
    return value.substr(0, value.size() - 1);
  }
  return value;
}

// The URL prefix chat-completions/model-list requests are built against:
// the fixed OpenAI API root for "openai", or the caller-supplied endpoint
// for "openai_compatible" (see is_valid_base_url() for what is accepted
// there).
std::string openai_style_root(const std::string &provider_id, const std::optional<std::string> &base_url) {
  if (provider_id == "openai") {
    return "https://api.openai.com/v1";
  }
  return trim_trailing_slash(base_url.value_or(""));
}

// How a connection reached through the generic path is named in what the
// lawyer reads: a service known by its address by its own name ("OpenRouter
// says this account does not have enough credit"), any other as "This
// endpoint" (2026-10-31: "Add credit to your This endpoint account").
std::string generic_service_name(const std::string &root) {
  if (root.find("openrouter.ai") != std::string::npos) return "OpenRouter";
  return "This endpoint";
}

std::optional<std::string> extract_error_message(const Json &payload) {
  if (payload.is_discarded() || !payload.is_object() || !payload.contains("error")) {
    return std::nullopt;
  }
  const auto &error = payload.at("error");
  if (error.is_object() && error.contains("message") && error.at("message").is_string()) {
    std::string message = error.at("message").get<std::string>();
    // A gateway (OpenRouter and similar) often says only "Provider returned
    // error" and puts the upstream model's OWN reason in metadata - which is
    // the only part a person can act on ("this model does not support ...").
    // Relayed verbatim and bounded; never any header, key or request body.
    if (error.contains("metadata") && error.at("metadata").is_object()) {
      const auto &metadata = error.at("metadata");
      std::string detail;
      Json raw = metadata.contains("raw") ? metadata.at("raw") : Json();
      if (raw.is_string()) {
        detail = raw.get<std::string>();
        raw = Json::parse(detail, nullptr, false);
      } else if (raw.is_object()) {
        detail = raw.dump();
      }
      // The upstream body is usually JSON of its own, and what a person reads
      // is the sentence inside it, not the envelope around it:
      // {"error":{"message":"The compiled grammar is too large ..."}}.
      if (raw.is_object()) {
        const Json *inner = &raw;
        if (raw.contains("error") && raw.at("error").is_object()) inner = &raw.at("error");
        if (inner->contains("message") && inner->at("message").is_string()
            && !inner->at("message").get<std::string>().empty())
          detail = inner->at("message").get<std::string>();
      }
      if (!detail.empty()) {
        constexpr std::size_t kMaximumRelayedDetail = 400;
        if (detail.size() > kMaximumRelayedDetail) detail = detail.substr(0, kMaximumRelayedDetail) + "...";
        if (message.find(detail) == std::string::npos) message += " - " + detail;
      }
      if (metadata.contains("provider_name") && metadata.at("provider_name").is_string()) {
        const std::string upstream = metadata.at("provider_name").get<std::string>();
        if (!upstream.empty() && message.find(upstream) == std::string::npos)
          message += " (upstream provider: " + upstream + ")";
      }
    }
    return message;
  }
  if (error.is_string()) {
    return error.get<std::string>();
  }
  return std::nullopt;
}

ModelDescriptor make_model_descriptor(const std::string &id,
                                      const std::optional<std::string> &display_name = std::nullopt) {
  ModelDescriptor descriptor;
  descriptor.id = id;
  descriptor.display_name = display_name;
  descriptor.excluded_reason = classify_model_exclusion(id);
  descriptor.text_capable = !descriptor.excluded_reason.has_value();
  return descriptor;
}

// Real, provider-reported model capability - OpenAI-compatible discovery
// (e.g. OpenRouter) reports a top-level `context_length` integer per model;
// direct OpenAI's real /models response does not report this today, so this
// returns std::nullopt for it exactly as it would for any endpoint that
// omits or malforms the field - never guessed, never derived from the model
// id/name. Rejects zero/negative so a malformed endpoint can never produce
// a false "zero capacity" that would be indistinguishable from unknown.
std::optional<std::uint64_t> discovered_context_window_tokens(const Json &model) {
  if (!model.contains("context_length") || !model.at("context_length").is_number_integer()) {
    return std::nullopt;
  }
  const auto contextLength = model.at("context_length").get<long long>();
  if (contextLength <= 0) return std::nullopt;
  return static_cast<std::uint64_t>(contextLength);
}

// Real, provider-reported request parameters this exact model accepts
// (OpenAI-compatible discovery's `supported_parameters`, e.g. OpenRouter) -
// raw passthrough strings, never interpreted here. Empty when the provider's
// /models response omits or malforms the field - direct OpenAI's real API
// does not report this today, identical fallback shape to
// discovered_context_window_tokens() above.
// When a provider's figures were read, in UTC: the age of a capability snapshot.
std::string utc_now_iso8601() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}

// Real, provider-reported output limit - OpenRouter reports the largest
// answer its top endpoint returns for the model as
// `top_provider.max_completion_tokens`. Unset when absent or malformed.
std::optional<std::uint64_t> discovered_max_output_tokens(const Json &model) {
  if (!model.contains("top_provider") || !model.at("top_provider").is_object()) return std::nullopt;
  const Json &provider = model.at("top_provider");
  if (!provider.contains("max_completion_tokens") || !provider.at("max_completion_tokens").is_number_integer()) {
    return std::nullopt;
  }
  const auto tokens = provider.at("max_completion_tokens").get<long long>();
  if (tokens <= 0) return std::nullopt;
  return static_cast<std::uint64_t>(tokens);
}

// Real, provider-reported prices - OpenRouter reports `pricing.prompt` and
// `pricing.completion` as decimal strings in dollars a token. Unset when
// absent, malformed or negative.
std::optional<double> discovered_price_per_token(const Json &model, const char *kind) {
  if (!model.contains("pricing") || !model.at("pricing").is_object()) return std::nullopt;
  const Json &pricing = model.at("pricing");
  if (!pricing.contains(kind)) return std::nullopt;
  try {
    const double price = pricing.at(kind).is_string() ? std::stod(pricing.at(kind).get<std::string>())
        : pricing.at(kind).is_number() ? pricing.at(kind).get<double>() : -1.0;
    if (price < 0.0) return std::nullopt;
    return price;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

// What the model reads, as OpenRouter reports it in
// `architecture.input_modalities`. Empty where absent or malformed.
std::vector<std::string> discovered_input_modalities(const Json &model) {
  std::vector<std::string> modalities;
  if (!model.contains("architecture") || !model.at("architecture").is_object()) return modalities;
  const Json &architecture = model.at("architecture");
  if (!architecture.contains("input_modalities") || !architecture.at("input_modalities").is_array()) return modalities;
  for (const auto &modality : architecture.at("input_modalities"))
    if (modality.is_string()) modalities.push_back(modality.get<std::string>());
  return modalities;
}

std::vector<std::string> discovered_supported_parameters(const Json &model) {
  std::vector<std::string> parameters;
  if (!model.contains("supported_parameters") || !model.at("supported_parameters").is_array()) {
    return parameters;
  }
  for (const auto &parameter : model.at("supported_parameters")) {
    if (parameter.is_string()) parameters.push_back(parameter.get<std::string>());
  }
  return parameters;
}

// Derives ModelDescriptor::supports_structured_output from the provider's
// own reported `supported_parameters` for this exact model - true only when
// the provider itself listed "response_format" or "structured_outputs".
// Computed once, here, at discovery time, so no caller downstream
// (ResearchToolLoop, InferenceExecutor) ever needs to inspect the raw
// parameter strings itself. Never true for a model the provider reported
// nothing for - no vendor/model name is ever consulted.
bool derive_supports_structured_output(const std::vector<std::string> &supportedParameters) {
  for (const std::string &parameter : {"response_format", "structured_outputs"})
    if (std::find(supportedParameters.begin(), supportedParameters.end(), parameter)
        != supportedParameters.end())
      return true;
  return false;
}

// Derives ModelDescriptor::supports_native_tools/supports_tool_choice
// independently of derive_supports_structured_output() above and of each
// other - each is true only when the provider itself listed that exact
// single parameter ("tools" / "tool_choice") for this exact model. Never
// conflated with "response_format"/"structured_outputs": a model can accept
// native tool calls without ever supporting a structured-output final
// contract, and vice versa.
bool derive_supports_native_tools(const std::vector<std::string> &supportedParameters) {
  return std::find(supportedParameters.begin(), supportedParameters.end(), "tools")
      != supportedParameters.end();
}

bool derive_supports_tool_choice(const std::vector<std::string> &supportedParameters) {
  return std::find(supportedParameters.begin(), supportedParameters.end(), "tool_choice")
      != supportedParameters.end();
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return value;
}

// The provider's own machine-readable reason, error.type or error.code, when
// it gave one as a word.
bool has_error_kind(const Json &payload, const std::string &kind) {
  if (payload.is_discarded() || !payload.is_object() || !payload.contains("error")) return false;
  const Json &error = payload.at("error");
  if (!error.is_object()) return false;
  for (const char *field : {"type", "code"})
    if (error.contains(field) && error.at(field).is_string() && lowercase(error.at(field).get<std::string>()) == kind)
      return true;
  return false;
}

// The account behind the key has no credit left for this request, a billing
// matter that waiting never fixes. Each provider says it its own way:
// OpenRouter answers 402 ("This request requires more credits...") and, on
// some routes, another status with the same words ("...exceeds your available
// credits"); OpenAI answers 429 with type and code "insufficient_quota" ("You
// exceeded your current quota, please check your plan and billing details");
// Anthropic answers 400 ("Your credit balance is too low to access the
// Anthropic API"). A gateway relaying one of these upstream carries the same
// words in its message. Nothing retries it, and the lawyer is told what to do.
// A per-minute quota ("Quota exceeded for metric ... per minute") is a rate
// limit, not an empty account, and is left to the retry.
bool is_out_of_credit(int status, const Json &payload) {
  if (status == 402) return true;
  if (has_error_kind(payload, "insufficient_quota")) return true;
  const std::optional<std::string> message = extract_error_message(payload);
  if (!message) return false;
  const std::string text = lowercase(*message);
  if (text.find("insufficient_quota") != std::string::npos
      || text.find("exceeded your current quota") != std::string::npos)
    return true;
  if (text.find("credit balance") != std::string::npos && text.find("too low") != std::string::npos) return true;
  return text.find("credit") != std::string::npos
      && (text.find("exceed") != std::string::npos || text.find("insufficient") != std::string::npos
          || text.find("more credits") != std::string::npos || text.find("afford") != std::string::npos);
}

std::string out_of_credit_message(const std::string &provider_display_name) {
  // A connection the app knows by no name is "the account", never "your This
  // endpoint account".
  const bool isNamed = provider_display_name != "This endpoint";
  return (isNamed == true ? provider_display_name : std::string("This service")) + kOutOfCreditWords
      + " Add credit to " + (isNamed == true ? "your " + provider_display_name + " account" : std::string("the account"))
      + ", or choose another model, then send the request again.";
}

std::chrono::milliseconds &retry_base_delay() {
  static std::chrono::milliseconds delay(1500);
  return delay;
}

// The longest a provider may ask Singularity to wait before its one retry. Past it
// the request ends at once and the lawyer is told when to try again: a person
// is waiting for the answer, with Stop in front of them.
constexpr long long kLongestRetryAfterSeconds = 20;

// The wait before the one retry when the provider named none: a little longer
// or shorter each time, so many requests turned away together do not all come
// back at the same moment.
std::chrono::milliseconds jittered_retry_delay() {
  static std::mutex randomMutex;
  static std::mt19937 engine(std::random_device{}());
  long long jitter = 0;
  {
    const std::lock_guard<std::mutex> lock(randomMutex);
    std::uniform_int_distribution<long long> spread(0, retry_base_delay().count());
    jitter = spread(engine);
  }
  return retry_base_delay() + std::chrono::milliseconds(jitter);
}

// Waits before the one retry. A Stop during the wait ends it at once, and then
// nothing is sent again: false.
bool wait_before_retry(std::chrono::milliseconds delay) {
  const auto until = std::chrono::steady_clock::now() + delay;
  const std::shared_ptr<platform::HttpCancelToken> token = platform::current_http_cancel_token();
  for (;;) {
    if (token != nullptr && token->is_cancelled()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now >= until) return true;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - now);
    std::this_thread::sleep_for(std::min(left, std::chrono::milliseconds(50)));
  }
}

// When to try again, in words a person reads: seconds, minutes, or later.
std::string try_again_in(long long seconds) {
  if (seconds == 1) return "Try again in 1 second.";
  if (seconds < 120) return "Try again in " + std::to_string(seconds) + " seconds.";
  if (seconds < 2 * 3600) return "Try again in about " + std::to_string((seconds + 59) / 60) + " minutes.";
  return "Try again later.";
}

// What to do when the provider still turns the request away after its one
// retry: the wait it asked for, when it named one, else `fallback`.
std::string wait_advice(const platform::HttpResponse &response, const std::string &fallback) {
  const std::optional<long long> asked = retry_after_seconds(response.retry_after, std::chrono::system_clock::now());
  if (asked && *asked > 0) return try_again_in(*asked);
  return fallback;
}

// Sends one generation request. A provider that is briefly overloaded (429),
// failing on its side (5xx) or too slow to answer (a timeout) is asked once
// more: nothing of the first answer was accepted, so the second attempt is the
// same request and the lawyer is spared a whole re-run for one passing error.
// The wait before it is the one the provider asked for (Retry-After), when it
// asked for one no longer than kLongestRetryAfterSeconds; a longer one ends the
// request at once with when to try again. A second failure, a request stopped
// by the lawyer - before or while it waits - and an account out of credit are
// never retried.
platform::HttpResponse post_generation(IProviderTransport &transport, const std::string &url,
                                       const std::vector<platform::HttpHeader> &headers, const std::string &body,
                                       const std::string &provider_display_name,
                                       const std::function<platform::HttpBodyReceiver()> &streamed = {}) {
  const std::string stopped = "The request was stopped before " + provider_display_name + " answered.";
  for (int attempt = 1;; ++attempt) {
    const bool isLastAttempt = attempt >= 2;
    platform::HttpResponse response;
    try {
      // A streamed request reads each attempt afresh.
      response = streamed ? transport.post_streaming(url, headers, body, kGenerationTimeoutSeconds, streamed())
          : transport.post(url, headers, body, kGenerationTimeoutSeconds);
    } catch (const platform::HttpTransportError &error) {
      if (error.cancelled) throw TaskError(TaskErrorCode::NetworkError, stopped);
      if (error.timed_out && isLastAttempt == false) {
        if (wait_before_retry(jittered_retry_delay()) == false) throw TaskError(TaskErrorCode::NetworkError, stopped);
        continue;
      }
      throw TaskError(TaskErrorCode::NetworkError, network_error_message(provider_display_name));
    } catch (const std::exception &) {
      throw TaskError(TaskErrorCode::NetworkError, network_error_message(provider_display_name));
    }
    const bool isPassingFailure = response.status == 429 || response.status >= 500;
    if (isPassingFailure == false || isLastAttempt == true) return response;
    if (is_out_of_credit(response.status, Json::parse(response.body, nullptr, false))) return response;
    const std::shared_ptr<platform::HttpCancelToken> token = platform::current_http_cancel_token();
    if (token != nullptr && token->is_cancelled()) throw TaskError(TaskErrorCode::NetworkError, stopped);
    std::chrono::milliseconds delay = jittered_retry_delay();
    const std::optional<long long> asked = retry_after_seconds(response.retry_after, std::chrono::system_clock::now());
    if (asked) {
      if (*asked > kLongestRetryAfterSeconds) {
        if (response.status == 429)
          throw TaskError(TaskErrorCode::ProviderRateLimit, provider_display_name + " asked Singularity to wait before "
              "sending this request again. " + try_again_in(*asked));
        throw TaskError(TaskErrorCode::NetworkError, provider_display_name + " is temporarily unavailable and asked "
            "Singularity to wait. " + try_again_in(*asked));
      }
      delay = std::chrono::seconds(*asked);
    }
    if (wait_before_retry(delay) == false) throw TaskError(TaskErrorCode::NetworkError, stopped);
  }
}

// The provider's answer is an account out of credit: said as what to do.
void refuse_out_of_credit(const platform::HttpResponse &response, const Json &payload,
                          const std::string &provider_display_name) {
  if (response.status >= 200 && response.status < 300) return;
  if (is_out_of_credit(response.status, payload))
    throw TaskError(TaskErrorCode::ProviderRejectedRequest, out_of_credit_message(provider_display_name));
}

} // namespace

// Pure parsing of an already-decoded OpenAI-compatible /models response body
// (no network) - exposed so model-discovery parsing, including real
// provider-reported context_length capture (see
// discovered_context_window_tokens() above), is directly unit-testable
// against a captured fixture without any transport injection. Returns
// std::nullopt for a malformed/unexpected shape, exactly the condition
// test_openai_style() itself already treated as an error.
std::optional<std::vector<ModelDescriptor>> parse_openai_style_models_response(const nlohmann::json &payload) {
  if (payload.is_discarded() || !payload.contains("data") || !payload.at("data").is_array()) {
    return std::nullopt;
  }
  std::vector<ModelDescriptor> models;
  const std::string discoveredAt = utc_now_iso8601();
  // Every model the provider reported, in the order it reported them - no
  // truncation. A misbehaving/malicious endpoint returning an unbounded
  // payload is still bounded by ordinary HTTP response limits upstream;
  // this loop does not add its own arbitrary cutoff.
  for (const auto &model : payload.at("data")) {
    if (model.contains("id") && model.at("id").is_string()) {
      ModelDescriptor descriptor = make_model_descriptor(model.at("id").get<std::string>());
      descriptor.context_window_tokens = discovered_context_window_tokens(model);
      descriptor.max_output_tokens = discovered_max_output_tokens(model);
      descriptor.input_price_per_token = discovered_price_per_token(model, "prompt");
      descriptor.output_price_per_token = discovered_price_per_token(model, "completion");
      descriptor.discovered_at = discoveredAt;
      descriptor.supported_parameters = discovered_supported_parameters(model);
      descriptor.supports_structured_output = derive_supports_structured_output(descriptor.supported_parameters);
      descriptor.supports_native_tools = derive_supports_native_tools(descriptor.supported_parameters);
      descriptor.supports_tool_choice = derive_supports_tool_choice(descriptor.supported_parameters);
      descriptor.input_modalities = discovered_input_modalities(model);
      models.push_back(std::move(descriptor));
    }
  }
  std::sort(models.begin(), models.end(),
            [](const ModelDescriptor &a, const ModelDescriptor &b) { return a.id < b.id; });
  return models;
}

// Generic, semantics-free JSON Schema shape normalization for the native
// tool-calling wire boundary - see the header's own comment for the full
// contract. Exposed (not anonymous-namespaced) so this shape logic is
// directly unit-testable, matching parse_openai_style_models_response()'s
// own pattern above.
std::optional<Json> lower_tool_schema_for_native_calling(const Json &schema) {
  if (schema.is_object() && schema.size() == 1 && schema.contains("oneOf") && schema.at("oneOf").is_array()) {
    const Json &alternatives = schema.at("oneOf");
    if (alternatives.size() == 1) return alternatives.front();
    return std::nullopt;
  }
  if (schema.is_object() && schema.size() == 1 && schema.contains("anyOf") && schema.at("anyOf").is_array()) {
    const Json &alternatives = schema.at("anyOf");
    if (alternatives.size() == 1) return alternatives.front();
    return std::nullopt;
  }
  return schema;
}

namespace {

ProviderTestResult test_openai_style(const std::string &provider_display_name, const std::string &root,
                                     const std::string &api_key) {
  ProviderTestResult result;
  try {
    const auto response =
        platform::https_get(root + "/models", {{"Authorization", "Bearer " + api_key}});

    if (response.status == 401 || response.status == 403) {
      result.error = "Could not verify this API key. Check the key and try again.";
      return result;
    }
    if (response.status < 200 || response.status >= 300) {
      const auto payload = Json::parse(response.body, nullptr, false);
      const auto message = extract_error_message(payload);
      result.error = message ? (provider_display_name + " rejected the request: " + *message)
                             : (provider_display_name + " could not be reached (error " +
                                   std::to_string(response.status) + ").");
      return result;
    }

    const auto payload = Json::parse(response.body, nullptr, false);
    const auto models = parse_openai_style_models_response(payload);
    if (!models) {
      result.error = provider_display_name + " returned an unexpected response.";
      return result;
    }
    // OpenRouter lists its models to anyone, so a listing proves nothing about
    // the key: its own key endpoint does, and refuses a wrong key with 401.
    if (root.rfind("https://openrouter.ai/", 0) == 0) {
      const auto key = platform::https_get(root + "/key", {{"Authorization", "Bearer " + api_key}});
      if (key.status == 401 || key.status == 403) {
        result.error = "Could not verify this API key. Check the key and try again.";
        return result;
      }
      if (key.status < 200 || key.status >= 300) {
        result.error = provider_display_name + " could not be reached (error " + std::to_string(key.status) + ").";
        return result;
      }
    }
    result.models = *models;
    result.ok = true;
  } catch (const std::exception &) {
    result.error = network_error_message(provider_display_name);
  }
  return result;
}

ProviderTestResult test_anthropic(const std::string &api_key) {
  ProviderTestResult result;
  try {
    const auto response =
        platform::https_get("https://api.anthropic.com/v1/models",
                            {{"x-api-key", api_key}, {"anthropic-version", "2023-06-01"}});

    if (response.status == 401) {
      result.error = "Could not verify this API key. Check the key and try again.";
      return result;
    }
    if (response.status < 200 || response.status >= 300) {
      result.error = "Anthropic could not be reached (error " + std::to_string(response.status) + ").";
      return result;
    }

    const auto payload = Json::parse(response.body, nullptr, false);
    if (payload.is_discarded() || !payload.contains("data") || !payload.at("data").is_array()) {
      result.error = "Anthropic returned an unexpected response.";
      return result;
    }

    for (const auto &model : payload.at("data")) {
      std::optional<std::string> display_name;
      if (model.contains("display_name") && model.at("display_name").is_string()) {
        display_name = model.at("display_name").get<std::string>();
      }
      if (model.contains("id") && model.at("id").is_string()) {
        result.models.push_back(make_model_descriptor(model.at("id").get<std::string>(), display_name));
      } else if (display_name) {
        result.models.push_back(make_model_descriptor(*display_name));
      }
    }
    result.ok = true;
  } catch (const std::exception &) {
    result.error = network_error_message("Anthropic");
  }
  return result;
}

std::optional<long long> json_int(const Json &value, const char *key) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number_integer()) {
    return std::nullopt;
  }
  return value.at(key).get<long long>();
}

void read_cost_evidence(const Json &usage, UsageMetadata &result) {
  if (usage.contains("cost") == false || usage.at("cost").is_number() == false) return;

  const double amount = usage.at("cost").get<double>();
  constexpr double maximum_amount =
      static_cast<double>(std::numeric_limits<long long>::max()) / 1'000'000.0;
  if (std::isfinite(amount) == false || amount < 0.0 || amount > maximum_amount) return;

  // Real product bug (caught live): OpenRouter (and every provider sharing
  // its OpenAI-compatible wire dialect that reports cost the same way)
  // returns a bare numeric usage.cost - always USD - in every real chat
  // completion response, with no cost_currency/cost_classification/
  // cost_methodology fields at all (OpenRouter's "Usage Accounting"
  // feature; usage.include is a deprecated no-op - cost is always present
  // now). The strict four-field shape below was never satisfiable by any
  // real provider response, so real Usage/AI cost was always empty despite
  // real spend. A provider that DOES supply its own cost_currency/
  // cost_classification/cost_methodology (the original, still-supported
  // shape - kept for whichever caller/double relies on it) is trusted
  // exactly as before and takes precedence over this fallback.
  std::string currency = "USD";
  std::string classification = "actual";
  std::string methodology = "provider_reported";

  const bool has_explicit_currency = usage.contains("cost_currency") && usage.at("cost_currency").is_string();
  const bool has_explicit_classification =
      usage.contains("cost_classification") && usage.at("cost_classification").is_string();
  const bool has_explicit_methodology = usage.contains("cost_methodology") && usage.at("cost_methodology").is_string();

  if (has_explicit_currency || has_explicit_classification || has_explicit_methodology) {
    // The original strict provider-reported shape: every one of the three
    // fields must be present and individually valid, or nothing is
    // recorded - never a silent partial mix with the USD/actual fallback
    // above.
    if (!has_explicit_currency || !has_explicit_classification || !has_explicit_methodology) return;
    classification = usage.at("cost_classification").get<std::string>();
    if (classification != "actual" && classification != "estimated") return;
    currency = usage.at("cost_currency").get<std::string>();
    methodology = usage.at("cost_methodology").get<std::string>();
    const bool currency_is_valid =
        currency.size() == 3 &&
        std::all_of(currency.begin(), currency.end(),
                    [](const char value) { return value >= 'A' && value <= 'Z'; });
    if (currency_is_valid == false || methodology.empty() == true) return;
  }

  result.cost_micros = static_cast<long long>(std::llround(amount * 1'000'000.0));
  result.cost_currency = currency;
  result.cost_classification = classification;
  result.cost_methodology = methodology;
}

// The prompt's parts as text blocks a provider's cache can end at, with the
// cache's breakpoints on the blocks the hint names (PromptCache.hpp). Only for
// a prompt whose data is given in parts (an array at dataPlane.payload.prompt);
// each block ends where a part's text ends, the first carrying what comes
// before the parts, and what comes after them - the protection legend, which
// grows as a talk looks things up - is a block of its own that no breakpoint
// names: the next call, its parts only grown, reads all of this one's from the
// cache (a talk's later rounds read none of them while the legend closed the
// last part's block, 2026-10-26). The blocks join to exactly `prompt`. Null
// when the prompt is not in parts or a part cannot be found as written: the
// prompt then goes as it is.
std::optional<Json> cached_prompt_blocks(const std::string &prompt, const PromptCache &cache) {
  const Json envelope = Json::parse(prompt, nullptr, false);
  if (envelope.is_discarded() || !envelope.is_object()) return std::nullopt;
  const Json::json_pointer where("/dataPlane/payload/prompt");
  if (!envelope.contains(where) || !envelope.at(where).is_array() || envelope.at(where).size() < 2) return std::nullopt;
  const Json &parts = envelope.at(where);
  std::size_t at = prompt.find("\"dataPlane\":");
  if (at == std::string::npos) return std::nullopt;
  std::vector<std::size_t> ends;
  for (const Json &part : parts) {
    if (!part.is_string()) return std::nullopt;
    const std::string written = part.dump();
    const std::size_t found = prompt.find(written, at);
    if (found == std::string::npos) return std::nullopt;
    at = found + written.size();
    ends.push_back(at);
  }
  if (ends.back() < prompt.size()) ends.push_back(prompt.size());
  Json blocks = Json::array();
  std::size_t begin = 0;
  std::size_t marked = 0;
  for (std::size_t index = 0; index < ends.size(); ++index) {
    Json block{{"type", "text"}, {"text", prompt.substr(begin, ends[index] - begin)}};
    for (const PromptCache::Breakpoint &breakpoint : cache.breakpoints) {
      if (breakpoint.part != index || marked >= 4) continue;
      block["cache_control"] = breakpoint.isHour ? Json{{"type", "ephemeral"}, {"ttl", "1h"}} : Json{{"type", "ephemeral"}};
      ++marked;
      break;
    }
    blocks.push_back(std::move(block));
    begin = ends[index];
  }
  return blocks;
}

GenerationResult generate_openai_style(const std::string &provider_display_name, const std::string &root,
                                       const std::string &api_key, const std::string &model_id,
                                       const std::string &prompt,
                                       const FinalPayloadValidator &final_payload_validator,
									   IProviderTransport &transport,
									   std::optional<std::uint64_t> maximum_output_tokens,
									   const std::optional<GenerationOutputContract> &output_contract,
									   const std::optional<GenerationToolControl> &tool_control,
									   ReasoningEffort reasoning_effort) {
  if (output_contract && tool_control) {
    throw std::invalid_argument(
        "A generation turn may not combine a response_format output contract with native tool control - "
        "tool selection and final structured output are different concepts.");
  }
  Json body{
      {"model", model_id},
      {"messages", Json::array({Json{{"role", "user"}, {"content", prompt}}})},
  };
  if (maximum_output_tokens) body["max_tokens"] = *maximum_output_tokens;
  // How long the model thinks, where the caller asked and the model takes it
  // (accepts_reasoning_effort()). OpenRouter carries it as its own "reasoning"
  // object for every model it routes; OpenAI and an OpenAI-compatible endpoint
  // as "reasoning_effort".
  if (reasoning_effort == ReasoningEffort::Low) {
    if (root.find("openrouter.ai") != std::string::npos) body["reasoning"] = Json{{"effort", "low"}};
    else body["reasoning_effort"] = "low";
  }
  // No thinking at all: OpenRouter turns it off; elsewhere the shortest think
  // the endpoint names.
  if (reasoning_effort == ReasoningEffort::None) {
    if (root.find("openrouter.ai") != std::string::npos) body["reasoning"] = Json{{"enabled", false}};
    else body["reasoning_effort"] = "low";
  }
  // The least thinking, for a model that will not answer without any.
  if (reasoning_effort == ReasoningEffort::Minimal) {
    if (root.find("openrouter.ai") != std::string::npos) body["reasoning"] = Json{{"effort", "minimal"}};
    else body["reasoning_effort"] = "low";
  }
  // Only ever attached when the caller already resolved, from this exact
  // model's own provider-reported ModelDescriptor::supports_structured_output
  // (see InferenceExecutor.cpp's own capability gating), that the model
  // accepts it - never guessed, never sent to a model that never said it
  // supports it. Singularity still fully validates the returned JSON's actual
  // shape/semantics itself (see ResearchToolLoop.hpp) regardless of this;
  // this only raises the odds the provider's own response is syntactically
  // valid JSON already matching the caller's exact protocol shape.
  if (output_contract) {
    body["response_format"] = Json{
        {"type", "json_schema"},
        {"json_schema", Json{
            {"name", output_contract->schema_name},
            {"strict", true},
            {"schema", output_contract->json_schema},
        }},
    };
    // Real, confirmed gap (2026-09-14 structured-output audit): the
    // OpenRouter-only "provider":{"require_parameters":true} routing
    // restriction below was previously wired ONLY inside the tool_control
    // branch, so a pure response_format/json_schema turn (output_contract
    // set, tool_control unset - output_contract and tool_control are already
    // mutually exclusive, enforced above) never got it at all. Without this,
    // OpenRouter may route to an endpoint that does not actually support
    // structured outputs and silently ignore "strict"/"schema" rather than
    // honoring or rejecting it - exactly the failure class this parameter
    // exists to prevent, and exactly the parameter class (never a per-model
    // guess) InferenceExecutor.cpp's own capability gate already establishes
    // is genuinely supported before output_contract is ever set at all.
    if (root.find("openrouter.ai") != std::string::npos) {
      body["provider"]["require_parameters"] = true;
    }
  }
  // The ordinary OpenAI-compatible function-tools shape. `tool_control` is
  // only ever passed once the caller already resolved, from this exact
  // model's own discovered ModelDescriptor::supports_native_tools, that this
  // model accepts a `tools` array - never guessed. Every schema here is the
  // caller's own (ResearchTools.hpp's canonical research tool schemas
  // today), never invented at this layer. supports_native_tools only means
  // this provider accepts a `tools` array at all - it says nothing about
  // whether ITS OWN native function-parameters slot can faithfully represent
  // a given caller schema's shape (e.g. a root-level oneOf/anyOf, as
  // TurnContract's per-item capability scoping produces). Lowering that
  // generic shape question - never what any schema means - to what this
  // wire slot actually supports belongs exactly here, not in the caller.
  if (tool_control) {
    Json toolsJson = Json::array();
    for (const ModelToolDefinition &tool : tool_control->tools) {
      const auto lowered = lower_tool_schema_for_native_calling(tool.parameters_schema);
      if (!lowered) {
        throw std::invalid_argument(
            "Tool \"" + tool.wire_name + "\" has a parameters schema (a root-level oneOf/anyOf with more than "
            "one alternative) that native tool calling cannot faithfully represent as one object schema; "
            "refusing to send a schema whose fidelity to the caller's contract is unknown.");
      }
      toolsJson.push_back(Json{
          {"type", "function"},
          {"function", Json{
              {"name", tool.wire_name},
              {"description", tool.description},
              // OpenAI Structured Outputs / strict function calling: the
              // provider itself constrains generated arguments to this exact
              // schema, rather than treating it as advisory - every canonical
              // Singularity tool schema (post-lowering above) is already authored
              // in the strict-compatible style every caller here already
              // needs for the LOCAL hard-JSON-Schema path too (every property
              // present in "required", optionality expressed as a nullable
              // type, "additionalProperties": false at every level) - never
              // rewritten or weakened here, only declared.
              {"strict", true},
              {"parameters", *lowered},
          }},
      });
    }
    body["tools"] = std::move(toolsJson);
    switch (tool_control->tool_choice) {
      case ToolChoicePolicy::ForceNamed:
        if (tool_control->forced_tool_wire_name) {
          body["tool_choice"] = Json{
              {"type", "function"},
              {"function", Json{{"name", *tool_control->forced_tool_wire_name}}},
          };
        } else {
          body["tool_choice"] = "required";
        }
        break;
      case ToolChoicePolicy::Required:
        body["tool_choice"] = "required";
        break;
      case ToolChoicePolicy::Auto:
        body["tool_choice"] = "auto";
        break;
    }
    // parallel_tool_calls is a best-effort request-side hint only, never a
    // guarantee any given provider honors it (a real provider - Gemini 2.5
    // Flash via OpenRouter - was already observed live returning more than
    // one tool call with this sent as false; see
    // ResearchToolLoop.hpp's repair_feedback_for_category() comment). The
    // real, load-bearing guarantee against more than one tool call per
    // research turn is that same file's independent downstream
    // "multiple_tool_calls_returned" rejection + bounded repair, which
    // applies regardless of whether a provider ever received or honored
    // this hint. OpenRouter's own live model-endpoints metadata (GET
    // /api/v1/models/{author}/{slug}/endpoints, checked 2026-09-07) confirms
    // every current openai/gpt-5.1 endpoint (OpenAI standard/Flex/Fast,
    // Azure standard/EU) declares "tools" and "tool_choice" as
    // supported_parameters - the two parameters require_parameters below
    // actually needs to guarantee - but none declare "parallel_tool_calls".
    // Sending it while provider.require_parameters is true makes OpenRouter
    // hard-reject every one of those otherwise-fully-compatible endpoints
    // with "No endpoints found", confirmed live. Sent only off the
    // OpenRouter path, where it remains a real, OpenAI-documented top-level
    // parameter a direct connection genuinely supports.
    if (root.find("openrouter.ai") == std::string::npos) {
      body["parallel_tool_calls"] = tool_control->parallel_tool_calls;
    }
    // OpenRouter-specific transport hardening only, never a substitute for
    // this layer's own schema-fidelity gate above: restricts routing to an
    // upstream endpoint that actually supports every parameter this exact
    // request sends (here, "tools"/"tool_choice"), rather than OpenRouter
    // silently falling back to one that would ignore them. Scoped to the
    // resolved OpenRouter root specifically - other openai_compatible
    // endpoints (a self-hosted gateway, a different aggregator) do not
    // necessarily recognize this OpenRouter-only "provider" extension.
    if (root.find("openrouter.ai") != std::string::npos) {
      body["provider"]["require_parameters"] = true;
    }
  }
  // A firm's confidential work goes only to endpoints that keep nothing of it
  // and train on none of it (D-116): OpenRouter routes it among the endpoints
  // that do not collect what they are sent, whatever the account allows. Which
  // endpoint served it is kept with the call's usage.
  if (root.find("openrouter.ai") != std::string::npos) {
    body["provider"]["data_collection"] = "deny";
  }
  // A conversation's prompt the provider keeps between its calls
  // (PromptCache.hpp): its calls on one endpoint, and its parts as blocks the
  // cache can end at. The blocks join to exactly the prompt validated below.
  if (root.find("openrouter.ai") != std::string::npos) {
    if (const std::shared_ptr<const PromptCache> cache = current_prompt_cache()) {
      if (!cache->session_id.empty()) body["session_id"] = cache->session_id.substr(0, 256);
      if (std::optional<Json> blocks = cached_prompt_blocks(prompt, *cache)) body["messages"][0]["content"] = std::move(*blocks);
    }
  }

  // Shown while it is written, where someone is showing it and the endpoint is
  // one known to stream: OpenAI, which reports usage at the end only when asked
  // ("include_usage"), and OpenRouter, which always does - and whose routing
  // with "require_parameters" is not asked about a parameter it does not need.
  const std::shared_ptr<GenerationStream> stream = current_generation_stream();
  const bool isOpenRouter = root.find("openrouter.ai") != std::string::npos;
  const bool streamed = stream != nullptr && stream->text && (isOpenRouter || root == "https://api.openai.com/v1");
  if (streamed) {
    body["stream"] = true;
    if (!isOpenRouter) body["stream_options"] = Json{{"include_usage", true}};
  }

  // The member's pictures, after the words, as image inputs (D-256).
  std::vector<std::string> pictureDigests;
  if (const std::shared_ptr<const std::vector<InputPicture>> pictures = current_input_pictures();
      pictures && pictures->empty() == false) {
    Json &content = body["messages"][0]["content"];
    if (content.is_string()) content = Json::array({Json{{"type", "text"}, {"text", content.get<std::string>()}}});
    for (const InputPicture &picture : *pictures) {
      content.push_back(Json{{"type", "image_url"},
          {"image_url", Json{{"url", "data:" + picture.media_type + ";base64," + picture.base64}}}});
      pictureDigests.push_back(picture.sha256);
    }
  }

  if (!final_payload_validator) {
    throw std::invalid_argument("final provider payload validator is required");
  }
  const auto serialized_body = answer_first(body);
  // Keep this call adjacent to transport: no provider content mutation may
  // occur after validation and before the bytes leave this layer.
  final_payload_validator(FinalPayloadView{serialized_body, {prompt}, pictureDigests});

  std::shared_ptr<ChatCompletionStream> events;
  const auto send = [&](const std::string &bytes) {
    return post_generation(transport,
        root + "/chat/completions",
        {{"Authorization", "Bearer " + api_key}, {"Content-Type", "application/json"}},
        bytes, provider_display_name,
        streamed ? std::function<platform::HttpBodyReceiver()>([&]() -> platform::HttpBodyReceiver {
          if (stream->begin) stream->begin();
          events = std::make_shared<ChatCompletionStream>(stream->text, api_key);
          return [events](std::string_view piece) { events->receive(piece); };
        }) : std::function<platform::HttpBodyReceiver()>());
  };
  platform::HttpResponse response = send(serialized_body);
  // A model that takes no forced tool choice (Claude Sonnet 5.5 and Opus 5.5
  // turn "required" and a named tool away) is asked once more with "auto": the
  // tools stay strict, and what comes back is checked by the caller exactly as
  // before, a turn with no tool call included (D-116).
  if (response.status == 400 && tool_control && tool_control->tool_choice != ToolChoicePolicy::Auto) {
    const auto rejected = Json::parse(response.body, nullptr, false);
    const std::optional<std::string> reason = extract_error_message(rejected);
    if (reason && lowercase(*reason).find("tool_choice") != std::string::npos) {
      body["tool_choice"] = "auto";
      const std::string retried = answer_first(body);
      final_payload_validator(FinalPayloadView{retried, {prompt}, pictureDigests});
      events.reset();
      response = send(retried);
    }
  }
  // A streamed answer is read as the response it stands for, once the raw
  // events are known to carry no credential. An endpoint that answered with
  // one whole response after all is read as that response.
  const auto isEventStream = [](const std::string &text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    return first != std::string::npos && text[first] != '{' && text[first] != '[';
  };
  if (streamed && events && response.status >= 200 && response.status < 300 && isEventStream(response.body)) {
    if (!api_key.empty() && response.body.find(api_key) != std::string::npos)
      throw TaskError(TaskErrorCode::ProviderAuthentication, "Provider returned credential material; response withheld.");
    events->receive("\n");
    if (const int status = events->error_status(); status != 0) response.status = status;
    response.body = events->response().dump();
  }

  // Credentials must never enter parsed results, error messages or observers,
  // including when a misbehaving provider echoes its Authorization value.
  if (!api_key.empty() && response.body.find(api_key) != std::string::npos)
    throw TaskError(TaskErrorCode::ProviderAuthentication, "Provider returned credential material; response withheld.");
  namespace obs = singularity::routing::observability;
  if (obs::has_sink()) {
    // Headers are never observed. A provider echoing its credential is withheld.
    const bool isCredentialEcho = !api_key.empty() && response.body.find(api_key) != std::string::npos;
    obs::emit({obs::OperationEventKind::ProviderResponseReceived, "Provider HTTP response received",
        {{"status", std::to_string(response.status)}, {"bodyBytes", std::to_string(response.body.size())},
         {"credentialEchoWithheld", isCredentialEcho ? "true" : "false"}},
        isCredentialEcho ? std::nullopt : std::optional<std::string>(response.body)});
  }
  const auto payload = Json::parse(response.body, nullptr, false);
  refuse_out_of_credit(response, payload, provider_display_name);
  // 401 is the key. 403 is often not: a gateway's security policy, a region, a
  // spending limit - and the provider says which. Telling a lawyer to reconnect
  // a key that works sends them to fix the wrong thing, so a 403 that carries
  // its own reason is reported in that reason's words.
  if (response.status == 403) {
    std::optional<std::string> message = extract_error_message(payload);
    if (!message && payload.is_object() && payload.contains("message") && payload.at("message").is_string())
      message = payload.at("message").get<std::string>();
    if (message && !message->empty())
      throw TaskError(TaskErrorCode::ProviderRejectedRequest,
                      provider_display_name + " refused access to this request: " + *message);
  }
  if (response.status == 401 || response.status == 403) {
    throw TaskError(TaskErrorCode::ProviderAuthentication,
                    "This API key is no longer valid. Reconnect this service in Models.");
  }
  if (response.status == 429) {
    const auto message = extract_error_message(payload);
    const std::string asked = wait_advice(response, std::string());
    throw TaskError(TaskErrorCode::ProviderRateLimit,
                    message ? (provider_display_name + " is rate-limiting requests: " + *message
                                   + (asked.empty() ? std::string() : " " + asked))
                            : (provider_display_name + " is rate-limiting requests. "
                                   + wait_advice(response, "Wait a moment and try again.")));
  }
  if (response.status >= 500) {
    const auto message = extract_error_message(payload);
    const std::string asked = wait_advice(response, std::string());
    throw TaskError(TaskErrorCode::NetworkError,
                    message ? (provider_display_name + " is temporarily unavailable: " + *message
                                   + (asked.empty() ? std::string() : " " + asked))
                            : (provider_display_name + " is temporarily unavailable (status " +
                                   std::to_string(response.status) + "). "
                                   + wait_advice(response, "Try again shortly.")));
  }
  if (response.status < 200 || response.status >= 300) {
    const auto message = extract_error_message(payload);
    // A connected provider refusing THIS request is its own outcome, not an
    // unknown failure: a person can act on it by choosing another model, and
    // nothing about their Project is wrong.
    throw TaskError(TaskErrorCode::ProviderRejectedRequest,
                    message ? (provider_display_name + " rejected the request: " + *message)
                            : (provider_display_name + " could not be reached (error " +
                                   std::to_string(response.status) + ")."));
  }
  if (payload.is_discarded() || !payload.contains("choices") || !payload.at("choices").is_array() ||
      payload.at("choices").empty()) {
    throw TaskError(TaskErrorCode::Unknown, provider_display_name + " returned an unexpected response.");
  }

  const auto &first_choice = payload.at("choices").at(0);
  GenerationResult result;
  // The request as the provider's records name it, and the answer's
  // fingerprint: the evidence a bill's line rests on (D-252).
  if (payload.contains("id") && payload.at("id").is_string()) result.response_id = payload.at("id").get<std::string>();
  result.response_sha256 = platform::sha256_hex_digest(response.body);
  if (first_choice.contains("message") && first_choice.at("message").contains("content") &&
      first_choice.at("message").at("content").is_string()) {
    result.text = first_choice.at("message").at("content").get<std::string>();
  }
  // Real choices[].message.tool_calls[].function.{name,arguments} parsing -
  // no string scraping or regex, the ordinary OpenAI-compatible shape. Every
  // entry is parsed mechanically only: `arguments` is handed back as a
  // discarded nlohmann::json when the provider's own arguments string was
  // not valid JSON, and `wire_name` is not yet mapped to any canonical tool
  // id here - see ResearchToolLoop.hpp/ResearchTools.hpp, which own that
  // normalization and every semantic/security check downstream of it. This
  // layer never executes, validates business rules, or trusts any of it.
  if (first_choice.contains("message") && first_choice.at("message").contains("tool_calls") &&
      first_choice.at("message").at("tool_calls").is_array()) {
    for (const auto &rawCall : first_choice.at("message").at("tool_calls")) {
      if (!rawCall.is_object() || !rawCall.contains("function") || !rawCall.at("function").is_object()) continue;
      const auto &function = rawCall.at("function");
      ModelToolCall call;
      call.call_id = rawCall.value("id", std::string());
      call.wire_name = function.value("name", std::string());
      const std::string rawArguments = function.value("arguments", std::string());
      call.arguments = Json::parse(rawArguments, nullptr, false);
      result.tool_calls.push_back(std::move(call));
    }
  }

  if (payload.contains("usage") && payload.at("usage").is_object()) {
    const auto &usage = payload.at("usage");
    result.usage.input_tokens = json_int(usage, "prompt_tokens");
    result.usage.output_tokens = json_int(usage, "completion_tokens");
    result.usage.total_tokens = json_int(usage, "total_tokens");
    if (usage.contains("completion_tokens_details") && usage.at("completion_tokens_details").is_object())
      result.usage.reasoning_tokens = json_int(usage.at("completion_tokens_details"), "reasoning_tokens");
    // What the provider read from its prompt cache and wrote to it (PromptCache.hpp).
    if (usage.contains("prompt_tokens_details") && usage.at("prompt_tokens_details").is_object()) {
      result.usage.cached_input_tokens = json_int(usage.at("prompt_tokens_details"), "cached_tokens");
      result.usage.cache_write_tokens = json_int(usage.at("prompt_tokens_details"), "cache_write_tokens");
    }
    read_cost_evidence(usage, result.usage);
  }
  if (payload.contains("provider") && payload.at("provider").is_string() && !payload.at("provider").get<std::string>().empty())
    result.usage.served_by = payload.at("provider").get<std::string>();
  if (first_choice.contains("finish_reason") && first_choice.at("finish_reason").is_string()) {
    result.finish_reason = first_choice.at("finish_reason").get<std::string>();
  }
  // A strict structured-output refusal arrives as message.refusal with null
  // content. Left alone it becomes an empty "successful" generation, which the
  // professional-response parser accepts as a blank NoAction answer. Fail
  // closed with usage kept for billing; ResearchToolLoop's repair path
  // rethrows this code without retrying. The refusal text is never echoed: it
  // is pre-restoration provider output.
  if (result.text.empty() && result.tool_calls.empty() && first_choice.contains("message")
      && first_choice.at("message").contains("refusal") && first_choice.at("message").at("refusal").is_string()
      && !first_choice.at("message").at("refusal").get_ref<const std::string &>().empty()) {
    throw model_refusal_error(provider_display_name, result.usage, result.finish_reason);
  }
  return result;
}

GenerationResult generate_anthropic(const std::string &api_key, const std::string &model_id,
                                    const std::string &prompt,
                                    const FinalPayloadValidator &final_payload_validator,
								    IProviderTransport &transport,
								    std::optional<std::uint64_t> maximum_output_tokens) {
  // The member's pictures, before the words, as Anthropic reads them best (D-256).
  Json content = prompt;
  std::vector<std::string> pictureDigests;
  if (const std::shared_ptr<const std::vector<InputPicture>> pictures = current_input_pictures();
      pictures && pictures->empty() == false) {
    content = Json::array();
    for (const InputPicture &picture : *pictures) {
      content.push_back(Json{{"type", "image"},
          {"source", Json{{"type", "base64"}, {"media_type", picture.media_type}, {"data", picture.base64}}}});
      pictureDigests.push_back(picture.sha256);
    }
    content.push_back(Json{{"type", "text"}, {"text", prompt}});
  }
  const Json body{
      {"model", model_id},
      // Anthropic requires a ceiling; with none asked for, one no answer reaches.
      {"max_tokens", maximum_output_tokens.value_or(8192)},
      {"messages", Json::array({Json{{"role", "user"}, {"content", std::move(content)}}})},
  };

  if (!final_payload_validator) {
    throw std::invalid_argument("final provider payload validator is required");
  }
  const auto serialized_body = body_text(body);
  // This is the final serialized Anthropic request body. Validate it after
  // construction and immediately before handing it to the transport.
  final_payload_validator(FinalPayloadView{serialized_body, {prompt}, pictureDigests});

  const platform::HttpResponse response = post_generation(transport,
      "https://api.anthropic.com/v1/messages",
      {{"x-api-key", api_key}, {"anthropic-version", "2023-06-01"}, {"Content-Type", "application/json"}},
      serialized_body, "Anthropic");

  // Credentials must never enter parsed results, error messages or observers,
  // including when a misbehaving provider echoes its Authorization value.
  if (!api_key.empty() && response.body.find(api_key) != std::string::npos)
    throw TaskError(TaskErrorCode::ProviderAuthentication, "Provider returned credential material; response withheld.");
  namespace obs = singularity::routing::observability;
  if (obs::has_sink()) {
    // Headers are never observed. A provider echoing its credential is withheld.
    const bool isCredentialEcho = !api_key.empty() && response.body.find(api_key) != std::string::npos;
    obs::emit({obs::OperationEventKind::ProviderResponseReceived, "Provider HTTP response received",
        {{"status", std::to_string(response.status)}, {"bodyBytes", std::to_string(response.body.size())},
         {"credentialEchoWithheld", isCredentialEcho ? "true" : "false"}},
        isCredentialEcho ? std::nullopt : std::optional<std::string>(response.body)});
  }
  const auto payload = Json::parse(response.body, nullptr, false);
  refuse_out_of_credit(response, payload, "Anthropic");
  if (response.status == 401 || response.status == 403) {
    throw TaskError(TaskErrorCode::ProviderAuthentication,
                    "This API key is no longer valid. Reconnect this service in Models.");
  }
  if (response.status == 429) {
    const auto message = extract_error_message(payload);
    const std::string asked = wait_advice(response, std::string());
    throw TaskError(TaskErrorCode::ProviderRateLimit,
                    message ? ("Anthropic is rate-limiting requests: " + *message
                                   + (asked.empty() ? std::string() : " " + asked))
                            : ("Anthropic is rate-limiting requests. "
                                   + wait_advice(response, "Wait a moment and try again.")));
  }
  if (response.status >= 500) {
    const auto message = extract_error_message(payload);
    const std::string asked = wait_advice(response, std::string());
    throw TaskError(TaskErrorCode::NetworkError,
                    message ? ("Anthropic is temporarily unavailable: " + *message
                                   + (asked.empty() ? std::string() : " " + asked))
                            : ("Anthropic is temporarily unavailable (status " +
                                   std::to_string(response.status) + "). "
                                   + wait_advice(response, "Try again shortly.")));
  }
  // A request Anthropic declined (a 4xx: a schema too large for it, a request it
  // will not take) is the provider's refusal, as on every other provider path:
  // the lawyer is told to choose another model, and a refused response grammar
  // is sent once more as a described contract (InferenceExecutor).
  if (response.status >= 400 && response.status < 500) {
    const auto message = extract_error_message(payload);
    throw TaskError(TaskErrorCode::ProviderRejectedRequest,
                    message ? ("Anthropic rejected the request: " + *message)
                            : ("Anthropic rejected the request (status " +
                                   std::to_string(response.status) + ")."));
  }
  if (response.status < 200 || response.status >= 300) {
    const auto message = extract_error_message(payload);
    throw TaskError(TaskErrorCode::Unknown,
                    message ? ("Anthropic rejected the request: " + *message)
                            : ("Anthropic returned an error (status " +
                                   std::to_string(response.status) + ")."));
  }
  if (payload.is_discarded() || !payload.contains("content") || !payload.at("content").is_array() ||
      payload.at("content").empty()) {
    throw TaskError(TaskErrorCode::Unknown, "Anthropic returned an unexpected response.");
  }

  GenerationResult result;
  if (payload.contains("id") && payload.at("id").is_string()) result.response_id = payload.at("id").get<std::string>();
  result.response_sha256 = platform::sha256_hex_digest(response.body);
  for (const auto &block : payload.at("content")) {
    if (block.contains("type") && block.at("type") == "text" && block.contains("text") &&
        block.at("text").is_string()) {
      result.text += block.at("text").get<std::string>();
    }
  }

  if (payload.contains("usage") && payload.at("usage").is_object()) {
    const auto &usage = payload.at("usage");
    result.usage.input_tokens = json_int(usage, "input_tokens");
    result.usage.output_tokens = json_int(usage, "output_tokens");
    if (result.usage.input_tokens && result.usage.output_tokens) {
      result.usage.total_tokens = *result.usage.input_tokens + *result.usage.output_tokens;
    }
    read_cost_evidence(usage, result.usage);
  }
  if (payload.contains("stop_reason") && payload.at("stop_reason").is_string()) {
    result.finish_reason = payload.at("stop_reason").get<std::string>();
  }
  return result;
}

} // namespace

platform::HttpResponse OutboundCapturingTransport::post(
    const std::string &, const std::vector<platform::HttpHeader> &, const std::string &body, int) {
  throw OutboundPayloadCaptured{body};
}

bool is_known_provider(const std::string &provider_id) {
  return std::find(kKnownProviderIds.begin(), kKnownProviderIds.end(), provider_id) !=
         kKnownProviderIds.end();
}

bool is_valid_custom_provider_id(const std::string &provider_id) {
  if (provider_id.empty() || provider_id.size() > 40) return false;
  static constexpr std::array<const char *, 5> kReserved{
      "openai", "anthropic", "openai_compatible", "google", "local"};
  if (std::find(kReserved.begin(), kReserved.end(), provider_id) != kReserved.end()) return false;
  return std::all_of(provider_id.begin(), provider_id.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
           character == '-';
  });
}

bool provider_requires_base_url(const std::string &provider_id) {
  return provider_id != "openai" && provider_id != "anthropic";
}

namespace {

// Whether an address's authority - what stands between "://" and the first
// "/", "?" or "#" - is this computer's own server: the host exactly
// localhost (in any case), 127.0.0.1 or [::1], then at most ":" and a port
// 1-65535 written in digits. Userinfo ("localhost@evil.net"), a longer host
// ("localhost.evil.net", "127.0.0.1.nip.io", "localhost."), another writing
// of the address (127.1, 2130706433, 0x7f000001) or an escape is a remote
// server, read here exactly as an HTTP client reads it.
bool is_loopback_authority(const std::string &authority) {
  std::string host = authority;
  std::string port;
  bool hasPort = false;
  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos) return false;
    host = authority.substr(0, close + 1);
    const std::string rest = authority.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != ':') return false;
      port = rest.substr(1);
      hasPort = true;
    }
  } else {
    const std::size_t colon = authority.find(':');
    if (colon != std::string::npos) {
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
      hasPort = true;
    }
  }
  if (lowercase(host) != "localhost" && host != "127.0.0.1" && host != "[::1]") return false;
  if (!hasPort) return true;
  if (port.empty() || port.size() > 5
      || !std::all_of(port.begin(), port.end(), [](unsigned char character) { return std::isdigit(character) != 0; }))
    return false;
  const long number = std::stol(port);
  return number >= 1 && number <= 65535;
}

} // namespace

bool is_valid_base_url(const std::string &base_url) {
  if (base_url.rfind("https://", 0) == 0 && base_url.size() > 8) {
    return true;
  }
  // Plain http:// carries the key and every request unencrypted, so it is for
  // this computer's own server alone. An address with a space, a control
  // character or a backslash in it is read differently by different HTTP
  // clients and is never taken for one.
  if (base_url.rfind("http://", 0) != 0) return false;
  if (std::any_of(base_url.begin(), base_url.end(), [](unsigned char character) {
        return character <= 0x20 || character >= 0x7f || character == '\\';
      }))
    return false;
  constexpr std::size_t kAuthorityStart = 7;
  const std::size_t authorityEnd = base_url.find_first_of("/?#", kAuthorityStart);
  return is_loopback_authority(base_url.substr(kAuthorityStart, authorityEnd == std::string::npos
      ? std::string::npos : authorityEnd - kAuthorityStart));
}

ProviderTestResult test_connection(const std::string &provider_id, const std::string &api_key,
                                   const std::optional<std::string> &base_url) {
  if (provider_id == "openai") {
    return test_openai_style("OpenAI", openai_style_root(provider_id, base_url), api_key);
  }
  if (provider_id == "anthropic") {
    return test_anthropic(api_key);
  }
  // "openai_compatible" and every real custom connection (is_valid_custom_
  // provider_id()) share this exact same generic path - the universal
  // "connect any provider" mechanism promised at this file's own top
  // comment. Anything reaching here that is neither a known preset nor a
  // valid custom id was already rejected by the caller (ProviderConnections.cpp)
  // before ever constructing a request.
  if (!base_url || !is_valid_base_url(*base_url)) {
    ProviderTestResult result;
    result.error = "Enter a valid HTTPS endpoint URL (or an http:// localhost URL for a local server).";
    return result;
  }
  const std::string root = openai_style_root(provider_id, base_url);
  return test_openai_style(generic_service_name(root), root, api_key);
}

bool is_openrouter_root(const std::string &root) { return root.find("openrouter.ai") != std::string::npos; }

std::optional<GenerationRecord> parse_openrouter_generation(const std::string &body) {
  const Json payload = Json::parse(body, nullptr, false);
  if (payload.is_discarded() || !payload.is_object() || !payload.contains("data") || !payload.at("data").is_object())
    return std::nullopt;
  const Json &data = payload.at("data");
  const auto count = [&data](const char *native, const char *plain) -> std::optional<long long> {
    for (const char *key : {native, plain})
      if (data.contains(key) && data.at(key).is_number_integer() && data.at(key).get<long long>() >= 0)
        return data.at(key).get<long long>();
    return std::nullopt;
  };
  const auto text = [&data](const char *key) {
    return data.contains(key) && data.at(key).is_string() ? data.at(key).get<std::string>() : std::string();
  };
  GenerationRecord record;
  if (data.contains("total_cost") && data.at("total_cost").is_number() && data.at("total_cost").get<double>() >= 0.0)
    record.cost_micros = std::llround(data.at("total_cost").get<double>() * 1000000.0);
  // The provider's own count of tokens where OpenRouter gives it, as the call
  // itself reported them; its own count otherwise.
  record.input_tokens = count("native_tokens_prompt", "tokens_prompt");
  record.output_tokens = count("native_tokens_completion", "tokens_completion");
  record.provider = text("provider_name");
  record.model = text("model");
  record.created_at = text("created_at");
  record.record_sha256 = platform::sha256_hex_digest(body);
  return record;
}

std::optional<GenerationRecord> openrouter_generation(const std::string &api_key, const std::string &root,
                                                      const std::string &id) {
  if (api_key.empty() || id.empty() || !is_openrouter_root(root)) return std::nullopt;
  // An id is OpenRouter's own ("gen-..."): letters, digits and dashes alone go
  // into the address.
  if (!std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; }))
    return std::nullopt;
  try {
    const platform::HttpResponse response =
        platform::https_get(trim_trailing_slash(root) + "/generation?id=" + id, {{"Authorization", "Bearer " + api_key}});
    if (response.status != 200) return std::nullopt;
    return parse_openrouter_generation(response.body);
  } catch (...) {
    return std::nullopt;
  }
}

GenerationResult generate(const std::string &provider_id, const std::string &api_key,
                          const std::string &model_id, const std::string &prompt,
                          const FinalPayloadValidator &final_payload_validator,
						  const std::optional<std::string> &base_url,
						  std::optional<std::uint64_t> maximum_output_tokens,
						  const std::optional<GenerationOutputContract> &output_contract,
						  const std::optional<GenerationToolControl> &tool_control,
						  ReasoningEffort reasoning_effort) {
  HttpsProviderTransport transport;
  return generate_with_transport(provider_id, api_key, model_id, prompt,
                                 final_payload_validator, transport, base_url, maximum_output_tokens,
                                 output_contract, tool_control, reasoning_effort);
}

GenerationResult generate_with_transport(
    const std::string &provider_id, const std::string &api_key,
    const std::string &model_id, const std::string &prompt,
    const FinalPayloadValidator &final_payload_validator,
    IProviderTransport &transport, const std::optional<std::string> &base_url,
	std::optional<std::uint64_t> maximum_output_tokens,
	const std::optional<GenerationOutputContract> &output_contract,
	const std::optional<GenerationToolControl> &tool_control,
	ReasoningEffort reasoning_effort) {
  if (provider_id == "openai") {
    return generate_openai_style("OpenAI", openai_style_root(provider_id, base_url), api_key,
                                 model_id, prompt, final_payload_validator, transport,
                                 maximum_output_tokens, output_contract, tool_control, reasoning_effort);
  }
  if (provider_id == "anthropic") {
    // Anthropic native tool calling is out of scope for Phase 24.1D - no
    // caller may reach here with tool_control set, since ResearchToolLoop.hpp
    // only ever builds one from a model's own discovered
    // ModelDescriptor::supports_native_tools, which discovery never sets for
    // "anthropic" (its /models response carries no supported_parameters).
    // Failing closed here rather than silently dropping the caller's tool
    // offer keeps that guarantee real instead of assumed.
    if (tool_control) {
      throw TaskError(TaskErrorCode::UnsupportedModel,
                      "Native tool calling is not supported for the Anthropic provider.");
    }
    return generate_anthropic(api_key, model_id, prompt, final_payload_validator, transport,
							  maximum_output_tokens);
  }
  // "openai_compatible" and every real custom connection share this exact
  // same generic OpenAI-style path - see test_connection()'s own comment.
  if (!base_url || !is_valid_base_url(*base_url)) {
    throw TaskError(TaskErrorCode::UnsupportedModel,
                    "This service's address is not configured. Reconnect it in Models.");
  }
  const std::string root = openai_style_root(provider_id, base_url);
  return generate_openai_style(generic_service_name(root), root, api_key,
                               model_id, prompt, final_payload_validator, transport,
                               maximum_output_tokens, output_contract, tool_control, reasoning_effort);
}

bool accepts_reasoning_effort(const std::string &provider_id, const ModelDescriptor &model) {
  if (provider_id == "anthropic" || provider_id == "local") return false;
  const std::string id = lowercase(model.id);
  // A model that does not think unless asked - Claude before its fifth
  // generation, a "-chat" model - would start thinking when asked for a short
  // think: slower, never faster. Claude from its fifth generation thinks by
  // itself, as much as it judges (its calls report thinking tokens unasked:
  // 3,669 for one conversational answer, 2026-10-26), so a short think asked
  // for is shorter, where the provider lists the parameter for it.
  if (id.find("-chat") != std::string::npos) return false;
  if (id.rfind("anthropic/", 0) == 0) {
    const std::size_t family = id.find("claude-");
    std::size_t at = family == std::string::npos ? std::string::npos : id.find('-', family + 7);
    const bool isFifthOrLater = at != std::string::npos && at + 1 < id.size()
        && std::isdigit(static_cast<unsigned char>(id[at + 1])) != 0 && std::atoi(id.c_str() + at + 1) >= 5;
    if (!isFifthOrLater) return false;
  }
  for (const std::string &parameter : model.supported_parameters)
    if (parameter == "reasoning" || parameter == "reasoning_effort") return true;
  if (provider_id != "openai") return false;
  // Direct OpenAI lists no parameters: its reasoning models by family. A
  // "-chat" model is the non-reasoning one of its family (above).
  for (const char *family : {"gpt-5", "o3", "o4"})
    if (id.rfind(family, 0) == 0) return true;
  // "o1" itself takes it; o1-mini and o1-preview never did.
  return id == "o1" || id.rfind("o1-20", 0) == 0;
}

bool requires_reasoning(const std::string &message) {
  const std::string lowered = lowercase(message);
  return lowered.find("reasoning") != std::string::npos
      && (lowered.find("mandatory") != std::string::npos || lowered.find("cannot be disabled") != std::string::npos
          || lowered.find("can't be disabled") != std::string::npos || lowered.find("is required") != std::string::npos);
}

bool refuses_reasoning_effort(const std::string &message) {
  const std::string lowered = lowercase(message);
  // OpenRouter, asked to require every parameter, turns a request away as a
  // whole when no one endpoint takes them all - reasoning among them - without
  // naming which: "No endpoints found that can handle the requested
  // parameters". Sent again without the reasoning parameter, it is the request
  // that worked before.
  if (lowered.find("no endpoints found") != std::string::npos
      || lowered.find("requested parameters") != std::string::npos)
    return true;
  return lowered.find("reasoning") != std::string::npos
      && (lowered.find("unsupported") != std::string::npos || lowered.find("not supported") != std::string::npos
          || lowered.find("unrecognized") != std::string::npos || lowered.find("unknown") != std::string::npos
          || lowered.find("invalid") != std::string::npos || lowered.find("does not support") != std::string::npos);
}

void set_generation_retry_delay_for_testing(std::chrono::milliseconds delay) {
  retry_base_delay() = delay;
}

std::optional<long long> retry_after_seconds(const std::string &value, std::chrono::system_clock::time_point now) {

  std::size_t begin = 0;
  std::size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
  const std::string text = value.substr(begin, end - begin);
  if (text.empty() == true) return std::nullopt;

  // Whole seconds. A figure too long to hold is simply a very long wait.
  if (std::all_of(text.begin(), text.end(), [](char character) { return character >= '0' && character <= '9'; }))
    return text.size() > 9 ? 999999999LL : std::stoll(text);

  /* - - - - - - */

  // An HTTP date. Its three forms hold the same parts in the same order - a day
  // of the month before the year - so the parts are read by their kind:
  // "Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT",
  // "Sun Nov  6 08:49:37 1994".
  static const char *const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                        "jul", "aug", "sep", "oct", "nov", "dec"};
  int day = -1, month = -1, year = -1, hour = -1, minute = -1, second = -1;
  std::string token;
  const auto read_token = [&]() {
    if (token.empty() == true) return;
    if (token.find(':') != std::string::npos) {
      if (std::sscanf(token.c_str(), "%d:%d:%d", &hour, &minute, &second) != 3) hour = -1;
    } else if (std::isdigit(static_cast<unsigned char>(token.front())) != 0) {
      const bool isNumber = std::all_of(token.begin(), token.end(),
          [](char character) { return character >= '0' && character <= '9'; });
      if (isNumber == true && token.size() <= 4) {
        const int number = std::stoi(token);
        if (day < 0) day = number;
        else if (year < 0) year = token.size() <= 2 ? (number < 70 ? 2000 + number : 1900 + number) : number;
      }
    } else if (token.size() >= 3) {
      const std::string name = lowercase(token.substr(0, 3));
      for (int index = 0; index < 12; ++index)
        if (name == kMonths[index]) month = index + 1;
    }
    token.clear();
  };
  for (const char character : text) {
    if (character == ' ' || character == ',' || character == '-' || character == '\t') read_token();
    else token.push_back(character);
  }
  read_token();
  if (day < 1 || day > 31 || month < 1 || year < 1970 || hour < 0 || hour > 23 || minute < 0 || minute > 59
      || second < 0 || second > 60)
    return std::nullopt;

  // Days since 1970-01-01 of a Gregorian date (H. Hinnant's days_from_civil).
  const long long shifted = month <= 2 ? year - 1 : year;
  const long long era = shifted / 400;
  const long long yearOfEra = shifted - era * 400;
  const long long dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const long long dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const long long days = era * 146097 + dayOfEra - 719468;
  const long long at = days * 86400 + hour * 3600 + minute * 60 + second;
  const long long current = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  return at > current ? at - current : 0;

}

} // namespace singularity::providers
