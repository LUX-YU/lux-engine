# stb for load image
set(STB_SOURCE_DIR ${THIRD_PARTY_SOURCE_DIR}/stb/stb-master)
# assimp for load model
find_package(assimp REQUIRED)

set(ASSET_SRCS
    src/LuxAssetManager.cpp
    src/ImageAsset.cpp
    src/ModelAsset.cpp
    src/ShaderAsset.cpp
)
# find_package(stduuid REQUIRED)

add_module(
    MODULE_NAME                 asset
    NAMESPACE                   resource
    SOURCE_FILES                ${ASSET_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PROJECT_SHARED_INCLUDE_DIRS ${STB_SOURCE_DIR}
    PRIVATE_INCLUDE_DIRS        pinclude
    PUBLIC_LIBRARIES            lux::engine::platform::system
                                lux::engine::core::math
                                lux::engine::core::meta
                                assimp::assimp
)
