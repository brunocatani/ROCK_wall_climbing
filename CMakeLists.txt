cmake_minimum_required(VERSION 4.2)

set(NAME "ROCK_Wall_Climbing")
set(FRIENDLY_NAME "ROCK (and Walls) Climbing")
set(VERSION 0.1.0)

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SOURCE_DIR "${ROOT_DIR}/src")
set(BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")
get_filename_component(WORKSPACE_ROOT "${ROOT_DIR}/../../.." ABSOLUTE)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed")
endif()

macro(set_from_environment VARIABLE)
  if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
    set(${VARIABLE} $ENV{${VARIABLE}})
  endif()
endmacro()

set_from_environment(VCPKG_ROOT)
if(DEFINED VCPKG_ROOT)
  set(CMAKE_TOOLCHAIN_FILE
    "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "")
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
  set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    CACHE STRING "")
else()
  message(FATAL_ERROR "VCPKG_ROOT is not set")
endif()

if(DEFINED COMMON_LIB_F4VR_PATH AND
    NOT COMMON_LIB_F4VR_PATH STREQUAL "")
  set(COMMON_LIB_F4VR_PATH_SELECTED
    "${COMMON_LIB_F4VR_PATH}"
    CACHE PATH "" FORCE)
else()
  set(COMMON_LIB_F4VR_PATH_SELECTED
    "${WORKSPACE_ROOT}/libraries_and_tools/CommonLibF4VR"
    CACHE PATH "" FORCE)
endif()
if(NOT EXISTS
    "${COMMON_LIB_F4VR_PATH_SELECTED}/CommonLibF4/include/REL/Relocation.h")
  message(FATAL_ERROR
    "CommonLibF4VR path not found in '${COMMON_LIB_F4VR_PATH_SELECTED}'")
endif()

if(DEFINED ROCK_SOURCE_INCLUDE_DIR AND
    NOT ROCK_SOURCE_INCLUDE_DIR STREQUAL "")
  set(ROCK_SOURCE_INCLUDE_DIR_SELECTED
    "${ROCK_SOURCE_INCLUDE_DIR}"
    CACHE PATH "" FORCE)
else()
  set(ROCK_SOURCE_INCLUDE_DIR_SELECTED
    "${WORKSPACE_ROOT}/main_projects/ROCK/src"
    CACHE PATH "" FORCE)
endif()
if(NOT EXISTS
    "${ROCK_SOURCE_INCLUDE_DIR_SELECTED}/api/ROCKProviderApi.h")
  message(FATAL_ERROR
    "ROCK V1 provider header not found in '${ROCK_SOURCE_INCLUDE_DIR_SELECTED}'")
endif()

project(${NAME} VERSION ${VERSION} LANGUAGES CXX)

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
set(F4SE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_NG OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_F4 OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_VR ON CACHE BOOL "" FORCE)

add_subdirectory(
  "${COMMON_LIB_F4VR_PATH_SELECTED}"
  "CommonLibF4"
  EXCLUDE_FROM_ALL)

configure_file(
  "${ROOT_DIR}/cmake/Version.h.in"
  "${BUILD_DIR}/include/Version.h"
  @ONLY)
configure_file(
  "${ROOT_DIR}/cmake/version.rc.in"
  "${BUILD_DIR}/version.rc"
  @ONLY)

add_library(ROCKWallClimbingCore STATIC
  "${SOURCE_DIR}/runtime/ControllerPolicy.h"
  "${SOURCE_DIR}/runtime/ClimbingPolicy.cpp"
  "${SOURCE_DIR}/runtime/ClimbingPolicy.h"
  "${SOURCE_DIR}/runtime/VelocityHistory.cpp"
  "${SOURCE_DIR}/runtime/VelocityHistory.h"
)
target_compile_features(ROCKWallClimbingCore PUBLIC cxx_std_23)
target_include_directories(ROCKWallClimbingCore PUBLIC "${SOURCE_DIR}")
target_compile_options(ROCKWallClimbingCore PRIVATE
  /sdl
  /utf-8
  /permissive-
  /Zc:__cplusplus
  /Zc:preprocessor
  /W4
)

add_library(${PROJECT_NAME} SHARED
  "${SOURCE_DIR}/api/RockApiClient.cpp"
  "${SOURCE_DIR}/api/RockApiClient.h"
  "${SOURCE_DIR}/PCH.h"
  "${SOURCE_DIR}/Plugin.cpp"
  "${SOURCE_DIR}/exports.def"
  "${SOURCE_DIR}/runtime/ClimbingRuntime.cpp"
  "${SOURCE_DIR}/runtime/ClimbingRuntime.h"
  "${SOURCE_DIR}/runtime/ControllerPolicy.h"
  "${SOURCE_DIR}/support/Config.cpp"
  "${SOURCE_DIR}/support/Config.h"
  "${SOURCE_DIR}/support/Logger.h"
  "${BUILD_DIR}/include/Version.h"
  "${BUILD_DIR}/version.rc"
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME} PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  NOMMNOSOUND
)
target_include_directories(${PROJECT_NAME} PRIVATE
  "${BUILD_DIR}/include"
  "${ROCK_SOURCE_INCLUDE_DIR_SELECTED}"
  "${SOURCE_DIR}"
)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE
  ROCKWallClimbingCore
  CommonLibF4::CommonLibF4
  spdlog::spdlog
  ole32.lib
  shell32.lib
)

