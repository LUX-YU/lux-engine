
set(RENDER_SRCS
    src/Camera.cpp
    src/CameraHelper.cpp
)

add_module(
    MODULE_NAME                 render
    NAMESPACE                   lux::engine::function
    SOURCE_FILES                ${RENDER_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PUBLIC_LIBRARIES            lux::engine::core::math
                                lux::engine::platform::cxx
                                lux::engine::platform::window
    PROJECT_SHARED_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/shared_include
)

