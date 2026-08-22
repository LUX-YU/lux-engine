## core::serialization — domain-neutral binary serialization primitives.
##
## Provides:
##   - ArchiveReader / ArchiveWriter (byte-level POD + string I/O over a byte buffer)
##   - NameTable (string interning table for binary file headers)
##
## This target deliberately has no reflection, Eigen, ECS, scene, asset,
## runtime, render or editor knowledge. Domain formats build on this foundation.

find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS binary)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_CORE_SERIALIZATION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_CORE_SERIALIZATION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_CORE_SERIALIZATION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/core/serialization/visibility.h
)

set(LUX_SERIALIZE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Archive.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NameTable.cpp
)

add_component(
    COMPONENT_NAME  serialization
    NAMESPACE       lux::engine::core
    OUTPUT_NAME     lux_engine_core_serialization
    SOURCE_FILES    ${LUX_SERIALIZE_SRCS}
)

component_include_directories(
    serialization
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    serialization
    PRIVATE
    LUX_CORE_SERIALIZATION_LIBRARY
)

target_link_libraries(
    serialization
    PUBLIC
    lux::cxx::binary
    stduuid
)

component_add_transitive_commands(
    serialization
    "find_package(lux-cxx REQUIRED COMPONENTS binary)"
    "find_package(stduuid CONFIG REQUIRED)"
)

lux_classify_target(
    TARGET  serialization
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    serialization_name_table_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/name_table_test.cpp
)
target_link_libraries(
    serialization_name_table_test
    PRIVATE lux::engine::core::serialization
)
if(MSVC)
    target_compile_options(serialization_name_table_test PRIVATE /UNDEBUG)
else()
    target_compile_options(serialization_name_table_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  serialization_name_table_test
    LAYER   CORE
    PRODUCT TEST
    ROLE    FOUNDATION
)
