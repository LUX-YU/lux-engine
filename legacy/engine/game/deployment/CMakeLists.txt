find_package(tomlplusplus CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_GAME_LIBRARY
    PUBLIC_MACRO_NAME       LUX_GAME_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_GAME_DLL_DISABLE
    GENERATE_FILE_PATH      lux/game/visibility.h
)

add_component(
    COMPONENT_NAME deployment
    NAMESPACE      lux::game
    OUTPUT_NAME    lux_game_deployment
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/LaunchManifest.cpp
)

component_include_directories(
    deployment
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    deployment
    PRIVATE LUX_GAME_LIBRARY
)

target_link_libraries(
    deployment
    PUBLIC
        lux::cxx::compile_time
        lux::engine::function::render_client
        lux::engine::extensions::extension_api
    PRIVATE
        tomlplusplus::tomlplusplus
)

component_add_transitive_commands(
    deployment
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)"
    "find_package(lux-engine-extensions REQUIRED COMPONENTS extension_api)"
)

lux_classify_target(
    TARGET  deployment
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

install_components(
    PROJECT_NAME lux-game
    VERSION      0.0.1
    NAMESPACE    lux::game
    COMPONENTS   deployment
)
