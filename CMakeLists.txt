cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
set(ENABLE_TIKTOK_LOGIN "ON" CACHE STRING "Embedded TikTok login: ON, OFF, or AUTO when a browser backend is available")
set_property(CACHE ENABLE_TIKTOK_LOGIN PROPERTY STRINGS ON OFF AUTO)
set(TIKTOK_BROWSER_BACKEND "auto" CACHE STRING "TikTok login browser backend: auto, webview2, qtwebengine or none")
set_property(CACHE TIKTOK_BROWSER_BACKEND PROPERTY STRINGS auto webview2 qtwebengine none)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.1/build_x64/libobs)
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.1/build_x64/deps/w32-pthreads)
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.1/build_x64/UI/obs-frontend-api)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)

# Optional Linux secure credential backend. When libsecret development
# files are available, link them and use the official API. Otherwise the
# plugin still builds and uses the memory-only credential fallback.
set(TIKTOK_HAVE_LIBSECRET OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)
    if(LIBSECRET_FOUND)
      set(TIKTOK_HAVE_LIBSECRET ON)
    endif()
  endif()
  if(NOT TIKTOK_HAVE_LIBSECRET)
    message(STATUS "obs-multi-rtmp: libsecret not found, Linux secure credential storage disabled")
  endif()
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(NOT TIKTOK_BROWSER_BACKEND MATCHES "^(auto|webview2|qtwebengine|none)$")
  message(FATAL_ERROR "obs-multi-rtmp: invalid TIKTOK_BROWSER_BACKEND='${TIKTOK_BROWSER_BACKEND}' (expected auto, webview2, qtwebengine or none)")
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)

  # ---------------------------------------------------------------------------
  # TikTok login browser backend
  #
  # The embedded login needs a browser backend. Windows uses the native
  # Microsoft Edge WebView2 (no Qt WebEngine runtime required), Linux and
  # macOS use Qt WebEngine. ENABLE_TIKTOK_LOGIN decides whether a missing
  # backend is a configuration error (ON) or only a warning (AUTO).
  # ---------------------------------------------------------------------------
  set(TIKTOK_BROWSER_BACKEND_SELECTED "none")
  set(TIKTOK_HAVE_WEBENGINE OFF)
  set(TIKTOK_HAVE_WEBVIEW2 OFF)

  if(NOT ENABLE_TIKTOK_LOGIN STREQUAL "OFF")
    if(TIKTOK_BROWSER_BACKEND STREQUAL "none")
      set(TIKTOK_BROWSER_BACKEND_SELECTED "none")
    elseif(TIKTOK_BROWSER_BACKEND STREQUAL "qtwebengine")
      find_package(Qt6 REQUIRED COMPONENTS WebEngineCore WebEngineWidgets)
      set(TIKTOK_HAVE_WEBENGINE ON)
      set(TIKTOK_BROWSER_BACKEND_SELECTED "qtwebengine")
    elseif(TIKTOK_BROWSER_BACKEND STREQUAL "webview2")
      if(NOT WIN32)
        message(FATAL_ERROR "obs-multi-rtmp: TIKTOK_BROWSER_BACKEND=webview2 is only supported on Windows")
      endif()
      include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/webview2.cmake")
      tiktok_setup_webview2()
      if(NOT TIKTOK_WEBVIEW2_FOUND)
        message(FATAL_ERROR "obs-multi-rtmp: the Microsoft.Web.WebView2 ${TIKTOK_WEBVIEW2_VERSION} SDK could not be acquired")
      endif()
      set(TIKTOK_HAVE_WEBVIEW2 ON)
      set(TIKTOK_BROWSER_BACKEND_SELECTED "webview2")
    else()
      # auto: prefer the native Windows backend, fall back to Qt WebEngine.
      if(WIN32)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/webview2.cmake")
        tiktok_setup_webview2()
        if(TIKTOK_WEBVIEW2_FOUND)
          set(TIKTOK_HAVE_WEBVIEW2 ON)
          set(TIKTOK_BROWSER_BACKEND_SELECTED "webview2")
        endif()
      endif()
      if(TIKTOK_BROWSER_BACKEND_SELECTED STREQUAL "none")
        find_package(Qt6 QUIET COMPONENTS WebEngineCore WebEngineWidgets)
        if(TARGET Qt6::WebEngineCore AND TARGET Qt6::WebEngineWidgets)
          set(TIKTOK_HAVE_WEBENGINE ON)
          set(TIKTOK_BROWSER_BACKEND_SELECTED "qtwebengine")
        endif()
      endif()
    endif()
  endif()

  if(TIKTOK_HAVE_WEBENGINE)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::WebEngineCore Qt6::WebEngineWidgets)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE WITH_TIKTOK_WEBENGINE=1)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ./src/tiktok/auth-webengine.h ./src/tiktok/auth-webengine.cpp)
  endif()

  if(TIKTOK_HAVE_WEBVIEW2)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${TIKTOK_WEBVIEW2_INCLUDE_DIR})
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${TIKTOK_WEBVIEW2_LIBRARY}" ole32 uuid)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE WITH_TIKTOK_WEBVIEW2=1)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ./src/tiktok/auth-webview2.h ./src/tiktok/auth-webview2.cpp)
    # The loader ships next to the plugin; OBS loads plugins with an altered
    # search path, so the plugin directory is searched first.
    install(FILES "${TIKTOK_WEBVIEW2_RUNTIME}" DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit")
    add_custom_command(
      TARGET ${CMAKE_PROJECT_NAME}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TIKTOK_WEBVIEW2_RUNTIME}" "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
      COMMENT "Copy WebView2Loader next to the plugin"
      VERBATIM
    )
  endif()

  if(ENABLE_TIKTOK_LOGIN STREQUAL "OFF")
    message(STATUS "obs-multi-rtmp: embedded TikTok login disabled by configuration")
  elseif(TIKTOK_BROWSER_BACKEND_SELECTED STREQUAL "none")
    if(ENABLE_TIKTOK_LOGIN STREQUAL "ON")
      message(FATAL_ERROR "obs-multi-rtmp: ENABLE_TIKTOK_LOGIN=ON requires a TikTok login browser backend, but none could be configured (TIKTOK_BROWSER_BACKEND=${TIKTOK_BROWSER_BACKEND})")
    else()
      message(WARNING "obs-multi-rtmp: embedded TikTok login disabled (no browser backend available; TIKTOK_BROWSER_BACKEND=${TIKTOK_BROWSER_BACKEND})")
    endif()
  else()
    message(STATUS "obs-multi-rtmp: embedded TikTok login enabled (browser backend: ${TIKTOK_BROWSER_BACKEND_SELECTED})")
  endif()

  # Marker file so the build pipeline can verify which backend was built
  # instead of trusting a successful compilation.
  file(WRITE "${CMAKE_BINARY_DIR}/tiktok-browser-backend.txt" "${TIKTOK_BROWSER_BACKEND_SELECTED}\n")

  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

