include_guard(GLOBAL)

set(_LUX_ARCH_LAYERS
    PLATFORM CORE RESOURCE FUNCTION ECS
    RUNTIME AUTHORING TOOLCHAIN EDITOR HOST
)
set(_LUX_ARCH_PRODUCTS
    RUNTIME PLAYER EDITOR TOOLCHAIN BUILD_TOOL TEST
)
set(_LUX_ARCH_ROLES
    FOUNDATION DOMAIN INTEGRATION COMPOSITION EXTENSION
)

function(_lux_arch_require_value kind value allowed)
    list(FIND ${allowed} "${value}" value_index)
    if(value_index EQUAL -1)
        message(FATAL_ERROR
            "lux_classify_target: invalid ${kind} '${value}'; expected one of: ${${allowed}}"
        )
    endif()
endfunction()

function(lux_classify_target)
    cmake_parse_arguments(ARG "" "TARGET;LAYER;PRODUCT;ROLE" "" ${ARGN})
    foreach(required TARGET LAYER PRODUCT ROLE)
        if(NOT ARG_${required})
            message(FATAL_ERROR "lux_classify_target: ${required} is required")
        endif()
    endforeach()
    if(NOT TARGET ${ARG_TARGET})
        message(FATAL_ERROR
            "lux_classify_target: '${ARG_TARGET}' is not a configured target"
        )
    endif()

    _lux_arch_require_value("LAYER" "${ARG_LAYER}" _LUX_ARCH_LAYERS)
    _lux_arch_require_value("PRODUCT" "${ARG_PRODUCT}" _LUX_ARCH_PRODUCTS)
    _lux_arch_require_value("ROLE" "${ARG_ROLE}" _LUX_ARCH_ROLES)

    get_target_property(alias_target ${ARG_TARGET} ALIASED_TARGET)
    if(alias_target)
        set(target ${alias_target})
    else()
        set(target ${ARG_TARGET})
    endif()

    get_target_property(existing_layer ${target} LUX_ARCH_LAYER)
    if(existing_layer AND NOT existing_layer STREQUAL "${ARG_LAYER}")
        message(FATAL_ERROR
            "lux_classify_target: '${target}' was already classified as ${existing_layer}"
        )
    endif()

    set_property(TARGET ${target} PROPERTY LUX_ARCH_LAYER ${ARG_LAYER})
    set_property(TARGET ${target} PROPERTY LUX_ARCH_PRODUCT ${ARG_PRODUCT})
    set_property(TARGET ${target} PROPERTY LUX_ARCH_ROLE ${ARG_ROLE})
    set_property(GLOBAL APPEND PROPERTY LUX_ARCH_TARGETS ${target})
endfunction()

function(lux_add_build_tool_dependency)
    cmake_parse_arguments(ARG "" "CONSUMER;TOOL" "" ${ARGN})
    if(NOT ARG_CONSUMER OR NOT ARG_TOOL)
        message(FATAL_ERROR
            "lux_add_build_tool_dependency: CONSUMER and TOOL are required"
        )
    endif()
    if(NOT TARGET ${ARG_CONSUMER} OR NOT TARGET ${ARG_TOOL})
        message(FATAL_ERROR
            "lux_add_build_tool_dependency: both arguments must name configured targets"
        )
    endif()
    add_dependencies(${ARG_CONSUMER} ${ARG_TOOL})
    set_property(
        TARGET ${ARG_CONSUMER}
        APPEND PROPERTY LUX_BUILD_TOOL_DEPENDENCIES ${ARG_TOOL}
    )
endfunction()

