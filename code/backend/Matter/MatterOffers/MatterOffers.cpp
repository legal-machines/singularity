#include "Matter/MatterOffers/MatterOffers.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <stdexcept>

namespace singularity::matter::offers {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr int kFormatVersion = 1;

// A Matter's id as a file's name: letters, digits, "_" and "-" only.
bool is_plain_id(const std::string &id) {
	if (id.empty() == true || id.size() > 128) return false;
	for (const char letter : id) {
		if (std::isalnum(static_cast<unsigned char>(letter)) == 0 && letter != '_' && letter != '-') return false;
	}
	return true;
}

void write_json(const fs::path &path, const Json &value) {
	std::error_code error;
	fs::create_directories(path.parent_path(), error);
	const fs::path staged = platform::staged_sibling_path(path);
	platform::write_file_durably(staged, value.dump(2) + "\n");
	platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
}

Json read_json(const fs::path &path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) return Json();
	return Json::parse(file, nullptr, false);
}

const char *answer_word(Answer answer) {
	switch (answer) {
		case Answer::Accepted: return "accepted";
		case Answer::Declined: return "declined";
		case Answer::None: break;
	}
	return "";
}

fs::path answers_file(const fs::path &records) {
	return records / "Matter Offers.json";
}

} // namespace

std::string signed_text(const Offer &offer) {
	return "singularity-offer-v1\n" + offer.matter_id + "\n" + offer.name + "\n" + std::to_string(offer.files) + "\n"
		+ std::to_string(offer.bytes) + "\n" + offer.from_actor + "\n" + offer.to_actor + "\n" + offer.from_device + "\n"
		+ offer.from_device_key + "\n" + offer.invitation + "\n" + offer.at;
}

std::string offer_path(const std::string &matter_id) {
	return ".lm/Offers/" + matter_id + ".json";
}

void write_offer(const fs::path &channel_folder, const Offer &offer) {
	if (is_plain_id(offer.matter_id) == false) throw std::invalid_argument("That is not a Matter's id.");
	write_json(channel_folder / fs::u8path(offer_path(offer.matter_id)),
		Json{{"formatVersion", kFormatVersion}, {"matterId", offer.matter_id}, {"name", offer.name}, {"files", offer.files},
			{"bytes", offer.bytes}, {"fromActorId", offer.from_actor}, {"toActorId", offer.to_actor},
			{"fromDeviceId", offer.from_device}, {"fromDeviceKey", offer.from_device_key}, {"invitation", offer.invitation},
			{"at", offer.at}, {"signature", offer.signature}});
}

std::vector<Offer> read_offers(const fs::path &channel_folder) {
	std::vector<Offer> offers;
	std::error_code error;
	const fs::path folder = channel_folder / ".lm" / "Offers";
	if (fs::is_directory(folder, error) == false) return offers;
	for (fs::directory_iterator entry(folder, error), end; error.value() == 0 && entry != end; entry.increment(error)) {
		if (entry->path().extension() != ".json") continue;
		const Json value = read_json(entry->path());
		if (value.is_object() == false || value.value("formatVersion", 0) != kFormatVersion) continue;
		Offer offer;
		offer.matter_id = value.value("matterId", std::string());
		offer.name = value.value("name", std::string());
		offer.files = value.value("files", static_cast<std::uint64_t>(0));
		offer.bytes = value.value("bytes", static_cast<std::uint64_t>(0));
		offer.from_actor = value.value("fromActorId", std::string());
		offer.to_actor = value.value("toActorId", std::string());
		offer.from_device = value.value("fromDeviceId", std::string());
		offer.from_device_key = value.value("fromDeviceKey", std::string());
		offer.invitation = value.value("invitation", std::string());
		offer.at = value.value("at", std::string());
		offer.signature = value.value("signature", std::string());
		// A file named for another Matter than it states is no offer.
		if (is_plain_id(offer.matter_id) == false || entry->path().filename().u8string() != offer.matter_id + ".json") continue;
		offers.push_back(std::move(offer));
	}
	return offers;
}

bool holds(const Offer &offer, const std::string &me, const Giver &giver, const Verify &verify) {
	if (me.empty() == true || offer.to_actor != me || offer.from_actor != giver.actor_id) return false;
	if (offer.from_device.empty() == true || offer.from_device != giver.device_id || offer.from_device_key != giver.device_key) return false;
	if (offer.signature.empty() == true || !verify) return false;
	return verify(giver.device_key, signed_text(offer), offer.signature);
}

Answer answer_of(const fs::path &records, const std::string &matter_id, const std::string &from_device) {
	const Json value = read_json(answers_file(records));
	if (value.is_object() == false || value.contains("answers") == false || value.at("answers").is_object() == false) return Answer::None;
	const Json &answers = value.at("answers");
	const std::string key = matter_id + "\n" + from_device;
	if (answers.contains(key) == false || answers.at(key).is_object() == false) return Answer::None;
	const std::string word = answers.at(key).value("answer", std::string());
	if (word == "accepted") return Answer::Accepted;
	if (word == "declined") return Answer::Declined;
	return Answer::None;
}

void keep_answer(const fs::path &records, const std::string &matter_id, const std::string &from_device, Answer answer,
		const std::string &at) {
	Json value = read_json(answers_file(records));
	if (value.is_object() == false) value = Json::object();
	if (value.contains("answers") == false || value.at("answers").is_object() == false) value["answers"] = Json::object();
	const std::string key = matter_id + "\n" + from_device;
	if (answer == Answer::None) {
		value["answers"].erase(key);
	} else {
		value["answers"][key] = Json{{"answer", answer_word(answer)}, {"at", at}};
	}
	value["formatVersion"] = kFormatVersion;
	write_json(answers_file(records), value);
}

} // namespace singularity::matter::offers
