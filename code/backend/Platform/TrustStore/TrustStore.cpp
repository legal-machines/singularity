#include "Platform/TrustStore/TrustStore.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>

#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <thread>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace singularity::platform::trust_store {

#if defined(_WIN32)

namespace {

constexpr DWORD kEncoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

PCCERT_CONTEXT certificate_of(const std::string &der) {
	if (der.empty()) return nullptr;
	return CertCreateCertificateContext(kEncoding, reinterpret_cast<const BYTE *>(der.data()), static_cast<DWORD>(der.size()));
}

// One of the current user's certificate stores: "Root", the trusted roots, or
// "TrustedPeople", where an earlier Singularity kept the certificate.
HCERTSTORE user_store(const wchar_t *name, bool writable) {
	DWORD flags = CERT_SYSTEM_STORE_CURRENT_USER;
	if (!writable) flags |= CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG;
	return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, flags, name);
}

bool holds(const wchar_t *name, PCCERT_CONTEXT certificate) {
	bool found = false;
	if (HCERTSTORE store = user_store(name, false)) {
		PCCERT_CONTEXT existing = CertFindCertificateInStore(store, kEncoding, 0, CERT_FIND_EXISTING, certificate, nullptr);
		if (existing != nullptr) {
			found = true;
			CertFreeCertificateContext(existing);
		}
		CertCloseStore(store, 0);
	}
	return found;
}

// Windows asks about the Trusted Root store in its own security warning, a
// dialog of this process with no window of its own above it: it opened behind
// Singularity's window, and the person, who never saw it, was left on
// "Connecting…". So while the store changes, Singularity's window owns each
// dialog of this process that opens with no owner: the question stands in
// front of it, and comes forward while the person is in Singularity. The
// watcher waits on the windows this process shows, never polling, and ends
// with the call.

// Whether a window belongs to this process.
bool is_ours(HWND window) {
	DWORD process = 0;
	return window != nullptr && GetWindowThreadProcessId(window, &process) != 0 && process == GetCurrentProcessId();
}

BOOL CALLBACK first_unowned(HWND window, LPARAM found) {
	if (!is_ours(window) || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
	*reinterpret_cast<HWND *>(found) = window;
	return FALSE;
}

// The window the person sees Singularity in: the foreground one when it is
// this process's, else this process's first visible window nothing owns.
HWND singularity_window() {
	const HWND foreground = GetForegroundWindow();
	if (is_ours(foreground)) return GetAncestor(foreground, GA_ROOTOWNER);
	HWND found = nullptr;
	EnumWindows(first_unowned, reinterpret_cast<LPARAM>(&found));
	return found;
}

// A dialog opened on its own, as Windows' security warning is: a top-level
// "#32770" window that nothing owns.
bool is_unowned_dialog(HWND window) {
	wchar_t name[16] = {};
	if (GetClassNameW(window, name, 16) == 0 || std::wcscmp(name, L"#32770") != 0) return false;
	return GetAncestor(window, GA_PARENT) == GetDesktopWindow() && GetWindow(window, GW_OWNER) == nullptr;
}

class PromptInFront;
// The watcher whose thread this is: a window event names no watcher.
thread_local PromptInFront *watching = nullptr;

class PromptInFront final {
public:
	explicit PromptInFront(HWND owner) : owner_(owner) {
		if (owner_ == nullptr) return;
		watcher_ = std::thread([this] { watch(); });
		std::unique_lock<std::mutex> lock(mutex_);
		started_.wait(lock, [this] { return thread_ != 0; });
	}

	~PromptInFront() {
		if (!watcher_.joinable()) return;
		PostThreadMessageW(thread_, WM_QUIT, 0, 0);
		watcher_.join();
	}

	PromptInFront(const PromptInFront &) = delete;
	PromptInFront &operator=(const PromptInFront &) = delete;

private:
	static void CALLBACK shown(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
		if (object != OBJID_WINDOW || child != CHILDID_SELF || watching == nullptr) return;
		watching->bring_forward(window);
	}

	void bring_forward(HWND dialog) const {
		if (!is_unowned_dialog(dialog) || !IsWindow(owner_)) return;
		// Owned, it stays above Singularity's window, and comes and goes with it.
		SetWindowLongPtrW(dialog, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner_));
		if (is_ours(GetForegroundWindow())) {
			SetWindowPos(dialog, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
			SetForegroundWindow(dialog);
			return;
		}
		// The person is in another program: Singularity's button on the taskbar
		// flashes until they come back to it, and the question waits in front.
		FLASHWINFO flash{};
		flash.cbSize = sizeof(flash);
		flash.hwnd = owner_;
		flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
		FlashWindowEx(&flash);
	}

	void watch() {
		watching = this;
		MSG message;
		// The thread's message queue, which the end's WM_QUIT reaches.
		PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
		const HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, shown,
			GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			thread_ = GetCurrentThreadId();
		}
		started_.notify_all();
		while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
		if (hook != nullptr) UnhookWinEvent(hook);
		watching = nullptr;
	}

	HWND owner_;
	std::thread watcher_;
	std::mutex mutex_;
	std::condition_variable started_;
	DWORD thread_ = 0;
};

