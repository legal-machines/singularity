#include "Provider/AccountGate/AccountGate.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace singularity::providers {
namespace {

std::mutex &gate_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::function<bool()> &gate() {
	static std::function<bool()> may_connect;
	return may_connect;
}

} // namespace

void set_may_connect_accounts(std::function<bool()> may_connect) {
	const std::lock_guard<std::mutex> lock(gate_mutex());
	gate() = std::move(may_connect);
}

void require_may_connect_accounts() {
	std::function<bool()> may_connect;
	{
		const std::lock_guard<std::mutex> lock(gate_mutex());
		may_connect = gate();
	}
	if (may_connect && may_connect() == false) {
		throw std::runtime_error("Only the firm's administrator connects the firm's model accounts. Your AI colleagues "
			"think with the accounts they connected.");
	}
}

} // namespace singularity::providers
