# Makes DESTINATION an exact copy of SOURCE without rewriting what is already
# there: file(COPY) skips a file whose timestamp already matches, and files
# SOURCE no longer has are removed, so a stale runtime library never outlives
# the closure it came from. For runtime closures too large to recopy on every
# build (the CUDA llama.cpp closure is over half a gigabyte).
#
#   cmake -DSOURCE=<directory> -DDESTINATION=<directory> -P MirrorDirectory.cmake
cmake_minimum_required(VERSION 3.24)
if(NOT IS_DIRECTORY "${SOURCE}")
  message(FATAL_ERROR "MirrorDirectory: ${SOURCE} is not a directory")
endif()
file(MAKE_DIRECTORY "${DESTINATION}")
file(GLOB_RECURSE source_files LIST_DIRECTORIES false RELATIVE "${SOURCE}" "${SOURCE}/*")
file(GLOB_RECURSE destination_files LIST_DIRECTORIES false RELATIVE "${DESTINATION}" "${DESTINATION}/*")
foreach(relative IN LISTS destination_files)
  if(NOT relative IN_LIST source_files)
    file(REMOVE "${DESTINATION}/${relative}")
  endif()
endforeach()
file(COPY "${SOURCE}/" DESTINATION "${DESTINATION}")
