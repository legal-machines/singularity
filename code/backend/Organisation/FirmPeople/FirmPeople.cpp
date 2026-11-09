#include "Organisation/FirmPeople/FirmPeople.hpp"

#include "Organisation/Types/FirmOffice.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace singularity::organisation {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr int kFormatVersion = 1;
constexpr std::size_t kMostWords = 160;
constexpr std::size_t kMostAbout = 600;

std::mutex &place_mutex() {
	static std::mutex mutex;
	return mutex;
}

ProfilePlace &installed() {
	static ProfilePlace place;
	return place;
}

ProfilePlace place_now() {
	const std::lock_guard<std::mutex> lock(place_mutex());
	return installed();
}

std::string trimmed(std::string value) {
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
	std::size_t start = 0;
	while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
	return value.substr(start);
}

// One line, no control characters: what a name or a title may hold.
bool is_one_line(const std::string &value) {
	return std::none_of(value.begin(), value.end(), [](char character) {
		const auto code = static_cast<unsigned char>(character);
		return code < 0x20 || code == 0x7f;
	});
}

// A seat's id as Legal Machines writes it: small letters and digits joined
// by hyphens, a folder's name as it stands.
bool is_seat_id(const std::string &value) {
	if (value.empty() || value.size() > 64 || value.front() == '-' || value.back() == '-') return false;
	bool hasHyphen = false;
	for (const char character : value) {
		if (character == '-') {
			hasHyphen = true;
			continue;
		}
		if (!(character >= 'a' && character <= 'z') && !(character >= '0' && character <= '9')) return false;
	}
	return hasHyphen && value.find("--") == std::string::npos;
}

fs::path people_folder(const fs::path &firm) {
	return firm / ".lm" / "People";
}

std::optional<SeatProfile> person_from(const Json &value) {
	if (!value.is_object()) return std::nullopt;
	const auto text = [&value](const char *key) {
		const auto found = value.find(key);
		return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
	};
	SeatProfile person;
	person.seat_id = text("seatId");
	person.name = text("name");
	person.rank = text("rank");
	person.title = text("title");
	person.practice = text("practice");
	person.about = text("about");
	person.photo = text("photo");
	person.at = text("at");
	person.written_by = text("writtenBy");
	person.state = text("state");
	if (person.state != "archived" && person.state != "removed") person.state.clear();
	if (!is_seat_id(person.seat_id) || person.at.empty()) return std::nullopt;
	return person;
}

Json json_of(const SeatProfile &person) {
	return Json{{"formatVersion", kFormatVersion}, {"seatId", person.seat_id}, {"name", person.name}, {"rank", person.rank},
		{"title", person.title}, {"practice", person.practice}, {"about", person.about}, {"photo", person.photo}, {"at", person.at},
		{"writtenBy", person.written_by}, {"state", person.state}};
}

// The newest of what the firm's computers wrote of one seat; on the same
// second, the one written by the computer whose id comes first.
bool is_newer(const SeatProfile &candidate, const SeatProfile &kept) {
	if (candidate.at != kept.at) return candidate.at > kept.at;
	return candidate.written_by < kept.written_by;
}

std::map<std::string, SeatProfile> read_all(const fs::path &firm) {
	std::map<std::string, SeatProfile> newest;
	std::error_code error;
	const fs::path root = people_folder(firm);
	if (!fs::is_directory(root, error)) return newest;
	for (fs::directory_iterator seat(root, error), end; !error && seat != end; seat.increment(error)) {
		if (!seat->is_directory()) continue;
		const std::string seatId = seat->path().filename().u8string();
		if (!is_seat_id(seatId)) continue;
		std::error_code inner;
		for (fs::directory_iterator file(seat->path(), inner), last; !inner && file != last; file.increment(inner)) {
			if (file->path().extension() != ".json") continue;
			std::ifstream in(file->path(), std::ios::binary);
			const std::optional<SeatProfile> person = person_from(Json::parse(in, nullptr, false));
			if (!person || person->seat_id != seatId) continue;
			const auto kept = newest.find(seatId);
			if (kept == newest.end() || is_newer(*person, kept->second)) newest[seatId] = *person;
		}
	}
	return newest;
}

} // namespace

void install_profile_place(ProfilePlace place) {
	const std::lock_guard<std::mutex> lock(place_mutex());
	installed() = std::move(place);
}

