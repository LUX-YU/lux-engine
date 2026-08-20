generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_AUTHORING_ASSETS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_AUTHORING_ASSETS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_AUTHORING_ASSETS_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/authoring/assets/visibility.h
)

add_component(
    COMPONENT_NAME authoring_assets
    NAMESPACE      lux::engine::authoring
    OUTPUT_NAME    lux_engine_authoring_assets
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/LooseAssetProvider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FlowGraphCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FlowGraphSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialAuthoringCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialGraphCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialGraphDocument.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/material/Node.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/material/MaterialGraph.cpp
)

lux_classify_target(
    TARGET  authoring_assets
    LAYER   AUTHORING
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

component_include_directories(
    authoring_assets
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE pinclude
)

target_compile_definitions(
    authoring_assets
    PRIVATE LUX_ENGINE_AUTHORING_ASSETS_LIBRARY
)

target_link_libraries(
    authoring_assets
    PUBLIC
        lux::engine::resource::asset_codecs
        lux::engine::authoring::authoring_flowforge
        lux::engine::core::serialization
        lux::cxx::binary
    PRIVATE
        lux::cxx::core
)

component_add_transitive_commands(
    authoring_assets
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_codecs)"
    "find_package(lux-engine-authoring REQUIRED COMPONENTS authoring_flowforge)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-cxx REQUIRED COMPONENTS binary core)"
)

if(ENABLE_ASSET_TEST)
    add_executable(
        material_graph_codec_test
        ${CMAKE_CURRENT_SOURCE_DIR}/test/material_graph_codec_test.cpp
    )
    lux_classify_target(
        TARGET  material_graph_codec_test
        LAYER   AUTHORING
        PRODUCT TEST
        ROLE    DOMAIN
    )
    target_link_libraries(
        material_graph_codec_test
        PRIVATE authoring_assets
    )
    target_include_directories(
        material_graph_codec_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    )
endif()
