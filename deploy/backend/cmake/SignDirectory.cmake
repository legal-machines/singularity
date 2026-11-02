# Signs each Mach-O file in a folder that is not a symbolic link, the
# libraries first and the programs after them, with one identity ("-" signs
# ad hoc). A folder the app carries whole - llama-server and the libraries it
# loads from beside itself - is signed this way: codesign --deep signs nothing
# beside a plain executable.
#
#   cmake -DDIRECTORY=<folder> -DIDENTITY=<identity> -P SignDirectory.cmake
if(NOT DEFINED DIRECTORY OR NOT DEFINED IDENTITY)
  message(FATAL_ERROR "SignDirectory.cmake needs -DDIRECTORY=<folder> and -DIDENTITY=<identity>.")
endif()
if(NOT IS_DIRECTORY "${DIRECTORY}")
  message(FATAL_ERROR "SignDirectory.cmake: ${DIRECTORY} is not a folder.")
endif()

file(GLOB entries LIST_DIRECTORIES false "${DIRECTORY}/*")
list(SORT entries)
set(libraries)
set(programs)
foreach(entry IN LISTS entries)
  if(IS_SYMLINK "${entry}")
    continue()
  endif()
  # A 64-bit Mach-O (cffaedfe, little-endian) or a universal binary (cafebabe).
  file(READ "${entry}" magic LIMIT 4 HEX)
  if(NOT magic STREQUAL "cffaedfe" AND NOT magic STREQUAL "cafebabe")
    continue()
  endif()
  if(entry MATCHES "\\.dylib$")
    list(APPEND libraries "${entry}")
  else()
    list(APPEND programs "${entry}")
  endif()
endforeach()

if(NOT programs)
  message(FATAL_ERROR "SignDirectory.cmake: ${DIRECTORY} holds no program to sign.")
endif()
foreach(file IN LISTS libraries programs)
  execute_process(COMMAND /usr/bin/codesign --force --sign "${IDENTITY}" "${file}"
    RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "codesign could not sign ${file}: ${error}")
  endif()
endforeach()
