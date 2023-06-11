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

add_component(
    COMPONENT_NAME                  asset
    NAMESPACE                       lux::engine::resource
    SOURCE_FILES                    ${ASSET_SRCS}
    # include directories
	BUILD_TIME_EXPORT_INCLUDE_DIRS  ${CMAKE_CURRENT_SOURCE_DIR}/include
                                    ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED_INCLUDE_DIRS  ${STB_SOURCE_DIR}
    INSTALL_TIME_INCLUDE_PREFIX     include
    PRIVATE_INCLUDE_DIRS            pinclude
    PUBLIC_LIBRARIES                lux::engine::platform::system
                                    lux::engine::core::math
                                    lux::engine::core::meta
                                    assimp::assimp
    PRIVATE_DEFINITIONS             LUX_RESOURCE_LIBRARY
    TRANSITIVE_PACKAGES_COMMANDS    "find_package(assimp REQUIRED)"
)