std::vector<SeatProfile> seat_profiles() {
	const ProfilePlace place = place_now();
	if (!place.firm) return {};
	const std::optional<fs::path> firm = place.firm();
	if (!firm) return {};
	std::vector<SeatProfile> people;
	for (auto &[seat, person] : read_all(*firm)) people.push_back(std::move(person));
	return people;
}

std::optional<SeatProfile> seat_profile(const std::string &seat_id) {
	for (SeatProfile &person : seat_profiles()) {
		if (person.seat_id == seat_id) return std::move(person);
	}
	return std::nullopt;
}

SeatProfile save_seat_profile(SeatProfile person) {
	person.seat_id = trimmed(person.seat_id);
	std::transform(person.seat_id.begin(), person.seat_id.end(), person.seat_id.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	person.name = trimmed(person.name);
	person.rank = trimmed(person.rank);
	person.title = trimmed(person.title);
	person.practice = trimmed(person.practice);
	person.about = trimmed(person.about);
	if (!is_seat_id(person.seat_id)) throw std::invalid_argument("Choose the seat.");
	if (!person.state.empty() && person.state != "archived" && person.state != "removed") {
		throw std::invalid_argument("A colleague is at work, archived or removed.");
	}
	if (person.name.empty()) throw std::invalid_argument("Enter their name.");
	if (person.name.size() > 120 || !is_one_line(person.name)) throw std::invalid_argument("A name is one line of at most 120 characters.");
	if (!person.rank.empty()) {
		const std::vector<std::string> &positions = firm_positions();
		if (std::find(positions.begin(), positions.end(), person.rank) == positions.end()) {
			throw std::invalid_argument("Choose a role from the firm's positions.");
		}
	}
	if (person.title.size() > kMostWords || !is_one_line(person.title)) throw std::invalid_argument("A position is one line of at most 160 characters.");
	if (person.practice.size() > kMostWords || !is_one_line(person.practice)) throw std::invalid_argument("A practice is one line of at most 160 characters.");
	if (person.about.size() > kMostAbout) throw std::invalid_argument("What they do is at most 600 characters.");
	const ProfilePlace place = place_now();
	if (!person.photo.empty() && (!place.is_photo || !place.is_photo(person.photo))) {
		throw std::invalid_argument("This photo can't be kept. Choose a JPEG or PNG picture.");
	}
	const std::optional<fs::path> firm = place.firm ? place.firm() : std::nullopt;
	if (!firm) throw std::runtime_error("This computer holds no record of the firm yet.");
	person.written_by = place.device_id ? place.device_id() : std::string();
	person.at = place.now ? place.now() : std::string();
	if (person.written_by.empty() || person.at.empty()) throw std::runtime_error("This computer can't say who wrote it or when.");
	// Each computer writes its own file for the seat: none overwrites another's.
	std::string file = person.written_by;
	for (char &character : file) {
		const auto code = static_cast<unsigned char>(character);
		if (!std::isalnum(code) && character != '_' && character != '-') character = '_';
	}
	const fs::path folder = people_folder(*firm) / fs::u8path(person.seat_id);
	fs::create_directories(folder);
	const fs::path path = folder / fs::u8path(file + ".json");
	const fs::path staged = platform::staged_sibling_path(path);
	platform::write_file_durably(staged, json_of(person).dump(2) + "\n");
	platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
	if (place.written) place.written(people_folder(*firm));
	return person;
}

namespace {

fs::path ai_setup_folder(const fs::path &firm) {
	return firm / ".lm" / "AI Setup";
}

// A role, a provider and a model id as AI Setup writes them: one line each,
// short, and the role and provider plain words.
bool is_plain_id(const std::string &value) {
	return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char character) {
		const auto code = static_cast<unsigned char>(character);
		return std::isalnum(code) || character == '_' || character == '-' || character == '.';
	});
}

