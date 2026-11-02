include_guard(GLOBAL)

# Local inference runs a local model in llama-server, a process Singularity
# starts and talks to over loopback HTTP. This module builds that server and
# the libraries it loads from the pinned, patched llama.cpp source
# (dependencies/llama.cpp) on every platform, as a build of its own: it is
# always Release, so a Debug Singularity still runs models at full speed, and
# none of llama.cpp's options reach Singularity's own targets. What it
# produces is one flat folder, NATIVE_APP_LLAMA_CPP_RUNTIME_DIR, which the
# application's bundling copies beside the executable as llama.cpp/.
#
# Which accelerator the runtime is compiled for is a property of the build,
# not of the operating system's name: auto selects Metal on Apple silicon,
# CUDA where a working CUDA toolkit is installed (Windows or Linux) and the
# CPU otherwise. NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR carries the choice.

set(NATIVE_APP_LLAMA_CPP_BUILD "b10516")
set(NATIVE_APP_LLAMA_CPP_SHA256 "bd13fa381488aa59ae4081aca24d368a956ad6e393c54931510a94d240590e33")
set(LOCAL_INFERENCE_ACCELERATOR "auto" CACHE STRING
  "Accelerator compiled into local inference's llama.cpp runtime: auto, metal, cuda, cpu or none")
set_property(CACHE LOCAL_INFERENCE_ACCELERATOR PROPERTY STRINGS auto metal cuda cpu none)
# OFF tunes the runtime to this computer (its CPU instruction set and its
# GPU's CUDA architecture): fast to build, right for development. ON builds
# what a package needs (deploy/scripts/build-*): an x86-64-v3 CPU baseline
# (AVX2, FMA, F16C, BMI2) and every CUDA architecture the toolkit supports.
# Apple silicon keeps llama.cpp's own settings either way.
option(LOCAL_INFERENCE_PORTABLE
  "Build local inference's runtime for distribution instead of for this computer" OFF)

string(TOLOWER "${LOCAL_INFERENCE_ACCELERATOR}" _local_inference_requested)
set(_local_inference_apple_silicon OFF)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(_local_inference_apple_silicon ON)
endif()

if(_local_inference_requested STREQUAL "auto")
  if(_local_inference_apple_silicon)
    set(_local_inference_resolved metal)
  elseif(APPLE)
    # Intel Macs have never had a local runtime; none is added untested.
    set(_local_inference_resolved none)
  else()
    include(CheckLanguage)
    check_language(CUDA)
    find_package(CUDAToolkit QUIET)
    if(CMAKE_CUDA_COMPILER AND CUDAToolkit_FOUND)
      set(_local_inference_resolved cuda)
    else()
      if(CUDAToolkit_FOUND)
        message(STATUS "Local inference: a CUDA toolkit is installed, but its compiler cannot "
          "build with this toolchain; building the CPU runtime. "
          "-DLOCAL_INFERENCE_ACCELERATOR=cuda makes this an error instead.")
      endif()
      set(_local_inference_resolved cpu)
    endif()
  endif()
elseif(_local_inference_requested MATCHES "^(metal|cuda|cpu|none)$")
  set(_local_inference_resolved "${_local_inference_requested}")
else()
  message(FATAL_ERROR "Unsupported LOCAL_INFERENCE_ACCELERATOR=${LOCAL_INFERENCE_ACCELERATOR}")
endif()
if(_local_inference_resolved STREQUAL "metal" AND NOT APPLE)
  message(FATAL_ERROR "LOCAL_INFERENCE_ACCELERATOR=metal needs an Apple platform")
endif()
if(_local_inference_resolved STREQUAL "cuda")
  find_package(CUDAToolkit REQUIRED)
endif()
set(NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR "${_local_inference_resolved}")
message(STATUS "Local inference: llama.cpp ${NATIVE_APP_LLAMA_CPP_BUILD}, "
  "accelerator=${NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR}, portable=${LOCAL_INFERENCE_PORTABLE}")

