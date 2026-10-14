find_package(lux-cxx CONFIG REQUIRED COMPONENTS compile_time concurrent)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_PLATFORM_IPC_LIBRARY
    PUBLIC_MACRO_NAME LUX_PLATFORM_IPC_PUBLIC
    GENERATE_FILE_PATH lux/engine/platform/ipc/visibility.h
)

set(LUX_IPC_INLINE_CAPACITY 128 CACHE STRING "IPC inline bytes (rebuild all consumers when changed)")

add_component(
    COMPONENT_NAME platform_ipc
    NAMESPACE lux::engine::platform
    OUTPUT_NAME lux_engine_platform_ipc
    SOURCE_FILES src/Ipc.cpp
)

component_include_directories(
    platform_ipc
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}"
    INSTALL_TIME include
    PRIVATE pinclude
)

target_compile_definitions(platform_ipc PRIVATE LUX_PLATFORM_IPC_LIBRARY)
target_compile_definitions(platform_ipc PUBLIC LUX_IPC_INLINE_CAPACITY=${LUX_IPC_INLINE_CAPACITY})
target_link_libraries(platform_ipc PUBLIC lux::cxx::compile_time PRIVATE lux::cxx::concurrent)
target_compile_features(platform_ipc PUBLIC cxx_std_20)

component_add_transitive_commands(platform_ipc "find_package(lux-cxx CONFIG REQUIRED COMPONENTS compile_time)")

if(BUILD_TESTING)
    target_compile_definitions(platform_ipc PRIVATE LUX_IPC_TESTING)
    target_sources(platform_ipc PRIVATE test/Allocation.cpp)
    target_include_directories(platform_ipc PRIVATE test)
    add_subdirectory(test)
endif()
