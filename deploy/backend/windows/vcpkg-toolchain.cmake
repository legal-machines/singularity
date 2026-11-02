# The Windows presets' toolchain: vcpkg's own, from wherever this computer
# keeps vcpkg. It is looked for in this order:
#   1. VCPKG_INSTALLATION_ROOT, as deploy/README.md names it;
#   2. VCPKG_ROOT, which Visual Studio's developer prompt sets;
#   3. a vcpkg on PATH;
#   4. the copy Visual Studio 2022 installs with its C++ workload (vswhere).
# When none is found the configure stops, saying what to set. The folder found
# is kept in VCPKG_INSTALLATION_ROOT for this run, since CMake reads a
# toolchain file again for each check it compiles.

set(singularity_vcpkg_root "")
foreach(singularity_candidate "$ENV{VCPKG_INSTALLATION_ROOT}" "$ENV{VCPKG_ROOT}")
  if(NOT singularity_vcpkg_root AND singularity_candidate
      AND EXISTS "${singularity_candidate}/scripts/buildsystems/vcpkg.cmake")
    set(singularity_vcpkg_root "${singularity_candidate}")
  endif()
endforeach()

if(NOT singularity_vcpkg_root)
  find_program(singularity_vcpkg_program NAMES vcpkg NO_CACHE)
  if(singularity_vcpkg_program)
    get_filename_component(singularity_candidate "${singularity_vcpkg_program}" DIRECTORY)
    if(EXISTS "${singularity_candidate}/scripts/buildsystems/vcpkg.cmake")
      set(singularity_vcpkg_root "${singularity_candidate}")
    endif()
  endif()
endif()

if(NOT singularity_vcpkg_root)
  set(singularity_vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
  if(EXISTS "${singularity_vswhere}")
    execute_process(
      COMMAND "${singularity_vswhere}" -latest -products * -property installationPath
      OUTPUT_VARIABLE singularity_visual_studio OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(singularity_visual_studio AND EXISTS "${singularity_visual_studio}/VC/vcpkg/scripts/buildsystems/vcpkg.cmake")
      set(singularity_vcpkg_root "${singularity_visual_studio}/VC/vcpkg")
    endif()
  endif()
endif()

if(NOT singularity_vcpkg_root)
  message(FATAL_ERROR "vcpkg was not found: set VCPKG_INSTALLATION_ROOT to the folder vcpkg is in "
    "(for example C:\\vcpkg), or install Visual Studio 2022's C++ workload, which brings vcpkg.")
endif()

file(TO_CMAKE_PATH "${singularity_vcpkg_root}" singularity_vcpkg_root)
if(NOT "$ENV{VCPKG_INSTALLATION_ROOT}" STREQUAL "${singularity_vcpkg_root}")
  message(STATUS "vcpkg: ${singularity_vcpkg_root}")
endif()
set(ENV{VCPKG_INSTALLATION_ROOT} "${singularity_vcpkg_root}")
include("${singularity_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
