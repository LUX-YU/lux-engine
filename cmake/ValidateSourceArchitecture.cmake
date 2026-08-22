cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED LUX_SOURCE_DIR)
    message(FATAL_ERROR "LUX_SOURCE_DIR is required")
endif()
if(NOT DEFINED LUX_REPORT_PATH)
    set(LUX_REPORT_PATH
        "${CMAKE_CURRENT_BINARY_DIR}/semantic-architecture-debt.txt")
endif()

file(TO_CMAKE_PATH "${LUX_SOURCE_DIR}" source_root)

set(retired_directories
    "engine/spatial3d"
    "engine/runtime/packs"
    "engine/runtime/spatial_partition"
    "engine/runtime/spatial2d"
    "engine/runtime/spatial3d"
    "engine/runtime/animation"
    "engine/runtime/extensions/contribution_host"
    "engine/runtime/launch"
    "engine/runtime/world"
)
foreach(relative IN LISTS retired_directories)
    if(EXISTS "${source_root}/${relative}")
        message(FATAL_ERROR
            "Architecture: retired directory '${relative}' reappeared."
        )
    endif()
endforeach()

file(GLOB_RECURSE ecs_sources LIST_DIRECTORIES false
    "${source_root}/ecs/*.hpp"
    "${source_root}/ecs/*.cpp"
)
foreach(source IN LISTS ecs_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    if(normalized MATCHES "/test/")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES "#[ \t]*include[ \t]*[<\"]lux/engine/runtime/")
        message(FATAL_ERROR
            "Architecture: ECS production source '${source}' includes "
            "engine/runtime. Inject a modules-level port instead."
        )
    endif()
endforeach()

file(GLOB_RECURSE runtime_sources LIST_DIRECTORIES false
    "${source_root}/engine/runtime/*.hpp"
    "${source_root}/engine/runtime/*.cpp"
)
foreach(source IN LISTS runtime_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    if(normalized MATCHES "/test/")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES
       "(class|struct)[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]+(final[ \t\r\n]+)?:[^{;]*ISystem")
        message(FATAL_ERROR
            "Architecture: Runtime production source '${source}' defines an "
            "ISystem subclass. World behavior belongs under ecs/."
        )
    endif()
    if(source MATCHES "\\.hpp$" AND content MATCHES
       "(class|struct)[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*Component([ \t\r\n:{]|$)")
        message(FATAL_ERROR
            "Architecture: Runtime public header '${source}' declares a "
            "Component. World facts belong under ecs/."
        )
    endif()
endforeach()

file(GLOB_RECURSE runtime_product_sources LIST_DIRECTORIES false
    "${source_root}/modules/*.hpp"
    "${source_root}/modules/*.cpp"
    "${source_root}/ecs/*.hpp"
    "${source_root}/ecs/*.cpp"
    "${source_root}/engine/runtime/*.hpp"
    "${source_root}/engine/runtime/*.cpp"
    "${source_root}/engine/game/*.hpp"
    "${source_root}/engine/game/*.cpp"
    "${source_root}/engine/hosts/*.hpp"
    "${source_root}/engine/hosts/*.cpp"
    "${source_root}/extensions/*.hpp"
    "${source_root}/extensions/*.cpp"
)
string(CONCAT build_usage_contract "ProjectUsage" "Manifest")
string(CONCAT generated_game_composition "Game" "Composition")
foreach(source IN LISTS runtime_product_sources)
    file(READ "${source}" content)
    if(content MATCHES "${build_usage_contract}|${generated_game_composition}")
        message(FATAL_ERROR
            "Architecture: Runtime/product source '${source}' mentions a "
            "Toolchain-only build-closure artifact. Build usage and generated "
            "composition must stop at the build graph."
        )
    endif()
endforeach()

file(GLOB_RECURSE semantic_sources LIST_DIRECTORIES false
    "${source_root}/cmake/*.cmake"
    "${source_root}/modules/*.hpp"
    "${source_root}/modules/*.cpp"
    "${source_root}/modules/CMakeLists.txt"
    "${source_root}/ecs/*.hpp"
    "${source_root}/ecs/*.cpp"
    "${source_root}/ecs/CMakeLists.txt"
    "${source_root}/engine/*.hpp"
    "${source_root}/engine/*.cpp"
    "${source_root}/engine/CMakeLists.txt"
    "${source_root}/extensions/*.hpp"
    "${source_root}/extensions/*.cpp"
    "${source_root}/extensions/CMakeLists.txt"
)

# Construct retired identifiers from fragments so this gate does not keep
# forbidden production vocabulary alive in its own scan surface.
string(CONCAT retired_scene_contribution "Scene" "Contribution")
string(CONCAT retired_world_feature "World" "Scene" "Feature")
string(CONCAT retired_render_effect "Render" "Effect")
string(CONCAT retired_runtime_pack "runtime" "_pack_")
string(CONCAT retired_host_kind "contribution" "_host")
string(CONCAT retired_extension_v4 "luxGetExtensionModule" "V4")
string(CONCAT retired_registration_v4
    "luxRegisterRuntimeContributions" "V4")
string(CONCAT retired_editor_panel_catalog "EditorPanel" "Catalog")
string(CONCAT retired_editor_tool_host "EditorTool" "Host")
string(CONCAT retired_editor_tool_ticket "EditorTool" "Ticket")
string(CONCAT retired_editor_panel_contribution
    "EditorPanelContribution" "Descriptor")
string(CONCAT retired_editor_registrar
    "EditorContribution" "Registrar")
string(CONCAT retired_editor_entrypoint
    "luxRegisterEditorContributions" "V5")
string(CONCAT retired_runtime_usage "RuntimeUsage" "Manifest")
string(CONCAT retired_usage_manager "UsageManifest" "Manager")
string(CONCAT retired_system_registry "System" "Registry")
string(CONCAT retired_scene_feature "Scene" "Feature")
string(CONCAT retired_feature_manager "Feature" "Manager")
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

set(retired_semantic_names
    ${retired_scene_contribution}
    ${retired_world_feature}
    ${retired_render_effect}
    ${retired_runtime_pack}
    ${retired_host_kind}
    ${retired_extension_v4}
    ${retired_registration_v4}
    ${retired_editor_panel_catalog}
    ${retired_editor_tool_host}
    ${retired_editor_tool_ticket}
    ${retired_editor_panel_contribution}
    ${retired_editor_registrar}
    ${retired_editor_entrypoint}
    ${retired_runtime_usage}
    ${retired_usage_manager}
    ${retired_system_registry}
    ${retired_scene_feature}
    ${retired_feature_manager}
    ${retired_spatial3d_systems}
    ${retired_tilemap_systems}
    ${retired_physics3d_systems}
    ${retired_navigation3d_systems}
    ${retired_presentation3d_systems}
)

set(report "# retired-semantic|count|required\n")
foreach(debt_name IN LISTS retired_semantic_names)
    set(actual 0)
    foreach(source IN LISTS semantic_sources)
        if(source STREQUAL CMAKE_CURRENT_LIST_FILE)
            continue()
        endif()
        file(READ "${source}" content)
        string(REGEX MATCHALL "${debt_name}" matches "${content}")
        list(LENGTH matches match_count)
        math(EXPR actual "${actual} + ${match_count}")
    endforeach()
    string(APPEND report "${debt_name}|${actual}|0\n")
    if(NOT actual EQUAL 0)
        message(FATAL_ERROR
            "Architecture: retired semantic '${debt_name}' reappeared "
            "${actual} time(s)."
        )
    endif()
endforeach()

file(WRITE "${LUX_REPORT_PATH}" "${report}")
