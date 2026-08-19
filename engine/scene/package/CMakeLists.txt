# Engine-owned LXSC scene package contract. The public API speaks in
# SceneFeatureId and ECS scene-format identities; the historical Resource
# manifest is used only by the private compatibility adapter while migration is
# in progress.

find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time core)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_PACKAGE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_PACKAGE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_PACKAGE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/package/visibility.h
)

add_component(
    COMPONENT_NAME scene_package
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_package
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScenePackageCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/LegacyEntitySceneAdapter.cpp
)

component_include_directories(
    scene_package
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(
    scene_package
    PRIVATE LUX_ENGINE_SCENE_PACKAGE_LIBRARY
)

target_link_libraries(
    scene_package
    PUBLIC
        lux::cxx::algorithm
        lux::cxx::compile_time
        lux::cxx::core
        lux::engine::scene::scene_api
        lux::engine::ecs::identity
        lux::engine::ecs::scene_format
        lux::engine::extensions::extension_api
    PRIVATE
        lux::engine::resource::entity_scene
)

component_add_transitive_commands(
    scene_package
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time core)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_api)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS identity scene_format)"
    "find_package(lux-engine-extensions REQUIRED COMPONENTS extension_api)"
)

lux_classify_target(
    TARGET  scene_package
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

# Public-link probe: this target intentionally does not link the legacy
# resource/entity_scene component. On Windows it therefore verifies that the
# scene_package DLL exports its canonical codec entry points and produces an
# import library.
add_executable(
    scene_package_public_link_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/scene_package_public_link_test.cpp
)
target_link_libraries(
    scene_package_public_link_test
    PRIVATE lux::engine::scene::scene_package
)
if(MSVC)
    target_compile_options(scene_package_public_link_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_package_public_link_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  scene_package_public_link_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)

# Compatibility contract: this test additionally links the frozen Resource
# codec so it can compare canonical ScenePackage bytes with the legacy LXSC
# implementation during the migration window.
add_executable(
    scene_package_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/scene_package_contract_test.cpp
)
target_link_libraries(
    scene_package_contract_test
    PRIVATE
        lux::engine::scene::scene_package
        lux::engine::resource::entity_scene
)
if(MSVC)
    target_compile_options(scene_package_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_package_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  scene_package_contract_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)
