#pragma once

#include <filesystem>
#include <string>

namespace singularity::platform {

// Streams the file in fixed-size chunks, so multi-hundred-MB executables
// never need to be loaded into memory at once.
std::string sha256_hex_digest_of_file(const std::filesystem::path &path);

// Hashes an in-memory byte string without writing sensitive request data to
// disk. Used for opaque final provider-payload evidence.
std::string sha256_hex_digest(const std::string &value);

} // namespace singularity::platform
