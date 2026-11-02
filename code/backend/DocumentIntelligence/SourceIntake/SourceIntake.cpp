#include "DocumentIntelligence/SourceIntake/SourceIntake.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <system_error>

#include <zip.h>

namespace singularity::document_intelligence {
namespace {

// The package's own parts this layer reads, and nothing else.
bool isPartToRead(const std::string& name) {

	if (name == "word/document.xml" || name == "word/styles.xml" || name == "word/numbering.xml") return true;
	if (name == "word/comments.xml" || name == "word/_rels/document.xml.rels") return true;
	if (name == "word/footnotes.xml" || name == "word/endnotes.xml") return true;
	if (name == "docProps/core.xml" || name == "docProps/app.xml") return true;
	if (name.rfind("word/header", 0) == 0 && name.size() > 15 && name.compare(name.size() - 4, 4, ".xml") == 0
		&& name.find('/', 5) == std::string::npos) {
		return true;
	}
	if (name.rfind("word/footer", 0) == 0 && name.size() > 15 && name.compare(name.size() - 4, 4, ".xml") == 0
		&& name.find('/', 5) == std::string::npos) {
		return true;
	}
	return false;

}

std::string lowered(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

bool startsWithZipMagic(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	char head[4] = {0, 0, 0, 0};
	input.read(head, 4);
	return input.gcount() == 4 && head[0] == 'P' && head[1] == 'K' && head[2] == '\x03' && head[3] == '\x04';
}

class ZipReader {
public:
	explicit ZipReader(const std::filesystem::path& path) {
		int errorCode = 0;
		archive_ = zip_open(path.u8string().c_str(), ZIP_RDONLY, &errorCode);
	}

	ZipReader(const ZipReader&) = delete;
	ZipReader& operator=(const ZipReader&) = delete;

	~ZipReader() {
		if (archive_ != nullptr) zip_discard(archive_);
	}

	zip_t* get() const {
		return archive_;
	}

private:
	zip_t* archive_ = nullptr;
};

// One entry's bytes, read only after its size was checked, and never more
// than it said it held.
bool readEntry(zip_t* archive, zip_uint64_t index, zip_uint64_t size, std::string& bytes) {

	std::unique_ptr<zip_file_t, int (*)(zip_file_t*)> file(zip_fopen_index(archive, index, 0), &zip_fclose);
	if (file == nullptr) return false;
	bytes.assign(static_cast<std::size_t>(size), '\0');
	zip_uint64_t total = 0;
	while (total < size) {
		const zip_int64_t read = zip_fread(file.get(), &bytes[static_cast<std::size_t>(total)], size - total);
		if (read <= 0) return false;
		total += static_cast<zip_uint64_t>(read);
	}
	char extra = 0;
	if (zip_fread(file.get(), &extra, 1) != 0) return false; // It held more than it declared.
	return true;

}

} // namespace

SourceIntake takeInSource(const std::filesystem::path& path, const IntakeLimits& limits) {

	SourceIntake intake;
	const auto refuse = [&](const std::string& reason) {
		intake.isAccepted = false;
		intake.refusal = reason;
		intake.parts.clear();
		return intake;
	};

	std::error_code error;
	const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
	if (error || std::filesystem::is_regular_file(status) == false) {
		return refuse("The document could not be found as an ordinary file, so nothing was read.");
	}
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error) return refuse("The document's size could not be read, so nothing was read.");
	if (size == 0) return refuse("The document is empty.");
	if (size > limits.maximumFileBytes) {
		return refuse("The document is larger than Singularity reads for a review (25 MB), so nothing was read.");
	}

	intake.identity.fileName = path.filename().u8string();
	intake.identity.sizeBytes = static_cast<long long>(size);
	const std::string extension = lowered(path.extension().u8string());
	if (extension == ".pdf") intake.identity.format = "pdf";
	else if (extension == ".txt" || extension == ".md") intake.identity.format = "text";
	else if (extension == ".docx") intake.identity.format = "docx";
	else if (extension == ".docm") {
		return refuse("The document is a Word file with macros (.docm). Singularity reviews .docx, PDF and text "
			"documents; save it as .docx without macros to review it.");
	} else {
		return refuse("Singularity reviews Word (.docx), PDF and text documents. This file is none of them.");
	}

	try {
		intake.identity.sha256 = platform::sha256_hex_digest_of_file(path);
	} catch (const std::exception&) {
		return refuse("The document could not be read to the end, so nothing was read.");
	}
	if (intake.identity.format != "docx") {
		intake.isAccepted = true;
		return intake;
	}

