cmake_minimum_required(VERSION 3.24)
project(ActivityScope VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)

find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd libudev)

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(ascore STATIC ${CORE_SOURCES})
target_include_directories(ascore PUBLIC src/core)
target_link_libraries(ascore PUBLIC SQLite3::SQLite3 PkgConfig::SYSTEMD ${CMAKE_DL_LIBS})

option(AS_BUILD_APP "Build the Qt application" ON)
if(AS_BUILD_APP AND EXISTS ${CMAKE_SOURCE_DIR}/src/app/main.cpp)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network DBus Concurrent)
  qt_standard_project_setup()
  file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp src/app/*.h)
  qt_add_executable(activityscope ${APP_SOURCES})
  qt_add_resources(activityscope assets PREFIX "/" FILES
    assets/fonts/Oxanium.ttf
    assets/icons/layout-dashboard.svg assets/icons/activity.svg assets/icons/list-tree.svg
    assets/icons/settings.svg assets/icons/cpu.svg assets/icons/memory-stick.svg assets/icons/monitor.svg
    assets/icons/hard-drive.svg assets/icons/globe.svg assets/icons/zap.svg assets/icons/thermometer.svg
    assets/icons/panel-left.svg assets/icons/search.svg assets/icons/chevron-right.svg
    assets/icons/chevron-down.svg assets/icons/palette.svg assets/icons/gauge.svg assets/icons/server.svg
    assets/icons/info.svg assets/icons/power.svg assets/icons/users.svg assets/icons/cable.svg
    assets/icons/package.svg assets/icons/chart-pie.svg assets/icons/cog.svg assets/icons/scroll-text.svg assets/icons/rocket.svg assets/icons/download.svg assets/icons/help-circle.svg)
  target_compile_definitions(activityscope PRIVATE AS_VERSION="${PROJECT_VERSION}")
  target_link_libraries(activityscope PRIVATE ascore Qt6::Widgets Qt6::Svg Qt6::Network Qt6::DBus Qt6::Concurrent)
  install(TARGETS activityscope RUNTIME DESTINATION bin)
  install(FILES packaging/activityscope.desktop DESTINATION share/applications)
  install(FILES assets/activityscope.svg DESTINATION share/icons/hicolor/scalable/apps)
  install(PROGRAMS tools/hindsight DESTINATION bin)
  install(PROGRAMS vendor/thatsweird/bin/weird DESTINATION share/activityscope/thatsweird/bin)
  install(FILES vendor/thatsweird/package.json vendor/thatsweird/LICENSE DESTINATION share/activityscope/thatsweird)
endif()

enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
if(TEST_SOURCES)
  add_executable(ascore_tests ${TEST_SOURCES})
  target_link_libraries(ascore_tests PRIVATE ascore)
  target_compile_definitions(ascore_tests PRIVATE FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
  add_test(NAME core COMMAND ascore_tests)
endif()

add_executable(asprobe EXCLUDE_FROM_ALL tools/probe.cpp)
target_link_libraries(asprobe PRIVATE ascore)
