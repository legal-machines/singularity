#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::platform {

// A UDP socket on all three systems, for the firm's computers to reach each
// other directly. One socket takes both IPv4 and IPv6 where the system has
// IPv6 - IPV6_V6ONLY is set off explicitly, since the systems' defaults differ
// - and IPv4 alone where it has not. Addresses are text: "192.168.1.72:51820",
// "[2001:db8::1]:51820"; an IPv4 address reached through the IPv6 socket is
// said as IPv4, never as "::ffff:". Winsock on Windows (UdpSocketWindows.cpp);
// BSD sockets on macOS and Linux (UdpSocketPosix.cpp).
class UdpSocket {
public:
	struct Datagram {
		std::string from;
		std::vector<std::uint8_t> bytes;
	};

	// Opens a socket on `port` - 0 for any free one - on every address of this
	// computer. Throws std::runtime_error, in plain words, when it cannot.
	explicit UdpSocket(std::uint16_t port = 0);
	~UdpSocket();
	UdpSocket(const UdpSocket&) = delete;
	UdpSocket& operator=(const UdpSocket&) = delete;

	std::uint16_t local_port() const { return port_; }
	bool is_dual_stack() const { return isDualStack_; }
	// Sends one datagram; false where the address is not one or the system
	// refused it.
	bool send_to(const std::string& address, const std::vector<std::uint8_t>& bytes);
	// Waits up to `timeout` for one datagram: nothing when none came, or when
	// the socket was closed meanwhile.
	std::optional<Datagram> receive(std::chrono::milliseconds timeout);
	// Closes the socket. A receive() waiting in another thread returns within a
	// tenth of a second.
	void close();
	bool is_open() const { return isOpen_.load(); }

	// The largest datagram it receives whole; the link sends far less (1,200).
	static constexpr std::size_t kMostDatagram = 65507;

private:
	std::intptr_t handle_ = -1;
	std::uint16_t port_ = 0;
	bool isDualStack_ = false;
	std::atomic<bool> isOpen_{false};
};

// This computer's own addresses, where others on its networks may reach it:
// those of its network cards - no loopback, no link-local, no interface that
// is down, and no tunnel, VPN or virtual network of its virtual machines and
// containers, whose addresses reach no other computer and only tell it where
// this one's VPN runs. Where no network card has an address, the others are
// given (a bridge may hold the card's address). Text without a port.
std::vector<std::string> local_addresses();

// Whether an interface of this name, as macOS and Linux name them, is a
// tunnel or VPN (utun, ipsec, ppp, gif, stf, tun, tap, wg, tailscale, zt), the
// bridge or virtual Ethernet of virtual machines and containers (bridge,
// vmenet, vmnet, vboxnet, docker, br-, veth, virbr, vnet, lxc, lxd, cni,
// podman), or one of Apple's links between its own devices (awdl, llw, anpi).
// A bridge a person made for their network ("br0") is not. Windows tells its
// network cards apart itself.
inline bool is_virtual_interface_name(const std::string& name) {
	static const char* const kPrefixes[] = {"utun", "ipsec", "ppp", "gif", "stf", "tun", "tap", "wg", "tailscale", "zt",
		"bridge", "vmenet", "vmnet", "vboxnet", "docker", "br-", "veth", "virbr", "vnet", "lxc", "lxd", "cni", "podman",
		"awdl", "llw", "anpi"};
	for (const char* prefix : kPrefixes) {
		if (name.rfind(prefix, 0) == 0) return true;
	}
	return false;
}

// The addresses a name stands for, with `port`, as UdpSocket says them
// ("142.250.82.127:19302", "[2001:4860:4864:5:8000::1]:19302"); nothing where
// the name is not found. It may wait on the network: never call it where a
// wait holds anything up.
std::vector<std::string> resolve(const std::string& host, std::uint16_t port);

} // namespace singularity::platform
