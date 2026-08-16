find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RESOURCE_ENTITY_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RESOURCE_ENTITY_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/entity_scene/visibility.h
)

add_component(
    COMPONENT_NAME entity_scene
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_resource_entity_scene
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityPersistenceJournal.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySceneValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySceneManifestCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionCodec.cpp
)

component_include_directories(
    entity_scene
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    entity_scene
    PRIVATE LUX_ENGINE_RESOURCE_ENTITY_SCENE_LIBRARY
)

target_link_libraries(
    entity_scene
    PUBLIC
        lux::cxx::compile_time
        lux::cxx::algorithm
        lux::engine::core::extension_abi
        stduuid
    PRIVATE
        lux::engine::core::serialization
        lux::engine::resource::asset_core
)

component_add_transitive_commands(
    entity_scene
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS extension_abi)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_core)"
    "find_package(stduuid CONFIG REQUIRED)"
)

lux_classify_target(
    TARGET  entity_scene
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    entity_scene_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/entity_scene_contract_test.cpp
)
target_link_libraries(
    entity_scene_contract_test
    PRIVATE lux::engine::resource::entity_scene
)
if(MSVC)
    target_compile_options(entity_scene_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(entity_scene_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  entity_scene_contract_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME entity_scene_contract_test COMMAND entity_scene_contract_test)
set_tests_properties(
    entity_scene_contract_test
    PROPERTIES LABELS "cpu;resource;entity_scene" TIMEOUT 30
)
