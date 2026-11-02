#include "Platform/DynamicLibrary/DynamicLibrary.hpp"

#if defined(_WIN32)
#include <windows.h>
#include "Platform/WindowsText/WindowsText.hpp"
#else
#include <dlfcn.h>
#endif

namespace singularity::platform {

std::optional<DynamicLibrary> DynamicLibrary::open_system(const char *name) {
#if defined(_WIN32)
	std::wstring wide;
	try {
		wide = windows::utf8_to_wide(name);
	} catch (...) {
		return std::nullopt;
	}
	HMODULE module = LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (module == nullptr) return std::nullopt;
	return DynamicLibrary(reinterpret_cast<void *>(module));
#else
	void *handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
	if (handle == nullptr) return std::nullopt;
	return DynamicLibrary(handle);
#endif
}

DynamicLibrary::DynamicLibrary(DynamicLibrary &&other) noexcept : handle_(other.handle_) {
	other.handle_ = nullptr;
}

namespace {

void close_library(void *handle) noexcept {
	if (handle == nullptr) return;
#if defined(_WIN32)
	FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
	::dlclose(handle);
#endif
}

} // namespace

DynamicLibrary &DynamicLibrary::operator=(DynamicLibrary &&other) noexcept {
	if (this != &other) {
		close_library(handle_);
		handle_ = other.handle_;
		other.handle_ = nullptr;
	}
	return *this;
}

DynamicLibrary::~DynamicLibrary() { close_library(handle_); }

void *DynamicLibrary::symbol(const char *name) const noexcept {
	if (handle_ == nullptr) return nullptr;
#if defined(_WIN32)
	return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
#else
	return ::dlsym(handle_, name);
#endif
}

} // namespace singularity::platform