bool remove_from(const wchar_t *name, PCCERT_CONTEXT certificate) {
	bool removed = false;
	if (HCERTSTORE store = user_store(name, true)) {
		PCCERT_CONTEXT existing = CertFindCertificateInStore(store, kEncoding, 0, CERT_FIND_EXISTING, certificate, nullptr);
		// CertDeleteCertificateFromStore frees the context it is given. Windows
		// asks before a root goes, in front of Singularity's window too.
		if (existing != nullptr) {
			const PromptInFront in_front(std::wcscmp(name, L"Root") == 0 ? singularity_window() : nullptr);
			removed = CertDeleteCertificateFromStore(existing) != FALSE;
		}
		CertCloseStore(store, 0);
	}
	return removed;
}

} // namespace

TrustState trust_state(const std::string &certificate_der) {
	PCCERT_CONTEXT certificate = certificate_of(certificate_der);
	if (certificate == nullptr) return TrustState::not_trusted;
	const bool found = holds(L"Root", certificate);
	CertFreeCertificateContext(certificate);
	return found ? TrustState::trusted : TrustState::not_trusted;
}

TrustOutcome trust_for_this_user(const std::string &certificate_der) {
	TrustOutcome outcome;
	PCCERT_CONTEXT certificate = certificate_of(certificate_der);
	if (certificate == nullptr) {
		outcome.problem = "Singularity's certificate for this computer could not be read.";
		return outcome;
	}
	// An earlier Singularity kept it in Trusted People, which software that
	// inspects encrypted connections (an antivirus) does not read.
	(void)remove_from(L"TrustedPeople", certificate);
	if (holds(L"Root", certificate)) {
		CertFreeCertificateContext(certificate);
		outcome.state = TrustState::trusted;
		return outcome;
	}
	HCERTSTORE store = user_store(L"Root", true);
	if (store == nullptr) {
		CertFreeCertificateContext(certificate);
		outcome.problem = "Windows did not open this user's list of trusted certificates.";
		return outcome;
	}
	// Windows asks the person here, in its own security warning, in front of
	// Singularity's window (PromptInFront).
	BOOL added = FALSE;
	DWORD error = 0;
	{
		const PromptInFront in_front(singularity_window());
		added = CertAddCertificateContextToStore(store, certificate, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
		error = added ? 0 : GetLastError();
	}
	CertCloseStore(store, 0);
	const bool trusted = holds(L"Root", certificate);
	CertFreeCertificateContext(certificate);
	if (trusted) {
		outcome.state = TrustState::trusted;
		return outcome;
	}
	if (error == ERROR_CANCELLED || static_cast<HRESULT>(error) == HRESULT_FROM_WIN32(ERROR_CANCELLED)
		|| static_cast<HRESULT>(error) == static_cast<HRESULT>(0x8010006EL)) {
		outcome.declined = true;
		return outcome;
	}
	outcome.problem = "Windows did not add Singularity's certificate (error " + std::to_string(error) + ").";
	return outcome;
}

bool forget(const std::string &certificate_der) {
	PCCERT_CONTEXT certificate = certificate_of(certificate_der);
	if (certificate == nullptr) return false;
	const bool from_people = remove_from(L"TrustedPeople", certificate);
	const bool from_root = remove_from(L"Root", certificate);
	CertFreeCertificateContext(certificate);
	return from_people || from_root;
}

#elif defined(__APPLE__)

namespace {

SecCertificateRef certificate_of(const std::string &der) {
	if (der.empty()) return nullptr;
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(der.data()), static_cast<CFIndex>(der.size()));
	if (data == nullptr) return nullptr;
	SecCertificateRef certificate = SecCertificateCreateWithData(kCFAllocatorDefault, data);
	CFRelease(data);
	return certificate;
}

} // namespace

