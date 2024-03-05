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
    COMPONENT_NAME asset
    NAMESPACE      lux::engine::resource
    SOURCE_FILES   ${ASSET_SRCS}
)

component_include_directories(
    asset
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
    PRIVATE
        pinclude
)

target_compile_definitions(
    asset
    PRIVATE
    LUX_RESOURCE_LIBRARY
)

target_link_libraries(
    asset
    PUBLIC
    lux::engine::core::math
    lux::engine::core::meta
    assimp::assimp
)

component_add_transitive_commands(
    asset
    "find_package(assimp REQUIRED)"
)
