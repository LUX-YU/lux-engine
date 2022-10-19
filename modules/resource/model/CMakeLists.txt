find_package(assimp REQUIRED)
set(RESOURCE_MODEL_SRCS
    src/ModelAssimpWrapper.cpp
)

add_module(
    MODULE_NAME                 model
    NAMESPACE                   lux::engine::resource
    SOURCE_FILES                ${RESOURCE_MODEL_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PROJECT_SHARED_INCLUDE_DIRS ${STB_SOURCE_DIR}
    PUBLIC_LIBRARIES            lux::engine::platform::cxx
                                lux::engine::core::math
                                assimp::assimp
)
