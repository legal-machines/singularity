#include "Matter/SharedMatter/SharedMatter.hpp"

#include "Document/DocumentVersionStore/DocumentVersionStore.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>

namespace singularity::matter {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMostPassages = 3;
constexpr std::size_t kMostPassageCharacters = 240;

std::filesystem::path lm_folder(const std::filesystem::path &project_folder) {
	return project_folder / ".lm";
}

bool has_lm_folder(const std::filesystem::path &project_folder) {
	std::error_code error;
	return std::filesystem::is_directory(lm_folder(project_folder), error);
}

std::optional<Json> read_json(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	if (!input.is_open()) return std::nullopt;
	const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	Json value = Json::parse(bytes, nullptr, false);
	if (value.is_discarded() || !value.is_object()) return std::nullopt;
	return value;
}

// A record written whole by one rename, so a computer reading the folder
// meanwhile sees the old record or the new one, never half of one.
void write_json(const std::filesystem::path &path, const Json &value, platform::StagedCommitMode mode) {
	platform::ensure_record_directory(path.parent_path());
	const std::filesystem::path staged = platform::staged_sibling_path(path);
	try {
		platform::write_file_durably(staged, value.dump(1, '\t'));
		platform::commit_staged_file(staged, path, mode, false);
	} catch (...) {
		platform::discard_staged_file(staged);
		throw;
	}
}

std::string text_of(const Json &value, const char *key) {
	const auto found = value.find(key);
	if (found == value.end() || !found->is_string()) return "";
	return found->get<std::string>();
}

int number_of(const Json &value, const char *key) {
	const auto found = value.find(key);
	if (found == value.end() || !found->is_number_integer()) return 0;
	return found->get<int>();
}

std::string member_file_name(const std::string &actor_id) {
	return "m_" + platform::sha256_hex_digest(actor_id).substr(0, 24) + ".json";
}

bool is_safe_record_id(const std::string &id) {
	if (id.size() != 27 || id.rfind("ow_", 0) != 0) return false;
	for (std::size_t index = 3; index < id.size(); ++index)
		if ((id[index] < '0' || id[index] > '9') && (id[index] < 'a' || id[index] > 'f')) return false;
	return true;
}

std::filesystem::path conflicts_folder(const std::filesystem::path &project_folder) {
	return lm_folder(project_folder) / "Conflicts";
}

std::string clipped(const std::string &text, std::size_t most) {
	std::size_t count = 0;
	for (std::size_t index = 0; index < text.size(); ++index) {
		if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) {
			if (count == most) return text.substr(0, index) + "…";
			++count;
		}
	}
	return text;
}

Json overwritten_to_json(const OverwrittenChange &change) {
	return Json{
		{"format", "singularity-overwritten-change"}, {"formatVersion", 1},
		{"id", change.id}, {"documentId", change.document_id}, {"name", change.name}, {"relativePath", change.relative_path},
		{"foundAt", change.found_at}, {"foundByDeviceId", change.found_by_device_id},
		{"writtenVersion", change.written_version}, {"writtenFingerprint", change.written_fingerprint},
		{"writtenByActorId", change.written_by_actor_id}, {"writtenOperationId", change.written_operation_id},
		{"baseVersion", change.base_version}, {"baseFingerprint", change.base_fingerprint},
		{"currentVersion", change.current_version}, {"currentFingerprint", change.current_fingerprint},
		{"missingPassages", change.missing_passages},
	};
}

