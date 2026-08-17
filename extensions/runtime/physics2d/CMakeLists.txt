# Use the exact reflection source selected by engine_enable_module_meta().  A
# native build generates it in its build tree; a cross build may instead reuse
# the fingerprint-checked HostTools export.  Hard-coding the target build-tree
# path here made Android compile an unrelated stale file whenever the shared
# HostTools artifact was selected for physics2d_meta.
if(NOT TARGET physics2d_meta)
    message(FATAL_ERROR
        "physics2d_extension requires LUX_BUILD_COMPONENT_META=ON"
    )
endif()
get_target_property(_physics2d_meta_sources physics2d_meta SOURCES)
set(_physics2d_component_meta_source "")
foreach(_source IN LISTS _physics2d_meta_sources)
    get_filename_component(_source_name "${_source}" NAME)
    if(_source_name STREQUAL "Physics2DComponents.meta.cpp")
        set(_physics2d_component_meta_source "${_source}")
        break()
    endif()
endforeach()
if(NOT _physics2d_component_meta_source)
    message(FATAL_ERROR
        "physics2d_meta did not publish Physics2DComponents.meta.cpp"
    )
endif()

add_library(
    physics2d_extension MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2D.cpp
    # The generated component schema belongs to the deployable runtime module.
    # Loading the DLL queues it into the same registrar transaction as the
    # WorldSystemContribution; Player no longer preloads a physics sidecar.
    ${_physics2d_component_meta_source}
)
add_library(lux::engine::extensions::physics2d ALIAS physics2d_extension)

set_target_properties(
    physics2d_extension
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "org.lux.physics2d.runtime"
)

set(_physics2d_runtime_inventory
    "$<TARGET_FILE_DIR:physics2d_extension>/$<TARGET_FILE_BASE_NAME:physics2d_extension>.runtime-files")

if(WIN32)
    add_custom_command(
        TARGET physics2d_extension
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:physics2d_extension>
                $<TARGET_FILE_DIR:physics2d_extension>
        COMMAND_EXPAND_LISTS
        COMMENT "Stage Physics2D extension runtime DLL closure"
    )
endif()

add_custom_command(
    TARGET physics2d_extension
    POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            "-DOUTPUT=${_physics2d_runtime_inventory}"
            "-DBINARY=$<TARGET_FILE:physics2d_extension>"
            "-DBINARY_KIND=LIBRARY"
            "-DSUBJECT=org.lux.physics2d.runtime"
            -P ${PROJECT_SOURCE_DIR}/cmake/WriteRuntimeDependencyInventory.cmake
    COMMENT "Inventory Physics2D extension runtime closure"
)

lux_classify_target(
    TARGET  physics2d_extension
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    EXTENSION
)

target_include_directories(
    physics2d_extension
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${LUX_GENERATE_HEADER_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(
    physics2d_extension
    PRIVATE
        LUX_PHYSICS2D_EXTENSION_LIBRARY
        # Generated meta currently uses the Function visibility token. This
        # module owns that generated entry point but must not re-export every
        # Function/ECS declaration included by the TU.
        LUX_FUNCTION_DLL_DISABLE
)

target_link_libraries(
    physics2d_extension
    PUBLIC
        lux::engine::runtime::runtime_contribution_host
        lux::engine::ecs::physics2d
        lux::engine::ecs::physics
        lux::engine::core::meta
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_PHYSICS2D_EXTENSION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_PHYSICS2D_EXTENSION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_PHYSICS2D_EXTENSION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/extensions/physics2d/visibility.h
)

install(
    TARGETS physics2d_extension
    LIBRARY DESTINATION extensions COMPONENT lux_runtime
    RUNTIME DESTINATION extensions COMPONENT lux_runtime
)
install(
    FILES ${_physics2d_runtime_inventory}
    DESTINATION extensions
    COMPONENT lux_runtime
)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION include
    COMPONENT lux_sdk
    FILES_MATCHING PATTERN "*.hpp"
)

if(TARGET toolchain_game_export AND TARGET lux_player AND TARGET render_features)
    add_executable(
        game_export_extension_smoke
        ${PROJECT_SOURCE_DIR}/engine/toolchain/game_export/test/game_export_smoke.cpp
    )
    lux_classify_target(
        TARGET  game_export_extension_smoke
        LAYER   TOOLCHAIN
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    target_link_libraries(
        game_export_extension_smoke
        PRIVATE
            lux::engine::toolchain::toolchain_game_export
            lux::engine::authoring::authoring_world
            lux::engine::resource::classic_mesh_content
    )
    lux_link_game_export_component_meta(game_export_extension_smoke)
    target_compile_definitions(
        game_export_extension_smoke
        PRIVATE
            LUX_GAME_EXPORT_TEST_WITH_EXTENSION
            "LUX_GAME_EXPORT_TEST_ASSET=\"${CMAKE_BINARY_DIR}/assets/shaders/canvas2d/group_composite.frag.luxasset\""
            "LUX_GAME_EXPORT_TEST_RUNTIME_ROOT=\"${CMAKE_BINARY_DIR}\""
            "LUX_GAME_EXPORT_TEST_EXTENSION_DIR=\"$<TARGET_FILE_DIR:physics2d_extension>\""
    )
    add_dependencies(
        game_export_extension_smoke
        lux_player
        render_features
        physics2d_extension
    )
endif()
