cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(ENABLE_TIKTOK_LOGIN "Enable embedded TikTok login via Qt WebEngine" ON)

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

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  # Qt WebEngine provides the embedded TikTok login browser. Release
  # builds keep ENABLE_TIKTOK_LOGIN=ON and therefore fail here if the
  # OBS/Qt bundle cannot provide a functional WebEngine runtime.
  # Minimal/development builds may set ENABLE_TIKTOK_LOGIN=OFF.
  if(ENABLE_TIKTOK_LOGIN)
    find_package(Qt6 REQUIRED COMPONENTS WebEngineCore WebEngineWidgets)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::WebEngineCore Qt6::WebEngineWidgets)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE WITH_TIKTOK_WEBENGINE=1)
    message(STATUS "obs-multi-rtmp: embedded TikTok login enabled")
  else()
    message(STATUS "obs-multi-rtmp: embedded TikTok login disabled")
  endif()
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

if(NOT ENABLE_QT AND ENABLE_TIKTOK_LOGIN)
  message(WARNING "obs-multi-rtmp: ENABLE_TIKTOK_LOGIN requires ENABLE_QT; embedded login will be unavailable")
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
  endif()
endif()

# Exercise the actual PushWidget/OBS paths with a deterministic client and
# without the embedded browser or an authenticated account.
if(BUILD_TESTING AND ENABLE_QT AND ENABLE_FRONTEND_API)
  get_target_property(_lifecycle_sources ${CMAKE_PROJECT_NAME} SOURCES)
  list(FILTER _lifecycle_sources EXCLUDE REGEX "push-widget\\.(cpp|h)$|streamlabs-client\\.cpp$")
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
endif()