target_precompile_headers(${PROJECT_NAME} PRIVATE "${SOURCE_DIR}/PCH.h")
target_compile_options(${PROJECT_NAME} PRIVATE
  /sdl
  /utf-8
  /Zi
  /permissive-
  /Zc:__cplusplus
  /Zc:preprocessor
  /W4
  /wd4715
  "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
)
target_link_options(${PROJECT_NAME} PRIVATE
  "/DEF:${SOURCE_DIR}/exports.def"
  "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
  "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
)
string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

option(
  POST_BUILD_COPY_PLUGIN
  "Copy ROCK Wall Climbing to a mod root."
  OFF)
set(
  COPY_PLUGIN_BASE_PATH
  ""
  CACHE STRING
  "Semicolon-separated mod roots containing F4SE/Plugins.")
if(POST_BUILD_COPY_PLUGIN)
  if(COPY_PLUGIN_BASE_PATH STREQUAL "")
    message(FATAL_ERROR
      "COPY_PLUGIN_BASE_PATH is required when POST_BUILD_COPY_PLUGIN is enabled")
  endif()
  foreach(copy_path IN LISTS COPY_PLUGIN_BASE_PATH)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "${copy_path}/F4SE/Plugins"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "${copy_path}/F4SE/Plugins/"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
        "${copy_path}/F4SE/Plugins/"
      COMMENT "Deploying ${PROJECT_NAME} to '${copy_path}'"
    )
  endforeach()
endif()

option(
  BUILD_ROCK_WALL_CLIMBING_TESTS
  "Build focused ROCK Wall Climbing policy tests."
  OFF)
if(BUILD_ROCK_WALL_CLIMBING_TESTS)
  enable_testing()
  add_executable(ROCKWallClimbingPolicyTests
    "${ROOT_DIR}/tests/ClimbingPolicyTests.cpp")
  target_compile_features(ROCKWallClimbingPolicyTests PRIVATE cxx_std_23)
  target_link_libraries(ROCKWallClimbingPolicyTests PRIVATE ROCKWallClimbingCore)
  target_compile_options(ROCKWallClimbingPolicyTests PRIVATE
    /sdl
    /utf-8
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /W4
  )
  add_test(
    NAME ROCKWallClimbingPolicyTests
    COMMAND "$<TARGET_FILE:ROCKWallClimbingPolicyTests>")
endif()