if(NOT ENABLE_QT AND NOT ENABLE_TIKTOK_LOGIN STREQUAL "OFF")
  if(ENABLE_TIKTOK_LOGIN STREQUAL "ON")
    message(FATAL_ERROR "obs-multi-rtmp: ENABLE_TIKTOK_LOGIN=ON requires ENABLE_QT")
  else()
    message(WARNING "obs-multi-rtmp: ENABLE_TIKTOK_LOGIN requires ENABLE_QT; embedded login will be unavailable")
  endif()
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  ./src/pch.h
  ./src/obs-multi-rtmp.h
  ./src/obs-multi-rtmp.cpp
  ./src/edit-widget.h
  ./src/edit-widget.cpp
  ./src/push-widget.h
  ./src/push-widget.cpp
  ./src/json-util.hpp
  ./src/output-config.h
  ./src/output-config.cpp
  ./src/obs-properties-widget.h
  ./src/obs-properties-widget.cpp
  ./src/protocols.h
  ./src/protocols.cpp
  ./src/helpers.h
  ./src/helpers.cpp
  ./src/encoder-source.h
  ./src/encoder-source.cpp
  ./src/streamlabs-tiktok.h
  ./src/streamlabs-tiktok.cpp
  ./src/tiktok/constants.h
  ./src/tiktok/auth-models.h
  ./src/tiktok/pkce.h
  ./src/tiktok/pkce.cpp
  ./src/tiktok/auth-url.h
  ./src/tiktok/auth-url.cpp
  ./src/tiktok/exchange.h
  ./src/tiktok/exchange.cpp
  ./src/tiktok/credential-store.h
  ./src/tiktok/credential-store.cpp
  ./src/tiktok/streamlabs-client.h
  ./src/tiktok/streamlabs-client.cpp
  ./src/tiktok/auth-flow.h
  ./src/tiktok/auth-flow.cpp
  ./src/tiktok/auth-browser.h
  ./src/tiktok/auth-browser.cpp
  ./src/tiktok/auth-browser-factory.h
  ./src/tiktok/auth-browser-factory.cpp
  ./src/tiktok/auth-profile.h
  ./src/tiktok/auth-profile.cpp
  ./src/tiktok/auth-jsvalue.h
  ./src/tiktok/auth-jsvalue.cpp
  ./src/tiktok/auth-dialog.h
  ./src/tiktok/auth-dialog.cpp
  ./src/tiktok/auth-manager.h
  ./src/tiktok/auth-manager.cpp
  ./src/tiktok/worker.h
  ./src/tiktok/worker.cpp
  ./src/tiktok/cleanup.h
  ./src/tiktok/cleanup.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
  ./src
  ./dep/nlohmann-json
  .deps/obs-studio-32.2.1/libobs
)

