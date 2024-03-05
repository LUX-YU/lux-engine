set(RENDER_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/Scene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/Scene2D.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/Scene3D.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/SceneObject.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/RenderableObject.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/RenderableObject2D.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/RenderableObject3D.cpp
)

add_component(
    COMPONENT_NAME render
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${RENDER_SRCS}
                   ${GRAPHIC_API_SRCS}
)

component_include_directories(
    render
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    render
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    render
    PUBLIC
    lux::engine::platform::RHI
    lux::engine::platform::window
    lux::engine::core::math
    lux::engine::core::meta
    lux::engine::resource::asset
)