# Batch-one gate. Full recursive DAG validation is enabled after every production
# target has been classified; this narrower gate already prevents foundation
# targets from silently acquiring an upper-layer dependency during migration.
function(lux_validate_foundation_targets)
    get_property(targets GLOBAL PROPERTY LUX_ARCH_TARGETS)
    list(REMOVE_DUPLICATES targets)
    foreach(target IN LISTS targets)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(layer ${target} LUX_ARCH_LAYER)
        if(NOT layer STREQUAL "PLATFORM" AND NOT layer STREQUAL "CORE")
            continue()
        endif()
        get_target_property(link_libraries ${target} LINK_LIBRARIES)
        get_target_property(interface_libraries ${target} INTERFACE_LINK_LIBRARIES)
        foreach(expression IN LISTS link_libraries interface_libraries)
            _lux_arch_resolve_dependencies("${expression}" dependencies)
            foreach(dependency IN LISTS dependencies)
                get_target_property(aliased ${dependency} ALIASED_TARGET)
                if(aliased)
                    set(resolved ${aliased})
                else()
                    set(resolved ${dependency})
                endif()
                get_target_property(imported ${resolved} IMPORTED)
                if(imported)
                    continue()
                endif()
                get_target_property(dependency_layer ${resolved} LUX_ARCH_LAYER)
                if(NOT dependency_layer)
                    message(FATAL_ERROR
                        "Architecture: foundation target '${target}' depends on unclassified target '${resolved}'"
                    )
                endif()
                if(layer STREQUAL "PLATFORM" AND NOT dependency_layer STREQUAL "PLATFORM")
                    message(FATAL_ERROR
                        "Architecture: PLATFORM target '${target}' may not depend on ${dependency_layer} target '${resolved}'"
                    )
                endif()
                if(layer STREQUAL "CORE" AND
                   NOT dependency_layer STREQUAL "PLATFORM" AND
                   NOT dependency_layer STREQUAL "CORE")
                    message(FATAL_ERROR
                        "Architecture: CORE target '${target}' may not depend on ${dependency_layer} target '${resolved}'"
                    )
                endif()
            endforeach()
        endforeach()
    endforeach()
endfunction()

