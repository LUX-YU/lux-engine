# stb for load image
if(ENABLE_SUPERBUILD)
    set(STB_SOURCE_DIR ${THIRD_PARTY_SOURCE_DIR}/stb/stb-master)
else()
    find_path(STB_SOURCE_DIR "stb_image.h" REQUIRED 
        HINTS "/usr/include/stb" "/usr/local/include/stb"
        "/usr/include" "/usr/local/include"
    )

    find_path(STB_SOURCE_DIR "stb_image.h" REQUIRED
            HINTS "/usr/include/stb" "/usr/local/include/stb"
            "/usr/include" "/usr/local/include"
    )
endif()

# assimp for load model
find_package(assimp REQUIRED)
# meshoptimizer for import-time discrete-LOD generation (meshopt_simplify)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx CONFIG REQUIRED COMPONENTS reflection_runtime)
find_package(spirv_cross_core CONFIG REQUIRED)
# 尝试 Vulkan SDK
find_package(Vulkan QUIET COMPONENTS shaderc_combined)
# 尝试 vcpkg
find_package(unofficial-shaderc CONFIG QUIET)
# 尝试 Conan
find_package(shaderc CONFIG QUIET)
add_library(shaderc_bridge INTERFACE)

if (TARGET unofficial::shaderc::shaderc)
    message(STATUS "Using shaderc from vcpkg")
  target_link_libraries(shaderc_bridge INTERFACE unofficial::shaderc::shaderc)
elseif (TARGET Vulkan::shaderc_combined)
    message(STATUS "Using shaderc from Vulkan SDK")
    target_link_libraries(shaderc_bridge INTERFACE Vulkan::shaderc_combined)
elseif (TARGET shaderc::shaderc)
    message(STATUS "Using shaderc from conan")
  target_link_libraries(shaderc_bridge INTERFACE shaderc::shaderc)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PC_SHADERC REQUIRED shaderc)
  target_include_directories(shaderc_bridge INTERFACE ${PC_SHADERC_INCLUDE_DIRS})
  target_link_libraries(shaderc_bridge INTERFACE ${PC_SHADERC_LIBRARIES})
endif()

# 统一一个本地别名，后面代码都用 shaderc::shaderc
add_library(shaderc::shaderc ALIAS shaderc_bridge)

# bc7enc_rdo for BC1-7 texture compression (vendored source)
set(BC7ENC_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bc7enc_rdo)
add_library(bc7enc_rdo STATIC
    ${BC7ENC_SOURCE_DIR}/rgbcx.cpp
    ${BC7ENC_SOURCE_DIR}/bc7enc.cpp
)
target_include_directories(bc7enc_rdo PUBLIC ${BC7ENC_SOURCE_DIR})
target_compile_definitions(bc7enc_rdo PRIVATE RGBCX_IMPLEMENTATION)
set_target_properties(bc7enc_rdo PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(bc7enc_rdo PRIVATE /W0)
else()
    target_compile_options(bc7enc_rdo PRIVATE -w)
endif()

# For target_add_meta
include_component_cmake_scripts(
    lux::cxx::reflection_runtime
)

set(ASSET_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MeshAsset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ModelAsset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderAsset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptAsset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptDescriptionCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TextureAsset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetSerDeserFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VirtualPath.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetVfs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LooseDirProvider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PakCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PakAssetProvider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PakCook.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TextureSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialGraphCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialInstanceSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MeshSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MeshDescriptionCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ModelSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkeletonAsset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkeletonSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkeletonDescriptionCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AnimationClipAsset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AnimationClipSerDeser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AnimationClipDescriptionCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Sprite2DAssets.cpp
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetSerDser.cpp
)

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
        ${STB_SOURCE_DIR}
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
    stduuid
    PRIVATE
    assimp::assimp
    meshoptimizer::meshoptimizer
    lux::cxx::reflection_runtime
    shaderc::shaderc
    spirv-cross-core
    bc7enc_rdo
)

component_add_internal_dependencies(
    asset
    description
)

component_add_cmake_scripts(
	asset
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/add_asset.cmake
)

# lux_asset_packer — the build-time asset packing tool that drove src/AssetTool.cpp —
# moved to engine/asset_pipeline/. It links resource::asset from the editor tier;
# resource::asset itself no longer defines that executable target.

component_add_transitive_commands(
    asset
    "find_package(stduuid CONFIG  REQUIRED)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta math)"
)

set(ENABLE_ASSET_TEST OFF CACHE BOOL "Enable asset module tests")
if(ENABLE_ASSET_TEST)
    add_subdirectory(test)
endif()
