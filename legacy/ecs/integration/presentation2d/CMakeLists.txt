generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ECS_PRESENTATION2D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ECS_PRESENTATION2D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ECS_PRESENTATION2D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/integration/presentation2d/visibility.h)

add_component(
    COMPONENT_NAME presentation_2d
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_presentation_2d
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/Presentation2DSystems.cpp)

component_include_directories(
    presentation_2d
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_compile_definitions(presentation_2d PRIVATE LUX_ECS_PRESENTATION2D_LIBRARY)
target_link_libraries(
    presentation_2d
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::animation_2d
        lux::engine::ecs::physics
        lux::engine::ecs::physics2d_pixel_integration
        lux::engine::ecs::pixel
        lux::engine::ecs::render_2d
        lux::engine::ecs::render_pixel_integration
        lux::engine::ecs::render_tilemap_integration
        lux::engine::ecs::tilemap
        lux::engine::ecs::transform
        lux::engine::resource::asset)
component_add_transitive_commands(
    presentation_2d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS animation_2d core physics physics2d_pixel_integration pixel render_2d render_pixel_integration render_tilemap_integration tilemap transform)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)")
lux_classify_target(
    TARGET presentation_2d
    LAYER ECS
    PRODUCT RUNTIME
    ROLE INTEGRATION)
