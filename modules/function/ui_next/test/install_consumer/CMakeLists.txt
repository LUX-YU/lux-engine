cmake_minimum_required(VERSION 3.25)
project(lux_ui_next_installed_consumer LANGUAGES CXX)

find_package(lux-engine-function REQUIRED COMPONENTS ui_next)

add_executable(lux_ui_next_installed_consumer main.cpp)
target_compile_features(lux_ui_next_installed_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_ui_next_installed_consumer
    PRIVATE lux::engine::function::ui_next
)