std::optional<OverwrittenChange> overwritten_from_json(const Json &value) {
	OverwrittenChange change;
	change.id = text_of(value, "id");
	if (!is_safe_record_id(change.id)) return std::nullopt;
	change.document_id = text_of(value, "documentId");
	change.name = text_of(value, "name");
	change.relative_path = text_of(value, "relativePath");
	change.found_at = text_of(value, "foundAt");
	change.found_by_device_id = text_of(value, "foundByDeviceId");
	change.written_version = number_of(value, "writtenVersion");
	change.written_fingerprint = text_of(value, "writtenFingerprint");
	change.written_by_actor_id = text_of(value, "writtenByActorId");
	change.written_operation_id = text_of(value, "writtenOperationId");
	change.base_version = number_of(value, "baseVersion");
	change.base_fingerprint = text_of(value, "baseFingerprint");
	change.current_version = number_of(value, "currentVersion");
	change.current_fingerprint = text_of(value, "currentFingerprint");
	const auto passages = value.find("missingPassages");
	if (passages != value.end() && passages->is_array())
		for (const Json &passage : *passages)
			if (passage.is_string()) change.missing_passages.push_back(passage.get<std::string>());
	return change;
}

void read_resolution(const std::filesystem::path &project_folder, OverwrittenChange &change) {
	const std::optional<Json> resolution = read_json(conflicts_folder(project_folder) / (change.id + ".resolution.json"));
	if (!resolution) return;
	change.resolution = text_of(*resolution, "resolution");
	change.resolved_at = text_of(*resolution, "resolvedAt");
	change.resolved_by_actor_id = text_of(*resolution, "resolvedByActorId");
}

// The Singularity write whose result had this content, if any.
const documents::DocumentEvent *written_event_with(const matters::Matter &matter, const std::string &document_id,
		const std::string &fingerprint) {
	const documents::DocumentEvent *found = nullptr;
	for (const documents::DocumentEvent &event : matter.document_events)
		if (event.actor == "singularity" && event.document_id == document_id && event.fingerprint == fingerprint
				&& event.kind == documents::DocumentEventKind::Changed)
			found = &event;
	return found;
}

} // namespace

namespace {

// What a computer states about its own key: whose computer it says it is.
std::string device_statement_bytes(const std::string &actor_id, const std::string &device_id,
		const std::string &public_key) {
	return "singularity-signed-record-v1\nmember_device\n"
		+ Json{{"actorId", actor_id}, {"deviceId", device_id}, {"publicKey", public_key}}.dump();
}

} // namespace

namespace {

std::mutex &own_office_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::function<organisation::MemberOffice()> &own_office_source() {
	static std::function<organisation::MemberOffice()> source;
	return source;
}

} // namespace

namespace {

std::function<std::string()> &own_photo_source() {
	static std::function<std::string()> source;
	return source;
}

std::function<std::string()> &own_organisation_source() {
	static std::function<std::string()> source;
	return source;
}

} // namespace

void set_own_organisation_source(std::function<std::string()> source) {
	const std::lock_guard<std::mutex> lock(own_office_mutex());
	own_organisation_source() = std::move(source);
}

std::string own_organisation_id() {
	std::function<std::string()> source;
	{
		const std::lock_guard<std::mutex> lock(own_office_mutex());
		source = own_organisation_source();
	}
	if (!source) return std::string();
	try {
		return source();
	} catch (const std::exception &) {
		return std::string();
	}
}

void set_own_photo_source(std::function<std::string()> source) {
	const std::lock_guard<std::mutex> lock(own_office_mutex());
	own_photo_source() = std::move(source);
}

std::string own_photo() {
	std::function<std::string()> source;
	{
		const std::lock_guard<std::mutex> lock(own_office_mutex());
		source = own_photo_source();
	}
	if (!source) return std::string();
	try {
		const std::string photo = source();
		return is_member_photo(photo) ? photo : std::string();
	} catch (const std::exception &) {
		return std::string();
	}
}