set(NATIVE_APP_LLAMA_CPP_RUNTIME_DIR "${CMAKE_BINARY_DIR}/local-inference/runtime")

if(NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR STREQUAL "none")
  # Nothing to build: an empty folder is bundled, and planning finds no runtime.
  file(MAKE_DIRECTORY "${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}")
  add_custom_target(local_inference_runtime)
  return()
endif()

# GNU patch ships with macOS, every Linux build environment and Git for
# Windows: one tool keeps the patches' meaning identical everywhere.
find_program(NATIVE_APP_PATCH_EXECUTABLE NAMES patch
  HINTS "$ENV{ProgramFiles}/Git/usr/bin" "$ENV{ProgramW6432}/Git/usr/bin"
  REQUIRED)

set(_local_inference_metal OFF)
set(_local_inference_cuda OFF)
if(NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR STREQUAL "metal")
  set(_local_inference_metal ON)
elseif(NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR STREQUAL "cuda")
  set(_local_inference_cuda ON)
endif()

set(_local_inference_args
  -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_PROJECT_INCLUDE_BEFORE=${CMAKE_CURRENT_LIST_DIR}/LocalInferenceGitCeiling.cmake"
  -DLLAMA_BUILD_TESTS=OFF
  -DLLAMA_BUILD_EXAMPLES=OFF
  -DLLAMA_BUILD_COMMON=ON
  -DLLAMA_BUILD_SERVER=ON
  -DLLAMA_BUILD_TOOLS=ON
  # The server's web UI and desktop app are never shipped (local inference
  # starts it with --no-webui), and the prebuilt UI would be fetched,
  # unpinned, on every build.
  -DLLAMA_BUILD_APP=OFF
  -DLLAMA_BUILD_UI=OFF
  -DLLAMA_USE_PREBUILT_UI=OFF
  -DLLAMA_OPENSSL=OFF
  # Only accelerators the planner can plan onto are compiled in.
  -DGGML_METAL=${_local_inference_metal}
  -DGGML_CUDA=${_local_inference_cuda}
)

