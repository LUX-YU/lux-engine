set(RENDER_SRCS
    src/render/RenderPipeline.cpp
)

add_component(
    COMPONENT_NAME                  render
    NAMESPACE                       lux::engine::function
    SOURCE_FILES                    ${RENDER_SRCS}
                                    ${GRAPHIC_API_SRCS}

    BUILD_TIME_EXPORT_INCLUDE_DIRS  ${CMAKE_CURRENT_SOURCE_DIR}/include
                                    ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME_INCLUDE_PREFIX     include

    PUBLIC_LIBRARIES                lux::engine::core::math
                                    lux::engine::platform::RHI
                                    lux::engine::platform::window
                                    lux::engine::resource::asset
    PRIVATE_DEFINITIONS             LUX_FUNCTION_LIBRARY
)
