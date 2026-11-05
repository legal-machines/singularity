#include "Platform/WebSocket/WebSocket.hpp"
#include "Platform/WebSocket/WebSocketInbox.hpp"

#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

#include <memory>
#include <string>

// macOS's own WebSocket client, NSURLSessionWebSocketTask: it keeps the
// system's TLS, proxies and trust, as the HTTP client does
// (Platform/HttpClient/HttpClientMacOS.mm). Compiled with ARC.

namespace singularity::platform {
namespace {

// A system message's words, or `otherwise` where it has none.
std::string words_of(NSString *text, const char *otherwise) {
	const char *words = text.UTF8String;
	return words != nullptr ? std::string(words) : std::string(otherwise);
}

// The most sends handed to the system and not yet written: a slow network
// holds the sender back here rather than piling up.
constexpr long kMostSendsInFlight = 256;

void receiveNext(NSURLSessionWebSocketTask *task, std::shared_ptr<WebSocketInbox> inbox) {
	[task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *message, NSError *error) {
		if (error != nil || message == nil) {
			inbox->end(error != nil ? words_of(error.localizedDescription, "The connection closed.") : std::string("The connection closed."));
			return;
		}
		WebSocket::Message received;
		if (message.type == NSURLSessionWebSocketMessageTypeString) {
			received.isText = true;
			received.data = words_of(message.string, "");
		} else {
			received.data.assign(static_cast<const char *>(message.data.bytes), message.data.length);
		}
		inbox->put(std::move(received));
		if (inbox->is_ended() == false) receiveNext(task, inbox);
	}];
}

class WebSocketMacOS final : public WebSocket {
public:
	WebSocketMacOS(NSURLSession *session, NSURLSessionWebSocketTask *task)
		: session_(session), task_(task), inbox_(std::make_shared<WebSocketInbox>()),
		  inFlight_(dispatch_semaphore_create(kMostSendsInFlight)) {
		[task_ resume];
		receiveNext(task_, inbox_);
	}
	~WebSocketMacOS() override { close(); }

	void send_binary(const std::string &data) override {
		NSData *bytes = [NSData dataWithBytes:data.data() length:data.size()];
		send([[NSURLSessionWebSocketMessage alloc] initWithData:bytes]);
	}

	void send_text(const std::string &text) override {
		NSString *string = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
		send([[NSURLSessionWebSocketMessage alloc] initWithString:string != nil ? string : @""]);
	}

	std::optional<Message> receive(std::chrono::milliseconds wait) override { return inbox_->take(wait); }

	void close() override {
		inbox_->end("The connection closed.");
		[task_ cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
		[session_ invalidateAndCancel];
	}

private:
	void send(NSURLSessionWebSocketMessage *message) {
		if (inbox_->is_ended() == true) throw HttpTransportError("The connection closed.", false);
		if (dispatch_semaphore_wait(inFlight_, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) != 0) {
			inbox_->end("The connection stopped taking data.");
			throw HttpTransportError("The connection stopped taking data.", true);
		}
		std::shared_ptr<WebSocketInbox> inbox = inbox_;
		dispatch_semaphore_t inFlight = inFlight_;
		[task_ sendMessage:message completionHandler:^(NSError *error) {
			dispatch_semaphore_signal(inFlight);
			if (error != nil) inbox->end(words_of(error.localizedDescription, "The connection closed."));
		}];
	}

	NSURLSession *session_;
	NSURLSessionWebSocketTask *task_;
	std::shared_ptr<WebSocketInbox> inbox_;
	dispatch_semaphore_t inFlight_;
};

} // namespace

std::unique_ptr<WebSocket> open_websocket(const std::string &url, const std::vector<HttpHeader> &headers, int timeout_seconds) {
	@autoreleasepool {
		std::string address = url;
		if (address.rfind("https://", 0) == 0) address = "wss://" + address.substr(8);
		if (address.rfind("wss://", 0) != 0) throw HttpTransportError("Only a secure address can be opened.", false);
		NSURL *target = [NSURL URLWithString:[NSString stringWithUTF8String:address.c_str()]];
		if (target == nil) throw HttpTransportError("The address could not be read.", false);
		NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:target];
		request.timeoutInterval = timeout_seconds;
		for (const HttpHeader &header : headers) {
			[request setValue:[NSString stringWithUTF8String:header.value.c_str()]
			    forHTTPHeaderField:[NSString stringWithUTF8String:header.name.c_str()]];
		}
		NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		configuration.timeoutIntervalForRequest = timeout_seconds;
		NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration];
		NSURLSessionWebSocketTask *task = [session webSocketTaskWithRequest:request];
		task.maximumMessageSize = 1 << 20;
		return std::make_unique<WebSocketMacOS>(session, task);
	}
}

} // namespace singularity::platform
