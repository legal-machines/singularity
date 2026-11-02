#include "Tool/DeviceTrust/DeviceTrust.hpp"

#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <iterator>
#include <mutex>

namespace singularity::tools {
namespace {

using Json = nlohmann::json;

std::mutex &trust_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::atomic<std::uint64_t> &decisions_made() {
	static std::atomic<std::uint64_t> count{0};
	return count;
}

std::filesystem::path trust_path() {
	return platform::computer_records_directory() / "Trusted Devices.json";
}

// A store that cannot be read holds no confirmation: every colleague's key
// reads as not confirmed until the person confirms it again.
Json read_store() {
	std::ifstream input(trust_path(), std::ios::binary);
	if (!input) return Json{{"format", "singularity-trusted-devices"}, {"devices", Json::array()}};
	const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	Json store = Json::parse(bytes, nullptr, false);
	if (!store.is_object() || !store.contains("devices") || !store.at("devices").is_array())
		return Json{{"format", "singularity-trusted-devices"}, {"devices", Json::array()}};
	return store;
}

void write_store(const Json &store) {
	const std::filesystem::path path = trust_path();
	std::filesystem::create_directories(path.parent_path());
	const std::filesystem::path staged = platform::staged_sibling_path(path);
	try {
		platform::write_file_durably(staged, store.dump());
		platform::restrict_to_owner(staged);
		platform::commit_staged_file(staged, path, std::filesystem::exists(path)
			? platform::StagedCommitMode::ReplaceExisting : platform::StagedCommitMode::CreateExclusive, false);
	} catch (...) {
		platform::discard_staged_file(staged);
		throw;
	}
}

Json *entry_of(Json &store, const std::string &actor_id, const std::string &public_key) {
	for (Json &entry : store.at("devices"))
		if (entry.is_object() && entry.value("actorId", std::string()) == actor_id
				&& entry.value("publicKey", std::string()) == public_key)
			return &entry;
	return nullptr;
}

bool is_own(const std::string &actor_id, const std::string &public_key) {
	return actor_id == platform::resolve_actor_identity().actor_id && public_key == platform::device_public_key();
}

void decide(const std::string &actor_id, const std::string &public_key, const char *state, const std::string &decided_by) {
	if (actor_id.empty() || public_key.empty() || platform::device_key_code(public_key).empty())
		throw std::runtime_error("This is not a computer Singularity can name.");
	if (is_own(actor_id, public_key)) return;
	const std::lock_guard<std::mutex> lock(trust_mutex());
	Json store = read_store();
	Json *entry = entry_of(store, actor_id, public_key);
	if (entry != nullptr && entry->value("state", std::string()) == "revoked") return;
	if (entry == nullptr) {
		store.at("devices").push_back(Json::object());
		entry = &store.at("devices").back();
	}
	*entry = Json{{"actorId", actor_id}, {"publicKey", public_key}, {"state", state},
		{"decidedAt", workspaces::current_timestamp()}, {"decidedBy", decided_by}};
	write_store(store);
	decisions_made().fetch_add(1);
}

} // namespace

std::uint64_t device_trust_decisions() {
	return decisions_made().load();
}

const char *to_string(DeviceTrustState state) {
	switch (state) {
		case DeviceTrustState::ThisComputer: return "this_computer";
		case DeviceTrustState::Confirmed: return "confirmed";
		case DeviceTrustState::Unconfirmed: return "unconfirmed";
		case DeviceTrustState::Revoked: return "revoked";
	}
	return "unconfirmed";
}

DeviceTrustState device_trust(const std::string &actor_id, const std::string &public_key) {
	if (actor_id.empty() || public_key.empty()) return DeviceTrustState::Unconfirmed;
	if (is_own(actor_id, public_key)) return DeviceTrustState::ThisComputer;
	const std::lock_guard<std::mutex> lock(trust_mutex());
	Json store = read_store();
	const Json *entry = entry_of(store, actor_id, public_key);
	if (entry == nullptr) return DeviceTrustState::Unconfirmed;
	const std::string state = entry->value("state", std::string());
	if (state == "revoked") return DeviceTrustState::Revoked;
	if (state == "confirmed") return DeviceTrustState::Confirmed;
	return DeviceTrustState::Unconfirmed;
}

void confirm_device(const std::string &actor_id, const std::string &public_key, const std::string &decided_by) {
	decide(actor_id, public_key, "confirmed", decided_by);
}

void revoke_device(const std::string &actor_id, const std::string &public_key, const std::string &decided_by) {
	decide(actor_id, public_key, "revoked", decided_by);
}

bool counts_as_theirs(const std::string &actor_id, const std::string &public_key) {
	const DeviceTrustState state = device_trust(actor_id, public_key);
	return state == DeviceTrustState::ThisComputer || state == DeviceTrustState::Confirmed;
}

bool is_refused_key(const std::string &public_key) {
	if (public_key.empty()) return false;
	const std::lock_guard<std::mutex> lock(trust_mutex());
	const Json store = read_store();
	for (const Json &entry : store.at("devices"))
		if (entry.is_object() && entry.value("publicKey", std::string()) == public_key && entry.value("state", std::string()) == "revoked")
			return true;
	return false;
}

} // namespace singularity::tools
