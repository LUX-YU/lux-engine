find_package(assimp REQUIRED)
set(RESOURCE_MODEL_SRCS
    src/ModelAssimpWrapper.cpp
)

add_module(
    MODULE_NAME                 model
    NAMESPACE                   resource
    SOURCE_FILES                ${RESOURCE_MODEL_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PROJECT_SHARED_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/private
    PUBLIC_LIBRARIES            lux::engine::platform::cxx
                                lux::engine::platform::system
                                lux::engine::core::math
                                lux::engine::resource::asset
                                assimp::assimp
)
