find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RESOURCE_TERRAIN_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RESOURCE_TERRAIN_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RESOURCE_TERRAIN_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/terrain/visibility.h
)

add_component(
    COMPONENT_NAME terrain_content
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_resource_terrain_content
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TerrainTileCodec.cpp
)

component_include_directories(
    terrain_content
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    terrain_content
    PRIVATE LUX_ENGINE_RESOURCE_TERRAIN_LIBRARY
)

target_link_libraries(
    terrain_content
    PUBLIC lux::cxx::compile_time
    PRIVATE lux::engine::core::serialization
)

component_add_transitive_commands(
    terrain_content
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)

lux_classify_target(
    TARGET  terrain_content
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    terrain_content_codec_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/terrain_content_codec_test.cpp
)
target_link_libraries(
    terrain_content_codec_test
    PRIVATE lux::engine::resource::terrain_content
)
if(MSVC)
    target_compile_options(terrain_content_codec_test PRIVATE /UNDEBUG)
else()
    target_compile_options(terrain_content_codec_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  terrain_content_codec_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)