namespace {

std::function<SealComputerTrust(const std::string &, const std::string &)> &seal_trust() {
	static std::function<SealComputerTrust(const std::string &, const std::string &)> trust;
	return trust;
}

std::function<bool(const std::string &, const std::filesystem::path &)> &seal_carrier() {
	static std::function<bool(const std::string &, const std::filesystem::path &)> carries;
	return carries;
}

// Whether the firm's exchange carries the Matter here; a carrier that cannot
// tell says no.
bool is_carried(const std::filesystem::path &project_folder, const std::string &matter_id) {
	std::function<bool(const std::string &, const std::filesystem::path &)> carries;
	{
		const std::lock_guard<std::mutex> lock(own_office_mutex());
		carries = seal_carrier();
	}
	try {
		return carries && carries(matter_id, project_folder);
	} catch (const std::exception &) {
		return false;
	}
}

// What a seal signs: the organisation, the Matter, and the computer and
// person that sealed it, when.
std::string seal_statement_bytes(const std::string &organisation_id, const std::string &matter_id, const std::string &actor_id,
		const std::string &device_id, const std::string &public_key, const std::string &sealed_at) {
	return "singularity-signed-record-v1\nmatter_seal\n"
		+ Json{{"organisationId", organisation_id}, {"matterId", matter_id}, {"actorId", actor_id}, {"deviceId", device_id},
			{"publicKey", public_key}, {"sealedAt", sealed_at}}.dump();
}

std::filesystem::path seal_path(const std::filesystem::path &project_folder) {
	return lm_folder(project_folder) / "Seal.json";
}

} // namespace

void set_seal_trust(std::function<SealComputerTrust(const std::string &actor_id, const std::string &public_key)> trust) {
	const std::lock_guard<std::mutex> lock(own_office_mutex());
	seal_trust() = std::move(trust);
}

void set_seal_carrier(std::function<bool(const std::string &matter_id, const std::filesystem::path &folder)> carries) {
	const std::lock_guard<std::mutex> lock(own_office_mutex());
	seal_carrier() = std::move(carries);
}

void seal_matter(const std::filesystem::path &project_folder, const std::string &matter_id, const std::string &organisation_id,
		const std::string &actor_id, const std::string &device_id, const std::string &now) {
	if (matter_id.empty() || !has_lm_folder(project_folder)) return;
	const std::string publicKey = platform::device_public_key();
	write_json(seal_path(project_folder), Json{
		{"format", "singularity-seal"}, {"formatVersion", 1},
		{"organisationId", organisation_id}, {"matterId", matter_id}, {"actorId", actor_id}, {"deviceId", device_id},
		{"publicKey", publicKey}, {"sealedAt", now},
		{"signature", platform::sign_with_device_key(seal_statement_bytes(organisation_id, matter_id, actor_id, device_id, publicKey, now))},
	}, platform::StagedCommitMode::ReplaceExisting);
}

MatterSealState matter_seal_state(const std::filesystem::path &project_folder, const std::string &matter_id,
		const std::string &organisation_id) {
	std::error_code error;
	if (!std::filesystem::exists(seal_path(project_folder), error)) return MatterSealState::Unsealed;
	const std::optional<Json> seal = read_json(seal_path(project_folder));
	if (!seal || !seal->is_object() || text_of(*seal, "format") != "singularity-seal") return MatterSealState::Untrusted;
	const std::string sealedFor = text_of(*seal, "organisationId");
	const std::string sealedMatter = text_of(*seal, "matterId");
	const std::string actor = text_of(*seal, "actorId");
	const std::string device = text_of(*seal, "deviceId");
	const std::string publicKey = text_of(*seal, "publicKey");
	const std::string sealedAt = text_of(*seal, "sealedAt");
	// A seal changed after it was signed holds nothing.
	if (!platform::verify_device_signature(publicKey,
			seal_statement_bytes(sealedFor, sealedMatter, actor, device, publicKey, sealedAt), text_of(*seal, "signature")))
		return MatterSealState::Untrusted;
	if (sealedFor != organisation_id) return MatterSealState::OtherOrganisation;
	// A seal of another Matter, copied onto this one, is not this Matter's.
	if (sealedMatter != matter_id) return MatterSealState::Untrusted;
	if (publicKey == platform::device_public_key()) return MatterSealState::Ours;
	std::function<SealComputerTrust(const std::string &, const std::string &)> trust;
	{
		const std::lock_guard<std::mutex> lock(own_office_mutex());
		trust = seal_trust();
	}
	SealComputerTrust decided = SealComputerTrust::Unconfirmed;
	try {
		if (trust) decided = trust(actor, publicKey);
	} catch (const std::exception &) {
	}
	if (decided == SealComputerTrust::Confirmed) return MatterSealState::Ours;
	if (decided == SealComputerTrust::Refused) return MatterSealState::Untrusted;
	return MatterSealState::Unconfirmed;
}

