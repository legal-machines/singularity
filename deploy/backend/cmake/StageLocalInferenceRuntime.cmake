# Gathers local inference's runtime from its llama.cpp build into one flat
# folder: llama-server, each library it loads, found transitively from what
# the server and its libraries were linked against, and any EXTRA_FILES
# ("|"- or ";"-separated, cuBLAS on Windows). Each library is one real file
# under the name it is loaded by (libllama.0.dylib, libllama.so.0), so no copy
# of the folder depends on symbolic links surviving it, and no unused version
# name is shipped. The folder is rebuilt whole, so nothing stale outlives a
# change.
#
#   cmake -DBUILD_DIR=<llama.cpp build> -DDESTINATION=<folder>
#         [-DEXTRA_FILES=<a|b>] -P StageLocalInferenceRuntime.cmake
cmake_minimum_required(VERSION 3.24)

set(_outputs "")
foreach(_candidate IN ITEMS "${BUILD_DIR}/bin/Release" "${BUILD_DIR}/bin")
  if(EXISTS "${_candidate}/llama-server" OR EXISTS "${_candidate}/llama-server.exe")
    set(_outputs "${_candidate}")
    break()
  endif()
endforeach()
if(NOT _outputs)
  message(FATAL_ERROR "Local inference: no llama-server was built under ${BUILD_DIR}/bin")
endif()
if(EXISTS "${_outputs}/llama-server.exe")
  set(_server "${_outputs}/llama-server.exe")
else()
  set(_server "${_outputs}/llama-server")
endif()

file(REMOVE_RECURSE "${DESTINATION}")
file(MAKE_DIRECTORY "${DESTINATION}")
get_filename_component(_server_name "${_server}" NAME)
file(COPY_FILE "${_server}" "${DESTINATION}/${_server_name}")
set(_count 1)

if(CMAKE_HOST_WIN32)
  # Windows links llama.cpp into the server itself (LocalInferenceRuntime.cmake):
  # only a DLL the build left beside it goes with it.
  file(GLOB _libraries LIST_DIRECTORIES false "${_outputs}/*.dll")
else()
  # The libraries the server loads from its own build, by the names it loads
  # them by; anything from the system stays the system's.
  string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" _outputs_pattern "${_outputs}")
  file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${_server}"
    RESOLVED_DEPENDENCIES_VAR _libraries
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    DIRECTORIES "${_outputs}"
    POST_INCLUDE_REGEXES "^${_outputs_pattern}/"
    POST_EXCLUDE_REGEXES ".*")
  foreach(_name IN LISTS _unresolved)
    if(_name MATCHES "(ggml|llama|mtmd)")
      message(FATAL_ERROR "Local inference: llama-server needs ${_name}, which its build did not make")
    endif()
  endforeach()
endif()

if(EXTRA_FILES)
  string(REPLACE "|" ";" _extra "${EXTRA_FILES}")
  list(APPEND _libraries ${_extra})
endif()
# file(COPY_FILE) writes each file's content under the given name, following
# a symbolic link rather than copying it.
foreach(_library IN LISTS _libraries)
  get_filename_component(_name "${_library}" NAME)
  file(COPY_FILE "${_library}" "${DESTINATION}/${_name}")
  math(EXPR _count "${_count} + 1")
endforeach()
message(STATUS "Local inference runtime: ${_count} files in ${DESTINATION}")
