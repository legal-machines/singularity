#include "Provider/CredentialStore/CredentialStore.hpp"

#include "Platform/AppSettings/AppSettings.hpp"

#import <Foundation/Foundation.h>

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <sys/stat.h>

namespace singularity::platform {
namespace {

std::filesystem::path credential_path(const std::string &provider_id) {
  if (provider_id.empty() || !std::all_of(provider_id.begin(), provider_id.end(), [](unsigned char value) {
        return std::isalnum(value) || value == '_' || value == '-';
      })) {
    throw std::runtime_error("Invalid provider credential identifier.");
  }
  return app_settings_directory() / "credentials" / (provider_id + ".credential");
}

NSURL *file_url(const std::filesystem::path &path) {
  return [NSURL fileURLWithPath:@(path.string().c_str())];
}

std::string native_error_summary(NSError *error) {
  if (error == nil) return "domain=unknown code=0";
  const char *domain = error.domain.UTF8String;
  return "domain=" + std::string(domain == nullptr ? "unknown" : domain)
      + " code=" + std::to_string(error.code);
}

// What went wrong, said to the person it happens to.
//
// "domain=NSCocoaErrorDomain code=257" is what this used to tell a lawyer whose
// Mac had simply locked itself. It is true and it is useless: it names no cause
// and no remedy. The cause is worth naming because the remedy is one action.
//
// A credential is stored with NSFileProtectionComplete, so while the Mac is
// locked it genuinely cannot be read - that is the protection working, not a
// fault. The technical summary stays, at the end, for whoever has to diagnose
// something rarer. No credential VALUE is ever part of any of this text.
std::string readable_credential_failure(NSError *error, bool reading) {
  const std::string detail = " (" + native_error_summary(error) + ")";
  const bool cocoa = error != nil && [error.domain isEqualToString:NSCocoaErrorDomain];
  if (cocoa && (error.code == NSFileReadNoPermissionError || error.code == NSFileWriteNoPermissionError)) {
    return std::string("This Mac is locked, so Singularity cannot ")
        + (reading ? "read the saved model credential" : "save the model credential")
        + ". Unlock the Mac and try again." + detail;
  }
  if (cocoa && error.code == NSFileReadCorruptFileError) {
    return "The saved model credential is damaged. Enter it again in Settings to replace it." + detail;
  }
  if (cocoa && error.code == NSFileWriteOutOfSpaceError) {
    return "There is no room on this disk to save the model credential. Free some space and try again."
        + detail;
  }
  return std::string("Singularity could not ") + (reading ? "read" : "save")
      + " the model credential on this Mac. Nothing was sent and nothing was changed." + detail;
}

} // namespace

std::string credential_unreadable_explanation(const std::string &provider_name) {
  return provider_name + " could not be opened on this Mac. Unlock the Mac, or reconnect " + provider_name + ".";
}

std::optional<std::string> get_provider_credential(const std::string &provider_id) {
  @autoreleasepool {
    const auto path = credential_path(provider_id);
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfURL:file_url(path)
                                        options:NSDataReadingMappedIfSafe
                                          error:&error];
    if (data == nil) {
      if (error != nil && error.code != NSFileReadNoSuchFileError) {
        // Locked: unreadable for now, and readable again once the Mac is
        // unlocked - told apart, so a reader can wait.
        if ([error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSFileReadNoPermissionError)
          throw CredentialUnavailable(readable_credential_failure(error, true));
        throw std::runtime_error(readable_credential_failure(error, true));
      }
      return std::nullopt;
    }
    return std::string(static_cast<const char *>(data.bytes), data.length);
  }
}

void set_provider_credential(const std::string &provider_id, const std::string &credential) {
  @autoreleasepool {
    const auto path = credential_path(provider_id);
    std::filesystem::create_directories(path.parent_path());
    NSData *data = [NSData dataWithBytes:credential.data() length:credential.size()];
    NSError *error = nil;
    const auto options = NSDataWritingAtomic | NSDataWritingFileProtectionComplete;
    if (![data writeToURL:file_url(path) options:options error:&error]) {
      throw std::runtime_error(readable_credential_failure(error, false));
    }
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
      [[NSFileManager defaultManager] removeItemAtURL:file_url(path) error:nil];
      throw std::runtime_error("The local provider credential permissions could not be secured.");
    }
    [file_url(path) setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
  }
}

void delete_provider_credential(const std::string &provider_id) {
  @autoreleasepool {
    const auto path = credential_path(provider_id);
    NSError *error = nil;
    if (![[NSFileManager defaultManager] removeItemAtURL:file_url(path) error:&error] &&
        error.code != NSFileNoSuchFileError) {
      throw std::runtime_error("The local provider credential could not be removed ("
          + native_error_summary(error) + ").");
    }
  }
}

} // namespace singularity::platform
