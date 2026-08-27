generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_RESOURCE_SCRIPT_ASSET_LIBRARY
    PUBLIC_MACRO_NAME       LUX_RESOURCE_SCRIPT_ASSET_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_RESOURCE_SCRIPT_ASSET_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/asset/script/visibility.h
)

add_component(
    COMPONENT_NAME script_asset
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_resource_script_asset
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptAsset.cpp
)
component_include_directories(
    script_asset
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    script_asset
    PRIVATE LUX_RESOURCE_SCRIPT_ASSET_LIBRARY
)
target_link_libraries(
    script_asset
    PUBLIC
        lux::engine::resource::asset
        lux::engine::resource::description
)
component_add_transitive_commands(
    script_asset
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset description)"
)
lux_classify_target(
    TARGET  script_asset
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

add_executable(
    script_asset_codec_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/script_asset_codec_test.cpp
)
target_link_libraries(
    script_asset_codec_test
    PRIVATE lux::engine::resource::script_asset
)
if(MSVC)
    target_compile_options(script_asset_codec_test PRIVATE /UNDEBUG)
else()
    target_compile_options(script_asset_codec_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  script_asset_codec_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME script_asset_codec_test COMMAND script_asset_codec_test)

install_components(
    PROJECT_NAME lux-engine-resource-script
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::resource
    COMPONENTS   script_asset
)