bool is_firms_matter(const std::filesystem::path &project_folder, const std::string &matter_id, const std::string &organisation_id) {
	return matter_seal_state(project_folder, matter_id, organisation_id) == MatterSealState::Ours
		|| is_carried(project_folder, matter_id);
}

bool may_open_matter(const std::filesystem::path &project_folder, const std::string &matter_id, const std::string &organisation_id) {
	const MatterSealState state = matter_seal_state(project_folder, matter_id, organisation_id);
	return state == MatterSealState::Ours || state == MatterSealState::Unconfirmed || is_carried(project_folder, matter_id);
}

void keep_seal(const std::filesystem::path &project_folder, const std::string &matter_id, const std::string &organisation_id,
		const std::string &actor_id, const std::string &device_id, const std::string &now) {
	const MatterSealState state = matter_seal_state(project_folder, matter_id, organisation_id);
	if (state == MatterSealState::Ours) return;
	if (state != MatterSealState::Unsealed) {
		const std::optional<Json> seal = read_json(seal_path(project_folder));
		const bool isThisComputers = seal && seal->is_object() && text_of(*seal, "publicKey") == platform::device_public_key();
		if (!isThisComputers) return;
		if (state == MatterSealState::OtherOrganisation && !text_of(*seal, "organisationId").empty()) return;
	}
	seal_matter(project_folder, matter_id, organisation_id, actor_id, device_id, now);
}

std::string picture_type(const std::string &bytes) {
	const auto starts = [&](const std::string &magic) { return bytes.size() >= magic.size() && bytes.compare(0, magic.size(), magic) == 0; };
	if (starts(std::string("\xFF\xD8\xFF", 3))) return "image/jpeg";
	if (starts(std::string("\x89PNG\r\n\x1A\n", 8))) return "image/png";
	if (starts("GIF87a") || starts("GIF89a")) return "image/gif";
	if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) return "image/webp";
	return std::string();
}

bool is_member_photo(const std::string &photo) {
	constexpr std::size_t kMostBytes = 96 * 1024;
	static const std::string jpeg = "data:image/jpeg;base64,";
	static const std::string png = "data:image/png;base64,";
	const bool isJpeg = photo.rfind(jpeg, 0) == 0;
	if (!isJpeg && photo.rfind(png, 0) != 0) return false;
	const std::size_t prefix = isJpeg ? jpeg.size() : png.size();
	if (photo.size() - prefix > (kMostBytes + 2) / 3 * 4) return false;
	const std::optional<std::string> bytes = platform::base64_decode(photo.substr(prefix));
	if (!bytes || bytes->empty() || bytes->size() > kMostBytes) return false;
	return picture_type(*bytes) == (isJpeg ? "image/jpeg" : "image/png");
}

void set_own_office_source(std::function<organisation::MemberOffice()> source) {
	const std::lock_guard<std::mutex> lock(own_office_mutex());
	own_office_source() = std::move(source);
}

organisation::MemberOffice own_office() {
	std::function<organisation::MemberOffice()> source;
	{
		const std::lock_guard<std::mutex> lock(own_office_mutex());
		source = own_office_source();
	}
	if (!source) return organisation::MemberOffice{};
	try {
		return source();
	} catch (const std::exception &) {
		return organisation::MemberOffice{};
	}
}

