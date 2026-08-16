find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx CONFIG REQUIRED COMPONENTS reflection_runtime)

include_component_cmake_scripts(lux::cxx::reflection_runtime)

# The directory tree is the ownership map: identity is a leaf vocabulary,
# core owns the ledger/value objects, codecs own cooked serialization, and pak
# owns the runtime container provider.  Public headers intentionally retain one
# stable include prefix: lux/engine/resource/asset/.
generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ASSET_CODECS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ASSET_CODECS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ASSET_CODECS_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/asset/codecs/visibility.h
)
generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ASSET_PAK_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ASSET_PAK_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ASSET_PAK_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/asset/pak/visibility.h
)

add_subdirectory(identity)
add_subdirectory(core)
add_subdirectory(codecs)
add_subdirectory(pak)

add_executable(
    asset_codec_catalog_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/asset_codec_catalog_test.cpp
)
lux_classify_target(
    TARGET  asset_codec_catalog_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)
target_link_libraries(
    asset_codec_catalog_test
    PRIVATE lux::engine::resource::asset_codecs
)
add_test(NAME asset_codec_catalog_test COMMAND asset_codec_catalog_test)
set_tests_properties(
    asset_codec_catalog_test
    PROPERTIES LABELS "cpu;resource" TIMEOUT 30
)

set(ENABLE_ASSET_TEST OFF CACHE BOOL "Enable asset module tests")
if(ENABLE_ASSET_TEST)
    add_subdirectory(test)
endif()
