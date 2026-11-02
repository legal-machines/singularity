#pragma once

#include "Agent/AiAppChannel/AiAppChannel.hpp"

#include <filesystem>
#include <string>

namespace singularity::agents {

// One program as Windows knows it (McpCallerWindows.cpp): its path, and its
// Authenticode signature when Windows verifies one, embedded in the file or in
// one of Windows' own catalogs, from a certificate a root this computer trusts
// issued. The signer is the certificate's subject name ("Anthropic, PBC"), the
// team its whole subject ("CN=…, O=…, L=…, S=…, C=…"), and the signing id the
// package family name of a packaged app, otherwise the program's file name in
// lower case. A program Windows cannot verify is named by its path alone and
// called unverified. Revocation is never asked of the network: the AI app is
// waiting, and the label is a consistency check, not a defence
// (Agent/McpServer/McpCaller.hpp).
AiAppCaller identify_program(const std::filesystem::path &program, const std::string &package_family_name = "");

} // namespace singularity::agents
