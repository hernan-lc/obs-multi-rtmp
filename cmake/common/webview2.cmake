# Microsoft Edge WebView2 SDK acquisition for the TikTok login browser.
#
# The official SDK is distributed as a NuGet package. This module downloads
# a pinned version, verifies its SHA-256 checksum and extracts the native
# header plus the loader import library.
#
# Nothing is bundled with the plugin beyond the small loader library: the
# Evergreen WebView2 runtime is used from the machine, exactly like the
# Microsoft documentation recommends for desktop applications.
#
# Usage:
#   tiktok_setup_webview2()
#
# Sets (in the calling scope):
#   TIKTOK_WEBVIEW2_FOUND         - TRUE when the SDK is usable
#   TIKTOK_WEBVIEW2_INCLUDE_DIR   - directory holding WebView2.h
#   TIKTOK_WEBVIEW2_LIBRARY       - loader import library
#   TIKTOK_WEBVIEW2_RUNTIME       - loader runtime library (shipped next
#                                   to the plugin)

include_guard(GLOBAL)

set(TIKTOK_WEBVIEW2_VERSION "1.0.3351.48" CACHE STRING "Microsoft.Web.WebView2 SDK version")
set(TIKTOK_WEBVIEW2_SHA256
    "ad98e9d951361e62b6edc137273ec1787242c5c28539c1a101f98a1e07d060f9"
    CACHE STRING "SHA-256 checksum of the pinned Microsoft.Web.WebView2 package")
set(
  TIKTOK_WEBVIEW2_PACKAGE_URL
  "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${TIKTOK_WEBVIEW2_VERSION}/microsoft.web.webview2.${TIKTOK_WEBVIEW2_VERSION}.nupkg"
  CACHE STRING "Download URL of the pinned Microsoft.Web.WebView2 package")
mark_as_advanced(TIKTOK_WEBVIEW2_SHA256 TIKTOK_WEBVIEW2_PACKAGE_URL)

function(tiktok_setup_webview2)
  set(_found OFF)
  set(_include_dir "")
  set(_library "")
  set(_runtime "")

  if(NOT WIN32)
    message(STATUS "obs-multi-rtmp: the WebView2 backend is only available on Windows")
  else()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_arch "x64")
    else()
      set(_arch "x86")
    endif()

    set(_root "${CMAKE_CURRENT_BINARY_DIR}/webview2-sdk/${TIKTOK_WEBVIEW2_VERSION}")
    set(_include "${_root}/include")
    set(_lib "${_root}/lib/${_arch}")
    set(_package "${_root}/microsoft.web.webview2.${TIKTOK_WEBVIEW2_VERSION}.nupkg")

    if(EXISTS "${_include}/WebView2.h" AND EXISTS "${_lib}/WebView2Loader.dll.lib")
      # Already extracted by an earlier configure run.
      set(_found ON)
    else()
      file(MAKE_DIRECTORY "${_root}")
      message(STATUS "obs-multi-rtmp: downloading Microsoft.Web.WebView2 ${TIKTOK_WEBVIEW2_VERSION}")
      file(
        DOWNLOAD "${TIKTOK_WEBVIEW2_PACKAGE_URL}" "${_package}"
        TLS_VERIFY ON
        STATUS _download_status
        SHOW_PROGRESS
        TIMEOUT 300)
      list(GET _download_status 0 _download_code)
      if(NOT _download_code EQUAL 0)
        message(STATUS "obs-multi-rtmp: WebView2 SDK download failed: ${_download_status}")
      else()
        file(SHA256 "${_package}" _actual_hash)
        if(NOT _actual_hash STREQUAL "${TIKTOK_WEBVIEW2_SHA256}")
          message(
            STATUS
              "obs-multi-rtmp: WebView2 SDK checksum mismatch (expected ${TIKTOK_WEBVIEW2_SHA256}, got ${_actual_hash})"
          )
          file(REMOVE "${_package}")
        else()
          file(
            ARCHIVE_EXTRACT
            INPUT "${_package}"
            DESTINATION "${_root}/extracted"
            PATTERNS "build/native/include/*" "build/native/${_arch}/*")
          file(COPY "${_root}/extracted/build/native/include/" DESTINATION "${_include}")
          file(COPY "${_root}/extracted/build/native/${_arch}/" DESTINATION "${_lib}")
          if(EXISTS "${_include}/WebView2.h" AND EXISTS "${_lib}/WebView2Loader.dll.lib")
            set(_found ON)
          else()
            message(STATUS "obs-multi-rtmp: the WebView2 SDK package did not contain the expected files")
          endif()
        endif()
      endif()
    endif()

    if(_found)
      set(_include_dir "${_include}")
      set(_library "${_lib}/WebView2Loader.dll.lib")
      set(_runtime "${_lib}/WebView2Loader.dll")
    endif()
  endif()

  set(TIKTOK_WEBVIEW2_FOUND "${_found}" PARENT_SCOPE)
  set(TIKTOK_WEBVIEW2_INCLUDE_DIR "${_include_dir}" PARENT_SCOPE)
  set(TIKTOK_WEBVIEW2_LIBRARY "${_library}" PARENT_SCOPE)
  set(TIKTOK_WEBVIEW2_RUNTIME "${_runtime}" PARENT_SCOPE)
endfunction()
