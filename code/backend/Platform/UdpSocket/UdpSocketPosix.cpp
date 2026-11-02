#include "Platform/UdpSocket/UdpSocket.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Platform/UdpSocket/UdpSocketAddresses.hpp"

#include <algorithm>
#include <cerrno>
#include <stdexcept>

namespace singularity::platform {

UdpSocket::UdpSocket(std::uint16_t port) {
	int made = ::socket(AF_INET6, SOCK_DGRAM, 0);
	if (made >= 0) {
		// Both stacks on one socket, whatever the system's default.
		const int off = 0;
		if (::setsockopt(made, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off) == 0) {
			isDualStack_ = true;
		} else {
			::close(made);
			made = -1;
		}
	}
	if (made < 0) made = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (made < 0) throw std::runtime_error("A network socket could not be opened on this computer.");
	sockaddr_storage local{};
	socklen_t length = 0;
	if (isDualStack_ == true) {
		sockaddr_in6* any = reinterpret_cast<sockaddr_in6*>(&local);
		any->sin6_family = AF_INET6;
		any->sin6_addr = in6addr_any;
		any->sin6_port = htons(port);
		length = sizeof(sockaddr_in6);
	} else {
		sockaddr_in* any = reinterpret_cast<sockaddr_in*>(&local);
		any->sin_family = AF_INET;
		any->sin_addr.s_addr = htonl(INADDR_ANY);
		any->sin_port = htons(port);
		length = sizeof(sockaddr_in);
	}
	if (::bind(made, reinterpret_cast<const sockaddr*>(&local), length) != 0) {
		::close(made);
		throw std::runtime_error("The network port " + std::to_string(port) + " is taken on this computer.");
	}
	sockaddr_storage bound{};
	socklen_t boundLength = sizeof bound;
	if (::getsockname(made, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
		port_ = bound.ss_family == AF_INET6 ? ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port)
			: ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
	}
	handle_ = made;
	isOpen_ = true;
}

UdpSocket::~UdpSocket() {
	close();
	// Given back only now, when no receive() can be waiting on it: a number
	// given back while one waits could be another socket's a moment later.
	if (handle_ >= 0) ::close(static_cast<int>(handle_));
	handle_ = -1;
}

bool UdpSocket::send_to(const std::string& address, const std::vector<std::uint8_t>& bytes) {
	if (isOpen_.load() == false || bytes.size() > kMostDatagram) return false;
	sockaddr_storage target{};
	int length = 0;
	if (udp_addresses::parse(address, isDualStack_ ? AF_INET6 : AF_INET, target, length) == false) return false;
	const ssize_t sent = ::sendto(static_cast<int>(handle_), bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&target),
		static_cast<socklen_t>(length));
	return sent == static_cast<ssize_t>(bytes.size());
}

std::optional<UdpSocket::Datagram> UdpSocket::receive(std::chrono::milliseconds timeout) {
	const auto until = std::chrono::steady_clock::now() + timeout;
	std::vector<std::uint8_t> buffer(kMostDatagram);
	while (isOpen_.load() == true) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
		if (left.count() < 0) return std::nullopt;
		// In slices of a tenth of a second, so close() is heard soon: closing a
		// descriptor does not wake a poll() on Linux.
		pollfd watched{};
		watched.fd = static_cast<int>(handle_);
		watched.events = POLLIN;
		const int ready = ::poll(&watched, 1, static_cast<int>(std::min<long long>(left.count(), 100)));
		if (ready <= 0) {
			if (ready < 0 && errno != EINTR) return std::nullopt;
			if (left.count() == 0) return std::nullopt;
			continue;
		}
		if ((watched.revents & (POLLERR | POLLNVAL)) != 0 && (watched.revents & POLLIN) == 0) return std::nullopt;
		sockaddr_storage from{};
		socklen_t fromLength = sizeof from;
		const ssize_t got = ::recvfrom(static_cast<int>(handle_), buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from),
			&fromLength);
		// Closed meanwhile: Linux wakes the wait and reads nothing, which is no
		// datagram.
		if (isOpen_.load() == false) return std::nullopt;
		if (got < 0) continue;
		Datagram datagram;
		datagram.from = udp_addresses::textOf(reinterpret_cast<const sockaddr*>(&from));
		datagram.bytes.assign(buffer.begin(), buffer.begin() + got);
		return datagram;
	}
	return std::nullopt;
}

void UdpSocket::close() {
	if (isOpen_.exchange(false) == false) return;
	// Stops it at once and wakes a waiting poll(); the descriptor itself is
	// given back by the destructor.
	::shutdown(static_cast<int>(handle_), SHUT_RDWR);
}

std::vector<std::string> resolve(const std::string& host, std::uint16_t port) {
	std::vector<std::string> found;
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* list = nullptr;
	if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0) return found;
	for (const addrinfo* entry = list; entry != nullptr; entry = entry->ai_next) {
		const std::string text = udp_addresses::textOf(entry->ai_addr);
		if (text.empty() == false && std::find(found.begin(), found.end(), text) == found.end()) found.push_back(text);
	}
	::freeaddrinfo(list);
	return found;
}

std::vector<std::string> local_addresses() {
	std::vector<std::string> cards;
	std::vector<std::string> others;
	ifaddrs* list = nullptr;
	if (::getifaddrs(&list) != 0) return cards;
	for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
		if (entry->ifa_addr == nullptr || (entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) continue;
		if (entry->ifa_addr->sa_family != AF_INET && entry->ifa_addr->sa_family != AF_INET6) continue;
		const std::string host = udp_addresses::hostOf(entry->ifa_addr);
		if (host.empty() == true || udp_addresses::isLinkLocalOrLoopback(host) == true) continue;
		const bool isCard = entry->ifa_name != nullptr && is_virtual_interface_name(entry->ifa_name) == false;
		std::vector<std::string>& into = isCard ? cards : others;
		if (std::find(into.begin(), into.end(), host) == into.end()) into.push_back(host);
	}
	::freeifaddrs(list);
	return cards.empty() == false ? cards : others;
}

} // namespace singularity::platform
