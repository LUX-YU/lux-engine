set(RENDER_SRCS
    src/render/RenderPipeline.cpp
)

add_module(
    MODULE_NAME                 render
    NAMESPACE                   function
    SOURCE_FILES                ${GRAPHIC_API_SRCS}
                                ${RENDER_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PRIVATE_INCLUDE_DIRS        ${CMAKE_PREFIX_PATH}/include
    PUBLIC_LIBRARIES            lux::engine::core::math
                                lux::engine::platform::RHI
                                lux::engine::platform::system
                                lux::engine::platform::window
                                lux::engine::resource::asset
                                
)
