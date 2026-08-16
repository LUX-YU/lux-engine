find_package(EnTT CONFIG REQUIRED)
if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()
if(NOT TARGET concurrentqueue::concurrentqueue)
    find_package(concurrentqueue CONFIG REQUIRED)
endif()

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_RUNTIME_CONTRIBUTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_RUNTIME_CONTRIBUTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_RUNTIME_CONTRIBUTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/extensions/contribution_visibility.h
)

add_component(
    COMPONENT_NAME runtime_scene_contributions
    NAMESPACE lux::engine::runtime
    OUTPUT_NAME lux_engine_runtime_scene_contributions
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneContributions.cpp
)

lux_classify_target(
    TARGET  runtime_scene_contributions
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

component_include_directories(
    runtime_scene_contributions
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(
    runtime_scene_contributions
    PRIVATE LUX_RUNTIME_CONTRIBUTION_LIBRARY
)

target_link_libraries(
    runtime_scene_contributions
    PUBLIC
        lux::engine::runtime::runtime_extension_loader
        lux::engine::ecs::core
        lux::cxx::memory
        lux::engine::core::events
        EnTT::EnTT
        concurrentqueue::concurrentqueue
)

component_add_transitive_commands(
    runtime_scene_contributions
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(concurrentqueue CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_extension_loader)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-core REQUIRED COMPONENTS events)"
)

add_component(
    COMPONENT_NAME runtime_render_effects
    NAMESPACE lux::engine::runtime
    OUTPUT_NAME lux_engine_runtime_render_effects
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderEffects.cpp
)

lux_classify_target(
    TARGET  runtime_render_effects
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

component_include_directories(
    runtime_render_effects
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(
    runtime_render_effects
    PRIVATE LUX_RUNTIME_CONTRIBUTION_LIBRARY
)

if(MSVC)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderEffects.cpp
        PROPERTIES COMPILE_OPTIONS "/permissive-;/Zc:__cplusplus;/Zc:preprocessor"
    )
endif()

target_link_libraries(
    runtime_render_effects
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::ecs::render_core
        lux::engine::function::render_client
        lux::cxx::memory
        lux::engine::core::events
        concurrentqueue::concurrentqueue
)

component_add_transitive_commands(
    runtime_render_effects
    "find_package(concurrentqueue CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_scene_contributions)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS render_core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)"
    "find_package(lux-engine-core REQUIRED COMPONENTS events)"
)

add_component(
    COMPONENT_NAME runtime_contribution_host
    NAMESPACE lux::engine::runtime
    OUTPUT_NAME lux_engine_runtime_contribution_host
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RuntimeContributionRegistrar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EngineExtensions.cpp
)

lux_classify_target(
    TARGET  runtime_contribution_host
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    COMPOSITION
)

component_include_directories(
    runtime_contribution_host
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(
    runtime_contribution_host
    PRIVATE LUX_RUNTIME_CONTRIBUTION_LIBRARY
)

if(MSVC)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EngineExtensions.cpp
        PROPERTIES COMPILE_OPTIONS "/permissive-;/Zc:__cplusplus;/Zc:preprocessor"
    )
endif()

target_link_libraries(
    runtime_contribution_host
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::runtime::runtime_render_effects
        lux::engine::runtime::runtime_execution
    PRIVATE STDEXEC::stdexec
)

component_add_transitive_commands(
    runtime_contribution_host
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_scene_contributions runtime_render_effects runtime_execution)"
)

add_subdirectory(test)
