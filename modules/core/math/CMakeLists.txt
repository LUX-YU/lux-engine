set(MATH_SRCS
    src/eigen_extend.cpp
)

find_package (Eigen3 REQUIRED)

add_module(
    MODULE_NAME         math
    NAMESPACE           core
    SOURCE_FILES        ${MATH_SRCS}
    EXPORT_INCLUDE_DIRS include
    PUBLIC_LIBRARIES    Eigen3::Eigen
                        lux::engine::platform::system
)
