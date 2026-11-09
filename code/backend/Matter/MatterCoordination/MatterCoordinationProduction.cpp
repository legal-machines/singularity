#include "Matter/MatterCoordination/MatterCoordination.hpp"

#include "Agent/AgentCard/AgentCard.hpp"
#include "Agent/AgentStore/AgentStore.hpp"

#include "Matter/CoordinationTransport/DirectTransport.hpp"
#include "Matter/CoordinationTransport/RelayClient.hpp"
#include "Matter/FirmComputers/FirmComputersProduction.hpp"
#include "Matter/CoordinationTransport/FolderTransport.hpp"
#include "Matter/SharedMatter/SharedMatter.hpp"
#include "LocalModel/ModelPackageManager/ModelPackageManager.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Node/LegalMachinesConnection/LegalMachinesConnection.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"
#include "Tool/DeviceTrust/DeviceTrust.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>

namespace singularity::matter::coordination {

std::string signed_in_organisation_id() {
	static std::mutex mutex;
	static std::string kept;
	static std::chrono::steady_clock::time_point read_at {};
	const std::lock_guard<std::mutex> lock(mutex);
	const auto now = std::chrono::steady_clock::now();
	if (read_at == std::chrono::steady_clock::time_point {} || now - read_at >= std::chrono::minutes(1)) {
		const platform::AppSettings settings = platform::load_app_settings();
		kept = settings.account.has_value() == true ? settings.account->organisation_id : std::string();
		read_at = now;
	}
	return kept;
}

Environment production_environment() {
	Environment environment;
	environment.records = platform::computer_records_directory() / "Coordination.json";
	environment.device_id = []() { return platform::current_device_id(); };
	environment.device_key = []() { return platform::device_public_key(); };
	environment.sign = [](const std::string &message) { return platform::sign_with_device_key(message); };
	environment.verify = [](const std::string &device_key, const std::string &message, const std::string &signature) {
		return platform::verify_device_signature(device_key, message, signature);
	};
	environment.code_of = [](const std::string &device_key) { return platform::device_key_code(device_key); };
	environment.actor_id = []() { return platform::resolve_actor_identity().actor_id; };
	environment.person_name = []() { return platform::resolve_actor_identity().display_name.value_or(std::string()); };
	// The keys - this computer's own for coordination and each Matter's - are in
	// the credential store, beside the providers' keys: never in a setting, a
	// Matter, a log or the firm's folder.
	environment.secret = [](const std::string &name) -> std::optional<std::string> {
		try {
			return platform::get_provider_credential(name);
		} catch (const std::exception &) {
			return std::nullopt;
		}
	};
	environment.keep_secret = [](const std::string &name, const std::string &value) { platform::set_provider_credential(name, value); };
	// The direct link between the firm's computers, and the firm's own folder
	// where the lawyer chose one. The engine asks for them only once this
	// computer shares or joins a Matter: nothing binds a network port for a
	// lawyer who never did.
	environment.transports = [](const std::filesystem::path &firm_folder, const seal::ComputerKeys &keys) {
		std::vector<std::shared_ptr<transport::Transport>> carriers;
		try {
			// Legal Machines' relay, for the firm's computers no direct way
			// reaches: opened with the seat's session and this computer's
			// coordination key, made once with the program's link.
			std::shared_ptr<transport::link::Relay> relay;
			if (!transport::DirectTransport::existing()) {
				transport::link::RelayClient::Options relayOptions;
				relayOptions.url = std::string(node::kLegalMachinesApiBase) + "/relay";
				const std::string coordinationKey = seal::key_text(keys.public_key);
				relayOptions.headers = [coordinationKey]() {
					std::vector<platform::HttpHeader> headers = local_models::production_model_package_manager().session_headers();
					if (headers.empty() == false) headers.push_back({"X-LM-Coordination-Key", coordinationKey});
					return headers;
				};
				relay = std::make_shared<transport::link::RelayClient>(std::move(relayOptions));
			}
			const std::shared_ptr<transport::DirectTransport> direct =
				transport::DirectTransport::shared(transport::noise::key_pair_from_secret(keys.secret), relay);
			// What comes over the link is taken at once, not at the next round:
			// a message on the office network shows as a socket that pushes
			// would show it.
			direct->on_arrival([]() { wake(); });
			carriers.push_back(direct);
		} catch (const std::exception &) {
			// No network socket on this computer now: the firm's folder, if any, carries all.
		}
		if (firm_folder.empty() == false) {
			carriers.push_back(std::make_shared<transport::FolderTransport>(
				firm_folder, platform::computer_records_directory() / "Coordination Folder"));
		}
		return carriers;
	};
	environment.now = []() { return workspaces::current_timestamp(); };
	// The organisation this computer's member signed in to, by Legal Machines'
	// id: whose firm's record it carries, and the one firm its computers are
	// of. Read from the settings at most once a minute.
	environment.organisation_id = []() { return signed_in_organisation_id(); };
	// A Matter sealed for another organisation is not carried while this
	// computer is signed in to a different one.
	environment.is_other_organisations = [](const std::filesystem::path &folder, const std::string &matter_id) {
		return matter::matter_seal_state(folder, matter_id, signed_in_organisation_id()) == matter::MatterSealState::OtherOrganisation;
	};
	// Where the direct link may be reached, for this computer's address cards;
	// what another computer's checked card says, for the link to reach it.
	environment.candidates = []() {
		const std::shared_ptr<transport::DirectTransport> direct = transport::DirectTransport::existing();
		return direct ? direct->candidates() : std::vector<std::string>();
	};
	// The computers of a Matter the direct link holds a live encrypted session
	// with now; none before this computer shares or joins a Matter.
	environment.connected = [](const std::string &matter_id) {
		const std::shared_ptr<transport::DirectTransport> direct = transport::DirectTransport::existing();
		return direct ? direct->connected_devices(matter_id) : std::vector<std::string>();
	};
	// What the direct link carries of a Matter to each computer, and that
	// computer confirmed; and what came of it that waits for its key.
	environment.sending = [](const std::string &matter_id) {
		std::vector<Transfer> going;
		const std::shared_ptr<transport::DirectTransport> direct = transport::DirectTransport::existing();
		if (!direct) return going;
		for (const transport::DirectTransport::Flow &flow : direct->flows(matter_id)) {
			Transfer transfer;
			transfer.device_id = flow.device_id;
			transfer.files = flow.messages;
			transfer.files_here = flow.confirmed_messages;
			transfer.bytes = flow.bytes;
			transfer.bytes_here = flow.confirmed_bytes;
			transfer.reachable = flow.reachable;
			going.push_back(std::move(transfer));
		}
		return going;
	};
	environment.sealed_here = [](const std::string &matter_id) -> std::uint64_t {
		const std::shared_ptr<transport::DirectTransport> direct = transport::DirectTransport::existing();
		return direct ? direct->waiting_bytes(matter_id) : 0;
	};
	environment.learned = [](const seal::AddressCard &card) {
		const std::shared_ptr<transport::DirectTransport> direct = transport::DirectTransport::existing();
		const std::optional<std::array<std::uint8_t, seal::kKeyBytes>> key = seal::key_from_text(card.coordination_key);
		if (!direct || key.has_value() == false) return;
		direct->learn(card.matter_id, transport::DirectTransport::Peer{card.device_id, *key, card.candidates});
	};
	// A computer of the Matter is known by the device key its member record
	// states, once the person here confirmed it as that member's.
	environment.confirmed_device_key = [](const std::filesystem::path &matter_folder,
			const std::string &device_id) -> std::optional<std::string> {
		for (const matter::SharedMember &member : matter::load_members(matter_folder)) {
			for (const matter::SharedDeviceKey &key : member.device_keys) {
				if (key.device_id == device_id && tools::counts_as_theirs(member.actor_id, key.public_key) == true) return key.public_key;
			}
		}
		return std::nullopt;
	};
	environment.refused_device_key = [](const std::string &device_key) { return tools::is_refused_key(device_key); };
	// A computer whose code the person here compared when taking the Matter
	// counts as its member's, once exactly one member's record states its key;
	// a decision the person already made about it stands.
	environment.compared_when_taken = [](const std::filesystem::path &matter_folder, const std::string &device_key) {
		std::vector<std::string> actors;
		for (const matter::SharedMember &member : matter::load_members(matter_folder)) {
			for (const matter::SharedDeviceKey &key : member.device_keys) {
				if (key.public_key == device_key) actors.push_back(member.actor_id);
			}
		}
		if (actors.size() != 1) return false;
		if (tools::device_trust(actors.front(), device_key) == tools::DeviceTrustState::Unconfirmed) {
			tools::confirm_device(actors.front(), device_key, platform::resolve_actor_identity().actor_id);
		}
		return tools::counts_as_theirs(actors.front(), device_key);
	};
	// Who a Matter's computer is, by its member records: a personal channel's
	// key goes only to a computer the person here confirmed as its member's.
	environment.confirmed_computer = [](const std::filesystem::path &matter_folder,
			const std::string &device_id) -> std::optional<ConfirmedComputer> {
		for (const matter::SharedMember &member : matter::load_members(matter_folder)) {
			for (const matter::SharedDeviceKey &key : member.device_keys) {
				if (key.device_id != device_id || tools::counts_as_theirs(member.actor_id, key.public_key) == false) continue;
				if (tools::is_refused_key(key.public_key) == true) return std::nullopt;
				return ConfirmedComputer{member.actor_id, key.public_key, member.organisation_id};
			}
		}
		return std::nullopt;
	};
	// The firm's computers a lawyer here checked by their number: the firm's,
	// for its record and the members' personal channels. Never asks Legal
	// Machines: the listing as the background last read it.
	environment.checked_firm_computers = []() { return firm_computers::checked_for_coordination(); };
	// The firm's model accounts: each connected here, with its key from the
	// credential store, for the firm's computers checked here; and one they
	// gave, kept in this computer's settings and credential store, the key never
	// anywhere else.
	environment.accounts_here = []() {
		std::vector<FirmAccount> accounts;
		platform::AppSettings settings;
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			settings = platform::load_app_settings();
		}
		for (const platform::ConnectedProvider &provider : settings.local_node_setup.connected_providers) {
			std::optional<std::string> credential;
			try {
				credential = platform::get_provider_credential(provider.id);
			} catch (const std::exception &) {
			}
			if (credential.has_value() == false || credential->empty() == true) continue;
			accounts.push_back(FirmAccount{provider.id, platform::connected_provider_json(provider).dump(), *credential});
		}
		return accounts;
	};
	environment.keep_account = [](const FirmAccount &account, bool removed) {
		// An account the firm's other computer sends is a provider's: one named
		// as Singularity's own secret - this computer's coordination key, a
		// Matter's key - is never written or removed (Cursor's bug hunt, 2026-11-08).
		if (platform::is_reserved_credential_name(account.provider_id)) return;
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		platform::AppSettings settings = platform::load_app_settings();
		auto &providers = settings.local_node_setup.connected_providers;
		if (removed == true) {
			providers.erase(std::remove_if(providers.begin(), providers.end(),
				[&account](const platform::ConnectedProvider &provider) { return provider.id == account.provider_id; }), providers.end());
			platform::delete_provider_credential(account.provider_id);
			platform::save_app_settings(settings);
			return;
		}
		const nlohmann::json entry = nlohmann::json::parse(account.entry, nullptr, false);
		if (entry.is_object() == false) return;
		const platform::ConnectedProvider given = platform::connected_provider_of(entry);
		if (given.id != account.provider_id || account.credential.empty() == true) return;
		platform::set_provider_credential(account.provider_id, account.credential);
		providers.erase(std::remove_if(providers.begin(), providers.end(),
			[&account](const platform::ConnectedProvider &provider) { return provider.id == account.provider_id; }), providers.end());
		providers.push_back(given);
		platform::save_app_settings(settings);
	};
	// An AI colleague's card as the computer that runs it signed it, and the
	// colleagues this computer runs.
	environment.agent_host_claim = [](const std::filesystem::path &matter_folder,
			const std::string &agent_id) -> std::optional<AgentHostClaim> {
		for (const agents::AgentCard &card : agents::load_agent_cards(matter_folder)) {
			if (card.agent_id != agent_id || card.signature.empty() == true) continue;
			return AgentHostClaim{card.host_device_id, card.host_device_key, card.id_nonce, card.responsible_actor_id, card.counter,
				card.signature};
		}
		return std::nullopt;
	};
	environment.hosted_agents = []() {
		std::vector<std::string> ids;
		try {
			for (const agents::Agent &agent : agents::load_agents()) {
				// A copy of the firm's colleague speaks for it in no channel: the
				// computer that runs it does.
				if (agent.status != agents::AgentStatus::Draft && agent.is_firm_copy == false) ids.push_back(agent.id);
			}
		} catch (const std::exception &) {
		}
		return ids;
	};
	// An invitation given lately opens the direct link's lobby; a computer the
	// lawyer refused is spoken with no more.
	environment.open_lobby = [](const std::string &matter_id, std::int64_t until_seconds) {
		if (const std::shared_ptr<transport::DirectTransport> direct = transport::DirectTransport::existing()) {
			direct->open_lobby(matter_id, until_seconds);
		}
	};
	environment.refused = [](const std::string &coordination_key) {
		const std::shared_ptr<transport::DirectTransport> direct = transport::DirectTransport::existing();
		const std::optional<std::array<std::uint8_t, seal::kKeyBytes>> key = seal::key_from_text(coordination_key);
		if (direct && key.has_value() == true) direct->refuse(*key);
	};
	return environment;
}

} // namespace singularity::matter::coordination