void record_member(const std::filesystem::path &project_folder, const std::string &actor_id, const std::string &name,
		const std::string &organisation, bool is_account_name, const std::string &device_id, const std::string &now) {
	record_member(project_folder, actor_id, name, organisation, ::singularity::organisation::MemberOffice{}, is_account_name,
		device_id, now);
}

void record_member(const std::filesystem::path &project_folder, const std::string &actor_id, const std::string &name,
		const std::string &organisation, const ::singularity::organisation::MemberOffice &office, bool is_account_name,
		const std::string &device_id, const std::string &now) {
	if (actor_id.empty() || !has_lm_folder(project_folder)) return;
	try {
		const std::filesystem::path path = lm_folder(project_folder) / "Members" / member_file_name(actor_id);
		Json record = read_json(path).value_or(Json::object());
		std::vector<std::string> devices;
		const auto known = record.find("deviceIds");
		if (known != record.end() && known->is_array())
			for (const Json &device : *known)
				if (device.is_string()) devices.push_back(device.get<std::string>());
		const bool isNewDevice = !device_id.empty() && std::find(devices.begin(), devices.end(), device_id) == devices.end();
		if (isNewDevice) devices.push_back(device_id);
		// This computer's key, stated under its own signature. Each computer
		// keeps only its own statement; a colleague's other computers keep theirs.
		Json keys = record.contains("deviceKeys") && record.at("deviceKeys").is_array() ? record.at("deviceKeys") : Json::array();
		const std::string publicKey = device_id.empty() ? std::string() : platform::device_public_key();
		bool isKeyStated = device_id.empty();
		for (const Json &entry : keys)
			if (entry.is_object() && entry.value("deviceId", std::string()) == device_id
					&& entry.value("publicKey", std::string()) == publicKey)
				isKeyStated = true;
		if (!isKeyStated) {
			keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const Json &entry) {
				return !entry.is_object() || entry.value("deviceId", std::string()) == device_id;
			}), keys.end());
			keys.push_back(Json{{"deviceId", device_id}, {"publicKey", publicKey},
				{"signature", platform::sign_with_device_key(device_statement_bytes(actor_id, device_id, publicKey))}});
		}
		const std::string lastSeen = text_of(record, "lastSeenAt");
		// Once a day is enough to say who works on the Matter: the record is not
		// rewritten every time it is opened.
		const bool isSameDay = lastSeen.size() >= 10 && now.size() >= 10 && lastSeen.compare(0, 10, now, 0, 10) == 0;
		// Their photo travels as their office does, and a changed one is written
		// at once.
		const std::string photo = own_photo();
		// The organisation they are signed in to, by Legal Machines' id: who is
		// of one firm is told by it, never by the name; a changed one is written
		// at once.
		const std::string organisationId = own_organisation_id();
		if (!isNewDevice && isKeyStated && isSameDay && text_of(record, "name") == name
				&& text_of(record, "organisation") == organisation && text_of(record, "organisationId") == organisationId
				&& text_of(record, "position") == office.position && text_of(record, "practice") == office.practice
				&& text_of(record, "photo") == photo) return;
		const std::string firstSeen = text_of(record, "firstSeenAt");
		Json written{
			{"format", "singularity-member"}, {"formatVersion", 1},
			{"actorId", actor_id}, {"name", name}, {"organisation", organisation},
			{"position", office.position}, {"practice", office.practice}, {"isAccountName", is_account_name},
			{"deviceIds", devices}, {"deviceKeys", keys}, {"firstSeenAt", firstSeen.empty() ? now : firstSeen},
			{"lastSeenAt", now},
		};
		if (!photo.empty()) written["photo"] = photo;
		if (!organisationId.empty()) written["organisationId"] = organisationId;
		write_json(path, written, platform::StagedCommitMode::ReplaceExisting);
	} catch (const std::exception &) {
	}
}

