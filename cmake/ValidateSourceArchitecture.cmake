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
    "modules/function/ui_next_vulkan"
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
    if(content MATCHES "#[ \t]*include[ \t]*[<\"]lux/engine/object/")
        message(FATAL_ERROR
            "Architecture: ECS production source '${source}' includes "
            "core/object. Object/UI interaction state is not World state."
        )
    endif()
endforeach()

# Signal coordinates are generated build-local data.  The only source-level
# bridge is the private helper declared by Signal.hpp and consumed by the
# generated meta template; production modules may not author either identity.
file(GLOB_RECURSE module_production_sources LIST_DIRECTORIES false
    "${source_root}/modules/*/include/*.hpp"
    "${source_root}/modules/*/sinclude/*.hpp"
    "${source_root}/modules/*/pinclude/*.hpp"
    "${source_root}/modules/*/src/*.cpp"
)
foreach(source IN LISTS module_production_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    if(normalized MATCHES "/test/" OR
       normalized MATCHES "/core/object/include/lux/engine/object/Signal.hpp$")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES "GeneratedSignalAccess|SignalIndex[ \t\r\n]*\\{")
        message(FATAL_ERROR
            "Architecture: production source '${source}' authors a generated "
            "Signal coordinate. Declare a LUX_OBJECT static signal and run codegen."
        )
    endif()
endforeach()

# Core reflection is a lower query primitive. Object may consume reflection,
# but reflection must never acquire Object lifetime or signal semantics.
file(GLOB_RECURSE meta_sources LIST_DIRECTORIES false
    "${source_root}/modules/core/meta/*.hpp"
    "${source_root}/modules/core/meta/*.cpp"
    "${source_root}/modules/core/meta/CMakeLists.txt"
)
foreach(source IN LISTS meta_sources)
    file(READ "${source}" content)
    if(content MATCHES "lux/engine/object/|core::object|[ \t]object[ \t\r\n]*\\)")
        message(FATAL_ERROR
            "Architecture: core/meta source '${source}' depends on core/object. "
            "The dependency direction is meta -> object consumers only."
        )
    endif()
endforeach()

file(GLOB_RECURSE object_sources LIST_DIRECTORIES false
    "${source_root}/modules/core/object/*.hpp"
    "${source_root}/modules/core/object/*.cpp"
    "${source_root}/modules/core/object/CMakeLists.txt"
)
foreach(source IN LISTS object_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    if(normalized MATCHES "/test/")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES
       "lux/cxx/event/|lux/engine/(events|ui|ecs|runtime)/")
        message(FATAL_ERROR
            "Architecture: core/object source '${source}' depends on an event, "
            "UI, ECS or Runtime framework. Object is a lower foundation."
        )
    endif()
    if(content MATCHES
       "LUX_OBJECT_SIGNAL|setDispatcher[ \t\r\n]*\\(|[ \t]emit[ \t\r\n]*\\(")
        message(FATAL_ERROR
            "Architecture: retired Object declaration, mutable affinity or "
            "emit surface reappeared in '${source}'."
        )
    endif()
endforeach()

# ui_next is the pure interaction primitive target. Renderer integration has
# its own sibling target and legacy UI remains a separate migration source.
file(GLOB_RECURSE ui_next_sources LIST_DIRECTORIES false
    "${source_root}/modules/function/ui_next/*.hpp"
    "${source_root}/modules/function/ui_next/*.cpp"
    "${source_root}/modules/function/ui_next/CMakeLists.txt"
)
foreach(source IN LISTS ui_next_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    if(normalized MATCHES "/test/")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES
       "lux/engine/(ui/|input/|resource/|ecs/|runtime/|extensions/|editor/|function/render/)")
        message(FATAL_ERROR
            "Architecture: UI vNext core source '${source}' crosses into "
            "legacy UI, Input, Resource, ECS, Runtime, Render or Editor."
        )
    endif()
    if(content MATCHES
       "class[ \t\r\n]+UISystem|UISession::post[ \t\r\n]*\\(")
        message(FATAL_ERROR
            "Architecture: UI vNext source '${source}' restores the retired "
            "UISystem owner or generic post surface."
        )
    endif()
    if(content MATCHES
       "CommandPresentation|PaneCreateContext|EUiPointerButton|EUiKey|ViewportDrop|setActiveContexts|setActivationScope")
        message(FATAL_ERROR
            "Architecture: UI vNext source '${source}' restores a retired "
            "presentation, factory, input, drag/drop or route wrapper."
        )
    endif()
