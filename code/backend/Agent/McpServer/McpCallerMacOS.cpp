#include "Agent/McpServer/McpCaller.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <libproc.h>
#include <unistd.h>

#include <string>

namespace singularity::agents {
namespace {

std::string text_of(CFStringRef value) {
	if (value == nullptr) return "";
	const CFIndex length = CFStringGetLength(value);
	const CFIndex most = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
	std::string buffer(static_cast<std::size_t>(most), '\0');
	if (CFStringGetCString(value, buffer.data(), most, kCFStringEncodingUTF8) == false) return "";
	buffer.resize(std::char_traits<char>::length(buffer.c_str()));
	return buffer;
}

} // namespace

AiAppCaller identify_caller() {
	AiAppCaller caller;
	const pid_t parent = ::getppid();
	char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
	if (proc_pidpath(parent, path, sizeof(path)) > 0) caller.path = path;

	CFNumberRef pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &parent);
	const void *keys[] = {kSecGuestAttributePid};
	const void *values[] = {pid};
	CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	SecCodeRef code = nullptr;
	const OSStatus found = SecCodeCopyGuestWithAttributes(nullptr, attributes, kSecCSDefaultFlags, &code);
	CFRelease(attributes);
	CFRelease(pid);
	if (found != errSecSuccess || code == nullptr) return caller;

	// Only a signature that chains to Apple counts: Developer ID, the App
	// Store, or Apple's own programs. Ad hoc signatures name no one.
	SecRequirementRef requirement = nullptr;
	const OSStatus made = SecRequirementCreateWithString(CFSTR("anchor apple generic"), kSecCSDefaultFlags, &requirement);
	const bool isValid = made == errSecSuccess && SecCodeCheckValidity(code, kSecCSDefaultFlags, requirement) == errSecSuccess;
	if (requirement != nullptr) CFRelease(requirement);

	SecStaticCodeRef staticCode = nullptr;
	CFDictionaryRef information = nullptr;
	if (isValid == true && SecCodeCopyStaticCode(code, kSecCSDefaultFlags, &staticCode) == errSecSuccess
			&& SecCodeCopySigningInformation(staticCode, kSecCSSigningInformation, &information) == errSecSuccess && information != nullptr) {
		const auto team = static_cast<CFStringRef>(CFDictionaryGetValue(information, kSecCodeInfoTeamIdentifier));
		const auto identifier = static_cast<CFStringRef>(CFDictionaryGetValue(information, kSecCodeInfoIdentifier));
		caller.team_id = text_of(team);
		caller.signing_id = text_of(identifier);
		const auto certificates = static_cast<CFArrayRef>(CFDictionaryGetValue(information, kSecCodeInfoCertificates));
		if (certificates != nullptr && CFArrayGetCount(certificates) > 0) {
			const auto leaf = static_cast<SecCertificateRef>(const_cast<void *>(CFArrayGetValueAtIndex(certificates, 0)));
			CFStringRef summary = SecCertificateCopySubjectSummary(leaf);
			caller.signer = text_of(summary);
			if (summary != nullptr) CFRelease(summary);
		}
		// Apple's own programs carry no team; they are Apple's.
		if (caller.team_id.empty() == true && caller.signer == "Software Signing") caller.team_id = "apple";
		caller.is_signer_verified = caller.signer.empty() == false && caller.team_id.empty() == false;
	}
	if (information != nullptr) CFRelease(information);
	if (staticCode != nullptr) CFRelease(staticCode);
	CFRelease(code);
	return caller;
}

} // namespace singularity::agents
