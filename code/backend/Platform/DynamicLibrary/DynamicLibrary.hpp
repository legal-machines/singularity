#pragma once

#include <optional>
#include <string>

namespace singularity::platform {

// A shared library opened at run time - used for optional system components
// (GPU drivers) that a build must not link against, because their absence
// on a machine is an ordinary, reportable fact rather than a load failure.
class DynamicLibrary final {
public:
	// Opens a library that belongs to the operating system or a driver
	// installation. On Windows only the system directory is searched, so a
	// same-named file in the working or application directory can never be
	// loaded instead. std::nullopt when it is not installed.
	static std::optional<DynamicLibrary> open_system(const char *name);

	DynamicLibrary(DynamicLibrary &&other) noexcept;
	DynamicLibrary &operator=(DynamicLibrary &&other) noexcept;
	DynamicLibrary(const DynamicLibrary &) = delete;
	DynamicLibrary &operator=(const DynamicLibrary &) = delete;
	~DynamicLibrary();

	void *symbol(const char *name) const noexcept;
	template <typename Function>
	Function function(const char *name) const noexcept {
		return reinterpret_cast<Function>(symbol(name));
	}

private:
	explicit DynamicLibrary(void *handle) noexcept : handle_(handle) {}
	void *handle_ = nullptr;
};

} // namespace singularity::platform
