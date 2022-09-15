set(STB_SOURCE_DIR            D:/Code/lux-game/prebuild/source/stb/stb-master)

set(MEDIA_LOADER_SRCS
    src/ImageStbwrapper.cpp
)

add_module(
    MODULE_NAME                 media_loaders
    NAMESPACE                   lux::engine::platform
    SOURCE_FILES                ${MEDIA_LOADER_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PROJECT_SHARED_INCLUDE_DIRS ${STB_SOURCE_DIR}
    PUBLIC_LIBRARIES            lux::engine::platform::cxx
)