bool has_other_members(const std::filesystem::path &project_folder, const std::string &my_actor_id) {
	const std::vector<SharedMember> members = load_members(project_folder);
	return std::any_of(members.begin(), members.end(), [&](const SharedMember &member) { return member.actor_id != my_actor_id; });
}

void add_asking_devices(std::vector<SharedMember> &members, const std::vector<AskingDevice> &asking,
		const std::function<std::string(const std::string &actor_id)> &name_of,
		const std::function<bool(const AskingDevice &device)> &is_refused) {
	for (const AskingDevice &device : asking) {
		if (device.actor_id.empty() == true || device.device_id.empty() == true || device.device_key.empty() == true) continue;
		if (is_refused && is_refused(device) == true) continue;
		auto member = std::find_if(members.begin(), members.end(), [&](const SharedMember &known) { return known.actor_id == device.actor_id; });
		if (member == members.end()) {
			SharedMember added;
			added.actor_id = device.actor_id;
			if (name_of) added.name = name_of(device.actor_id);
			members.push_back(std::move(added));
			member = std::prev(members.end());
		}
		auto stated = std::find_if(member->device_keys.begin(), member->device_keys.end(),
			[&](const SharedDeviceKey &key) { return key.device_id == device.device_id; });
		if (stated == member->device_keys.end()) {
			member->device_keys.push_back(SharedDeviceKey{device.device_id, device.device_key, true});
		} else {
			stated->public_key = device.device_key;
			stated->is_asking = true;
		}
	}
}

std::vector<SharedMember> load_members(const std::filesystem::path &project_folder) {
	std::vector<SharedMember> members;
	std::error_code error;
	std::filesystem::directory_iterator entries(lm_folder(project_folder) / "Members", error);
	if (error) return members;
	for (const auto &entry : entries) {
		const std::string fileName = entry.path().filename().string();
		if (fileName.rfind("m_", 0) != 0 || entry.path().extension() != ".json") continue;
		const std::optional<Json> record = read_json(entry.path());
		if (!record || text_of(*record, "actorId").empty()) continue;
		SharedMember member;
		member.actor_id = text_of(*record, "actorId");
		// A record names only its own writer: one whose name does not match its
		// actor is not believed.
		if (fileName != member_file_name(member.actor_id)) continue;
		member.name = text_of(*record, "name");
		member.organisation = text_of(*record, "organisation");
		member.organisation_id = text_of(*record, "organisationId");
		member.position = text_of(*record, "position");
		member.practice = text_of(*record, "practice");
		const std::string photo = text_of(*record, "photo");
		if (is_member_photo(photo)) member.photo = photo;
		member.is_account_name = record->value("isAccountName", false);
		const auto devices = record->find("deviceIds");
		if (devices != record->end() && devices->is_array())
			for (const Json &device : *devices)
				if (device.is_string()) member.device_ids.push_back(device.get<std::string>());
		const auto keys = record->find("deviceKeys");
		if (keys != record->end() && keys->is_array())
			for (const Json &entry : *keys) {
				if (!entry.is_object()) continue;
				const std::string deviceId = text_of(entry, "deviceId");
				const std::string publicKey = text_of(entry, "publicKey");
				// A statement whose signature does not verify states nothing.
				if (deviceId.empty() || !platform::verify_device_signature(publicKey,
						device_statement_bytes(member.actor_id, deviceId, publicKey), text_of(entry, "signature")))
					continue;
				member.device_keys.push_back(SharedDeviceKey{deviceId, publicKey, false});
			}
		member.first_seen_at = text_of(*record, "firstSeenAt");
		member.last_seen_at = text_of(*record, "lastSeenAt");
		members.push_back(std::move(member));
	}
	std::sort(members.begin(), members.end(),
		[](const SharedMember &left, const SharedMember &right) { return left.last_seen_at > right.last_seen_at; });
	return members;
}

