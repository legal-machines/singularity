# Run by CPack before it makes the Windows package (CPACK_PRE_BUILD_SCRIPTS),
# in the MinGW cross-build: every program and library at the top of the staged
# package may import only libraries the package ships beside it, or Windows'
# own. One that imports anything else - a zlib1.dll the build machine happened
# to have, say - does not start on a clean Windows, so the package is refused
# instead, naming what is missing.
#
#   CPACK_TEMPORARY_INSTALL_DIRECTORY  the staged tree (CPack sets it)
#   CPACK_SINGULARITY_OBJDUMP         the objdump that reads PE imports
#
# llama.cpp/ is local inference's runtime folder: its optional backends name libraries
# a computer may lack on purpose (a GPU's runtime), and Singularity loads them only
# where they are, so it is left out.

# A script CPack runs takes no policy from the project: IN_LIST needs it.
cmake_policy(VERSION 3.19)

set(windows_libraries
  advapi32 bcrypt bcryptprimitives cfgmgr32 combase comctl32 comdlg32 crypt32 d3d11 d3d12 d3dcompiler_47 dbghelp
  dnsapi dwmapi dxgi gdi32 gdiplus imm32 iphlpapi kernel32 ktmw32 msimg32 msvcp_win msvcrt ncrypt netapi32 normaliz
  ntdll ole32 oleacc oleaut32 opengl32 powrprof propsys psapi rpcrt4 secur32 setupapi shcore shell32 shlwapi
  ucrtbase user32 userenv uxtheme version winhttp wininet winmm winspool wintrust ws2_32 wsock32)

if(NOT CPACK_SINGULARITY_OBJDUMP OR NOT EXISTS "${CPACK_SINGULARITY_OBJDUMP}")
  message(FATAL_ERROR "The Windows package's imports cannot be checked: no objdump (CPACK_SINGULARITY_OBJDUMP).")
endif()

set(staged "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
file(GLOB shipped LIST_DIRECTORIES false RELATIVE "${staged}" "${staged}/*.dll")
string(TOLOWER "${shipped}" shipped)
file(GLOB programs LIST_DIRECTORIES false "${staged}/*.exe" "${staged}/*.dll")

# Microsoft's Visual C++ runtime counts as Windows' own once the package
# carries Microsoft's redistributable, which its installer runs
# (CPACK_NSIS_EXTRA_INSTALL_COMMANDS). The redistributable itself is
# Microsoft's installer, not read here.
set(redistributable "${staged}/VC_redist.x64.exe")
if(EXISTS "${redistributable}")
  list(APPEND windows_libraries concrt140 msvcp140 msvcp140_1 msvcp140_2 msvcp140_atomic_wait msvcp140_codecvt_ids
    vccorlib140 vcomp140 vcruntime140 vcruntime140_1)
  list(REMOVE_ITEM programs "${redistributable}")
endif()
if(NOT programs)
  message(FATAL_ERROR "The Windows package's imports cannot be checked: nothing is staged in ${staged}.")
endif()

set(missing)
foreach(program IN LISTS programs)
  execute_process(COMMAND "${CPACK_SINGULARITY_OBJDUMP}" -p "${program}"
    OUTPUT_VARIABLE dump RESULT_VARIABLE dumped ERROR_QUIET)
  if(NOT dumped EQUAL 0)
    message(FATAL_ERROR "The Windows package's imports cannot be checked: objdump cannot read ${program}.")
  endif()
  string(REGEX MATCHALL "DLL Name: [^\r\n]+" imports "${dump}")
  get_filename_component(program_name "${program}" NAME)
  foreach(import IN LISTS imports)
    string(REGEX REPLACE "^DLL Name: " "" library "${import}")
    string(STRIP "${library}" library)
    string(TOLOWER "${library}" library)
    string(REGEX REPLACE "\\.dll$" "" library_base "${library}")
    if(library IN_LIST shipped OR library_base IN_LIST windows_libraries OR library MATCHES "^api-ms-win-")
      continue()
    endif()
    list(APPEND missing "${program_name} imports ${library}")
  endforeach()
endforeach()

if(missing)
  list(REMOVE_DUPLICATES missing)
  list(JOIN missing "\n  " named)
  message(FATAL_ERROR "The Windows package would not start on a clean Windows. Neither the package nor Windows "
    "has what these import:\n  ${named}\nLink it in, ship it beside the program, or name it here if Windows has it.")
endif()
message(STATUS "Windows package imports: every program finds what it imports in the package or in Windows.")
