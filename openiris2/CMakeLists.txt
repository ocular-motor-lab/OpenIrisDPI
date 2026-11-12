# The DPI plugin for OpenIris 2 (DPI-SPEC.md): Ryan Ressmeyer's dual Purkinje imaging, whose C#
# plugin for OpenIris is in ../OpenIrisDPI, as a plugin for OpenIris 2. Built outside OpenIris 2,
# against a build of it, with its plugin kit (OpenIris 2's PLUGINS-SPEC 22). README.md says how:
#
#   cmake --preset macos -DOpenIris2_DIR=<OpenIris 2's build folder>    (or --preset windows)
#   cmake --build build --config Release
#   ctest --test-dir build -C Release
#   cmake --install build --config Release    # into the user's plugins folder
cmake_minimum_required(VERSION 3.24)
project(OpenIrisDPI_OpenIris2 VERSION 1.0.0 LANGUAGES CXX)

find_package(OpenIris2 REQUIRED)

# The module openiris-dpi, with its outputs/ (dpi/p1, dpi/p4, dpi/p4_p1, dpi/model/gaze) and its
# profiles/ ("DPI rig", "Synthetic eyes, DPI", "Rendered eyes, DPI").
openiris2_add_plugin(dpi
  DESCRIPTION "Dual Purkinje imaging: P1, the pupil, P4 and P4 − P1 (Ryan Ressmeyer's method)"
  ENTRY plugin.cpp
  SOURCES stages_dpi.cpp calibration_dpi.cpp
  INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR})

# The tests: they link the plugin's code to test its pieces, and load its module (and OpenIris 2's
# rendered eyes) to run it in the tracker.
option(OPENIRIS_DPI_BUILD_TESTS "Build the DPI plugin's tests" ON)
if(OPENIRIS_DPI_BUILD_TESTS)
  # Catch2, as OpenIris 2's tests have it: its copy if its build has one, else downloaded.
  include(FetchContent)
  if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_CATCH2 AND EXISTS ${OpenIris2_BUILD_DIR}/_deps/catch2-src/CMakeLists.txt)
    set(FETCHCONTENT_SOURCE_DIR_CATCH2 ${OpenIris2_BUILD_DIR}/_deps/catch2-src)
  endif()
  FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
    URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(Catch2)

  add_executable(dpi_tests tests/test_environment.cpp tests/test_dpi.cpp tests/test_dpi_gaze.cpp)
  if(TARGET OpenIris2::rendered)
    target_sources(dpi_tests PRIVATE tests/test_dpi_rendered.cpp)
    target_link_libraries(dpi_tests PRIVATE OpenIris2::rendered)
  endif()
  target_link_libraries(dpi_tests PRIVATE oi_plugin_dpi OpenIris2::core Catch2::Catch2WithMain openiris_warnings)
  target_compile_definitions(dpi_tests PRIVATE DPI_PLUGINS_DIR="$<TARGET_FILE_DIR:openiris-dpi>"
                                               OPENIRIS2_PLUGINS_DIR="${OpenIris2_PLUGINS_DIR}")
  add_dependencies(dpi_tests openiris-dpi)

  enable_testing()
  include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
  # Those on the rendered eyes ray trace on every core: each runs alone.
  catch_discover_tests(dpi_tests TEST_SPEC "~[rendered]" TEST_LIST dpi_fast_tests)
  catch_discover_tests(dpi_tests TEST_SPEC "[rendered]" TEST_LIST dpi_rendered_tests PROPERTIES PROCESSORS 64)
endif()
