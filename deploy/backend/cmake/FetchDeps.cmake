include_guard(GLOBAL)

include(FetchContent)

function(native_app_fetch_dependencies)
  # A MinGW tree configured before zlib was linked in (windows/mingw-x64.cmake)
  # remembers its import library, libz.dll.a; forgotten, the static one is found.
  if(MINGW AND ZLIB_USE_STATIC_LIBS)
    foreach(cached ZLIB_LIBRARY ZLIB_LIBRARY_RELEASE ZLIB_LIBRARY_DEBUG)
      if(DEFINED CACHE{${cached}} AND "$CACHE{${cached}}" MATCHES "\\.dll\\.a$")
        unset(${cached} CACHE)
      endif()
    endforeach()
  endif()

  set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_BUILD_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_ENABLE_CHECKS OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
  if(MINGW)
    # Microsoft.Web.WebView2 expects the Windows SDK's case-sensitive
    # EventToken.h, which is absent from MinGW. Webview 0.12.0 ships the
    # compatible definition specifically for this cross-build case.
    set(WEBVIEW_USE_COMPAT_MINGW ON CACHE BOOL "" FORCE)
  endif()

  if(UNIX AND NOT APPLE)
    set(WEBVIEW_WEBKITGTK_API "4.1" CACHE STRING "" FORCE)
  endif()

  set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(NFD_BUILD_SDL2_TESTS OFF CACHE BOOL "" FORCE)
  set(NFD_BUILD_GLFW3_TESTS OFF CACHE BOOL "" FORCE)
  set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  # DOCX is a ZIP container of Office Open XML. Build only the bounded reader
  # libraries Singularity embeds; command-line tools, examples, regression suites,
  # documentation and non-system optional compression/crypto backends are not
  # runtime dependencies of document extraction.
  set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(BUILD_REGRESS OFF CACHE BOOL "" FORCE)
  set(BUILD_OSSFUZZ OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_DOC OFF CACHE BOOL "" FORCE)
  set(ENABLE_BZIP2 OFF CACHE BOOL "" FORCE)
  set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
  set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
  set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
  set(ENABLE_GNUTLS OFF CACHE BOOL "" FORCE)
  set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG 0.12.0
    GIT_SHALLOW TRUE
    # The application names WebView2's user data folder (PatchWebview.cmake).
    PATCH_COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PatchWebview.cmake"
  )
  FetchContent_Declare(
    nativefiledialog
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG v1.3.0
    GIT_SHALLOW TRUE
  )
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
  )
  FetchContent_Declare(
    libzip
    GIT_REPOSITORY https://github.com/nih-at/libzip.git
    # v1.11.4, pinned to the resolved commit for reproducible dependency input.
    GIT_TAG 6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea
    GIT_SHALLOW TRUE
  )
  FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    # v1.16, pinned to the resolved commit for reproducible dependency input.
    GIT_TAG c8033ce9d039e7f9d134877c363397b3cfe20816
    GIT_SHALLOW TRUE
  )

  # Unicode normalization/case-folding/character-category mechanics for the
  # research-query privacy boundary (ExternalResearch/ResearchTools) are
  # infrastructure, not Legal Machines proprietary logic - Singularity owns the
  # privacy matching *policy* (which values are sensitive, how a boundary
  # counts as a match), utf8proc owns the Unicode tables/algorithms
  # underneath it. Testing (never redownloaded at configure time; see
  # UTF8PROC_ENABLE_TESTING below) is not needed for that.
  set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    utf8proc
    GIT_REPOSITORY https://github.com/JuliaStrings/utf8proc.git
    # v2.11.3 (latest stable release as of 2025-12-30), pinned to the
    # resolved commit for reproducible dependency input.
    GIT_TAG e5e799221b45bbb90f5fdc5c69b6b8dfbf017e78
    GIT_SHALLOW TRUE
  )

  if(WIN32)
    # WebView2's SDK, pinned by version and SHA-256 rather than fetched by
    # version alone as webview 0.12.0 would (its default is 1.0.1150.38).
    # 1.0.4258.31 gives the host the paths of files dropped on the page
    # (ICoreWebView2File, Platform/FileDrop). Here the SDK is headers: webview
    # loads the installed WebView2 runtime itself
    # (WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL). Populated first, so webview finds it
    # (MSWebView2_ROOT) and fetches nothing of its own.
    set(WEBVIEW_MSWEBVIEW2_VERSION "1.0.4258.31" CACHE STRING "MS WebView2 version" FORCE)
    FetchContent_Declare(
      microsoft_web_webview2
      URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.4258.31
      URL_HASH SHA256=56f7f4b8bf9aee4b8efefbbdd4f67d5f74ebd1b100ed0806da71bf76af481aa9
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(microsoft_web_webview2)
    set(MSWebView2_ROOT "${microsoft_web_webview2_SOURCE_DIR}")
  endif()

  FetchContent_MakeAvailable(webview nativefiledialog nlohmann_json)
  # A webview fetched without its patch would build, with WebView2's data back
  # in the roaming profile and nothing to say so.
  FetchContent_GetProperties(webview)
  file(STRINGS "${webview_SOURCE_DIR}/core/include/webview/webview.h" webview_patched LIMIT_COUNT 1
    REGEX "Singularity patch \\(deploy/backend/cmake/PatchWebview\\.cmake\\)")
  if(NOT webview_patched)
    message(FATAL_ERROR "webview at ${webview_SOURCE_DIR} lacks its patch (PatchWebview.cmake), which names "
      "WebView2's user data folder. Delete that folder and configure again, so it is fetched and patched.")
  endif()

  # libzip, pugixml and utf8proc are implementation dependencies of document
  # extraction/research-query privacy, not a product install surface.
  # Populating them through plain FetchContent_MakeAvailable pulls their own
  # upstream install() rules (headers, pkg-config files and static
  # libraries) into Singularity's install prefix, which CPack's DragNDrop
  # generator then packages as loose developer files alongside Singularity.app -
  # the same class of packaging defect the pinned llama.cpp dependency below
  # deliberately avoids with EXCLUDE_FROM_ALL. None of the three has any
  # packaged runtime surface of its own (all three link statically into
  # native_app), so excluding their upstream install rules from the default
  # install target loses nothing.
  foreach(dependency libzip pugixml utf8proc)
    FetchContent_GetProperties(${dependency})
    if(NOT ${dependency}_POPULATED)
      if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
      endif()
      FetchContent_Populate(${dependency})
      if(POLICY CMP0169)
        cmake_policy(POP)
      endif()
    endif()
    add_subdirectory(
      "${${dependency}_SOURCE_DIR}"
      "${${dependency}_BINARY_DIR}"
      EXCLUDE_FROM_ALL)
  endforeach()

  # Ed25519 for the records a computer signs into a shared Matter
  # (Platform/DeviceKey): Monocypher 4.0.2 (CC0, or BSD-2), its release archive
  # pinned by SHA-256 - the same bytes monocypher.org and its GitHub release
  # serve. Two C files, linked statically; nothing installed.
  FetchContent_Declare(
    monocypher
    URL https://monocypher.org/download/monocypher-4.0.2.tar.gz
    URL_HASH SHA256=38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_GetProperties(monocypher)
  if(NOT monocypher_POPULATED)
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(monocypher)
    if(POLICY CMP0169)
      cmake_policy(POP)
    endif()
  endif()
  if(NOT TARGET monocypher)
    add_library(monocypher STATIC
      "${monocypher_SOURCE_DIR}/src/monocypher.c"
      "${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c")
    target_include_directories(monocypher PUBLIC
      "${monocypher_SOURCE_DIR}/src"
      "${monocypher_SOURCE_DIR}/src/optional")
    set_target_properties(monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()
  set(SINGULARITY_MONOCYPHER_LICENCE "${monocypher_SOURCE_DIR}/LICENCE.md" PARENT_SCOPE)

  # TLS for the Office connection: Word, Excel and Outlook load Singularity's
  # panes only over HTTPS, and the panes come from this computer. Mbed TLS
  # 3.6.7 (Apache-2.0), the long-term support line, its release archive
  # pinned by SHA-256 - the same bytes its GitHub release lists in
  # mbedtls-3.6.7-sha256sum.txt. Its library sources are built here as one
  # static library, with Singularity's additions to its configuration
  # (cmake/mbedtls/lm-mbedtls-config.h); no programs, tests or install rules.
  FetchContent_Declare(
    mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_GetProperties(mbedtls)
  if(NOT mbedtls_POPULATED)
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(mbedtls)
    if(POLICY CMP0169)
      cmake_policy(POP)
    endif()
  endif()
  if(NOT TARGET lm_mbedtls)
    file(GLOB lm_mbedtls_sources CONFIGURE_DEPENDS "${mbedtls_SOURCE_DIR}/library/*.c")
    add_library(lm_mbedtls STATIC ${lm_mbedtls_sources})
    target_include_directories(lm_mbedtls SYSTEM PUBLIC
      "${mbedtls_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/mbedtls")
    target_include_directories(lm_mbedtls PRIVATE "${mbedtls_SOURCE_DIR}/library")
    target_compile_definitions(lm_mbedtls PUBLIC "MBEDTLS_USER_CONFIG_FILE=\"lm-mbedtls-config.h\"")
    set_target_properties(lm_mbedtls PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
      target_compile_options(lm_mbedtls PRIVATE /W0 /utf-8)
    else()
      target_compile_options(lm_mbedtls PRIVATE -w)
    endif()
    if(WIN32)
      target_link_libraries(lm_mbedtls PUBLIC bcrypt ws2_32)
    endif()
  endif()
  set(SINGULARITY_MBEDTLS_LICENCE "${mbedtls_SOURCE_DIR}/LICENSE" PARENT_SCOPE)
endfunction()
