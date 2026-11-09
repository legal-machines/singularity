#include "Matter/CoordinationTransport/FolderTransport.hpp"

#include <monocypher.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace singularity::matter::coordination::transport {
namespace {

using Json = nlohmann::json;

// When this program last offered each envelope of each Matter's folder, by
// the number of the poll, for as long as it runs: the engine leaves for later
// what it cannot open yet, and that must not hide what came after it. A
// computer that asked to join waits on thousands of a Matter's changes it
// cannot read until the key comes - and the key comes last.
struct Offered {
	std::mutex mutex;
	std::uint64_t polls = 0;
	std::map<std::string, std::map<std::string, std::uint64_t>> byFolder;
};

Offered& offered() {
	// Never destroyed: polled from the engine's thread, which may outlive
	// static destruction at exit.
	static Offered* const state = new Offered();
	return *state;
}

constexpr const char* kExtension = ".lme";
constexpr const char* kPassing = ".partial";

std::string hexOf(const std::uint8_t* bytes, std::size_t size) {
	static const char* const digits = "0123456789abcdef";
	std::string text;
	text.reserve(size * 2);
	for (std::size_t index = 0; index < size; ++index) {
		text += digits[bytes[index] >> 4];
		text += digits[bytes[index] & 0x0F];
	}
	return text;
}

// What the storage's owner sees of a Matter: a hash that says nothing of it.
std::string folderNameOf(const std::string& matterId) {
	const std::string named = "singularity exchange folder\n" + matterId;
	std::uint8_t hash[16];
	crypto_blake2b(hash, sizeof hash, reinterpret_cast<const std::uint8_t*>(named.data()), named.size());
	return hexOf(hash, sizeof hash);
}

bool isEnvelopeId(const std::string& text) {
	return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char character) {
		return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
	});
}

std::optional<SealedBytes> readWhole(const std::filesystem::path& path, std::size_t most) {
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error || size > most) return std::nullopt;
	std::ifstream file(path, std::ios::binary);
	if (!file) return std::nullopt;
	SealedBytes bytes(static_cast<std::size_t>(size));
	if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) return std::nullopt;
	return bytes;
}

// Writes a file whole or not at all: a passing name, then the real one.
void writeWhole(const std::filesystem::path& target, const std::string& text) {
	const std::filesystem::path passing = target.parent_path() / (target.filename().u8string() + kPassing);
	{
		std::ofstream file(passing, std::ios::binary | std::ios::trunc);
		if (!file) throw std::runtime_error("This computer's record of the firm's folder could not be written.");
		file << text;
		file.flush();
		if (!file) throw std::runtime_error("This computer's record of the firm's folder could not be written.");
	}
	std::error_code error;
	std::filesystem::rename(passing, target, error);
	if (error) {
		std::filesystem::remove(passing, error);
		throw std::runtime_error("This computer's record of the firm's folder could not be written.");
	}
}

} // namespace

FolderTransport::FolderTransport(std::filesystem::path folder, std::filesystem::path records)
	: folder_(std::move(folder)), records_(std::move(records)) {}

std::string FolderTransport::name() const {
	return "Firm folder";
}

std::filesystem::path FolderTransport::matter_folder(const std::string& matterId) const {
	return folder_ / folderNameOf(matterId);
}

void FolderTransport::send(const std::string& matterId, const SealedBytes& bytes, const std::vector<std::string>&) {
	// Every computer that keeps the folder sees the envelope: the seal says whom
	// it is for, and only they open what is theirs alone.
	const std::filesystem::path folder = matter_folder(matterId);
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	if (error) throw std::runtime_error("The firm's folder could not be reached: " + error.message() + ".");
	const std::string id = envelope_id(bytes);
	const std::filesystem::path target = folder / (id + kExtension);
	if (std::filesystem::exists(target, error)) return;
	const std::filesystem::path passing = folder / ("." + id + kPassing);
	{
		std::ofstream file(passing, std::ios::binary | std::ios::trunc);
		if (!file) throw std::runtime_error("The firm's folder could not be written to.");
		if (bytes.empty() == false) file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		file.flush();
		if (!file) {
			file.close();
			std::filesystem::remove(passing, error);
			throw std::runtime_error("The firm's folder could not be written to.");
		}
	}
	std::filesystem::rename(passing, target, error);
	if (error) {
		std::error_code ignored;
		std::filesystem::remove(passing, ignored);
		if (std::filesystem::exists(target, ignored) == false)
			throw std::runtime_error("The firm's folder could not take the envelope: " + error.message() + ".");
	}
}

