cmake_minimum_required(VERSION 3.25)
project(engine_content_install_consumer LANGUAGES CXX)

find_package(lux-engine-content REQUIRED COMPONENTS content)

add_executable(engine_content_install_consumer main.cpp)
target_compile_features(engine_content_install_consumer PRIVATE cxx_std_20)
target_link_libraries(
    engine_content_install_consumer
    PRIVATE lux::engine::content
)