std::optional<OverwrittenChange> detect_overwritten_change(const std::filesystem::path &project_folder,
		const std::filesystem::path &versions_root, const matters::Matter &matter, const documents::DocumentEvent &event,
		const std::filesystem::path &current_file, const ParagraphReader &read_paragraphs, const std::string &device_id,
		const std::string &now) {
	if (!event.actor.empty() || event.kind != documents::DocumentEventKind::Changed) return std::nullopt;
	if (event.previous_fingerprint.empty() || event.fingerprint.empty() || !has_lm_folder(project_folder)) return std::nullopt;
	// What replaced it is itself an Singularity write, seen from this computer:
	// not an overwrite.
	if (written_event_with(matter, event.document_id, event.fingerprint) != nullptr) return std::nullopt;
	const documents::DocumentEvent *written = written_event_with(matter, event.document_id, event.previous_fingerprint);
	if (written == nullptr || written->previous_fingerprint.empty()) return std::nullopt;

	OverwrittenChange change;
	change.id = "ow_" + platform::sha256_hex_digest(event.document_id + "\n" + written->fingerprint + "\n" + event.fingerprint).substr(0, 24);
	const std::filesystem::path recordPath = conflicts_folder(project_folder) / (change.id + ".json");
	std::error_code error;
	if (std::filesystem::exists(recordPath, error)) {
		const std::optional<Json> existing = read_json(recordPath);
		if (existing) {
			std::optional<OverwrittenChange> kept = overwritten_from_json(*existing);
			if (kept) read_resolution(project_folder, *kept);
			return kept;
		}
	}

	// The three texts: what the write was made to, what it wrote, what is there now.
	std::optional<documents::KeptDocumentVersion> base;
	std::optional<documents::KeptDocumentVersion> writtenCopy;
	for (const documents::KeptDocumentVersion &kept : documents::list_kept_document_versions(versions_root, event.document_id)) {
		if (kept.fingerprint == written->previous_fingerprint) base = kept;
		if (kept.fingerprint == written->fingerprint) writtenCopy = kept;
	}
	if (!base || !writtenCopy) return std::nullopt;
	const std::optional<std::vector<std::string>> baseText = read_paragraphs(base->path);
	const std::optional<std::vector<std::string>> writtenText = read_paragraphs(writtenCopy->path);
	const std::optional<std::vector<std::string>> currentText = read_paragraphs(current_file);
	if (!baseText || !writtenText || !currentText) return std::nullopt;
	const std::set<std::string> before(baseText->begin(), baseText->end());
	const std::set<std::string> now_(currentText->begin(), currentText->end());
	for (const std::string &paragraph : *writtenText) {
		if (before.count(paragraph) != 0 || now_.count(paragraph) != 0) continue;
		if (change.missing_passages.size() < kMostPassages) change.missing_passages.push_back(clipped(paragraph, kMostPassageCharacters));
		else break;
	}
	// The write's own words are all still there: whatever else changed, nothing
	// it made was lost.
	if (change.missing_passages.empty()) return std::nullopt;

	change.document_id = event.document_id;
	change.name = event.name;
	change.relative_path = event.relative_path;
	change.found_at = now;
	change.found_by_device_id = device_id;
	change.written_version = written->version;
	change.written_fingerprint = written->fingerprint;
	change.written_by_actor_id = written->actor_id;
	change.written_operation_id = written->operation_id;
	change.base_version = written->previous_version;
	change.base_fingerprint = written->previous_fingerprint;
	change.current_version = event.version;
	change.current_fingerprint = event.fingerprint;
	try {
		// The same overwrite found by two computers is one record: whichever
		// lands first is kept, the other is not written over it.
		write_json(recordPath, overwritten_to_json(change), platform::StagedCommitMode::CreateExclusive);
	} catch (const std::exception &) {
		const std::optional<Json> existing = read_json(recordPath);
		if (!existing) return std::nullopt;
		std::optional<OverwrittenChange> kept = overwritten_from_json(*existing);
		if (kept) read_resolution(project_folder, *kept);
		return kept;
	}
	return change;
}

