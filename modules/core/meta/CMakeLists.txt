set(LUX_META_SRCS
    src/LuxObject.cpp
)

add_module(
    MODULE_NAME             meta
    NAMESPACE               core
    SOURCE_FILES            ${LUX_META_SRCS}
    EXPORT_INCLUDE_DIRS     include
    PRIVATE_INCLUDE_DIRS    pinclude
    PUBLIC_LIBRARIES        lux::engine::platform::system
)
