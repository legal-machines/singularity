#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "DocumentIntelligence/Types/LegalDocument.hpp"

namespace singularity::document_intelligence {

// How much of a source this layer is willing to open. A professional document
// is untrusted input: a file far larger than any agreement, or a Word package
// that expands to gigabytes when read, is refused before it is read.
struct IntakeLimits {
	std::uintmax_t maximumFileBytes = 25ULL * 1024ULL * 1024ULL;
	std::size_t maximumPackageEntries = 2000;
	std::uint64_t maximumUnpackedBytes = 128ULL * 1024ULL * 1024ULL;
	std::uint64_t maximumPartBytes = 40ULL * 1024ULL * 1024ULL;
	// How many times larger an entry may become when unpacked. Text compresses
	// well, but not a thousand times.
	std::uint64_t maximumExpansionRatio = 200;
};

// A source taken in: who it is, the parts of its package the structure reader
// needs (a Word document's XML parts, by their names in the package), and what
// about it a person must know - active content, links that point outside it,
// objects embedded in it.
struct SourceIntake {
	bool isAccepted = false;
	// Why it was refused, in words a lawyer reads.
	std::string refusal;
	SourceIdentity identity;
	// The parts this layer reads, by name, with their bytes.
	std::map<std::string, std::string> parts;
	// Every part the package holds, read or not, in the package's order.
	std::vector<std::string> packageParts;
	std::vector<std::string> notes;
	bool hasMacros = false;
	bool hasExternalLinks = false;
	bool hasEmbeddedObjects = false;
};

// Reads `path` and nothing else: never follows a link or a reference out of
// it, never writes. A .docx is opened as a package and checked entry by entry
// before a byte of it is unpacked; a .pdf and a .txt or .md are taken as
// files, their text read later by the extractor that knows them.
SourceIntake takeInSource(const std::filesystem::path& path, const IntakeLimits& limits = IntakeLimits());

} // namespace singularity::document_intelligence
