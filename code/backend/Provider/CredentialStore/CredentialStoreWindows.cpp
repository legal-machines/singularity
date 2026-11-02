#include "Provider/CredentialStore/CredentialStore.hpp"

#include "Platform/AppSettings/AppSettings.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dpapi.h>

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace singularity::platform {
namespace {
std::filesystem::path credential_path(const std::string &provider_id) {
  if (provider_id.empty() || !std::all_of(provider_id.begin(), provider_id.end(), [](unsigned char value) {
        return std::isalnum(value) || value == '_' || value == '-';
      })) {
    throw std::runtime_error("Invalid provider credential identifier.");
  }
  return computer_records_directory() / "credentials" / (provider_id + ".credential");
}
} // namespace

// DPAPI ties the file to this Windows account; there is nothing to unlock,
// so a new key is the way back.
std::string credential_unreadable_explanation(const std::string &provider_name) {
  return provider_name + " could not be opened on this computer. Reconnect " + provider_name + ".";
}

std::optional<std::string> get_provider_credential(const std::string &provider_id) {
  std::ifstream input(credential_path(provider_id), std::ios::binary);
  if (!input) return std::nullopt;
  std::vector<unsigned char> encrypted((std::istreambuf_iterator<char>(input)), {});
  DATA_BLOB source{static_cast<DWORD>(encrypted.size()), encrypted.data()};
  DATA_BLOB plain{};
  if (!CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
    throw std::runtime_error("The local provider credential could not be decrypted.");
  }
  std::string result(reinterpret_cast<char *>(plain.pbData), plain.cbData);
  SecureZeroMemory(plain.pbData, plain.cbData);
  LocalFree(plain.pbData);
  return result;
}

void set_provider_credential(const std::string &provider_id, const std::string &credential) {
  DATA_BLOB source{static_cast<DWORD>(credential.size()),
                   reinterpret_cast<BYTE *>(const_cast<char *>(credential.data()))};
  DATA_BLOB encrypted{};
  if (!CryptProtectData(&source, L"Singularity provider credential", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
    throw std::runtime_error("The local provider credential could not be encrypted.");
  }
  const auto path = credential_path(provider_id);
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(encrypted.pbData), encrypted.cbData);
  output.close();
  SecureZeroMemory(encrypted.pbData, encrypted.cbData);
  LocalFree(encrypted.pbData);
  if (!output) throw std::runtime_error("The local provider credential could not be stored.");
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::rename(temporary, path);
}

void delete_provider_credential(const std::string &provider_id) {
  std::error_code error;
  std::filesystem::remove(credential_path(provider_id), error);
  if (error) throw std::runtime_error("The local provider credential could not be removed.");
}

} // namespace singularity::platform
