if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()

add_component(
    COMPONENT_NAME runtime_assets
    STATIC
    NAMESPACE lux::engine::runtime
    OUTPUT_NAME lux_engine_runtime_assets
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetLoadService.cpp
)

component_include_directories(
    runtime_assets
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(
    runtime_assets
    PUBLIC
        lux::engine::runtime::runtime_execution
        lux::engine::resource::asset
    PRIVATE
        STDEXEC::stdexec
        lux::engine::core::log
)

if(MSVC)
    target_compile_options(
        runtime_assets
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor)
endif()

component_add_transitive_commands(
    runtime_assets
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
)

lux_classify_target(
    TARGET  runtime_assets
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_subdirectory(tilemap)
