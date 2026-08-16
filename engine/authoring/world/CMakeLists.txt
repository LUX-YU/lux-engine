find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_AUTHORING_WORLD_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_AUTHORING_WORLD_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/authoring/world/visibility.h
)

add_component(
    COMPONENT_NAME authoring_world
    NAMESPACE      lux::engine::authoring
    OUTPUT_NAME    lux_engine_authoring_world
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldIdentifiers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldPartition.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSourceCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldDescriptorIndex.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldAuthoringTransaction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldTerrainAuthoring.cpp
)

lux_classify_target(
    TARGET  authoring_world
    LAYER   AUTHORING
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

component_include_directories(
    authoring_world
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    authoring_world
    PRIVATE LUX_ENGINE_AUTHORING_WORLD_LIBRARY
)

target_link_libraries(
    authoring_world
    PUBLIC
        lux::cxx::compile_time
        lux::engine::resource::asset_identity
        lux::engine::resource::entity_scene
        lux::engine::resource::spatial
        lux::cxx::algorithm
        lux::engine::core::serialization
        stduuid
)

component_add_transitive_commands(
    authoring_world
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)"
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_identity entity_scene spatial)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)

add_executable(
    world_source_codec_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/world_source_codec_test.cpp
)
lux_classify_target(
    TARGET  world_source_codec_test
    LAYER   AUTHORING
    PRODUCT TEST
    ROLE    DOMAIN
)
target_link_libraries(
    world_source_codec_test
    PRIVATE lux::engine::authoring::authoring_world
)
if(MSVC)
    target_compile_options(world_source_codec_test PRIVATE /UNDEBUG)
else()
    target_compile_options(world_source_codec_test PRIVATE -UNDEBUG)
endif()
add_test(NAME world_source_codec_test COMMAND world_source_codec_test)
set_tests_properties(
    world_source_codec_test
    PROPERTIES LABELS "cpu;authoring;world" TIMEOUT 30
)

add_executable(
    world_descriptor_index_scale_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/world_descriptor_index_scale_test.cpp
)
lux_classify_target(
    TARGET  world_descriptor_index_scale_test
    LAYER   AUTHORING
    PRODUCT TEST
    ROLE    DOMAIN
)
target_link_libraries(
    world_descriptor_index_scale_test
    PRIVATE lux::engine::authoring::authoring_world
)
if(MSVC)
    target_compile_options(world_descriptor_index_scale_test PRIVATE /UNDEBUG)
else()
    target_compile_options(world_descriptor_index_scale_test PRIVATE -UNDEBUG)
endif()
add_test(
    NAME world_descriptor_index_scale_test
    COMMAND world_descriptor_index_scale_test
)
set_tests_properties(
    world_descriptor_index_scale_test
    PROPERTIES LABELS "cpu;authoring;world;scale" TIMEOUT 300
)

add_executable(
    world_terrain_authoring_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/world_terrain_authoring_test.cpp
)
lux_classify_target(
    TARGET  world_terrain_authoring_test
    LAYER   AUTHORING
    PRODUCT TEST
    ROLE    DOMAIN
)
target_link_libraries(
    world_terrain_authoring_test
    PRIVATE lux::engine::authoring::authoring_world
)
if(MSVC)
    target_compile_options(world_terrain_authoring_test PRIVATE /UNDEBUG)
else()
    target_compile_options(world_terrain_authoring_test PRIVATE -UNDEBUG)
endif()
add_test(
    NAME world_terrain_authoring_test
    COMMAND world_terrain_authoring_test
)
set_tests_properties(
    world_terrain_authoring_test
    PROPERTIES LABELS "cpu;authoring;world;terrain" TIMEOUT 30
)
