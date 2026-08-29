find_package(lux-cxx REQUIRED COMPONENTS algorithm)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_WORLD_STORAGE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_WORLD_STORAGE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_WORLD_STORAGE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/world/storage/visibility.h
)

add_component(
    COMPONENT_NAME world_storage
    NAMESPACE      lux::engine::world
    OUTPUT_NAME    lux_engine_world_storage
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldPartitionData.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldStorageCodec.cpp
)
add_library(lux::engine::world::storage ALIAS world_storage)

component_include_directories(
    world_storage
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    world_storage
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sinclude>
)

target_compile_definitions(world_storage PRIVATE LUX_ENGINE_WORLD_STORAGE_LIBRARY)
target_link_libraries(
    world_storage
    PUBLIC
        lux::engine::world
        lux::cxx::algorithm
    PRIVATE
        lux::engine::core::serialization
)
component_add_transitive_commands(
    world_storage
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm)"
    "find_package(lux-engine-world REQUIRED COMPONENTS world)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "if(NOT TARGET lux::engine::world::storage)"
    "add_library(lux::engine::world::storage INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::world::storage PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::world::world_storage)"
    "endif()"
)

lux_classify_target(
    TARGET  world_storage
    LAYER   WORLD
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(world_storage_test test/world_storage_test.cpp)
target_link_libraries(world_storage_test PRIVATE lux::engine::world::storage)
if(MSVC)
    target_compile_options(world_storage_test PRIVATE /UNDEBUG)
else()
    target_compile_options(world_storage_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  world_storage_test
    LAYER   WORLD
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME world_storage_test COMMAND world_storage_test)

install_components(
    PROJECT_NAME lux-engine-world-storage
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::world
    COMPONENTS   world_storage
)
