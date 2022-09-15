set(MATH_SRCS
    src/math.cpp
    src/EigenTools.cpp
)

find_package (Eigen3 REQUIRED)

add_module(
    MODULE_NAME         math
    NAMESPACE           lux::engine::core
    SOURCE_FILES        ${MATH_SRCS}
    EXPORT_INCLUDE_DIRS include
    PUBLIC_LIBRARIES    Eigen3::Eigen
)
