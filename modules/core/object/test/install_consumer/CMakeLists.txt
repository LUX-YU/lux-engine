cmake_minimum_required(VERSION 3.25)
project(lux_object_installed_consumer LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-core REQUIRED COMPONENTS meta object)

include_component_cmake_scripts(lux::engine::core::meta)

engine_add_meta(
    NAME installed_object_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/InstalledObject.hpp
    REGISTER_FUNC_NAME lux_installed_object
)

add_executable(lux_object_installed_consumer main.cpp)
target_compile_features(lux_object_installed_consumer PRIVATE cxx_std_20)
target_include_directories(
    lux_object_installed_consumer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(
    lux_object_installed_consumer
    PRIVATE lux::engine::core::object
)
engine_target_add_meta(
    TARGET lux_object_installed_consumer
    METAS installed_object_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)

add_test(
    NAME lux_object_installed_consumer
    COMMAND lux_object_installed_consumer
)
set_tests_properties(
    lux_object_installed_consumer
    PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:lux::engine::core::object>"
)
