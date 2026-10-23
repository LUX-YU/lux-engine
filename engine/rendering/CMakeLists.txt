include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/TargetClassification.cmake)

add_component(
    COMPONENT_NAME engine_rendering
    NAMESPACE lux::engine
    STATIC
    SOURCE_FILES
        src/RenderRequester.cpp
        src/RenderExecution.cpp
        src/RenderDiagnostics.cpp
        src/RenderAssets.cpp
)

component_include_directories(
    engine_rendering
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
    INSTALL_TIME include
)

target_compile_features(engine_rendering PUBLIC cxx_std_20)

target_link_libraries(
    engine_rendering
    PUBLIC
        lux::engine::function::render
        lux::engine::core::core_execution
        lux::engine::process::asset_loading
    PRIVATE
        lux::engine::core::log
        lux::engine::core::error
)

component_add_transitive_commands(
    engine_rendering
    "find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)"
    "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
    "find_package(lux-engine-core CONFIG REQUIRED COMPONENTS log error)"
    "find_package(lux-engine-process-asset-loading CONFIG REQUIRED COMPONENTS process_asset_loading)"
)

lux_classify_target(TARGET engine_rendering LAYER ENGINE PRODUCT RUNTIME ROLE LIBRARY)

lux_engine_install_components(
    PROJECT_NAME lux-engine-rendering
    VERSION 0.0.1
    NAMESPACE lux::engine
    COMPONENTS engine_rendering
)

if(BUILD_TESTING)
    add_executable(engine_render_transport_test test/RenderRequester.cpp)
    target_link_libraries(engine_render_transport_test PRIVATE engine_rendering)
    add_test(NAME engine.render_transport COMMAND engine_render_transport_test)
    set_tests_properties(engine.render_transport PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
endif()
