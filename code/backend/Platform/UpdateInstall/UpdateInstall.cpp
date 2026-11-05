#include "Platform/UpdateInstall/UpdateInstall.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace singularity::platform::update_install {

namespace {

constexpr const char *kOutcomeFile = "outcome.txt";

// A field of the outcome's one line: no tab and no line break inside.
std::string field(std::string value) {
	for (char &c : value) {
		if (c == '\t' || c == '\n' || c == '\r') c = ' ';
	}
	return value;
}

bool isState(const std::string &state) {
	return state == "installed" || state == "refused" || state == "failed";
}

} // namespace

bool record_outcome(const std::filesystem::path &folder, const Outcome &outcome) {
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	const std::filesystem::path path = folder / kOutcomeFile;
	const std::filesystem::path partial = folder / "outcome.txt.part";
	{
		std::ofstream out(partial, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		out << field(outcome.state) << '\t' << field(outcome.version) << '\t' << field(outcome.detail) << '\n';
		if (!out) return false;
	}
	std::filesystem::rename(partial, path, error);
	if (error) {
		std::filesystem::remove(partial, error);
		return false;
	}
	return true;
}

std::optional<Outcome> take_outcome(const std::filesystem::path &folder) {
	const std::filesystem::path path = folder / kOutcomeFile;
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
	std::string line;
	{
		std::ifstream in(path, std::ios::binary);
		std::getline(in, line);
	}
	std::filesystem::remove(path, error);
	if (!line.empty() && line.back() == '\r') line.pop_back();
	Outcome outcome;
	std::istringstream fields(line);
	if (!std::getline(fields, outcome.state, '\t') || !std::getline(fields, outcome.version, '\t')) return std::nullopt;
	std::getline(fields, outcome.detail);
	if (!isState(outcome.state) || outcome.version.empty()) return std::nullopt;
	return outcome;
}

} // namespace singularity::platform::update_install
