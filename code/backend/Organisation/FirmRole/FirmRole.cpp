#include "Organisation/FirmRole/FirmRole.hpp"

#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>

#include <nlohmann/json.hpp>

namespace singularity::organisation {
namespace {

struct Recorded {
	std::string seat_id;
	FirmRole role = FirmRole::Administrator;
};

std::filesystem::path role_path() {
	return platform::app_settings_directory() / "Member Role.json";
}

std::mutex &role_mutex() {
	static std::mutex mutex;
	return mutex;
}

// What this computer recorded, or nothing where it recorded nothing it can
// read: a damaged record grants nothing it did not say.
std::optional<Recorded> read_recorded() {
	std::ifstream file(role_path(), std::ios::binary);
	if (!file) return std::nullopt;
	const nlohmann::json value = nlohmann::json::parse(file, nullptr, false);
	if (!value.is_object() || !value.contains("role") || !value.at("role").is_string()) return std::nullopt;
	const std::string role = value.at("role").get<std::string>();
	if (role != "administrator" && role != "member") return std::nullopt;
	Recorded recorded;
	recorded.seat_id = value.contains("seatId") && value.at("seatId").is_string() ? value.at("seatId").get<std::string>() : std::string();
	recorded.role = role == "member" ? FirmRole::Member : FirmRole::Administrator;
	return recorded;
}

void write_recorded(const Recorded &recorded) {
	const std::filesystem::path path = role_path();
	std::filesystem::create_directories(path.parent_path());
	const std::filesystem::path staged = platform::staged_sibling_path(path);
	platform::write_file_durably(staged,
		nlohmann::json{{"seatId", recorded.seat_id}, {"role", to_string(recorded.role)}}.dump(2) + "\n");
	platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
}

// The role a whole-product test asks for, in a Debug build alone.
std::optional<FirmRole> emulated_role() {
#if defined(SINGULARITY_ACCOUNT_EMULATION)
	const char *mode = std::getenv("NATIVE_APP_EMULATE_ACCOUNT");
	const std::string chosen = mode != nullptr ? mode : "";
	if (chosen == "register") return FirmRole::Administrator;
	if (chosen == "join") return FirmRole::Member;
#endif
	return std::nullopt;
}

std::string signed_in_seat() {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const platform::AppSettings settings = platform::load_app_settings();
	return settings.account.has_value() ? settings.account->seat_id : std::string();
}

} // namespace

FirmRole firm_role() {
	if (const std::optional<FirmRole> emulated = emulated_role()) return *emulated;
	const std::string seat = signed_in_seat();
	const std::lock_guard<std::mutex> lock(role_mutex());
	const std::optional<Recorded> recorded = read_recorded();
	if (!recorded.has_value()) {
		// Signed in before roles were recorded: this computer's own seat keeps
		// the administrator's rights it always had, from now on by record.
		if (!seat.empty()) write_recorded(Recorded{seat, FirmRole::Administrator});
		return FirmRole::Administrator;
	}
	if (seat.empty() || recorded->seat_id.empty() || recorded->seat_id == seat) return recorded->role;
	// Another seat than the one recorded is signed in here: it was recorded on
	// its own sign-in, unless it came another way; it starts as a member.
	return FirmRole::Member;
}

void record_role(const std::string &seat_id, FirmRole role) {
	if (seat_id.empty()) return;
	const std::lock_guard<std::mutex> lock(role_mutex());
	write_recorded(Recorded{seat_id, role});
}

bool is_administrator() {
	return firm_role() == FirmRole::Administrator;
}

void record_joined(const std::string &seat_id) {
	const std::lock_guard<std::mutex> lock(role_mutex());
	write_recorded(Recorded{seat_id, FirmRole::Member});
}

void record_registered(const std::string &seat_id) {
	const std::lock_guard<std::mutex> lock(role_mutex());
	write_recorded(Recorded{seat_id, FirmRole::Administrator});
}

void record_signed_in(const std::string &seat_id) {
	const std::lock_guard<std::mutex> lock(role_mutex());
	const std::optional<Recorded> recorded = read_recorded();
	if (recorded.has_value() && recorded->seat_id == seat_id) return;
	write_recorded(Recorded{seat_id, FirmRole::Member});
}

std::string to_string(FirmRole role) {
	return role == FirmRole::Member ? "member" : "administrator";
}

} // namespace singularity::organisation
