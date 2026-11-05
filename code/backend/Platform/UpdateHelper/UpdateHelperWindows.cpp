// singularity-update.exe: Singularity's updater on Windows (UpdateHelper.hpp).
// It stands alone - Windows' own libraries and a static runtime - because it
// runs from a copy outside the install folder, which the installer empties and
// fills again. While it installs it shows one small window of its own: it says
// what happens, and Windows asks for permission in front of the person only
// for a program whose window is in front. It leaves how it went as every
// system does (update_install::record_outcome).

#include "Platform/UpdateHelper/UpdateHelper.hpp"
#include "Platform/UpdateInstall/UpdateInstall.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace update = singularity::platform::update_helper;
namespace install = singularity::platform::update_install;

namespace {

// Singularity is given a minute to close; an install, half an hour.
constexpr DWORD kCloseWaitMilliseconds = 60 * 1000;
constexpr DWORD kInstallWaitMilliseconds = 30 * 60 * 1000;

// What the install tells the window: Windows gave permission; it ended.
constexpr UINT kPermitted = WM_APP + 1;
constexpr UINT kEnded = WM_APP + 2;

// Every sentence here is ASCII: the version is digits, letters and signs
// (request_of).
std::wstring wide(const std::string &ascii) {
	return std::wstring(ascii.begin(), ascii.end());
}

void leave(const update::Request &request, const std::string &state, const std::string &detail) {
	install::record_outcome(std::filesystem::path(request.folder), install::Outcome{state, request.version, detail});
}

// The SHA-256 of what `file` holds, read through that handle from its start,
// in lowercase hex; empty when it cannot be read.
std::string sha256_of(HANDLE file) {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
	std::string hex;
	BCRYPT_HASH_HANDLE hash = nullptr;
	if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
		LARGE_INTEGER start{};
		bool read_all = SetFilePointerEx(file, start, nullptr, FILE_BEGIN) != FALSE;
		std::vector<unsigned char> buffer(1 << 20);
		while (read_all) {
			DWORD read = 0;
			if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) read_all = false;
			else if (read == 0) break;
			else if (BCryptHashData(hash, buffer.data(), read, 0) != 0) read_all = false;
		}
		unsigned char digest[32] = {};
		if (read_all && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
			static const char *digits = "0123456789abcdef";
			for (const unsigned char byte : digest) {
				hex.push_back(digits[byte >> 4]);
				hex.push_back(digits[byte & 15]);
			}
		}
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return hex;
}

// One install, as the worker leaves it for the main thread.
struct Run {
	explicit Run(const update::Request &asked) : request(asked) {}
	const update::Request &request;
	HWND window = nullptr;
	std::string state = "failed";
	std::string detail;
	// Its installer started: Singularity's folder may have changed.
	bool installer_ran = false;
};

void failed(Run &run, const std::string &why) {
	run.state = "failed";
	run.detail = update::failure_sentence(run.request.version, why);
}

