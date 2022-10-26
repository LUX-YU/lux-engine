add_module(
    MODULE_NAME         no_module
    NAMESPACE           tools
    SOURCE_FILES        src/empty.cpp
    EXPORT_INCLUDE_DIRS include
    PUBLIC_LIBRARIES    lux::engine::platform::system
)
