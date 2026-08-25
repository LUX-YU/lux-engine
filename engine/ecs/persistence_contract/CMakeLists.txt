generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_PERSISTENCE_CONTRACT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_PERSISTENCE_CONTRACT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_PERSISTENCE_CONTRACT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/persistence_contract/visibility.h
)

add_component(
    COMPONENT_NAME persistence_contract
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_persistence_contract
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/PersistenceContract.cpp
)
component_include_directories(
    persistence_contract
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
component_add_internal_dependencies(
    persistence_contract
    core
    schema
)
target_link_libraries(
    persistence_contract
    PUBLIC lux::engine::core::serialization
)
component_add_transitive_commands(
    persistence_contract
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)
target_compile_definitions(
    persistence_contract
    PRIVATE LUX_ENGINE_ECS_PERSISTENCE_CONTRACT_LIBRARY
)
lux_classify_target(
    TARGET persistence_contract
    LAYER ECS
    PRODUCT RUNTIME
    ROLE FOUNDATION
)
