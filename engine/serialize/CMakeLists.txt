## engine/serialize — binary serialization for editor + future runtime.
##
## Provides:
##   - ArchiveReader / ArchiveWriter (byte-level POD + string I/O over a byte buffer)
##   - NameTable (string interning table for binary file headers)
##   - TaggedPropertyArchive (reflection-driven structured serialization;
##     write/read reflected structs as named/typed/sized property streams,
##     skipping unknown fields cleanly for forward compatibility)
##
## Depends on lux::engine::core::meta (RefClass/RefField iteration) and stduuid
## (asset_id_t encoding). HAS NO dependency on editor / GUI / render, so the
## future engine/runtime/ sub-target (M3b) can link it for .luxpackage
## reading without dragging the editor in. That is the architectural reason
## this lives at engine/serialize/ rather than inside engine/editor/.

find_package(stduuid CONFIG REQUIRED)
find_package(Eigen3 CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SERIALIZE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SERIALIZE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SERIALIZE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/serialize/visibility.h
)

set(LUX_SERIALIZE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Archive.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NameTable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TaggedPropertyArchive.cpp
)

add_component(
    COMPONENT_NAME  serialize
    NAMESPACE       lux::engine
    OUTPUT_NAME     lux_engine_serialize
    SOURCE_FILES    ${LUX_SERIALIZE_SRCS}
)

component_include_directories(
    serialize
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    serialize
    PRIVATE
    LUX_ENGINE_SERIALIZE_LIBRARY
)

target_link_libraries(
    serialize
    PUBLIC
    lux::engine::core::meta
    stduuid
    PRIVATE
    Eigen3::Eigen
)

component_add_transitive_commands(
    serialize
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
    "find_package(stduuid CONFIG REQUIRED)"
)

set(ENABLE_SERIALIZE_TEST OFF CACHE BOOL "Enable serialize unit tests")
if(ENABLE_SERIALIZE_TEST AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
    add_subdirectory(test)
endif()
