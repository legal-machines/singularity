#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace singularity::protected_api::papinian {

// Whether `word` is an ordinary word of the language, read from the public
// domain list beside this header (see README.md). Case-insensitive for ASCII;
// anything holding a non-ASCII byte answers false, because the list is English.
//
// This exists to STOP a rule, never to start one. Papinian Guard's
// text-evidence rule may treat a detected name as an ordinary word only when
// the language has that word; a name written in lowercase - which is how a
// client's name reaches a document by accident - is not a word of the language
// and stays masked. When the list cannot be read, this answers false for
// everything, so the rule never fires and the Guard masks exactly as it would
// without it.
//
// Header-only and free of any other module on purpose: every build that links
// the Guard gets it, and none of them has to remember to add a source file.
namespace common_words_detail {

inline std::filesystem::path executable_directory() {
#if defined(__APPLE__)
	std::vector<char> buffer(1024);
	auto size = static_cast<std::uint32_t>(buffer.size());
	if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
		buffer.resize(size);
		if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
	}
	return std::filesystem::path(buffer.data()).parent_path();
#elif defined(_WIN32)
	std::vector<wchar_t> buffer(1024);
	const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (written == 0) return {};
	return std::filesystem::path(std::wstring(buffer.data(), written)).parent_path();
#else
	std::error_code error;
	const auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
	return error ? std::filesystem::path{} : resolved.parent_path();
#endif
}

inline std::vector<std::filesystem::path> candidate_paths() {
	std::vector<std::filesystem::path> paths;
	if (const char *override_path = std::getenv("SINGULARITY_COMMON_WORDS_FILE"))
		if (*override_path != '\0') paths.emplace_back(override_path);
	const std::filesystem::path beside = executable_directory();
	if (!beside.empty()) {
		paths.push_back(beside.parent_path() / "Resources" / "english_common_words.txt");
		paths.push_back(beside / "english_common_words.txt");
		paths.push_back(beside.parent_path() / "share" / "singularity" / "english_common_words.txt");
	}
#ifdef NATIVE_APP_ROOT_DIR
	paths.push_back(std::filesystem::path(NATIVE_APP_ROOT_DIR) / "code" / "backend" / "PapinianGuard"
		/ "CommonWords" / "english_common_words.txt");
#endif
	return paths;
}

struct Loaded {
	std::unordered_set<std::string> words;
	std::filesystem::path source;
};

inline const Loaded &loaded() {
	static const Loaded list = [] {
		Loaded loading;
		for (const std::filesystem::path &path : candidate_paths()) {
			std::ifstream file(path);
			if (!file.is_open()) continue;
			std::string word;
			while (std::getline(file, word)) {
				while (!word.empty() && (word.back() == '\r' || word.back() == '\n' || word.back() == ' '))
					word.pop_back();
				if (word.size() >= 3) loading.words.insert(word);
			}
			if (loading.words.empty()) continue;
			loading.source = path;
			break;
		}
		return loading;
	}();
	return list;
}

} // namespace common_words_detail

// A word the list has, or a regular form of one: the list (web2) has almost no
// inflected forms - "counterpart" but not "counterparts" - so a plural ("-s",
// "-es", "-ies" for "-y"), a possessive ("'s", "s'"), a past ("-ed", "-d",
// "-ied", a doubled consonant) and "-ing" are read back to the word they
// inflect. "whitfield" and "whitfields" are still no word of the language.
[[nodiscard]] inline bool is_common_word(const std::string &word) {
	std::string folded;
	folded.reserve(word.size());
	for (const char character : word) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte >= 0x80 || (std::isalpha(byte) == 0 && character != '\'')) return false;
		folded.push_back(static_cast<char>(std::tolower(byte)));
	}
	// The possessive: "party's", "parties'".
	if (folded.size() > 2 && folded.compare(folded.size() - 2, 2, "'s") == 0) folded.erase(folded.size() - 2);
	else if (!folded.empty() && folded.back() == '\'') folded.pop_back();
	if (folded.size() < 3 || folded.find('\'') != std::string::npos) return false;
	const auto &words = common_words_detail::loaded().words;
	// Words the language gained after 1934, which Webster's cannot list: the
	// vocabulary of every commercial contract now ("warehouse robotics" in a
	// non-compete was masked as a company for want of "robotics"). Only evidence,
	// like the list: a detected name the document writes this way as ordinary
	// words, in lower case, is read as words; nothing is masked because of it.
	static const std::set<std::string> modern{"internet", "online", "offline", "website", "webpage", "email", "smartphone",
		"laptop", "database", "dataset", "download", "startup", "fintech", "ecommerce", "cybersecurity", "blockchain",
		"cryptocurrency", "app", "biotech", "biotechnology", "nanotechnology", "telecom", "telecommunications", "broadband",
		"podcast", "smartwatch", "microchip", "microprocessor", "compute", "cyber", "malware", "spyware", "ransomware",
		"username", "logout", "workflow", "spreadsheet", "chatbot", "automate", "automation", "telemedicine", "telehealth",
		"esports", "rideshare", "outsource", "crowdfunding", "fundraising", "benchmark", "onboarding", "offboarding",
		"upskilling", "noncompete", "subprocessor", "microservice", "cellphone", "touchscreen", "videoconference",
		"teleconference", "webinar", "encrypt", "cybercrime", "software", "hardware"};
	const auto listed = [&](const std::string &form) {
		return form.size() >= 3 && (words.count(form) > 0 || modern.count(form) > 0);
	};
	if (listed(folded)) return true;
	const auto endsWith = [&](const char *suffix) {
		const std::size_t size = std::char_traits<char>::length(suffix);
		return folded.size() > size + 2 && folded.compare(folded.size() - size, size, suffix) == 0;
	};
	const auto stem = [&](std::size_t cut) { return folded.substr(0, folded.size() - cut); };
	const auto doubled = [](const std::string &base) {
		return base.size() >= 4 && base[base.size() - 1] == base[base.size() - 2];
	};
	if (endsWith("ies") && listed(stem(3) + "y")) return true;
	// A field of study named from its word: "robotics", "electronics".
	if (endsWith("ics") && listed(stem(3))) return true;
	if (endsWith("es") && listed(stem(2))) return true;
	if (endsWith("s") && !endsWith("ss") && listed(stem(1))) return true;
	if (endsWith("ied") && listed(stem(3) + "y")) return true;
	if (endsWith("ed")) {
		const std::string base = stem(2);
		if (listed(base) || listed(base + "e") || (doubled(base) && listed(base.substr(0, base.size() - 1)))) return true;
	}
	if (endsWith("ing")) {
		const std::string base = stem(3);
		if (listed(base) || listed(base + "e") || (doubled(base) && listed(base.substr(0, base.size() - 1)))) return true;
	}
	return false;
}