endforeach()

file(GLOB_RECURSE ui_drawdata_sources LIST_DIRECTORIES false
    "${source_root}/modules/function/ui_next_drawdata/*.hpp"
    "${source_root}/modules/function/ui_next_drawdata/*.cpp"
    "${source_root}/modules/function/ui_next_drawdata/CMakeLists.txt"
)
foreach(source IN LISTS ui_drawdata_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    if(normalized MATCHES "/test/")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES
       "lux/engine/function/render/|render_(client|graph|vulkan|features)|Vulkan")
        message(FATAL_ERROR
            "Architecture: draw-data source '${source}' depends on Render or "
            "Vulkan. ui_next_drawdata owns snapshots only."
        )
    endif()
endforeach()

# Foundation stabilization is deliberately isolated from Engine/ECS/plugin
# migration. Keep this gate after freezing until an explicit consumer re-audit
# approves and implements the first upper-layer dependency.
file(GLOB_RECURSE frozen_consumer_sources LIST_DIRECTORIES false
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
foreach(source IN LISTS frozen_consumer_sources)
    file(READ "${source}" content)
    if(content MATCHES "ui_next")
        message(FATAL_ERROR
            "Architecture: frozen Engine/ECS/Extension consumer '${source}' "
            "uses UI vNext before the post-freeze consumer re-audit."
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
    if(content MATCHES
       "(class|struct)[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]+(final[ \t\r\n]+)?:[^{;]*RenderStage")
        message(FATAL_ERROR
            "Architecture: Runtime production source '${source}' defines a "
            "RenderStage subclass. ECS-to-render projection belongs under "
            "ecs/render/."
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
string(CONCAT retired_scene_integration
    "ISceneRuntime" "Integration")
string(CONCAT retired_stage_lifecycle
    "RenderSystem" "Stages")
string(CONCAT retired_stage_install
    "install" "Stage")
string(CONCAT retired_stage_remove
    "remove" "Stage")
string(CONCAT retired_generated_component_queue
    "queueGenerated" "Component")

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
    ${retired_scene_integration}
    ${retired_stage_lifecycle}
    ${retired_stage_install}
    ${retired_stage_remove}
    ${retired_generated_component_queue}
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

# Renderer requirements are build-closure facts, never SceneDescription
# fields. Restrict this check to the Scene contract so the Toolchain's
# ephemeral ProjectBuildUsage vocabulary remains legitimate.
file(GLOB_RECURSE scene_contract_sources LIST_DIRECTORIES false
    "${source_root}/engine/scene/*.hpp"
    "${source_root}/engine/scene/*.cpp"
)
string(CONCAT retired_scene_required_features
    "required" "_render_features")
string(CONCAT retired_scene_optional_features
    "optional" "_render_features")
foreach(source IN LISTS scene_contract_sources)
    file(TO_CMAKE_PATH "${source}" normalized_source)
    if(normalized_source MATCHES "/test/")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES
       "${retired_scene_required_features}|${retired_scene_optional_features}")
        message(FATAL_ERROR
            "Architecture: Scene contract '${source}' contains renderer "
            "requirements. Capability roots belong to cold product "
            "composition."
        )
    endif()
endforeach()

file(WRITE "${LUX_REPORT_PATH}" "${report}")
