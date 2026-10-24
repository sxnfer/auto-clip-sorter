find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
find_path(OBS_INCLUDE_DIR obs-module.h PATH_SUFFIXES obs REQUIRED)

add_executable(
  activity_ui_tests
  test-activity-status.cpp
  test-clip-filer.cpp
  host-stubs.cpp
  ../../src/ui/activity-status.cpp
  ../../src/obs/clip-filer.cpp
  ../../src/core/activity-model.cpp
  ../../src/core/file-mover.cpp
  ../../src/core/clip-layout.cpp
)
target_include_directories(
  activity_ui_tests
  PRIVATE ../../src ../../src/core ../../src/obs ../../src/ui ../../src/platform ${OBS_INCLUDE_DIR}
)
if(WIN32)
  target_sources(activity_ui_tests PRIVATE ../../src/platform/motion-preference-win.cpp)
else()
  target_sources(activity_ui_tests PRIVATE ../../src/platform/motion-preference-stub.cpp)
endif()
target_compile_definitions(
  activity_ui_tests
  PRIVATE CLIP_ATLAS_LOCALE="${CMAKE_CURRENT_SOURCE_DIR}/../../data/locale/en-US.ini"
)
target_link_libraries(activity_ui_tests PRIVATE Qt6::Widgets Qt6::Test Catch2::Catch2)
if(MSVC)
  target_compile_options(activity_ui_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(activity_ui_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME activity_ui_tests COMMAND activity_ui_tests)
set_tests_properties(activity_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
