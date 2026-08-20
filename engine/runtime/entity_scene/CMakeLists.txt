find_package(EnTT CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time memory)
if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()
find_package(zstd CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_ENTITY_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_ENTITY_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/entity_scene/visibility.h
)

add_component(
    COMPONENT_NAME runtime_entity_scene
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_entity_scene
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityBatchDecoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/PreparedEntityBatch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SectionBlobStore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityBatchStager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityBatchMaterializer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySceneCatalog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionGeneratorCatalog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionService.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionLoaderSystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/StartupSectionSystem.cpp
)

component_include_directories(
    runtime_entity_scene
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(
    runtime_entity_scene
    PRIVATE LUX_ENGINE_RUNTIME_ENTITY_SCENE_LIBRARY
)

target_link_libraries(
    runtime_entity_scene
    PUBLIC
        lux::cxx::compile_time
        lux::cxx::memory
        lux::engine::ecs::core
        lux::engine::ecs::scene_format
        lux::engine::scene::scene_package
        lux::engine::resource::asset
        lux::engine::runtime::runtime_execution
        lux::engine::scene::scene_api
        EnTT::EnTT
        STDEXEC::stdexec
    PRIVATE
        lux::engine::core::meta
        lux::engine::core::serialization
        zstd::libzstd
)

component_add_transitive_commands(
    runtime_entity_scene
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta serialization)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core scene_format)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_api scene_package)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution)"
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(zstd CONFIG REQUIRED)"
)

lux_classify_target(
    TARGET  runtime_entity_scene
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    runtime_entity_scene_integration_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/runtime_entity_scene_integration_test.cpp
)
target_link_libraries(
    runtime_entity_scene_integration_test
    PRIVATE
        lux::engine::runtime::runtime_entity_scene
        lux::engine::core::serialization
        lux::engine::core::meta
)
if(MSVC)
    target_compile_options(runtime_entity_scene_integration_test PRIVATE /UNDEBUG)
else()
    target_compile_options(runtime_entity_scene_integration_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  runtime_entity_scene_integration_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    DOMAIN
)

add_executable(
    entity_section_loader_integration_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/entity_section_loader_integration_test.cpp
)
target_link_libraries(
    entity_section_loader_integration_test
    PRIVATE lux::engine::runtime::runtime_entity_scene
)
if(MSVC)
    target_compile_options(entity_section_loader_integration_test PRIVATE /UNDEBUG)
else()
    target_compile_options(entity_section_loader_integration_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  entity_section_loader_integration_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    DOMAIN
)

# Frozen wire contract for ECS-owned LXES v1 and Persistence Journal v1.
add_executable(
    entity_section_wire_compatibility_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/entity_section_wire_compatibility_test.cpp
)
target_link_libraries(
    entity_section_wire_compatibility_test
    PRIVATE lux::engine::ecs::scene_format
)
if(MSVC)
    target_compile_options(entity_section_wire_compatibility_test PRIVATE /UNDEBUG)
else()
    target_compile_options(entity_section_wire_compatibility_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  entity_section_wire_compatibility_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)

add_executable(
    entity_scene_catalog_identity_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/entity_scene_catalog_identity_test.cpp
)
target_link_libraries(
    entity_scene_catalog_identity_test
    PRIVATE
        lux::engine::runtime::runtime_entity_scene
        lux::engine::extensions::extension_api
        lux::engine::scene::scene_api
)
if(MSVC)
    target_compile_options(entity_scene_catalog_identity_test PRIVATE /UNDEBUG)
else()
    target_compile_options(entity_scene_catalog_identity_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  entity_scene_catalog_identity_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)

# Public API closure and ownership probe. This target intentionally includes
# only canonical Runtime EntitySection headers; any legacy Resource DTO leak
# becomes a compile error here.
add_executable(
    entity_section_public_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/entity_section_public_contract_test.cpp
)
target_link_libraries(
    entity_section_public_contract_test
    PRIVATE lux::engine::runtime::runtime_entity_scene
)
if(MSVC)
    target_compile_options(entity_section_public_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(entity_section_public_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  entity_section_public_contract_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)
