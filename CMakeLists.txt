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
  src/csv_model.cpp
  src/csv_index.cpp
)
target_include_directories(csvtui_core PUBLIC src)
target_link_libraries(csvtui_core PUBLIC ftxui::screen Threads::Threads)

add_executable(csvtui
  src/main.cpp
  src/csv_view.cpp
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
  )
  target_link_libraries(csvtui_tests PRIVATE csvtui_core)
  target_include_directories(csvtui_tests PRIVATE tests)
  add_test(NAME csvtui_tests COMMAND csvtui_tests)
endif()