function(_lux_arch_dependency_allowed consumer_layer dependency_layer output)
    set(allowed FALSE)
    if(consumer_layer STREQUAL "PLATFORM")
        if(dependency_layer STREQUAL "PLATFORM")
            set(allowed TRUE)
        endif()
    elseif(consumer_layer STREQUAL "CORE")
        if(dependency_layer MATCHES "^(PLATFORM|CORE)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_layer STREQUAL "RESOURCE")
        if(dependency_layer MATCHES "^(PLATFORM|CORE|RESOURCE)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_layer STREQUAL "FUNCTION")
        if(dependency_layer MATCHES "^(PLATFORM|CORE|RESOURCE|FUNCTION)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_layer STREQUAL "ECS")
        if(dependency_layer MATCHES "^(PLATFORM|CORE|RESOURCE|FUNCTION|ECS)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_layer STREQUAL "RUNTIME")
        if(dependency_layer MATCHES "^(PLATFORM|CORE|RESOURCE|FUNCTION|ECS|RUNTIME)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_layer STREQUAL "AUTHORING")
        if(dependency_layer MATCHES "^(PLATFORM|CORE|RESOURCE|FUNCTION|AUTHORING)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_layer STREQUAL "TOOLCHAIN")
        if(dependency_layer MATCHES "^(PLATFORM|CORE|RESOURCE|FUNCTION|ECS|RUNTIME|AUTHORING|TOOLCHAIN)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_layer STREQUAL "EDITOR")
        if(NOT dependency_layer STREQUAL "HOST")
            set(allowed TRUE)
        endif()
    elseif(consumer_layer STREQUAL "HOST")
        set(allowed TRUE)
    endif()
    set(${output} ${allowed} PARENT_SCOPE)
endfunction()

function(_lux_arch_product_dependency_allowed consumer_product dependency_product output)
    set(allowed FALSE)
    if(consumer_product STREQUAL "RUNTIME")
        if(dependency_product STREQUAL "RUNTIME")
            set(allowed TRUE)
        endif()
    elseif(consumer_product STREQUAL "PLAYER")
        if(dependency_product MATCHES "^(RUNTIME|PLAYER)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_product STREQUAL "EDITOR")
        if(dependency_product MATCHES "^(RUNTIME|EDITOR|TOOLCHAIN|BUILD_TOOL)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_product MATCHES "^(TOOLCHAIN|BUILD_TOOL)$")
        if(dependency_product MATCHES "^(RUNTIME|TOOLCHAIN|BUILD_TOOL)$")
            set(allowed TRUE)
        endif()
    elseif(consumer_product STREQUAL "TEST")
        set(allowed TRUE)
    endif()
    set(${output} ${allowed} PARENT_SCOPE)
endfunction()

function(_lux_arch_resolve_dependencies expression output)
    # LINK_LIBRARIES frequently contains LINK_ONLY, BUILD_INTERFACE and
    # conditional generator expressions.  Silently skipping every `$<...>`
    # edge made the graph look clean while a product dependency could still
    # leak through an interface.  Extract configured target tokens instead;
    # non-target conditions and linker flags are naturally ignored.
    set(dependencies "")
    if(TARGET "${expression}")
        list(APPEND dependencies "${expression}")
    else()
        string(REGEX MATCHALL
            "[A-Za-z_][A-Za-z0-9_.+-]*(::[A-Za-z0-9_.:+-]+)?"
            candidates
            "${expression}"
        )
        foreach(candidate IN LISTS candidates)
            if(TARGET "${candidate}")
                list(APPEND dependencies "${candidate}")
            endif()
        endforeach()
    endif()
    list(REMOVE_DUPLICATES dependencies)
    set(${output} "${dependencies}" PARENT_SCOPE)
endfunction()

function(lux_validate_target_dag)
    get_property(targets GLOBAL PROPERTY LUX_ARCH_TARGETS)
    list(REMOVE_DUPLICATES targets)
    foreach(target IN LISTS targets)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(consumer_layer ${target} LUX_ARCH_LAYER)
        get_target_property(consumer_product ${target} LUX_ARCH_PRODUCT)
        get_target_property(consumer_role ${target} LUX_ARCH_ROLE)
        get_target_property(link_libraries ${target} LINK_LIBRARIES)
        get_target_property(interface_libraries ${target} INTERFACE_LINK_LIBRARIES)

        foreach(expression IN LISTS link_libraries interface_libraries)
            _lux_arch_resolve_dependencies("${expression}" dependencies)
            foreach(dependency IN LISTS dependencies)
                get_target_property(aliased ${dependency} ALIASED_TARGET)
                if(aliased)
                    set(dependency ${aliased})
                endif()
                get_target_property(imported ${dependency} IMPORTED)
                if(imported)
                    continue()
                endif()
                get_target_property(dependency_layer ${dependency} LUX_ARCH_LAYER)
                if(NOT dependency_layer)
                    # Unclassified targets remain visible to the migration audit,
                    # but only classified production edges are authoritative until
                    # the remaining legacy targets are split into their final homes.
                    continue()
                endif()
                get_target_property(dependency_product ${dependency} LUX_ARCH_PRODUCT)
                get_target_property(dependency_role ${dependency} LUX_ARCH_ROLE)

                _lux_arch_dependency_allowed(
                    "${consumer_layer}"
                    "${dependency_layer}"
                    layer_allowed
                )
                if(NOT layer_allowed)
                    message(FATAL_ERROR
                        "Architecture DAG: ${consumer_layer} target '${target}' "
                        "may not link ${dependency_layer} target '${dependency}'"
                    )
                endif()

                _lux_arch_product_dependency_allowed(
                    "${consumer_product}"
                    "${dependency_product}"
                    product_allowed
                )
                if(NOT product_allowed)
                    message(FATAL_ERROR
                        "Product DAG: ${consumer_product} target '${target}' may "
                        "not link ${dependency_product} target '${dependency}'"
                    )
                endif()

                # Concrete extensions are leaves. Hosts may load them from a
                # manifest but no engine domain links their implementation.
                if(dependency_role STREQUAL "EXTENSION" AND
                   NOT consumer_role STREQUAL "EXTENSION")
                    message(FATAL_ERROR
                        "Extension DAG: '${target}' links concrete extension "
                        "'${dependency}'; use the public contribution ABI instead"
                    )
                endif()
            endforeach()
        endforeach()
    endforeach()
endfunction()

function(_lux_arch_collect_targets directory output)
    get_property(local_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    set(result ${local_targets})
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        _lux_arch_collect_targets("${child}" child_targets)
        list(APPEND result ${child_targets})
    endforeach()
    set(${output} ${result} PARENT_SCOPE)
endfunction()

function(_lux_arch_collect_target_closure target output)
    if(NOT TARGET ${target})
        set(${output} "" PARENT_SCOPE)
        return()
    endif()

    get_target_property(aliased ${target} ALIASED_TARGET)
    if(aliased)
        set(target ${aliased})
    endif()

    get_property(visited GLOBAL PROPERTY LUX_ARCH_PROFILE_VISITED)
    if(target IN_LIST visited)
        set(${output} "" PARENT_SCOPE)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY LUX_ARCH_PROFILE_VISITED ${target})

    set(closure ${target})
    foreach(property
            LINK_LIBRARIES
            INTERFACE_LINK_LIBRARIES
            MANUALLY_ADDED_DEPENDENCIES
            LUX_BUILD_TOOL_DEPENDENCIES)
        get_target_property(expressions ${target} ${property})
        if(NOT expressions OR expressions MATCHES "-NOTFOUND$")
            continue()
        endif()
        foreach(expression IN LISTS expressions)
            _lux_arch_resolve_dependencies("${expression}" dependencies)
            foreach(dependency IN LISTS dependencies)
                get_target_property(dependency_alias ${dependency} ALIASED_TARGET)
                if(dependency_alias)
                    set(dependency ${dependency_alias})
                endif()
                get_target_property(imported ${dependency} IMPORTED)
                if(imported)
                    continue()
                endif()
                _lux_arch_collect_target_closure(${dependency} nested)
                list(APPEND closure ${nested})
            endforeach()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES closure)
    set(${output} ${closure} PARENT_SCOPE)
endfunction()

function(_lux_arch_require_closure_excludes root)
    if(NOT TARGET ${root})
        return()
    endif()
    set_property(GLOBAL PROPERTY LUX_ARCH_PROFILE_VISITED "")
    _lux_arch_collect_target_closure(${root} closure)
    foreach(forbidden IN LISTS ARGN)
        if(forbidden IN_LIST closure)
            message(FATAL_ERROR
                "ECS dimensional closure: '${root}' reaches forbidden target "
                "'${forbidden}'. Keep neutral, 2D, 3D and Pixel integration "
                "as separate target/schema closures."
            )
        endif()
    endforeach()
endfunction()

function(lux_validate_ecs_dimension_closures)
    foreach(legacy ecs_render ecs_animation flowforge shadergen)
        if(TARGET ${legacy})
            message(FATAL_ERROR
                "Architecture closure: legacy target '${legacy}' "
                "must not be restored as an alias or compatibility shim."
            )
        endif()
    endforeach()

    _lux_arch_require_closure_excludes(
        render_core
        render_2d render_3d render_pixel_integration
        render_components_2d render_components_3d
        animation_2d animation_3d pixel
    )
    _lux_arch_require_closure_excludes(
        render_2d
        render_3d render_components_3d render_pixel_integration
        animation_3d pixel
    )
    _lux_arch_require_closure_excludes(
        render_3d
        render_2d render_components_2d render_pixel_integration
        animation_2d pixel
    )
    _lux_arch_require_closure_excludes(
        animation_2d
        render_3d render_components_3d animation_3d
    )
    _lux_arch_require_closure_excludes(
        animation_3d
        render_2d render_components_2d render_pixel_integration
        animation_2d pixel
    )
    _lux_arch_require_closure_excludes(
        runtime_tilemap_prepare
        render_2d render_3d render_components_2d render_components_3d
        render_pixel_integration render_tilemap_integration animation_2d
        animation_3d pixel physics2d physics3d navigation3d
        spatial2d_streaming
    )

    if(LUX_BUILD_COMPONENT_META)
        _lux_arch_require_closure_excludes(
            render_components_2d_meta
            render_components_3d render_components_3d_meta
        )
        _lux_arch_require_closure_excludes(
            render_components_3d_meta
            render_components_2d render_components_2d_meta
        )
        _lux_arch_require_closure_excludes(
            animation_2d_meta
            animation_3d render_3d render_components_3d
        )
    endif()

    foreach(player_root game_application player_host lux_player)
        _lux_arch_require_closure_excludes(
            ${player_root}
            authoring_flowforge authoring_assets project
            toolchain_shader_ir toolchain_shader_compiler
            toolchain_material_compiler toolchain_flowforge
        )
    endforeach()
endfunction()

# A TOOLCHAIN configure sees Resource formats, Authoring models, offline
# transforms and the schema-only ECS targets those transforms serialize. It
# still configures no Runtime orchestration, render backend, UI or Host. Some
# early host build tools are introduced while Resource is configured; starting
# from every classified Toolchain and Build-Tool product therefore gives the
# default `all` target an explicit, auditable closure without hard-coding
# domain target names here. Required ECS/schema targets enter only as ordinary
# dependencies of those roots.
function(lux_limit_toolchain_default_build)
    _lux_arch_collect_targets("${CMAKE_SOURCE_DIR}" all_targets)
    list(REMOVE_DUPLICATES all_targets)

    set(roots "")
    foreach(target IN LISTS all_targets)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(product ${target} LUX_ARCH_PRODUCT)
        if(product MATCHES "^(TOOLCHAIN|BUILD_TOOL)$")
            list(APPEND roots ${target})
        endif()
    endforeach()

    set_property(GLOBAL PROPERTY LUX_ARCH_PROFILE_VISITED "")
    set(included "")
    foreach(root IN LISTS roots)
        _lux_arch_collect_target_closure(${root} closure)
        list(APPEND included ${closure})
    endforeach()
    list(REMOVE_DUPLICATES included)

    foreach(target IN LISTS all_targets)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(imported ${target} IMPORTED)
        if(imported OR target IN_LIST included)
            continue()
        endif()
        get_target_property(source_dir ${target} SOURCE_DIR)
        if(source_dir MATCHES "[/\\\\]test($|[/\\\\])" OR
           target MATCHES "(_test|_probe|_probes|_stress|_demo|_smoke)$")
            # Test selection remains orthogonal to the product profile.  If a
            # configured module intentionally registered a test, `all` must
            # still build its executable so `ctest` never points at a missing
            # file.  Unrelated Runtime/Editor tests disappear earlier with
            # their owning subdirectories.
            continue()
        endif()
        set_property(TARGET ${target} PROPERTY EXCLUDE_FROM_ALL TRUE)
    endforeach()

    list(LENGTH roots root_count)
    list(LENGTH included included_count)
    message(STATUS
        "TOOLCHAIN default build: ${root_count} roots, "
        "${included_count} targets in the resolved closure"
    )
endfunction()

function(lux_write_unclassified_target_report)
    _lux_arch_collect_targets("${CMAKE_SOURCE_DIR}" all_targets)
    list(REMOVE_DUPLICATES all_targets)
    list(SORT all_targets)

    set(lines "# target|type|source_dir\n")
    set(classified_lines "# target|type|layer|product|role|source_dir\n")
    foreach(target IN LISTS all_targets)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(imported ${target} IMPORTED)
        get_target_property(type ${target} TYPE)
        if(imported OR type STREQUAL "UTILITY")
            continue()
        endif()
        get_target_property(layer ${target} LUX_ARCH_LAYER)
        if(layer)
            get_target_property(product ${target} LUX_ARCH_PRODUCT)
            get_target_property(role ${target} LUX_ARCH_ROLE)
            get_target_property(source_dir ${target} SOURCE_DIR)
            string(APPEND classified_lines
                "${target}|${type}|${layer}|${product}|${role}|${source_dir}\n")
            continue()
        endif()
        get_target_property(source_dir ${target} SOURCE_DIR)
        string(APPEND lines "${target}|${type}|${source_dir}\n")
    endforeach()
    file(WRITE
        "${CMAKE_BINARY_DIR}/target-architecture-unclassified.txt"
        "${lines}"
    )
    file(WRITE
        "${CMAKE_BINARY_DIR}/target-architecture-classified.txt"
        "${classified_lines}"
    )
endfunction()

function(lux_validate_all_production_targets)
    _lux_arch_collect_targets("${CMAKE_SOURCE_DIR}" all_targets)
    list(REMOVE_DUPLICATES all_targets)
    foreach(target IN LISTS all_targets)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(imported ${target} IMPORTED)
        get_target_property(type ${target} TYPE)
        get_target_property(source_dir ${target} SOURCE_DIR)
        if(imported OR type STREQUAL "UTILITY" OR
           source_dir MATCHES "[/\\\\]test($|[/\\\\])" OR
           target MATCHES "(_test|_probe|_probes|_stress|_demo|_smoke|test_support)$")
            continue()
        endif()
        get_target_property(layer ${target} LUX_ARCH_LAYER)
        if(NOT layer)
            message(FATAL_ERROR
                "Architecture: production target '${target}' (${source_dir}) "
                "must declare LAYER, PRODUCT and ROLE with lux_classify_target"
            )
        endif()
    endforeach()
endfunction()

# Source-level rules complement the target DAG.  The dependency graph cannot
# detect a header that reaches across the layer boundary without a direct link
# edge, nor can it prevent a new Runtime-owned ISystem from being added to an
# already classified target.
#
# The semantic-debt limits are intentionally monotonic.  They freeze the
# 2026-08-22 baseline so new uses fail configure immediately; each migration
# wave lowers the limit until the final value is zero.
function(lux_validate_source_boundaries)
    string(CONCAT retired_spatial3d_systems
        "runtime" "_spatial3d_" "systems")
    string(CONCAT retired_tilemap_systems
        "runtime" "_tilemap_" "systems")
    string(CONCAT retired_physics3d_systems
        "runtime" "_physics3d_" "systems")
    string(CONCAT retired_navigation3d_systems
        "runtime" "_navigation3d_" "systems")
    string(CONCAT retired_presentation3d_systems
        "runtime" "_presentation3d_" "systems")
    foreach(retired_target IN ITEMS
            scene_catalog
            runtime_spatial3d_streaming_systems
            ${retired_spatial3d_systems}
            ${retired_tilemap_systems}
            ${retired_physics3d_systems}
            ${retired_navigation3d_systems}
            ${retired_presentation3d_systems})
        if(TARGET ${retired_target})
            message(FATAL_ERROR
                "Architecture: retired target '${retired_target}' reappeared."
            )
        endif()
    endforeach()

    execute_process(
        COMMAND
            ${CMAKE_COMMAND}
            "-DLUX_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DLUX_REPORT_PATH=${CMAKE_BINARY_DIR}/semantic-architecture-debt.txt"
            -P "${CMAKE_SOURCE_DIR}/cmake/ValidateSourceArchitecture.cmake"
        RESULT_VARIABLE source_validation_result
    )
    if(NOT source_validation_result EQUAL 0)
        message(FATAL_ERROR "Architecture: source validation failed.")
    endif()
endfunction()
