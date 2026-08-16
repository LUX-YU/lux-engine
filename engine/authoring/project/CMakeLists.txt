## Authoring workspace data shared by Editor and offline Toolchain.
##
## Pure data layer for the user's workspace:
##   - ProjectManifest: in-memory schema of a `.luxproject` TOML file
##   - Project:         resolved directory layout + manifest, scan helpers
##   - RecentProjects:  free functions over %APPDATA%/Lux/recent.txt
##
## Has NO render / editor / GUI dependencies. Both lux_editor and lux_launcher
## link this; future engine/runtime/ (M3b) could too. Lives outside engine/editor
## for that decoupling reason — same architectural rationale as engine/serialize.

find_package(stduuid CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_AUTHORING_PROJECT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_AUTHORING_PROJECT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_AUTHORING_PROJECT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/authoring/project/visibility.h
)

set(LUX_PROJECT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Project.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ProjectManifest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RecentProjects.cpp
)

add_component(
    COMPONENT_NAME  project
    NAMESPACE       lux::engine::authoring
    OUTPUT_NAME     lux_engine_authoring_project
    SOURCE_FILES    ${LUX_PROJECT_SRCS}
)

lux_classify_target(
    TARGET  project
    LAYER   AUTHORING
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

component_include_directories(
    project
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    project
    PRIVATE
    LUX_ENGINE_AUTHORING_PROJECT_LIBRARY
)

target_link_libraries(
    project
    PUBLIC
    stduuid
    lux::cxx::compile_time
    lux::engine::core::extension_abi
    PRIVATE
    tomlplusplus::tomlplusplus
)

component_add_transitive_commands(
    project
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS extension_abi)"
)