// Abbreviations every business or legal reader uses and no one is named by:
// "SLA", "NDA", "EBITDA", «НДС», «УПД», «ДС» (дополнительное соглашение).
// Like the list above, only evidence that a detected all-capitals word is
// ordinary - never a reason to mask - and matched exactly as written, in
// capitals: "Sla" or "Ндс" is not the abbreviation. A company that trades
// under one of these letters alone is still masked wherever the Project or
// the lawyer names it (the registry and custom terms are matched first).
[[nodiscard]] inline bool is_public_abbreviation(const std::string &word) {
	static const std::unordered_set<std::string> abbreviations{
		// English business, finance and legal usage.
		"SLA", "SLAS", "NDA", "NDAS", "VAT", "GST", "EBITDA", "EBIT", "KPI", "KPIS", "SOW", "SOWS", "LOI", "MOU",
		"MSA", "SPA", "APA", "IPO", "ROI", "IRR", "NPV", "EPS", "ARR", "MRR", "WACC", "CAPEX", "OPEX", "FY", "YTD",
		"QTD", "EOD", "COB", "PO", "RFP", "RFQ", "RFI", "SKU", "EDI", "API", "SAAS", "IT", "HR", "PR", "QA", "FAQ",
		"PDF", "KYC", "AML", "ESG", "TBD", "TBC", "NA", "JV", "SPV", "LP", "GP", "DD", "PPE", "COGS", "SGA", "AR",
		"AP", "GL", "TOS", "EULA", "DPA", "SCC", "SCCS", "IP", "IPR", "R&D", "M&A", "P&L", "B2B", "B2C", "CRM",
		"ERP", "UAT", "SLO", "RPO", "RTO", "MFN", "ROFR", "ROFO", "LIBOR", "SOFR", "PIK", "LTV", "DSCR",
		// Russian business, tax and legal usage.
		"ДС", "ДУ", "ДДУ", "НДС", "УПД", "ЭДО", "СЭД", "ЕГРЮЛ", "ЕГРИП", "КПП", "ИНН", "ОГРН", "ОГРНИП", "БИК",
		"ОКПО", "ОКВЭД", "НДФЛ", "ТМЦ", "ГК", "АПК", "ГПК", "НК", "ТК", "КОАП", "ФЗ", "РФ", "СНГ", "ЕАЭС", "ЦБ",
		"ТЗ", "КП", "ОС", "ИТ", "ПО", "ФНС", "ПФР", "ФСС", "СФР", "МСФО", "РСБУ", "ЭЦП", "КЭП", "ЭП", "НМА", "ЗУ",
		"УСН", "ОСНО", "НПА", "ВНД", "ЛНА", "ПВТР", "ТТН", "СФ", "АВР", "ДКП", "ЕГРН", "ЖК", "ФАС", "ЕИС",
	};
	return abbreviations.count(word) > 0;
}

// Latin phrases of legal usage, lower-case: ordinary words of the law's own
// language, never anyone's name.
[[nodiscard]] inline bool is_legal_latin_phrase(const std::string &phrase) {
	static const std::unordered_set<std::string> phrases{
		"de minimis", "pro rata", "bona fide", "bona fides", "inter alia", "mutatis mutandis", "force majeure",
		"pari passu", "prima facie", "ad hoc", "ex parte", "per se", "quantum meruit", "res judicata",
		"sui generis", "ultra vires", "status quo", "de facto", "de jure", "in rem", "in personam", "pro forma",
		"vice versa", "et al", "ab initio", "caveat emptor", "amicus curiae", "stare decisis", "obiter dicta",
		"ratio decidendi", "forum non conveniens", "lex loci", "nunc pro tunc", "pro tanto", "ejusdem generis",
		"contra proferentem", "in camera", "sub judice", "ex gratia", "per diem", "per annum", "pro bono",
	};
	return phrases.count(phrase) > 0;
}

// Where the list was read from, empty when none was found. For diagnostics and
// tests; never part of a masking decision.
[[nodiscard]] inline std::filesystem::path common_words_source() {
	return common_words_detail::loaded().source;
}

} // namespace singularity::protected_api::papinian
