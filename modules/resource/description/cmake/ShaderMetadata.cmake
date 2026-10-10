include_guard(GLOBAL)

# Header-only neutral contract boundary; no Math, Script, Asset or Engine SDK.
add_library(shader_metadata INTERFACE)

add_library(lux::engine::resource::shader_metadata ALIAS shader_metadata)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_RESOURCE_LIBRARY
    PUBLIC_MACRO_NAME LUX_RESOURCE_PUBLIC
    GENERATE_FILE_PATH lux/engine/resource/visibility.h
)

target_include_directories(
    shader_metadata INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>"
    "$<BUILD_INTERFACE:${LUX_GENERATE_HEADER_DIR}>"
    "$<INSTALL_INTERFACE:include>"
)
