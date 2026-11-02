# Run by CPack after it has staged the application and before it makes the
# disk image (CPACK_PRE_BUILD_SCRIPTS). CPack's install step rewrites the
# executable's library paths, which breaks the signature the build gave it, so
# the staged application is signed again here - its nested code first, then
# the bundle - and the signature is verified: the disk image never carries an
# application macOS refuses to open.
#
# With a Developer ID identity (NATIVE_APP_RELEASE_SIGN_IDENTITY) the
# application is signed for distribution: hardened runtime and a secure
# timestamp. Notarization and stapling of the disk image follow outside CPack
# and need the Legal Machines notary credentials.

set(_identity "${CPACK_LM_SIGN_IDENTITY}")
if(_identity STREQUAL "")
  set(_identity "-")
endif()
set(_options --force --sign "${_identity}")
if(CPACK_LM_SIGN_FOR_DISTRIBUTION)
  list(APPEND _options --options runtime --timestamp)
endif()

file(GLOB _apps LIST_DIRECTORIES true "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*.app")
if(NOT _apps)
  message(FATAL_ERROR "No application was staged in ${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
endif()

foreach(_app IN LISTS _apps)
  get_filename_component(_name "${_app}" NAME_WE)
  # Nested code, inside out: libraries, the local inference runtime, helpers.
  file(GLOB_RECURSE _nested LIST_DIRECTORIES false
    "${_app}/Contents/Frameworks/*"
    "${_app}/Contents/Resources/llama.cpp/*")
  file(GLOB _helpers LIST_DIRECTORIES false "${_app}/Contents/MacOS/*")
  list(APPEND _nested ${_helpers})
  foreach(_file IN LISTS _nested)
    if(IS_SYMLINK "${_file}")
      continue()
    endif()
    get_filename_component(_file_name "${_file}" NAME)
    if("${_file}" STREQUAL "${_app}/Contents/MacOS/${_name}")
      continue()
    endif()
    execute_process(COMMAND /usr/bin/file -b "${_file}" OUTPUT_VARIABLE _kind OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _kind MATCHES "Mach-O")
      continue()
    endif()
    execute_process(COMMAND /usr/bin/codesign ${_options} "${_file}" RESULT_VARIABLE _signed ERROR_VARIABLE _error)
    if(NOT _signed EQUAL 0)
      message(FATAL_ERROR "Could not sign ${_file_name}: ${_error}")
    endif()
  endforeach()
  execute_process(COMMAND /usr/bin/codesign ${_options} "${_app}" RESULT_VARIABLE _signed ERROR_VARIABLE _error)
  if(NOT _signed EQUAL 0)
    message(FATAL_ERROR "Could not sign ${_app}: ${_error}")
  endif()
  execute_process(COMMAND /usr/bin/codesign --verify --deep --strict "${_app}" RESULT_VARIABLE _valid ERROR_VARIABLE _error)
  if(NOT _valid EQUAL 0)
    message(FATAL_ERROR "The staged application's signature does not verify: ${_error}")
  endif()
  message(STATUS "Signed ${_app} (${_identity})")
endforeach()
