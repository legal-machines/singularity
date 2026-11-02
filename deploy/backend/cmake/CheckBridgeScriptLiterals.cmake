# Fails when a raw string piece of the window's bridge script reaches what MSVC
# refuses: 16,380 bytes for one piece before adjacent pieces join (C2026), or
# 65,535 bytes for pieces joined, its terminating null included. Clang and GCC
# take longer literals, so on the Mac and Linux only this test says so
# (2026-10-31: 2842891 broke native_app on Windows alone; d0122b6 split it).
#   cmake -DSOURCE=<NativeBridge.cpp> -P CheckBridgeScriptLiterals.cmake
# A script run with -P takes the project's policies only when it says so:
# without them while(TRUE) reads TRUE as an unset variable.
cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED SOURCE OR NOT EXISTS "${SOURCE}")
  message(FATAL_ERROR "SOURCE names no file: '${SOURCE}'")
endif()

set(open "R\"js(")
set(close ")js\"")
string(LENGTH "${open}" open_length)
string(LENGTH "${close}" close_length)
set(most_piece 16380)
set(most_joined 65535)

file(READ "${SOURCE}" rest)
set(problems "")
set(pieces 0)
set(largest 0)
set(joined 0)
set(largest_joined 0)
set(is_first TRUE)
while(TRUE)
  string(FIND "${rest}" "${open}" start)
  if(start EQUAL -1)
    break()
  endif()
  # Pieces join when only blanks and // comments stand between them.
  string(SUBSTRING "${rest}" 0 ${start} between)
  string(REGEX REPLACE "//[^\n]*" "" between "${between}")
  string(STRIP "${between}" between)
  if(is_first OR NOT between STREQUAL "")
    set(joined 0)
  endif()
  set(is_first FALSE)

  math(EXPR body_start "${start} + ${open_length}")
  string(SUBSTRING "${rest}" ${body_start} -1 rest)
  string(FIND "${rest}" "${close}" stop)
  if(stop EQUAL -1)
    message(FATAL_ERROR "A raw piece opened with ${open} is never closed in ${SOURCE}.")
  endif()
  string(SUBSTRING "${rest}" 0 ${stop} body)
  string(LENGTH "${body}" size)
  math(EXPR pieces "${pieces} + 1")
  math(EXPR joined "${joined} + ${size}")
  if(size GREATER largest)
    set(largest ${size})
  endif()
  if(joined GREATER largest_joined)
    set(largest_joined ${joined})
  endif()
  if(size GREATER_EQUAL most_piece)
    string(SUBSTRING "${body}" 0 60 opening)
    string(REPLACE "\n" " " opening "${opening}")
    list(APPEND problems "piece ${pieces} is ${size} bytes, at or over ${most_piece}: it begins '${opening}'")
  endif()
  if(joined GREATER_EQUAL most_joined)
    list(APPEND problems "pieces joined up to piece ${pieces} are ${joined} bytes, at or over ${most_joined}")
  endif()

  math(EXPR after "${stop} + ${close_length}")
  string(SUBSTRING "${rest}" ${after} -1 rest)
endwhile()

if(pieces EQUAL 0)
  message(FATAL_ERROR "No ${open} piece found in ${SOURCE}: the check no longer sees the script.")
endif()
if(problems)
  list(JOIN problems "\n  " lines)
  message(FATAL_ERROR "The bridge script would not compile with MSVC. Split the piece where its methods part, "
    "as adjacent pieces (see NativeBridge.cpp):\n  ${lines}")
endif()
message(STATUS "Bridge script: ${pieces} raw pieces, the largest ${largest} of ${most_piece} bytes, "
  "the largest joined ${largest_joined} of ${most_joined}.")