if(WIN32)
  # Windows Credential Manager (CredReadW/CredWriteW/CredDeleteW).
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Advapi32)
elseif(APPLE)
  # macOS Keychain Services.
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "-framework Security" "-framework CoreFoundation")
elseif(TIKTOK_HAVE_LIBSECRET)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::LIBSECRET)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE TIKTOK_USE_LIBSECRET=1)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

include(CTest)
if(BUILD_TESTING)
  add_executable(test-streamlabs-tiktok ./tests/test-streamlabs-tiktok.cpp ./src/streamlabs-tiktok.cpp)
  target_include_directories(test-streamlabs-tiktok PRIVATE ./src ./dep/nlohmann-json)
  target_link_libraries(test-streamlabs-tiktok PRIVATE CURL::libcurl)
  add_test(NAME streamlabs-tiktok COMMAND test-streamlabs-tiktok)

  add_executable(test-tiktok-exchange ./tests/test-tiktok-exchange.cpp ./src/tiktok/exchange.cpp)
  target_include_directories(test-tiktok-exchange PRIVATE ./src ./dep/nlohmann-json)
  add_test(NAME tiktok-exchange COMMAND test-tiktok-exchange)
  find_program(TIKTOK_TEST_NODE node)
  if(TIKTOK_TEST_NODE)
    add_test(NAME tiktok-exchange-runtime COMMAND "${TIKTOK_TEST_NODE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-tiktok-exchange-runtime.js" $<TARGET_FILE:test-tiktok-exchange>)
  endif()

  add_executable(test-tiktok-credential-store ./tests/test-tiktok-credential-store.cpp ./src/tiktok/credential-store.cpp)
  target_include_directories(test-tiktok-credential-store PRIVATE ./src)
  if(APPLE)
    target_link_libraries(test-tiktok-credential-store PRIVATE "-framework Security" "-framework CoreFoundation")
  elseif(WIN32)
    target_link_libraries(test-tiktok-credential-store PRIVATE Advapi32)
  elseif(TIKTOK_HAVE_LIBSECRET)
    target_link_libraries(test-tiktok-credential-store PRIVATE PkgConfig::LIBSECRET)
    target_compile_definitions(test-tiktok-credential-store PRIVATE TIKTOK_USE_LIBSECRET=1)
  endif()
  add_test(NAME tiktok-credential-store COMMAND test-tiktok-credential-store)

  find_package(Threads REQUIRED)
  add_executable(test-encoder-source ./tests/test-encoder-source.cpp ./src/encoder-source.cpp)
  target_include_directories(test-encoder-source PRIVATE ./src ./dep/nlohmann-json)
  add_test(NAME encoder-source COMMAND test-encoder-source)

  add_executable(test-tiktok-cleanup ./tests/test-tiktok-cleanup.cpp ./src/tiktok/cleanup.cpp)
  target_include_directories(test-tiktok-cleanup PRIVATE ./src)
  target_link_libraries(test-tiktok-cleanup PRIVATE Threads::Threads)
  add_test(NAME tiktok-cleanup COMMAND test-tiktok-cleanup)

  # Qt-based pure-logic tests (no WebEngine, no OBS needed).
  find_package(Qt6 COMPONENTS Core QUIET)
  if(Qt6Core_FOUND)
    add_executable(test-tiktok-pkce ./tests/test-tiktok-pkce.cpp ./src/tiktok/pkce.cpp)
    target_include_directories(test-tiktok-pkce PRIVATE ./src)
    target_link_libraries(test-tiktok-pkce PRIVATE Qt6::Core)
    add_test(NAME tiktok-pkce COMMAND test-tiktok-pkce)

    add_executable(test-tiktok-auth-url ./tests/test-tiktok-auth-url.cpp ./src/tiktok/auth-url.cpp)
    target_include_directories(test-tiktok-auth-url PRIVATE ./src)
    target_link_libraries(test-tiktok-auth-url PRIVATE Qt6::Core)
    add_test(NAME tiktok-auth-url COMMAND test-tiktok-auth-url)

    add_executable(test-tiktok-auth-flow
      ./tests/test-tiktok-auth-flow.cpp
      ./src/tiktok/auth-flow.cpp
      ./src/tiktok/auth-url.cpp
      ./src/tiktok/pkce.cpp)
    target_include_directories(test-tiktok-auth-flow PRIVATE ./src)
    target_link_libraries(test-tiktok-auth-flow PRIVATE Qt6::Core)
    add_test(NAME tiktok-auth-flow COMMAND test-tiktok-auth-flow)

    # Browser backend contract: readiness, failure, queued navigation and
    # cleanup with a mock backend (no real browser is started).
    add_executable(test-tiktok-auth-browser
      ./tests/test-tiktok-auth-browser.cpp
      ./src/tiktok/auth-browser.cpp)
    target_include_directories(test-tiktok-auth-browser PRIVATE ./src)
    target_link_libraries(test-tiktok-auth-browser PRIVATE Qt6::Core)
    set_target_properties(test-tiktok-auth-browser PROPERTIES AUTOMOC ON)
    add_test(NAME tiktok-auth-browser COMMAND test-tiktok-auth-browser)
    set_tests_properties(tiktok-auth-browser PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Factory selection rules (platform preference, no silent downgrade).
    add_executable(test-tiktok-auth-browser-factory
      ./tests/test-tiktok-auth-browser-factory.cpp
      ./src/tiktok/auth-browser.cpp
      ./src/tiktok/auth-browser-factory.cpp)
    target_include_directories(test-tiktok-auth-browser-factory PRIVATE ./src)
    target_link_libraries(test-tiktok-auth-browser-factory PRIVATE Qt6::Core)
    set_target_properties(test-tiktok-auth-browser-factory PROPERTIES AUTOMOC ON)
    if(TIKTOK_HAVE_WEBENGINE)
      # The factory creates the configured backend, so its implementation is
      # needed for linking.
      target_sources(test-tiktok-auth-browser-factory PRIVATE
        ./src/tiktok/auth-webengine.cpp
        ./src/tiktok/auth-profile.cpp)
      target_compile_definitions(test-tiktok-auth-browser-factory PRIVATE WITH_TIKTOK_WEBENGINE=1)
      target_link_libraries(test-tiktok-auth-browser-factory PRIVATE Qt6::WebEngineCore Qt6::WebEngineWidgets)
    endif()
    if(TIKTOK_HAVE_WEBVIEW2)
      target_sources(test-tiktok-auth-browser-factory PRIVATE
        ./src/tiktok/auth-webview2.cpp
        ./src/tiktok/auth-profile.cpp
        ./src/tiktok/auth-jsvalue.cpp)
      target_compile_definitions(test-tiktok-auth-browser-factory PRIVATE WITH_TIKTOK_WEBVIEW2=1)
      target_include_directories(test-tiktok-auth-browser-factory PRIVATE ${TIKTOK_WEBVIEW2_INCLUDE_DIR})
      target_link_libraries(test-tiktok-auth-browser-factory PRIVATE "${TIKTOK_WEBVIEW2_LIBRARY}")
    endif()
    add_test(NAME tiktok-auth-browser-factory COMMAND test-tiktok-auth-browser-factory)
    set_tests_properties(tiktok-auth-browser-factory PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # WebView2 style JavaScript result conversion (JSON -> QVariant).
    add_executable(test-tiktok-auth-jsvalue ./tests/test-tiktok-auth-jsvalue.cpp ./src/tiktok/auth-jsvalue.cpp)
    target_include_directories(test-tiktok-auth-jsvalue PRIVATE ./src)
    target_link_libraries(test-tiktok-auth-jsvalue PRIVATE Qt6::Core)
    add_test(NAME tiktok-auth-jsvalue COMMAND test-tiktok-auth-jsvalue)
  endif()
endif()

# Exercise the actual PushWidget/OBS paths with a deterministic client and
# without the embedded browser or an authenticated account.
if(BUILD_TESTING AND ENABLE_QT AND ENABLE_FRONTEND_API)
  get_target_property(_lifecycle_sources ${CMAKE_PROJECT_NAME} SOURCES)
  list(FILTER _lifecycle_sources EXCLUDE REGEX "push-widget\\.(cpp|h)$|streamlabs-client\\.cpp$|auth-webengine\\.(cpp|h)$|auth-webview2\\.(cpp|h)$")
  add_executable(test-tiktok-lifecycle ./tests/test-tiktok-lifecycle.cpp ${_lifecycle_sources})
  get_target_property(_lifecycle_includes ${CMAKE_PROJECT_NAME} INCLUDE_DIRECTORIES)
  target_include_directories(test-tiktok-lifecycle PRIVATE ${_lifecycle_includes})
  target_link_libraries(test-tiktok-lifecycle PRIVATE OBS::libobs OBS::obs-frontend-api
    CURL::libcurl Qt6::Core Qt6::Widgets plugin-support)
  if(WIN32)
    target_link_libraries(test-tiktok-lifecycle PRIVATE Advapi32)
  elseif(APPLE)
    target_link_libraries(test-tiktok-lifecycle PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  set_target_properties(test-tiktok-lifecycle PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
  add_test(NAME tiktok-lifecycle COMMAND test-tiktok-lifecycle)
  set_tests_properties(tiktok-lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)

  # Real standalone-streaming integration: independent x264+AAC through the
  # actual widget paths with real video/audio contexts and OBS plugins. No
  # network, no account, and the main OBS streaming output is never
  # started. Skips cleanly (exit 0 with a SKIP note) where video init is
  # unavailable, e.g. display-less CI. Run with a display for GL:
  # DISPLAY=:0 ctest -R tiktok-standalone
  get_target_property(_standalone_sources ${CMAKE_PROJECT_NAME} SOURCES)
  list(FILTER _standalone_sources EXCLUDE REGEX "push-widget\\.(cpp|h)$|streamlabs-client\\.cpp$|auth-webengine\\.(cpp|h)$|auth-webview2\\.(cpp|h)$")
  add_executable(test-tiktok-standalone ./tests/test-tiktok-standalone.cpp ${_standalone_sources})
  get_target_property(_standalone_includes ${CMAKE_PROJECT_NAME} INCLUDE_DIRECTORIES)
  target_include_directories(test-tiktok-standalone PRIVATE ${_standalone_includes})
  target_link_libraries(test-tiktok-standalone PRIVATE OBS::libobs OBS::obs-frontend-api
    CURL::libcurl Qt6::Core Qt6::Widgets plugin-support)
  if(WIN32)
    target_link_libraries(test-tiktok-standalone PRIVATE Advapi32)
  elseif(APPLE)
    target_link_libraries(test-tiktok-standalone PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  set_target_properties(test-tiktok-standalone PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
  add_test(NAME tiktok-standalone COMMAND test-tiktok-standalone)
  set_tests_properties(tiktok-standalone PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180 SKIP_RETURN_CODE 77)
endif()
