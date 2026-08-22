find_package(lux-cxx REQUIRED COMPONENTS core memory)
if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_RUNTIME_EXTENSION_ORCHESTRATION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_RUNTIME_EXTENSION_ORCHESTRATION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_RUNTIME_EXTENSION_ORCHESTRATION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/extensions/orchestration_visibility.h
)

add_component(
    COMPONENT_NAME runtime_extension_orchestration
    NAMESPACE lux::engine::runtime
    OUTPUT_NAME lux_engine_runtime_extension_orchestration
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ModuleMetadataBatch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EngineExtensions.cpp
)

lux_classify_target(
    TARGET  runtime_extension_orchestration
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    COMPOSITION
)

component_include_directories(
    runtime_extension_orchestration
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(
    runtime_extension_orchestration
    PRIVATE LUX_RUNTIME_EXTENSION_ORCHESTRATION_LIBRARY
)

if(MSVC)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EngineExtensions.cpp
        PROPERTIES COMPILE_OPTIONS "/permissive-;/Zc:__cplusplus;/Zc:preprocessor"
    )
endif()

target_link_libraries(
    runtime_extension_orchestration
    PUBLIC
        lux::engine::runtime::runtime_extension_loader
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::core
        lux::engine::core::events
        lux::cxx::core
        lux::cxx::memory
    PRIVATE STDEXEC::stdexec
)

component_add_transitive_commands(
    runtime_extension_orchestration
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS core memory)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_extension_loader runtime_execution)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-core REQUIRED COMPONENTS events)"
)

add_subdirectory(test)