	/* - - - - - - */

	// A Word document is a package: every entry is checked before any is read.
	if (startsWithZipMagic(path) == false) {
		return refuse("The file is named .docx but is not a Word document package, so nothing was read.");
	}
	const ZipReader reader(path);
	zip_t* archive = reader.get();
	if (archive == nullptr) return refuse("The Word document's package could not be opened, so nothing was read.");
	const zip_int64_t entryCount = zip_get_num_entries(archive, 0);
	if (entryCount < 0 || static_cast<std::size_t>(entryCount) > limits.maximumPackageEntries) {
		return refuse("The Word document holds more parts than any agreement does, so it was not read.");
	}

	std::uint64_t totalUnpacked = 0;
	bool hasDocument = false;
	bool hasContentTypes = false;
	std::vector<std::pair<zip_uint64_t, zip_uint64_t>> toRead;
	std::vector<std::string> toReadNames;
	for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entryCount); ++index) {
		zip_stat_t entry;
		zip_stat_init(&entry);
		if (zip_stat_index(archive, index, 0, &entry) != 0 || (entry.valid & ZIP_STAT_NAME) == 0 || entry.name == nullptr) {
			return refuse("A part of the Word document could not be inspected safely, so it was not read.");
		}
		if ((entry.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 && entry.encryption_method != ZIP_EM_NONE) {
			return refuse("The Word document is encrypted. Remove its password in Word to review it.");
		}
		if ((entry.valid & ZIP_STAT_SIZE) == 0 || (entry.valid & ZIP_STAT_COMP_SIZE) == 0) {
			return refuse("A part of the Word document does not say how large it is, so it was not read.");
		}
		if (entry.size > limits.maximumUnpackedBytes - std::min(limits.maximumUnpackedBytes, totalUnpacked)) {
			return refuse("The Word document expands far beyond its size when opened, so it was not read.");
		}
		totalUnpacked += entry.size;
		if (entry.size > 1024ULL * 1024ULL && entry.size / std::max<zip_uint64_t>(entry.comp_size, 1)
			> limits.maximumExpansionRatio) {
			return refuse("A part of the Word document expands far beyond its packed size, so it was not read.");
		}

		const std::string name(entry.name);
		const std::string lowerName = lowered(name);
		// Every part the package holds, read or not, so the reading can account
		// for each one (LegalStructure's manifest).
		if (name.empty() == false && name.back() != '/') intake.packageParts.push_back(name);
		if (name == "word/document.xml") hasDocument = true;
		if (name == "[Content_Types].xml") hasContentTypes = true;
		if (lowerName.find("vbaproject.bin") != std::string::npos) intake.hasMacros = true;
		if (lowerName.rfind("word/embeddings/", 0) == 0 || lowerName.rfind("word/activex/", 0) == 0) {
			intake.hasEmbeddedObjects = true;
		}
		if (isPartToRead(name) == true) {
			if (entry.size > limits.maximumPartBytes) {
				return refuse("A part of the Word document is larger than any agreement's text, so it was not read.");
			}
			toRead.push_back({index, entry.size});
			toReadNames.push_back(name);
		}
	}
	if (hasDocument == false || hasContentTypes == false) {
		return refuse("The file has no Word document text in it, so nothing was read.");
	}

	for (std::size_t item = 0; item < toRead.size(); ++item) {
		std::string bytes;
		if (readEntry(archive, toRead[item].first, toRead[item].second, bytes) == false) {
			return refuse("A part of the Word document could not be read, so nothing was read.");
		}
		intake.parts[toReadNames[item]] = std::move(bytes);
	}

	/* - - - - - - */

	// What the package carries besides its text, said once.
	const auto relationships = intake.parts.find("word/_rels/document.xml.rels");
	if (relationships != intake.parts.end() && relationships->second.find("TargetMode=\"External\"") != std::string::npos) {
		intake.hasExternalLinks = true;
		intake.notes.push_back("The document links to places outside it. Singularity did not follow those links.");
	}
	if (intake.hasMacros == true) {
		intake.notes.push_back("The document carries macros. Singularity never runs them and read only its text.");
	}
	if (intake.hasEmbeddedObjects == true) {
		intake.notes.push_back("The document has objects embedded in it. Singularity read only its text, not those "
			"objects.");
	}
	intake.isAccepted = true;
	return intake;

}

} // namespace singularity::document_intelligence
