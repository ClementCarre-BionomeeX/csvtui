cmake_minimum_required(VERSION 3.16)
project(csvtui VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CSVTUI_BUILD_TESTS "Build the unit tests" ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui.git
  GIT_TAG v5.0.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui)

# Parsing and file access, free of any UI dependency so tests can drive them.
add_library(csvtui_core STATIC
  src/csv_parser.cpp
  src/csv_system.cpp
  src/csv_cache.cpp
  src/csv_model.cpp
  src/csv_scan.cpp
  src/csv_sortrun.cpp
  # The view is here rather than in the executable so the tests can render it
  # off-screen and compare the result against a golden file.
  src/csv_view.cpp
)
target_include_directories(csvtui_core PUBLIC src)
target_link_libraries(csvtui_core PUBLIC ftxui::screen ftxui::dom Threads::Threads)

add_executable(csvtui
  src/main.cpp
  src/csv_controller.cpp
)
target_link_libraries(csvtui PRIVATE
  csvtui_core
  ftxui::dom
  ftxui::component
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(csvtui_core PRIVATE -Wall -Wextra)
  target_compile_options(csvtui PRIVATE -Wall -Wextra)
endif()

install(TARGETS csvtui RUNTIME DESTINATION bin)
install(FILES doc/csvtui.1 DESTINATION share/man/man1 OPTIONAL)

if(CSVTUI_BUILD_TESTS)
  enable_testing()
  add_executable(csvtui_tests
    tests/test_main.cpp
    tests/test_parser.cpp
    tests/test_model.cpp
    tests/test_limits.cpp
    tests/test_scan.cpp
    tests/test_sortrun.cpp
    tests/test_cache.cpp
    tests/test_view.cpp
  )
  target_link_libraries(csvtui_tests PRIVATE csvtui_core)
  target_include_directories(csvtui_tests PRIVATE tests)
  # So the snapshot tests can find their golden files wherever they are run.
  target_compile_definitions(csvtui_tests PRIVATE
    CSVTUI_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME csvtui_tests COMMAND csvtui_tests)
endif()

# libFuzzer needs Clang. The crash that shipped was undefined behaviour on a
# byte value, so build this with -fsanitize=address,undefined as well or it
# will happily run straight past the thing it is looking for.
option(CSVTUI_BUILD_FUZZERS "Build the libFuzzer targets (requires Clang)" OFF)
if(CSVTUI_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "CSVTUI_BUILD_FUZZERS requires Clang")
  endif()
  add_executable(fuzz_parser tests/fuzz/fuzz_parser.cpp)
  target_link_libraries(fuzz_parser PRIVATE csvtui_core)
  target_compile_options(fuzz_parser PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer)
endif()
