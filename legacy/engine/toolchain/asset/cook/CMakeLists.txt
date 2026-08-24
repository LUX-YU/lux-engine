generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_ASSET_COOK_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_ASSET_COOK_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TOOLCHAIN_ASSET_COOK_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/asset/cook/visibility.h
)

add_component(
    COMPONENT_NAME toolchain_asset_cook
    STATIC
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_asset_cook
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/PakCook.cpp
)

lux_classify_target(
    TARGET  toolchain_asset_cook
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

component_include_directories(
    toolchain_asset_cook
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    toolchain_asset_cook
    PUBLIC LUX_ENGINE_TOOLCHAIN_ASSET_COOK_DLL_DISABLE
)

target_link_libraries(
    toolchain_asset_cook
    PUBLIC
        lux::engine::resource::asset
        lux::cxx::algorithm
    PRIVATE lux::cxx::core
)

component_add_transitive_commands(
    toolchain_asset_cook
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
)
