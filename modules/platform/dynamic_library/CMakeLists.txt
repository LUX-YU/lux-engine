## Component: dynamic_library
## ---------------------------
## Cross-platform RAII wrapper over the OS dynamic-library loader, with a
## first-class `load_from_memory()` entry point. This component is the
## platform-layer dependency that script::native (and any other engine
## subsystem that needs to dlopen() compiled artefacts at runtime) consumes.
##
## Backends:
##   - Windows: LoadLibraryExW for path; per-process tmp-file for memory
##     loads (write→close→load: the loader shares no WRITE access, and
##     DELETE_ON_CLOSE would make its open delete-pending — cleanup is
##     explicit on unload + a dead-pid scratch-dir sweep per run). When
##     LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE=ON, a bundled MemoryModule
##     PE-loader (third_party/MemoryModule, BSD-2-Clause) is used instead
##     for true zero-disk loading.
##   - Linux: dlopen for path; memfd_create + dlopen("/proc/self/fd/N") for
##     memory loads, with mkstemp fallback on old kernels.
##   - macOS: dlopen for path; mkstemp + unlink for memory loads (dyld has no
##     public from-memory API).

option(LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE
       "Use the bundled MemoryModule PE-loader for true zero-disk in-memory loading on Windows"
       OFF)

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_PLATFORM_DYNAMIC_LIBRARY_LIBRARY
    PUBLIC_MACRO_NAME   LUX_PLATFORM_DYNAMIC_LIBRARY_PUBLIC
    GENERATE_FILE_PATH  lux/engine/dynamic_library/visibility.h
)

if(WIN32)
    set(LUX_DYNAMIC_LIBRARY_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/DynamicLibrary.win32.cpp
    )
else()
    set(LUX_DYNAMIC_LIBRARY_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/DynamicLibrary.posix.cpp
    )
endif()

if(WIN32 AND LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE)
    list(APPEND LUX_DYNAMIC_LIBRARY_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/MemoryModule/MemoryModule.c
    )
endif()

add_component(
    COMPONENT_NAME  dynamic_library
    NAMESPACE       lux::engine::platform
    SOURCE_FILES    ${LUX_DYNAMIC_LIBRARY_SRCS}
)

lux_classify_target(
    TARGET  dynamic_library
    LAYER   PLATFORM
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

component_include_directories(
    dynamic_library
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
    PRIVATE
        pinclude
)

if(WIN32 AND LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE)
    target_include_directories(
        dynamic_library
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/MemoryModule
    )
    target_compile_definitions(
        dynamic_library
        PRIVATE LUX_DYNAMIC_LIBRARY_USE_MEMORY_MODULE
    )
endif()

target_compile_definitions(
    dynamic_library
    PRIVATE
    LUX_PLATFORM_DYNAMIC_LIBRARY_LIBRARY
)

if(WIN32)
    target_link_libraries(dynamic_library PRIVATE kernel32)
else()
    target_link_libraries(
        dynamic_library
        PRIVATE
            ${CMAKE_DL_LIBS}
            lux::cxx::core
    )
endif()

component_add_transitive_commands(
    dynamic_library
    "find_package(lux-cxx REQUIRED COMPONENTS core)"
)
