#include "Matter/FirmComputers/FirmComputersProduction.hpp"

#include "LocalModel/ModelPackageManager/ModelPackageManager.hpp"
#include "Matter/MatterCoordination/MatterCoordination.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Node/LegalMachinesAccount/LegalMachinesAccount.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Platform/Events/Events.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Tool/DeviceTrust/DeviceTrust.hpp"

namespace singularity::matter::firm_computers {

namespace {

// Posts to Legal Machines with this computer's session, as the account's own
// requests do; a short wait, since the background asks every few seconds and
// a closing window waits for it.
node::AccountPoster session_post() {
	return [](const std::string &url, const std::string &body) {
		std::vector<platform::HttpHeader> headers = local_models::production_model_package_manager().session_headers();
		if (headers.empty() == true) return platform::HttpResponse{401, "", {}};
		headers.push_back({"Content-Type", "application/json"});
		return platform::https_post(url, headers, body, 10);
	};
}

// The seat signed in here: its member is "seat:<id>" (Platform/ActorIdentity).
std::string seat_signed_in() {
	const std::string actor = platform::resolve_actor_identity().actor_id;
	return actor.rfind("seat:", 0) == 0 ? actor.substr(5) : std::string();
}

} // namespace

Environment production_environment() {
	Environment environment;
	environment.device_id = []() { return platform::current_device_id(); };
	environment.device_key = []() { return platform::device_public_key(); };
	environment.coordination_key = []() { return coordination::own_coordination_key(); };
	environment.seat_id = []() { return seat_signed_in(); };
	environment.candidates = []() { return coordination::own_candidates(); };
	environment.sign = [](const std::string &message) { return platform::sign_with_device_key(message); };
	environment.verify = [](const std::string &key, const std::string &message, const std::string &signature) {
		return platform::verify_device_signature(key, message, signature);
	};
	environment.nonce = []() { return platform::secure_random_hex(32); };
	// Public keys and addresses only, never a name, a Matter or a file
	// (AG-SEC-9).
	environment.announce = [](const Computer &self) {
		node::ListedComputer listed;
		listed.seat_id = self.seat_id;
		listed.device_id = self.device_id;
		listed.device_key = self.device_key;
		listed.coordination_key = self.coordination_key;
		listed.signature = self.signature;
		listed.addresses = self.addresses;
		node::announce_computer(session_post(), listed);
	};
	environment.list = []() {
		std::vector<Computer> computers;
		for (const node::ListedComputer &listed : node::list_computers(session_post())) {
			Computer computer;
			computer.seat_id = listed.seat_id;
			computer.role = listed.role;
			computer.device_id = listed.device_id;
			computer.device_key = listed.device_key;
			computer.coordination_key = listed.coordination_key;
			computer.signature = listed.signature;
			computer.addresses = listed.addresses;
			computer.announced_at = listed.announced_at;
			computers.push_back(std::move(computer));
		}
		return computers;
	};
	environment.send = [](const std::string &to_device, const std::string &kind, const std::string &value) {
		node::send_pairing(session_post(), platform::current_device_id(), to_device, kind, value);
	};
	environment.inbox = []() {
		std::vector<Message> messages;
		for (const node::PairingMessage &message : node::pairing_inbox(session_post(), platform::current_device_id())) {
			messages.push_back(Message{message.from_device, message.kind, message.value});
		}
		return messages;
	};
	// The lawyer's decisions are this computer's own (Tool/DeviceTrust), as for
	// a computer confirmed in a Matter.
	environment.is_confirmed = [](const std::string &actor_id, const std::string &device_key) {
		return tools::counts_as_theirs(actor_id, device_key);
	};
	environment.is_refused = [](const std::string &device_key) { return tools::is_refused_key(device_key); };
	environment.confirm = [](const std::string &actor_id, const std::string &device_key) {
		tools::confirm_device(actor_id, device_key, platform::resolve_actor_identity().actor_id);
	};
	environment.refuse = [](const std::string &actor_id, const std::string &device_key) {
		tools::revoke_device(actor_id, device_key, platform::resolve_actor_identity().actor_id);
	};
	environment.records = []() { return platform::computer_records_directory() / "Firm Computers.json"; };
	environment.now = []() { return workspaces::current_timestamp(); };
	environment.code_of = [](const std::string &device_key) { return platform::device_key_code(device_key); };
	// The window reads the firm's computers again; a computer checked or
	// refused changes who the coordination carries the firm's record to.
	environment.changed = []() {
		platform::events::publish("firmComputers.changed", nlohmann::json::object());
		coordination::wake();
	};
	return environment;
}

std::optional<std::string> administrator_computer_key(const std::string &device_id) {
	if (device_id.empty() == true) return std::nullopt;
	for (const Computer &computer : checked()) {
		if (computer.device_id != device_id || computer.device_key.empty() == true) continue;
		try {
			for (const node::FirmSeat &seat : node::list_seats(session_post()).seats)
				if (seat.seat_id == computer.seat_id) return seat.role == "admin" ? std::optional<std::string>(computer.device_key) : std::nullopt;
		} catch (const std::exception &) {
		}
		return std::nullopt;
	}
	return std::nullopt;
}

std::vector<coordination::CheckedFirmComputer> checked_for_coordination() {
	std::vector<coordination::CheckedFirmComputer> found;
	for (const Computer &computer : checked()) {
		found.push_back(coordination::CheckedFirmComputer{"seat:" + computer.seat_id, computer.device_id, computer.device_key,
			computer.coordination_key, computer.addresses});
	}
	return found;
}

} // namespace singularity::matter::firm_computers
