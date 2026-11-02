#include "Platform/TrashFile/TrashFile.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <mutex>
#include <thread>

namespace singularity::platform {
namespace {

std::mutex override_mutex;
std::optional<TrashResult> result_override;

std::wstring wide_path(const std::string &utf8) {
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
		static_cast<int>(utf8.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring wide(static_cast<std::size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
		wide.data(), length);
	// Project paths join generic relative paths ("folder/file.docx"); the
	// shell's parser wants Windows separators throughout.
	for (wchar_t &character : wide)
		if (character == L'/') character = L'\\';
	return wide;
}

// The shell deletes an item permanently when its volume has no Recycle Bin
// (most network shares, some removable drives) - silently, with no UI. An
// ordinary Remove must never become a permanent delete, so this sink vetoes
// every delete the shell is not going to recycle; the file stays untouched.
class RecycleOnly final : public IFileOperationProgressSink {
public:
	bool refused_permanent_delete = false;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
		if (object == nullptr) return E_POINTER;
		if (iid == IID_IUnknown || iid == __uuidof(IFileOperationProgressSink)) {
			*object = static_cast<IFileOperationProgressSink *>(this);
			AddRef();
			return S_OK;
		}
		*object = nullptr;
		return E_NOINTERFACE;
	}
	// Lives on the caller's stack for exactly one operation.
	ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }

	HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem *) override {
		if ((flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) == 0) {
			refused_permanent_delete = true;
			return HRESULT_FROM_WIN32(ERROR_CANCELLED);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
	HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT,
		IShellItem *) override {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT,
		IShellItem *) override {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *, HRESULT, IShellItem *) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT,
		IShellItem *) override {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
	HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};

template <typename Interface> struct Released {
	Interface *pointer = nullptr;
	~Released() {
		if (pointer != nullptr) pointer->Release();
	}
};

// IFileOperation wants a single-threaded apartment, which the calling thread
// may not be: the operation runs on a thread of its own that owns one.
TrashResult recycle(const std::wstring &path) {
	if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
		return TrashResult::Failed;
	}
	struct Apartment {
		~Apartment() { CoUninitialize(); }
	} apartment;

	Released<IShellItem> item;
	if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item.pointer)))) {
		return TrashResult::Failed;
	}
	Released<IFileOperation> operation;
	if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation.pointer)))) {
		return TrashResult::Failed;
	}
	if (FAILED(operation.pointer->SetOperationFlags(
			FOF_ALLOWUNDO | FOF_NO_UI | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE))) {
		return TrashResult::Failed;
	}
	RecycleOnly sink;
	DWORD cookie = 0;
	if (FAILED(operation.pointer->Advise(&sink, &cookie))) return TrashResult::Failed;
	const HRESULT queued = operation.pointer->DeleteItem(item.pointer, nullptr);
	const HRESULT performed = SUCCEEDED(queued) ? operation.pointer->PerformOperations() : queued;
	BOOL aborted = FALSE;
	operation.pointer->GetAnyOperationsAborted(&aborted);
	operation.pointer->Unadvise(cookie);

	if (FAILED(performed) || aborted || sink.refused_permanent_delete) return TrashResult::Failed;
	// The shell reported success; the file being gone from its place is the proof.
	return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? TrashResult::Succeeded
	                                                                   : TrashResult::Failed;
}

} // namespace

const char *trash_name() { return "Recycle Bin"; }

TrashResult move_to_trash(const std::string &path) {
	{
		const std::lock_guard<std::mutex> lock(override_mutex);
		if (result_override) return *result_override;
	}

	const std::wstring wide = wide_path(path);
	if (wide.empty()) return TrashResult::Failed;
	TrashResult result = TrashResult::Failed;
	std::thread([&] { result = recycle(wide); }).join();
	return result;
}

void set_trash_result_override_for_testing(std::optional<TrashResult> value) {
	const std::lock_guard<std::mutex> lock(override_mutex);
	result_override = value;
}

} // namespace singularity::platform
