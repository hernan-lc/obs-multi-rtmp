cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

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

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  # Qt WebEngine provides the embedded TikTok login browser. It is
  # resolved separately from Widgets because "Widgets available" does
  # not imply "WebEngine available" (OBS Flatpak / dependency bundles).
  # Without it the plugin still builds; the login dialog reports that
  # WebEngine packaging is missing instead of offering login.
  find_package(Qt6 COMPONENTS WebEngineCore WebEngineWidgets)
  if(Qt6WebEngineWidgets_FOUND)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::WebEngineCore Qt6::WebEngineWidgets)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE WITH_TIKTOK_WEBENGINE=1)
    message(STATUS "obs-multi-rtmp: Qt WebEngine found, embedded TikTok login enabled")
  else()
    message(WARNING "obs-multi-rtmp: Qt WebEngine NOT found, TikTok login dialog will be unavailable")
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
else()
  # Linux Secret Service backend loads libsecret with dlopen.
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
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

  add_executable(test-tiktok-credential-store ./tests/test-tiktok-credential-store.cpp ./src/tiktok/credential-store.cpp)
  target_include_directories(test-tiktok-credential-store PRIVATE ./src)
  if(APPLE)
    target_link_libraries(test-tiktok-credential-store PRIVATE "-framework Security" "-framework CoreFoundation")
  elseif(WIN32)
    target_link_libraries(test-tiktok-credential-store PRIVATE Advapi32)
  else()
    target_link_libraries(test-tiktok-credential-store PRIVATE ${CMAKE_DL_LIBS})
  endif()
  add_test(NAME tiktok-credential-store COMMAND test-tiktok-credential-store)

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
