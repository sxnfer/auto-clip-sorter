cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# Work packages add files under src/ and never edit this file. The globs pick new files up, and
# CONFIGURE_DEPENDS re-runs them at build time, so no manual re-configure is needed.
if(WIN32)
  set(clip_atlas_platform_glob "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*-win.cpp")
else()
  # Non-Windows builds compile the stubs so they still build. The Win32 files never reach them.
  set(clip_atlas_platform_glob "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*-stub.cpp")
endif()

file(
  GLOB clip_atlas_sources
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/obs/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
  "${clip_atlas_platform_glob}"
)

# Headers add nothing to the build. Listing them lets IDEs show them and lets AUTOMOC see Q_OBJECT classes.
file(
  GLOB clip_atlas_headers
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/obs/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.hpp"
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp ${clip_atlas_sources} ${clip_atlas_headers})

# Headers in src/obs and src/ui include core headers by bare name, so every src/ folder is on the path.
target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src src/core src/obs src/platform src/ui
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
