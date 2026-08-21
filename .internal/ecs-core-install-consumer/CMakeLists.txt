cmake_minimum_required(VERSION 3.25)
project(ecs_core_install_consumer LANGUAGES CXX)

find_package(lux-engine-ecs REQUIRED COMPONENTS core)

add_executable(ecs_core_install_consumer main.cpp)
target_compile_features(ecs_core_install_consumer PRIVATE cxx_std_20)
target_link_libraries(
    ecs_core_install_consumer
    PRIVATE lux::engine::ecs::core
)
