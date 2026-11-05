cmake_minimum_required(VERSION 3.24)

# The pinned official ONNX Runtime 1.23.2 macOS distribution requires a
# deployment target of at least macOS 13.4. Normalize any missing or stale
# lower cached/environment value to 13.4 so CMake, Cargo/Rust, tests, and the
# final application all use one explicit minimum target.
if(CMAKE_HOST_APPLE)
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET
     OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "13.4")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.4" CACHE STRING
        "Minimum macOS version supported by the native application" FORCE)
  endif()

  message(STATUS
    "Singularity macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

get_filename_component(NATIVE_APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(NATIVE_APP_CODE_ROOT "${NATIVE_APP_ROOT}/code")
set(NATIVE_APP_SOURCE_DIR "${NATIVE_APP_CODE_ROOT}/backend")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/AppMetadata.cmake")
file(READ "${NATIVE_APP_ROOT}/docs/VERSION" NATIVE_APP_VERSION_VALUE)
string(STRIP "${NATIVE_APP_VERSION_VALUE}" NATIVE_APP_VERSION_VALUE)

project(
  NativeApp
  VERSION "${NATIVE_APP_VERSION_VALUE}"
  DESCRIPTION "${NATIVE_APP_DESCRIPTION}"
  LANGUAGES C CXX
)

# The name of the application's data folders, the same in every target that
# names one (Platform/AppPaths, Platform/AppSettings, Platform/WebViewData).
add_compile_definitions("NATIVE_APP_DATA_NAME=\"${NATIVE_APP_DATA_NAME}\"")

if(WIN32)
  # Rust's esaxx dependency intentionally uses the static MSVC runtime. Keep
  # every native object on the same runtime and make the packaged executable
  # independent of a separately installed Visual C++ redistributable.
  # Debug included: Rust links only the release CRT (cc builds esaxx's C++
  # with /MT), and objects built for the debug CRT cannot share an image with
  # it (LNK2038 on RuntimeLibrary and _ITERATOR_DEBUG_LEVEL). Debug keeps /Od
  # and full debug information; the presets' release-only static vcpkg
  # triplet gives both configurations the same libraries.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

if(MSVC)
  # For every target - the app, its tests and tools alike: sources are UTF-8
  # (without /utf-8 MSVC decodes them, Cyrillic literals included, with the
  # machine's ANSI code page), the largest translation units exceed COFF's
  # default section count, windows.h must not define min/max macros, and the
  # portable C calls used on every platform (getenv, sscanf) are not
  # deprecated in favour of Microsoft-only _s variants.
  add_compile_options(
    "$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>"
    "$<$<COMPILE_LANGUAGE:CXX>:/bigobj>")
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
  # windows.h without the parts nothing here uses - common dialogs, RPC, OLE,
  # winsock 1 - whose macros rename the product's own words: commdlg.h turns
  # the document action ReplaceText into ReplaceTextA, rpcndr.h turns every
  # `small` into char. A file that needs one of those headers includes it
  # itself. Defined empty, so the files that #define it too agree.
  add_compile_definitions(WIN32_LEAN_AND_MEAN=)
endif()

if(APPLE)
  enable_language(OBJC OBJCXX)

  # The Mac reads a picture's words with Vision, and the picture itself with
  # ImageIO (Document/PdfExtractor/PdfExtractorMacOS.mm, which the app and
  # its tests build): every target defined here links both, before any is.
  link_libraries("-framework Vision" "-framework ImageIO")

  # Ad-hoc signing ("-") gives every rebuild a different code identity, so
  # macOS can never remember a Keychain "Always Allow" grant across builds -
  # the app re-prompts for the Keychain password every single time (see
  # Prefer a stable local signing identity when one exists. Provider
  # credentials no longer use Keychain and do not depend on code identity.
  set(NATIVE_APP_CODE_SIGN_IDENTITY "-")
  execute_process(
    COMMAND security find-identity -v -p codesigning
    OUTPUT_VARIABLE NATIVE_APP_CODESIGN_IDENTITIES
    ERROR_QUIET
  )
  if(NATIVE_APP_CODESIGN_IDENTITIES MATCHES "\"Singularity Dev\"")
    set(NATIVE_APP_CODE_SIGN_IDENTITY "Singularity Dev")
    message(STATUS "Signing with the local 'Singularity Dev' identity (stable across rebuilds)")
  else()
    message(STATUS "No local 'Singularity Dev' code-signing identity found - using ad-hoc signing")
  endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build configuration lives under deploy/backend; production sources live
# under code/backend. Resolve every executable source against those two
# explicit roots so generated/build concerns never leak back into code/.
# A minimal Info.plist embedded into every non-bundle executable (see
# singularity_add_executable below). LSBackgroundOnly keeps a command-line or test
# binary out of the Dock and out of the menu bar even once AppKit initialises.
set(SINGULARITY_CONSOLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/ConsoleInfo.plist")
file(WRITE "${SINGULARITY_CONSOLE_INFO_PLIST}"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
\t<key>LSBackgroundOnly</key>
\t<true/>
\t<key>LSUIElement</key>
\t<true/>
</dict>
</plist>
")

function(singularity_add_executable target)
  set(resolved_sources)
  foreach(item IN LISTS ARGN)
    if(IS_ABSOLUTE "${item}" OR item MATCHES "^\\$<")
      list(APPEND resolved_sources "${item}")
    elseif(EXISTS "${NATIVE_APP_SOURCE_DIR}/${item}")
      list(APPEND resolved_sources "${NATIVE_APP_SOURCE_DIR}/${item}")
    elseif(EXISTS "${NATIVE_APP_CODE_ROOT}/${item}")
      list(APPEND resolved_sources "${NATIVE_APP_CODE_ROOT}/${item}")
    else()
      list(APPEND resolved_sources "${item}")
    endif()
  endforeach()
  add_executable(${target} ${resolved_sources})
  # Text is matched by Unicode code point everywhere (Platform/PhraseMatch,
  # Platform/TextFold), and those headers reach nearly every target through
  # the chat and quotation types: every executable links utf8proc.
  if(TARGET utf8proc)
    target_link_libraries(${target} PRIVATE utf8proc)
  endif()
  # Identifiers and tokens take their bytes from the system's generator
  # (Platform/SecureRandom), which reaches nearly every target: on Windows,
  # BCryptGenRandom.
  if(WIN32)
    target_link_libraries(${target} PRIVATE bcrypt)
    # The firm's computers link directly over UDP (Platform/UdpSocket, D-124),
    # which every build of the application's sources carries: Winsock, and the
    # adapters' addresses a computer tells the others.
    target_link_libraries(${target} PRIVATE ws2_32 iphlpapi)
    # This machine's graphics adapters and their memory budget, as local
    # inference plans for them (Node/DeviceCapabilities): DXGI.
    target_link_libraries(${target} PRIVATE dxgi)
  elseif(UNIX AND NOT APPLE AND CMAKE_DL_LIBS)
    # The GPU driver local inference finds at run time, never links
    # (Platform/DynamicLibrary): the dynamic loader.
    target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
  endif()
  # The product's name, the same way in every target.
  target_compile_definitions(${target} PRIVATE "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\"")
  # Console/test executables link AppKit (Papinian's CoreML path, NSURLSession,
  # the Security framework), and macOS gives any such process a Dock icon and
  # a menu-bar presence the moment AppKit initialises - so a plain `ctest` run
  # made a row of nameless app icons appear in the Dock. Marking these binaries
  # as background-only fixes that at the source. The real application is a
  # MACOSX_BUNDLE target and sets its own policy through its own Info.plist, so
  # it is deliberately left alone here: this only ever applies to non-bundle
  # executables, which are exactly the command-line and test binaries.
  if(APPLE)
    get_target_property(singularity_is_bundle ${target} MACOSX_BUNDLE)
    if(NOT singularity_is_bundle)
      # The plist declares the intent; ConsoleActivationPolicy.mm's own
      # pre-main constructor is what actually enforces it for a bare
      # executable (see that file for why the plist alone is not enough).
      target_sources(${target} PRIVATE
        "${NATIVE_APP_SOURCE_DIR}/Platform/ConsoleActivationPolicy/ConsoleActivationPolicy.mm")
      # ConsoleActivationPolicy.mm pins the accessory activation policy through
      # AppKit. Most console targets already pull AppKit in transitively;
      # linking it here explicitly means a target that does not still builds.
      target_link_libraries(${target} PRIVATE "-framework AppKit")
      target_link_options(${target} PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${SINGULARITY_CONSOLE_INFO_PLIST}")
    endif()
  endif()
  # Every Windows executable - the app, its tests and tools - runs with UTF-8
  # narrow strings, long paths and per-monitor DPI (windows/singularity.manifest).
  # MSVC merges a listed .manifest into the one its linker writes; the MinGW
  # release build embeds the same file through the app's resource script.
  if(MSVC)
    target_sources(${target} PRIVATE
      "${NATIVE_APP_ROOT}/deploy/backend/windows/singularity.manifest")
  endif()
endfunction()

# The file system watcher of a platform without FSEvents, for the test targets'
# non-macOS branches: Windows watches with ReadDirectoryChangesW, Linux with
# inotify.
if(WIN32)
  set(NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherWindows.cpp")
else()
  set(NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherLinux.cpp")
endif()

include(cmake/FetchDeps.cmake)
native_app_fetch_dependencies()

include(FetchContent)
# nlohmann/json's licence ships with the app (licenses/nlohmann-json).
FetchContent_GetProperties(nlohmann_json)
set(NATIVE_APP_ONNXRUNTIME_VERSION "1.23.2")
if(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(NATIVE_APP_ONNXRUNTIME_ARCHIVE
        "onnxruntime-osx-arm64-${NATIVE_APP_ONNXRUNTIME_VERSION}.tgz")
    set(NATIVE_APP_ONNXRUNTIME_ARCHIVE_SHA256
        "b4d513ab2b26f088c66891dbbc1408166708773d7cc4163de7bdca0e9bbb7856")
  else()
    set(NATIVE_APP_ONNXRUNTIME_ARCHIVE
        "onnxruntime-osx-x86_64-${NATIVE_APP_ONNXRUNTIME_VERSION}.tgz")
    set(NATIVE_APP_ONNXRUNTIME_ARCHIVE_SHA256
        "d10359e16347b57d9959f7e80a225a5b4a66ed7d7e007274a15cae86836485a6")
  endif()
  set(NATIVE_APP_ONNXRUNTIME_RELATIVE_LIBRARY
      "lib/libonnxruntime.${NATIVE_APP_ONNXRUNTIME_VERSION}.dylib")
elseif(WIN32)
  set(NATIVE_APP_ONNXRUNTIME_ARCHIVE
      "onnxruntime-win-x64-${NATIVE_APP_ONNXRUNTIME_VERSION}.zip")
  set(NATIVE_APP_ONNXRUNTIME_ARCHIVE_SHA256
      "0b38df9af21834e41e73d602d90db5cb06dbd1ca618948b8f1d66d607ac9f3cd")
  set(NATIVE_APP_ONNXRUNTIME_RELATIVE_LIBRARY "lib/onnxruntime.dll")
else()
  set(NATIVE_APP_ONNXRUNTIME_ARCHIVE
      "onnxruntime-linux-x64-${NATIVE_APP_ONNXRUNTIME_VERSION}.tgz")
  set(NATIVE_APP_ONNXRUNTIME_ARCHIVE_SHA256
      "1fa4dcaef22f6f7d5cd81b28c2800414350c10116f5fdd46a2160082551c5f9b")
  set(NATIVE_APP_ONNXRUNTIME_RELATIVE_LIBRARY
      "lib/libonnxruntime.so.${NATIVE_APP_ONNXRUNTIME_VERSION}")
endif()
FetchContent_Declare(
  native_app_onnxruntime
  URL "https://github.com/microsoft/onnxruntime/releases/download/v${NATIVE_APP_ONNXRUNTIME_VERSION}/${NATIVE_APP_ONNXRUNTIME_ARCHIVE}"
  URL_HASH "SHA256=${NATIVE_APP_ONNXRUNTIME_ARCHIVE_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(native_app_onnxruntime)
set(NATIVE_APP_ONNXRUNTIME_LIBRARY
    "${native_app_onnxruntime_SOURCE_DIR}/${NATIVE_APP_ONNXRUNTIME_RELATIVE_LIBRARY}")
if(NOT EXISTS "${NATIVE_APP_ONNXRUNTIME_LIBRARY}")
  message(FATAL_ERROR "Pinned ONNX Runtime library is missing after extraction")
endif()

# On macOS, a test or command-line program that runs Papinian NER loads ONNX
# Runtime from its own folder (LocalModel/Models/PapinianNER/NativeRuntime
# src/lib.rs). One target copies it there once, and each such program depends
# on it (singularity_onnxruntime_beside). When every program copied it after
# its own link, Xcode built them in parallel and their copies raced on the one
# file: a build failed with "Error copying file" and left it empty. The
# programs keep CMake's default output folder: the build folder, and under a
# multi-config generator such as Xcode its configuration's folder.
if(APPLE)
  get_property(singularity_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(singularity_multi_config)
    set(singularity_program_dir "${CMAKE_BINARY_DIR}/$<CONFIG>")
  else()
    set(singularity_program_dir "${CMAKE_BINARY_DIR}")
  endif()
  add_custom_target(onnxruntime_beside_tests
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${singularity_program_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_ONNXRUNTIME_LIBRARY}"
            "${singularity_program_dir}/libonnxruntime.${NATIVE_APP_ONNXRUNTIME_VERSION}.dylib"
    COMMENT "Placing ONNX Runtime beside the test and command-line programs"
    VERBATIM)
endif()

function(singularity_onnxruntime_beside target)
  if(TARGET onnxruntime_beside_tests)
    add_dependencies(${target} onnxruntime_beside_tests)
  endif()
endfunction()

# PDFium, Chrome's PDF engine (BSD-3-Clause; its bundled libraries' licenses
# ship in licenses/), as a pinned prebuilt release of bblanchon/pdfium-binaries.
# On Windows and Linux it reads PDFs; on every system it writes the annotated
# and redacted copies of a PDF (D-115), so one engine writes the same copy
# everywhere. macOS reads PDFs with PDFKit.
if(WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(NATIVE_APP_PDFIUM_RELEASE "chromium/8086")
  if(WIN32)
    set(NATIVE_APP_PDFIUM_ARCHIVE "pdfium-win-x64.tgz")
    set(NATIVE_APP_PDFIUM_ARCHIVE_SHA256
        "1fd8af952832dbb0eb16d9249f68fe09e5f5ebf7c3dd9f6066ea2720cc28487d")
    set(NATIVE_APP_PDFIUM_RELATIVE_LIBRARY "bin/pdfium.dll")
  elseif(APPLE)
    # The build's one architecture: the Mac's own unless CMAKE_OSX_ARCHITECTURES
    # names Intel alone.
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64"
        OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64"))
      set(NATIVE_APP_PDFIUM_ARCHIVE "pdfium-mac-x64.tgz")
      set(NATIVE_APP_PDFIUM_ARCHIVE_SHA256
          "933a85a138f6027243c56bff8676375c33ceeb767401389415ffc44d689ca85d")
    else()
      set(NATIVE_APP_PDFIUM_ARCHIVE "pdfium-mac-arm64.tgz")
      set(NATIVE_APP_PDFIUM_ARCHIVE_SHA256
          "e98679e052c07edbb5a627980902abb823d4b3f35744d877bd21668bd9fc13ab")
    endif()
    set(NATIVE_APP_PDFIUM_RELATIVE_LIBRARY "lib/libpdfium.dylib")
  else()
    set(NATIVE_APP_PDFIUM_ARCHIVE "pdfium-linux-x64.tgz")
    set(NATIVE_APP_PDFIUM_ARCHIVE_SHA256
        "588577cf52dabc1a444988bac841920df54cc2f141801424de97ab04f4fbb935")
    set(NATIVE_APP_PDFIUM_RELATIVE_LIBRARY "lib/libpdfium.so")
  endif()
  FetchContent_Declare(
    native_app_pdfium
    URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${NATIVE_APP_PDFIUM_RELEASE}/${NATIVE_APP_PDFIUM_ARCHIVE}"
    URL_HASH "SHA256=${NATIVE_APP_PDFIUM_ARCHIVE_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(native_app_pdfium)
  set(NATIVE_APP_PDFIUM_LIBRARY
      "${native_app_pdfium_SOURCE_DIR}/${NATIVE_APP_PDFIUM_RELATIVE_LIBRARY}")
  if(NOT EXISTS "${NATIVE_APP_PDFIUM_LIBRARY}")
    message(FATAL_ERROR "Pinned PDFium library is missing after extraction")
  endif()
  if(APPLE)
    # The release names its dylib ./libpdfium.dylib, which dyld would look for
    # in the working directory. A copy named @rpath/libpdfium.dylib, signed
    # again (an arm64 Mac runs no unsigned code), is what every program links
    # and what the app carries in Frameworks.
    set(NATIVE_APP_PDFIUM_DIR "${CMAKE_BINARY_DIR}/pdfium")
    file(MAKE_DIRECTORY "${NATIVE_APP_PDFIUM_DIR}")
    file(COPY_FILE "${NATIVE_APP_PDFIUM_LIBRARY}" "${NATIVE_APP_PDFIUM_DIR}/libpdfium.dylib")
    execute_process(
      COMMAND install_name_tool -id "@rpath/libpdfium.dylib" "${NATIVE_APP_PDFIUM_DIR}/libpdfium.dylib"
      RESULT_VARIABLE singularity_pdfium_renamed ERROR_QUIET)
    execute_process(
      COMMAND /usr/bin/codesign --force --sign - "${NATIVE_APP_PDFIUM_DIR}/libpdfium.dylib"
      RESULT_VARIABLE singularity_pdfium_signed OUTPUT_QUIET ERROR_QUIET)
    if(NOT singularity_pdfium_renamed EQUAL 0 OR NOT singularity_pdfium_signed EQUAL 0)
      message(FATAL_ERROR "The pinned PDFium could not be renamed and signed for macOS")
    endif()
    set(NATIVE_APP_PDFIUM_LIBRARY "${NATIVE_APP_PDFIUM_DIR}/libpdfium.dylib")
  endif()
  # MAJOR.MINOR.BUILD.PATCH of the pinned build, part of the extractor's
  # cache version: a new PDFium re-extracts every PDF.
  file(STRINGS "${native_app_pdfium_SOURCE_DIR}/VERSION" NATIVE_APP_PDFIUM_VERSION_LINES)
  set(NATIVE_APP_PDFIUM_VERSION "")
  foreach(line IN LISTS NATIVE_APP_PDFIUM_VERSION_LINES)
    string(REGEX REPLACE "^[A-Z]+=" "" component "${line}")
    if(NATIVE_APP_PDFIUM_VERSION)
      string(APPEND NATIVE_APP_PDFIUM_VERSION ".${component}")
    else()
      set(NATIVE_APP_PDFIUM_VERSION "${component}")
    endif()
  endforeach()
  add_library(native_app_pdfium SHARED IMPORTED GLOBAL)
  set_target_properties(native_app_pdfium PROPERTIES
    IMPORTED_LOCATION "${NATIVE_APP_PDFIUM_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${native_app_pdfium_SOURCE_DIR}/include"
    INTERFACE_COMPILE_DEFINITIONS "NATIVE_APP_PDFIUM_VERSION=\"${NATIVE_APP_PDFIUM_VERSION}\""
  )
  if(APPLE)
    set_target_properties(native_app_pdfium PROPERTIES IMPORTED_SONAME "@rpath/libpdfium.dylib")
  endif()
  if(WIN32)
    set_target_properties(native_app_pdfium PROPERTIES
      IMPORTED_IMPLIB "${native_app_pdfium_SOURCE_DIR}/lib/pdfium.dll.lib")
  endif()
endif()

# Local inference's runtime (llama-server and the libraries it loads), built
# here from the pinned, patched llama.cpp source: NATIVE_APP_LLAMA_CPP_RUNTIME_DIR
# and the local_inference_runtime target that fills it.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LocalInferenceRuntime.cmake")

# The sources that detect and calibrate this machine for local inference, by
# operating system and by the accelerator its runtime was built for - never by
# the operating system's name alone. The application and every test that plans
# for this machine compile the same set.
if(APPLE)
  set(NATIVE_APP_HARDWARE_SOURCES
    Node/DeviceCapabilities/DeviceCapabilitiesMacOS.cpp
    Platform/StorageProbe/StorageProbePosix.cpp)
  set(NATIVE_APP_CHILD_PROCESS_SOURCES Platform/ChildProcess/ChildProcessPosix.cpp)
elseif(WIN32)
  set(NATIVE_APP_HARDWARE_SOURCES
    Node/DeviceCapabilities/DeviceCapabilitiesWindows.cpp
    Platform/StorageProbe/StorageProbeWindows.cpp
    Platform/CudaDriver/CudaDriver.cpp
    Platform/DynamicLibrary/DynamicLibrary.cpp)
  set(NATIVE_APP_CHILD_PROCESS_SOURCES Platform/ChildProcess/ChildProcessWindows.cpp)
else()
  set(NATIVE_APP_HARDWARE_SOURCES
    Node/DeviceCapabilities/DeviceCapabilitiesLinux.cpp
    Platform/StorageProbe/StorageProbePosix.cpp
    Platform/CudaDriver/CudaDriver.cpp
    Platform/DynamicLibrary/DynamicLibrary.cpp)
  set(NATIVE_APP_CHILD_PROCESS_SOURCES Platform/ChildProcess/ChildProcessPosix.cpp)
endif()
# What the runtime this build carries can execute (LocalInference/
# RuntimeClosure) is a build fact, told to the one file that states it.
list(APPEND NATIVE_APP_HARDWARE_SOURCES LocalInference/RuntimeClosure/RuntimeClosure.cpp)
set_source_files_properties("${NATIVE_APP_SOURCE_DIR}/LocalInference/RuntimeClosure/RuntimeClosure.cpp"
  PROPERTIES COMPILE_DEFINITIONS
  "LOCAL_INFERENCE_ACCELERATOR=\"${NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR}\";LOCAL_INFERENCE_LLAMA_CPP_BUILD=\"${NATIVE_APP_LLAMA_CPP_BUILD}\"")
if(NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR STREQUAL "metal")
  list(APPEND NATIVE_APP_HARDWARE_SOURCES Platform/AcceleratorCalibration/AcceleratorCalibrationMacOS.mm)
elseif(NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR STREQUAL "cuda")
  list(APPEND NATIVE_APP_HARDWARE_SOURCES Platform/AcceleratorCalibration/AcceleratorCalibrationCuda.cpp)
else()
  list(APPEND NATIVE_APP_HARDWARE_SOURCES Platform/AcceleratorCalibration/AcceleratorCalibrationUnavailable.cpp)
endif()

# Papinian Guard's production GLiNER2 path is a zero-Python Rust static
# library. It pins the native eight-session engine and exposes a narrow C ABI;
# GLiNER2/Rust types never cross into the rest of the native application.
# A build tree an older container image configured may name a cargo this one
# lacks (Rust was in /root, now in /opt/cargo): it is looked up again.
if(NATIVE_APP_CARGO_EXECUTABLE AND NOT EXISTS "${NATIVE_APP_CARGO_EXECUTABLE}")
  unset(NATIVE_APP_CARGO_EXECUTABLE CACHE)
endif()
find_program(NATIVE_APP_CARGO_EXECUTABLE cargo REQUIRED)
set(NATIVE_APP_GLINER2_BRIDGE_DIR
    "${NATIVE_APP_SOURCE_DIR}/LocalModel/Models/PapinianNER/NativeRuntime")
set(NATIVE_APP_GLINER2_CARGO_TARGET_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/papinian-gliner2-cargo")
set(NATIVE_APP_RUST_TARGET "" CACHE STRING
    "Optional Rust target triple for a CMake cross-build")
file(GLOB_RECURSE NATIVE_APP_GLINER2_VENDOR_SOURCES CONFIGURE_DEPENDS
     "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/*.rs"
     "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/Cargo.toml")
set(NATIVE_APP_GLINER2_CARGO_ENV
    "CARGO_TARGET_DIR=${NATIVE_APP_GLINER2_CARGO_TARGET_DIR}")
set(NATIVE_APP_GLINER2_CARGO_BUILD_ARGS build --release --locked)
set(NATIVE_APP_GLINER2_CARGO_OUTPUT_DIR
    "${NATIVE_APP_GLINER2_CARGO_TARGET_DIR}/release")
if(NATIVE_APP_RUST_TARGET)
  list(APPEND NATIVE_APP_GLINER2_CARGO_BUILD_ARGS
       --target "${NATIVE_APP_RUST_TARGET}")
  set(NATIVE_APP_GLINER2_CARGO_OUTPUT_DIR
      "${NATIVE_APP_GLINER2_CARGO_TARGET_DIR}/${NATIVE_APP_RUST_TARGET}/release")
endif()
if(APPLE)
  list(APPEND NATIVE_APP_GLINER2_CARGO_ENV
       "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
       "RUSTC_BOOTSTRAP=1")
  # Homebrew Rust can ship a precompiled stdlib targeted at the host macOS.
  # Rebuild std from the installed rust-src so every object in the
  # static bridge archive uses the same explicit deployment target as CMake.
  list(APPEND NATIVE_APP_GLINER2_CARGO_BUILD_ARGS
       -Z build-std=std)
  # The build's own proc-macro dylibs (darling, tokio-macros, ...) are loaded
  # by rustc itself. Linked for macOS 13.4 with chained fixups, ld-27037 (the
  # Xcode/CLT 27 linker) writes a LINKEDIT string pool that the macOS 27
  # loader rejects ("mis-aligned LINKEDIT string pool"), and the build stops.
  # Classic fixups load everywhere. The static archive itself is never linked
  # by cargo, so its objects are unaffected.
  list(APPEND NATIVE_APP_GLINER2_CARGO_ENV
       "RUSTFLAGS=-C link-arg=-Wl,-no_fixup_chains")
endif()
if(WIN32)
  # Keep Rust's staticlib on the same static MSVC CRT as CMake and vcpkg.
  # Without this, the archive imports DLL CRT symbols such as __imp_strcat_s
  # that are unavailable when the final executable links /MT.
  list(APPEND NATIVE_APP_GLINER2_CARGO_ENV
       "RUSTFLAGS=-C target-feature=+crt-static")
endif()
if(MSVC)
  set(NATIVE_APP_GLINER2_BRIDGE_LIBRARY
      "${NATIVE_APP_GLINER2_CARGO_OUTPUT_DIR}/singularity_gliner2_bridge.lib")
else()
  set(NATIVE_APP_GLINER2_BRIDGE_LIBRARY
      "${NATIVE_APP_GLINER2_CARGO_OUTPUT_DIR}/libsingularity_gliner2_bridge.a")
endif()
add_custom_command(
  OUTPUT "${NATIVE_APP_GLINER2_BRIDGE_LIBRARY}"
  COMMAND "${CMAKE_COMMAND}" -E env --unset=MAKEFLAGS
          ${NATIVE_APP_GLINER2_CARGO_ENV}
          "${NATIVE_APP_CARGO_EXECUTABLE}" ${NATIVE_APP_GLINER2_CARGO_BUILD_ARGS}
          --manifest-path "${NATIVE_APP_GLINER2_BRIDGE_DIR}/Cargo.toml"
  DEPENDS
    "${NATIVE_APP_GLINER2_BRIDGE_DIR}/Cargo.toml"
    "${NATIVE_APP_GLINER2_BRIDGE_DIR}/Cargo.lock"
    "${NATIVE_APP_GLINER2_BRIDGE_DIR}/src/lib.rs"
    "${NATIVE_APP_GLINER2_BRIDGE_DIR}/gliner2_ffi.h"
    ${NATIVE_APP_GLINER2_VENDOR_SOURCES}
  COMMENT "Building the zero-Python Papinian GLiNER2 runtime bridge"
  VERBATIM
)
add_custom_target(native_app_gliner2_bridge
                  DEPENDS "${NATIVE_APP_GLINER2_BRIDGE_LIBRARY}")
add_library(native_app_gliner2_bridge_library STATIC IMPORTED GLOBAL)
set_target_properties(native_app_gliner2_bridge_library PROPERTIES
  IMPORTED_LOCATION "${NATIVE_APP_GLINER2_BRIDGE_LIBRARY}"
)
add_dependencies(native_app_gliner2_bridge_library native_app_gliner2_bridge)
if(APPLE)
  target_link_libraries(native_app_gliner2_bridge_library INTERFACE
    "-framework CoreFoundation"
    "-framework Security"
    "-framework SystemConfiguration"
  )
elseif(UNIX)
  # Rust static libraries do not encode their native link dependencies for
  # the final C++ linker. gliner2_inference downloads models through
  # native-tls, which uses the platform OpenSSL installation on Linux.
  find_package(OpenSSL REQUIRED)
  target_link_libraries(native_app_gliner2_bridge_library INTERFACE
    OpenSSL::SSL
    OpenSSL::Crypto
  )
elseif(WIN32)
  # Cargo's staticlib leaves Windows SDK dependencies to the final C++ link.
  target_link_libraries(native_app_gliner2_bridge_library INTERFACE
    bcrypt
    crypt32
    ncrypt
    ntdll
    secur32
    userenv
    ws2_32
  )
endif()

set(NATIVE_APP_FRONTEND_DIST "${NATIVE_APP_ROOT}/deploy/frontend/dist"
    CACHE PATH "Path to the Vite production build")
set(NATIVE_APP_ICON_SOURCE "${NATIVE_APP_ROOT}/deploy/assets/app-icon.png"
  CACHE FILEPATH "SVG or PNG used to generate native application icons")
# Move an existing build off the former default, while keeping explicit
# custom artwork overrides intact.
if(NATIVE_APP_ICON_SOURCE STREQUAL "${NATIVE_APP_ROOT}/deploy/assets/app-icon.svg")
  set(NATIVE_APP_ICON_SOURCE "${NATIVE_APP_ROOT}/deploy/assets/app-icon.png"
    CACHE FILEPATH "SVG or PNG used to generate native application icons" FORCE)
endif()
set(NATIVE_APP_INSTALLER_BRANDING
    "${NATIVE_APP_ROOT}/deploy/assets/installer-branding.json"
    CACHE FILEPATH "Installer text and color configuration")
set(NATIVE_APP_MACOS_ICON "${NATIVE_APP_ROOT}/deploy/backend/macos/icons/app.icns")
set(NATIVE_APP_WINDOWS_ICON "${NATIVE_APP_ROOT}/deploy/backend/windows/icons/app.ico")
set(NATIVE_APP_LINUX_ICON "${NATIVE_APP_ROOT}/deploy/backend/linux/icons/app.png")
set(NATIVE_APP_DMG_BACKGROUND
    "${NATIVE_APP_ROOT}/deploy/backend/macos/installer/dmg-background.tiff")
set(NATIVE_APP_NSIS_SIDEBAR
    "${NATIVE_APP_ROOT}/deploy/backend/windows/installer/nsis-sidebar.bmp")
set(NATIVE_APP_WIX_DIALOG
    "${NATIVE_APP_ROOT}/deploy/backend/windows/installer/wix-dialog.bmp")
set(NATIVE_APP_WIX_BANNER
    "${NATIVE_APP_ROOT}/deploy/backend/windows/installer/wix-banner.bmp")

set(NATIVE_APP_BRANDING_OUTPUTS
    "${NATIVE_APP_ROOT}/deploy/assets/app-icon-preview.png"
    "${NATIVE_APP_ROOT}/code/frontend/public/app-icon.png"
    "${NATIVE_APP_ROOT}/deploy/backend/macos/icons/app-icon-1024.png"
    "${NATIVE_APP_MACOS_ICON}"
    "${NATIVE_APP_WINDOWS_ICON}"
    "${NATIVE_APP_LINUX_ICON}"
    "${NATIVE_APP_DMG_BACKGROUND}"
    "${NATIVE_APP_NSIS_SIDEBAR}"
    "${NATIVE_APP_WIX_DIALOG}"
    "${NATIVE_APP_WIX_BANNER}"
)
set(NATIVE_APP_BRANDING_OUTPUTS_EXIST TRUE)
foreach(icon_path IN LISTS NATIVE_APP_BRANDING_OUTPUTS)
  if(NOT EXISTS "${icon_path}")
    set(NATIVE_APP_BRANDING_OUTPUTS_EXIST FALSE)
  endif()
endforeach()

# On Windows the Python interpreter on PATH comes first: the Microsoft Store's
# own Python registers a path inside Program Files\WindowsApps that no other
# program may start, so neither the build nor ctest could run it.
if(WIN32)
  set(Python3_FIND_REGISTRY LAST)
endif()

if(EXISTS "${NATIVE_APP_ICON_SOURCE}")
  find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
  set(NATIVE_APP_PILLOW_AVAILABLE FALSE)
  if(Python3_Interpreter_FOUND)
    execute_process(
      COMMAND "${Python3_EXECUTABLE}" -c "import PIL"
      RESULT_VARIABLE NATIVE_APP_PILLOW_RESULT
      OUTPUT_QUIET
      ERROR_QUIET
    )
    if(NATIVE_APP_PILLOW_RESULT EQUAL 0)
      set(NATIVE_APP_PILLOW_AVAILABLE TRUE)
    endif()
  endif()

  # The full set includes the DMG's multi-resolution TIFF (macOS tiffutil).
  # Other hosts use the committed outputs; generate_icons.py --skip-dmg can
  # regenerate the native icons and Windows artwork on those hosts.
  if(NATIVE_APP_PILLOW_AVAILABLE AND CMAKE_HOST_APPLE)
    add_custom_command(
      OUTPUT ${NATIVE_APP_BRANDING_OUTPUTS}
      COMMAND "${Python3_EXECUTABLE}"
              "${NATIVE_APP_ROOT}/deploy/scripts/generate_icons.py"
              --source "${NATIVE_APP_ICON_SOURCE}"
              --config "${NATIVE_APP_INSTALLER_BRANDING}"
      DEPENDS
        "${NATIVE_APP_ICON_SOURCE}"
        "${NATIVE_APP_INSTALLER_BRANDING}"
        "${NATIVE_APP_ROOT}/deploy/scripts/generate_icons.py"
        "${NATIVE_APP_ROOT}/deploy/scripts/requirements-icons.txt"
      COMMENT "Generating native application branding"
      VERBATIM
    )
    add_custom_target(native_app_branding DEPENDS ${NATIVE_APP_BRANDING_OUTPUTS})
  elseif(NATIVE_APP_BRANDING_OUTPUTS_EXIST)
    message(STATUS "Using pre-generated application branding")
  else()
    message(FATAL_ERROR
      "Application branding outputs are missing. Install deploy/scripts/requirements-icons.txt "
      "and run deploy/scripts/generate_icons.py before configuring the native build."
    )
  endif()
endif()

include(cmake/BackendSources.cmake)

# The protected computation protocol (DESIGN4, Papinian Cycle 4): compiled
# into every build that reads operations (every target that compiles
# ProtectedOperations.cpp; see singularity_add_protected_computation at the end of
# this file), with the pure sources they rest on. Several of those builds do
# not compile PapinianGuardService.cpp, so these sources reach the guard only
# through OpContext callbacks and header-inline wiring. The same files are
# listed in cmake/BackendSources.cmake for the application.
set(SINGULARITY_PROTECTED_COMPUTATION_SOURCES
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedComputation/ComputeTypes.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedComputation/ComputeProtocol.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/TypeSystem.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/Admission.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/Normaliser.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/ObservationLedger.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/ArithmeticOps.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/TemporalOps.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/TableOps.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedTables/ProtectedTables.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedTables/FormulaEval.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticRoles/SemanticRoles.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/AnswerContract/AnswerContract.cpp"
  # What they rest on: pure, standard-library-only sources.
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/ProtectedOperations.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedRelations/ProtectedRelations.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/RestorationGrammar/RestorationGrammar.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Context/BusinessCalendar/BusinessCalendar.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Context/ControlText/ControlText.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Context/TemporalSemantics/TemporalSemantics.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Platform/Sha256/Sha256.cpp"
)
# The relation policy and the taxonomy, kept apart: protected_relations_tests
# compiles them into its own test file.
set(SINGULARITY_PROTECTED_COMPUTATION_POLICY_SOURCES
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/Policy/Policy.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/Taxonomy/Taxonomy.cpp"
)
# The Document-facing table source (DESIGN4 §6.2): into every build that
# compiles MatterRequestExecution.cpp, all of which read workbooks, Word
# tables and CSV.
set(SINGULARITY_PROTECTED_TABLE_SOURCE_SOURCES
  "${NATIVE_APP_SOURCE_DIR}/Routing/ProtectedTableSource/ProtectedTableSource.cpp"
)

set(NATIVE_APP_HAS_ICON 0)
list(APPEND NATIVE_APP_SOURCES ${NATIVE_APP_HARDWARE_SOURCES} ${NATIVE_APP_CHILD_PROCESS_SOURCES})
if(APPLE)
  list(APPEND NATIVE_APP_SOURCES
    Document/PdfExtractor/PdfExtractorMacOS.mm
    Platform/BackgroundActivity/BackgroundActivityMacOS.mm
    Platform/FileDrop/FileDropMacOS.mm
    Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm
    Platform/HttpClient/HttpClientMacOS.mm
    Platform/MailDraft/MailDraftMacOS.mm
    Platform/MacOSChrome/MacOSChrome.hpp
    Platform/MacOSChrome/MacOSChrome.mm
    Platform/SeatPasswordStore/SeatPasswordStoreNone.cpp
    Platform/TrashFile/TrashFileMacOS.mm
    Platform/UpdateInstall/UpdateInstallMacOS.mm
    Platform/SystemPermissions/SystemPermissionsMacOS.mm
    Platform/UdpSocket/UdpSocketPosix.cpp
    Platform/WebSocket/WebSocketMacOS.mm
    Platform/WindowNavigation/WindowNavigationMacOS.cpp
  )
  # What macOS allows Singularity, asked in onboarding, holds its blocks with ARC.
  set_source_files_properties("${NATIVE_APP_SOURCE_DIR}/Platform/SystemPermissions/SystemPermissionsMacOS.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  # The relay's WebSocket (D-242) keeps its task and blocks with ARC.
  set_source_files_properties("${NATIVE_APP_SOURCE_DIR}/Platform/WebSocket/WebSocketMacOS.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  # The file-drop module holds blocks and objects across a drag: ARC keeps them.
  set_source_files_properties("${NATIVE_APP_SOURCE_DIR}/Platform/FileDrop/FileDropMacOS.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  # The mail draft keeps the sharing service while Mail takes the message: ARC keeps it.
  set_source_files_properties("${NATIVE_APP_SOURCE_DIR}/Platform/MailDraft/MailDraftMacOS.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set(NATIVE_APP_ICON_NAME "")
  if(EXISTS "${NATIVE_APP_MACOS_ICON}" OR EXISTS "${NATIVE_APP_ICON_SOURCE}")
    set(NATIVE_APP_HAS_ICON 1)
    set(NATIVE_APP_ICON_NAME "app.icns")
    if(NOT EXISTS "${NATIVE_APP_MACOS_ICON}")
      set_source_files_properties("${NATIVE_APP_MACOS_ICON}" PROPERTIES GENERATED TRUE)
    endif()
    set_source_files_properties(
      "${NATIVE_APP_MACOS_ICON}"
      PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )
    list(APPEND NATIVE_APP_SOURCES "${NATIVE_APP_MACOS_ICON}")
  endif()
  configure_file(
    "${NATIVE_APP_ROOT}/deploy/backend/macos/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    @ONLY
  )
  singularity_add_executable(native_app MACOSX_BUNDLE ${NATIVE_APP_SOURCES})
elseif(WIN32)
  list(APPEND NATIVE_APP_SOURCES
    Document/PdfExtractor/PdfExtractorPdfium.cpp
    Platform/BackgroundActivity/BackgroundActivityWindows.cpp
    Platform/FileDrop/FileDropWindows.cpp
    Platform/FileSystemWatcher/FileSystemWatcherWindows.cpp
    Platform/HttpClient/HttpClientWindows.cpp
    Platform/SeatPasswordStore/SeatPasswordStoreWindows.cpp
    Platform/TrashFile/TrashFileWindows.cpp
    Platform/UpdateInstall/UpdateInstallWindows.cpp
    Platform/UpdateHelper/UpdateHelper.cpp
    Platform/SystemPermissions/SystemPermissionsWindows.cpp
    Platform/UdpSocket/UdpSocketWindows.cpp
    Platform/WebSocket/WebSocketWindows.cpp
    Platform/WebViewData/WebViewDataWindows.cpp
    Platform/WebViewRecovery/WebViewRecoveryWindows.cpp
    Platform/WindowNavigation/WindowNavigationWindows.cpp
    Platform/WindowsText/WindowsText.hpp
  )
  if(EXISTS "${NATIVE_APP_WINDOWS_ICON}" OR EXISTS "${NATIVE_APP_ICON_SOURCE}")
    set(NATIVE_APP_HAS_ICON 1)
    if(MSVC)
      set(NATIVE_APP_WINDOWS_MANIFEST_RESOURCE "")
    else()
      set(NATIVE_APP_WINDOWS_MANIFEST_RESOURCE
          "CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST \"${NATIVE_APP_ROOT}/deploy/backend/windows/singularity.manifest\"")
    endif()
    configure_file(
      "${NATIVE_APP_ROOT}/deploy/backend/windows/app.rc.in"
      "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
      @ONLY
    )
    list(APPEND NATIVE_APP_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
  endif()
  singularity_add_executable(native_app WIN32 ${NATIVE_APP_SOURCES})
  # Every thread of the app gets the 8 MiB of stack a Linux thread has, not
  # MSVC's default 1 MiB: a colleague's work goes deep, a Debug build's frames
  # are large, and an MSVC catch block runs on top of every frame of the call
  # that threw (a locked credential overflowed document_review's 1 MiB,
  # 0xC00000FD, 2026-11-05). Reserved, not committed: a thread uses what it
  # touches.
  target_link_options(native_app PRIVATE /STACK:8388608)
else()
  list(APPEND NATIVE_APP_SOURCES
    Document/PdfExtractor/PdfExtractorPdfium.cpp
    Platform/BackgroundActivity/BackgroundActivityLinux.cpp
    Platform/FileDrop/FileDropLinux.cpp
    Platform/FileSystemWatcher/FileSystemWatcherLinux.cpp
    Platform/HttpClient/HttpClientLinux.cpp
    Platform/SeatPasswordStore/SeatPasswordStoreNone.cpp
    Platform/TrashFile/TrashFileLinux.cpp
    Platform/UpdateInstall/UpdateInstallLinux.cpp
    Platform/SystemPermissions/SystemPermissionsLinux.cpp
    Platform/UdpSocket/UdpSocketPosix.cpp
    Platform/WebSocket/WebSocketLinux.cpp
    Platform/WindowNavigation/WindowNavigationLinux.cpp
  )
  # libcurl is a soft dependency: widely preinstalled on Linux desktops but
  # not guaranteed, so model provider connections degrade to a clear error
  # (see http_client_linux.cpp) instead of failing the whole build when it
  # is absent.
  find_package(CURL QUIET)
  # The provider key store (Provider/CredentialStore/CredentialStoreLinux.cpp)
  # keeps keys in the desktop keyring through libsecret. It is required: a
  # key is never kept anywhere else.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
  if(EXISTS "${NATIVE_APP_LINUX_ICON}" OR EXISTS "${NATIVE_APP_ICON_SOURCE}")
    set(NATIVE_APP_HAS_ICON 1)
  endif()
  configure_file(
    "${NATIVE_APP_ROOT}/deploy/backend/linux/app.metainfo.xml.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${NATIVE_APP_IDENTIFIER}.metainfo.xml"
    @ONLY
  )
  singularity_add_executable(native_app ${NATIVE_APP_SOURCES})
endif()

# Forensic observability (the protection's original-to-symbol mapping, exact
# provider bodies) is compiled out of the shipping application in every
# configuration: Routing/Observability/OperationEvent.hpp. Checked by
# deploy/scripts/check_forensic_gate.py.
target_compile_definitions(native_app PRIVATE SINGULARITY_NO_FORENSIC_OBSERVABILITY=1)

if(TARGET native_app_branding)
  add_dependencies(native_app native_app_branding)
endif()
# Local inference's runtime is built before it is bundled beside the app.
add_dependencies(native_app local_inference_runtime)

target_include_directories(native_app PRIVATE "${NATIVE_APP_SOURCE_DIR}")
target_link_libraries(native_app PRIVATE
  webview::core
  nfd
  nlohmann_json::nlohmann_json
  libzip::zip
  pugixml::pugixml
  utf8proc
  native_app_gliner2_bridge_library
)
# Treat fetched headers as third-party code so warnings from a newer compiler do not
# obscure warnings produced by the application itself.
target_include_directories(native_app SYSTEM PRIVATE
  "$<TARGET_PROPERTY:webview::core,INTERFACE_INCLUDE_DIRECTORIES>"
  "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
  "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
)
if(WIN32)
  target_sources(native_app PRIVATE "${NATIVE_APP_SOURCE_DIR}/Provider/CredentialStore/CredentialStoreWindows.cpp")
  # psapi is Platform/ProcessMetrics/ProcessMetrics.cpp's GetProcessMemoryInfo -
  # real peak-RSS evidence for Phase 05's resource benchmark harness.
  # oleaut32 is Windows Firewall's COM (Platform/SystemPermissions, D-244):
  # its rules come as VARIANTs and BSTRs.
  target_link_libraries(native_app PRIVATE dwmapi uxtheme ole32 oleaut32 shell32 advapi32 crypt32 winhttp psapi)
endif()
if(APPLE)
  target_sources(native_app PRIVATE "${NATIVE_APP_SOURCE_DIR}/Provider/CredentialStore/CredentialStoreMacOS.mm")
  target_link_libraries(native_app PRIVATE
    "-framework Security"
    "-framework PDFKit"
    "-framework CoreGraphics"
    "-framework CoreServices"
    "-framework Foundation"
    "-framework Metal"
    "-framework Network"
  )
endif()
if(UNIX AND NOT APPLE)
  target_sources(native_app PRIVATE "${NATIVE_APP_SOURCE_DIR}/Provider/CredentialStore/CredentialStoreLinux.cpp")
  target_link_libraries(native_app PRIVATE PkgConfig::LIBSECRET)
endif()
if(UNIX AND NOT APPLE AND CURL_FOUND)
  target_link_libraries(native_app PRIVATE CURL::libcurl)
  target_compile_definitions(native_app PRIVATE SINGULARITY_HAVE_CURL)
endif()
target_compile_definitions(native_app PRIVATE
  "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  "NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
  "NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
  "$<$<AND:$<BOOL:${WIN32}>,$<COMPILE_LANGUAGE:CXX>>:NOMINMAX>"
)

# The Debug-only developer model source (LocalModel/DeveloperModelSource): a
# Debug build installs the pinned protection model from the git-ignored
# deploy/.dev-models folder without a Legal Machines account. Every other
# configuration compiles it out - developer_model_source_tests_release proves
# what such a build does.
get_filename_component(NATIVE_APP_DEVELOPER_MODEL_ROOT "${NATIVE_APP_ROOT}/deploy/.dev-models" ABSOLUTE)
set(NATIVE_APP_DEVELOPER_MODEL_DEFINITIONS
  "$<$<CONFIG:Debug>:SINGULARITY_DEVELOPER_MODEL_SOURCE=1>"
  "$<$<CONFIG:Debug>:SINGULARITY_DEVELOPER_MODEL_ROOT=\"${NATIVE_APP_DEVELOPER_MODEL_ROOT}\">"
)
target_compile_definitions(native_app PRIVATE ${NATIVE_APP_DEVELOPER_MODEL_DEFINITIONS})

# A whole-product test on a Debug build may stand in for Legal Machines'
# account at onboarding (NATIVE_APP_EMULATE_ACCOUNT, Bridge/NativeBridge).
# Every other configuration compiles it out.
target_compile_definitions(native_app PRIVATE "$<$<CONFIG:Debug>:SINGULARITY_ACCOUNT_EMULATION=1>")

if(MSVC)
  target_compile_options(native_app PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(native_app PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(APPLE)
  set_target_properties(native_app PROPERTIES
    OUTPUT_NAME "${NATIVE_APP_DISPLAY_NAME}"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "${NATIVE_APP_DISPLAY_NAME}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${NATIVE_APP_IDENTIFIER}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${NATIVE_APP_IDENTIFIER}"
    # Xcode's own automatic CodeSign build phase runs AFTER the POST_BUILD
    # codesign command below (it's the actual last word on the bundle's
    # final signature for the Xcode generator), and defaults to ad-hoc
    # ("Sign to Run Locally") unless told otherwise - which is exactly the
    # unstable-identity-every-rebuild problem this whole block exists to
    # avoid. CODE_SIGN_STYLE=Manual stops Xcode from trying automatic
    # signing management (which wants a Team/Apple ID we don't have here).
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${NATIVE_APP_CODE_SIGN_IDENTITY}"
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "YES"
    INSTALL_RPATH "@executable_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH FALSE
  )
    add_custom_command(TARGET native_app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/CommonWords/english_common_words.txt"
      "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/english_common_words.txt"
    COMMENT "Staging the public domain word list Papinian Guard reads"
    VERBATIM)
    # The role and query lexicons of the protected computation protocol
    # (DESIGN4 §5.2, §8.7): legal-general words, no names.
    add_custom_command(TARGET native_app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticRoles/role_lexicon_en.txt"
      "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticRoles/role_lexicon_ru.txt"
      "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticRoles/query_lexicon_en.txt"
      "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticRoles/query_lexicon_ru.txt"
      "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/"
    COMMENT "Staging the role and query lexicons Papinian Guard reads"
    VERBATIM)

add_custom_command(
    TARGET native_app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Frameworks"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_ONNXRUNTIME_LIBRARY}"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Frameworks/libonnxruntime.${NATIVE_APP_ONNXRUNTIME_VERSION}.dylib"
    # PDFium, which writes a PDF's annotated and redacted copies (D-115).
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_PDFIUM_LIBRARY}"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Frameworks/libpdfium.dylib"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/nlohmann-json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/nlohmann-json/LICENSE.MIT"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/onnxruntime"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${native_app_onnxruntime_SOURCE_DIR}/LICENSE"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/onnxruntime/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${native_app_onnxruntime_SOURCE_DIR}/ThirdPartyNotices.txt"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/onnxruntime/ThirdPartyNotices.txt"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/pdfium"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${native_app_pdfium_SOURCE_DIR}/LICENSE"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/pdfium/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${native_app_pdfium_SOURCE_DIR}/licenses"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/pdfium"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/gliner2-rs"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/LICENSE"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/gliner2-rs/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/NOTICE"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/gliner2-rs/NOTICE"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/monocypher"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SINGULARITY_MONOCYPHER_LICENCE}"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/monocypher/LICENCE.md"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/mbedtls"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SINGULARITY_MBEDTLS_LICENCE}"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/mbedtls/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/papinian-ner"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_ROOT}/deploy/backend/licenses/papinian-ner/NOTICE"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/papinian-ner/NOTICE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/LICENSE"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/papinian-ner/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/llama.cpp"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_ROOT}/deploy/backend/licenses/llama.cpp/LICENSE"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/llama.cpp/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_ROOT}/deploy/backend/licenses/llama.cpp/NOTICE"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/licenses/llama.cpp/NOTICE"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf --
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/llama.cpp"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/llama.cpp"
    COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Frameworks/libonnxruntime.${NATIVE_APP_ONNXRUNTIME_VERSION}.dylib"
    COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Frameworks/libpdfium.dylib"
    COMMAND "${CMAKE_COMMAND}" "-DDIRECTORY=$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/llama.cpp"
            "-DIDENTITY=${NATIVE_APP_CODE_SIGN_IDENTITY}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignDirectory.cmake"
    COMMENT "Bundling pinned ONNX Runtime, PDFium and the local inference runtime"
    VERBATIM
  )
elseif(WIN32)
  set_target_properties(native_app PROPERTIES
    OUTPUT_NAME "${NATIVE_APP_DISPLAY_NAME}"
  )
  # Windows loads a DLL from the executable's own directory first, so ONNX
  # Runtime sits beside it, with local inference's llama.cpp runtime in
  # llama.cpp/ (where Singularity looks for it beside its executable) and the
  # word list Papinian Guard reads.
  add_custom_command(
    TARGET native_app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_ONNXRUNTIME_LIBRARY}"
            "$<TARGET_FILE_DIR:native_app>/onnxruntime.dll"
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}"
            "-DDESTINATION=$<TARGET_FILE_DIR:native_app>/llama.cpp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MirrorDirectory.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/CommonWords/english_common_words.txt"
            "$<TARGET_FILE_DIR:native_app>/english_common_words.txt"
    COMMENT "Bundling pinned ONNX Runtime and the local inference runtime"
    VERBATIM
  )
else()
  set_target_properties(native_app PROPERTIES OUTPUT_NAME "${NATIVE_APP_SLUG}")
  # The same layout as Windows: ONNX Runtime beside the executable, local
  # inference's llama.cpp runtime in llama.cpp/ (where Singularity looks for
  # it beside its executable) and the word list Papinian Guard reads.
  add_custom_command(
    TARGET native_app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_ONNXRUNTIME_LIBRARY}"
            "$<TARGET_FILE_DIR:native_app>/libonnxruntime.so.${NATIVE_APP_ONNXRUNTIME_VERSION}"
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}"
            "-DDESTINATION=$<TARGET_FILE_DIR:native_app>/llama.cpp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MirrorDirectory.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/CommonWords/english_common_words.txt"
            "$<TARGET_FILE_DIR:native_app>/english_common_words.txt"
    COMMENT "Bundling pinned ONNX Runtime and the local inference runtime"
    VERBATIM
  )
endif()

# The sample matter (fictional US deal documents) a lawyer can try Singularity
# on without opening a client's files (Matter/MatterActions create_sample).
set(NATIVE_APP_SAMPLE_MATTER "${CMAKE_CURRENT_SOURCE_DIR}/sample-matter")
if(IS_DIRECTORY "${NATIVE_APP_SAMPLE_MATTER}")
  if(APPLE)
    add_custom_command(
      TARGET native_app POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E rm -rf --
              "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/sample-matter"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "${NATIVE_APP_SAMPLE_MATTER}"
              "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/sample-matter"
      COMMAND /usr/bin/codesign --force --deep --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
              "$<TARGET_BUNDLE_DIR:native_app>"
      COMMENT "Copying the sample matter into the app bundle"
      VERBATIM
    )
  else()
    add_custom_command(
      TARGET native_app POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E rm -rf --
              "$<TARGET_FILE_DIR:native_app>/sample-matter"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "${NATIVE_APP_SAMPLE_MATTER}"
              "$<TARGET_FILE_DIR:native_app>/sample-matter"
      COMMENT "Copying the sample matter next to the executable"
      VERBATIM
    )
  endif()
endif()

if(EXISTS "${NATIVE_APP_FRONTEND_DIST}/index.html")
  if(APPLE)
    add_custom_command(
      TARGET native_app POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E rm -rf --
              "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/frontend-dist"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/frontend-dist"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "${NATIVE_APP_FRONTEND_DIST}"
              "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/Resources/frontend-dist"
      COMMAND /usr/bin/codesign --force --deep --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
              "$<TARGET_BUNDLE_DIR:native_app>"
      COMMENT "Copying frontend into the app bundle"
      VERBATIM
    )
  else()
    add_custom_command(
      TARGET native_app POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E rm -rf --
              "$<TARGET_FILE_DIR:native_app>/frontend-dist"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "${NATIVE_APP_FRONTEND_DIST}"
              "$<TARGET_FILE_DIR:native_app>/frontend-dist"
      COMMENT "Copying frontend next to the executable"
      VERBATIM
    )
  endif()
else()
  message(WARNING "deploy/frontend/dist is missing; run npm run build in deploy/frontend/")
endif()

if(UNIX AND NOT APPLE AND NATIVE_APP_HAS_ICON)
  add_custom_command(
    TARGET native_app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_LINUX_ICON}"
            "$<TARGET_FILE_DIR:native_app>/app-icon.png"
    COMMENT "Copying the Linux application icon"
    VERBATIM
  )
endif()

# A source tree without the tests - the open repository's (the owner,
# 2026-11-01) - builds the application and its tools and defines no test,
# whatever the preset asks: every test target below needs BUILD_TESTING. The
# tests' own files are looked for, not a folder that may stand empty.
file(GLOB_RECURSE singularity_present_tests "${NATIVE_APP_SOURCE_DIR}/*.test.cpp")
if(NOT singularity_present_tests)
  if(BUILD_TESTING)
    message(STATUS "This source tree holds no tests: building without them.")
  endif()
  set(BUILD_TESTING OFF CACHE BOOL "Build the testing tree." FORCE)
endif()
include(CTest)

# The Document Intelligence & Preparation Layer: a library of its own, linked
# by the application, with its tests (cmake/DocumentIntelligence.cmake).
include(cmake/DocumentIntelligence.cmake)

if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  # The window's bridge script stays within MSVC's string literal limits on
  # every platform, so a Clang or GCC build catches what only MSVC refuses
  # (cmake/CheckBridgeScriptLiterals.cmake).
  add_test(NAME bridge_script_literals
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${NATIVE_APP_SOURCE_DIR}/Bridge/NativeBridge/NativeBridge.cpp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckBridgeScriptLiterals.cmake")
  singularity_add_executable(native_core_tests
    Bridge/NativeBridge/BackgroundExecutor.test.cpp
    Bridge/NativeBridge/BackgroundExecutor.cpp
  )
  target_include_directories(native_core_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(native_core_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(native_core_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME background_executor COMMAND native_core_tests)

  singularity_add_executable(file_drop_tests
    Platform/FileDrop/FileDrop.test.cpp
    Platform/FileDrop/FileDrop.cpp
  )
  target_include_directories(file_drop_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(file_drop_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(file_drop_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(file_drop_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME file_drop COMMAND file_drop_tests)

  if(APPLE)
    # The macOS quit reaches Singularity's own shutdown (Beta 1 #55).
    singularity_add_executable(macos_chrome_tests
      Platform/MacOSChrome/MacOSChrome.test.mm
      Platform/MacOSChrome/MacOSChrome.mm
      Platform/FileDrop/FileDropMacOS.mm
      Platform/FileDrop/FileDrop.cpp
    )
    target_include_directories(macos_chrome_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(macos_chrome_tests PRIVATE nlohmann_json::nlohmann_json
      "-framework AppKit" "-framework WebKit")
    add_test(NAME macos_chrome COMMAND macos_chrome_tests)
  endif()

  singularity_add_executable(protected_api_tests
    PapinianGuard/ProtectionTransformer/ProtectionTransformer.test.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/ProtectionTransformer/ProtectionTransformer.cpp
  )
  target_include_directories(protected_api_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(protected_api_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(protected_api_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_link_libraries(protected_api_tests PRIVATE utf8proc)
  add_test(NAME beta1_privacy_protected_api COMMAND protected_api_tests)

  singularity_add_executable(protected_preview_tests
    PapinianGuard/ProtectedPreview/ProtectedPreview.test.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
  )
  target_include_directories(protected_preview_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(protected_preview_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(protected_preview_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_privacy_protected_preview COMMAND protected_preview_tests)

  singularity_add_executable(preview_session_tests
    PapinianGuard/PreviewSession/PreviewSession.test.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
  )
  target_include_directories(preview_session_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(preview_session_tests PRIVATE Threads::Threads)
  if(MSVC)
    target_compile_options(preview_session_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(preview_session_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME preview_session COMMAND preview_session_tests)

  singularity_add_executable(papinian_guard_tests
    PapinianGuard/PapinianGuardService/PapinianGuardService.test.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/Benchmark/PapinianGuardBenchmark.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/ProtectionTransformer/ProtectionTransformer.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(papinian_guard_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
  )
  target_include_directories(papinian_guard_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_compile_definitions(papinian_guard_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  target_link_libraries(papinian_guard_tests PRIVATE Threads::Threads utf8proc)
  target_link_libraries(papinian_guard_tests PRIVATE native_app_gliner2_bridge_library)
  if(APPLE)
    singularity_onnxruntime_beside(papinian_guard_tests)
  endif()
  if(MSVC)
    target_compile_options(papinian_guard_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(papinian_guard_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_privacy_papinian_guard COMMAND papinian_guard_tests)

  singularity_add_executable(provider_protection_tests
    tests/provider_protection_test.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Provider/ProviderRegistry/ProviderRegistry.cpp
    Routing/ProtectedInference/ProtectedInference.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
    Privacy/SentPassages/SentPassages.cpp
  )
  target_compile_definitions(provider_protection_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
  )
  target_include_directories(provider_protection_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_compile_definitions(provider_protection_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  target_link_libraries(provider_protection_tests PRIVATE
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
	utf8proc
  )
  if(APPLE)
    singularity_onnxruntime_beside(provider_protection_tests)
  endif()
  if(MSVC)
    target_compile_options(provider_protection_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(provider_protection_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME provider_protection COMMAND provider_protection_tests)

  # Relations through the whole protected pipeline with a scripted model: E1, E2 and R2 shapes, one call each.
  singularity_add_executable(protected_relations_pipeline_tests
    tests/protected_relations_pipeline_test.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Provider/ProviderRegistry/ProviderRegistry.cpp
    Routing/ProtectedInference/ProtectedInference.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(protected_relations_pipeline_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
  )
  target_include_directories(protected_relations_pipeline_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(protected_relations_pipeline_tests PRIVATE
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
	utf8proc
  )
  if(APPLE)
    singularity_onnxruntime_beside(protected_relations_pipeline_tests)
  endif()
  if(MSVC)
    target_compile_options(protected_relations_pipeline_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(protected_relations_pipeline_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME protected_relations_pipeline COMMAND protected_relations_pipeline_tests)

  # Fact identity is not value equality: facts, anchors, provenance, relation sensitivity (DESIGN3 9.3-9.4, lane C1).
  singularity_add_executable(fact_identity_tests
    tests/fact_identity_test.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Provider/ProviderRegistry/ProviderRegistry.cpp
    Routing/ProtectedInference/ProtectedInference.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(fact_identity_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
  )
  target_include_directories(fact_identity_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(fact_identity_tests PRIVATE
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
	utf8proc
  )
  if(APPLE)
    singularity_onnxruntime_beside(fact_identity_tests)
  endif()
  if(MSVC)
    target_compile_options(fact_identity_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(fact_identity_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME fact_identity COMMAND fact_identity_tests)

  # Local operations through the protected pipeline with a scripted model (DESIGN3 9.6, lane C2).
  singularity_add_executable(protected_operations_pipeline_tests
    tests/protected_operations_pipeline_test.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Provider/ProviderRegistry/ProviderRegistry.cpp
    Routing/ProtectedInference/ProtectedInference.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(protected_operations_pipeline_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
  )
  target_include_directories(protected_operations_pipeline_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(protected_operations_pipeline_tests PRIVATE
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
	utf8proc
  )
  if(APPLE)
    singularity_onnxruntime_beside(protected_operations_pipeline_tests)
  endif()
  if(MSVC)
    target_compile_options(protected_operations_pipeline_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(protected_operations_pipeline_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME protected_operations_pipeline COMMAND protected_operations_pipeline_tests)

  # Local operations over protected facts: compare, difference, sum, rate, deadline (DESIGN3 9.6, lane C2).
  singularity_add_executable(protected_operations_tests
    PapinianGuard/ProtectedOperations/ProtectedOperations.test.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Provider/ProviderRegistry/ProviderRegistry.cpp
    Routing/ProtectedInference/ProtectedInference.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(protected_operations_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
  )
  target_include_directories(protected_operations_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(protected_operations_tests PRIVATE
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
	utf8proc
  )
  if(APPLE)
    singularity_onnxruntime_beside(protected_operations_tests)
  endif()
  if(MSVC)
    target_compile_options(protected_operations_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(protected_operations_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME protected_operations COMMAND protected_operations_tests)

  # The protected computation protocol (DESIGN4 §13.3, Papinian Cycle 4): each
  # lane's own test target, built over the real guard (as
  # protected_operations_tests is) and the protocol's sources.
  set(SINGULARITY_PROTECTED_GUARD_TEST_SOURCES
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Provider/ProviderRegistry/ProviderRegistry.cpp
    Routing/ProtectedInference/ProtectedInference.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  function(singularity_add_protected_computation_test target test_name)
    singularity_add_executable(${target} ${ARGN} ${SINGULARITY_PROTECTED_GUARD_TEST_SOURCES}
      ${SINGULARITY_PROTECTED_COMPUTATION_SOURCES})
    target_compile_definitions(${target} PRIVATE
      "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
      "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
    )
    target_include_directories(${target} PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE
      nlohmann_json::nlohmann_json
      native_app_gliner2_bridge_library
      utf8proc
    )
    # The provider registry's transport links; no test here makes a call.
    if(APPLE)
      target_sources(${target} PRIVATE "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientMacOS.mm")
      target_link_libraries(${target} PRIVATE "-framework Foundation" "-framework Security")
      singularity_onnxruntime_beside(${target})
    elseif(WIN32)
      target_sources(${target} PRIVATE "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientWindows.cpp")
      target_link_libraries(${target} PRIVATE winhttp)
    else()
      target_sources(${target} PRIVATE "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientLinux.cpp")
    endif()
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${test_name} COMMAND ${target})
  endfunction()

  # The seam's types, the wire protocol (lane K) and the executor's
  # paired-world oracle block (lane X).
  singularity_add_protected_computation_test(protected_computation_tests protected_computation
    PapinianGuard/ProtectedComputation/ComputeProtocol.test.cpp
    PapinianGuard/ProtectedOperations/ComputeOracle.test.cpp)
  # The local table model, the table source and recalc (lane T), with the
  # table family's own cases.
  singularity_add_protected_computation_test(protected_tables_tests protected_tables
    PapinianGuard/ProtectedTables/ProtectedTables.test.cpp
    PapinianGuard/ProtectedOperations/TableOps.test.cpp)
  # Semantic roles, the words rule and cross-source pairs (lane R).
  singularity_add_protected_computation_test(semantic_roles_tests semantic_roles
    PapinianGuard/SemanticRoles/SemanticRoles.test.cpp)
  # The final answer contract (lane V).
  singularity_add_protected_computation_test(answer_contract_tests answer_contract
    PapinianGuard/AnswerContract/AnswerContract.test.cpp)
  # The focused protected-computation corpus with scripted models, through the
  # real protected pipeline and research loop (DESIGN4 §14.2, lane A): the
  # scripted inference executor, and the Routing Log with its sent passages so
  # the disclosure record (§14.2 E) resolves in the same process.
  singularity_add_protected_computation_test(protected_computation_acceptance_tests protected_computation_acceptance
    tests/protected_computation_acceptance_test.cpp
    ExternalResearch/ExternalResearchAdapter/ExternalResearchAdapter.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Platform/AppSettings/AppSettings.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/ProtectionTransformer/ProtectionTransformer.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/RoutingLog/RoutingLog.cpp
    Privacy/SentPassages/SentPassages.cpp)

  # Deterministic inventory privacy over the A/B fixtures (lane P1).
  singularity_add_executable(papinian_inventory_tests
    tests/papinian_inventory_test.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Provider/ProviderRegistry/ProviderRegistry.cpp
    Routing/ProtectedInference/ProtectedInference.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(papinian_inventory_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
  )
  target_include_directories(papinian_inventory_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(papinian_inventory_tests PRIVATE
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
	utf8proc
  )
  if(APPLE)
    singularity_onnxruntime_beside(papinian_inventory_tests)
  endif()
  if(MSVC)
    target_compile_options(papinian_inventory_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(papinian_inventory_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME papinian_inventory COMMAND papinian_inventory_tests)

  # Built WITHOUT the forensic observability define: a forensic sink cannot run (lane P1).
  singularity_add_executable(observability_forensic_gate_tests
    tests/observability_forensic_gate_test.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Provider/ProviderRegistry/ProviderRegistry.cpp
    Routing/ProtectedInference/ProtectedInference.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(observability_forensic_gate_tests PRIVATE
    SINGULARITY_NO_FORENSIC_OBSERVABILITY=1
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
  )
  target_include_directories(observability_forensic_gate_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(observability_forensic_gate_tests PRIVATE
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
	utf8proc
  )
  if(APPLE)
    singularity_onnxruntime_beside(observability_forensic_gate_tests)
  endif()
  if(MSVC)
    target_compile_options(observability_forensic_gate_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(observability_forensic_gate_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME observability_forensic_gate COMMAND observability_forensic_gate_tests)

  # Deterministic provider-boundary privacy acceptance.
  #
  # This target performs no billable provider call, requires no local API
  # credential and never accesses an OS secret store. A synthetic transport
  # receives the exact serialized provider request after Papinian Guard's
  # final leak check, returns a controlled placeholder-preserving response,
  # and the test verifies exact local restoration.
  #
  # Real external-provider acceptance belongs to the authenticated Legal
  # Machines inference gateway and must not reintroduce desktop BYOK.
  if(APPLE)
    # Manual, billable Phase 15 production-path proof. It compiles the same
    # backend implementation as the app (minus the GUI entry point), so the
    # run uses InferenceExecutor, live Papinian/local/provider routing and
    # ProjectStore rather than a mock transport or direct Context insertion.
    set(CONTEXT_AUTOMATION_REAL_ACCEPTANCE_SOURCES ${NATIVE_APP_SOURCES})
    list(REMOVE_ITEM CONTEXT_AUTOMATION_REAL_ACCEPTANCE_SOURCES
      Platform/ApplicationMain/ApplicationMain.cpp
      "${NATIVE_APP_MACOS_ICON}"
    )
    singularity_add_executable(context_automation_real_acceptance EXCLUDE_FROM_ALL
      tests/context_automation_real_acceptance.cpp
      ${CONTEXT_AUTOMATION_REAL_ACCEPTANCE_SOURCES}
      Provider/CredentialStore/CredentialStoreMacOS.mm
    )
    target_include_directories(context_automation_real_acceptance PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(context_automation_real_acceptance PRIVATE
      webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
      native_app_gliner2_bridge_library
      "-framework Security" "-framework PDFKit" "-framework CoreGraphics"
      "-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network"
    )
    target_compile_definitions(context_automation_real_acceptance PRIVATE
      "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
      "NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
      "NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
    )
    target_compile_options(context_automation_real_acceptance PRIVATE -Wall -Wextra -Wpedantic)
    singularity_onnxruntime_beside(context_automation_real_acceptance)
    add_custom_command(TARGET context_automation_real_acceptance POST_BUILD
      COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
              "$<TARGET_FILE:context_automation_real_acceptance>"
      VERBATIM)

	# Manual, billable Phase 18 production-path proof. The two-stage runner
	# executes one real local and one real protected external MatterTask, then
	# verifies Usage reconciliation and statement exports in a fresh process.
	set(USAGE_LIVE_ACCEPTANCE_SOURCES ${NATIVE_APP_SOURCES})
	list(REMOVE_ITEM USAGE_LIVE_ACCEPTANCE_SOURCES
		Platform/ApplicationMain/ApplicationMain.cpp
		"${NATIVE_APP_MACOS_ICON}"
	)
	singularity_add_executable(usage_live_acceptance EXCLUDE_FROM_ALL
		tests/usage_live_acceptance.cpp
		${USAGE_LIVE_ACCEPTANCE_SOURCES}
		Provider/CredentialStore/CredentialStoreMacOS.mm
	)
	target_include_directories(usage_live_acceptance PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(usage_live_acceptance PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network"
	)
	target_compile_definitions(usage_live_acceptance PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
	)
	target_compile_options(usage_live_acceptance PRIVATE -Wall -Wextra -Wpedantic)
	singularity_onnxruntime_beside(usage_live_acceptance)
	add_dependencies(usage_live_acceptance local_inference_runtime)
	add_custom_command(TARGET usage_live_acceptance POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E rm -rf --
			"$<TARGET_FILE_DIR:usage_live_acceptance>/llama.cpp"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}"
			"$<TARGET_FILE_DIR:usage_live_acceptance>/llama.cpp"
		COMMAND "${CMAKE_COMMAND}" "-DDIRECTORY=$<TARGET_FILE_DIR:usage_live_acceptance>/llama.cpp"
			"-DIDENTITY=${NATIVE_APP_CODE_SIGN_IDENTITY}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignDirectory.cmake"
		COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
			"$<TARGET_FILE:usage_live_acceptance>"
		VERBATIM)

	# Manual Phase 23 production-path proof. It compiles the same backend the
	# app does (minus the GUI entry point) and drives every Phase 23 step
	# through the real NativeBridge worker dispatch, against this computer's
	# genuinely installed Papinian package and genuinely connected provider.
	# It makes no provider network call and runs no generative inference -
	# routing::preview() captures the outbound request instead of sending it.
	set(PHASE23_CHAT_PREVIEW_REAL_ACCEPTANCE_SOURCES ${NATIVE_APP_SOURCES})
	list(REMOVE_ITEM PHASE23_CHAT_PREVIEW_REAL_ACCEPTANCE_SOURCES
		Platform/ApplicationMain/ApplicationMain.cpp
		"${NATIVE_APP_MACOS_ICON}"
	)
	singularity_add_executable(chat_preview_live_acceptance EXCLUDE_FROM_ALL
		tests/chat_preview_live_acceptance.cpp
		${PHASE23_CHAT_PREVIEW_REAL_ACCEPTANCE_SOURCES}
		Provider/CredentialStore/CredentialStoreMacOS.mm
	)
	target_include_directories(chat_preview_live_acceptance PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(chat_preview_live_acceptance PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network"
	)
	target_compile_definitions(chat_preview_live_acceptance PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
	)
	target_compile_options(chat_preview_live_acceptance PRIVATE -Wall -Wextra -Wpedantic)
	singularity_onnxruntime_beside(chat_preview_live_acceptance)
	add_custom_command(TARGET chat_preview_live_acceptance POST_BUILD
		COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
			"$<TARGET_FILE:chat_preview_live_acceptance>"
		VERBATIM)

	# Manual Phase 24 real acceptance. Same reasons as Phase 23's own runner
	# above (real Papinian package, real connected provider), but this one
	# DOES make a real, minimal external provider network call through
	# chat.send - never fabricated evidence. See tests/
	# chat_execution_live_acceptance.cpp's own top comment.
	set(PHASE24_CHAT_EXECUTION_REAL_ACCEPTANCE_SOURCES ${NATIVE_APP_SOURCES})
	list(REMOVE_ITEM PHASE24_CHAT_EXECUTION_REAL_ACCEPTANCE_SOURCES
		Platform/ApplicationMain/ApplicationMain.cpp
		"${NATIVE_APP_MACOS_ICON}"
	)
	singularity_add_executable(chat_execution_live_acceptance EXCLUDE_FROM_ALL
		tests/chat_execution_live_acceptance.cpp
		${PHASE24_CHAT_EXECUTION_REAL_ACCEPTANCE_SOURCES}
		Provider/CredentialStore/CredentialStoreMacOS.mm
	)
	target_include_directories(chat_execution_live_acceptance PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(chat_execution_live_acceptance PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network"
	)
	target_compile_definitions(chat_execution_live_acceptance PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
	)
	target_compile_options(chat_execution_live_acceptance PRIVATE -Wall -Wextra -Wpedantic)
	singularity_onnxruntime_beside(chat_execution_live_acceptance)
	add_custom_command(TARGET chat_execution_live_acceptance POST_BUILD
		COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
			"$<TARGET_FILE:chat_execution_live_acceptance>"
		VERBATIM)

	set(PHASE24_1B_EXTERNAL_RESEARCH_REAL_ACCEPTANCE_SOURCES ${NATIVE_APP_SOURCES})
	list(REMOVE_ITEM PHASE24_1B_EXTERNAL_RESEARCH_REAL_ACCEPTANCE_SOURCES
		Platform/ApplicationMain/ApplicationMain.cpp
		"${NATIVE_APP_MACOS_ICON}"
	)
	singularity_add_executable(external_research_live_acceptance EXCLUDE_FROM_ALL
		tests/external_research_live_acceptance.cpp
		${PHASE24_1B_EXTERNAL_RESEARCH_REAL_ACCEPTANCE_SOURCES}
		Provider/CredentialStore/CredentialStoreMacOS.mm
	)
	target_include_directories(external_research_live_acceptance PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(external_research_live_acceptance PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network"
	)
	target_compile_definitions(external_research_live_acceptance PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
	)
	target_compile_options(external_research_live_acceptance PRIVATE -Wall -Wextra -Wpedantic)
	singularity_onnxruntime_beside(external_research_live_acceptance)
	add_custom_command(TARGET external_research_live_acceptance POST_BUILD
		COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
			"$<TARGET_FILE:external_research_live_acceptance>"
		VERBATIM)

	set(PHASE24_1D_CONNECTED_GOLDEN_REAL_ACCEPTANCE_SOURCES ${NATIVE_APP_SOURCES})
	list(REMOVE_ITEM PHASE24_1D_CONNECTED_GOLDEN_REAL_ACCEPTANCE_SOURCES
		Platform/ApplicationMain/ApplicationMain.cpp
		"${NATIVE_APP_MACOS_ICON}"
	)
	singularity_add_executable(connected_golden_live_acceptance EXCLUDE_FROM_ALL
		tests/connected_golden_live_acceptance.cpp
		${PHASE24_1D_CONNECTED_GOLDEN_REAL_ACCEPTANCE_SOURCES}
		Provider/CredentialStore/CredentialStoreMacOS.mm
	)
	target_include_directories(connected_golden_live_acceptance PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(connected_golden_live_acceptance PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network"
	)
	target_compile_definitions(connected_golden_live_acceptance PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
		"NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
	)
	target_compile_options(connected_golden_live_acceptance PRIVATE -Wall -Wextra -Wpedantic)
	singularity_onnxruntime_beside(connected_golden_live_acceptance)
	add_dependencies(connected_golden_live_acceptance local_inference_runtime)
	add_custom_command(TARGET connected_golden_live_acceptance POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E rm -rf --
			"$<TARGET_FILE_DIR:connected_golden_live_acceptance>/llama.cpp"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}"
			"$<TARGET_FILE_DIR:connected_golden_live_acceptance>/llama.cpp"
		COMMAND "${CMAKE_COMMAND}" "-DDIRECTORY=$<TARGET_FILE_DIR:connected_golden_live_acceptance>/llama.cpp"
			"-DIDENTITY=${NATIVE_APP_CODE_SIGN_IDENTITY}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignDirectory.cmake"
		COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
			"$<TARGET_FILE:connected_golden_live_acceptance>"
		VERBATIM)

	# Temporary, offline-only replay of the exact Phase 24.1D production
	# orchestration. Only the socket-capable HttpClient translation unit is
	# replaced with captured deterministic responses; NativeBridge,
	# ProjectChat, MatterRequestExecution, Papinian and ResearchToolLoop remain
	# the ordinary production implementations above that boundary.
	set(PHASE24_1D_OFFLINE_MEMORY_REPLAY_SOURCES ${NATIVE_APP_SOURCES})
	list(REMOVE_ITEM PHASE24_1D_OFFLINE_MEMORY_REPLAY_SOURCES
		Platform/ApplicationMain/ApplicationMain.cpp
		Platform/HttpClient/HttpClientMacOS.mm
		"${NATIVE_APP_MACOS_ICON}"
	)
	singularity_add_executable(long_horizon_replay_runner
		tests/connected_golden_live_acceptance.cpp
		tests/captured_http_replay.cpp
		${PHASE24_1D_OFFLINE_MEMORY_REPLAY_SOURCES}
		tests/live_test_credentials.cpp
	)
	target_include_directories(long_horizon_replay_runner PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(long_horizon_replay_runner PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network"
	)
	target_compile_definitions(long_horizon_replay_runner PRIVATE
		SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC=1
		SINGULARITY_ISOLATED_TEST_SETTINGS=1
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
		"NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
	)
	target_compile_options(long_horizon_replay_runner PRIVATE -Wall -Wextra -Wpedantic)
	singularity_onnxruntime_beside(long_horizon_replay_runner)
	add_custom_command(TARGET long_horizon_replay_runner POST_BUILD
		COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
			"$<TARGET_FILE:long_horizon_replay_runner>"
		VERBATIM)

	add_test(NAME beta1_operation_long_horizon_replay COMMAND long_horizon_replay_runner offline-replay)
	set_tests_properties(beta1_operation_long_horizon_replay PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

	# Same CLI source and application stack; deterministic transport and isolated
	# settings are link/build substitutions, never a product CLI execution mode.
	singularity_add_executable(singularity_cli_offline
		Cli/SingularityCli/SingularityCli.cpp
		Context/ComputedStatements/ComputedStatements.cpp
		Context/TemporalContext/TemporalContext.cpp
		Context/TemporalSemantics/TemporalSemantics.cpp
		tests/singularity_cli_captured_http.cpp
		tests/live_test_credentials.cpp
		PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
		Context/BusinessCalendar/BusinessCalendar.cpp
		PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
		PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
		Context/ControlText/ControlText.cpp
		PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
		${PHASE24_1D_OFFLINE_MEMORY_REPLAY_SOURCES})
	target_include_directories(singularity_cli_offline PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(singularity_cli_offline PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network")
	target_compile_definitions(singularity_cli_offline PRIVATE
		SINGULARITY_ISOLATED_TEST_SETTINGS=1
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
		"NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\"")
	singularity_onnxruntime_beside(singularity_cli_offline)
	add_custom_command(TARGET singularity_cli_offline POST_BUILD
		COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
			"$<TARGET_FILE:singularity_cli_offline>" VERBATIM)

	# The same CLI with its provider replaced by a local program
	# (SINGULARITY_FAKE_MODEL): no network, no credential - a technical check of what
	# Singularity sends a model and takes back from it, on real Projects.
	singularity_add_executable(singularity_cli_fake
		Cli/SingularityCli/SingularityCli.cpp
		Context/ComputedStatements/ComputedStatements.cpp
		Context/TemporalContext/TemporalContext.cpp
		Context/TemporalSemantics/TemporalSemantics.cpp
		tests/singularity_cli_fake_model_http.cpp
		PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
		Context/BusinessCalendar/BusinessCalendar.cpp
		PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
		PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
		Context/ControlText/ControlText.cpp
		PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
		${PHASE24_1D_OFFLINE_MEMORY_REPLAY_SOURCES})
	set_target_properties(singularity_cli_fake PROPERTIES EXCLUDE_FROM_ALL TRUE)
	target_include_directories(singularity_cli_fake PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(singularity_cli_fake PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network")
	target_compile_definitions(singularity_cli_fake PRIVATE
		SINGULARITY_ISOLATED_TEST_SETTINGS=1
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
		"NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\"")
	singularity_onnxruntime_beside(singularity_cli_fake)
	add_custom_command(TARGET singularity_cli_fake POST_BUILD
		COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
			"$<TARGET_FILE:singularity_cli_fake>" VERBATIM)

	# Singularity CLI: a real, generic terminal client entering through the same
	# NativeBridge chat.send production boundary as the app UI and the
	# acceptance harnesses above - never a second orchestration
	# implementation. EXCLUDE_FROM_ALL: built on request (it needs real
	# provider credentials/network to be useful), never as part of an
	# ordinary package build.
	set(SINGULARITY_CLI_SOURCES ${NATIVE_APP_SOURCES})
	list(REMOVE_ITEM SINGULARITY_CLI_SOURCES
		Platform/ApplicationMain/ApplicationMain.cpp
		"${NATIVE_APP_MACOS_ICON}"
	)
	singularity_add_executable(singularity_cli EXCLUDE_FROM_ALL
		Cli/SingularityCli/SingularityCli.cpp
		Context/ComputedStatements/ComputedStatements.cpp
		Context/TemporalContext/TemporalContext.cpp
		Context/TemporalSemantics/TemporalSemantics.cpp
		${SINGULARITY_CLI_SOURCES}
		Provider/CredentialStore/CredentialStoreMacOS.mm
		PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
		Context/BusinessCalendar/BusinessCalendar.cpp
		PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
		PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
		Context/ControlText/ControlText.cpp
		PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
	)
	target_include_directories(singularity_cli PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(singularity_cli PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network"
	)
	target_compile_definitions(singularity_cli PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
		"NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
		${NATIVE_APP_DEVELOPER_MODEL_DEFINITIONS}
	)
	target_compile_options(singularity_cli PRIVATE -Wall -Wextra -Wpedantic)
	singularity_onnxruntime_beside(singularity_cli)
	add_custom_command(TARGET singularity_cli POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E rm -rf --
			"$<TARGET_FILE_DIR:singularity_cli>/llama.cpp"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}"
			"$<TARGET_FILE_DIR:singularity_cli>/llama.cpp"
		COMMAND "${CMAKE_COMMAND}" "-DDIRECTORY=$<TARGET_FILE_DIR:singularity_cli>/llama.cpp"
			"-DIDENTITY=${NATIVE_APP_CODE_SIGN_IDENTITY}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignDirectory.cmake"
		COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
			"$<TARGET_FILE:singularity_cli>"
		VERBATIM)

	# Non-shipping backend lifecycle acceptance driver (never installed, never
	# part of the packaged Release app, never reachable through the production
	# Native Bridge) - see code/tests/singularity_backend_acceptance.cpp's own
	# header comment for exactly what it does and does not do. EXCLUDE_FROM_ALL
	# for the same reason as singularity_cli above: needs real provider credentials/
	# network to be useful for a send, never part of an ordinary package build.
	singularity_add_executable(singularity_backend_acceptance EXCLUDE_FROM_ALL
		tests/singularity_backend_acceptance.cpp
		${SINGULARITY_CLI_SOURCES}
		Provider/CredentialStore/CredentialStoreMacOS.mm
	)
	target_include_directories(singularity_backend_acceptance PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(singularity_backend_acceptance PRIVATE
		webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
		native_app_gliner2_bridge_library
		"-framework Security" "-framework PDFKit" "-framework CoreGraphics"
		"-framework CoreServices" "-framework Foundation" "-framework Metal" "-framework Network"
	)
	target_compile_definitions(singularity_backend_acceptance PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
		"NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
		"NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
	)
	target_compile_options(singularity_backend_acceptance PRIVATE -Wall -Wextra -Wpedantic)

	# Singularity CLI regression suite: drives the real CLI binary as a subprocess
	# (piped stdin, captured output, real exit code) - never a second parse or
	# reimplementation of its dispatch logic. Offline only: no provider secret,
	# no network, no real Project ever created.
	#
	# It drives singularity_cli_offline, NOT singularity_cli: the offline build is an
	# ordinary target (no EXCLUDE_FROM_ALL) and is an explicit dependency below,
	# so `cmake --build` always leaves this test a binary as new as its own
	# sources. singularity_cli, the one that reaches real providers, IS
	# EXCLUDE_FROM_ALL - so anyone running a REAL CLI journey by hand must build
	# that target first, or they will drive a stale binary and attribute its
	# transcript to code that is not in it. That mistake cost one invalid
	# acceptance run; see docs/TEST_TIERS.md.
	singularity_add_executable(singularity_cli_tests
		tests/singularity_cli_test.cpp
		Platform/AppSettings/AppSettings.cpp
		PapinianGuard/Policy/Policy.cpp
		Provider/ModelClassification/ModelClassification.cpp
		Node/LegacyWorkspace/LegacyWorkspace.cpp
	)
	add_dependencies(singularity_cli_tests singularity_cli_offline)
	target_include_directories(singularity_cli_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(singularity_cli_tests PRIVATE nlohmann_json::nlohmann_json "-framework Foundation")
	target_compile_definitions(singularity_cli_tests PRIVATE
		"SINGULARITY_CLI_BINARY_PATH=\"$<TARGET_FILE:singularity_cli_offline>\""
		SINGULARITY_ISOLATED_TEST_SETTINGS=1
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
	target_compile_options(singularity_cli_tests PRIVATE -Wall -Wextra -Wpedantic)
	add_test(NAME beta1_cli_journey COMMAND singularity_cli_tests)
	set_tests_properties(beta1_cli_journey PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)

    singularity_add_executable(papinian_provider_boundary_acceptance
      tests/papinian_provider_boundary_acceptance.cpp
      Platform/AppPaths/AppPaths.cpp
      Platform/HttpClient/HttpClientMacOS.mm
      PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
      PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
      Context/ComputedStatements/ComputedStatements.cpp
      Context/TemporalContext/TemporalContext.cpp
      Context/TemporalSemantics/TemporalSemantics.cpp
      PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
      PapinianGuard/Taxonomy/Taxonomy.cpp
      PapinianGuard/Policy/Policy.cpp
      Provider/ModelClassification/ModelClassification.cpp
      Provider/ProviderRegistry/ProviderRegistry.cpp
      Routing/ProtectedInference/ProtectedInference.cpp
      Routing/LiveAnswer/LiveAnswer.cpp
      Platform/Sha256/Sha256.cpp
      PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
      Context/BusinessCalendar/BusinessCalendar.cpp
      PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
      PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
      Context/ControlText/ControlText.cpp
      PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
    )
    target_include_directories(papinian_provider_boundary_acceptance PRIVATE
      "${NATIVE_APP_SOURCE_DIR}"
    )
    target_link_libraries(papinian_provider_boundary_acceptance PRIVATE
      nlohmann_json::nlohmann_json
	  utf8proc
      "-framework Foundation"
    )
    target_compile_definitions(papinian_provider_boundary_acceptance PRIVATE
      "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
      "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    )
    target_compile_options(papinian_provider_boundary_acceptance PRIVATE
      -Wall -Wextra -Wpedantic)
    add_custom_command(TARGET papinian_provider_boundary_acceptance POST_BUILD
      COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
              "$<TARGET_FILE:papinian_provider_boundary_acceptance>"
      VERBATIM)
  elseif(WIN32)
    # The same manual Phase 24 acceptance on Windows: the backend the app
    # compiles (minus its GUI entry point), Windows' credential store, and the
    # DLLs native_app stages in this same output folder (ONNX Runtime,
    # and its llama.cpp runtime, PDFium, the word list). Like its macOS
    # counterpart it is in no build and no ctest; it runs on purpose, once,
    # through deploy/scripts/live_provider_acceptance.py.
    set(SINGULARITY_WINDOWS_LIVE_SOURCES ${NATIVE_APP_SOURCES})
    list(REMOVE_ITEM SINGULARITY_WINDOWS_LIVE_SOURCES Platform/ApplicationMain/ApplicationMain.cpp)
    singularity_add_executable(chat_execution_live_acceptance EXCLUDE_FROM_ALL
      tests/chat_execution_live_acceptance.cpp
      ${SINGULARITY_WINDOWS_LIVE_SOURCES}
      Provider/CredentialStore/CredentialStoreWindows.cpp
    )
    target_include_directories(chat_execution_live_acceptance PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(chat_execution_live_acceptance PRIVATE
      webview::core nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
      native_app_gliner2_bridge_library
      dwmapi uxtheme ole32 shell32 advapi32 crypt32 winhttp psapi
    )
    target_compile_definitions(chat_execution_live_acceptance PRIVATE
      "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
      "NATIVE_APP_VERSION=\"${PROJECT_VERSION}\""
      "NATIVE_APP_HAS_ICON=${NATIVE_APP_HAS_ICON}"
    )
    target_compile_options(chat_execution_live_acceptance PRIVATE /W4 /permissive- /utf-8)
    # Its DLLs are the ones native_app stages beside itself.
    add_dependencies(chat_execution_live_acceptance native_app)
  endif()

  # Legacy Phase 06 external-provider acceptance.
  #
  # This executable is intentionally excluded from ordinary package builds.
  # It reads an already connected BYOK credential through the same native
  # Data Protection store as the app and never prints it. Uses the same
  # lightweight stand-in semantic adapter (no
  # ONNX/Gliner2 runtime needed) since this binary's synthetic content has
  # no real PII and Papinian NER already has its own live acceptance
  # evidence (Phase 03).
  if(APPLE)
    singularity_add_executable(sources_outputs_live_acceptance EXCLUDE_FROM_ALL
      tests/sources_outputs_live_acceptance.cpp
      WorkProduct/FindingParser/FindingParser.cpp
      Routing/LegalTaskPresets/LegalTaskPresets.cpp
      WorkProduct/ObligationParser/ObligationParser.cpp
      Context/PromptAssembler/PromptAssembler.cpp
      Context/TemporalContext/TemporalContext.cpp
      Context/TemporalSemantics/TemporalSemantics.cpp
      Context/SourceMarkers/SourceMarkers.cpp
      Context/QuoteVerification/QuoteVerification.cpp
      Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
      Platform/AppSettings/AppSettings.cpp
      Provider/CredentialStore/CredentialStoreMacOS.mm
      Platform/HttpClient/HttpClientMacOS.mm
      PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
      PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
      Context/ComputedStatements/ComputedStatements.cpp
      PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
      Platform/Sha256/Sha256.cpp
      PapinianGuard/Taxonomy/Taxonomy.cpp
      PapinianGuard/Policy/Policy.cpp
      Provider/ModelClassification/ModelClassification.cpp
      Provider/ProviderRegistry/ProviderRegistry.cpp
      Routing/ProtectedInference/ProtectedInference.cpp
      Routing/LiveAnswer/LiveAnswer.cpp
      Node/LegacyWorkspace/LegacyWorkspace.cpp
      PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
      Context/BusinessCalendar/BusinessCalendar.cpp
      PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
      PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
      Context/ControlText/ControlText.cpp
      PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
    )
    target_include_directories(sources_outputs_live_acceptance PRIVATE
      "${NATIVE_APP_SOURCE_DIR}"
    )
    target_link_libraries(sources_outputs_live_acceptance PRIVATE
      nlohmann_json::nlohmann_json
	  utf8proc
      "-framework Security"
      "-framework Foundation"
    )
    target_compile_definitions(sources_outputs_live_acceptance PRIVATE
      "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    )
    target_compile_options(sources_outputs_live_acceptance PRIVATE
      -Wall -Wextra -Wpedantic)
    add_custom_command(TARGET sources_outputs_live_acceptance POST_BUILD
      COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
              "$<TARGET_FILE:sources_outputs_live_acceptance>"
      VERBATIM)
  endif()

  singularity_add_executable(papinian_native_benchmark
    PapinianGuard/Benchmark/PapinianGuardBenchmarkMain.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/Benchmark/PapinianGuardBenchmark.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Platform/Sha256/Sha256.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(papinian_native_benchmark PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\""
  )
  target_include_directories(papinian_native_benchmark PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_compile_definitions(papinian_native_benchmark PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  target_link_libraries(papinian_native_benchmark PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
	utf8proc
  )
  target_include_directories(papinian_native_benchmark SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(papinian_native_benchmark PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(papinian_native_benchmark PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(APPLE)
    singularity_onnxruntime_beside(papinian_native_benchmark)
  endif()

  # TEMPORARY - offline diagnostic for the connected-golden 1-external jetsam OOM
  # kill. Not part of any regression suite; remove this target and
  # tests/papinian_memory_growth_diagnostic.cpp once the memory-growth
  # question it exists to answer is closed.
  singularity_add_executable(papinian_memory_growth_diagnostic EXCLUDE_FROM_ALL
    tests/papinian_memory_growth_diagnostic.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_include_directories(papinian_memory_growth_diagnostic PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_compile_definitions(papinian_memory_growth_diagnostic PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  target_link_libraries(papinian_memory_growth_diagnostic PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
	utf8proc
  )
  target_include_directories(papinian_memory_growth_diagnostic SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_options(papinian_memory_growth_diagnostic PRIVATE -Wall -Wextra -Wpedantic)
  if(APPLE)
    singularity_onnxruntime_beside(papinian_memory_growth_diagnostic)
    add_custom_command(
      TARGET papinian_memory_growth_diagnostic POST_BUILD
      COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
              "$<TARGET_FILE:papinian_memory_growth_diagnostic>"
      VERBATIM
    )
  endif()

  # Measures what Papinian Guard masks in documents, protected the way a request
  # carries them (tests/papinian_masking_measure.cpp). Not a test: the
  # Papinian masking benchmark builds and runs it.
  singularity_add_executable(papinian_masking_measure EXCLUDE_FROM_ALL
    tests/papinian_masking_measure.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/Policy/Policy.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
  )
  if(APPLE)
    target_sources(papinian_masking_measure PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
  else()
    target_sources(papinian_masking_measure PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
  endif()
  target_include_directories(papinian_masking_measure PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_compile_definitions(papinian_masking_measure PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  target_link_libraries(papinian_masking_measure PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
    libzip::zip
    pugixml::pugixml
    utf8proc
  )
  target_include_directories(papinian_masking_measure SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(papinian_masking_measure PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(papinian_masking_measure PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  singularity_onnxruntime_beside(papinian_masking_measure)
  if(APPLE)
    target_link_libraries(papinian_masking_measure PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
    add_custom_command(
      TARGET papinian_masking_measure POST_BUILD
      COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
              "$<TARGET_FILE:papinian_masking_measure>"
      VERBATIM
    )
  endif()
  # The guard reads ordinary words from the list the application carries in its
  # Resources; without it the guard masks more than the application does.
  add_custom_command(
    TARGET papinian_masking_measure POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/CommonWords/english_common_words.txt"
            "$<TARGET_FILE_DIR:papinian_masking_measure>/english_common_words.txt"
    VERBATIM
  )

  # TEMPORARY - layer 1 of the same investigation: calls IGliner2Runtime::infer()
  # directly, bypassing PapinianGuard entirely. Remove alongside
  # papinian_memory_growth_diagnostic once closed.
  singularity_add_executable(papinian_layer1_engine_diagnostic EXCLUDE_FROM_ALL
    tests/papinian_layer1_engine_diagnostic.cpp
    LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(papinian_layer1_engine_diagnostic PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(papinian_layer1_engine_diagnostic PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    native_app_gliner2_bridge_library
  )
  target_include_directories(papinian_layer1_engine_diagnostic SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_options(papinian_layer1_engine_diagnostic PRIVATE -Wall -Wextra -Wpedantic)
  if(APPLE)
    singularity_onnxruntime_beside(papinian_layer1_engine_diagnostic)
    add_custom_command(
      TARGET papinian_layer1_engine_diagnostic POST_BUILD
      COMMAND /usr/bin/codesign --force --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
              "$<TARGET_FILE:papinian_layer1_engine_diagnostic>"
      VERBATIM
    )
  endif()

  singularity_add_executable(workspace_registry_tests
    Node/LegacyWorkspace/LegacyWorkspace.test.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_include_directories(workspace_registry_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(workspace_registry_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(workspace_registry_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME workspace_registry COMMAND workspace_registry_tests)

  singularity_add_executable(matter_registry_tests
    Matter/MatterRepository/MatterRepository.test.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  if(APPLE)
    target_sources(matter_registry_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
  else()
    target_sources(matter_registry_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
  endif()
  target_include_directories(matter_registry_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(matter_registry_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
  target_include_directories(matter_registry_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(matter_registry_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(matter_registry_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(matter_registry_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(matter_registry_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME matter_registry COMMAND matter_registry_tests)

  singularity_add_executable(model_classification_tests
    Provider/ModelClassification/ModelClassification.test.cpp
    Provider/ModelClassification/ModelClassification.cpp
  )
  target_include_directories(model_classification_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(model_classification_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(model_classification_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME model_classification COMMAND model_classification_tests)

  # Header-only: utf8_scalar_offset()/utf8_scalar_offsets() are pure
  # string/offset math with no ONNX/native-runtime dependency (see
  # Gliner2Runtime.hpp's own comment) - this target deliberately never links
  # Gliner2Runtime.cpp or native_app_gliner2_bridge_library, unlike
  # papinian_guard_tests, so the offset-conversion algorithm itself can be
  # regression-tested without the heavy native model runtime.
  singularity_add_executable(gliner2_runtime_offset_tests
    LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.test.cpp
  )
  target_include_directories(gliner2_runtime_offset_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(gliner2_runtime_offset_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(gliner2_runtime_offset_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME gliner2_runtime_offset COMMAND gliner2_runtime_offset_tests)

  # Header-only: the data room's journal as a request is given it.
  singularity_add_executable(document_journal_tests
    Document/DocumentJournal/DocumentJournal.test.cpp
  )
  target_include_directories(document_journal_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_journal_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(document_journal_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_journal_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_journal COMMAND document_journal_tests)

  # Which documents a request needs, picked from a list: the list, the named
  # documents, the prompt and the reading of the reply.
  singularity_add_executable(source_picker_tests
    Context/SourcePicker/SourcePicker.test.cpp
    Context/SourcePicker/SourcePicker.cpp
  )
  target_include_directories(source_picker_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(source_picker_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
  if(MSVC)
    target_compile_options(source_picker_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(source_picker_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME source_picker COMMAND source_picker_tests)

  # Arithmetic an answer asks Singularity to work out on the lawyer's computer,
  # over values Papinian Guard kept from the model.
  singularity_add_executable(computed_statements_tests
    Context/ComputedStatements/ComputedStatements.test.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_include_directories(computed_statements_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(computed_statements_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(computed_statements_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME computed_statements COMMAND computed_statements_tests)

  # Protected quantities and dates as typed values: exact arithmetic, relations
  # and expressions worked out on the lawyer's computer, never sent.
  singularity_add_executable(semantic_algebra_tests
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.test.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
  )
  target_include_directories(semantic_algebra_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  # A long expression is worked out on a worker thread's stack in one check.
  target_link_libraries(semantic_algebra_tests PRIVATE Threads::Threads)
  if(NOT MSVC)
    target_compile_options(semantic_algebra_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME semantic_algebra COMMAND semantic_algebra_tests)

  # Business days counted by the US federal and Russian production calendars.
  singularity_add_executable(business_calendar_tests
    Context/BusinessCalendar/BusinessCalendar.test.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
  )
  target_include_directories(business_calendar_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(business_calendar_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME business_calendar COMMAND business_calendar_tests)

  # Relations over protected amounts and dates, worked out locally and sent as symbols.
  singularity_add_executable(protected_relations_tests
    PapinianGuard/ProtectedRelations/ProtectedRelations.test.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
  )
  target_include_directories(protected_relations_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(protected_relations_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME protected_relations COMMAND protected_relations_tests)

  # What a protected symbol looks like, and reading symbols back (header-only).
  singularity_add_executable(protected_markers_tests
    PapinianGuard/ProtectedMarkers/ProtectedMarkers.test.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
  )
  target_include_directories(protected_markers_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(protected_markers_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME protected_markers COMMAND protected_markers_tests)

  # Singularity's own continuation lines, told apart from the lawyer's data.
  singularity_add_executable(control_text_tests
    Context/ControlText/ControlText.test.cpp
    Context/ControlText/ControlText.cpp
  )
  target_include_directories(control_text_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(control_text_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME control_text COMMAND control_text_tests)

  # Restored names in the form their place in a Russian sentence needs.
  singularity_add_executable(restoration_grammar_tests
    PapinianGuard/RestorationGrammar/RestorationGrammar.test.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_include_directories(restoration_grammar_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(restoration_grammar_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME restoration_grammar COMMAND restoration_grammar_tests)

  # Russian names declined, and words agreed with a name's gender, on the
  # lawyer's computer (header-only).
  singularity_add_executable(russian_names_tests
    Context/RussianNames/RussianNames.test.cpp
  )
  target_include_directories(russian_names_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(russian_names_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME russian_names COMMAND russian_names_tests)

  # Dates read on the lawyer's computer, and the order of a request's documents
  # given to the model as relations - never as dates.
  singularity_add_executable(temporal_semantics_tests
    Context/TemporalSemantics/TemporalSemantics.test.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
  )
  target_include_directories(temporal_semantics_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(temporal_semantics_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME temporal_semantics COMMAND temporal_semantics_tests)
  singularity_add_executable(temporal_context_tests
    Context/TemporalContext/TemporalContext.test.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
  )
  target_include_directories(temporal_context_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(temporal_context_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME temporal_context COMMAND temporal_context_tests)

  # Read-only Project inspection for one operation: the request's source
  # boundary over names and words, one room per call, and the provenance basis
  # every found passage is cited in - on real extracted documents, through the
  # production ToolBroker.
  singularity_add_executable(project_evidence_tests
    Context/ProjectEvidence/ProjectEvidence.test.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
  )
  if(APPLE)
    target_sources(project_evidence_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
  else()
    target_sources(project_evidence_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
  endif()
  target_include_directories(project_evidence_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(project_evidence_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
  target_include_directories(project_evidence_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(project_evidence_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(project_evidence_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(project_evidence_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(project_evidence_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME project_evidence COMMAND project_evidence_tests)

  # Header-only: validation of a file the interface built and one file write.
  singularity_add_executable(generated_file_bridge_tests
    Bridge/GeneratedFileBridge/GeneratedFileBridge.test.cpp
  )
  target_include_directories(generated_file_bridge_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(generated_file_bridge_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(generated_file_bridge_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(generated_file_bridge_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME generated_file_bridge COMMAND generated_file_bridge_tests)

  singularity_add_executable(operation_event_tests
    Routing/Observability/OperationEvent.test.cpp
    Bridge/NativeBridge/BackgroundExecutor.cpp
  )
  target_include_directories(operation_event_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(operation_event_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(operation_event_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME operation_event COMMAND operation_event_tests)

  singularity_add_executable(execution_progress_tests
    Routing/Observability/ExecutionProgress.test.cpp
  )
  target_include_directories(execution_progress_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(execution_progress_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(execution_progress_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME execution_progress COMMAND execution_progress_tests)

  singularity_add_executable(provider_registry_tests
    Provider/ProviderRegistry/ProviderRegistry.test.cpp
    Provider/ProviderRegistry/ProviderRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Platform/Sha256/Sha256.cpp
  )
  if(APPLE)
    target_sources(provider_registry_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientMacOS.mm")
  elseif(WIN32)
    target_sources(provider_registry_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientWindows.cpp")
    target_link_libraries(provider_registry_tests PRIVATE winhttp)
  else()
    target_sources(provider_registry_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientLinux.cpp")
  endif()
  target_include_directories(provider_registry_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(provider_registry_tests PRIVATE nlohmann_json::nlohmann_json)
  if(APPLE)
    target_link_libraries(provider_registry_tests PRIVATE "-framework Foundation" "-framework Security")
  endif()
  if(MSVC)
    target_compile_options(provider_registry_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(provider_registry_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME provider_registry COMMAND provider_registry_tests)

  singularity_add_executable(routing_log_tests
    Routing/RoutingLog/RoutingLog.test.cpp
    Routing/RoutingLog/RoutingLog.cpp
  )
  target_include_directories(routing_log_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(routing_log_tests PRIVATE nlohmann_json::nlohmann_json)
  target_include_directories(routing_log_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(routing_log_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(routing_log_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_routing_log COMMAND routing_log_tests)

  # What a protected request sent, read back beside the local text: the
  # alignment of a passage's sent form with the words it was built from, and
  # the sent passages the Routing Log keeps (never a restoration mapping).
  singularity_add_executable(sent_passages_tests
    Privacy/SentPassages/SentPassages.test.cpp
    Privacy/SentPassages/SentPassages.cpp
    Routing/RoutingLog/RoutingLog.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(sent_passages_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(sent_passages_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
  if(MSVC)
    target_compile_options(sent_passages_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(sent_passages_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME sent_passages COMMAND sent_passages_tests)

  # A record of what the Project remembers, found again from the Project's own
  # records for the disclosure of what left: whole journal lines first, and a
  # search too wide to finish said so rather than called a change.
  singularity_add_executable(context_records_tests
    Privacy/ContextRecords/ContextRecords.test.cpp
    Privacy/ContextRecords/ContextRecords.cpp
    Privacy/SentPassages/SentPassages.cpp
    Routing/RoutingLog/RoutingLog.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(context_records_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(context_records_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
  if(MSVC)
    target_compile_options(context_records_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(context_records_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME context_records COMMAND context_records_tests)

	singularity_add_executable(usage_accounting_tests
		Usage/UsageAccounting/UsageAccounting.test.cpp
		Usage/UsageAccounting/UsageAccounting.cpp
		Usage/UsageStatement/UsageStatement.cpp
		Platform/Sha256/Sha256.cpp
	)
	target_include_directories(usage_accounting_tests PRIVATE
		"${NATIVE_APP_SOURCE_DIR}"
	)
	target_link_libraries(usage_accounting_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
	target_include_directories(usage_accounting_tests SYSTEM PRIVATE
		"$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
	)
	if(MSVC)
		target_compile_options(usage_accounting_tests PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(usage_accounting_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME beta1_usage_accounting COMMAND usage_accounting_tests)

	# A usage statement on a full disk is refused as DiskFull with no path and
	# leaves nothing behind; with room again both files are written.
	singularity_add_executable(usage_statement_tests
		Usage/UsageStatement/UsageStatement.test.cpp
		Usage/UsageStatement/UsageStatement.cpp
		Usage/UsageAccounting/UsageAccounting.cpp
		Platform/Sha256/Sha256.cpp
	)
	target_include_directories(usage_statement_tests PRIVATE
		"${NATIVE_APP_SOURCE_DIR}"
	)
	target_link_libraries(usage_statement_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
	target_include_directories(usage_statement_tests SYSTEM PRIVATE
		"$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
	)
	if(MSVC)
		target_compile_options(usage_statement_tests PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(usage_statement_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME beta1_usage_statement COMMAND usage_statement_tests)

  singularity_add_executable(ai_setup_tests
    Routing/AiSetup/AiSetup.test.cpp
    Routing/AiSetup/AiSetup.cpp
  )
  target_include_directories(ai_setup_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  # Platform/AppSettings.hpp declares its JSON forms (D-210).
  target_include_directories(ai_setup_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(ai_setup_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(ai_setup_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_ai_setup COMMAND ai_setup_tests)

  # Pure decision logic for Local storage/Manage's "Remove from this
  # computer" (Phase 07) - deliberately links only AiSetup.cpp, never
  # LocalModelRegistry.cpp/LocalInferenceService.cpp/the local model
  # runtime, so this stays a cheap, runtime-independent regression target
  # even though LocalModelRemovalPolicy.hpp's types come from
  # LocalModelRegistry.hpp (header-only use - see LocalModelBridge/LocalModelRemovalPolicy.cpp).
  singularity_add_executable(local_model_removal_policy_tests
    Bridge/LocalModelBridge/LocalModelRemovalPolicy.test.cpp
    Bridge/LocalModelBridge/LocalModelRemovalPolicy.cpp
    Routing/AiSetup/AiSetup.cpp
  )
  target_include_directories(local_model_removal_policy_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  # Platform/AppSettings.hpp declares its JSON forms (D-210).
  target_include_directories(local_model_removal_policy_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(local_model_removal_policy_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(local_model_removal_policy_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME local_model_removal_policy COMMAND local_model_removal_policy_tests)

  # The workspace-scan contract (Phase 07) - real Node/Project/Chat
  # assignment gathering, fail-closed on an uninspectable workspace. Links
  # only AiSetup.cpp; Matter/Chat types are header-only for construction,
  # so this stays as cheap as local_model_removal_policy_tests above.
  singularity_add_executable(local_model_assignment_scan_tests
    Bridge/LocalModelBridge/LocalModelAssignmentScan.test.cpp
    Bridge/LocalModelBridge/LocalModelAssignmentScan.cpp
    Routing/AiSetup/AiSetup.cpp
  )
  target_include_directories(local_model_assignment_scan_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(local_model_assignment_scan_tests PRIVATE nlohmann_json::nlohmann_json)
  target_include_directories(local_model_assignment_scan_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(local_model_assignment_scan_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(local_model_assignment_scan_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME local_model_assignment_scan COMMAND local_model_assignment_scan_tests)

  singularity_add_executable(model_lifecycle_tests
    LocalModel/ModelLifecycle/ModelLifecycle.test.cpp
    LocalModel/ModelLifecycle/ModelLifecycle.cpp
  )
  target_include_directories(model_lifecycle_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(model_lifecycle_tests PRIVATE Threads::Threads)
  if(MSVC)
    target_compile_options(model_lifecycle_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(model_lifecycle_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME model_lifecycle COMMAND model_lifecycle_tests)

  singularity_add_executable(execution_compatibility_tests
    LocalModel/ExecutionCompatibility/ExecutionCompatibility.test.cpp
    LocalModel/ExecutionCompatibility/ExecutionCompatibility.cpp
    LocalModel/ExecutionBackend/ExecutionBackend.cpp
    Node/DeviceCapabilities/DeviceCapabilities.cpp
  )
  target_include_directories(execution_compatibility_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_options(execution_compatibility_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(execution_compatibility_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME execution_compatibility COMMAND execution_compatibility_tests)

  singularity_add_executable(local_model_registry_tests
    LocalModel/LocalModelRegistry/LocalModelRegistry.test.cpp
    LocalModel/ExecutionBackend/ExecutionBackend.cpp
    LocalModel/ExecutionCompatibility/ExecutionCompatibility.cpp
    LocalModel/LocalModelRegistry/LocalModelRegistry.cpp
    Node/DeviceCapabilities/DeviceCapabilities.cpp
    ${NATIVE_APP_HARDWARE_SOURCES}
  )
  target_include_directories(local_model_registry_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(local_model_registry_tests PRIVATE "-framework Metal" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(local_model_registry_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(local_model_registry_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME local_model_registry COMMAND local_model_registry_tests)

  singularity_add_executable(user_model_files_tests
    LocalModel/UserModelFiles/UserModelFiles.test.cpp
    LocalModel/UserModelFiles/UserModelFiles.cpp
    Platform/AppPaths/AppPaths.cpp
  )
  target_include_directories(user_model_files_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_compile_definitions(user_model_files_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\"")
  if(MSVC)
    target_compile_options(user_model_files_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(user_model_files_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME user_model_files COMMAND user_model_files_tests)

  # The real uninstall orchestration (Phase 07) - a real
  # hardware::LocalModelRegistry and real filesystem operations prove
  # "not installed/required/loaded/Node-assigned/Project-assigned/Chat-
  # assigned -> refuse", "deletion failure -> command fails, no state
  # mutation" and "success -> real artifact gone, correct identity/state
  # semantics" end to end, without ever linking
  # LocalInferenceService.cpp/the local-model runtime (a fake collaborator
  # stands in for LocalInferenceService::rediscover_after_package_change()'s underlying disposition using the
  # identical std::filesystem::remove()+error-check primitive) - see
  # LocalModelUninstallOrchestrator.test.cpp's own comments. Reuses
  # local_model_registry_tests' own device-capability source selection
  # above for the same real LocalModelRegistry dependency set.
  singularity_add_executable(local_model_uninstall_orchestrator_tests
    LocalModel/ExecutionBackend/ExecutionBackend.cpp
    LocalModel/ExecutionCompatibility/ExecutionCompatibility.cpp
    LocalModel/LocalModelRegistry/LocalModelRegistry.cpp
    Node/DeviceCapabilities/DeviceCapabilities.cpp
    ${NATIVE_APP_HARDWARE_SOURCES}
    Bridge/LocalModelBridge/LocalModelUninstallOrchestrator.test.cpp
    Bridge/LocalModelBridge/LocalModelUninstallOrchestrator.cpp
    Bridge/LocalModelBridge/LocalModelRemovalPolicy.cpp
    Bridge/LocalModelBridge/LocalModelAssignmentScan.cpp
    Routing/AiSetup/AiSetup.cpp
  )
  target_include_directories(local_model_uninstall_orchestrator_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(local_model_uninstall_orchestrator_tests PRIVATE nlohmann_json::nlohmann_json)
  target_include_directories(local_model_uninstall_orchestrator_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(APPLE)
    target_link_libraries(local_model_uninstall_orchestrator_tests PRIVATE "-framework Metal" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(local_model_uninstall_orchestrator_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(local_model_uninstall_orchestrator_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME local_model_uninstall_orchestrator COMMAND local_model_uninstall_orchestrator_tests)

  singularity_add_executable(hardware_calibration_tests
    Node/HardwareCalibration/HardwareCalibration.test.cpp
    Node/HardwareCalibration/HardwareCalibration.cpp
    LocalModel/ExecutionBackend/ExecutionBackend.cpp
    ${NATIVE_APP_HARDWARE_SOURCES}
  )
  target_include_directories(hardware_calibration_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(hardware_calibration_tests PRIVATE nlohmann_json::nlohmann_json)
  if(APPLE)
    target_link_libraries(hardware_calibration_tests PRIVATE "-framework Metal" "-framework Foundation")
  endif()
  target_include_directories(hardware_calibration_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(hardware_calibration_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(hardware_calibration_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME hardware_calibration COMMAND hardware_calibration_tests)

  singularity_add_executable(resource_benchmark_tests
    LocalModel/ResourceBenchmark/ResourceBenchmark.test.cpp
    LocalModel/ResourceBenchmark/ResourceBenchmark.cpp
    Node/DeviceCapabilities/DeviceCapabilities.cpp
    ${NATIVE_APP_HARDWARE_SOURCES}
    Platform/ProcessMetrics/ProcessMetrics.cpp
  )
  if(APPLE)
    target_link_libraries(resource_benchmark_tests PRIVATE "-framework Metal" "-framework Foundation")
  endif()
  if(WIN32)
    target_link_libraries(resource_benchmark_tests PRIVATE psapi)
  endif()
  target_include_directories(resource_benchmark_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(resource_benchmark_tests PRIVATE nlohmann_json::nlohmann_json)
  target_include_directories(resource_benchmark_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(resource_benchmark_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(resource_benchmark_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME resource_benchmark COMMAND resource_benchmark_tests)

  # Local inference's own components and the platform pieces it runs on.
  include(cmake/LocalInferenceTests.cmake)

  singularity_add_executable(app_settings_tests
    Platform/AppSettings/AppSettings.test.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/AppPaths/AppPaths.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_include_directories(app_settings_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(app_settings_tests PRIVATE nlohmann_json::nlohmann_json)
  target_include_directories(app_settings_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(app_settings_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(MSVC)
    target_compile_options(app_settings_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(app_settings_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME app_settings COMMAND app_settings_tests)

	 singularity_add_executable(permission_access_tests
		Platform/PermissionAccess/PermissionAccess.test.cpp
		Platform/PermissionAccess/PermissionAccess.cpp
		Node/ActiveWorkspace/ActiveWorkspace.cpp
		Platform/AppPaths/AppPaths.cpp
		Platform/AppSettings/AppSettings.cpp
		PapinianGuard/Policy/Policy.cpp
		Provider/ModelClassification/ModelClassification.cpp
		Node/LegacyWorkspace/LegacyWorkspace.cpp
	)
	target_include_directories(permission_access_tests PRIVATE
		"${NATIVE_APP_SOURCE_DIR}"
	)
	target_link_libraries(permission_access_tests PRIVATE nlohmann_json::nlohmann_json)
	target_include_directories(permission_access_tests SYSTEM PRIVATE
		"$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
	)
	target_compile_definitions(permission_access_tests PRIVATE
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
	)
	if(MSVC)
		target_compile_options(permission_access_tests PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(permission_access_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME beta1_permission_access COMMAND permission_access_tests)

  if(APPLE)
    singularity_add_executable(credential_store_tests
      Provider/CredentialStore/CredentialStore.test.mm
      Platform/AppSettings/AppSettings.cpp
      Provider/CredentialStore/CredentialStoreMacOS.mm
      PapinianGuard/Policy/Policy.cpp
      Provider/ModelClassification/ModelClassification.cpp
      Node/LegacyWorkspace/LegacyWorkspace.cpp
    )
    target_include_directories(credential_store_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(credential_store_tests PRIVATE
      nlohmann_json::nlohmann_json
      "-framework Foundation"
    )
    target_compile_definitions(credential_store_tests PRIVATE
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}-credential-test\""
    )
    target_compile_options(credential_store_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME beta1_credential_store COMMAND credential_store_tests)
    # A real, rare, non-deterministic write failure was observed once under
    # heavy parallel ctest load ("The local provider credential could not be
    # stored securely.") - never on direct invocation, never on isolated
    # ctest invocation, and never reproduced across 4 further heavy-load
    # stress attempts (with diagnostics capturing the real NSError, which
    # never fired). This is real macOS Data Protection file-write behavior
    # (NSDataWritingFileProtectionComplete in CredentialStoreMacOS.mm), not
    # this test's own logic, and not something this session could prove a
    # single root cause for despite good-faith reproduction attempts.
    # RUN_SERIAL removes the one plausible correlating factor (resource
    # contention from running alongside this suite's own heaviest tests) at
    # effectively zero cost (this test runs in under 0.5s) - same convention
    # already used for beta1_operation_long_horizon_replay/beta1_cli_journey above. This never
    # weakens the test's own real security assertions (owner-only
    # permissions, complete file protection, backup exclusion, real
    # round-trip and deletion) - it only changes when the test is scheduled.
    set_tests_properties(beta1_credential_store PROPERTIES RUN_SERIAL TRUE)
  elseif(UNIX)
    # Linux keeps keys in the desktop keyring. The test runs its own private
    # D-Bus session bus and GNOME Keyring (dbus-daemon, gnome-keyring-daemon),
    # never the keyring of whoever runs it.
    singularity_add_executable(credential_store_tests
      Provider/CredentialStore/CredentialStoreLinux.test.cpp
      Platform/AppSettings/AppSettings.cpp
      Provider/CredentialStore/CredentialStoreLinux.cpp
      PapinianGuard/Policy/Policy.cpp
      Provider/ModelClassification/ModelClassification.cpp
      Node/LegacyWorkspace/LegacyWorkspace.cpp
    )
    target_include_directories(credential_store_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(credential_store_tests PRIVATE nlohmann_json::nlohmann_json PkgConfig::LIBSECRET)
    target_compile_definitions(credential_store_tests PRIVATE
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}-credential-test\""
    )
    target_compile_options(credential_store_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME beta1_credential_store COMMAND credential_store_tests)
    set_tests_properties(beta1_credential_store PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
  endif()

  singularity_add_executable(system_prompt_profiles_tests
    Routing/SystemPromptProfiles/SystemPromptProfiles.test.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
  )
  target_include_directories(system_prompt_profiles_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(system_prompt_profiles_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(system_prompt_profiles_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME system_prompt_profiles COMMAND system_prompt_profiles_tests)

  singularity_add_executable(legal_task_presets_tests
    Routing/LegalTaskPresets/LegalTaskPresets.test.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
  )
  target_include_directories(legal_task_presets_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(legal_task_presets_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(legal_task_presets_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME legal_task_presets COMMAND legal_task_presets_tests)

  singularity_add_executable(workflow_definitions_tests
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.test.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
  )
  target_include_directories(workflow_definitions_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(workflow_definitions_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(workflow_definitions_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME workflow_definitions COMMAND workflow_definitions_tests)

  singularity_add_executable(context_retriever_tests
    Context/ContextRetriever/ContextRetriever.test.cpp
    Context/ContextRetriever/ContextRetriever.cpp
  )
  target_include_directories(context_retriever_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(context_retriever_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(context_retriever_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME context_retriever COMMAND context_retriever_tests)

  singularity_add_executable(live_answer_tests
    Routing/LiveAnswer/LiveAnswer.test.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
  )
  target_include_directories(live_answer_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(live_answer_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(live_answer_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME live_answer COMMAND live_answer_tests)

  singularity_add_executable(source_markers_tests
    Context/SourceMarkers/SourceMarkers.test.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
  )
  target_include_directories(source_markers_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(source_markers_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(source_markers_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME source_markers COMMAND source_markers_tests)

  singularity_add_executable(findings_tests
    WorkProduct/FindingParser/FindingParser.test.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
  )
  target_include_directories(findings_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(findings_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(findings_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME findings COMMAND findings_tests)

  singularity_add_executable(obligations_tests
    WorkProduct/ObligationParser/ObligationParser.test.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
  )
  target_include_directories(obligations_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(obligations_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(obligations_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME obligations COMMAND obligations_tests)

  singularity_add_executable(chronology_tests
    WorkProduct/ChronologyParser/ChronologyParser.test.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
  )
  target_include_directories(chronology_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(chronology_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chronology_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME chronology COMMAND chronology_tests)

  singularity_add_executable(comparison_tests
    WorkProduct/ComparisonParser/ComparisonParser.test.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
  )
  target_include_directories(comparison_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(comparison_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(comparison_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME comparison COMMAND comparison_tests)

  singularity_add_executable(drafting_tests
    Context/MatterContextBuilder/MatterContextBuilder.test.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
  )
  target_include_directories(drafting_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  if(MSVC)
    target_compile_options(drafting_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(drafting_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME drafting COMMAND drafting_tests)

  singularity_add_executable(prompt_assembler_tests
    Context/PromptAssembler/PromptAssembler.test.cpp
    Context/ControlText/ControlText.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
  )
  target_include_directories(prompt_assembler_tests PRIVATE ${NATIVE_APP_SOURCE_DIR})
  target_link_libraries(prompt_assembler_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(prompt_assembler_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(prompt_assembler_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_context_prompt_assembly COMMAND prompt_assembler_tests)

  singularity_add_executable(persistence_coordinator_tests
    Persistence/PersistenceCoordinator/PersistenceCoordinator.test.cpp
    Platform/AppSettings/AppSettings.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_include_directories(persistence_coordinator_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(persistence_coordinator_tests PRIVATE
    nlohmann_json::nlohmann_json
  )
  target_include_directories(persistence_coordinator_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(persistence_coordinator_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(MSVC)
    target_compile_options(persistence_coordinator_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(persistence_coordinator_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME persistence_coordinator COMMAND persistence_coordinator_tests)

  singularity_add_executable(actor_identity_tests
    Platform/ActorIdentity/ActorIdentity.test.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppSettings/AppSettings.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_include_directories(actor_identity_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(actor_identity_tests PRIVATE
    nlohmann_json::nlohmann_json
  )
  target_include_directories(actor_identity_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(actor_identity_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(MSVC)
    target_compile_options(actor_identity_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(actor_identity_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME actor_identity COMMAND actor_identity_tests)

  singularity_add_executable(project_store_tests
    Persistence/ProjectStore/ProjectStore.test.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  if(APPLE)
    target_sources(project_store_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
  else()
    target_sources(project_store_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
  endif()
  target_include_directories(project_store_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(project_store_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(project_store_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(project_store_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(project_store_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(project_store_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(project_store_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_project_persistence COMMAND project_store_tests)

	singularity_add_executable(work_product_actions_tests
		WorkProduct/WorkProductActions/WorkProductActions.test.cpp
		WorkProduct/WorkProductActions/WorkProductActions.cpp
		Context/ComputedStatements/ComputedStatements.cpp
		Context/TemporalContext/TemporalContext.cpp
		Context/TemporalSemantics/TemporalSemantics.cpp
		WorkProduct/WorkProductExport/WorkProductExport.cpp
		Persistence/ProjectStore/ProjectStore.cpp
		Platform/ActorIdentity/ActorIdentity.cpp
		Platform/AppPaths/AppPaths.cpp
		Platform/AppSettings/AppSettings.cpp
		Platform/OpenExternal/OpenExternal.cpp
		Platform/Sha256/Sha256.cpp
		PapinianGuard/Policy/Policy.cpp
		PapinianGuard/Taxonomy/Taxonomy.cpp
		Privacy/EntityRegistry/EntityRegistry.cpp
		Context/ContextRegistry/ContextRegistry.cpp
		Provider/ModelClassification/ModelClassification.cpp
		WorkProduct/ChronologyParser/ChronologyParser.cpp
		WorkProduct/ComparisonParser/ComparisonParser.cpp
		Context/ContextRetriever/ContextRetriever.cpp
		WorkProduct/FindingParser/FindingParser.cpp
		WorkProduct/ObligationParser/ObligationParser.cpp
		Context/SourceMarkers/SourceMarkers.cpp
		Context/QuoteVerification/QuoteVerification.cpp
		Document/DocumentCache/DocumentCache.cpp
		Document/DocumentChunker/DocumentChunker.cpp
		Document/DocumentExtractor/DocumentExtractor.cpp
		Document/CsvExtractor/CsvExtractor.cpp
		Document/PptxExtractor/PptxExtractor.cpp
		Document/XlsxExtractor/XlsxExtractor.cpp
		Document/XlsxWorkbookView/XlsxWorkbookView.cpp
		Document/XlsxNumberFormat/XlsxNumberFormat.cpp
		Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
		Matter/MatterRepository/MatterRepository.cpp
		Document/DocumentRegistry/DocumentRegistry.cpp
		Node/ActiveWorkspace/ActiveWorkspace.cpp
		Node/LegacyWorkspace/LegacyWorkspace.cpp
		PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
		Context/BusinessCalendar/BusinessCalendar.cpp
		PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
		PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
		Context/ControlText/ControlText.cpp
		PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
	)
	if(APPLE)
		target_sources(work_product_actions_tests PRIVATE
			"${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
	else()
		target_sources(work_product_actions_tests PRIVATE
			"${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
	endif()
	target_include_directories(work_product_actions_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(work_product_actions_tests PRIVATE
		nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
	target_include_directories(work_product_actions_tests SYSTEM PRIVATE
		"$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
		"$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
		"$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
	)
	target_compile_definitions(work_product_actions_tests PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
	)
	if(APPLE)
		target_link_libraries(work_product_actions_tests PRIVATE
			"-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
	endif()
	if(MSVC)
		target_compile_options(work_product_actions_tests PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(work_product_actions_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME work_product_actions COMMAND work_product_actions_tests)

	# The Project's memory keeper with a scripted local model: what it folds in,
	# what it persists, and that it never writes without one.
	singularity_add_executable(memory_keeper_tests
		Context/MemoryKeeper/MemoryKeeper.test.cpp
		Context/MemoryKeeper/MemoryKeeper.cpp
		Context/ContextCuration/ContextCuration.cpp
		Persistence/ProjectStore/ProjectStore.cpp
		Platform/ActorIdentity/ActorIdentity.cpp
		Platform/AppPaths/AppPaths.cpp
		Platform/AppSettings/AppSettings.cpp
		Platform/OpenExternal/OpenExternal.cpp
		Platform/Sha256/Sha256.cpp
		PapinianGuard/Policy/Policy.cpp
		PapinianGuard/Taxonomy/Taxonomy.cpp
		Privacy/EntityRegistry/EntityRegistry.cpp
		Context/ContextRegistry/ContextRegistry.cpp
		Provider/ModelClassification/ModelClassification.cpp
		WorkProduct/ChronologyParser/ChronologyParser.cpp
		WorkProduct/ComparisonParser/ComparisonParser.cpp
		Context/ContextRetriever/ContextRetriever.cpp
		WorkProduct/FindingParser/FindingParser.cpp
		WorkProduct/ObligationParser/ObligationParser.cpp
		Context/SourceMarkers/SourceMarkers.cpp
		Context/QuoteVerification/QuoteVerification.cpp
		Document/DocumentCache/DocumentCache.cpp
		Document/DocumentChunker/DocumentChunker.cpp
		Document/DocumentExtractor/DocumentExtractor.cpp
		Document/CsvExtractor/CsvExtractor.cpp
		Document/PptxExtractor/PptxExtractor.cpp
		Document/XlsxExtractor/XlsxExtractor.cpp
		Document/XlsxWorkbookView/XlsxWorkbookView.cpp
		Document/XlsxNumberFormat/XlsxNumberFormat.cpp
		Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
		Matter/MatterRepository/MatterRepository.cpp
		Context/TemporalContext/TemporalContext.cpp
		Context/TemporalSemantics/TemporalSemantics.cpp
		Document/DocumentRegistry/DocumentRegistry.cpp
		Node/ActiveWorkspace/ActiveWorkspace.cpp
		Node/LegacyWorkspace/LegacyWorkspace.cpp
	)
	if(APPLE)
		target_sources(memory_keeper_tests PRIVATE
			"${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
	else()
		target_sources(memory_keeper_tests PRIVATE
			"${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
	endif()
	target_include_directories(memory_keeper_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(memory_keeper_tests PRIVATE
		nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
	target_include_directories(memory_keeper_tests SYSTEM PRIVATE
		"$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
		"$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
		"$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
	)
	target_compile_definitions(memory_keeper_tests PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
	)
	if(APPLE)
		target_link_libraries(memory_keeper_tests PRIVATE
			"-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
	endif()
	if(MSVC)
		target_compile_options(memory_keeper_tests PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(memory_keeper_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME memory_keeper COMMAND memory_keeper_tests)

	singularity_add_executable(consilium_actions_tests
		Consilium/ConsiliumActions/ConsiliumActions.test.cpp
		Consilium/ConsiliumActions/ConsiliumActions.cpp
		Bridge/ConsiliumBridge/ConsiliumBridge.cpp
		Routing/AiSetup/AiSetup.cpp
		Persistence/ProjectStore/ProjectStore.cpp
		Platform/ActorIdentity/ActorIdentity.cpp
		Platform/AppPaths/AppPaths.cpp
		Platform/AppSettings/AppSettings.cpp
		Platform/OpenExternal/OpenExternal.cpp
		Platform/Sha256/Sha256.cpp
		PapinianGuard/Policy/Policy.cpp
		PapinianGuard/Taxonomy/Taxonomy.cpp
		Privacy/EntityRegistry/EntityRegistry.cpp
		Context/ContextRegistry/ContextRegistry.cpp
		Provider/ModelClassification/ModelClassification.cpp
		WorkProduct/ChronologyParser/ChronologyParser.cpp
		WorkProduct/ComparisonParser/ComparisonParser.cpp
		Context/ContextRetriever/ContextRetriever.cpp
		WorkProduct/FindingParser/FindingParser.cpp
		WorkProduct/ObligationParser/ObligationParser.cpp
		Context/SourceMarkers/SourceMarkers.cpp
		Context/QuoteVerification/QuoteVerification.cpp
		Document/DocumentCache/DocumentCache.cpp
		Document/DocumentChunker/DocumentChunker.cpp
		Document/DocumentExtractor/DocumentExtractor.cpp
		Document/CsvExtractor/CsvExtractor.cpp
		Document/PptxExtractor/PptxExtractor.cpp
		Document/XlsxExtractor/XlsxExtractor.cpp
		Document/XlsxWorkbookView/XlsxWorkbookView.cpp
		Document/XlsxNumberFormat/XlsxNumberFormat.cpp
		Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
		Matter/MatterRepository/MatterRepository.cpp
		Context/TemporalContext/TemporalContext.cpp
		Context/TemporalSemantics/TemporalSemantics.cpp
		Document/DocumentRegistry/DocumentRegistry.cpp
		Node/ActiveWorkspace/ActiveWorkspace.cpp
		Node/LegacyWorkspace/LegacyWorkspace.cpp
	)
	if(APPLE)
		target_sources(consilium_actions_tests PRIVATE
			"${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
	else()
		target_sources(consilium_actions_tests PRIVATE
			"${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
	endif()
	target_include_directories(consilium_actions_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(consilium_actions_tests PRIVATE
		nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
	target_include_directories(consilium_actions_tests SYSTEM PRIVATE
		"$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
		"$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
		"$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
	)
	target_compile_definitions(consilium_actions_tests PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
	)
	if(APPLE)
		target_link_libraries(consilium_actions_tests PRIVATE
			"-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
	endif()
	if(MSVC)
		target_compile_options(consilium_actions_tests PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(consilium_actions_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME beta1_consilium_actions COMMAND consilium_actions_tests)

  singularity_add_executable(document_registry_tests
    Document/DocumentRegistry/DocumentRegistry.test.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(document_registry_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(document_registry_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(document_registry_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(document_registry_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_registry_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(document_registry_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(document_registry_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(document_registry_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(document_registry_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_registry_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_registry COMMAND document_registry_tests)

  # --- Privacy (Phase 10 - entities and Project coverage) -----------------
  #
  # entity_resolver_tests/entity_registry_tests/entity_curation_tests link the
  # same Document/Context/WorkProduct/Persistence source set as
  # project_store_tests/document_registry_tests above: EntityRegistry.cpp
  # calls Document/Types/MatterDocument.hpp's document_lifecycle_state()/
  # to_string(), both defined in MatterRepository.cpp, which pulls in its own
  # real dependency graph. None of the three need the Papinian NER/ONNX
  # runtime - Entity Registry query/curation logic is model-agnostic; only
  # project_preprocessing_tests below exercises real Guard detection.
  set(NATIVE_APP_PRIVACY_MATTER_SOURCES
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )

  function(singularity_add_privacy_matter_test target_name test_source)
    singularity_add_executable(${target_name} ${test_source} Privacy/EntityRegistry/EntityRegistry.cpp
      ${NATIVE_APP_PRIVACY_MATTER_SOURCES})
    if(APPLE)
      target_sources(${target_name} PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
    else()
      target_sources(${target_name} PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
    endif()
    target_include_directories(${target_name} PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(${target_name} PRIVATE nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
    target_include_directories(${target_name} SYSTEM PRIVATE
      "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
      "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
      "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    )
    # Papinian Guard reads the public domain word list beside the executable or,
    # in a build like this one, beside the sources.
    target_compile_definitions(${target_name} PRIVATE
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
      "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
      "NATIVE_APP_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../..\"")
    if(APPLE)
      target_link_libraries(${target_name} PRIVATE "-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
    endif()
    if(MSVC)
      target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endfunction()

  # Header-only, no Project state: the multi-pattern scanner Papinian Guard
  # finds known names with.
  # Checking an answer's quotations against the passages it cited.
  singularity_add_executable(quote_verification_tests
    Context/QuoteVerification/QuoteVerification.test.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Context/SourceMarkers/SourceMarkers.cpp)
  target_include_directories(quote_verification_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(quote_verification_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(quote_verification_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(quote_verification_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME quote_verification COMMAND quote_verification_tests)

  singularity_add_executable(office_text_edit_tests
    Document/OfficeTextEdit/OfficeTextEdit.test.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Context/SourceMarkers/SourceMarkers.cpp)
  target_include_directories(office_text_edit_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(office_text_edit_tests PRIVATE nlohmann_json::nlohmann_json pugixml::pugixml)
  if(MSVC)
    target_compile_options(office_text_edit_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(office_text_edit_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME office_text_edit COMMAND office_text_edit_tests)

  singularity_add_executable(pattern_scanner_tests PapinianGuard/PatternScanner/PatternScanner.test.cpp)
  target_include_directories(pattern_scanner_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(pattern_scanner_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(pattern_scanner_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME pattern_scanner COMMAND pattern_scanner_tests)

  singularity_add_privacy_matter_test(entity_registry_tests Privacy/EntityRegistry/EntityRegistry.test.cpp)
  add_test(NAME entity_registry COMMAND entity_registry_tests)

  singularity_add_privacy_matter_test(entity_resolver_tests Privacy/EntityResolver/EntityResolver.test.cpp)
  target_sources(entity_resolver_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Privacy/EntityResolver/EntityResolver.cpp")
  add_test(NAME entity_resolver COMMAND entity_resolver_tests)

  singularity_add_privacy_matter_test(entity_curation_tests Privacy/EntityCuration/EntityCuration.test.cpp)
  target_sources(entity_curation_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}/Privacy/EntityCuration/EntityCuration.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Privacy/EntityResolver/EntityResolver.cpp"
  )
  add_test(NAME entity_curation COMMAND entity_curation_tests)

  # project_preprocessing_tests additionally exercises the real (fixture-
  # backed, no ONNX weights needed) Papinian Guard detection pipeline -
  # Gliner2Adapter.cpp has no ONNX/Gliner2Runtime dependency of its own (only
  # VerifiedGliner2Runtime does), so this stays as light as
  # entity_registry_tests above plus the Guard/taxonomy sources themselves.
  singularity_add_privacy_matter_test(project_preprocessing_tests Privacy/ProjectPreprocessing/ProjectPreprocessing.test.cpp)
  target_sources(project_preprocessing_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}/Privacy/ProjectPreprocessing/ProjectPreprocessing.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Privacy/EntityResolver/EntityResolver.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Privacy/EntityCuration/EntityCuration.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/PapinianGuardService/PapinianGuardService.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/DeterministicDetector/DeterministicDetector.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectionTransformer/ProtectionTransformer.cpp"
    "${NATIVE_APP_SOURCE_DIR}/LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Context/BusinessCalendar/BusinessCalendar.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedRelations/ProtectedRelations.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/ProtectedOperations.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Context/ControlText/ControlText.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Context/TemporalSemantics/TemporalSemantics.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/RestorationGrammar/RestorationGrammar.cpp"
  )
  add_test(NAME project_preprocessing COMMAND project_preprocessing_tests)

  # Durable Papinian detection reuse, with a counting fake model - never the
  # real NER weights.
  singularity_add_privacy_matter_test(semantic_detection_store_tests
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.test.cpp)
  target_sources(semantic_detection_store_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/PapinianGuardService/PapinianGuardService.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/DeterministicDetector/DeterministicDetector.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectionTransformer/ProtectionTransformer.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Context/BusinessCalendar/BusinessCalendar.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedRelations/ProtectedRelations.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/ProtectedOperations.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Context/ControlText/ControlText.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Context/TemporalSemantics/TemporalSemantics.cpp"
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/RestorationGrammar/RestorationGrammar.cpp"
  )
  add_test(NAME semantic_detection_store COMMAND semantic_detection_store_tests)

  # Launch-time cleanup of local model runtimes an earlier, killed session
  # left behind - selection logic only, never a real process.
  singularity_add_executable(stale_runtime_reaper_tests
    Platform/StaleRuntimeReaper/StaleRuntimeReaper.test.cpp
    Platform/StaleRuntimeReaper/StaleRuntimeReaper.cpp
  )
  target_include_directories(stale_runtime_reaper_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  add_test(NAME stale_runtime_reaper COMMAND stale_runtime_reaper_tests)
  # Windows' reaping with real processes: stand-ins started suspended, as Singularity
  # starts its runtime, from copies of the test program, never the real
  # runtime.
  if(WIN32)
    singularity_add_executable(stale_runtime_reaper_windows_tests
      Platform/StaleRuntimeReaper/StaleRuntimeReaperWindows.test.cpp
      Platform/StaleRuntimeReaper/StaleRuntimeReaper.cpp
    )
    target_include_directories(stale_runtime_reaper_windows_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    if(MSVC)
      target_compile_options(stale_runtime_reaper_windows_tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME stale_runtime_reaper_windows COMMAND stale_runtime_reaper_windows_tests)
  endif()

  # FIPS 180-4 known answers for the platform SHA-256 (the system's own on
  # macOS, the portable implementation elsewhere).
  singularity_add_executable(sha256_tests
    Platform/Sha256/Sha256.test.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(sha256_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  add_test(NAME platform_sha256 COMMAND sha256_tests)

  # --- Context (Phase 12 - canonical data model and authority) ------------
  #
  # Same reasoning as the Privacy tests above: ContextRegistry.cpp calls
  # MatterRepository.cpp's own to_string()/from_string() helpers through
  # Matter, so both link the same NATIVE_APP_PRIVACY_MATTER_SOURCES set
  # (which already includes ContextRegistry.cpp - see its own comment).
  # Neither needs the Papinian NER/ONNX runtime; Context authority logic is
  # model-agnostic.
  singularity_add_privacy_matter_test(context_registry_tests Context/ContextRegistry/ContextRegistry.test.cpp)
  add_test(NAME beta1_context_registry COMMAND context_registry_tests)

  singularity_add_privacy_matter_test(context_curation_tests Context/ContextCuration/ContextCuration.test.cpp)
  target_sources(context_curation_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Context/ContextCuration/ContextCuration.cpp")
  add_test(NAME beta1_context_curation COMMAND context_curation_tests)

  singularity_add_privacy_matter_test(context_automation_tests Context/ContextAutomation/ContextAutomation.test.cpp)
  target_sources(context_automation_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}/Context/ContextAutomation/ContextAutomation.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Context/ContextCuration/ContextCuration.cpp"
  )
  add_test(NAME beta1_context_automation COMMAND context_automation_tests)

  singularity_add_executable(open_external_tests
    Platform/OpenExternal/OpenExternal.test.cpp
    Platform/OpenExternal/OpenExternal.cpp
  )
  target_include_directories(open_external_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  add_test(NAME open_external COMMAND open_external_tests)

  singularity_add_executable(window_navigation_tests
    Platform/WindowNavigation/WindowNavigation.test.cpp
    Platform/WindowNavigation/WindowNavigation.cpp
  )
  target_include_directories(window_navigation_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  add_test(NAME window_navigation COMMAND window_navigation_tests)
  # The folder watcher's lifetime contract: stop() and destruction wait for a
  # report being delivered, unless called from it (Platform/FileSystemWatcher).
  singularity_add_executable(file_system_watcher_tests
    Platform/FileSystemWatcher/FileSystemWatcher.test.cpp
  )
  if(APPLE)
    target_sources(file_system_watcher_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm")
    target_link_libraries(file_system_watcher_tests PRIVATE "-framework CoreServices" "-framework Foundation")
  else()
    target_sources(file_system_watcher_tests PRIVATE "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}")
  endif()
  target_include_directories(file_system_watcher_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(file_system_watcher_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(file_system_watcher_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME file_system_watcher COMMAND file_system_watcher_tests)
  if(APPLE)
    # A real WKWebView, no window: a page that leaves its own folder is
    # stopped by the window's navigation policy. Run by hand; not in ctest.
    singularity_add_executable(window_navigation_webkit_probe
      Platform/WindowNavigation/WindowNavigation.probe.mm
      Platform/WindowNavigation/WindowNavigation.cpp
      Platform/WindowNavigation/WindowNavigationMacOS.cpp
    )
    set_target_properties(window_navigation_webkit_probe PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_include_directories(window_navigation_webkit_probe PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(window_navigation_webkit_probe PRIVATE "-framework WebKit" "-framework AppKit"
      "-framework Foundation")
  endif()
  if(WIN32)
    # A real WebView2 in a window that is never shown: the window's own page
    # reaches native code; an iframe, another origin and a second window do
    # not. Run by hand (it needs a desktop session); not in ctest.
    singularity_add_executable(window_navigation_webview2_probe
      Platform/WindowNavigation/WindowNavigationWindows.probe.cpp
      Platform/WindowNavigation/WindowNavigation.cpp
      Platform/WindowNavigation/WindowNavigationWindows.cpp
    )
    set_target_properties(window_navigation_webview2_probe PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_include_directories(window_navigation_webview2_probe PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(window_navigation_webview2_probe PRIVATE webview::core ws2_32)
    if(MSVC)
      target_compile_options(window_navigation_webview2_probe PRIVATE /W4 /permissive- /utf-8)
    endif()
  if(WIN32)
    # A real WebView2 in a window that is never shown: after its renderer is
    # ended the page is shown again, and after its browser process is ended
    # the window is told (Platform/WebViewRecovery). Run by hand; not in ctest.
    singularity_add_executable(web_view_recovery_probe
      Platform/WebViewRecovery/WebViewRecoveryWindows.probe.cpp
      Platform/WebViewRecovery/WebViewRecoveryWindows.cpp
    )
    set_target_properties(web_view_recovery_probe PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_include_directories(web_view_recovery_probe PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(web_view_recovery_probe PRIVATE webview::core)
    if(MSVC)
      target_compile_options(web_view_recovery_probe PRIVATE /W4 /permissive- /utf-8)
    endif()
    # WebView2's COM without WRL (Platform/WebViewCom), which the Windows
    # package's MinGW build needs: every interface id it names is WebView2.h's
    # own, and a handler made from a function behaves as a COM object.
    singularity_add_executable(webview_com_tests Platform/WebViewCom/WebViewCom.test.cpp)
    target_include_directories(webview_com_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(webview_com_tests PRIVATE webview::core)
    add_test(NAME webview_com COMMAND webview_com_tests)
  endif()
  endif()
  if(UNIX AND NOT APPLE)
    # A real WebKitGTK window with the real webview library: the window's own
    # page reaches native code; an iframe, another origin and a second window
    # do not. Run by hand on a display (xvfb-run in the Linux container); not
    # in ctest.
    singularity_add_executable(window_navigation_webkitgtk_probe
      Platform/WindowNavigation/WindowNavigationLinux.probe.cpp
      Platform/WindowNavigation/WindowNavigation.cpp
      Platform/WindowNavigation/WindowNavigationLinux.cpp
    )
    set_target_properties(window_navigation_webkitgtk_probe PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_include_directories(window_navigation_webkitgtk_probe PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(window_navigation_webkitgtk_probe PRIVATE webview::core nlohmann_json::nlohmann_json)
  endif()

  singularity_add_executable(matter_actions_tests
    Matter/MatterActions/MatterActions.test.cpp
    Matter/MatterActions/MatterActions.cpp
    Document/DocumentActions/DocumentActions.cpp
    Document/DocumentFileActions/DocumentFileActions.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Usage/UsageQueries/UsageQueries.cpp
    Usage/UsageAccounting/UsageAccounting.cpp
    Routing/RoutingLog/RoutingLog.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(matter_actions_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  # Its Document actions add a document from outside the folder, and move one
  # to the system's own Trash (Document/DocumentFileActions).
  if(APPLE)
    target_sources(matter_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileMacOS.mm"
    )
  elseif(WIN32)
    target_sources(matter_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileWindows.cpp"
    )
    target_link_libraries(matter_actions_tests PRIVATE ole32 shell32)
  else()
    target_sources(matter_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileLinux.cpp"
    )
  endif()
  target_include_directories(matter_actions_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(matter_actions_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(matter_actions_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(matter_actions_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(matter_actions_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(matter_actions_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(matter_actions_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_project_actions COMMAND matter_actions_tests)

  # A Matter's letters filed from the emails in its folder (D-180), on the same
  # Matter stack as the Matter actions above.
  singularity_add_executable(correspondence_actions_tests
    Correspondence/CorrespondenceActions/CorrespondenceActions.test.cpp
    Correspondence/CorrespondenceActions/CorrespondenceActions.cpp
    Correspondence/LetterRouting/LetterRouting.cpp
    DocumentIntelligence/EmailDraft/EmailDraft.cpp
    Office/OfficePaths/OfficePaths.cpp
    Matter/MatterActions/MatterActions.cpp
    Document/DocumentActions/DocumentActions.cpp
    Document/DocumentFileActions/DocumentFileActions.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Usage/UsageQueries/UsageQueries.cpp
    Usage/UsageAccounting/UsageAccounting.cpp
    Routing/RoutingLog/RoutingLog.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  if(APPLE)
    target_sources(correspondence_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileMacOS.mm"
    )
    target_link_libraries(correspondence_actions_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  elseif(WIN32)
    target_sources(correspondence_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileWindows.cpp"
    )
    target_link_libraries(correspondence_actions_tests PRIVATE ole32 shell32)
  else()
    target_sources(correspondence_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileLinux.cpp"
    )
  endif()
  target_include_directories(correspondence_actions_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(correspondence_actions_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
  target_include_directories(correspondence_actions_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(correspondence_actions_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(MSVC)
    target_compile_options(correspondence_actions_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(correspondence_actions_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME correspondence_actions COMMAND correspondence_actions_tests)

  # The time members record on a Matter (D-181), on the same Matter stack as
  # the Matter actions above.
  singularity_add_executable(time_entries_tests
    Billing/TimeEntries/TimeEntries.test.cpp
    Billing/TimeEntries/TimeEntries.cpp
    Matter/MatterActions/MatterActions.cpp
    Document/DocumentActions/DocumentActions.cpp
    Document/DocumentFileActions/DocumentFileActions.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Usage/UsageQueries/UsageQueries.cpp
    Usage/UsageAccounting/UsageAccounting.cpp
    Routing/RoutingLog/RoutingLog.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  if(APPLE)
    target_sources(time_entries_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileMacOS.mm"
    )
    target_link_libraries(time_entries_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  elseif(WIN32)
    target_sources(time_entries_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileWindows.cpp"
    )
    target_link_libraries(time_entries_tests PRIVATE ole32 shell32)
  else()
    target_sources(time_entries_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileLinux.cpp"
    )
  endif()
  target_include_directories(time_entries_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(time_entries_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
  target_include_directories(time_entries_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(time_entries_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(MSVC)
    target_compile_options(time_entries_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(time_entries_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME time_entries COMMAND time_entries_tests)

  # A genuine TWO-PROCESS proof of Phase 24.1C's task-agnostic structural
  # read -> guarded DocumentPatch -> ToolBroker -> atomic write -> Registry
  # graph. It deliberately uses an arbitrary CSV conclusions task; workflow
  # meaning belongs to callers, never this capability boundary.
  singularity_add_executable(document_actions_live_acceptance_tests
    tests/document_actions_live_acceptance.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Document/DocumentPatchTool/DocumentPatchTool.cpp
    Document/DocumentPatchValidator/DocumentPatchValidator.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(document_actions_live_acceptance_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_ROOT_DIR=\"${NATIVE_APP_ROOT}\""
  )
  if(APPLE)
    target_sources(document_actions_live_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(document_actions_live_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(document_actions_live_acceptance_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_actions_live_acceptance_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(document_actions_live_acceptance_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(document_actions_live_acceptance_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(document_actions_live_acceptance_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(document_actions_live_acceptance_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_actions_live_acceptance_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_document_restart_recovery_stage1    COMMAND document_actions_live_acceptance_tests 1)
  add_test(NAME beta1_document_restart_recovery_stage2
    COMMAND document_actions_live_acceptance_tests 2)
  set_tests_properties(beta1_document_restart_recovery_stage1 PROPERTIES
    FIXTURES_SETUP beta1_document_restart_recovery)
  set_tests_properties(beta1_document_restart_recovery_stage2 PROPERTIES
    FIXTURES_REQUIRED beta1_document_restart_recovery)

  # Two-process acceptance for DirectDocumentWork's own follow-up continuity
  # (Beta ship-path blocker C) - same real-.lm/matters.json-only cross-
  # process contract as document_actions_live_acceptance_tests
  # above, extended with DirectDocumentWork/Chat/WorkProduct so a second,
  # genuinely separate OS process can rediscover a Project, its Chat, its
  # prior MatterTask and its retained WorkProduct, and correctly resolve a
  # same-object UPDATE_EXISTING follow-up - never a real model call.
  singularity_add_executable(direct_document_work_continuity_fresh_process_acceptance_tests
    tests/direct_document_work_continuity_fresh_process_acceptance.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Chat/ChatActions/ChatActions.cpp
    Document/DirectDocumentWork/DirectDocumentWork.cpp
    Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
    Document/DocumentPatchTool/DocumentPatchTool.cpp
    Document/DocumentPatchValidator/DocumentPatchValidator.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(direct_document_work_continuity_fresh_process_acceptance_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_ROOT_DIR=\"${NATIVE_APP_ROOT}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_sources(direct_document_work_continuity_fresh_process_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(direct_document_work_continuity_fresh_process_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(direct_document_work_continuity_fresh_process_acceptance_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(direct_document_work_continuity_fresh_process_acceptance_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(direct_document_work_continuity_fresh_process_acceptance_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(APPLE)
    target_link_libraries(direct_document_work_continuity_fresh_process_acceptance_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(direct_document_work_continuity_fresh_process_acceptance_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(direct_document_work_continuity_fresh_process_acceptance_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_document_work_continuity_stage1
    COMMAND direct_document_work_continuity_fresh_process_acceptance_tests 1)
  add_test(NAME beta1_document_work_continuity_stage2
    COMMAND direct_document_work_continuity_fresh_process_acceptance_tests 2)
  set_tests_properties(beta1_document_work_continuity_stage1 PROPERTIES
    FIXTURES_SETUP beta1_document_work_continuity)
  set_tests_properties(beta1_document_work_continuity_stage2 PROPERTIES
    FIXTURES_REQUIRED beta1_document_work_continuity)

  singularity_add_executable(document_patch_validator_tests
    Document/DocumentPatchValidator/DocumentPatchValidator.test.cpp
    Document/DocumentPatchValidator/DocumentPatchValidator.cpp
  )
  target_include_directories(document_patch_validator_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_patch_validator_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
  target_include_directories(document_patch_validator_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
	"$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(document_patch_validator_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_patch_validator_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_patch_validator COMMAND document_patch_validator_tests)

  # The structural CSV/PPTX/XLSX readers and the format-neutral structural
  # dispatcher - lightweight, no Matter/Persistence dependency.
  singularity_add_executable(csv_extractor_tests
    Document/CsvExtractor/CsvExtractor.test.cpp
    Document/CsvExtractor/CsvExtractor.cpp
  )
  target_include_directories(csv_extractor_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(csv_extractor_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
  target_include_directories(csv_extractor_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
	"$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(csv_extractor_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(csv_extractor_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME csv_extractor COMMAND csv_extractor_tests)

  singularity_add_executable(pptx_extractor_tests
    Document/PptxExtractor/PptxExtractor.test.cpp
    Document/PptxExtractor/PptxExtractor.cpp
  )
  target_include_directories(pptx_extractor_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(pptx_extractor_tests PRIVATE libzip::zip pugixml::pugixml utf8proc)
  target_include_directories(pptx_extractor_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
	"$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(pptx_extractor_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(pptx_extractor_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME pptx_extractor COMMAND pptx_extractor_tests)

  singularity_add_executable(xlsx_extractor_tests
    Document/XlsxExtractor/XlsxExtractor.test.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/Policy/Policy.cpp
  )
  target_include_directories(xlsx_extractor_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(xlsx_extractor_tests PRIVATE libzip::zip pugixml::pugixml utf8proc)
  target_include_directories(xlsx_extractor_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
	"$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(xlsx_extractor_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(xlsx_extractor_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME xlsx_extractor COMMAND xlsx_extractor_tests)

  singularity_add_executable(legal_machines_connection_tests
    Node/LegalMachinesConnection/LegalMachinesConnection.test.cpp
    Node/LegalMachinesConnection/LegalMachinesConnection.cpp
  )
  target_include_directories(legal_machines_connection_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(legal_machines_connection_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(legal_machines_connection_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME legal_machines_connection COMMAND legal_machines_connection_tests)

  # Singularity's new versions from Legal Machines' site (Node/AppUpdate): the
  # release Legal Machines signed, the order of versions, and the installer
  # checked byte for byte - over an injected transport, with a test key.
  singularity_add_executable(app_update_tests
    Node/AppUpdate/AppUpdate.test.cpp
    Node/AppUpdate/AppUpdate.cpp
  )
  target_include_directories(app_update_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(app_update_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
  target_compile_definitions(app_update_tests PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  if(MSVC)
    target_compile_options(app_update_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(app_update_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME app_update COMMAND app_update_tests)

  # Singularity installing its own new versions (Platform/UpdateInstall,
  # D-261): what the last install did, said once from one line any system's
  # helper writes; and a test program, never an installed Singularity, which
  # installs by hand and changes nothing.
  set(SINGULARITY_UPDATE_INSTALL_TEST_SOURCES
    Platform/UpdateInstall/UpdateInstall.test.cpp
    Platform/UpdateInstall/UpdateInstall.cpp
    Platform/Sha256/Sha256.cpp
  )
  if(APPLE)
    list(APPEND SINGULARITY_UPDATE_INSTALL_TEST_SOURCES Platform/UpdateInstall/UpdateInstallMacOS.mm)
  elseif(WIN32)
    list(APPEND SINGULARITY_UPDATE_INSTALL_TEST_SOURCES
      Platform/UpdateInstall/UpdateInstallWindows.cpp
      Platform/UpdateHelper/UpdateHelper.cpp)
  else()
    list(APPEND SINGULARITY_UPDATE_INSTALL_TEST_SOURCES Platform/UpdateInstall/UpdateInstallLinux.cpp)
  endif()
  singularity_add_executable(update_install_tests ${SINGULARITY_UPDATE_INSTALL_TEST_SOURCES})
  target_include_directories(update_install_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(APPLE)
    target_link_libraries(update_install_tests PRIVATE "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(update_install_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(update_install_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME update_install COMMAND update_install_tests)

  # Singularity's updater on Windows (Platform/UpdateHelper): the request it
  # reads, its SHA-256 comparison and the member's sentences, on every system;
  # on Windows the command line split back by Windows itself.
  singularity_add_executable(update_helper_tests
    Platform/UpdateHelper/UpdateHelper.test.cpp
    Platform/UpdateHelper/UpdateHelper.cpp
  )
  target_include_directories(update_helper_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(WIN32)
    target_link_libraries(update_helper_tests PRIVATE shell32)
  endif()
  if(MSVC)
    target_compile_options(update_helper_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(update_helper_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME update_helper COMMAND update_helper_tests)

  # Signing in to Legal Machines on this computer, through the account bridge,
  # over an injected transport: no network, no credential store, no download.
  # Settings live in a directory of the test's own (SINGULARITY_ISOLATED_TEST_SETTINGS).
  singularity_add_executable(account_bridge_tests
    Bridge/AccountBridge/AccountBridge.test.cpp
    Bridge/AccountBridge/AccountBridge.cpp
    Organisation/FirmRole/FirmRole.cpp
    Node/LegalMachinesAccount/LegalMachinesAccount.cpp
    Node/OpenPgpMessage/OpenPgpMessage.cpp
    Node/WorkLog/WorkLog.cpp
    Node/WorkLogShare/WorkLogShare.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_include_directories(account_bridge_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(account_bridge_tests PRIVATE nlohmann_json::nlohmann_json libzip::zip lm_mbedtls monocypher)
  target_compile_definitions(account_bridge_tests PRIVATE
    SINGULARITY_ISOLATED_TEST_SETTINGS=1
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  if(MSVC)
    target_compile_options(account_bridge_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(account_bridge_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME account_bridge COMMAND account_bridge_tests)

  # A seat's password kept at the member's choice (D-243, Platform/
  # SeatPasswordStore): on Windows the user's Credential Manager itself, under a
  # test entry of Singularity's own prefix that the test removes again; on macOS
  # and Linux, that nothing is kept.
  if(WIN32)
    singularity_add_executable(seat_password_store_tests
      Platform/SeatPasswordStore/SeatPasswordStore.test.cpp
      Platform/SeatPasswordStore/SeatPasswordStoreWindows.cpp
    )
    target_link_libraries(seat_password_store_tests PRIVATE advapi32)
  else()
    singularity_add_executable(seat_password_store_tests
      Platform/SeatPasswordStore/SeatPasswordStore.test.cpp
      Platform/SeatPasswordStore/SeatPasswordStoreNone.cpp
    )
  endif()
  target_include_directories(seat_password_store_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(seat_password_store_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(seat_password_store_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME seat_password_store COMMAND seat_password_store_tests)

  # What Windows Firewall's rules for Singularity mean for the firm's computers
  # reaching this one (D-244, Platform/SystemPermissions/FirewallState.hpp):
  # the decision alone, the same on every system; Windows reads the rules.
  singularity_add_executable(firewall_state_tests
    Platform/SystemPermissions/FirewallState.test.cpp
  )
  target_include_directories(firewall_state_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(firewall_state_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(firewall_state_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME firewall_state COMMAND firewall_state_tests)

  # Signing in and the model download session over an injected transport: the
  # password and the session token never reach an error, a trace or settings.
  singularity_add_executable(legal_machines_account_tests
    Node/LegalMachinesAccount/LegalMachinesAccount.test.cpp
    Node/LegalMachinesAccount/LegalMachinesAccount.cpp
  )
  target_include_directories(legal_machines_account_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(legal_machines_account_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(legal_machines_account_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(legal_machines_account_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME legal_machines_account COMMAND legal_machines_account_tests)

  # An OpenPGP message sealed on this computer to Legal Machines' keys (D-257):
  # read from a key openpgp.js made, and opened again with that key's secret.
  singularity_add_executable(openpgp_message_tests
    Node/OpenPgpMessage/OpenPgpMessage.test.cpp
    Node/OpenPgpMessage/OpenPgpMessage.cpp
  )
  target_include_directories(openpgp_message_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(openpgp_message_tests PRIVATE lm_mbedtls monocypher)
  if(MSVC)
    target_compile_options(openpgp_message_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(openpgp_message_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME openpgp_message COMMAND openpgp_message_tests)
  set_tests_properties(openpgp_message PROPERTIES
    ENVIRONMENT "SINGULARITY_OPENPGP_SAMPLE=${CMAKE_CURRENT_BINARY_DIR}/openpgp-sample.asc"
    FIXTURES_SETUP openpgp_sample)
  # A design partner's work log of its model calls (D-257): kept only where
  # its folder is named and the record is on, each chat apart, for 14 days,
  # and erased when it is turned off.
  singularity_add_executable(work_log_tests
    Node/WorkLog/WorkLog.test.cpp
    Node/WorkLog/WorkLog.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(work_log_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(work_log_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(work_log_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(work_log_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME work_log COMMAND work_log_tests)

  # A design partner's work log sent (D-257): packed as a zip, signed, put in
  # a letter Seal reads, sealed to Legal Machines' pinned keys and posted to
  # the mail server's intake; never while a pinned key is missing.
  singularity_add_executable(work_log_share_tests
    Node/WorkLogShare/WorkLogShare.test.cpp
    Node/WorkLogShare/WorkLogShare.cpp
    Node/WorkLog/WorkLog.cpp
    Node/OpenPgpMessage/OpenPgpMessage.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(work_log_share_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(work_log_share_tests PRIVATE nlohmann_json::nlohmann_json libzip::zip lm_mbedtls monocypher)
  if(MSVC)
    target_compile_options(work_log_share_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(work_log_share_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME work_log_share COMMAND work_log_share_tests)

  # Its interop proof: openpgp.js, the version Seal opens it with (a
  # devDependency of deploy/frontend), and GnuPG where it is installed, open
  # the sample the test above sealed. Skipped where Node or the frontend's
  # packages are not installed.
  find_program(SINGULARITY_NODE_PROGRAM node)
  if(SINGULARITY_NODE_PROGRAM)
    add_test(NAME openpgp_interop
      COMMAND "${SINGULARITY_NODE_PROGRAM}" "${NATIVE_APP_SOURCE_DIR}/Node/OpenPgpMessage/OpenPgpMessage.interop.mjs"
              "${CMAKE_CURRENT_BINARY_DIR}/openpgp-sample.asc"
              "${CMAKE_CURRENT_SOURCE_DIR}/../frontend/node_modules/openpgp/dist/node/openpgp.mjs")
    set_tests_properties(openpgp_interop PROPERTIES FIXTURES_REQUIRED openpgp_sample SKIP_RETURN_CODE 77)
  endif()

  # The developer model source, compiled as a Debug build has it and as every
  # other build has it (none at all), from the same test source.
  singularity_add_executable(developer_model_source_tests
    LocalModel/DeveloperModelSource/DeveloperModelSource.test.cpp
    LocalModel/DeveloperModelSource/DeveloperModelSource.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(developer_model_source_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_compile_definitions(developer_model_source_tests PRIVATE SINGULARITY_DEVELOPER_MODEL_SOURCE=1)
  singularity_add_executable(developer_model_source_tests_release
    LocalModel/DeveloperModelSource/DeveloperModelSource.test.cpp
    LocalModel/DeveloperModelSource/DeveloperModelSource.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(developer_model_source_tests_release PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  foreach(developer_model_source_target developer_model_source_tests developer_model_source_tests_release)
    if(MSVC)
      target_compile_options(${developer_model_source_target} PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(${developer_model_source_target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endforeach()
  add_test(NAME developer_model_source COMMAND developer_model_source_tests)
  add_test(NAME developer_model_source_release COMMAND developer_model_source_tests_release)

  # The recovery Papinian Guard's model offers while it is missing
  # (LocalModel/ModelPackageManager papinian_recovery_for).
  singularity_add_executable(model_package_recovery_tests
    LocalModel/ModelPackageManager/ModelPackageManager.test.cpp
  )
  target_include_directories(model_package_recovery_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(model_package_recovery_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(model_package_recovery_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME model_package_recovery COMMAND model_package_recovery_tests)

  singularity_add_executable(xlsx_number_format_tests
    Document/XlsxNumberFormat/XlsxNumberFormat.test.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
  )
  target_include_directories(xlsx_number_format_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(xlsx_number_format_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(xlsx_number_format_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME xlsx_number_format COMMAND xlsx_number_format_tests)

  singularity_add_executable(xlsx_workbook_view_tests
    Document/XlsxWorkbookView/XlsxWorkbookView.test.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
  )
  target_include_directories(xlsx_workbook_view_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(xlsx_workbook_view_tests PRIVATE nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml)
  target_include_directories(xlsx_workbook_view_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(xlsx_workbook_view_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(xlsx_workbook_view_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME xlsx_workbook_view COMMAND xlsx_workbook_view_tests)

  singularity_add_executable(xlsx_row_insertion_tests
    Document/XlsxRowInsertion/XlsxRowInsertion.test.cpp
    Document/XlsxRowInsertion/XlsxRowInsertion.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
  )
  target_include_directories(xlsx_row_insertion_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(xlsx_row_insertion_tests PRIVATE nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml)
  target_include_directories(xlsx_row_insertion_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(xlsx_row_insertion_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(xlsx_row_insertion_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME xlsx_row_insertion COMMAND xlsx_row_insertion_tests)

  singularity_add_executable(document_structural_reader_tests
    Document/DocumentStructuralReader/DocumentStructuralReader.test.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
  )
  if(APPLE)
    target_sources(document_structural_reader_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
    )
    target_link_libraries(document_structural_reader_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  else()
    target_sources(document_structural_reader_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
    )
  endif()
  target_include_directories(document_structural_reader_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(document_structural_reader_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(document_structural_reader_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(document_structural_reader_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_structural_reader_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_structural_reader COMMAND document_structural_reader_tests)

  # Permission-gating for document.patch.propose/apply - the real Matter/
  # Project round trip is proven by the connected acceptance test above.
  singularity_add_executable(document_patch_tool_tests
    Document/DocumentPatchTool/DocumentPatchTool.test.cpp
    Document/DocumentPatchTool/DocumentPatchTool.cpp
    Document/DocumentPatchValidator/DocumentPatchValidator.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(document_patch_tool_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(document_patch_tool_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(document_patch_tool_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(document_patch_tool_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(document_patch_tool_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(document_patch_tool_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(document_patch_tool_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(document_patch_tool_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(document_patch_tool_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_patch_tool_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_patch_tool COMMAND document_patch_tool_tests)

	if(APPLE)
		singularity_add_executable(work_product_export_acceptance EXCLUDE_FROM_ALL
			tests/work_product_export_acceptance.cpp
			WorkProduct/WorkProductExport/WorkProductExport.cpp
			Matter/MatterRepository/MatterRepository.cpp
			Context/TemporalContext/TemporalContext.cpp
			Context/TemporalSemantics/TemporalSemantics.cpp
			Document/DocumentRegistry/DocumentRegistry.cpp
			Persistence/ProjectStore/ProjectStore.cpp
			Platform/ActorIdentity/ActorIdentity.cpp
			Platform/AppPaths/AppPaths.cpp
			Platform/AppSettings/AppSettings.cpp
			Platform/NativeDialogs/NativeDialogs.cpp
			Platform/OpenExternal/OpenExternal.cpp
			Platform/Sha256/Sha256.cpp
			PapinianGuard/Policy/Policy.cpp
			PapinianGuard/Taxonomy/Taxonomy.cpp
			Privacy/EntityRegistry/EntityRegistry.cpp
			Context/ContextRegistry/ContextRegistry.cpp
			Provider/ModelClassification/ModelClassification.cpp
			WorkProduct/ChronologyParser/ChronologyParser.cpp
			WorkProduct/ComparisonParser/ComparisonParser.cpp
			Context/ContextRetriever/ContextRetriever.cpp
			WorkProduct/FindingParser/FindingParser.cpp
			WorkProduct/ObligationParser/ObligationParser.cpp
			Context/SourceMarkers/SourceMarkers.cpp
			Context/QuoteVerification/QuoteVerification.cpp
			Document/DocumentCache/DocumentCache.cpp
			Document/DocumentChunker/DocumentChunker.cpp
			Document/DocumentExtractor/DocumentExtractor.cpp
			Document/CsvExtractor/CsvExtractor.cpp
			Document/PptxExtractor/PptxExtractor.cpp
			Document/XlsxExtractor/XlsxExtractor.cpp
			Document/XlsxWorkbookView/XlsxWorkbookView.cpp
			Document/XlsxNumberFormat/XlsxNumberFormat.cpp
			Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
			Document/PdfExtractor/PdfExtractorMacOS.mm
			Node/ActiveWorkspace/ActiveWorkspace.cpp
			Node/LegacyWorkspace/LegacyWorkspace.cpp
		)
		target_include_directories(work_product_export_acceptance PRIVATE "${NATIVE_APP_SOURCE_DIR}")
		target_link_libraries(work_product_export_acceptance PRIVATE
			nfd nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
			"-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
		target_compile_definitions(work_product_export_acceptance PRIVATE
			"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
			"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
		target_compile_options(work_product_export_acceptance PRIVATE -Wall -Wextra -Wpedantic)

		singularity_add_executable(work_product_acceptance_setup EXCLUDE_FROM_ALL
			tests/work_product_acceptance_setup.cpp
			Matter/MatterActions/MatterActions.cpp
			Matter/MatterRepository/MatterRepository.cpp
			Context/TemporalContext/TemporalContext.cpp
			Context/TemporalSemantics/TemporalSemantics.cpp
			Document/DocumentRegistry/DocumentRegistry.cpp
			Persistence/ProjectStore/ProjectStore.cpp
			Platform/ActorIdentity/ActorIdentity.cpp
			Platform/AppPaths/AppPaths.cpp
			Platform/AppSettings/AppSettings.cpp
			Platform/OpenExternal/OpenExternal.cpp
			Platform/PermissionAccess/PermissionAccess.cpp
			Platform/Sha256/Sha256.cpp
			PapinianGuard/Policy/Policy.cpp
			PapinianGuard/Taxonomy/Taxonomy.cpp
			Privacy/EntityRegistry/EntityRegistry.cpp
			Context/ContextRegistry/ContextRegistry.cpp
			Provider/ModelClassification/ModelClassification.cpp
			Routing/AiSetup/AiSetup.cpp
			WorkProduct/ChronologyParser/ChronologyParser.cpp
			WorkProduct/ComparisonParser/ComparisonParser.cpp
			Context/ContextRetriever/ContextRetriever.cpp
			WorkProduct/FindingParser/FindingParser.cpp
			WorkProduct/ObligationParser/ObligationParser.cpp
			Context/SourceMarkers/SourceMarkers.cpp
			Context/QuoteVerification/QuoteVerification.cpp
			Document/DocumentCache/DocumentCache.cpp
			Document/DocumentChunker/DocumentChunker.cpp
			Document/DocumentExtractor/DocumentExtractor.cpp
			Document/CsvExtractor/CsvExtractor.cpp
			Document/PptxExtractor/PptxExtractor.cpp
			Document/XlsxExtractor/XlsxExtractor.cpp
			Document/XlsxWorkbookView/XlsxWorkbookView.cpp
			Document/XlsxNumberFormat/XlsxNumberFormat.cpp
			Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
			Document/PdfExtractor/PdfExtractorMacOS.mm
			Node/ActiveWorkspace/ActiveWorkspace.cpp
			Node/LegacyWorkspace/LegacyWorkspace.cpp
			Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm
		)
		target_include_directories(work_product_acceptance_setup PRIVATE "${NATIVE_APP_SOURCE_DIR}")
		target_link_libraries(work_product_acceptance_setup PRIVATE
			nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
			"-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
		target_compile_definitions(work_product_acceptance_setup PRIVATE
			"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
			"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
		target_compile_options(work_product_acceptance_setup PRIVATE -Wall -Wextra -Wpedantic)
	endif()

	# The Native API checked against the code that serves it (Beta 1 #269,
	# docs/API_CONTRACT.md API-2): every contract method has a real
	# register_handler, its params are what its handler reads, with the same
	# optionality, and its events are the ones the backend publishes. The
	# check and its own tests are plain Python and run on every platform.
	find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
	if(Python3_Interpreter_FOUND)
		add_test(NAME native_contract_check
			COMMAND "${Python3_EXECUTABLE}" "${NATIVE_APP_ROOT}/deploy/scripts/generate_native_contract.py" --check)
		set_tests_properties(native_contract_check PROPERTIES LABELS "unit;bridge")
		add_test(NAME native_contract_check_tests
			COMMAND "${Python3_EXECUTABLE}" "${NATIVE_APP_ROOT}/deploy/scripts/test_native_contract.py")
		set_tests_properties(native_contract_check_tests PROPERTIES LABELS "unit;bridge")
	else()
		message(STATUS "Python 3.9 or later not found: the Native API contract check is not registered with ctest")
	endif()

	# The two steps of the contract parity acceptance that have no
	# --native-call: a workspace and a Project's folder, set up the way the
	# product sets them up (code/tests/native_contract_parity_seed.cpp).
	singularity_add_executable(native_contract_parity_seed
		tests/native_contract_parity_seed.cpp
		Matter/MatterActions/MatterActions.cpp
		Matter/MatterRepository/MatterRepository.cpp
		Context/TemporalContext/TemporalContext.cpp
		Context/TemporalSemantics/TemporalSemantics.cpp
		Document/DocumentRegistry/DocumentRegistry.cpp
		Persistence/ProjectStore/ProjectStore.cpp
		Platform/ActorIdentity/ActorIdentity.cpp
		Platform/AppPaths/AppPaths.cpp
		Platform/AppSettings/AppSettings.cpp
		Platform/OpenExternal/OpenExternal.cpp
		Platform/PermissionAccess/PermissionAccess.cpp
		Platform/Sha256/Sha256.cpp
		PapinianGuard/Policy/Policy.cpp
		PapinianGuard/Taxonomy/Taxonomy.cpp
		Privacy/EntityRegistry/EntityRegistry.cpp
		Context/ContextRegistry/ContextRegistry.cpp
		Provider/ModelClassification/ModelClassification.cpp
		Routing/AiSetup/AiSetup.cpp
		WorkProduct/ChronologyParser/ChronologyParser.cpp
		WorkProduct/ComparisonParser/ComparisonParser.cpp
		Context/ContextRetriever/ContextRetriever.cpp
		WorkProduct/FindingParser/FindingParser.cpp
		WorkProduct/ObligationParser/ObligationParser.cpp
		Context/SourceMarkers/SourceMarkers.cpp
		Context/QuoteVerification/QuoteVerification.cpp
		Document/DocumentCache/DocumentCache.cpp
		Document/DocumentChunker/DocumentChunker.cpp
		Document/DocumentExtractor/DocumentExtractor.cpp
		Document/CsvExtractor/CsvExtractor.cpp
		Document/PptxExtractor/PptxExtractor.cpp
		Document/XlsxExtractor/XlsxExtractor.cpp
		Document/XlsxWorkbookView/XlsxWorkbookView.cpp
		Document/XlsxNumberFormat/XlsxNumberFormat.cpp
		Document/DocxExtractor/DocxExtractor.cpp
		Document/OfficeTextEdit/OfficeTextEdit.cpp
		Node/ActiveWorkspace/ActiveWorkspace.cpp
		Node/LegacyWorkspace/LegacyWorkspace.cpp
	)
	if(APPLE)
		target_sources(native_contract_parity_seed PRIVATE
			"${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
			"${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm")
		target_link_libraries(native_contract_parity_seed PRIVATE
			"-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
	else()
		target_sources(native_contract_parity_seed PRIVATE
			"${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
			"${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}")
	endif()
	target_include_directories(native_contract_parity_seed PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(native_contract_parity_seed PRIVATE
		nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
	target_compile_definitions(native_contract_parity_seed PRIVATE
		"NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
		"NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
	if(MSVC)
		target_compile_options(native_contract_parity_seed PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(native_contract_parity_seed PRIVATE -Wall -Wextra -Wpedantic)
	endif()

	# The Native API's Worker surface called on the real application, one
	# separate process per call, each request and answer checked against the
	# generated contract (deploy/scripts/native_contract_parity_acceptance.py).
	# It needs --native-call, which a Debug build has and a release build
	# refuses (D-025): against a release build the test says so and is
	# skipped. A bridge method runs beside the application's window, which on
	# Linux needs a display: the test runs under xvfb-run where it is
	# installed, and without a display it says so and is skipped. Windows has
	# no --native-call at all, since its application opens its window as it
	# starts, so the test is not registered there.
	if(Python3_Interpreter_FOUND AND NOT WIN32)
		set(singularity_parity_display)
		if(NOT APPLE)
			find_program(SINGULARITY_XVFB_RUN xvfb-run)
			if(SINGULARITY_XVFB_RUN)
				set(singularity_parity_display "${SINGULARITY_XVFB_RUN}" -a)
			endif()
		endif()
		add_test(NAME native_contract_parity
			COMMAND ${singularity_parity_display} "${Python3_EXECUTABLE}"
				"${NATIVE_APP_ROOT}/deploy/scripts/native_contract_parity_acceptance.py"
				--binary "$<TARGET_FILE:native_app>" --seed-tool "$<TARGET_FILE:native_contract_parity_seed>")
		set_tests_properties(native_contract_parity PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 LABELS "acceptance;bridge")
	endif()

  singularity_add_executable(document_file_actions_tests
    Document/DocumentFileActions/DocumentFileActions.test.cpp
    Document/DocumentFileActions/DocumentFileActions.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(document_file_actions_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(document_file_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileMacOS.mm"
    )
  elseif(WIN32)
    target_sources(document_file_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileWindows.cpp"
    )
    target_link_libraries(document_file_actions_tests PRIVATE ole32 shell32)
  else()
    target_sources(document_file_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
      "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFileLinux.cpp"
    )
  endif()
  target_include_directories(document_file_actions_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_file_actions_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(document_file_actions_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(document_file_actions_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(document_file_actions_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(document_file_actions_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_file_actions_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_file_actions COMMAND document_file_actions_tests)

  # What Linux's own folder watcher (inotify) reports, held to what
  # DocumentRegistry and MatterActions rely on from the macOS and Windows
  # watchers.
  if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    singularity_add_executable(file_system_watcher_linux_tests
      Platform/FileSystemWatcher/FileSystemWatcherLinux.test.cpp
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
    target_include_directories(file_system_watcher_linux_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(file_system_watcher_linux_tests PRIVATE Threads::Threads)
    target_compile_options(file_system_watcher_linux_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME file_system_watcher_reports COMMAND file_system_watcher_linux_tests)
  endif()

  # A background Singularity keeps its pace (Platform/BackgroundActivity):
  # macOS's App Nap off, Windows' power throttling off, nothing on Linux.
  if(APPLE)
    singularity_add_executable(background_activity_tests
      Platform/BackgroundActivity/BackgroundActivity.test.cpp
      Platform/BackgroundActivity/BackgroundActivityMacOS.mm
    )
    target_link_libraries(background_activity_tests PRIVATE "-framework Foundation")
  elseif(WIN32)
    singularity_add_executable(background_activity_tests
      Platform/BackgroundActivity/BackgroundActivity.test.cpp
      Platform/BackgroundActivity/BackgroundActivityWindows.cpp
    )
  else()
    singularity_add_executable(background_activity_tests
      Platform/BackgroundActivity/BackgroundActivity.test.cpp
      Platform/BackgroundActivity/BackgroundActivityLinux.cpp
    )
  endif()
  target_include_directories(background_activity_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  add_test(NAME background_activity COMMAND background_activity_tests)

  # The firm's computers pass sealed envelopes to each other directly, never
  # through Legal Machines (Matter/CoordinationTransport, D-124): the firm's
  # own folder, and the Noise IK handshake checked against the protocol's
  # published test vectors.
  singularity_add_executable(folder_transport_tests
    Matter/CoordinationTransport/FolderTransport.test.cpp
    Matter/CoordinationTransport/FolderTransport.cpp
    Matter/CoordinationTransport/CoordinationTransport.cpp
  )
  target_include_directories(folder_transport_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(folder_transport_tests PRIVATE monocypher nlohmann_json::nlohmann_json)
  add_test(NAME folder_transport COMMAND folder_transport_tests)
  singularity_add_executable(noise_ik_tests
    Matter/CoordinationTransport/NoiseIK.test.cpp
    Matter/CoordinationTransport/NoiseIK.cpp
  )
  target_include_directories(noise_ik_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(noise_ik_tests PRIVATE monocypher)
  add_test(NAME noise_ik COMMAND noise_ik_tests)
  # Whole messages over a path that loses, repeats and reorders datagrams.
  singularity_add_executable(reliable_delivery_tests
    Matter/CoordinationTransport/ReliableDelivery.test.cpp
    Matter/CoordinationTransport/ReliableDelivery.cpp
  )
  target_include_directories(reliable_delivery_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  add_test(NAME reliable_delivery COMMAND reliable_delivery_tests)
  # The UDP socket the direct link runs on (Platform/UdpSocket, shared ground,
  # agreed): Winsock on Windows, BSD sockets on macOS and Linux.
  if(WIN32)
    singularity_add_executable(udp_socket_tests
      Platform/UdpSocket/UdpSocket.test.cpp
      Platform/UdpSocket/UdpSocketWindows.cpp
    )
    target_link_libraries(udp_socket_tests PRIVATE ws2_32 iphlpapi)
  else()
    singularity_add_executable(udp_socket_tests
      Platform/UdpSocket/UdpSocket.test.cpp
      Platform/UdpSocket/UdpSocketPosix.cpp
    )
  endif()
  target_include_directories(udp_socket_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  add_test(NAME udp_socket COMMAND udp_socket_tests)
  # Two computers linked directly over UDP, as WireGuard links them: the Noise
  # IK handshake, sealed datagrams, whole messages over a lossy path, renewed
  # keys, a restart, and a computer in the middle that reads nothing.
  if(WIN32)
    set(direct_link_socket Platform/UdpSocket/UdpSocketWindows.cpp)
  else()
    set(direct_link_socket Platform/UdpSocket/UdpSocketPosix.cpp)
  endif()
  singularity_add_executable(direct_link_tests
    Matter/CoordinationTransport/DirectLink.test.cpp
    Matter/CoordinationTransport/DirectLink.cpp
    Matter/CoordinationTransport/NoiseIK.cpp
    Matter/CoordinationTransport/ReliableDelivery.cpp
    Matter/CoordinationTransport/Stun.cpp
    ${direct_link_socket}
  )
  if(WIN32)
    target_link_libraries(direct_link_tests PRIVATE ws2_32 iphlpapi)
  endif()
  target_include_directories(direct_link_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(direct_link_tests PRIVATE monocypher Threads::Threads)
  add_test(NAME direct_link COMMAND direct_link_tests)
  # The engine's envelopes over the direct link: only for the Matters whose
  # address cards named the computer that sent them.
  singularity_add_executable(direct_transport_tests
    Matter/CoordinationTransport/DirectTransport.test.cpp
    Matter/CoordinationTransport/DirectTransport.cpp
    Matter/CoordinationTransport/CoordinationTransport.cpp
    Matter/CoordinationTransport/DirectLink.cpp
    Matter/CoordinationTransport/NoiseIK.cpp
    Matter/CoordinationTransport/ReliableDelivery.cpp
    Matter/CoordinationTransport/Stun.cpp
    Matter/CoordinationSeal/CoordinationSeal.cpp
    ${direct_link_socket}
  )
  target_include_directories(direct_transport_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(direct_transport_tests PRIVATE monocypher nlohmann_json::nlohmann_json Threads::Threads)
  add_test(NAME direct_transport COMMAND direct_transport_tests)
  # This computer's Trash of Matters (Matter/MatterTrash, D-247).
  singularity_add_executable(matter_trash_tests
    Matter/MatterTrash/MatterTrash.test.cpp
    Matter/MatterTrash/MatterTrash.cpp
  )
  target_include_directories(matter_trash_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(matter_trash_tests PRIVATE nlohmann_json::nlohmann_json)
  add_test(NAME matter_trash COMMAND matter_trash_tests)
  # A personal conversation cleared by the member here (Chat/ConversationClearing).
  singularity_add_executable(conversation_clearing_tests
    Chat/ConversationClearing/ConversationClearing.test.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
  )
  target_include_directories(conversation_clearing_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(conversation_clearing_tests PRIVATE nlohmann_json::nlohmann_json)
  add_test(NAME conversation_clearing COMMAND conversation_clearing_tests)
  # The relay's WebSocket frames (Platform/WebSocket, D-242), as the Linux
  # client speaks them: RFC 6455's samples, a message in pieces, a broken frame.
  singularity_add_executable(websocket_frames_tests
    Platform/WebSocket/WebSocketFrames.test.cpp
    Platform/WebSocket/WebSocketFrames.cpp
  )
  target_include_directories(websocket_frames_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  add_test(NAME websocket_frames COMMAND websocket_frames_tests)
  # How a computer behind a router learns the address it shows outside: STUN's
  # Binding request and response (RFC 8489), checked against RFC 5769's samples.
  singularity_add_executable(stun_tests
    Matter/CoordinationTransport/Stun.test.cpp
    Matter/CoordinationTransport/Stun.cpp
  )
  target_include_directories(stun_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  add_test(NAME stun COMMAND stun_tests)

  # Linux's Trash: the freedesktop.org Trash specification, which its file
  # managers read.
  if(UNIX AND NOT APPLE)
    singularity_add_executable(trash_file_tests
      Platform/TrashFile/TrashFile.test.cpp
      Platform/TrashFile/TrashFileLinux.cpp
    )
    target_include_directories(trash_file_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_compile_options(trash_file_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME trash_file COMMAND trash_file_tests)
  endif()

  # The Windows download path against a server of the test's own on 127.0.0.1
  # (Platform/HttpClient, Beta 1 #175): a whole package of the protection
  # model's size, a body cut short, a refusal, a Stop and a stall at the
  # product's own limit. Loopback only; about a minute, most of it the stall.
  if(WIN32)
    singularity_add_executable(http_download_tests
      Platform/HttpClient/HttpClientWindows.test.cpp
      Platform/HttpClient/HttpClientWindows.cpp
    )
    target_include_directories(http_download_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(http_download_tests PRIVATE winhttp ws2_32 psapi)
    if(MSVC)
      target_compile_options(http_download_tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME http_download COMMAND http_download_tests)
  endif()

  # Where WebView2 keeps the window's data on Windows (Platform/WebViewData,
  # D-040).
  if(WIN32)
    singularity_add_executable(web_view_data_tests
      Platform/WebViewData/WebViewDataWindows.test.cpp
      Platform/WebViewData/WebViewDataWindows.cpp
      Platform/AppPaths/AppPaths.cpp
    )
    target_include_directories(web_view_data_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_include_directories(web_view_data_tests SYSTEM PRIVATE
      "$<TARGET_PROPERTY:webview::core,INTERFACE_INCLUDE_DIRECTORIES>")
    target_link_libraries(web_view_data_tests PRIVATE webview::core shell32)
    if(MSVC)
      target_compile_options(web_view_data_tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME web_view_data COMMAND web_view_data_tests)
  endif()

  singularity_add_executable(document_source_resolver_tests
    Document/DocumentSourceResolver/DocumentSourceResolver.test.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(document_source_resolver_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(document_source_resolver_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(document_source_resolver_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(document_source_resolver_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_source_resolver_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(document_source_resolver_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(document_source_resolver_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(document_source_resolver_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(document_source_resolver_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_source_resolver_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_source_resolver COMMAND document_source_resolver_tests)

  singularity_add_executable(chat_actions_tests
    Chat/ChatActions/ChatActions.test.cpp
    Chat/ChatActions/ChatActions.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(chat_actions_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(chat_actions_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_actions_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_actions_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_actions_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_actions_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_actions_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_actions_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_chat_actions COMMAND chat_actions_tests)

  # Phase 22's narrow pre-execution resolution boundary (Chat/
  # ChatOperationSetup/ChatOperationSetup.hpp) - real Project/Chat
  # persistence, Node -> Project -> Chat inheritance and legal task preset
  # resolution, exercised through the same production paths chat_actions_
  # tests already uses. Shares its dependency set exactly - the live model/
  # provider registry check ChatBridge.cpp's resolve_operation_setup()
  # performs before calling into this domain function is supplied as an
  # inert link seam here too (see ChatActions.test.cpp's own comment) so
  # this stays independent of the local-model runtime.
  singularity_add_executable(chat_operation_setup_tests
    Chat/ChatOperationSetup/ChatOperationSetup.test.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(chat_operation_setup_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_operation_setup_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_operation_setup_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(chat_operation_setup_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_operation_setup_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_operation_setup_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_operation_setup_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_operation_setup_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_operation_setup_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_operation_setup_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME chat_operation_setup COMMAND chat_operation_setup_tests)

  # A genuine TWO-PROCESS fresh-reload proof that a Chat's own AI Setup
  # override survives an actual new OS process
  # (Chat/ChatOperationSetup/ChatOperationSetupReloadAcceptance.cpp) - reuses
  # chat_source_resolver_reload_acceptance_tests' own two-stage CTest
  # FIXTURES pattern rather than inventing a second one. Shares
  # chat_operation_setup_tests' own dependency set.
  singularity_add_executable(chat_operation_setup_reload_acceptance_tests
    Chat/ChatOperationSetup/ChatOperationSetupReloadAcceptance.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(chat_operation_setup_reload_acceptance_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_operation_setup_reload_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_operation_setup_reload_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(chat_operation_setup_reload_acceptance_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_operation_setup_reload_acceptance_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_operation_setup_reload_acceptance_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_operation_setup_reload_acceptance_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_operation_setup_reload_acceptance_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_operation_setup_reload_acceptance_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_operation_setup_reload_acceptance_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_chat_operation_setup_reload_stage1
    COMMAND chat_operation_setup_reload_acceptance_tests 1)
  add_test(NAME beta1_chat_operation_setup_reload_stage2
    COMMAND chat_operation_setup_reload_acceptance_tests 2)
  set_tests_properties(beta1_chat_operation_setup_reload_stage1 PROPERTIES
    FIXTURES_SETUP beta1_chat_operation_setup_reload)
  set_tests_properties(beta1_chat_operation_setup_reload_stage2 PROPERTIES
    FIXTURES_REQUIRED beta1_chat_operation_setup_reload)

  # Phase 21's real backend-owned Chat composition/source-policy resolution
  # (Chat/ChatSourceResolver/ChatSourceResolver.hpp) - Relevant Project
  # sources and the strict Selected documents boundary, both built from the
  # existing Project Documents pool (matter::list_documents()) and the
  # existing ContextRetriever, never a second retrieval system. Shares
  # chat_actions_tests' own dependency set: this is the same Document/
  # ContextRetriever machinery, exercised through real on-disk documents.
  singularity_add_executable(chat_source_resolver_tests
    Chat/ChatSourceResolver/ChatSourceResolver.test.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatActions/ChatActions.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(chat_source_resolver_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_source_resolver_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_source_resolver_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(chat_source_resolver_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_source_resolver_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_source_resolver_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_source_resolver_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_source_resolver_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_source_resolver_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_source_resolver_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME chat_source_resolver COMMAND chat_source_resolver_tests)

  # Phase 21 corrective closure: a genuine TWO-PROCESS fresh-reload proof
  # (Chat/ChatSourceResolver/ChatSourceResolverReloadAcceptance.cpp) -
  # stopping an in-process watcher is not fresh-process evidence, so this
  # single binary is invoked twice as two entirely separate OS process
  # launches below, with CTest FIXTURES guaranteeing stage 1 completes
  # before stage 2 starts. Shares chat_source_resolver_tests' own dependency
  # set - the same Document/ContextRetriever machinery, exercised through
  # real on-disk documents and real production native contracts.
  singularity_add_executable(chat_source_resolver_reload_acceptance_tests
    Chat/ChatSourceResolver/ChatSourceResolverReloadAcceptance.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatActions/ChatActions.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_compile_definitions(chat_source_resolver_reload_acceptance_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_source_resolver_reload_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_source_resolver_reload_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(chat_source_resolver_reload_acceptance_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_source_resolver_reload_acceptance_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_source_resolver_reload_acceptance_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_source_resolver_reload_acceptance_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_source_resolver_reload_acceptance_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_source_resolver_reload_acceptance_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_source_resolver_reload_acceptance_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_chat_source_resolution_reload_stage1
    COMMAND chat_source_resolver_reload_acceptance_tests 1)
  add_test(NAME beta1_chat_source_resolution_reload_stage2
    COMMAND chat_source_resolver_reload_acceptance_tests 2)
  set_tests_properties(beta1_chat_source_resolution_reload_stage1 PROPERTIES
    FIXTURES_SETUP beta1_chat_source_resolution_reload)
  set_tests_properties(beta1_chat_source_resolution_reload_stage2 PROPERTIES
    FIXTURES_REQUIRED beta1_chat_source_resolution_reload)

  # Proves Phase 09's "Ask in Chat" persisted selection is genuinely
  # consumed by real local context assembly (MatterRequestExecution.hpp's
  # resolve_task_context_for_testing()) - not merely stored. Links
  # InferenceExecutorStubForTesting.cpp instead of the real
  # Routing/InferenceExecutor/InferenceExecutor.cpp: the same real-.cpp-
  # substitution seam this file already uses for
  # PdfExtractorMacOS.mm/PdfExtractorPdfium.cpp and
  # FileSystemWatcherMacOS.mm/FileSystemWatcherUnavailable.cpp, since this
  # test's proof deliberately never reaches the network/local-model call a
  # real task attempt makes after context assembly - see that stub's own
  # top comment.
  singularity_add_executable(matter_request_execution_tests
    Routing/MatterRequestExecution/MatterRequestExecution.test.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Context/SourcePicker/SourcePicker.cpp
    Context/ContextCuration/ContextCuration.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    Usage/UsageAccounting/UsageAccounting.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorStubForTesting.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Chat/ChatActions/ChatActions.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardServiceStubForTesting.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(matter_request_execution_tests PRIVATE Threads::Threads)
  target_compile_definitions(matter_request_execution_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(matter_request_execution_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(matter_request_execution_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(matter_request_execution_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(matter_request_execution_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(matter_request_execution_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(matter_request_execution_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(matter_request_execution_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(matter_request_execution_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(matter_request_execution_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_request_orchestration COMMAND matter_request_execution_tests)

  # Phase 23's Chat outbound-preview boundary (Chat/ChatOutboundPreview/
  # ChatOutboundPreview.hpp) - proves one pending ProjectChat composition
  # flows through Phase 21's real source resolution and Phase 22's real
  # operation setup into the EXISTING canonical request-preparation params,
  # with no second Chat request assembler. Links the same
  # InferenceExecutorStubForTesting.cpp/PapinianGuardServiceStubForTesting.cpp
  # seam matter_request_execution_tests uses: this target deliberately stops
  # before the provider/local-model call, so the real Papinian preview,
  # ephemeral session and literal outboundText are proven by the manual
  # production-path acceptance runner instead (tests/
  # chat_preview_live_acceptance.cpp).
  singularity_add_executable(chat_outbound_preview_tests
    Chat/ChatOutboundPreview/ChatOutboundPreview.test.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Bridge/ChatPreviewBridge/ChatPreviewBridge.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Context/SourcePicker/SourcePicker.cpp
    Context/ContextCuration/ContextCuration.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorStubForTesting.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Chat/ChatActions/ChatActions.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardServiceStubForTesting.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(chat_outbound_preview_tests PRIVATE Threads::Threads)
  target_compile_definitions(chat_outbound_preview_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_outbound_preview_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_outbound_preview_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(chat_outbound_preview_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_outbound_preview_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_outbound_preview_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_outbound_preview_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_outbound_preview_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_outbound_preview_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_outbound_preview_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME chat_outbound_preview COMMAND chat_outbound_preview_tests)

  # Phase 24's Chat execution, result and citation boundary
  # (Chat/ChatExecution/ChatExecution.hpp and Chat/ChatConversation/
  # ChatConversation.hpp). It runs the WHOLE real production pipeline above
  # inference - chat::run_chat_request() -> chat::prepare_chat_request()
  # (Phase 21 sources, Phase 22 setup) -> the canonical
  # matter_request_execution::run_task() -> PromptAssembler/ContextRetriever
  # -> the real SourceMarkerRegistry -> real MatterTask creation and real
  # persistence - and substitutes only the provider/local-model call itself
  # through InferenceExecutorScriptedForTesting.cpp, the same alternative-
  # implementation link seam chat_outbound_preview_tests already uses for
  # this header. That is what makes a fabricated-citation response and an
  # in-flight cancellation reproducible without asking a live model to
  # misbehave; real local and real external execution are proven separately
  # by tests/chat_execution_live_acceptance.cpp.
  singularity_add_executable(chat_execution_tests
    Chat/ChatExecution/ChatExecution.test.cpp
    Chat/ChatExecution/ChatExecution.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Context/ProjectContextActions/ProjectContextActions.cpp
    Context/ContextAutomation/ContextAutomation.cpp
    Privacy/EntityResolver/EntityResolver.cpp
    Chat/ChatConversation/ChatConversation.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Bridge/ChatExecutionBridge/ChatExecutionBridge.cpp
    Bridge/ChatPreviewBridge/ChatPreviewBridge.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/SourcePicker/SourcePicker.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
	Tool/ToolBroker/ToolBroker.cpp
	Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(chat_execution_tests PRIVATE Threads::Threads)
  target_compile_definitions(chat_execution_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "SINGULARITY_TEST_FIXTURES_DIR=\"${NATIVE_APP_CODE_ROOT}/tests/fixtures\""
  )
  if(APPLE)
    target_sources(chat_execution_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_execution_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(chat_execution_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_execution_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_execution_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_execution_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_execution_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_execution_tests PRIVATE /W4 /permissive- /utf-8)
    # run_tests() is one 9,000-line function: a Debug build gives it an 836 KiB
    # stack frame, and its first Chat request goes more than 170 KiB deeper -
    # past the 1 MiB main-thread stack MSVC links by default (0xC00000FD, a
    # stack overflow). The 8 MiB macOS and Linux give a main thread.
    target_link_options(chat_execution_tests PRIVATE /STACK:8388608)
  else()
    target_compile_options(chat_execution_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_chat_execution COMMAND chat_execution_tests)

  # Phase 27 corrective closeout: retry_task()/revise_task()/operation-
  # history need an independently-green target of their own - the broad
  # chat_execution_tests above has a known pre-existing crash in its own
  # item13 (Phase 24.1x connected-document/web-research, unrelated to Phase
  # 27) that aborts the whole process, so it can never be the sole PASS
  # evidence for Phase 27's own semantics. Same real scripted-execution
  # dependency closure as chat_execution_tests (a proven-working link), just
  # a different, narrowly-scoped entry point - see ChatLifecycleRevisionHistory.test.cpp's own header comment.
  singularity_add_executable(chat_lifecycle_revision_history_tests
    Chat/ChatExecution/ChatLifecycleRevisionHistory.test.cpp
    Chat/ChatExecution/ChatExecution.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Chat/ChatConversation/ChatConversation.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Bridge/ChatExecutionBridge/ChatExecutionBridge.cpp
    Bridge/ChatPreviewBridge/ChatPreviewBridge.cpp
    Bridge/ContextBridge/ContextBridge.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/SourcePicker/SourcePicker.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
	Tool/ToolBroker/ToolBroker.cpp
	Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(chat_lifecycle_revision_history_tests PRIVATE Threads::Threads)
  target_compile_definitions(chat_lifecycle_revision_history_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_lifecycle_revision_history_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_lifecycle_revision_history_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(chat_lifecycle_revision_history_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_lifecycle_revision_history_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_lifecycle_revision_history_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_lifecycle_revision_history_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_lifecycle_revision_history_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_lifecycle_revision_history_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_lifecycle_revision_history_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_chat_revision_history COMMAND chat_lifecycle_revision_history_tests)

  # Codex-like Project inspection end to end: a model that was not given
  # enough asks - twice, the second need raised by what the first read - to
  # list, search and read the Project through ToolBroker, then proposes a Word
  # change that waits for approval; after a real restart the approved change
  # is written as tracked changes with its provenance intact, and a proposal
  # whose evidence changed meanwhile is refused. Same scripted-execution
  # dependency closure as chat_lifecycle_revision_history_tests, so its proof
  # never depends on the broad chat_execution_tests suite.
  singularity_add_executable(chat_project_inspection_tests
    Chat/ChatExecution/ChatProjectInspection.test.cpp
    Chat/ChatExecution/ChatExecution.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Chat/ChatConversation/ChatConversation.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Bridge/ChatExecutionBridge/ChatExecutionBridge.cpp
    Bridge/ChatPreviewBridge/ChatPreviewBridge.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/SourcePicker/SourcePicker.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
	Tool/ToolBroker/ToolBroker.cpp
	Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(chat_project_inspection_tests PRIVATE Threads::Threads)
  target_compile_definitions(chat_project_inspection_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_project_inspection_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_project_inspection_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_WATCHER_SOURCE_OUTSIDE_MACOS}"
    )
  endif()
  target_include_directories(chat_project_inspection_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_project_inspection_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_project_inspection_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_project_inspection_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_project_inspection_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_project_inspection_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_project_inspection_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  # Two processes: stage 2 is the restart after which the approved change is
  # resumed and a proposal whose evidence changed is refused.
  add_test(NAME beta1_chat_project_inspection_stage1 COMMAND chat_project_inspection_tests 1)
  add_test(NAME beta1_chat_project_inspection_stage2 COMMAND chat_project_inspection_tests 2)
  set_tests_properties(beta1_chat_project_inspection_stage1 PROPERTIES
    FIXTURES_SETUP beta1_chat_project_inspection)
  set_tests_properties(beta1_chat_project_inspection_stage2 PROPERTIES
    FIXTURES_REQUIRED beta1_chat_project_inspection)
  # "Go ahead and create it" after a change shown first: put to the lawyer
  # again with no model call. Its own home, independent of stages 1 and 2.
  add_test(NAME beta1_chat_project_inspection_reoffer COMMAND chat_project_inspection_tests reoffer)

  # A question to the lawyer in the middle of a run, end to end: the model
  # stops, the operation waits for the lawyer's answer from another thread, and
  # the SAME run continues with it - answered, left to Singularity, stopped, closed,
  # past its limit, refused as written - in every action mode. Same
  # scripted-execution dependency closure as chat_project_inspection_tests.
  singularity_add_executable(chat_lawyer_question_tests
    Chat/ChatExecution/ChatLawyerQuestion.test.cpp
    Chat/ChatExecution/ChatExecution.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Chat/ChatConversation/ChatConversation.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Bridge/ChatExecutionBridge/ChatExecutionBridge.cpp
    Bridge/ChatPreviewBridge/ChatPreviewBridge.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/SourcePicker/SourcePicker.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
	Tool/ToolBroker/ToolBroker.cpp
	Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(chat_lawyer_question_tests PRIVATE Threads::Threads)
  target_compile_definitions(chat_lawyer_question_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_lawyer_question_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_lawyer_question_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp"
    )
  endif()
  target_include_directories(chat_lawyer_question_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_lawyer_question_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_lawyer_question_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_lawyer_question_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_lawyer_question_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_lawyer_question_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_lawyer_question_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_chat_lawyer_question COMMAND chat_lawyer_question_tests)

  # A question or web search the lawyer has not answered yet survives a
  # restart as a wait, not as an interruption, and the chat takes its title
  # when the question is filed. Same dependency closure as
  # chat_lawyer_question_tests.
  singularity_add_executable(chat_kept_pause_tests
    Chat/ChatExecution/ChatKeptPause.test.cpp
    Chat/ChatExecution/ChatExecution.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Chat/ChatConversation/ChatConversation.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Bridge/ChatExecutionBridge/ChatExecutionBridge.cpp
    Bridge/ChatPreviewBridge/ChatPreviewBridge.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/SourcePicker/SourcePicker.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
	Tool/ToolBroker/ToolBroker.cpp
	Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(chat_kept_pause_tests PRIVATE Threads::Threads)
  target_compile_definitions(chat_kept_pause_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_kept_pause_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_kept_pause_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp"
    )
  endif()
  target_include_directories(chat_kept_pause_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_kept_pause_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_kept_pause_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_kept_pause_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_kept_pause_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_kept_pause_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_kept_pause_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_chat_kept_pause COMMAND chat_kept_pause_tests)

  # An agent's change waits for the lawyer on every path that carries its
  # operation on - Resume after a restart, Retry, Send again - and an approval
  # written into a shared Matter's .lm writes nothing (Tool/OperationAuthority).
  singularity_add_executable(chat_agent_authority_tests
    Chat/ChatExecution/ChatAgentAuthority.test.cpp
    Chat/ChatExecution/ChatExecution.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Chat/ChatConversation/ChatConversation.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Bridge/ChatExecutionBridge/ChatExecutionBridge.cpp
    Bridge/ChatPreviewBridge/ChatPreviewBridge.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/SourcePicker/SourcePicker.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
	Tool/ToolBroker/ToolBroker.cpp
	Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(chat_agent_authority_tests PRIVATE Threads::Threads)
  target_compile_definitions(chat_agent_authority_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_agent_authority_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_agent_authority_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp"
    )
  endif()
  target_include_directories(chat_agent_authority_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_agent_authority_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_agent_authority_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_agent_authority_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_agent_authority_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_agent_authority_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_agent_authority_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME chat_agent_authority COMMAND chat_agent_authority_tests)

  # Work an agent does only on this computer stays here on every path that
  # carries its operation on - Resume after a restart, Retry, Run with another
  # model, Send again, its thread - with web research off and no network tool,
  # and is never folded into the Matter's memory (Tool/OperationAuthority).
  singularity_add_executable(chat_local_only_tests
    Chat/ChatExecution/ChatLocalOnly.test.cpp
    Chat/ChatExecution/ChatExecution.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Chat/ChatConversation/ChatConversation.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Bridge/ChatExecutionBridge/ChatExecutionBridge.cpp
    Bridge/ChatPreviewBridge/ChatPreviewBridge.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/SourcePicker/SourcePicker.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
	Tool/ToolBroker/ToolBroker.cpp
	Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(chat_local_only_tests PRIVATE Threads::Threads)
  target_compile_definitions(chat_local_only_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_local_only_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_local_only_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp"
    )
  endif()
  target_include_directories(chat_local_only_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_local_only_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_local_only_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_local_only_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_local_only_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_local_only_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_local_only_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME chat_local_only COMMAND chat_local_only_tests)

  # One Project stays usable however many other Projects the workspace holds:
  # a protection pass reads and writes only its own Project, opening a Project
  # page touches only that Project, and a workspace-wide read does not replay
  # every Project again. Counted per Project folder, never timed.
  singularity_add_executable(project_entry_scale_tests
    tests/project_entry_scale_acceptance.cpp
    Privacy/PrivacyActions/PrivacyActions.cpp
    Privacy/ProjectPreprocessing/ProjectPreprocessing.cpp
    Privacy/EntityCuration/EntityCuration.cpp
    Privacy/EntityResolver/EntityResolver.cpp
    Context/ContextAutomation/ContextAutomation.cpp
    Usage/UsageAccounting/UsageAccounting.cpp
    Context/ProjectContextActions/ProjectContextActions.cpp
    Usage/UsageQueries/UsageQueries.cpp
    Chat/ChatExecution/ChatExecution.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Chat/ChatConversation/ChatConversation.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/SourcePicker/SourcePicker.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
    Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
    Document/DirectDocumentWork/DirectDocumentWork.cpp
    Document/DocumentPatchTool/DocumentPatchTool.cpp
    Document/DocumentPatchValidator/DocumentPatchValidator.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
    Document/XlsxRowInsertion/XlsxRowInsertion.cpp
    Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(project_entry_scale_tests PRIVATE Threads::Threads)
  target_compile_definitions(project_entry_scale_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_sources(project_entry_scale_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
    target_link_libraries(project_entry_scale_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  else()
    target_sources(project_entry_scale_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp"
    )
  endif()
  target_include_directories(project_entry_scale_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(project_entry_scale_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(project_entry_scale_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(project_entry_scale_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(project_entry_scale_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_project_entry_scale COMMAND project_entry_scale_tests)

	singularity_add_executable(tool_broker_tests
	  Tool/ToolBroker/ToolBroker.test.cpp
	  Tool/ToolBroker/ToolBroker.cpp
	  Tool/OperationAuthority/OperationAuthority.cpp
	  Platform/AppSettings/AppSettings.cpp
	  Platform/Sha256/Sha256.cpp
	  Node/LegacyWorkspace/LegacyWorkspace.cpp
	  PapinianGuard/Policy/Policy.cpp
	  PapinianGuard/Taxonomy/Taxonomy.cpp
	  Provider/ModelClassification/ModelClassification.cpp
	)
	target_include_directories(tool_broker_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(tool_broker_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
	target_compile_definitions(tool_broker_tests PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
	if(MSVC)
	  target_compile_options(tool_broker_tests PRIVATE /W4 /permissive- /utf-8)
	else()
	  target_compile_options(tool_broker_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME beta1_research_tool_broker COMMAND tool_broker_tests)

	# What an operation may do, as this computer recorded it (Tool/OperationAuthority):
	# an agent's ceiling, content generated here and approvals given here - never
	# what a shared Matter's .lm says.
	singularity_add_executable(operation_authority_tests
	  Tool/OperationAuthority/OperationAuthority.test.cpp
	  Tool/OperationAuthority/OperationAuthority.cpp
	  Platform/AppSettings/AppSettings.cpp
	  Platform/Sha256/Sha256.cpp
	  Node/LegacyWorkspace/LegacyWorkspace.cpp
	  PapinianGuard/Policy/Policy.cpp
	  PapinianGuard/Taxonomy/Taxonomy.cpp
	  Provider/ModelClassification/ModelClassification.cpp
	)
	target_include_directories(operation_authority_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(operation_authority_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
	target_compile_definitions(operation_authority_tests PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
	if(MSVC)
	  target_compile_options(operation_authority_tests PRIVATE /W4 /permissive- /utf-8)
	else()
	  target_compile_options(operation_authority_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME operation_authority COMMAND operation_authority_tests)

	# Header-only: the question a model puts to the lawyer in the middle of a run
	# (Tool/LawyerQuestion) - the shape Singularity puts it in, every answer it
	# refuses and the words the model reads the answer in. Same dependency
	# closure as tool_broker_tests, for the production registration it proves.
	singularity_add_executable(lawyer_question_tests
	  Tool/LawyerQuestion/LawyerQuestion.test.cpp
	  Context/ControlText/ControlText.cpp
	  Tool/ToolBroker/ToolBroker.cpp
	  Tool/OperationAuthority/OperationAuthority.cpp
	  Platform/AppSettings/AppSettings.cpp
	  Platform/Sha256/Sha256.cpp
	  Node/LegacyWorkspace/LegacyWorkspace.cpp
	  PapinianGuard/Policy/Policy.cpp
	  PapinianGuard/Taxonomy/Taxonomy.cpp
	  Provider/ModelClassification/ModelClassification.cpp
	)
	target_include_directories(lawyer_question_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_link_libraries(lawyer_question_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
	target_compile_definitions(lawyer_question_tests PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
	if(MSVC)
	  target_compile_options(lawyer_question_tests PRIVATE /W4 /permissive- /utf-8)
	else()
	  target_compile_options(lawyer_question_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME lawyer_question COMMAND lawyer_question_tests)

	singularity_add_executable(external_research_tests
	  ExternalResearch/ExternalResearch.test.cpp
	  ExternalResearch/ExternalResearchAdapter/ExternalResearchAdapter.cpp
	  Tool/ToolBroker/ToolBroker.cpp
	  Tool/OperationAuthority/OperationAuthority.cpp
	  Platform/AppPaths/AppPaths.cpp
	  Platform/AppSettings/AppSettings.cpp
	  Platform/Sha256/Sha256.cpp
	  Node/LegacyWorkspace/LegacyWorkspace.cpp
	  PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
	  PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
	  Context/ComputedStatements/ComputedStatements.cpp
	  Context/TemporalContext/TemporalContext.cpp
	  Context/TemporalSemantics/TemporalSemantics.cpp
	  PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
	  PapinianGuard/ProtectionTransformer/ProtectionTransformer.cpp
	  PapinianGuard/Taxonomy/Taxonomy.cpp
	  PapinianGuard/Policy/Policy.cpp
	  Provider/ModelClassification/ModelClassification.cpp
	  PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
	  Context/BusinessCalendar/BusinessCalendar.cpp
	  PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
	  PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
	  Context/ControlText/ControlText.cpp
	  PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
	)
	if(APPLE)
	  target_sources(external_research_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientMacOS.mm")
	elseif(WIN32)
	  target_sources(external_research_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientWindows.cpp")
	  target_link_libraries(external_research_tests PRIVATE winhttp)
	else()
	  target_sources(external_research_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientLinux.cpp")
	endif()
	target_include_directories(external_research_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_compile_definitions(external_research_tests PRIVATE
	  "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
	  "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\"")
	target_link_libraries(external_research_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
	if(APPLE)
	  target_link_libraries(external_research_tests PRIVATE "-framework Foundation" "-framework Security")
	endif()
	if(MSVC)
	  target_compile_options(external_research_tests PRIVATE /W4 /permissive- /utf-8)
	else()
	  target_compile_options(external_research_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME beta1_research_external_evidence COMMAND external_research_tests)

	singularity_add_executable(capability_scoped_turn_contract_tests
	  ExternalResearch/CapabilityScopedTurnContract/CapabilityScopedTurnContract.test.cpp
	  ExternalResearch/ExternalResearchAdapter/ExternalResearchAdapter.cpp
	  Tool/ToolBroker/ToolBroker.cpp
	  Tool/OperationAuthority/OperationAuthority.cpp
	  Platform/AppPaths/AppPaths.cpp
	  Platform/AppSettings/AppSettings.cpp
	  Platform/Sha256/Sha256.cpp
	  Node/LegacyWorkspace/LegacyWorkspace.cpp
	  PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
	  PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
	  Context/ComputedStatements/ComputedStatements.cpp
	  Context/TemporalContext/TemporalContext.cpp
	  Context/TemporalSemantics/TemporalSemantics.cpp
	  PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
	  PapinianGuard/ProtectionTransformer/ProtectionTransformer.cpp
	  PapinianGuard/Taxonomy/Taxonomy.cpp
	  PapinianGuard/Policy/Policy.cpp
	  Provider/ModelClassification/ModelClassification.cpp
	  PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
	  Context/BusinessCalendar/BusinessCalendar.cpp
	  PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
	  PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
	  Context/ControlText/ControlText.cpp
	  PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
	)
	if(APPLE)
	  target_sources(capability_scoped_turn_contract_tests PRIVATE
	    "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientMacOS.mm")
	elseif(WIN32)
	  target_sources(capability_scoped_turn_contract_tests PRIVATE
	    "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientWindows.cpp")
	  target_link_libraries(capability_scoped_turn_contract_tests PRIVATE winhttp)
	else()
	  target_sources(capability_scoped_turn_contract_tests PRIVATE
	    "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientLinux.cpp")
	endif()
	target_include_directories(capability_scoped_turn_contract_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
	target_compile_definitions(capability_scoped_turn_contract_tests PRIVATE
	  "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
	  "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\"")
	target_link_libraries(capability_scoped_turn_contract_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
	if(APPLE)
	  target_link_libraries(capability_scoped_turn_contract_tests PRIVATE "-framework Foundation" "-framework Security")
	endif()
	if(MSVC)
	  target_compile_options(capability_scoped_turn_contract_tests PRIVATE /W4 /permissive- /utf-8)
	else()
	  target_compile_options(capability_scoped_turn_contract_tests PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	add_test(NAME capability_scoped_turn_contract COMMAND capability_scoped_turn_contract_tests)

  # A genuine TWO-PROCESS fresh-reload proof that a completed Chat result,
  # its validated citation, its real model provenance and a stopped
  # operation's lifecycle all survive an actual new OS process
  # (Chat/ChatExecution/ChatExecutionReloadAcceptance.cpp) - reuses
  # chat_operation_setup_reload_acceptance_tests' own two-stage CTest
  # FIXTURES pattern and chat_execution_tests' own dependency set.
  singularity_add_executable(chat_execution_reload_acceptance_tests
    Chat/ChatExecution/ChatExecutionReloadAcceptance.cpp
    Chat/ChatExecution/ChatExecution.cpp
    Routing/LiveAnswer/LiveAnswer.cpp
    Chat/ChatConversation/ChatConversation.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Chat/ChatOutboundPreview/ChatOutboundPreview.cpp
    Chat/ChatSourceResolver/ChatSourceResolver.cpp
    Chat/ChatOperationSetup/ChatOperationSetup.cpp
    Chat/ChatActions/ChatActions.cpp
    Bridge/ChatBridge/ChatBridge.cpp
    Bridge/ChatExecutionBridge/ChatExecutionBridge.cpp
    Bridge/ChatPreviewBridge/ChatPreviewBridge.cpp
    Routing/MatterRequestExecution/MatterRequestExecution.cpp
    Chat/ConversationClearing/ConversationClearing.cpp
    Chat/ConversationClearing/ConversationClearingRecords.cpp
    Context/SourcePicker/SourcePicker.cpp
    Consilium/ConsiliumActions/ConsiliumActions.cpp
    WorkProduct/WorkProductActions/WorkProductActions.cpp
	Document/ConnectedDocumentAction/ConnectedDocumentAction.cpp
	Document/DirectDocumentWork/DirectDocumentWork.cpp
	Document/DocumentPatchTool/DocumentPatchTool.cpp
	Document/DocumentPatchValidator/DocumentPatchValidator.cpp
	Document/DocumentStructuralReader/DocumentStructuralReader.cpp
	Document/XlsxWorkbookView/XlsxWorkbookView.cpp
	Document/XlsxNumberFormat/XlsxNumberFormat.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
	Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
	Document/XlsxRowInsertion/XlsxRowInsertion.cpp
	Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.cpp
    Routing/InferenceExecutor/InferenceExecutorScriptedForTesting.cpp
	Tool/ToolBroker/ToolBroker.cpp
	Tool/OperationAuthority/OperationAuthority.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Document/DocumentSourceResolver/DocumentSourceResolver.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    PapinianGuard/DeterministicDetector/DeterministicDetector.cpp
    PapinianGuard/PreviewSession/PreviewSession.cpp
    PapinianGuard/ProtectedPreview/ProtectedPreview.cpp
    PapinianGuard/PapinianGuardService/PapinianGuardService.cpp
    PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    Routing/LegalTaskPresets/LegalTaskPresets.cpp
    Routing/SystemPromptProfiles/SystemPromptProfiles.cpp
    Routing/RoutingLog/RoutingLog.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    WorkProduct/WorkflowDefinitions/WorkflowDefinitions.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    Context/PromptAssembler/PromptAssembler.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_link_libraries(chat_execution_reload_acceptance_tests PRIVATE Threads::Threads)
  target_compile_definitions(chat_execution_reload_acceptance_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  )
  if(APPLE)
    target_sources(chat_execution_reload_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(chat_execution_reload_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp"
    )
  endif()
  target_include_directories(chat_execution_reload_acceptance_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(chat_execution_reload_acceptance_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(chat_execution_reload_acceptance_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(chat_execution_reload_acceptance_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(chat_execution_reload_acceptance_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(chat_execution_reload_acceptance_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(chat_execution_reload_acceptance_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_chat_execution_reload_stage1
    COMMAND chat_execution_reload_acceptance_tests 1)
  add_test(NAME beta1_chat_execution_reload_stage2
    COMMAND chat_execution_reload_acceptance_tests 2)
  set_tests_properties(beta1_chat_execution_reload_stage1 PROPERTIES
    FIXTURES_SETUP beta1_chat_execution_reload)
  set_tests_properties(beta1_chat_execution_reload_stage2 PROPERTIES
    FIXTURES_REQUIRED beta1_chat_execution_reload)

  singularity_add_executable(document_extractor_tests
    Document/DocumentExtractor/DocumentExtractor.test.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
  )
  if(APPLE)
    target_sources(document_extractor_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
    )
  else()
    target_sources(document_extractor_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
    )
  endif()
  target_include_directories(document_extractor_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_extractor_tests PRIVATE
    nlohmann_json::nlohmann_json
    libzip::zip
    pugixml::pugixml
    utf8proc
  )
  target_include_directories(document_extractor_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(APPLE)
    target_link_libraries(document_extractor_tests PRIVATE
      "-framework PDFKit"
      "-framework CoreGraphics"
      "-framework Foundation"
    )
  endif()
  if(MSVC)
    target_compile_options(document_extractor_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_extractor_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_extractor COMMAND document_extractor_tests)

  singularity_add_executable(document_cache_tests
    Document/DocumentCache/DocumentCache.test.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Platform/Sha256/Sha256.cpp
  )
  if(APPLE)
    target_sources(document_cache_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
  else()
    target_sources(document_cache_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
  endif()
  target_include_directories(document_cache_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(document_cache_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
  target_include_directories(document_cache_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>")
  if(APPLE)
    target_link_libraries(document_cache_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(document_cache_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_cache_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_cache COMMAND document_cache_tests)

  singularity_add_executable(document_chunk_tests
    Document/DocumentChunker/DocumentChunker.test.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Platform/Sha256/Sha256.cpp
  )
  if(APPLE)
    target_sources(document_chunk_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
  else()
    target_sources(document_chunk_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
  endif()
  target_include_directories(document_chunk_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_chunk_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(document_chunk_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(APPLE)
    target_link_libraries(document_chunk_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework Foundation"
    )
  endif()
  if(MSVC)
    target_compile_options(document_chunk_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_chunk_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_chunk COMMAND document_chunk_tests)

  singularity_add_executable(docx_extractor_tests
    Document/DocxExtractor/DocxExtractor.test.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
  )
  target_include_directories(docx_extractor_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(docx_extractor_tests PRIVATE
    nlohmann_json::nlohmann_json
    libzip::zip
    pugixml::pugixml
    utf8proc
  )
  target_include_directories(docx_extractor_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(docx_extractor_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(docx_extractor_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME docx_extractor COMMAND docx_extractor_tests)

  # The reading a lawyer looks at, on a real Word package: pages, header and
  # footer, Word's own numbering, notes, comments, links, tracked changes and
  # tables under the locator's own identities.
  singularity_add_executable(docx_render_tests
    Document/DocxExtractor/DocxRender.test.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
  )
  target_include_directories(docx_render_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(docx_render_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
  target_include_directories(docx_render_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(docx_render_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(docx_render_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME docx_render COMMAND docx_render_tests)

  # The execution machine on a real Word package: every operation applied to a
  # file on disk, the file reopened, and the document itself asked what it now
  # says and how it is written.
  singularity_add_executable(docx_document_patch_tests
    Document/DocxExtractor/DocxDocumentPatch.test.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Document/DocumentPatchValidator/DocumentPatchValidator.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Platform/Sha256/Sha256.cpp
  )
  if(APPLE)
    target_sources(docx_document_patch_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
    target_link_libraries(docx_document_patch_tests PRIVATE "-framework PDFKit" "-framework Vision" "-framework CoreGraphics" "-framework AppKit" "-framework Foundation")
  else()
    target_sources(docx_document_patch_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
  endif()
  target_include_directories(docx_document_patch_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(docx_document_patch_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
  target_include_directories(docx_document_patch_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  if(MSVC)
    target_compile_options(docx_document_patch_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(docx_document_patch_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME docx_document_patch COMMAND docx_document_patch_tests)

  singularity_add_executable(document_operations_acceptance_tests
    tests/document_operations_acceptance.cpp
    Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
    Document/XlsxRowInsertion/XlsxRowInsertion.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Document/DirectDocumentWork/DirectDocumentWork.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Document/DocumentPatchTool/DocumentPatchTool.cpp
    Document/DocumentPatchValidator/DocumentPatchValidator.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(document_operations_acceptance_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_ROOT_DIR=\"${NATIVE_APP_ROOT}\""
  )
  if(APPLE)
    target_sources(document_operations_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(document_operations_acceptance_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp"
    )
  endif()
  target_include_directories(document_operations_acceptance_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_operations_acceptance_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(document_operations_acceptance_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(document_operations_acceptance_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(document_operations_acceptance_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(document_operations_acceptance_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_operations_acceptance_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_operations_stage1 COMMAND document_operations_acceptance_tests 1)
  add_test(NAME document_operations_stage2 COMMAND document_operations_acceptance_tests 2)
  set_tests_properties(document_operations_stage1 PROPERTIES FIXTURES_SETUP document_operations)
  set_tests_properties(document_operations_stage2 PROPERTIES FIXTURES_REQUIRED document_operations)

  # A lawyer's workbook changed through the one path every spreadsheet change takes.
  singularity_add_executable(professional_artifact_action_xlsx_tests
    Document/ProfessionalArtifactAction/ProfessionalArtifactActionXlsx.test.cpp
    Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
    Document/XlsxRowInsertion/XlsxRowInsertion.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Document/DirectDocumentWork/DirectDocumentWork.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Document/DocumentPatchTool/DocumentPatchTool.cpp
    Document/DocumentPatchValidator/DocumentPatchValidator.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(professional_artifact_action_xlsx_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_ROOT_DIR=\"${NATIVE_APP_ROOT}\""
  )
  if(APPLE)
    target_sources(professional_artifact_action_xlsx_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(professional_artifact_action_xlsx_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp"
    )
  endif()
  target_include_directories(professional_artifact_action_xlsx_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(professional_artifact_action_xlsx_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(professional_artifact_action_xlsx_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(professional_artifact_action_xlsx_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(professional_artifact_action_xlsx_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(professional_artifact_action_xlsx_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(professional_artifact_action_xlsx_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME professional_artifact_action_xlsx COMMAND professional_artifact_action_xlsx_tests)

  # The safety of every write Singularity makes to a lawyer's file: kept versions
  # and restoring one, one writer per document, read-only and metadata-
  # preserving replacement, and conflicts said in plain words.
  singularity_add_executable(document_version_store_tests
    Document/DocumentVersionStore/DocumentVersionStore.test.cpp
    Document/ProfessionalArtifactAction/ProfessionalArtifactAction.cpp
    Document/XlsxRowInsertion/XlsxRowInsertion.cpp
    Context/ComputedStatements/ComputedStatements.cpp
    Document/DirectDocumentWork/DirectDocumentWork.cpp
    Document/OfficeTextEdit/OfficeTextEdit.cpp
    Tool/ToolBroker/ToolBroker.cpp
    Tool/OperationAuthority/OperationAuthority.cpp
    Document/DocumentPatchTool/DocumentPatchTool.cpp
    Document/DocumentPatchValidator/DocumentPatchValidator.cpp
    Document/DocumentStructuralReader/DocumentStructuralReader.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Matter/MatterActions/MatterActions.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/PermissionAccess/PermissionAccess.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Routing/AiSetup/AiSetup.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/DocxExtractor/DocxExtractor.cpp
    Node/ActiveWorkspace/ActiveWorkspace.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/BusinessCalendar/BusinessCalendar.cpp
    PapinianGuard/ProtectedRelations/ProtectedRelations.cpp
    PapinianGuard/ProtectedOperations/ProtectedOperations.cpp
    Context/ControlText/ControlText.cpp
    PapinianGuard/RestorationGrammar/RestorationGrammar.cpp
  )
  target_compile_definitions(document_version_store_tests PRIVATE
    "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
    "NATIVE_APP_ROOT_DIR=\"${NATIVE_APP_ROOT}\""
  )
  if(APPLE)
    target_sources(document_version_store_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm"
    )
  else()
    target_sources(document_version_store_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp"
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp"
    )
  endif()
  target_include_directories(document_version_store_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(document_version_store_tests PRIVATE
    nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc
  )
  target_include_directories(document_version_store_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(document_version_store_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
  )
  if(APPLE)
    target_link_libraries(document_version_store_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework CoreServices" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(document_version_store_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(document_version_store_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME document_version_store COMMAND document_version_store_tests)

  # Real Chat -> DirectDocumentWork product routing (resolve_direct_document_
  # work_task_plan() in DirectDocumentWork.cpp) needs the same real Matter/
  # DocumentRegistry infrastructure matter_registry_tests already proves
  # buildable as a focused (non-full-app) target - this mirrors that exact
  # proven dependency set rather than inventing a second one.
  singularity_add_executable(direct_document_work_tests
    Document/DirectDocumentWork/DirectDocumentWork.test.cpp
    Document/DirectDocumentWork/DirectDocumentWork.cpp
    Document/DocxExtractor/DocxExtractor.cpp
	Document/OfficeTextEdit/OfficeTextEdit.cpp
    Document/DocumentRegistry/DocumentRegistry.cpp
    WorkProduct/ChronologyParser/ChronologyParser.cpp
    WorkProduct/ComparisonParser/ComparisonParser.cpp
    Context/ContextRetriever/ContextRetriever.cpp
    WorkProduct/FindingParser/FindingParser.cpp
    WorkProduct/ObligationParser/ObligationParser.cpp
    Context/SourceMarkers/SourceMarkers.cpp
    Context/QuoteVerification/QuoteVerification.cpp
    Document/DocumentCache/DocumentCache.cpp
    Document/DocumentChunker/DocumentChunker.cpp
    Document/DocumentExtractor/DocumentExtractor.cpp
    Document/CsvExtractor/CsvExtractor.cpp
    Document/PptxExtractor/PptxExtractor.cpp
    Document/XlsxExtractor/XlsxExtractor.cpp
    Document/XlsxWorkbookView/XlsxWorkbookView.cpp
    Document/XlsxNumberFormat/XlsxNumberFormat.cpp
    Matter/MatterRepository/MatterRepository.cpp
    Context/TemporalContext/TemporalContext.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp
    Persistence/ProjectStore/ProjectStore.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/OpenExternal/OpenExternal.cpp
    Platform/Sha256/Sha256.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Privacy/EntityRegistry/EntityRegistry.cpp
    Context/ContextRegistry/ContextRegistry.cpp
    Context/ContextCuration/ContextCuration.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  if(APPLE)
    target_sources(direct_document_work_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorMacOS.mm")
  else()
    target_sources(direct_document_work_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
  endif()
  target_include_directories(direct_document_work_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}"
  )
  target_link_libraries(direct_document_work_tests PRIVATE
    nlohmann_json::nlohmann_json
    libzip::zip
    pugixml::pugixml
    utf8proc
  )
  target_include_directories(direct_document_work_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  target_compile_definitions(direct_document_work_tests PRIVATE
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    "SINGULARITY_TEST_FIXTURES_DIR=\"${NATIVE_APP_CODE_ROOT}/tests/fixtures\""
  )
  if(APPLE)
    target_link_libraries(direct_document_work_tests PRIVATE
      "-framework PDFKit" "-framework CoreGraphics" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(direct_document_work_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(direct_document_work_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME beta1_document_direct_work COMMAND direct_document_work_tests)

  if(APPLE)
    singularity_add_executable(pdf_extractor_tests
      Document/PdfExtractor/PdfExtractor.test.mm
      Document/PdfExtractor/PdfExtractorMacOS.mm
    )
    target_include_directories(pdf_extractor_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}"
    )
    target_link_libraries(pdf_extractor_tests PRIVATE
      "-framework PDFKit"
      "-framework CoreGraphics"
      "-framework Foundation"
    )
    target_compile_options(pdf_extractor_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME pdf_extractor COMMAND pdf_extractor_tests)
  else()
    singularity_add_executable(pdf_extractor_tests
      Document/PdfExtractor/PdfExtractor.test.cpp
      Document/PdfExtractor/PdfExtractorPdfium.cpp
    )
    target_include_directories(pdf_extractor_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}"
    )
    if(MSVC)
      target_compile_options(pdf_extractor_tests PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(pdf_extractor_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME pdf_extractor COMMAND pdf_extractor_tests)
  endif()

  # --- Agent (Public Beta 1: standing work on this computer) --------------
  # Pure pieces first: the paragraph diff, the content-hash scan of a data
  # room, the agent store; then the run engine with fake reading, model and
  # report; then the state machine with its scheduler thread and a real
  # folder watch.
  function(singularity_add_agent_test target test_name)
    singularity_add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json)
    target_include_directories(${target} SYSTEM PRIVATE
      "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
    )
    target_compile_definitions(${target} PRIVATE
      "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
    )
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${test_name} COMMAND ${target})
  endfunction()

  singularity_add_agent_test(agent_text_diff_tests agent_text_diff
    Agent/TextDiff/TextDiff.test.cpp
    Agent/TextDiff/TextDiff.cpp
  )
  singularity_add_agent_test(agent_data_room_scan_tests agent_data_room_scan
    Agent/DataRoomScan/DataRoomScan.test.cpp
    Agent/DataRoomScan/DataRoomScan.cpp
    Platform/Sha256/Sha256.cpp
  )
  singularity_add_agent_test(agent_store_tests agent_store
    Agent/AgentStore/AgentStore.test.cpp
    Agent/AgentStore/AgentStore.cpp
    Platform/AppPaths/AppPaths.cpp
  )
  singularity_add_agent_test(agent_runner_tests agent_runner
    Agent/AgentRunner/AgentRunner.test.cpp
    Agent/AgentRunner/AgentRunner.cpp
    Agent/AgentStore/AgentStore.cpp
    Agent/DataRoomScan/DataRoomScan.cpp
    Agent/TextDiff/TextDiff.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/Sha256/Sha256.cpp
  )
  singularity_add_agent_test(agent_actions_tests agent_actions
    Agent/AgentActions/AgentActions.test.cpp
    Agent/AgentActions/AgentActions.cpp
    Agent/AgentRoles/AgentRoles.cpp
    Agent/AgentCard/AgentCard.cpp
    Agent/AgentScheduler/AgentScheduler.cpp
    Agent/AgentRunner/AgentRunner.cpp
    Agent/AgentStore/AgentStore.cpp
    Agent/DataRoomScan/DataRoomScan.cpp
    Agent/TextDiff/TextDiff.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/Sha256/Sha256.cpp
  )
  if(APPLE)
    target_sources(agent_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm")
    target_link_libraries(agent_actions_tests PRIVATE "-framework CoreServices" "-framework Foundation")
  else()
    target_sources(agent_actions_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp")
  endif()
  singularity_add_agent_test(ai_app_connections_tests ai_app_connections
    Agent/AiAppConnections/AiAppConnections.test.cpp
    Agent/AiAppConnections/AiAppConnections.cpp
    Agent/AgentRoles/AgentRoles.cpp
    Agent/AiAppChannel/AiAppChannel.cpp
    Agent/AgentActions/AgentActions.cpp
    Agent/AgentCard/AgentCard.cpp
    Agent/AgentScheduler/AgentScheduler.cpp
    Agent/AgentRunner/AgentRunner.cpp
    Agent/AgentStore/AgentStore.cpp
    Agent/DataRoomScan/DataRoomScan.cpp
    Agent/TextDiff/TextDiff.cpp
    Platform/AppPaths/AppPaths.cpp
    Platform/Sha256/Sha256.cpp
  )
  if(APPLE)
    target_sources(ai_app_connections_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm")
    target_link_libraries(ai_app_connections_tests PRIVATE "-framework CoreServices" "-framework Foundation")
  else()
    target_sources(ai_app_connections_tests PRIVATE
      "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherUnavailable.cpp")
  endif()
  singularity_add_agent_test(mcp_server_tests mcp_server
    Agent/McpServer/McpServer.test.cpp
    Agent/McpServer/McpServer.cpp
    Agent/AgentRoles/AgentRoles.cpp
  )
  # Windows names the AI app that started the helper by its Authenticode
  # signature: Windows' own catalogs, a signature embedded in the file (the
  # WebView2 loader Microsoft ships), an unsigned program, and ctest itself
  # as the caller.
  if(WIN32)
    singularity_add_executable(mcp_caller_windows_tests
      Agent/McpServer/McpCallerWindows.test.cpp
      Agent/McpServer/McpCallerWindows.cpp
    )
    target_include_directories(mcp_caller_windows_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(mcp_caller_windows_tests PRIVATE nlohmann_json::nlohmann_json wintrust crypt32)
    FetchContent_GetProperties(microsoft_web_webview2)
    set(singularity_embedded_signed_file "${microsoft_web_webview2_SOURCE_DIR}/build/native/x64/WebView2Loader.dll")
    if(EXISTS "${singularity_embedded_signed_file}")
      target_compile_definitions(mcp_caller_windows_tests PRIVATE
        "SINGULARITY_TEST_EMBEDDED_SIGNED_FILE=\"${singularity_embedded_signed_file}\"")
    endif()
    add_test(NAME mcp_caller_windows COMMAND mcp_caller_windows_tests)
  endif()
  # The one catalogue of the roles a colleague can be begun in (Beta 1 #272).
  singularity_add_agent_test(agent_roles_tests agent_roles
    Agent/AgentRoles/AgentRoles.test.cpp
    Agent/AgentRoles/AgentRoles.cpp
  )
  # What changed, said once to whoever listens (Beta 1 #263).
  singularity_add_agent_test(platform_events_tests platform_events
    Platform/Events/Events.test.cpp
  )
  # Singularity is closing: one fact every domain reads (Beta 1 #55, #16, #216).
  singularity_add_agent_test(platform_app_lifecycle_tests platform_app_lifecycle
    Platform/AppLifecycle/AppLifecycle.test.cpp
  )
  # The organisation read, built from facts alone (Beta 1 #273).
  singularity_add_agent_test(organisation_read_model_tests organisation_read_model
    Organisation/OrganisationReadModel/OrganisationReadModel.test.cpp
    Organisation/OrganisationReadModel/OrganisationReadModel.cpp
  )
  # Who at this computer answers for the firm (D-184): the role the account
  # flow records. Its settings stand in a directory of the test's own.
  singularity_add_executable(firm_role_tests
    Organisation/FirmRole/FirmRole.test.cpp
    Organisation/FirmRole/FirmRole.cpp
    Platform/AppSettings/AppSettings.cpp
    PapinianGuard/Policy/Policy.cpp
    Provider/ModelClassification/ModelClassification.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
  )
  target_include_directories(firm_role_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(firm_role_tests PRIVATE nlohmann_json::nlohmann_json)
  target_compile_definitions(firm_role_tests PRIVATE
    SINGULARITY_ISOLATED_TEST_SETTINGS=1
    "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  if(MSVC)
    target_compile_options(firm_role_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(firm_role_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME firm_role COMMAND firm_role_tests)
  # Who sits in each of the firm's seats (D-209), in a firm record of the
  # test's own.
  singularity_add_executable(firm_people_tests
    Organisation/FirmPeople/FirmPeople.test.cpp
    Organisation/FirmPeople/FirmPeople.cpp
  )
  target_include_directories(firm_people_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(firm_people_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(firm_people_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(firm_people_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME firm_people COMMAND firm_people_tests)
  # The firm's computers and the check two of them make of each other
  # (D-205), with a Legal Machines of the test's own in between.
  singularity_add_executable(firm_computers_tests
    Matter/FirmComputers/FirmComputers.test.cpp
    Matter/FirmComputers/FirmComputers.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(firm_computers_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(firm_computers_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(firm_computers_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(firm_computers_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME firm_computers COMMAND firm_computers_tests)
  # A Matter given to a member by choosing them: the signed offer in the two
  # members' personal channel, and the lawyer's answers (D-205).
  singularity_add_executable(matter_offers_tests
    Matter/MatterOffers/MatterOffers.test.cpp
    Matter/MatterOffers/MatterOffers.cpp
    Platform/Sha256/Sha256.cpp
  )
  target_include_directories(matter_offers_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(matter_offers_tests PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(matter_offers_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(matter_offers_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME matter_offers COMMAND matter_offers_tests)
  # Who connects or removes the firm's model accounts on this computer (D-184).
  singularity_add_agent_test(provider_account_gate_tests provider_account_gate
    Provider/AccountGate/AccountGate.test.cpp
    Provider/AccountGate/AccountGate.cpp
  )
  singularity_add_agent_test(shared_matter_tests shared_matter
    Matter/SharedMatter/SharedMatter.test.cpp
    Matter/SharedMatter/SharedMatter.cpp
    Platform/Sha256/Sha256.cpp
  )
  # The brief every AI colleague's request carries (D-101).
  singularity_add_agent_test(colleague_brief_tests colleague_brief
    Agent/ColleagueBrief/ColleagueBrief.test.cpp
    Agent/ColleagueBrief/ColleagueBrief.cpp
  )
  # A firm's Matters carried between its computers directly, sealed end to end
  # (D-123, D-124): two computers, two folders, the firm's own folder between.
  singularity_add_agent_test(matter_coordination_tests matter_coordination
    Matter/MatterCoordination/MatterCoordination.test.cpp
    Matter/MatterCoordination/MatterCoordination.cpp
    Matter/CoordinationSeal/CoordinationSeal.cpp
    Matter/CoordinationTransport/CoordinationTransport.cpp
    Matter/CoordinationTransport/FolderTransport.cpp
    Platform/Sha256/Sha256.cpp
  )
  # The seals of what the firm's computers send each other (D-123): a Matter's
  # key, a computer's key, a key sealed to a computer, a request to join, an
  # address card - XChaCha20-Poly1305, X25519 and BLAKE2b from Monocypher.
  singularity_add_agent_test(coordination_seal_tests coordination_seal
    Matter/CoordinationSeal/CoordinationSeal.test.cpp
    Matter/CoordinationSeal/CoordinationSeal.cpp
  )

  # Singularity in Word, Excel and Outlook (Office/, D-105): the certificate
  # and its one-shot authority, the rules for a pane's paths and names, the
  # add-ins' manifests, and the connection itself - its protocol and a real
  # TLS exchange on this computer's loopback. Mbed TLS is linked below.
  singularity_add_agent_test(office_certificate_tests office_certificate
    Office/OfficeCertificate/OfficeCertificate.test.cpp
    Office/OfficeCertificate/OfficeCertificate.cpp
  )
  singularity_add_agent_test(office_paths_tests office_paths
    Office/OfficePaths/OfficePaths.test.cpp
    Office/OfficePaths/OfficePaths.cpp
  )
  # Where a letter belongs among a firm's Matters (D-106).
  singularity_add_agent_test(letter_routing_tests letter_routing
    Correspondence/LetterRouting/LetterRouting.test.cpp
    Correspondence/LetterRouting/LetterRouting.cpp
  )
  singularity_add_agent_test(office_addins_tests office_addins
    Office/OfficeAddins/OfficeAddins.test.cpp
    Office/OfficeAddins/OfficeAddins.cpp
  )
  target_link_libraries(office_addins_tests PRIVATE pugixml::pugixml)
  # The applications Singularity opens as the person connects them, and the
  # file of its own each of Word, Excel and PowerPoint opens with (D-191): the
  # packages are read back here; no application is started.
  singularity_add_agent_test(office_apps_tests office_apps
    Office/OfficeApps/OfficeApps.test.cpp
    Office/OfficeApps/OfficeApps.cpp
  )
  target_link_libraries(office_apps_tests PRIVATE pugixml::pugixml libzip::zip)
  singularity_add_agent_test(office_clients_tests office_clients
    Office/OfficeClients/OfficeClients.test.cpp
    Office/OfficeClients/OfficeClients.cpp
    Platform/Sha256/Sha256.cpp
  )
  singularity_add_agent_test(office_connection_tests office_connection
    Office/OfficeConnection/OfficeConnection.test.cpp
    Office/OfficeConnection/OfficeConnection.cpp
    Office/OfficeCertificate/OfficeCertificate.cpp
    Office/OfficeClients/OfficeClients.cpp
    Platform/Sha256/Sha256.cpp
  )
endif()

# The helper an AI app starts to reach Singularity over the Model Context
# Protocol (Agent/McpServer): standard input and output only, no port. It is
# shipped inside the application, beside its executable.
set(NATIVE_APP_MCP_HELPER_SOURCES
  "${NATIVE_APP_SOURCE_DIR}/Agent/McpServer/McpServerMain.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/McpServer/McpServer.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AgentRoles/AgentRoles.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AiAppChannel/AiAppChannel.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Platform/AppPaths/AppPaths.cpp"
)
if(APPLE)
  list(APPEND NATIVE_APP_MCP_HELPER_SOURCES "${NATIVE_APP_SOURCE_DIR}/Agent/McpServer/McpCallerMacOS.cpp")
elseif(WIN32)
  list(APPEND NATIVE_APP_MCP_HELPER_SOURCES "${NATIVE_APP_SOURCE_DIR}/Agent/McpServer/McpCallerWindows.cpp")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND NATIVE_APP_MCP_HELPER_SOURCES "${NATIVE_APP_SOURCE_DIR}/Agent/McpServer/McpCallerLinux.cpp")
else()
  list(APPEND NATIVE_APP_MCP_HELPER_SOURCES "${NATIVE_APP_SOURCE_DIR}/Agent/McpServer/McpCallerUnavailable.cpp")
endif()
add_executable(singularity_mcp ${NATIVE_APP_MCP_HELPER_SOURCES})
set_target_properties(singularity_mcp PROPERTIES OUTPUT_NAME "singularity-mcp")
target_include_directories(singularity_mcp PRIVATE "${NATIVE_APP_SOURCE_DIR}")
target_link_libraries(singularity_mcp PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(singularity_mcp PRIVATE
  "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
)
if(WIN32)
  # A request's id comes from the system's secure random source
  # (BCryptGenRandom); the AI app that started the helper is named by its
  # Authenticode signature (wintrust, crypt32). It is built with the app and
  # lands beside its executable, in the same output folder, where the AI
  # apps' setup names it (Bridge/AgentBridge).
  target_link_libraries(singularity_mcp PRIVATE bcrypt wintrust crypt32)
  add_dependencies(native_app singularity_mcp)
endif()
if(APPLE)
  target_link_libraries(singularity_mcp PRIVATE "-framework Security" "-framework CoreFoundation")
  add_dependencies(native_app singularity_mcp)
  add_custom_command(TARGET native_app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:singularity_mcp>"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/MacOS/singularity-mcp"
    COMMAND /usr/bin/codesign --force --options runtime --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
            "$<TARGET_BUNDLE_CONTENT_DIR:native_app>/MacOS/singularity-mcp"
    # The bundle was sealed by the steps above; it is sealed again with the
    # helper inside it.
    COMMAND /usr/bin/codesign --force --deep --sign "${NATIVE_APP_CODE_SIGN_IDENTITY}"
            "$<TARGET_BUNDLE_DIR:native_app>"
    COMMENT "Placing the AI apps helper (singularity-mcp) inside the application"
    VERBATIM)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Built beside the executable, where the app tells AI apps to find it
  # (AgentBridge); installed beside it too.
  add_dependencies(native_app singularity_mcp)
endif()

# Singularity's updater on Windows (Platform/UpdateHelper; D-261). It stands
# alone, Windows' own libraries and a static runtime, because Singularity
# starts it from a copy outside its install folder, which the installer
# empties and fills again; it leaves its outcome through UpdateInstall.cpp,
# plain C++. Built beside the executable and installed beside it, where
# Platform/UpdateInstall finds it.
if(WIN32)
  add_executable(singularity_update WIN32
    "${NATIVE_APP_SOURCE_DIR}/Platform/UpdateHelper/UpdateHelper.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Platform/UpdateHelper/UpdateHelperWindows.cpp"
    "${NATIVE_APP_SOURCE_DIR}/Platform/UpdateInstall/UpdateInstall.cpp"
  )
  set_target_properties(singularity_update PROPERTIES
    OUTPUT_NAME "singularity-update"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_include_directories(singularity_update PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_compile_definitions(singularity_update PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(singularity_update PRIVATE bcrypt shell32 user32 gdi32 advapi32 ole32 dwmapi)
  if(MSVC)
    target_compile_options(singularity_update PRIVATE /W4 /permissive- /utf-8)
  else()
    target_link_options(singularity_update PRIVATE -static -municode)
  endif()
  add_dependencies(native_app singularity_update)
endif()

# Every target that reads PDFs through PDFium links it, and on Windows finds
# pdfium.dll beside its own executable (Linux finds libpdfium.so through the
# build tree's RPATH).
if(NOT APPLE)
  get_property(singularity_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(singularity_target IN LISTS singularity_targets)
    get_target_property(singularity_target_sources ${singularity_target} SOURCES)
    if(NOT singularity_target_sources)
      continue()
    endif()
    list(FILTER singularity_target_sources INCLUDE REGEX "PdfExtractorPdfium\\.cpp$")
    if(NOT singularity_target_sources)
      continue()
    endif()
    target_link_libraries(${singularity_target} PRIVATE native_app_pdfium)
    if(WIN32)
      add_custom_command(TARGET ${singularity_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NATIVE_APP_PDFIUM_LIBRARY}" "$<TARGET_FILE_DIR:${singularity_target}>"
        VERBATIM)
    endif()
  endforeach()
endif()

if(APPLE)
  install(TARGETS native_app BUNDLE DESTINATION . COMPONENT Runtime)
  install(
    DIRECTORY "${NATIVE_APP_FRONTEND_DIST}/"
    DESTINATION "${NATIVE_APP_DISPLAY_NAME}.app/Contents/Resources/frontend-dist"
    COMPONENT Runtime OPTIONAL
  )
  install(
    DIRECTORY "${NATIVE_APP_SAMPLE_MATTER}/"
    DESTINATION "${NATIVE_APP_DISPLAY_NAME}.app/Contents/Resources/sample-matter"
    COMPONENT Runtime OPTIONAL
  )
elseif(WIN32)
  install(TARGETS native_app RUNTIME DESTINATION . COMPONENT Runtime)
  # The helper an AI app starts, beside the executable as in the build tree.
  install(TARGETS singularity_mcp RUNTIME DESTINATION . COMPONENT Runtime)
  install(TARGETS singularity_update RUNTIME DESTINATION . COMPONENT Runtime)
  # Microsoft's Visual C++ runtime, which ONNX Runtime's own build
  # (onnxruntime.dll) needs: Microsoft's redistributable, pinned by its
  # versioned URL and SHA-256, which the installer runs and then removes
  # (CPACK_NSIS_EXTRA_INSTALL_COMMANDS). Installed centrally, Windows Update
  # keeps it patched; copies beside the program would never be.
  set(NATIVE_APP_VC_REDIST "${CMAKE_BINARY_DIR}/vc-redist/VC_redist.x64.exe")
  file(DOWNLOAD
    "https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe"
    "${NATIVE_APP_VC_REDIST}"
    EXPECTED_HASH SHA256=cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b
    STATUS native_app_vc_redist_status)
  list(GET native_app_vc_redist_status 0 native_app_vc_redist_code)
  if(NOT native_app_vc_redist_code EQUAL 0)
    message(FATAL_ERROR "Microsoft's Visual C++ redistributable 14.44.35211 could not be fetched or did not match "
      "its pinned SHA-256: ${native_app_vc_redist_status}")
  endif()
  install(FILES "${NATIVE_APP_VC_REDIST}" DESTINATION . COMPONENT Runtime)
  install(FILES "${NATIVE_APP_ONNXRUNTIME_LIBRARY}" DESTINATION .
          RENAME onnxruntime.dll COMPONENT Runtime)
  install(FILES "${native_app_onnxruntime_SOURCE_DIR}/LICENSE"
          "${native_app_onnxruntime_SOURCE_DIR}/ThirdPartyNotices.txt"
          DESTINATION licenses/onnxruntime COMPONENT Runtime)
  # The same layout the build tree stages beside the executable: local
  # inference's llama.cpp runtime (found in llama.cpp/ beside the executable),
  # Papinian Guard's word list, and the licenses the macOS bundle carries
  # in Resources/licenses.
  install(DIRECTORY "${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}/" DESTINATION llama.cpp COMPONENT Runtime)
  install(FILES "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/CommonWords/english_common_words.txt"
          DESTINATION . COMPONENT Runtime)
  install(FILES "${NATIVE_APP_ROOT}/deploy/backend/licenses/llama.cpp/LICENSE"
          "${NATIVE_APP_ROOT}/deploy/backend/licenses/llama.cpp/NOTICE"
          DESTINATION licenses/llama.cpp COMPONENT Runtime)
  install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT"
          DESTINATION licenses/nlohmann-json COMPONENT Runtime)
  if(NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR STREQUAL "cuda")
    install(FILES "${NATIVE_APP_ROOT}/deploy/backend/licenses/cuda/NOTICE"
            DESTINATION licenses/cuda COMPONENT Runtime)
  endif()
  install(FILES "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/LICENSE"
          "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/NOTICE"
          DESTINATION licenses/gliner2-rs COMPONENT Runtime)
  install(FILES "${NATIVE_APP_ROOT}/deploy/backend/licenses/papinian-ner/NOTICE"
          "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/LICENSE"
          DESTINATION licenses/papinian-ner COMPONENT Runtime)
  install(FILES "${NATIVE_APP_PDFIUM_LIBRARY}" DESTINATION . COMPONENT Runtime)
  install(FILES "${native_app_pdfium_SOURCE_DIR}/LICENSE"
          DESTINATION licenses/pdfium COMPONENT Runtime)
  install(FILES "${SINGULARITY_MBEDTLS_LICENCE}"
          DESTINATION licenses/mbedtls COMPONENT Runtime)
  install(DIRECTORY "${native_app_pdfium_SOURCE_DIR}/licenses/"
          DESTINATION licenses/pdfium COMPONENT Runtime)
  install(
    DIRECTORY "${NATIVE_APP_FRONTEND_DIST}/"
    DESTINATION frontend-dist COMPONENT Runtime OPTIONAL
  )
  install(
    DIRECTORY "${NATIVE_APP_SAMPLE_MATTER}/"
    DESTINATION sample-matter COMPONENT Runtime OPTIONAL
  )
else()
  # Linux: one self-contained application directory, the layout Windows
  # installs. /opt/singularity holds the executable and every library it
  # loads through $ORIGIN, local inference's llama.cpp runtime in llama.cpp/
  # (where Singularity looks for it), Papinian Guard's word list, the
  # frontend, the sample Matter and the licenses. /usr/bin/singularity links
  # to it, and the desktop entry, icon and metainfo go where desktops read
  # them. The package installs under "/" (CPACK_PACKAGING_INSTALL_PREFIX).
  set(NATIVE_APP_LINUX_APP_DIR "opt/${NATIVE_APP_SLUG}")
  install(TARGETS native_app singularity_mcp RUNTIME DESTINATION "${NATIVE_APP_LINUX_APP_DIR}" COMPONENT Runtime)
  set_target_properties(native_app PROPERTIES INSTALL_RPATH "$ORIGIN")
  install(FILES "${NATIVE_APP_ONNXRUNTIME_LIBRARY}" DESTINATION "${NATIVE_APP_LINUX_APP_DIR}"
          RENAME "libonnxruntime.so.${NATIVE_APP_ONNXRUNTIME_VERSION}" COMPONENT Runtime)
  install(DIRECTORY "${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}/"
          DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/llama.cpp" USE_SOURCE_PERMISSIONS COMPONENT Runtime)
  install(FILES "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/CommonWords/english_common_words.txt"
          DESTINATION "${NATIVE_APP_LINUX_APP_DIR}" COMPONENT Runtime)
  install(FILES "${NATIVE_APP_PDFIUM_LIBRARY}" DESTINATION "${NATIVE_APP_LINUX_APP_DIR}" COMPONENT Runtime)
  install(FILES "${native_app_onnxruntime_SOURCE_DIR}/LICENSE"
          "${native_app_onnxruntime_SOURCE_DIR}/ThirdPartyNotices.txt"
          DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/licenses/onnxruntime" COMPONENT Runtime)
  install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT"
          DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/licenses/nlohmann-json" COMPONENT Runtime)
  if(CURL_FOUND)
    install(FILES "${NATIVE_APP_ROOT}/deploy/backend/licenses/curl/NOTICE"
            DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/licenses/curl" COMPONENT Runtime)
  endif()
  if(NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR STREQUAL "cuda")
    install(FILES "${NATIVE_APP_ROOT}/deploy/backend/licenses/cuda/NOTICE"
            DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/licenses/cuda" COMPONENT Runtime)
  endif()
  install(FILES "${NATIVE_APP_ROOT}/deploy/backend/licenses/llama.cpp/LICENSE"
          "${NATIVE_APP_ROOT}/deploy/backend/licenses/llama.cpp/NOTICE"
          DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/licenses/llama.cpp" COMPONENT Runtime)
  install(FILES "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/LICENSE"
          "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/NOTICE"
          DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/licenses/gliner2-rs" COMPONENT Runtime)
  install(FILES "${NATIVE_APP_ROOT}/deploy/backend/licenses/papinian-ner/NOTICE"
          "${NATIVE_APP_GLINER2_BRIDGE_DIR}/vendor/gliner2_inference/LICENSE"
          DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/licenses/papinian-ner" COMPONENT Runtime)
  install(FILES "${native_app_pdfium_SOURCE_DIR}/LICENSE"
          DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/licenses/pdfium" COMPONENT Runtime)
  install(DIRECTORY "${native_app_pdfium_SOURCE_DIR}/licenses/"
          DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/licenses/pdfium" COMPONENT Runtime)
  install(
    DIRECTORY "${NATIVE_APP_FRONTEND_DIST}/"
    DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/frontend-dist"
    COMPONENT Runtime OPTIONAL
  )
  install(
    DIRECTORY "${NATIVE_APP_SAMPLE_MATTER}/"
    DESTINATION "${NATIVE_APP_LINUX_APP_DIR}/sample-matter"
    COMPONENT Runtime OPTIONAL
  )
  install(CODE "
    set(lm_bin \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/usr/bin\")
    file(MAKE_DIRECTORY \"\${lm_bin}\")
    file(CREATE_LINK \"/${NATIVE_APP_LINUX_APP_DIR}/${NATIVE_APP_SLUG}\" \"\${lm_bin}/${NATIVE_APP_SLUG}\" SYMBOLIC)
  " COMPONENT Runtime)
  install(
    FILES "${NATIVE_APP_ROOT}/deploy/backend/linux/app.desktop"
    DESTINATION "usr/share/applications"
    RENAME "${NATIVE_APP_SLUG}.desktop"
    COMPONENT Runtime
  )
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/${NATIVE_APP_IDENTIFIER}.metainfo.xml"
    DESTINATION "usr/share/metainfo"
    COMPONENT Runtime
  )
  if(NATIVE_APP_HAS_ICON)
    install(
      FILES "${NATIVE_APP_LINUX_ICON}"
      DESTINATION "usr/share/icons/hicolor/256x256/apps"
      RENAME "${NATIVE_APP_SLUG}.png"
      COMPONENT Runtime
    )
  endif()
endif()

set(CPACK_PACKAGE_NAME "${NATIVE_APP_DISPLAY_NAME}")
set(CPACK_PACKAGE_VENDOR "Legal Machines")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${NATIVE_APP_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Legal Machines")
set(CPACK_PACKAGE_CHECKSUM SHA256)
if(EXISTS "${NATIVE_APP_ROOT}/LICENSE")
  set(CPACK_RESOURCE_FILE_LICENSE "${NATIVE_APP_ROOT}/LICENSE")
endif()
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_VERBATIM_VARIABLES YES)

# The processor each installer is for, in the app's and the site's words
# (D-216): x64 or arm64, or universal for a macOS build of both. publish.py
# site reads it from the name, so every installer says it.
set(_singularity_processor "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _singularity_architectures)
  if(_singularity_architectures GREATER 1)
    set(_singularity_processor "universal")
  elseif(_singularity_architectures EQUAL 1)
    set(_singularity_processor "${CMAKE_OSX_ARCHITECTURES}")
  endif()
endif()
string(TOLOWER "${_singularity_processor}" _singularity_processor)
if(_singularity_processor MATCHES "^(arm64|aarch64)$")
  set(SINGULARITY_PACKAGE_PROCESSOR "arm64")
elseif(_singularity_processor MATCHES "^(x86_64|amd64|x64)$")
  set(SINGULARITY_PACKAGE_PROCESSOR "x64")
else()
  set(SINGULARITY_PACKAGE_PROCESSOR "${_singularity_processor}")
endif()

if(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_DMG_VOLUME_NAME "${NATIVE_APP_DISPLAY_NAME}")
  set(CPACK_DMG_FORMAT "UDZO")
  set(CPACK_DMG_BACKGROUND_IMAGE "${NATIVE_APP_DMG_BACKGROUND}")
  # The disk image places the bundle by its name (D-078).
  configure_file(
    "${NATIVE_APP_ROOT}/deploy/backend/macos/installer/dmg-layout.applescript.in"
    "${CMAKE_CURRENT_BINARY_DIR}/dmg-layout.applescript"
    @ONLY)
  set(CPACK_DMG_DS_STORE_SETUP_SCRIPT
      "${CMAKE_CURRENT_BINARY_DIR}/dmg-layout.applescript")
  set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK OFF)
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)
  set(CPACK_PACKAGE_FILE_NAME
      "${NATIVE_APP_SLUG}-${PROJECT_VERSION}-macOS-${SINGULARITY_PACKAGE_PROCESSOR}")
  # The staged application is signed again before the disk image is made:
  # CPack's install step rewrites its library paths, which breaks the build's
  # signature (macos/cpack-sign.cmake). A Developer ID identity signs it for
  # distribution; without one it is signed ad hoc, for this computer only.
  set(NATIVE_APP_RELEASE_SIGN_IDENTITY "" CACHE STRING
      "Developer ID Application identity that signs the distributed application; empty signs it with the build's identity")
  if(NATIVE_APP_RELEASE_SIGN_IDENTITY STREQUAL "")
    set(CPACK_LM_SIGN_IDENTITY "${NATIVE_APP_CODE_SIGN_IDENTITY}")
    set(CPACK_LM_SIGN_FOR_DISTRIBUTION OFF)
  else()
    set(CPACK_LM_SIGN_IDENTITY "${NATIVE_APP_RELEASE_SIGN_IDENTITY}")
    set(CPACK_LM_SIGN_FOR_DISTRIBUTION ON)
  endif()
  set(CPACK_PRE_BUILD_SCRIPTS "${NATIVE_APP_ROOT}/deploy/backend/macos/cpack-sign.cmake")
elseif(WIN32)
  # Preserve native separators through CPack's generated config and makensis.
  string(REPLACE "/" "\\\\" NATIVE_APP_WINDOWS_ICON_NSIS
         "${NATIVE_APP_WINDOWS_ICON}")
  string(REPLACE "/" "\\\\" NATIVE_APP_NSIS_SIDEBAR_NSIS
         "${NATIVE_APP_NSIS_SIDEBAR}")
  set(NATIVE_APP_WINDOWS_PACKAGER "NSIS" CACHE STRING
      "CPack generator for Windows: NSIS or WIX")
  set_property(CACHE NATIVE_APP_WINDOWS_PACKAGER PROPERTY STRINGS NSIS WIX)
  set(CPACK_GENERATOR "${NATIVE_APP_WINDOWS_PACKAGER}")
  # The install folder bears the application's name: AI apps keep the path of
  # the helper in it (singularity-mcp.exe), and an upgrade replaces the
  # program in place (D-125).
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "${NATIVE_APP_DATA_NAME}")
  set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "${NATIVE_APP_SLUG}")
  set(CPACK_PACKAGE_EXECUTABLES
      "${NATIVE_APP_DISPLAY_NAME}" "${NATIVE_APP_DISPLAY_NAME}")
  set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
  set(CPACK_NSIS_DISPLAY_NAME "${NATIVE_APP_DISPLAY_NAME}")
  set(CPACK_NSIS_PACKAGE_NAME "${NATIVE_APP_DISPLAY_NAME}")
  set(CPACK_NSIS_CONTACT "Legal Machines")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
  set(CPACK_NSIS_MUI_ICON "${NATIVE_APP_WINDOWS_ICON_NSIS}")
  set(CPACK_NSIS_MUI_UNIICON "${NATIVE_APP_WINDOWS_ICON_NSIS}")
  set(CPACK_NSIS_MUI_WELCOMEFINISHPAGE_BITMAP
      "${NATIVE_APP_NSIS_SIDEBAR_NSIS}")
  set(CPACK_NSIS_MUI_UNWELCOMEFINISHPAGE_BITMAP
      "${NATIVE_APP_NSIS_SIDEBAR_NSIS}")
  set(CPACK_NSIS_WELCOME_TITLE "Install ${NATIVE_APP_DISPLAY_NAME}")
  set(CPACK_NSIS_FINISH_TITLE "${NATIVE_APP_DISPLAY_NAME} is ready")
  set(CPACK_NSIS_BRANDING_TEXT "Legal Machines")
  # Linux NSIS deliberately disables /TRIM because its text measurement uses
  # Win32 APIs. Keep the same branding without /TRIM so a real Windows
  # installer can be produced locally from the macOS Docker toolchain.
  set(CPACK_NSIS_BRANDING_TEXT_CODE "BrandingText \"Legal Machines\"")
  set(CPACK_NSIS_INSTALLED_ICON_NAME "${NATIVE_APP_DISPLAY_NAME}.exe")
  # The last page offers to start Singularity, through Explorer, so that it
  # runs as the person and not with the installer's rights: an elevated
  # Singularity takes no files dragged in from Explorer (the owner,
  # 2026-11-05). An update installs silently and shows no page; its updater
  # starts Singularity itself (Platform/UpdateHelper).
  set(CPACK_NSIS_DEFINES "
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT \"Run ${NATIVE_APP_DISPLAY_NAME}\"
!define MUI_FINISHPAGE_RUN_FUNCTION RunSingularityAsUser
Function RunSingularityAsUser
  Exec '\"$WINDIR\\explorer.exe\" \"$INSTDIR\\${NATIVE_APP_DISPLAY_NAME}.exe\"'
FunctionEnd")
  set(CPACK_NSIS_UNINSTALL_NAME "Uninstall ${NATIVE_APP_DISPLAY_NAME}")
  # The Visual C++ runtime is installed from Microsoft's redistributable the
  # package carries, quietly, and the redistributable is then removed. Its
  # answer is in the installer's details: 0 installed, 1638 a newer one was
  # there, 3010 installed and Windows wants a restart.
  set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
    "ExecWait '\"$INSTDIR\\VC_redist.x64.exe\" /install /quiet /norestart' $0
  DetailPrint 'Microsoft Visual C++ runtime: $0'
  Delete '$INSTDIR\\VC_redist.x64.exe'")
  set(CPACK_WIX_PRODUCT_ICON "${NATIVE_APP_WINDOWS_ICON}")
  set(CPACK_WIX_UI_DIALOG "${NATIVE_APP_WIX_DIALOG}")
  set(CPACK_WIX_UI_BANNER "${NATIVE_APP_WIX_BANNER}")
  set(CPACK_WIX_PROGRAM_MENU_FOLDER "${NATIVE_APP_DISPLAY_NAME}")
  set(CPACK_WIX_ROOT_FEATURE_TITLE "${NATIVE_APP_DISPLAY_NAME}")
  set(CPACK_WIX_ROOT_FEATURE_DESCRIPTION "${NATIVE_APP_DESCRIPTION}")
  set(CPACK_PACKAGE_FILE_NAME
      "${NATIVE_APP_SLUG}-${PROJECT_VERSION}-Windows-${SINGULARITY_PACKAGE_PROCESSOR}")
  # The MinGW package is refused before it is made when a program in it
  # imports a library that neither it nor Windows has
  # (windows/CheckPackageImports.cmake): such a program does not start on a
  # clean Windows.
  if(MINGW AND CMAKE_OBJDUMP)
    set(CPACK_SINGULARITY_OBJDUMP "${CMAKE_OBJDUMP}")
    set(CPACK_PRE_BUILD_SCRIPTS "${NATIVE_APP_ROOT}/deploy/backend/windows/CheckPackageImports.cmake")
  endif()
else()
  set(CPACK_GENERATOR "DEB")
  # The install rules name /opt/singularity and /usr/share themselves.
  set(CPACK_PACKAGING_INSTALL_PREFIX "/")
  # The Debian package is named by the slug, as its folders are, not by the
  # display name CPack would take.
  set(CPACK_DEBIAN_PACKAGE_NAME "${NATIVE_APP_SLUG}")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Legal Machines")
  set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_COMPRESSION_TYPE "xz")
  set(CPACK_DEBIAN_COMPRESSION_LEVEL 9)
  set(CPACK_DEBIAN_FILE_NAME
      "${NATIVE_APP_SLUG}-${PROJECT_VERSION}-Linux-${SINGULARITY_PACKAGE_PROCESSOR}.deb")
  set(CPACK_PACKAGE_FILE_NAME
      "${NATIVE_APP_SLUG}-${PROJECT_VERSION}-Linux-${SINGULARITY_PACKAGE_PROCESSOR}")
endif()

include(CPack)

# ---------------------------------------------------------------------------
# Singularity test suite labels
#
# Every CTest name states a current product capability and behaviour. No test
# name records the development phase, work package or dated run that happened
# to introduce it - that history belongs in source comments, where it explains
# a decision, not in a name a reader must decode years later.
#
# Beta 1 tests carry the canonical beta1_<capability>_<behavior> name and the
# `beta1` label. They are labelled rather than duplicated, so they stay the
# SAME tests the ordinary gate runs: `ctest` is the full gate, `ctest -L beta1`
# is simply the question "is the product itself sound".
#
# Labels are orthogonal and combinable:
#   tier    beta1 | acceptance | integration | unit
#   domain  chat | document | context | privacy | project | local | provider
#           research | usage | cli | platform | restart | bridge
# so `ctest -L privacy`, `ctest -L restart` or `ctest -L "beta1" -L document`
# each select a real, meaningful slice of the suite.
function(singularity_label_test singularity_test)
	if(TEST ${singularity_test})
		set_property(TEST ${singularity_test} APPEND PROPERTY LABELS ${ARGN})
	endif()
endfunction()

# Beta 1: a lawyer-visible product invariant proven end to end - a real request
# through the real pipeline, a real document change and its recovery, real
# provenance, real cost, a real restart - never a unit of code in isolation.
singularity_label_test(beta1_chat_execution beta1 acceptance chat)
singularity_label_test(beta1_chat_execution_reload_stage1 beta1 acceptance chat restart)
singularity_label_test(beta1_chat_execution_reload_stage2 beta1 acceptance chat restart)
singularity_label_test(beta1_request_orchestration beta1 acceptance chat)
singularity_label_test(beta1_document_direct_work beta1 acceptance document)
singularity_label_test(beta1_document_work_continuity_stage1 beta1 acceptance document restart)
singularity_label_test(beta1_document_work_continuity_stage2 beta1 acceptance document restart)
singularity_label_test(beta1_document_restart_recovery_stage1 beta1 acceptance document restart)
singularity_label_test(beta1_document_restart_recovery_stage2 beta1 acceptance document restart)
singularity_label_test(beta1_operation_long_horizon_replay beta1 acceptance chat restart)
singularity_label_test(beta1_chat_operation_setup_reload_stage1 beta1 acceptance chat restart)
singularity_label_test(beta1_chat_operation_setup_reload_stage2 beta1 acceptance chat restart)
singularity_label_test(beta1_chat_source_resolution_reload_stage1 beta1 acceptance chat restart context)
singularity_label_test(beta1_chat_source_resolution_reload_stage2 beta1 acceptance chat restart context)
singularity_label_test(beta1_chat_actions beta1 acceptance chat)
singularity_label_test(beta1_chat_revision_history beta1 acceptance chat)
singularity_label_test(beta1_chat_lawyer_question beta1 acceptance chat)
singularity_label_test(beta1_chat_kept_pause beta1 acceptance chat restart)
singularity_label_test(beta1_project_actions beta1 acceptance project)
singularity_label_test(beta1_project_persistence beta1 acceptance project)
singularity_label_test(beta1_context_automation beta1 acceptance context)
singularity_label_test(beta1_context_curation beta1 acceptance context)
singularity_label_test(beta1_context_registry beta1 acceptance context)
singularity_label_test(beta1_context_prompt_assembly beta1 acceptance context)
singularity_label_test(beta1_usage_accounting beta1 acceptance usage)
singularity_label_test(beta1_usage_statement beta1 acceptance usage)
singularity_label_test(beta1_routing_log beta1 acceptance usage)
singularity_label_test(beta1_research_external_evidence beta1 acceptance research)
singularity_label_test(beta1_research_tool_broker beta1 acceptance research)
singularity_label_test(beta1_cli_journey beta1 acceptance cli)
singularity_label_test(beta1_consilium_actions beta1 acceptance project)
singularity_label_test(beta1_privacy_papinian_guard beta1 acceptance privacy)
singularity_label_test(beta1_privacy_protected_api beta1 acceptance privacy)
singularity_label_test(beta1_privacy_protected_preview beta1 acceptance privacy)
singularity_label_test(beta1_credential_store beta1 acceptance privacy)
singularity_label_test(beta1_permission_access beta1 acceptance privacy)
singularity_label_test(beta1_ai_setup beta1 acceptance local)
singularity_label_test(beta1_local_inference_service beta1 acceptance local)

# The rest of the suite: focused proofs of one component's behaviour.
singularity_label_test(background_executor unit platform)
singularity_label_test(preview_session unit privacy)
singularity_label_test(provider_protection unit privacy)
singularity_label_test(workspace_registry unit project)
singularity_label_test(matter_registry unit project)
singularity_label_test(model_classification unit local)
singularity_label_test(generated_file_bridge unit platform)
singularity_label_test(lawyer_question unit chat)
singularity_label_test(document_journal unit document)
singularity_label_test(memory_keeper unit context)
singularity_label_test(gliner2_runtime_offset unit privacy)
singularity_label_test(operation_event unit platform)
singularity_label_test(execution_progress unit platform)
singularity_label_test(provider_registry unit provider)
singularity_label_test(local_model_removal_policy unit local)
singularity_label_test(local_model_assignment_scan unit local)
singularity_label_test(model_lifecycle unit local)
singularity_label_test(execution_compatibility unit local)
singularity_label_test(local_model_registry unit local)
singularity_label_test(user_model_files unit local)
singularity_label_test(file_system_watcher unit platform)
singularity_label_test(local_model_uninstall_orchestrator unit local)
singularity_label_test(hardware_calibration unit local)
singularity_label_test(resource_benchmark unit local)
singularity_label_test(app_settings unit platform)
singularity_label_test(system_prompt_profiles unit context)
singularity_label_test(legal_task_presets unit context)
singularity_label_test(workflow_definitions unit context)
singularity_label_test(context_retriever unit context)
singularity_label_test(source_markers unit context)
singularity_label_test(findings unit document)
singularity_label_test(obligations unit document)
singularity_label_test(chronology unit document)
singularity_label_test(comparison unit document)
singularity_label_test(drafting unit document)
singularity_label_test(persistence_coordinator unit project)
singularity_label_test(actor_identity unit project)
singularity_label_test(work_product_actions unit project)
singularity_label_test(document_registry unit document)
singularity_label_test(entity_registry unit privacy)
singularity_label_test(entity_resolver unit privacy)
singularity_label_test(entity_curation unit privacy)
singularity_label_test(project_preprocessing unit privacy)
singularity_label_test(document_patch_validator unit document)
singularity_label_test(csv_extractor unit document)
singularity_label_test(pptx_extractor unit document)
singularity_label_test(xlsx_extractor unit document)
singularity_label_test(xlsx_number_format unit document)
singularity_label_test(xlsx_workbook_view unit document)
singularity_label_test(document_structural_reader unit document)
singularity_label_test(document_patch_tool unit document)
singularity_label_test(document_file_actions unit document)
singularity_label_test(file_system_watcher_reports unit platform)
singularity_label_test(mcp_caller_windows unit platform)
singularity_label_test(webview_com unit platform)
singularity_label_test(trash_file unit platform)
singularity_label_test(http_download unit platform)
singularity_label_test(web_view_data unit platform)
singularity_label_test(stale_runtime_reaper_windows unit platform)
singularity_label_test(document_source_resolver unit document)
singularity_label_test(chat_operation_setup integration chat)
singularity_label_test(chat_source_resolver integration chat)
singularity_label_test(chat_outbound_preview integration privacy)
singularity_label_test(capability_scoped_turn_contract integration context)
singularity_label_test(document_extractor unit document)
singularity_label_test(document_cache unit document)
singularity_label_test(document_chunk unit document)
singularity_label_test(docx_extractor unit document)
singularity_label_test(docx_render unit document)
singularity_label_test(pdf_extractor unit document)

# The protected computation protocol (DESIGN4, Papinian Cycle 4) in every build
# that reads operations: each target below compiles ProtectedOperations.cpp.
# The application and every build made from NATIVE_APP_SOURCES take the same
# files from cmake/BackendSources.cmake. A new target that compiles
# ProtectedOperations.cpp joins this list.
function(singularity_add_protected_computation target)
  if(TARGET ${target})
    target_sources(${target} PRIVATE ${SINGULARITY_PROTECTED_COMPUTATION_SOURCES})
    if(NOT target STREQUAL "protected_relations_tests")
      target_sources(${target} PRIVATE ${SINGULARITY_PROTECTED_COMPUTATION_POLICY_SOURCES})
    endif()
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json utf8proc)
  endif()
endfunction()
foreach(singularity_operations_target IN ITEMS
    papinian_guard_tests
    provider_protection_tests
    protected_relations_pipeline_tests
    fact_identity_tests
    protected_operations_pipeline_tests
    protected_operations_tests
    papinian_inventory_tests
    observability_forensic_gate_tests
    singularity_cli_offline
    singularity_cli
    papinian_provider_boundary_acceptance
    sources_outputs_live_acceptance
    papinian_native_benchmark
    papinian_memory_growth_diagnostic
    papinian_masking_measure
    computed_statements_tests
    semantic_algebra_tests
    protected_relations_tests
    work_product_actions_tests
    project_preprocessing_tests
    semantic_detection_store_tests
    direct_document_work_continuity_fresh_process_acceptance_tests
    matter_request_execution_tests
    chat_outbound_preview_tests
    chat_execution_tests
    chat_lifecycle_revision_history_tests
    chat_project_inspection_tests
    chat_lawyer_question_tests
    chat_kept_pause_tests
    chat_agent_authority_tests
    chat_local_only_tests
    project_entry_scale_tests
    external_research_tests
    capability_scoped_turn_contract_tests
    chat_execution_reload_acceptance_tests
    document_operations_acceptance_tests
    professional_artifact_action_xlsx_tests
    document_version_store_tests)
  singularity_add_protected_computation(${singularity_operations_target})
endforeach()
# ProtectedOperations.test.cpp runs the temporal family's cases (lane C).
if(TARGET protected_operations_tests)
  target_sources(protected_operations_tests PRIVATE
    "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/ProtectedOperations/TemporalOps.test.cpp")
endif()
# The table source, in every build that compiles MatterRequestExecution.cpp.
foreach(singularity_request_target IN ITEMS
    matter_request_execution_tests
    chat_outbound_preview_tests
    chat_execution_tests
    chat_lifecycle_revision_history_tests
    chat_project_inspection_tests
    chat_lawyer_question_tests
    chat_kept_pause_tests
    chat_agent_authority_tests
    chat_local_only_tests
    project_entry_scale_tests
    chat_execution_reload_acceptance_tests)
  if(TARGET ${singularity_request_target})
    target_sources(${singularity_request_target} PRIVATE ${SINGULARITY_PROTECTED_TABLE_SOURCE_SOURCES})
  endif()
endforeach()

# A computer's signatures (Platform/DeviceKey), the colleagues' computers the
# person here confirmed (Tool/DeviceTrust) and whose a colleague's message is
# (Chat/MessageProvenance): compiled into every build that compiles a source
# that uses them, signed with Monocypher.
set(SINGULARITY_TRUST_SOURCES
  "${NATIVE_APP_SOURCE_DIR}/Platform/DeviceKey/DeviceKey.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Tool/DeviceTrust/DeviceTrust.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Chat/MessageProvenance/MessageProvenance.cpp"
  # Who began an operation, signed into its Matter, and what this computer has
  # seen of a Matter's operations (Chat/OperationOrigin).
  "${NATIVE_APP_SOURCE_DIR}/Chat/OperationOrigin/OperationOrigin.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Tool/OperationAuthority/OperationAuthority.cpp"
  # What they rest on: this computer's settings, its person and the time.
  "${NATIVE_APP_SOURCE_DIR}/Platform/ActorIdentity/ActorIdentity.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Platform/AppSettings/AppSettings.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Platform/Sha256/Sha256.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Node/LegacyWorkspace/LegacyWorkspace.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/Policy/Policy.cpp"
  "${NATIVE_APP_SOURCE_DIR}/PapinianGuard/Taxonomy/Taxonomy.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Provider/ModelClassification/ModelClassification.cpp")
# Their tests, and the other tests defined from here on, are built only with
# the tests (BUILD_TESTING): the release builds - windows-cross among them,
# which runs no test - compile none of them (Beta 1 #176).
if(BUILD_TESTING)
  singularity_add_executable(device_key_tests
    Platform/DeviceKey/DeviceKey.test.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/Sha256/Sha256.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Provider/ModelClassification/ModelClassification.cpp
  )
  singularity_add_executable(message_provenance_tests
    Chat/MessageProvenance/MessageProvenance.test.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/Sha256/Sha256.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Provider/ModelClassification/ModelClassification.cpp
  )
  singularity_add_executable(operation_origin_tests
    Chat/OperationOrigin/OperationOrigin.test.cpp
    Platform/ActorIdentity/ActorIdentity.cpp
    Platform/AppSettings/AppSettings.cpp
    Platform/Sha256/Sha256.cpp
    Node/LegacyWorkspace/LegacyWorkspace.cpp
    PapinianGuard/Policy/Policy.cpp
    PapinianGuard/Taxonomy/Taxonomy.cpp
    Provider/ModelClassification/ModelClassification.cpp
  )
  foreach(singularity_trust_test IN ITEMS device_key_tests message_provenance_tests operation_origin_tests)
    target_include_directories(${singularity_trust_test} PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(${singularity_trust_test} PRIVATE nlohmann_json::nlohmann_json utf8proc)
    target_compile_definitions(${singularity_trust_test} PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
    if(MSVC)
      target_compile_options(${singularity_trust_test} PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(${singularity_trust_test} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endforeach()
  add_test(NAME device_key COMMAND device_key_tests)
  # What keeps those files to their owner on each platform (Platform/PrivateFile).
  singularity_add_executable(private_file_tests
    Platform/PrivateFile/PrivateFile.test.cpp
  )
  target_include_directories(private_file_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(private_file_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(private_file_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME private_file COMMAND private_file_tests)
  # Whole numbers of 128 bits on every compiler (Platform/Int128): the portable
  # type MSVC uses, held to GCC's and Clang's own __int128.
  singularity_add_executable(int128_tests
    Platform/Int128/Int128.test.cpp
  )
  target_include_directories(int128_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(int128_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(int128_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME int128 COMMAND int128_tests)
  add_test(NAME message_provenance COMMAND message_provenance_tests)
  add_test(NAME operation_origin COMMAND operation_origin_tests)
endif()
get_property(singularity_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(singularity_target IN LISTS singularity_all_targets)
  get_target_property(singularity_target_type ${singularity_target} TYPE)
  if(NOT singularity_target_type STREQUAL "EXECUTABLE")
    continue()
  endif()
  get_target_property(singularity_target_sources ${singularity_target} SOURCES)
  set(singularity_uses_trust FALSE)
  foreach(singularity_source IN LISTS singularity_target_sources)
    if(singularity_source MATCHES "/(ChatActions|ChatConversation|ChatExecution|MatterRequestExecution|SharedMatter|SharedMatterService|AgentProduction|MatterBridge|DeviceKey|DeviceTrust|MessageProvenance|OperationOrigin|AppUpdate)(\\.test)?\\.(cpp|mm)$")
      set(singularity_uses_trust TRUE)
      break()
    endif()
  endforeach()
  if(singularity_uses_trust)
    target_sources(${singularity_target} PRIVATE ${SINGULARITY_TRUST_SOURCES})
    target_link_libraries(${singularity_target} PRIVATE monocypher)
  endif()
endforeach()

# Review Tables (WorkProduct/ReviewTable): the same questions asked of many
# documents, run through the real request pipeline with only the model's
# replies scripted - built from exactly the sources the chat execution tests
# build from, with the table's own.
set(SINGULARITY_REVIEW_TABLE_SOURCES
  "${NATIVE_APP_SOURCE_DIR}/WorkProduct/ReviewTable/ReviewTable.cpp"
  "${NATIVE_APP_SOURCE_DIR}/WorkProduct/ReviewTableExport/ReviewTableExport.cpp"
  "${NATIVE_APP_SOURCE_DIR}/WorkProduct/Playbook/Playbook.cpp"
  "${NATIVE_APP_SOURCE_DIR}/WorkProduct/PlaybookChanges/PlaybookChanges.cpp")
if(TARGET chat_local_only_tests)
  get_target_property(singularity_review_sources chat_local_only_tests SOURCES)
  list(FILTER singularity_review_sources EXCLUDE REGEX "ChatLocalOnly\\.test\\.cpp$")
  add_executable(review_table_tests
    "${NATIVE_APP_SOURCE_DIR}/WorkProduct/ReviewTable/ReviewTable.test.cpp"
    ${singularity_review_sources} ${SINGULARITY_REVIEW_TABLE_SOURCES})
  foreach(singularity_review_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
      LINK_LIBRARIES CXX_STANDARD CXX_STANDARD_REQUIRED)
    get_target_property(singularity_review_value chat_local_only_tests ${singularity_review_property})
    if(singularity_review_value)
      set_target_properties(review_table_tests PROPERTIES ${singularity_review_property} "${singularity_review_value}")
    endif()
  endforeach()
  add_test(NAME review_table COMMAND review_table_tests)
endif()

# An agent that checks a Matter's contracts against the firm's Playbook, end
# to end (Agent/AgentProduction): the production runner, the real Review Table,
# the real Playbook changes and the real request pipeline, with only the
# model's replies scripted - built from exactly the Review Table test's
# sources, with the agent's own.
set(SINGULARITY_AGENT_PRODUCTION_SOURCES
  "${NATIVE_APP_SOURCE_DIR}/Agent/AgentActions/AgentActions.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AgentCard/AgentCard.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AgentProduction/AgentProduction.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/ColleagueBrief/ColleagueBrief.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AgentRoles/AgentRoles.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AgentRunner/AgentRunner.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AgentScheduler/AgentScheduler.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AgentStore/AgentStore.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AiAppChannel/AiAppChannel.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/AiAppConnections/AiAppConnections.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/DataRoomScan/DataRoomScan.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Agent/TextDiff/TextDiff.cpp"
  "${NATIVE_APP_SOURCE_DIR}/Matter/SharedMatter/SharedMatter.cpp")
if(TARGET review_table_tests)
  get_target_property(singularity_agent_flow_sources review_table_tests SOURCES)
  list(FILTER singularity_agent_flow_sources EXCLUDE REGEX "ReviewTable\\.test\\.cpp$")
  list(APPEND singularity_agent_flow_sources ${SINGULARITY_AGENT_PRODUCTION_SOURCES})
  list(REMOVE_DUPLICATES singularity_agent_flow_sources)
  add_executable(playbook_agent_tests
    "${NATIVE_APP_SOURCE_DIR}/Agent/AgentProduction/AgentProduction.test.cpp"
    ${singularity_agent_flow_sources})
  foreach(singularity_agent_flow_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
      LINK_LIBRARIES CXX_STANDARD CXX_STANDARD_REQUIRED)
    get_target_property(singularity_agent_flow_value review_table_tests ${singularity_agent_flow_property})
    if(singularity_agent_flow_value)
      set_target_properties(playbook_agent_tests PROPERTIES ${singularity_agent_flow_property} "${singularity_agent_flow_value}")
    endif()
  endforeach()
  add_test(NAME playbook_agent COMMAND playbook_agent_tests)
  singularity_label_test(playbook_agent acceptance chat)
endif()
include(cmake/DocumentReviewTests.cmake)

if(BUILD_TESTING)
  # Finding the passage a restored quote means (Platform/WordAlignment).
  singularity_add_executable(word_alignment_tests Platform/WordAlignment/WordAlignment.test.cpp)
  target_include_directories(word_alignment_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  add_test(NAME word_alignment COMMAND word_alignment_tests)

  # Identifiers and tokens from the system's secure random source (Platform/SecureRandom).
  singularity_add_executable(secure_random_tests Platform/SecureRandom/SecureRandom.test.cpp)
  target_include_directories(secure_random_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  add_test(NAME secure_random COMMAND secure_random_tests)
  singularity_label_test(secure_random unit platform)

  # What the lawyer reads when a native call fails, and what never reaches them -
  # a path, a provider's reply (Bridge/NativeBridge/NativeError.hpp).
  singularity_add_executable(native_error_tests Bridge/NativeBridge/NativeError.test.cpp)
  target_include_directories(native_error_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(native_error_tests PRIVATE nlohmann_json::nlohmann_json utf8proc)
  add_test(NAME native_error COMMAND native_error_tests)
  singularity_label_test(native_error unit privacy)
endif()

# Every test runs as if on a computer that holds nothing of the person's: a
# home of its own under the build tree, made empty before each run (the stages
# of one test share theirs), so no test can read a real profile, Project or
# provider credential. Its temporary folder is inside that home too, so two
# build trees - two worktrees on one computer - can run their tests at once
# without a fixture of one overwriting the other's. A live provider acceptance
# never runs here; it runs on purpose through
# deploy/scripts/live_provider_acceptance.py, with one credential in a
# throwaway profile.
get_property(singularity_registered_tests DIRECTORY PROPERTY TESTS)
# The homes are under the build tree unless a build names another place. The
# Linux container names its own disk (deploy/scripts/singularity.py): a Mac
# shares the source folder with the container through virtiofs, which refuses
# libstdc++'s std::filesystem::copy_file and carries no Unix socket, so tests
# that copy a file or run a private D-Bus would fail there for no fault of
# theirs.
set(SINGULARITY_TEST_HOMES_ROOT "${CMAKE_BINARY_DIR}/test-homes" CACHE PATH
    "Where each test's own home is made, emptied before every run.")
set(singularity_test_homes_root "${SINGULARITY_TEST_HOMES_ROOT}")
# The tests that run Papinian Guard's real model read it - only it, the
# hash-pinned catalog package - from where this computer installed it. No
# setting, Project or credential comes with it. Without it they say the model
# is not available.
set(SINGULARITY_PAPINIAN_MODEL_DIR "$ENV{HOME}/Library/Application Support/Singularity/Models/papinian-ner"
    CACHE PATH "Papinian Guard's installed model package, linked read-only into the homes of the tests that run it.")
set(singularity_tests_with_papinian beta1_operation_long_horizon_replay beta1_cli_journey)
set(singularity_test_homes_reset "file(REMOVE_RECURSE \"${singularity_test_homes_root}\")\n")
foreach(singularity_test IN LISTS singularity_registered_tests)
  string(REGEX REPLACE "_stage[0-9]+$" "" singularity_test_home "${singularity_test}")
  set(singularity_test_home "${singularity_test_homes_root}/${singularity_test_home}")
  string(APPEND singularity_test_homes_reset "file(MAKE_DIRECTORY \"${singularity_test_home}/tmp\")\n")
  if(APPLE AND singularity_test IN_LIST singularity_tests_with_papinian AND EXISTS "${SINGULARITY_PAPINIAN_MODEL_DIR}")
    set(singularity_test_models "${singularity_test_home}/Library/Application Support/Singularity/Models")
    string(APPEND singularity_test_homes_reset
      "file(MAKE_DIRECTORY \"${singularity_test_models}\")\n"
      "file(CREATE_LINK \"${SINGULARITY_PAPINIAN_MODEL_DIR}\" \"${singularity_test_models}/papinian-ner\" SYMBOLIC)\n")
  endif()
  set_property(TEST ${singularity_test} APPEND PROPERTY ENVIRONMENT_MODIFICATION
    "HOME=set:${singularity_test_home}" "CFFIXED_USER_HOME=set:${singularity_test_home}"
    "XDG_CONFIG_HOME=unset:" "XDG_DATA_HOME=unset:" "TMPDIR=set:${singularity_test_home}/tmp")
  # Windows finds the settings, the application data, the home and the
  # temporary folder through these instead (AppSettings.cpp, AppPaths.cpp,
  # GetTempPath): the same home stands in.
  if(WIN32)
    file(TO_NATIVE_PATH "${singularity_test_home}" singularity_native_test_home)
    set_property(TEST ${singularity_test} APPEND PROPERTY ENVIRONMENT_MODIFICATION
      "USERPROFILE=set:${singularity_native_test_home}"
      "APPDATA=set:${singularity_native_test_home}\\AppData\\Roaming"
      "LOCALAPPDATA=set:${singularity_native_test_home}\\AppData\\Local"
      "TMP=set:${singularity_native_test_home}\\tmp" "TEMP=set:${singularity_native_test_home}\\tmp")
  endif()
  set_property(TEST ${singularity_test} APPEND PROPERTY FIXTURES_REQUIRED singularity_test_homes)
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/reset-test-homes.cmake" "${singularity_test_homes_reset}")
add_test(NAME singularity_test_homes COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_BINARY_DIR}/reset-test-homes.cmake")
set_tests_properties(singularity_test_homes PROPERTIES FIXTURES_SETUP singularity_test_homes)

# Every colocated test is compiled by some target: a test file no target builds
# never runs, and no gate says so (NativeError.test.cpp ran nowhere from
# 2026-10-12 to 2026-10-21). The check is made on macOS, where every test but
# the few named here is built; elsewhere the Apple-only tests are rightly left
# out. A test built only off macOS is named here on purpose, with why.
set(singularity_tests_built_off_macos
  # PDFium, the PDF reader of Windows and Linux; macOS reads PDFs through PDFKit,
  # tested by PdfExtractor.test.mm.
  "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractor.test.cpp"
  # What Linux's own folder watcher (inotify) reports.
  "${NATIVE_APP_SOURCE_DIR}/Platform/FileSystemWatcher/FileSystemWatcherLinux.test.cpp"
  # Linux's Trash, the freedesktop.org Trash specification.
  "${NATIVE_APP_SOURCE_DIR}/Platform/TrashFile/TrashFile.test.cpp"
  # Linux's key store, the desktop keyring; macOS's is CredentialStore.test.mm.
  "${NATIVE_APP_SOURCE_DIR}/Provider/CredentialStore/CredentialStoreLinux.test.cpp"
  # How Windows names the AI app that started the helper: its Authenticode
  # signature (Agent/McpServer/McpCallerWindows.cpp).
  "${NATIVE_APP_SOURCE_DIR}/Agent/McpServer/McpCallerWindows.test.cpp"
  # WebView2's COM without WRL, for the Windows package's MinGW build.
  "${NATIVE_APP_SOURCE_DIR}/Platform/WebViewCom/WebViewCom.test.cpp"
  # The Windows download path (WinHTTP); macOS and Linux download otherwise.
  "${NATIVE_APP_SOURCE_DIR}/Platform/HttpClient/HttpClientWindows.test.cpp"
  # WebView2's data folder; WKWebView and WebKitGTK keep theirs locally.
  "${NATIVE_APP_SOURCE_DIR}/Platform/WebViewData/WebViewDataWindows.test.cpp"
  # Windows' reaping with real suspended stand-ins; the selection all three share
  # is StaleRuntimeReaper.test.cpp.
  "${NATIVE_APP_SOURCE_DIR}/Platform/StaleRuntimeReaper/StaleRuntimeReaperWindows.test.cpp"
  # Windows' one UTF-8 boundary for its wide-character APIs.
  "${NATIVE_APP_SOURCE_DIR}/Platform/WindowsText/WindowsText.test.cpp"
  # The NVIDIA driver, found at run time on Windows and Linux; a Mac has none.
  "${NATIVE_APP_SOURCE_DIR}/Platform/CudaDriver/CudaDriver.test.cpp")
if(BUILD_TESTING AND APPLE)
  file(GLOB_RECURSE singularity_test_files CONFIGURE_DEPENDS
    "${NATIVE_APP_SOURCE_DIR}/*.test.cpp" "${NATIVE_APP_SOURCE_DIR}/*.test.mm")
  list(REMOVE_ITEM singularity_test_files ${singularity_tests_built_off_macos})
  set(singularity_compiled_tests)
  get_property(singularity_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(singularity_target IN LISTS singularity_all_targets)
    get_target_property(singularity_target_sources ${singularity_target} SOURCES)
    if(NOT singularity_target_sources)
      continue()
    endif()
    foreach(singularity_source IN LISTS singularity_target_sources)
      if(singularity_source MATCHES "\\.test\\.(cpp|mm)$")
        get_filename_component(singularity_source "${singularity_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND singularity_compiled_tests "${singularity_source}")
      endif()
    endforeach()
  endforeach()
  set(singularity_orphan_tests)
  foreach(singularity_test_file IN LISTS singularity_test_files)
    if(NOT singularity_test_file IN_LIST singularity_compiled_tests)
      list(APPEND singularity_orphan_tests "${singularity_test_file}")
    endif()
  endforeach()
  if(singularity_orphan_tests)
    list(JOIN singularity_orphan_tests "\n  " singularity_orphan_words)
    message(FATAL_ERROR "These tests are compiled by no target, so they would never run:\n  ${singularity_orphan_words}\n"
      "Add each to a test target and register it with add_test.")
  endif()
endif()

# The Office connection (Office/, Platform/TrustStore, D-105) uses Mbed TLS
# for its listener, its certificate and Base64, and the system's certificate
# and Office registration services: linked into every target that builds its
# sources - the app, the targets that build the app's whole source list, and
# its own tests.
get_property(singularity_office_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(singularity_target IN LISTS singularity_office_targets)
  get_target_property(singularity_target_type ${singularity_target} TYPE)
  if(NOT singularity_target_type STREQUAL "EXECUTABLE")
    continue()
  endif()
  get_target_property(singularity_target_sources ${singularity_target} SOURCES)
  if(NOT singularity_target_sources)
    continue()
  endif()
  set(singularity_uses_office FALSE)
  foreach(singularity_source IN LISTS singularity_target_sources)
    if(singularity_source MATCHES "/(Office[A-Za-z]*|TrustStore)(\\.test)?\\.cpp$")
      set(singularity_uses_office TRUE)
      break()
    endif()
  endforeach()
  if(singularity_uses_office)
    target_link_libraries(${singularity_target} PRIVATE lm_mbedtls)
    if(WIN32)
      target_link_libraries(${singularity_target} PRIVATE crypt32 advapi32 ws2_32)
    elseif(APPLE)
      target_link_libraries(${singularity_target} PRIVATE "-framework Security" "-framework CoreFoundation")
    endif()
  endif()
endforeach()

# (Placed last in this file, so they see every program.)
get_property(singularity_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
# Every program that runs the Native Bridge keeps its pace in the background
# (Platform/BackgroundActivity, D-122), whichever source list it was built
# from: the bridge asks for it when it starts its services. On macOS each test
# program names its own system sources, so a list that predates this would
# otherwise leave the call unresolved.
if(APPLE)
  set(SINGULARITY_BACKGROUND_ACTIVITY_SOURCE
    "${NATIVE_APP_SOURCE_DIR}/Platform/BackgroundActivity/BackgroundActivityMacOS.mm")
elseif(WIN32)
  set(SINGULARITY_BACKGROUND_ACTIVITY_SOURCE
    "${NATIVE_APP_SOURCE_DIR}/Platform/BackgroundActivity/BackgroundActivityWindows.cpp")
else()
  set(SINGULARITY_BACKGROUND_ACTIVITY_SOURCE
    "${NATIVE_APP_SOURCE_DIR}/Platform/BackgroundActivity/BackgroundActivityLinux.cpp")
endif()
foreach(singularity_target IN LISTS singularity_all_targets)
  get_target_property(singularity_target_type ${singularity_target} TYPE)
  if(NOT singularity_target_type STREQUAL "EXECUTABLE")
    continue()
  endif()
  get_target_property(singularity_target_sources ${singularity_target} SOURCES)
  set(singularity_runs_bridge FALSE)
  set(singularity_has_background_activity FALSE)
  foreach(singularity_source IN LISTS singularity_target_sources)
    if(singularity_source MATCHES "/?NativeBridge/NativeBridge\\.cpp$")
      set(singularity_runs_bridge TRUE)
    endif()
    if(singularity_source MATCHES "BackgroundActivity(MacOS|Windows|Linux)\\.(mm|cpp)$")
      set(singularity_has_background_activity TRUE)
    endif()
  endforeach()
  if(singularity_runs_bridge AND NOT singularity_has_background_activity)
    target_sources(${singularity_target} PRIVATE "${SINGULARITY_BACKGROUND_ACTIVITY_SOURCE}")
    if(APPLE)
      target_link_libraries(${singularity_target} PRIVATE "-framework Foundation")
    endif()
  endif()
endforeach()

# Every program that seals what the firm's computers send each other links
# Monocypher (Matter/CoordinationSeal, D-123), whichever source list brought
# the seal in.
foreach(singularity_target IN LISTS singularity_all_targets)
  get_target_property(singularity_target_type ${singularity_target} TYPE)
  if(NOT singularity_target_type STREQUAL "EXECUTABLE")
    continue()
  endif()
  get_target_property(singularity_target_sources ${singularity_target} SOURCES)
  foreach(singularity_source IN LISTS singularity_target_sources)
    if(singularity_source MATCHES "CoordinationSeal/CoordinationSeal\\.cpp$")
      target_link_libraries(${singularity_target} PRIVATE monocypher)
      break()
    endif()
  endforeach()
endforeach()

# Every program that makes model calls keeps them in a design partner's work
# log (Node/WorkLog, D-257) - recorded only where the app names the log's
# folder - whichever source list brought the calls in; the account bridge
# turns it on and off from the seat's own record. A colleague's calls come
# from the review library (lm_document_review), which, like its other
# domains, leaves the log to whoever links it: a program that links it, or
# that the review joins once this directory is read (the bridges, see
# cmake/DocumentIntelligence.cmake), compiles the log too.
foreach(singularity_target IN LISTS singularity_all_targets)
  get_target_property(singularity_target_type ${singularity_target} TYPE)
  if(NOT singularity_target_type STREQUAL "EXECUTABLE")
    continue()
  endif()
  get_target_property(singularity_target_sources ${singularity_target} SOURCES)
  get_target_property(singularity_target_links ${singularity_target} LINK_LIBRARIES)
  set(singularity_makes_calls FALSE)
  set(singularity_has_work_log FALSE)
  if(singularity_target_links MATCHES "(^|;)lm_document_review(;|$)")
    set(singularity_makes_calls TRUE)
  endif()
  foreach(singularity_source IN LISTS singularity_target_sources)
    if(singularity_source MATCHES "(ProtectedInference/ProtectedInference|ColleagueWork/ColleagueWork|AccountBridge/AccountBridge|NativeBridge/NativeBridge|DocumentReviewBridge/DocumentReviewBridge)\\.cpp$")
      set(singularity_makes_calls TRUE)
    elseif(singularity_source MATCHES "WorkLog/WorkLog\\.cpp$")
      set(singularity_has_work_log TRUE)
    endif()
  endforeach()
  if(singularity_makes_calls AND NOT singularity_has_work_log)
    target_sources(${singularity_target} PRIVATE "${NATIVE_APP_SOURCE_DIR}/Node/WorkLog/WorkLog.cpp")
  endif()
endforeach()

# On macOS every program that reads PDFs with PDFKit also writes their copies
# with PDFium (D-115; PdfExtractorPdfium.cpp builds only that there) and finds
# libpdfium.dylib through its build RPATH; the app carries it in Frameworks.
# Last in this file, so it sees every program, those made from others' lists
# included.
if(APPLE)
  get_property(singularity_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(singularity_target IN LISTS singularity_all_targets)
    get_target_property(singularity_target_type ${singularity_target} TYPE)
    if(NOT singularity_target_type STREQUAL "EXECUTABLE")
      continue()
    endif()
    get_target_property(singularity_target_sources ${singularity_target} SOURCES)
    if(NOT singularity_target_sources)
      continue()
    endif()
    set(singularity_reads_with_pdfkit FALSE)
    set(singularity_writes_with_pdfium FALSE)
    foreach(singularity_source IN LISTS singularity_target_sources)
      if(singularity_source MATCHES "PdfExtractorMacOS\\.mm$")
        set(singularity_reads_with_pdfkit TRUE)
      endif()
      if(singularity_source MATCHES "PdfExtractorPdfium\\.cpp$")
        set(singularity_writes_with_pdfium TRUE)
      endif()
    endforeach()
    if(NOT singularity_reads_with_pdfkit AND NOT singularity_writes_with_pdfium)
      continue()
    endif()
    if(singularity_reads_with_pdfkit AND NOT singularity_writes_with_pdfium)
      target_sources(${singularity_target} PRIVATE
        "${NATIVE_APP_SOURCE_DIR}/Document/PdfExtractor/PdfExtractorPdfium.cpp")
    endif()
    target_link_libraries(${singularity_target} PRIVATE native_app_pdfium)
    set_property(TARGET ${singularity_target} APPEND PROPERTY BUILD_RPATH "${NATIVE_APP_PDFIUM_DIR}")
  endforeach()
endif()
