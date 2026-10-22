include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/TargetClassification.cmake)

add_component(
    COMPONENT_NAME engine_rendering
    NAMESPACE lux::engine
    STATIC
    SOURCE_FILES src/RenderRequester.cpp
)

component_include_directories(
    engine_rendering
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

target_compile_features(engine_rendering PUBLIC cxx_std_20)

target_link_libraries(engine_rendering PUBLIC lux::engine::function::render)

component_add_transitive_commands(
    engine_rendering
    "find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)"
)

lux_classify_target(TARGET engine_rendering LAYER ENGINE PRODUCT RUNTIME ROLE LIBRARY)

lux_engine_install_components(
    PROJECT_NAME lux-engine-rendering
    VERSION 0.0.1
    NAMESPACE lux::engine
    COMPONENTS engine_rendering
)
