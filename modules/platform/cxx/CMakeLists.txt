add_module(
    MODULE_NAME         cxx
    NAMESPACE           platform
    SOURCE_FILES        src/SubProgram.cpp
    EXPORT_INCLUDE_DIRS include
    PUBLIC_LIBRARIES    lux::engine::platform::system
)
