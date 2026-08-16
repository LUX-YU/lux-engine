find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RESOURCE_TILEMAP_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RESOURCE_TILEMAP_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RESOURCE_TILEMAP_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/tilemap/visibility.h
)

add_component(
    COMPONENT_NAME tilemap_content
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_resource_tilemap_content
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TilemapChunkCodec.cpp
)

component_include_directories(
    tilemap_content
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    tilemap_content
    PRIVATE LUX_ENGINE_RESOURCE_TILEMAP_LIBRARY
)

target_link_libraries(
    tilemap_content
    PUBLIC lux::cxx::compile_time
    PRIVATE lux::engine::core::serialization
)

component_add_transitive_commands(
    tilemap_content
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)

lux_classify_target(
    TARGET  tilemap_content
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    tilemap_content_codec_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/tilemap_content_codec_test.cpp
)
target_link_libraries(
    tilemap_content_codec_test
    PRIVATE lux::engine::resource::tilemap_content
)
if(MSVC)
    target_compile_options(tilemap_content_codec_test PRIVATE /UNDEBUG)
else()
    target_compile_options(tilemap_content_codec_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  tilemap_content_codec_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME tilemap_content_codec_test
    COMMAND tilemap_content_codec_test
)
set_tests_properties(
    tilemap_content_codec_test
    PROPERTIES LABELS "cpu;resource;tilemap" TIMEOUT 30
)
