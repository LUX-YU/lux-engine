cmake_minimum_required(VERSION 3.25)
project(core_meta_install_consumer LANGUAGES CXX)

find_package(lux-engine-core REQUIRED COMPONENTS meta serialization)

add_executable(core_meta_install_consumer main.cpp)
target_compile_features(core_meta_install_consumer PRIVATE cxx_std_20)
target_link_libraries(
    core_meta_install_consumer
    PRIVATE
        lux::engine::core::meta
        lux::engine::core::serialization
)
