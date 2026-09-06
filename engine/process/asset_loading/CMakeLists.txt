generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_PROCESS_ASSET_LOADING_LIBRARY
    PUBLIC_MACRO_NAME       LUX_PROCESS_ASSET_LOADING_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_PROCESS_ASSET_LOADING_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/process/asset_loading/visibility.h
)

add_component(
    COMPONENT_NAME process_asset_loading
    NAMESPACE      lux::engine::process
    OUTPUT_NAME    lux_engine_process_asset_loading
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/VfsAssetReadEndpoint.cpp
)
add_library(lux::engine::process::asset_loading ALIAS process_asset_loading)
component_include_directories(
    process_asset_loading
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(process_asset_loading PRIVATE LUX_PROCESS_ASSET_LOADING_LIBRARY)
target_link_libraries(
    process_asset_loading
    PUBLIC
        lux::engine::process::process_execution
        lux::engine::resource::asset
)
component_add_transitive_commands(
    process_asset_loading
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "add_library(lux::engine::process::asset_loading INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::process::asset_loading PROPERTY
        INTERFACE_LINK_LIBRARIES lux::engine::process::process_asset_loading)"
)
lux_classify_target(
    TARGET  process_asset_loading
    LAYER   PROCESS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

install_components(
    PROJECT_NAME lux-engine-process-asset-loading
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::process
    COMPONENTS   process_asset_loading
)
