find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RESOURCE_CLASSIC_MESH_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RESOURCE_CLASSIC_MESH_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RESOURCE_CLASSIC_MESH_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/classic_mesh/visibility.h
)

add_component(
    COMPONENT_NAME classic_mesh_content
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_resource_classic_mesh_content
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ClassicMeshBatchCodec.cpp
)

component_include_directories(
    classic_mesh_content
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    classic_mesh_content
    PRIVATE LUX_ENGINE_RESOURCE_CLASSIC_MESH_LIBRARY
)

target_link_libraries(
    classic_mesh_content
    PUBLIC
        lux::cxx::compile_time
        lux::engine::resource::asset_identity
    PRIVATE
        lux::engine::core::serialization
)

component_add_transitive_commands(
    classic_mesh_content
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_identity)"
)

lux_classify_target(
    TARGET  classic_mesh_content
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    classic_mesh_content_codec_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/classic_mesh_content_codec_test.cpp
)
target_link_libraries(
    classic_mesh_content_codec_test
    PRIVATE lux::engine::resource::classic_mesh_content
)
if(MSVC)
    target_compile_options(classic_mesh_content_codec_test PRIVATE /UNDEBUG)
else()
    target_compile_options(classic_mesh_content_codec_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  classic_mesh_content_codec_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME classic_mesh_content_codec_test
    COMMAND classic_mesh_content_codec_test
)
set_tests_properties(
    classic_mesh_content_codec_test
    PROPERTIES LABELS "cpu;resource;classic_mesh" TIMEOUT 30
)