TrustState trust_state(const std::string &certificate_der) {
	SecCertificateRef certificate = certificate_of(certificate_der);
	if (certificate == nullptr) return TrustState::not_trusted;
	CFArrayRef settings = nullptr;
	const OSStatus status = SecTrustSettingsCopyTrustSettings(certificate, kSecTrustSettingsDomainUser, &settings);
	if (settings != nullptr) CFRelease(settings);
	CFRelease(certificate);
	return status == errSecSuccess ? TrustState::trusted : TrustState::not_trusted;
}

TrustOutcome trust_for_this_user(const std::string &certificate_der) {
	TrustOutcome outcome;
	if (trust_state(certificate_der) == TrustState::trusted) {
		outcome.state = TrustState::trusted;
		return outcome;
	}
	SecCertificateRef certificate = certificate_of(certificate_der);
	if (certificate == nullptr) {
		outcome.problem = "Singularity's certificate for this computer could not be read.";
		return outcome;
	}
	const void *keys[] = {kSecClass, kSecValueRef};
	const void *values[] = {kSecClassCertificate, certificate};
	CFDictionaryRef item = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	const OSStatus added = SecItemAdd(item, nullptr);
	CFRelease(item);
	if (added != errSecSuccess && added != errSecDuplicateItem) {
		CFRelease(certificate);
		outcome.problem = "macOS did not keep Singularity's certificate in the login keychain.";
		return outcome;
	}
	// macOS asks the person for their password here.
	const OSStatus trusted = SecTrustSettingsSetTrustSettings(certificate, kSecTrustSettingsDomainUser, nullptr);
	CFRelease(certificate);
	if (trusted == errSecSuccess) {
		outcome.state = TrustState::trusted;
		return outcome;
	}
	if (trusted == errAuthorizationCanceled || trusted == errSecAuthFailed) {
		outcome.declined = true;
		return outcome;
	}
	outcome.problem = "macOS did not trust Singularity's certificate (error " + std::to_string(trusted) + ").";
	return outcome;
}

bool forget(const std::string &certificate_der) {
	SecCertificateRef certificate = certificate_of(certificate_der);
	if (certificate == nullptr) return false;
	const OSStatus status = SecTrustSettingsRemoveTrustSettings(certificate, kSecTrustSettingsDomainUser);
	CFRelease(certificate);
	return status == errSecSuccess;
}

#else

TrustState trust_state(const std::string &) {
	return TrustState::unsupported;
}

TrustOutcome trust_for_this_user(const std::string &) {
	TrustOutcome outcome;
	outcome.state = TrustState::unsupported;
	outcome.problem = "Word, Excel, PowerPoint and Outlook do not run on this system.";
	return outcome;
}

bool forget(const std::string &) {
	return false;
}

#endif

} // namespace singularity::platform::trust_store