void install_update(Run &run) {
	const update::Request &request = run.request;
	// The installer is held from its check until it ends, readable by everyone
	// and changeable by no one, so the file Windows runs is the file checked.
	const HANDLE installer = CreateFileW(request.installer.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (installer == INVALID_HANDLE_VALUE) {
		failed(run, "its installer could not be opened");
		return;
	}
	if (!update::same_sha256(request.sha256, sha256_of(installer))) {
		CloseHandle(installer);
		failed(run, "its installer is not the one Legal Machines signed, so it was not run");
		return;
	}

	// Silently, into Singularity's own folder, with the rights an install for
	// every user of the computer needs: Windows asks the person whether to
	// allow it, over this updater's window.
	const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const std::wstring parameters = update::installer_parameters_of(request);
	SHELLEXECUTEINFOW execute{};
	execute.cbSize = sizeof execute;
	execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	execute.hwnd = run.window;
	execute.lpVerb = L"runas";
	execute.lpFile = request.installer.c_str();
	execute.lpParameters = parameters.c_str();
	execute.nShow = SW_HIDE;
	const BOOL started = ShellExecuteExW(&execute);
	const DWORD error = GetLastError();
	if (SUCCEEDED(com)) CoUninitialize();
	if (!started || execute.hProcess == nullptr) {
		CloseHandle(installer);
		if (error == ERROR_CANCELLED) {
			run.state = "refused";
			run.detail = update::refusal_sentence(request.version);
		} else {
			failed(run, "Windows could not start its installer (error " + std::to_string(error) + ")");
		}
		return;
	}
	run.installer_ran = true;
	if (run.window != nullptr) PostMessageW(run.window, kPermitted, 0, 0);
	const DWORD waited = WaitForSingleObject(execute.hProcess, kInstallWaitMilliseconds);
	DWORD code = 1;
	if (waited == WAIT_OBJECT_0) GetExitCodeProcess(execute.hProcess, &code);
	CloseHandle(execute.hProcess);
	CloseHandle(installer);
	if (waited != WAIT_OBJECT_0) {
		failed(run, "its installer did not finish within half an hour");
	} else if (code != 0) {
		failed(run, "its installer stopped with code " + std::to_string(code));
	} else {
		run.state = "installed";
		run.detail.clear();
	}
}

DWORD WINAPI install_in_background(void *context) {
	Run &run = *static_cast<Run *>(context);
	install_update(run);
	PostMessageW(run.window, kEnded, 0, 0);
	return 0;
}

// The window's look, one window for this program: the system's fonts at the
// system's scale, the heading a size up and semibold, and the person's light
// or dark choice for apps, unless high contrast sets every colour.
struct Look {
	HFONT heading = nullptr;
	HFONT words = nullptr;
	HBRUSH fill = nullptr;
	COLORREF background = 0;
	COLORREF text = 0;
	HWND words_label = nullptr;
	std::wstring permitted_words;
};

Look look;

bool dark_apps() {
	HIGHCONTRASTW contrast{};
	contrast.cbSize = sizeof contrast;
	if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON))
		return false;
	DWORD light = 1;
	DWORD size = sizeof light;
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		    L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
		return false;
	return light == 0;
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
	switch (message) {
	case WM_CTLCOLORSTATIC: {
		const HDC dc = reinterpret_cast<HDC>(wparam);
		SetTextColor(dc, look.text);
		SetBkColor(dc, look.background);
		return reinterpret_cast<LRESULT>(look.fill);
	}
	case kPermitted:
		SetWindowTextW(look.words_label, look.permitted_words.c_str());
		return 0;
	case kEnded:
		DestroyWindow(window);
		return 0;
	case WM_CLOSE:
		// The install decides when this window goes.
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		return DefWindowProcW(window, message, wparam, lparam);
	}
}

// The window, centred on the screen the person is at, in front when Windows
// lets it be (Singularity allowed it as it ended); nothing when it cannot be
// made, and the install goes on without it.
HWND show_window(const update::Request &request) {
	const HINSTANCE instance = GetModuleHandleW(nullptr);
	const bool dark = dark_apps();
	look.background = dark ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW);
	look.text = dark ? RGB(255, 255, 255) : GetSysColor(COLOR_WINDOWTEXT);
	look.fill = CreateSolidBrush(look.background);

	WNDCLASSEXW kind{};
	kind.cbSize = sizeof kind;
	kind.lpfnWndProc = window_procedure;
	kind.hInstance = instance;
	kind.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	kind.hbrBackground = look.fill;
	kind.lpszClassName = L"SingularityUpdate";
	if (look.fill == nullptr || RegisterClassExW(&kind) == 0) return nullptr;

	NONCLIENTMETRICSW metrics{};
	metrics.cbSize = sizeof metrics;
	if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0)) return nullptr;
	LOGFONTW words_font = metrics.lfMessageFont;
	LOGFONTW heading_font = words_font;
	heading_font.lfHeight = MulDiv(words_font.lfHeight, 4, 3);
	heading_font.lfWeight = FW_SEMIBOLD;
	look.words = CreateFontIndirectW(&words_font);
	look.heading = CreateFontIndirectW(&heading_font);
	if (look.words == nullptr || look.heading == nullptr) return nullptr;

	const std::wstring heading = wide(update::window_heading(request));
	const std::wstring words = wide(update::window_words(request, false));
	look.permitted_words = wide(update::window_words(request, true));

	// Measured at the system's scale; the words before permission are the
	// longer, so the window keeps its size when they change.
	const HDC screen = GetDC(nullptr);
	if (screen == nullptr) return nullptr;
	const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
	const auto scaled = [dpi](int pixels) { return MulDiv(pixels, dpi, 96); };
	const int width = scaled(420);
	const int padding = scaled(24);
	const int gap = scaled(8);
	const auto height_of = [screen, width, padding](HFONT font, const std::wstring &text) {
		RECT area{0, 0, width - 2 * padding, 0};
		const HGDIOBJ previous = SelectObject(screen, font);
		DrawTextW(screen, text.c_str(), -1, &area, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
		SelectObject(screen, previous);
		return static_cast<int>(area.bottom - area.top);
	};
	const int heading_height = height_of(look.heading, heading);
	const int words_height = height_of(look.words, words);
	ReleaseDC(nullptr, screen);

	const DWORD style = WS_OVERLAPPED | WS_CAPTION;
	const DWORD extended = WS_EX_APPWINDOW;
	RECT frame{0, 0, width, padding + heading_height + gap + words_height + padding};
	AdjustWindowRectEx(&frame, style, FALSE, extended);
	const int frame_width = frame.right - frame.left;
	const int frame_height = frame.bottom - frame.top;
	POINT pointer{};
	GetCursorPos(&pointer);
	MONITORINFO monitor{};
	monitor.cbSize = sizeof monitor;
	if (!GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY), &monitor)) return nullptr;
	const RECT &work = monitor.rcWork;
	const HWND window = CreateWindowExW(extended, kind.lpszClassName, L"Singularity", style,
		work.left + (work.right - work.left - frame_width) / 2, work.top + (work.bottom - work.top - frame_height) / 2,
		frame_width, frame_height, nullptr, nullptr, instance, nullptr);
	if (window == nullptr) return nullptr;
	if (dark) {
		// DWMWA_USE_IMMERSIVE_DARK_MODE: a dark title bar where Windows has one.
		const BOOL on = TRUE;
		DwmSetWindowAttribute(window, 20, &on, sizeof on);
	}
	const auto label = [&](HFONT font, const std::wstring &text, int top, int height) {
		const HWND control = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
			padding, top, width - 2 * padding, height, window, nullptr, instance, nullptr);
		if (control != nullptr) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
		return control;
	};
	label(look.heading, heading, padding, heading_height);
	look.words_label = label(look.words, words, padding + heading_height + gap, words_height);
	// Singularity's own icon in the taskbar.
	// (Not "small": Windows' RPC headers make that word a type.)
	HICON big_icon = nullptr;
	HICON small_icon = nullptr;
	ExtractIconExW(request.application.c_str(), 0, &big_icon, &small_icon, 1);
	if (big_icon != nullptr) SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
	if (small_icon != nullptr) SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
	ShowWindow(window, SW_SHOWNORMAL);
	UpdateWindow(window);
	SetForegroundWindow(window);
	return window;
}

