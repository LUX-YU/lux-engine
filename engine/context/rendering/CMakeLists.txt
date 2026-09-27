add_component(COMPONENT_NAME engine_context_render NAMESPACE lux::engine STATIC SOURCE_FILES src/EngineRendering.cpp src/RenderContext.cpp)
component_include_directories(engine_context_render BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(engine_context_render PUBLIC lux::engine::engine_context lux::engine::function::render_runtime lux::engine::scene::scene_render)
target_link_libraries(engine_context_render PRIVATE lux::engine::core::log)
component_add_transitive_commands(engine_context_render
    "find_package(lux-engine-context REQUIRED COMPONENTS engine_context)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_runtime)"
    "find_package(lux-engine-core REQUIRED COMPONENTS log)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)")
lux_classify_target(TARGET engine_context_render LAYER ENGINE PRODUCT RUNTIME ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-context-render VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine COMPONENTS engine_context_render)
