find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time memory)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_RUNTIME_EXTENSION_LOADER_LIBRARY
    PUBLIC_MACRO_NAME       LUX_RUNTIME_EXTENSION_LOADER_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_RUNTIME_EXTENSION_LOADER_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/extensions/loader_visibility.h
)

add_component(
    COMPONENT_NAME runtime_extension_loader
    NAMESPACE lux::engine::runtime
    OUTPUT_NAME lux_engine_runtime_extension_loader
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ExtensionModuleManager.cpp
)

lux_classify_target(
    TARGET  runtime_extension_loader
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

component_include_directories(
    runtime_extension_loader
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_extension_loader
    PRIVATE LUX_RUNTIME_EXTENSION_LOADER_LIBRARY
)

target_link_libraries(
    runtime_extension_loader
    PUBLIC
        lux::cxx::algorithm
        lux::cxx::compile_time
        lux::cxx::memory
        lux::engine::extensions::extension_api
        lux::engine::platform::dynamic_library
)

component_add_transitive_commands(
    runtime_extension_loader
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time memory)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)"
    "find_package(lux-engine-extensions REQUIRED COMPONENTS extension_api)"
)

if(ENABLE_EXECUTION_TEST)
    add_subdirectory(test)
endif()
