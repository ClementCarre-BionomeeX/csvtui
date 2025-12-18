cmake_minimum_required(VERSION 3.10)
project(hextui)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(FetchContent)

# Fetch and include FTXUI
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui.git
  GIT_TAG v5.0.0
)

FetchContent_MakeAvailable(ftxui)

add_executable(csvtui src/main.cpp src/csv_model.cpp src/csv_controller.cpp src/csv_view.cpp )

target_link_libraries(csvtui PRIVATE ftxui::screen ftxui::dom ftxui::component pthread )

install(TARGETS csvtui DESTINATION bin)
