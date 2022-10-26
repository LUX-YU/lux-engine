set(STB_SOURCE_DIR ${THIRD_PARTY_SOURCE_DIR}/stb/stb-master)
set(RESOURCE_IMAGE_SRCS
    src/ImageStbwrapper.cpp
)

add_module(
    MODULE_NAME                 image
    NAMESPACE                   resource
    SOURCE_FILES                ${RESOURCE_IMAGE_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PROJECT_SHARED_INCLUDE_DIRS ${STB_SOURCE_DIR}
    PUBLIC_LIBRARIES            lux::engine::platform::cxx
                                lux::engine::resource::asset
)