std::vector<OverwrittenChange> load_overwritten_changes(const std::filesystem::path &project_folder) {
	std::vector<OverwrittenChange> changes;
	std::error_code error;
	std::filesystem::directory_iterator entries(conflicts_folder(project_folder), error);
	if (error) return changes;
	for (const auto &entry : entries) {
		const std::string fileName = entry.path().filename().string();
		if (fileName.rfind("ow_", 0) != 0 || fileName.find(".resolution.") != std::string::npos || entry.path().extension() != ".json")
			continue;
		const std::optional<Json> record = read_json(entry.path());
		if (!record) continue;
		std::optional<OverwrittenChange> change = overwritten_from_json(*record);
		if (!change || change->id + ".json" != fileName) continue;
		read_resolution(project_folder, *change);
		changes.push_back(std::move(*change));
	}
	std::sort(changes.begin(), changes.end(),
		[](const OverwrittenChange &left, const OverwrittenChange &right) { return left.found_at > right.found_at; });
	return changes;
}

OverwrittenChange resolve_overwritten_change(const std::filesystem::path &project_folder, const std::string &id,
		const std::string &resolution, const std::string &actor_id, const std::string &now) {
	if (!is_safe_record_id(id)) throw std::invalid_argument("This change is not one Singularity found.");
	if (resolution != "kept_current" && resolution != "restored_written")
		throw std::invalid_argument("Choose whether to keep the document as it is or bring the change back.");
	const std::optional<Json> record = read_json(conflicts_folder(project_folder) / (id + ".json"));
	if (!record) throw std::invalid_argument("This change is not one Singularity found.");
	std::optional<OverwrittenChange> change = overwritten_from_json(*record);
	if (!change) throw std::invalid_argument("This change is not one Singularity found.");
	read_resolution(project_folder, *change);
	if (!change->resolution.empty()) throw std::invalid_argument("This was already decided.");
	// A decision is its own record, made once: two lawyers deciding at once
	// cannot both land.
	write_json(conflicts_folder(project_folder) / (id + ".resolution.json"),
		Json{{"id", id}, {"resolution", resolution}, {"resolvedAt", now}, {"resolvedByActorId", actor_id}},
		platform::StagedCommitMode::CreateExclusive);
	change->resolution = resolution;
	change->resolved_at = now;
	change->resolved_by_actor_id = actor_id;
	return *change;
}

std::vector<AttributedChange> attributed_changes(const matters::Matter &matter, const std::vector<SharedMember> &members,
		const std::string &my_actor_id, std::size_t most) {
	std::map<std::string, std::string> names;
	for (const SharedMember &member : members) names[member.actor_id] = member.name;
	std::vector<AttributedChange> changes;
	for (auto event = matter.document_events.rbegin(); event != matter.document_events.rend() && changes.size() < most; ++event) {
		AttributedChange change;
		change.event = *event;
		if (event->actor == "singularity") {
			if (event->actor_id.empty()) change.author = ChangeAuthor::Workspace;
			else if (event->actor_id == my_actor_id) change.author = ChangeAuthor::You;
			else change.author = ChangeAuthor::Member;
			if (change.author == ChangeAuthor::Member) change.author_name = names.count(event->actor_id) != 0 ? names.at(event->actor_id) : "";
		} else if (event->kind == documents::DocumentEventKind::Changed
				&& written_event_with(matter, event->document_id, event->fingerprint) != nullptr) {
			// This computer seeing what an Singularity write made: that write is
			// listed, as itself.
			continue;
		} else {
			change.author = ChangeAuthor::External;
		}
		changes.push_back(std::move(change));
	}
	return changes;
}

} // namespace singularity::matter