std::vector<SealedBytes> FolderTransport::poll(const std::string& matterId) {
	std::vector<SealedBytes> found;
	const std::filesystem::path folder = matter_folder(matterId);
	std::error_code error;
	if (std::filesystem::is_directory(folder, error) == false) return found;
	const std::set<std::string> already = taken(matterId);
	struct Waiting {
		std::filesystem::file_time_type written;
		std::string id;
		std::filesystem::path path;
		std::uint64_t offeredAt = 0;
	};
	Offered& state = offered();
	const std::lock_guard<std::mutex> lock(state.mutex);
	std::map<std::string, std::uint64_t>& last = state.byFolder[folder.u8string()];
	const std::uint64_t poll = ++state.polls;
	std::vector<Waiting> files;
	std::set<std::string> present;
	for (std::filesystem::directory_iterator entry(folder, error), end; error.value() == 0 && entry != end; entry.increment(error)) {
		const std::filesystem::path path = entry->path();
		if (path.extension().u8string() != kExtension) continue;
		const std::string id = path.stem().u8string();
		if (isEnvelopeId(id) == false) continue;
		present.insert(id);
		if (already.count(id) > 0) continue;
		std::error_code timeError;
		const auto offeredAt = last.find(id);
		files.push_back(Waiting{entry->last_write_time(timeError), id, path, offeredAt == last.end() ? 0 : offeredAt->second});
	}
	// What was taken and the folder no longer holds is forgotten, once enough
	// has gone to be worth writing the record anew - and only after the whole
	// folder was read.
	if (error.value() == 0) {
		std::size_t gone = 0;
		for (const std::string& id : already) {
			if (present.count(id) == 0) ++gone;
		}
		if (gone >= 256 && gone * 2 >= already.size()) {
			std::set<std::string> kept;
			for (const std::string& id : already) {
				if (present.count(id) > 0) kept.insert(id);
			}
			keepTaken(matterId, kept);
		}
	}
	// What was never offered goes first, the newest first, so what just came -
	// a key, a request to join - is looked at on the next round; then what
	// waits, offered longest ago first, so each is offered again in turn. The
	// seal's counters set the order the engine applies them in.
	std::sort(files.begin(), files.end(), [](const Waiting& first, const Waiting& second) {
		const bool isFirstNew = first.offeredAt == 0;
		const bool isSecondNew = second.offeredAt == 0;
		if (isFirstNew != isSecondNew) return isFirstNew;
		if (isFirstNew == true) return first.written > second.written;
		if (first.offeredAt != second.offeredAt) return first.offeredAt < second.offeredAt;
		return first.written < second.written;
	});
	// Forgotten: what the folder no longer offers.
	std::map<std::string, std::uint64_t> stillThere;
	for (const Waiting& file : files) {
		if (file.offeredAt != 0) stillThere[file.id] = file.offeredAt;
	}
	last.swap(stillThere);
	std::size_t read = 0;
	for (const Waiting& file : files) {
		if (found.size() >= kMostPerPoll || read >= kMostBytesPerPoll) break;
		std::optional<SealedBytes> bytes = readWhole(file.path, kMostBytesPerPoll - read);
		if (bytes.has_value() == false) continue;
		// Still syncing, cut short, or put there by someone else: looked at again
		// next time, never offered as it is.
		if (envelope_id(*bytes) != file.id) continue;
		read += bytes->size();
		last[file.id] = poll;
		found.push_back(std::move(*bytes));
	}
	return found;
}

void FolderTransport::ack(const std::string& matterId, const std::string& envelopeId) {
	if (isEnvelopeId(envelopeId) == false) return;
	addTaken(matterId, envelopeId);
}

