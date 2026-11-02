#include "Platform/UdpSocket/UdpSocket.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mswsock.h>

#include "Platform/UdpSocket/UdpSocketAddresses.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace singularity::platform {
namespace {

void startWinsock() {
	static std::once_flag once;
	std::call_once(once, [] {
		WSADATA data{};
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Windows' network could not be started.");
	});
}

SOCKET socketOf(std::intptr_t handle) {
	return static_cast<SOCKET>(handle);
}

// A network card of this computer, as the system says of its adapters. Tunnels
// and VPN adapters (WireGuard, Wintun, TAP), the virtual switches only this
// computer's virtual machines see (WSL's, Hyper-V's) and dial-up links are
// not: their addresses reach no other computer, or STUN finds them.
bool isNetworkCard(const IP_ADAPTER_ADDRESSES* adapter) {
	if (adapter->TunnelType != TUNNEL_TYPE_NONE) return false;
	MIB_IF_ROW2 row{};
	row.InterfaceLuid = adapter->Luid;
	if (GetIfEntry2(&row) != NO_ERROR) return false;
	return row.InterfaceAndOperStatusFlags.HardwareInterface != FALSE;
}

} // namespace

UdpSocket::UdpSocket(std::uint16_t port) {
	startWinsock();
	SOCKET made = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (made != INVALID_SOCKET) {
		// Both stacks on one socket, whatever the system's default.
		DWORD off = 0;
		if (setsockopt(made, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off) == 0) {
			isDualStack_ = true;
		} else {
			closesocket(made);
			made = INVALID_SOCKET;
		}
	}
	if (made == INVALID_SOCKET) made = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (made == INVALID_SOCKET) throw std::runtime_error("A network socket could not be opened on this computer.");
	// Windows reports an earlier datagram's "port unreachable" as an error on
	// the next read; trying addresses to get through routers sends many such.
	BOOL reportReset = FALSE;
	DWORD returned = 0;
	WSAIoctl(made, SIO_UDP_CONNRESET, &reportReset, sizeof reportReset, nullptr, 0, &returned, nullptr, nullptr);
	sockaddr_storage local{};
	int length = 0;
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
	if (bind(made, reinterpret_cast<const sockaddr*>(&local), length) != 0) {
		closesocket(made);
		throw std::runtime_error("The network port " + std::to_string(port) + " is taken on this computer.");
	}
	sockaddr_storage bound{};
	int boundLength = sizeof bound;
	if (getsockname(made, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
		port_ = bound.ss_family == AF_INET6 ? ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port)
			: ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
	}
	handle_ = static_cast<std::intptr_t>(made);
	isOpen_ = true;
}

UdpSocket::~UdpSocket() {
	close();
	// Given back only now, when no receive() can be waiting on it.
	if (handle_ >= 0) closesocket(socketOf(handle_));
	handle_ = -1;
}

bool UdpSocket::send_to(const std::string& address, const std::vector<std::uint8_t>& bytes) {
	if (isOpen_.load() == false || bytes.size() > kMostDatagram) return false;
	sockaddr_storage target{};
	int length = 0;
	if (udp_addresses::parse(address, isDualStack_ ? AF_INET6 : AF_INET, target, length) == false) return false;
	const int sent = sendto(socketOf(handle_), reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
		reinterpret_cast<const sockaddr*>(&target), length);
	return sent == static_cast<int>(bytes.size());
}

std::optional<UdpSocket::Datagram> UdpSocket::receive(std::chrono::milliseconds timeout) {
	const auto until = std::chrono::steady_clock::now() + timeout;
	std::vector<std::uint8_t> buffer(kMostDatagram);
	while (isOpen_.load() == true) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
		if (left.count() < 0) return std::nullopt;
		// In slices of a tenth of a second, so close() is heard soon.
		WSAPOLLFD watched{};
		watched.fd = socketOf(handle_);
		watched.events = POLLRDNORM;
		const int ready = WSAPoll(&watched, 1, static_cast<INT>(std::min<long long>(left.count(), 100)));
		if (ready <= 0) {
			if (left.count() == 0) return std::nullopt;
			continue;
		}
		if ((watched.revents & (POLLERR | POLLNVAL)) != 0 && (watched.revents & POLLRDNORM) == 0) return std::nullopt;
		sockaddr_storage from{};
		int fromLength = sizeof from;
		const int got = recvfrom(socketOf(handle_), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
			reinterpret_cast<sockaddr*>(&from), &fromLength);
		// Closed meanwhile: what the wait woke to is no datagram.
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
	// Stops it at once; a waiting receive() sees it within its slice. The
	// socket itself is given back by the destructor.
	shutdown(socketOf(handle_), SD_BOTH);
}

std::vector<std::string> resolve(const std::string& host, std::uint16_t port) {
	std::vector<std::string> found;
	try {
		startWinsock();
	} catch (const std::exception&) {
		return found;
	}
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* list = nullptr;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0) return found;
	for (const addrinfo* entry = list; entry != nullptr; entry = entry->ai_next) {
		const std::string text = udp_addresses::textOf(entry->ai_addr);
		if (text.empty() == false && std::find(found.begin(), found.end(), text) == found.end()) found.push_back(text);
	}
	freeaddrinfo(list);
	return found;
}

std::vector<std::string> local_addresses() {
	startWinsock();
	std::vector<std::string> found;
	ULONG size = 16 * 1024;
	std::vector<unsigned char> buffer(size);
	const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
	if (result == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(size);
		result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
	}
	if (result != NO_ERROR) return found;
	std::vector<std::string> others;
	for (const IP_ADAPTER_ADDRESSES* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
		adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		std::vector<std::string>& into = isNetworkCard(adapter) ? found : others;
		for (const IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
			const std::string host = udp_addresses::hostOf(unicast->Address.lpSockaddr);
			if (host.empty() == true || udp_addresses::isLinkLocalOrLoopback(host) == true) continue;
			if (std::find(into.begin(), into.end(), host) == into.end()) into.push_back(host);
		}
	}
	return found.empty() == false ? found : others;
}

} // namespace singularity::platform
