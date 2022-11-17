set(ASSET_SRCS
    src/LuxAssetManager.cpp
)
# find_package(stduuid REQUIRED)

add_module(
    MODULE_NAME             asset
    NAMESPACE               resource
    SOURCE_FILES            ${ASSET_SRCS}
    EXPORT_INCLUDE_DIRS     include
    PUBLIC_LIBRARIES        lux::engine::platform::system
)