std::size_t FolderTransport::remove_older_than(const std::string& matterId, std::chrono::hours age) {
	const std::filesystem::path folder = matter_folder(matterId);
	std::error_code error;
	if (std::filesystem::is_directory(folder, error) == false) return 0;
	const std::filesystem::file_time_type before = std::filesystem::file_time_type::clock::now() - age;
	// A passing file left by a computer that stopped while writing goes after an
	// hour.
	const std::filesystem::file_time_type passingBefore = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
	std::vector<std::filesystem::path> gone;
	for (std::filesystem::directory_iterator entry(folder, error), end; error.value() == 0 && entry != end; entry.increment(error)) {
		std::error_code timeError;
		const std::filesystem::file_time_type written = entry->last_write_time(timeError);
		if (timeError) continue;
		const std::string name = entry->path().filename().u8string();
		const bool isEnvelope = entry->path().extension().u8string() == kExtension;
		const bool isPassing = name.size() > std::char_traits<char>::length(kPassing)
			&& name.compare(name.size() - std::char_traits<char>::length(kPassing), std::string::npos, kPassing) == 0;
		if ((isEnvelope && written < before) || (isPassing && written < passingBefore)) gone.push_back(entry->path());
	}
	std::size_t removed = 0;
	for (const std::filesystem::path& path : gone) {
		std::error_code removeError;
		if (std::filesystem::remove(path, removeError) == true && path.extension().u8string() == kExtension) ++removed;
	}
	return removed;
}

std::filesystem::path FolderTransport::takenFile(const std::string& matterId) const {
	return records_ / (folderNameOf(matterId) + ".taken");
}

std::set<std::string> FolderTransport::taken(const std::string& matterId) const {
	carryOverJsonRecord(matterId);
	std::set<std::string> ids;
	std::ifstream file(takenFile(matterId), std::ios::binary);
	if (!file) return ids;
	for (std::string line; std::getline(file, line);) {
		if (line.empty() == false && line.back() == '\r') line.pop_back();
		// A line cut short when the computer stopped is no id, and is passed over.
		if (isEnvelopeId(line) == true) ids.insert(line);
	}
	return ids;
}

void FolderTransport::addTaken(const std::string& matterId, const std::string& envelopeId) const {
	carryOverJsonRecord(matterId);
	std::error_code error;
	std::filesystem::create_directories(records_, error);
	if (error) throw std::runtime_error("This computer's record of the firm's folder could not be kept: " + error.message() + ".");
	std::ofstream file(takenFile(matterId), std::ios::binary | std::ios::app);
	file << envelopeId << '\n';
	file.flush();
	if (!file) throw std::runtime_error("This computer's record of the firm's folder could not be kept.");
}

void FolderTransport::keepTaken(const std::string& matterId, const std::set<std::string>& ids) const {
	std::error_code error;
	std::filesystem::create_directories(records_, error);
	if (error) throw std::runtime_error("This computer's record of the firm's folder could not be kept: " + error.message() + ".");
	std::string text;
	text.reserve(ids.size() * 65);
	for (const std::string& id : ids) text += id + "\n";
	writeWhole(takenFile(matterId), text);
}

void FolderTransport::carryOverJsonRecord(const std::string& matterId) const {
	const std::filesystem::path record = records_ / (folderNameOf(matterId) + ".json");
	std::error_code error;
	if (std::filesystem::exists(record, error) == false) return;
	std::set<std::string> ids;
	{
		std::ifstream file(record, std::ios::binary);
		std::stringstream text;
		text << file.rdbuf();
		const Json parsed = Json::parse(text.str(), nullptr, false);
		if (parsed.is_object() == true && parsed.contains("taken") == true && parsed.at("taken").is_array() == true) {
			for (const Json& id : parsed.at("taken")) {
				if (id.is_string() == true && isEnvelopeId(id.get<std::string>()) == true) ids.insert(id.get<std::string>());
			}
		}
	}
	std::ifstream lines(takenFile(matterId), std::ios::binary);
	for (std::string line; std::getline(lines, line);) {
		if (line.empty() == false && line.back() == '\r') line.pop_back();
		if (isEnvelopeId(line) == true) ids.insert(line);
	}
	lines.close();
	keepTaken(matterId, ids);
	std::filesystem::remove(record, error);
}

} // namespace singularity::matter::coordination::transport