if(NOT _local_inference_apple_silicon AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  if(LOCAL_INFERENCE_PORTABLE)
    list(APPEND _local_inference_args -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON
      -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON)
  else()
    list(APPEND _local_inference_args -DGGML_NATIVE=ON)
  endif()
endif()
if(NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR STREQUAL "cuda")
  if(LOCAL_INFERENCE_PORTABLE)
    # Turing (7.5) is the oldest architecture CUDA 13 can target; the virtual
    # entries keep newer GPUs working through the driver's JIT.
    # "|" separates the list for the separate build (LIST_SEPARATOR below).
    list(APPEND _local_inference_args "-DCMAKE_CUDA_ARCHITECTURES=75-virtual|80-virtual|86-real|89-real|120a-real|120-virtual")
  else()
    list(APPEND _local_inference_args -DCMAKE_CUDA_ARCHITECTURES=native)
  endif()
  if(CMAKE_CUDA_COMPILER)
    list(APPEND _local_inference_args "-DCMAKE_CUDA_COMPILER=${CMAKE_CUDA_COMPILER}")
  endif()
endif()

if(WIN32)
  # The patch adds cross-library ggml symbols that are not marked for DLL
  # export, so Windows links the libraries into the server itself.
  list(APPEND _local_inference_args -DBUILD_SHARED_LIBS=OFF)
  if(MSVC)
    # MSVC's OpenMP runtime exists only as a DLL; ggml's own thread pool
    # keeps the runtime free of it. The patched source selects its C++20
    # UTF-8 literal bridge from __cplusplus, which MSVC reports truthfully
    # only with /Zc:__cplusplus.
    list(APPEND _local_inference_args -DGGML_OPENMP=OFF "-DCMAKE_CXX_FLAGS_INIT=/Zc:__cplusplus")
  else()
    # A MinGW cross build names its compilers and target system explicitly.
    list(APPEND _local_inference_args "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    if(CMAKE_RC_COMPILER)
      list(APPEND _local_inference_args "-DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}")
    endif()
    if(CMAKE_FIND_ROOT_PATH)
      list(APPEND _local_inference_args "-DCMAKE_FIND_ROOT_PATH=${CMAKE_FIND_ROOT_PATH}")
    endif()
  endif()
else()
  list(APPEND _local_inference_args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON)
  if(APPLE)
    # llama-server and its libraries find each other in their own folder,
    # wherever the application is, never in the build tree that made them.
    list(APPEND _local_inference_args "-DCMAKE_INSTALL_RPATH=@loader_path" -DCMAKE_MACOSX_RPATH=ON
      "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
      "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  else()
    list(APPEND _local_inference_args "-DCMAKE_INSTALL_RPATH=\$ORIGIN" -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON)
  endif()
  if(CMAKE_C_COMPILER)
    list(APPEND _local_inference_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
  endif()
  if(CMAKE_CXX_COMPILER)
    list(APPEND _local_inference_args "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
  endif()
endif()

# The CUDA libraries llama-server loads beside it on Windows (the CUDA
# runtime itself is linked statically).
set(_local_inference_extra_files "")
if(WIN32 AND NATIVE_APP_LOCAL_INFERENCE_ACCELERATOR STREQUAL "cuda")
  file(GLOB _local_inference_cuda_libraries
    "${CUDAToolkit_BIN_DIR}/cublas64_*.dll" "${CUDAToolkit_BIN_DIR}/cublasLt64_*.dll"
    "${CUDAToolkit_BIN_DIR}/x64/cublas64_*.dll" "${CUDAToolkit_BIN_DIR}/x64/cublasLt64_*.dll")
  if(NOT _local_inference_cuda_libraries)
    message(FATAL_ERROR "Local inference: cuBLAS was not found beside ${CUDAToolkit_BIN_DIR}")
  endif()
  string(REPLACE ";" "|" _local_inference_extra_files "${_local_inference_cuda_libraries}")
endif()

include(ExternalProject)
set(_local_inference_patches "${NATIVE_APP_ROOT}/dependencies/llama.cpp")
ExternalProject_Add(local_inference_runtime
  PREFIX "${CMAKE_BINARY_DIR}/local-inference/llama.cpp"
  URL "https://github.com/ggml-org/llama.cpp/archive/refs/tags/${NATIVE_APP_LLAMA_CPP_BUILD}.tar.gz"
  URL_HASH "SHA256=${NATIVE_APP_LLAMA_CPP_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PATCH_COMMAND "${NATIVE_APP_PATCH_EXECUTABLE}" -p1 --forward --input
      "${_local_inference_patches}/b10516-moe-expert-cache.patch"
    COMMAND "${NATIVE_APP_PATCH_EXECUTABLE}" -p1 --forward --input
      "${_local_inference_patches}/b10516-moe-route-trace.patch"
  LIST_SEPARATOR |
  CMAKE_ARGS ${_local_inference_args}
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target llama-server --parallel
  INSTALL_COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=<BINARY_DIR>"
    "-DDESTINATION=${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}"
    "-DEXTRA_FILES=${_local_inference_extra_files}"
    -P "${CMAKE_CURRENT_LIST_DIR}/StageLocalInferenceRuntime.cmake"
  BUILD_BYPRODUCTS "${NATIVE_APP_LLAMA_CPP_RUNTIME_DIR}/llama-server${CMAKE_EXECUTABLE_SUFFIX}"
  LOG_DOWNLOAD ON
  LOG_CONFIGURE ON
  LOG_BUILD ON
  LOG_INSTALL ON
  LOG_OUTPUT_ON_FAILURE ON
)