std::optional<FirmAiSetup> setup_from(const Json &value) {
	if (!value.is_object()) return std::nullopt;
	FirmAiSetup setup;
	setup.at = value.value("at", std::string());
	setup.written_by = value.value("writtenBy", std::string());
	if (setup.at.empty() || setup.written_by.empty()) return std::nullopt;
	const auto roles = value.find("roles");
	if (roles == value.end() || !roles->is_array()) return std::nullopt;
	for (const Json &entry : *roles) {
		if (!entry.is_object()) continue;
		FirmModelRole role{entry.value("role", std::string()), entry.value("providerId", std::string()),
			entry.value("modelId", std::string())};
		if (!is_plain_id(role.role) || !is_plain_id(role.provider_id) || role.model_id.empty() || role.model_id.size() > 200
				|| !is_one_line(role.model_id)) {
			continue;
		}
		setup.roles.push_back(std::move(role));
	}
	return setup;
}

bool same_roles(const std::vector<FirmModelRole> &first, const std::vector<FirmModelRole> &second) {
	if (first.size() != second.size()) return false;
	for (const FirmModelRole &role : first) {
		const bool isThere = std::any_of(second.begin(), second.end(), [&role](const FirmModelRole &other) {
			return other.role == role.role && other.provider_id == role.provider_id && other.model_id == role.model_id;
		});
		if (!isThere) return false;
	}
	return true;
}

} // namespace

std::optional<FirmAiSetup> firm_ai_setup() {
	const ProfilePlace place = place_now();
	const std::optional<fs::path> firm = place.firm ? place.firm() : std::nullopt;
	if (!firm) return std::nullopt;
	std::optional<FirmAiSetup> newest;
	std::error_code error;
	for (fs::directory_iterator file(ai_setup_folder(*firm), error), end; !error && file != end; file.increment(error)) {
		if (file->path().extension() != ".json") continue;
		std::ifstream in(file->path(), std::ios::binary);
		const std::optional<FirmAiSetup> setup = setup_from(Json::parse(in, nullptr, false));
		if (!setup) continue;
		if (!newest || setup->at > newest->at || (setup->at == newest->at && setup->written_by < newest->written_by)) newest = setup;
	}
	return newest;
}

bool save_firm_ai_setup(const std::vector<FirmModelRole> &roles) {
	std::vector<FirmModelRole> kept;
	for (const FirmModelRole &role : roles) {
		if (is_plain_id(role.role) && is_plain_id(role.provider_id) && !role.model_id.empty() && role.model_id.size() <= 200
				&& is_one_line(role.model_id)) {
			kept.push_back(role);
		}
	}
	const ProfilePlace place = place_now();
	const std::optional<fs::path> firm = place.firm ? place.firm() : std::nullopt;
	if (!firm) throw std::runtime_error("This computer holds no record of the firm yet.");
	const std::optional<FirmAiSetup> known = firm_ai_setup();
	if (known && same_roles(known->roles, kept)) return false;
	FirmAiSetup setup{kept, place.now ? place.now() : std::string(), place.device_id ? place.device_id() : std::string()};
	if (setup.written_by.empty() || setup.at.empty()) throw std::runtime_error("This computer can't say who wrote it or when.");
	Json roleList = Json::array();
	for (const FirmModelRole &role : setup.roles) {
		roleList.push_back(Json{{"role", role.role}, {"providerId", role.provider_id}, {"modelId", role.model_id}});
	}
	std::string file = setup.written_by;
	for (char &character : file) {
		const auto code = static_cast<unsigned char>(character);
		if (!std::isalnum(code) && character != '_' && character != '-') character = '_';
	}
	const fs::path folder = ai_setup_folder(*firm);
	fs::create_directories(folder);
	const fs::path path = folder / fs::u8path(file + ".json");
	const fs::path staged = platform::staged_sibling_path(path);
	platform::write_file_durably(staged, Json{{"formatVersion", kFormatVersion}, {"roles", roleList}, {"at", setup.at},
		{"writtenBy", setup.written_by}}.dump(2) + "\n");
	platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
	if (place.written) place.written(folder);
	return true;
}

std::vector<SeatComputer> computers_of_removed(const std::set<std::string> &removed_seats, const std::string &own_seat,
	const std::vector<SeatComputer> &known) {
	std::vector<SeatComputer> refused;
	std::set<std::string> keys;
	for (const SeatComputer &computer : known) {
		if (computer.device_key.empty() || computer.seat_id == own_seat || removed_seats.count(computer.seat_id) == 0) continue;
		if (keys.insert(computer.device_key).second) refused.push_back(computer);
	}
	return refused;
}

} // namespace singularity::organisation