// Singularity again, from this updater, which runs as the person and never
// elevated: so does Singularity, and Explorer can drop files into it. Whether
// it started, or was not to be started.
bool start_again(const update::Request &request) {
	if (!request.relaunch) return true;
	std::wstring line = update::command_line_of(request.application, {});
	const auto slash = request.application.find_last_of(L"\\/");
	const std::wstring folder = slash == std::wstring::npos ? std::wstring() : request.application.substr(0, slash);
	STARTUPINFOW startup{};
	startup.cb = sizeof startup;
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(request.application.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
		    folder.empty() ? nullptr : folder.c_str(), &startup, &process))
		return false;
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}

// An install that ran and failed may leave no Singularity to say why: the
// person is told here, and shown its installer.
void tell_to_install_by_hand(const update::Request &request, const std::string &failure) {
	const std::wstring words = wide(update::by_hand_words(failure));
	MessageBoxW(nullptr, words.c_str(), L"Singularity", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
	const std::wstring select = L"/select,\"" + request.installer + L"\"";
	ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
}

int apply(const update::Request &request) {
	// Singularity closes first: its files are about to be replaced. A process
	// that cannot be opened has already gone.
	if (const HANDLE singularity = OpenProcess(SYNCHRONIZE, FALSE, request.process_id)) {
		const DWORD waited = WaitForSingleObject(singularity, kCloseWaitMilliseconds);
		CloseHandle(singularity);
		if (waited != WAIT_OBJECT_0) {
			leave(request, "failed", update::failure_sentence(request.version, "Singularity did not close in time"));
			return 1;
		}
	}

	// The install runs beside the window, which answers Windows meanwhile.
	Run run(request);
	run.window = show_window(request);
	const HANDLE worker = run.window != nullptr ? CreateThread(nullptr, 0, install_in_background, &run, 0, nullptr) : nullptr;
	if (worker != nullptr) {
		MSG message;
		while (GetMessageW(&message, nullptr, 0, 0) > 0) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		WaitForSingleObject(worker, INFINITE);
		CloseHandle(worker);
	} else {
		install_update(run);
		if (run.window != nullptr) {
			DestroyWindow(run.window);
			MSG quit;
			PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);
		}
	}

	leave(request, run.state, run.detail);
	start_again(request);
	std::error_code error;
	if (run.installer_ran && run.state == "failed" && !std::filesystem::exists(std::filesystem::path(request.application), error))
		tell_to_install_by_hand(request, run.detail);
	return run.state == "installed" ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	// Measured and drawn at the system's scale, never stretched by Windows.
	SetProcessDPIAware();
	int count = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &count);
	if (argv == nullptr) return 2;
	const std::vector<std::wstring> arguments(argv + 1, argv + count);
	LocalFree(argv);
	const auto request = update::request_of(arguments);
	if (!request) return 2;
	return apply(*request);
}
