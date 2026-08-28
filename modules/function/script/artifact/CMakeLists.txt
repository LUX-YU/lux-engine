generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_SCRIPT_ARTIFACT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_SCRIPT_ARTIFACT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_SCRIPT_ARTIFACT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/function/script/artifact/visibility.h
)

add_component(
    COMPONENT_NAME script_artifact
    NAMESPACE      lux::engine::function
    OUTPUT_NAME    lux_engine_function_script_artifact
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptArtifact.cpp
)
component_include_directories(
    script_artifact
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    script_artifact
    PRIVATE LUX_SCRIPT_ARTIFACT_LIBRARY
)
target_link_libraries(
    script_artifact
    PUBLIC
        lux::engine::function::script_core
        lux::engine::resource::asset
        lux::engine::resource::description
)
component_add_transitive_commands(
    script_artifact
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset description)"
)
lux_classify_target(
    TARGET  script_artifact
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    script_artifact_codec_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/script_artifact_codec_test.cpp
)
target_link_libraries(
    script_artifact_codec_test
    PRIVATE lux::engine::function::script_artifact
)
if(MSVC)
    target_compile_options(script_artifact_codec_test PRIVATE /UNDEBUG)
else()
    target_compile_options(script_artifact_codec_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  script_artifact_codec_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME script_artifact_codec_test COMMAND script_artifact_codec_test)
