cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED INSTALL_PREFIX OR INSTALL_PREFIX STREQUAL "")
    message(FATAL_ERROR "INSTALL_PREFIX is required")
endif()

file(TO_CMAKE_PATH "${INSTALL_PREFIX}" prefix)
if(NOT IS_DIRECTORY "${prefix}")
    message(FATAL_ERROR "Install prefix does not exist: ${prefix}")
endif()

foreach(retired_package IN ITEMS
    "${prefix}/share/lux-engine-authoring"
    "${prefix}/share/lux-engine-world"
    "${prefix}/share/lux-engine-simulation-core"
    "${prefix}/share/lux-engine-process-asset"
    "${prefix}/share/lux-engine-process-world"
    "${prefix}/share/lux-engine-scene-world-runtime"
    "${prefix}/share/lux-engine-graph-kit"
    "${prefix}/share/lux-engine-flowforge"
    "${prefix}/share/lux-engine-flowforge-script-compiler"
    "${prefix}/share/lux-engine-resource-asset-script"
)
    if(EXISTS "${retired_package}")
        message(FATAL_ERROR "Install surface exposes retired package: ${retired_package}")
    endif()
endforeach()

foreach(retired_surface IN ITEMS
    "${prefix}/include/lux/engine/authoring"
    "${prefix}/include/lux/engine/toolchain/asset/material"
    "${prefix}/share/lux-engine-toolchain-asset/toolchain_material_cooker"
    "${prefix}/share/lux-engine-toolchain-asset/toolchain_shader_emitter"
    "${prefix}/include/lux/engine/domain/WorldObjectId.hpp"
    "${prefix}/include/lux/engine/process/asset"
    "${prefix}/include/lux/engine/process/world"
    "${prefix}/include/lux/engine/simulation/core"
    "${prefix}/include/lux/engine/simulation/systems"
    "${prefix}/include/lux/engine/scene/runtime"
    "${prefix}/share/lux-engine-scene/scene_core"
    "${prefix}/share/lux-engine-scene-presentation/scene_runtime_presentation"
    "${prefix}/share/lux-engine-scene-render/scene_runtime_render"
    "${prefix}/share/lux-engine-scene-render/scene_runtime_render_meta"
    "${prefix}/bin/lux_engine_world.dll"
    "${prefix}/lib/lux_engine_world.lib"
    "${prefix}/lib/liblux_engine_world.so"
    "${prefix}/bin/lux_engine_simulation_core.dll"
    "${prefix}/lib/lux_engine_simulation_core.lib"
    "${prefix}/bin/lux_engine_process_world.dll"
    "${prefix}/lib/lux_engine_process_world.lib"
    "${prefix}/bin/lux_engine_scene_core.dll"
    "${prefix}/lib/lux_engine_scene_core.lib"
    "${prefix}/bin/lux_engine_scene_runtime_world.dll"
    "${prefix}/lib/lux_engine_scene_runtime_world.lib"
    "${prefix}/bin/lux_engine_scene_runtime_render.dll"
    "${prefix}/lib/lux_engine_scene_runtime_render.lib"
    "${prefix}/bin/lux_engine_scene_runtime_render_meta.dll"
    "${prefix}/lib/lux_engine_scene_runtime_render_meta.lib"
)
    if(EXISTS "${retired_surface}")
        message(FATAL_ERROR "Install surface exposes retired topology: ${retired_surface}")
    endif()
endforeach()

file(GLOB_RECURSE installed_entries LIST_DIRECTORIES true "${prefix}/*")
foreach(entry IN LISTS installed_entries)
    file(TO_CMAKE_PATH "${entry}" normalized)
    set(is_lux_owned_path false)
    if(normalized MATCHES "[/]include[/]lux[/]" OR
       normalized MATCHES "[/]share[/]lux-engine")
        set(is_lux_owned_path true)
    endif()
    if((is_lux_owned_path AND normalized MATCHES "[/](legacy|sinclude|pinclude)([/]|$)") OR
       normalized MATCHES "[/]ecs[/](detail|core/detail|schedule/detail)([/]|$)" OR
       normalized MATCHES "[/]include[/]lux[/]engine[/]ecs([/]|$)" OR
       normalized MATCHES "[/]include[/]lux[/]engine[/]graph_kit([/]|$)" OR
       normalized MATCHES "[/]include[/]lux[/]engine[/]material[/](compiler[/](MaterialIR|ShaderIR|Lowering|Backend)|import[/]MaterialToGraph)[.]hpp$" OR
       normalized MATCHES "[/]include[/]lux[/]engine[/]simulation[/]script([/]|$)" OR
       normalized MATCHES "[/]include[/]lux[/]engine[/]resource[/]asset[/]script([/]|$)")
        message(FATAL_ERROR
            "Install surface exposes quarantine/private detail: ${normalized}"
        )
    endif()
    get_filename_component(name "${entry}" NAME)
    if(normalized MATCHES "[/]include[/]lux[/]engine[/]simulation[/]ecs[/]" AND
       name MATCHES
       "^(EcsState|EcsMutation|SimulationEcsMutation|EcsCommands|EcsTaskResources|EcsChangeJournal|Query|ChangeCursor|ComponentChanges|EntityChanges|HierarchySystem|FrameInfo|ISystem|System|SystemFrame|SystemHandle|SystemPhase|SystemSetId|Schedule|ScheduleEdit|ScheduleError|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|World|WorldMutation|WorldSnapshot|WorldSection.*|PersistentEntity|SceneServices|AssetManager|AssetRef|AssetLoadPort|AssetServices|ComponentCodec|TaggedPropertyArchive|Archive|ByteIO|NameTable)\\.(h|hpp)$")
        message(FATAL_ERROR "Install surface exposes retired API: ${normalized}")
    endif()
    if(normalized MATCHES "[/]include[/]lux[/]engine[/]simulation[/]" AND
       name MATCHES "^(FrameInfo|SimulationExecution|SystemExecutionPoint|SystemHookPoint|SystemEventDescription|ScriptMountFacts|ScriptEventWriter|ScriptMetaAdapter|ScriptBindingSession|ScriptComponent|EntityBehavior|SystemEventBuffer|ScriptBindingCompatibility|ScriptMountDescription)\\.(h|hpp)$")
        message(FATAL_ERROR "Install surface exposes retired API: ${normalized}")
    endif()
    if(normalized MATCHES
       "[/]include[/]lux[/]engine[/]simulation[/]ecs[/]TransformSystem[.]hpp$")
        message(FATAL_ERROR "Install surface exposes retired Transform System path: ${normalized}")
    endif()
endforeach()

set(material_toolchain_package "${prefix}/share/lux-engine-toolchain-material")
if(EXISTS "${material_toolchain_package}")
    foreach(material_contract IN ITEMS
        "${prefix}/share/lux-engine-function/material_graph/lux-engine-function-material_graph-config-targets.cmake"
        "${material_toolchain_package}/toolchain_material_compiler/lux-engine-toolchain-material-toolchain_material_compiler-config-targets.cmake"
        "${material_toolchain_package}/toolchain_material_cooker/lux-engine-toolchain-material-toolchain_material_cooker-config-targets.cmake"
    )
        if(NOT EXISTS "${material_contract}")
            message(FATAL_ERROR "Installed Material target is missing: ${material_contract}")
        endif()
    endforeach()
endif()

set(shader_toolchain_package "${prefix}/share/lux-engine-toolchain-shader")
if(EXISTS "${material_toolchain_package}" AND
   NOT EXISTS "${shader_toolchain_package}/toolchain_shader_emitter/lux-engine-toolchain-shader-toolchain_shader_emitter-config-targets.cmake")
    message(FATAL_ERROR "Installed Toolchain Shader emitter package is missing.")
endif()

foreach(endpoint_header IN ITEMS
    "${prefix}/include/lux/engine/simulation/HookPoint.hpp"
    "${prefix}/include/lux/engine/simulation/HookChannel.hpp"
)
    if(EXISTS "${endpoint_header}")
        file(READ "${endpoint_header}" endpoint_contract)
        if(endpoint_contract MATCHES
           "flushMutations|mutation_capacity|struct[ \t\r\n]+Mutation|mutations_")
            message(FATAL_ERROR
                "Installed endpoint restores deferred topology mutation: ${endpoint_header}"
            )
        endif()
    endif()
endforeach()

file(GLOB_RECURSE installed_text LIST_DIRECTORIES false
    "${prefix}/*.hpp"
    "${prefix}/*.h"
    "${prefix}/*.cmake"
)
string(CONCAT retired_script_compression_vocabulary
    "ScriptAssetContent|Script(CallFrame|Value|Signature)[.]hpp|"
    "ScriptSemantic(Type|Layout|TypeTraits)|sameScriptSignature|scriptBuiltinLayout|"
    "flowforge_script_compiler|flowforge_compiler_dialect"
)
foreach(entry IN LISTS installed_text)
    file(TO_CMAKE_PATH "${entry}" normalized)
    if(NOT normalized MATCHES "[/]include[/]lux[/]" AND
       NOT normalized MATCHES "[/]share[/]lux-engine")
        continue()
    endif()
    file(READ "${entry}" content)
    if(content MATCHES "(^|[\r\n])[ \t]*throw([ \t;(]|$)" OR
       content MATCHES "takeOrThrow")
        message(FATAL_ERROR "Installed public file exposes a throwing Lux API: ${entry}")
    endif()
    if(content MATCHES "[/\\\\]legacy[/\\\\]" OR
       content MATCHES "AssetStore|AssetClient|AssetLease|AssetManager|AssetRef|AssetLoadPort|AssetServices|SceneServices|ISystem|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|WorldSection|PersistentEntity|PersistentId|ComponentLoadBinding|ComponentLoadSet|ecs_load|section[ \\t]*=[ \\t]*(LOAD|OMIT)|connectConstruct|connectUpdate|connectDestroy|observer_relations_|ComponentCodec|ComponentPersistence|EcsBinaryWriter|EcsBinaryReader|persistence_contract|[.]ecs_persistence[.]hpp|TaggedProperty|schema_reflection|cooked_relocation|LXES|LXWS|LUX_REBUILD_COMPONENT_SCHEMA|LUX_COMPONENT_SCHEMA|LUX_COMPONENT_SNAPSHOT|LUX_COMPONENT_WORLD_SECTION|lux/cxx/serialization/|lux::cxx::ser|LUX_CLASS[ \\t]*\\(|LUX_ENUM[ \\t]*\\(|SystemExecutionPoint|SystemHookPoint|dispatch_point|ESystemEventTarget::BROADCAST|ScriptEventRegistry|ScriptBindingSession|ScriptComponent|EntityBehavior|default_bindings|EScriptBindingSetMode|ScriptMountFacts|ScriptEventWriter|CppBehaviorScript|PythonSourceScript|SemanticCatalog|TargetCatalog|entity_to_sidecar|entity_slots|hook_range_begin|hook_range_count|hot_path_(allocations|name_lookups|asset_lookups|signature_adaptations|scene_scans)|struct[ ]+LuaComponentBinding[^}]*string_view|struct[ ]+(ScriptBindingTargetCatalogEntry|ExportMethodNode)[^}]*ScriptSemanticType|ScriptSystemCapacities|EBehaviorStopReason|startInstance|stopInstance" OR
       content MATCHES "${retired_script_compression_vocabulary}")
        message(FATAL_ERROR "Installed file contains a retired boundary: ${entry}")
    endif()
    if(content MATCHES "lux/engine/ecs/|lux::engine::ecs|lux::ecs::")
        message(FATAL_ERROR
            "Installed file restores retired top-level ECS surface: ${entry}"
        )
    endif()
    if(normalized MATCHES "[/]include[/]lux[/]engine[/]ui[/]" AND
       content MATCHES "imgui|ImGui|ImVec[24]|ImTextureID|ImDraw(Data|List)|ax::NodeEditor")
        message(FATAL_ERROR
            "Installed Lux UI header exposes a private backend type/include: ${entry}"
        )
    endif()
    if(normalized MATCHES "[/]include[/]lux[/]engine[/]editor[/]inspector[/]" AND
       content MATCHES "imgui|ImGui|ImVec[24]|ImTextureID|ImDraw(Data|List)|ax::NodeEditor|RefClass|RefField|ReflectionRegistry")
        message(FATAL_ERROR
            "Installed Inspector SDK exposes backend UI or runtime reflection vocabulary: ${entry}"
        )
    endif()
    if(normalized MATCHES "[/]include[/]lux[/]engine[/]simulation[/](scripting|ScriptSystem)" AND
       NOT normalized MATCHES "[/]simulation[/]scripting[/]cpp_static[/]" AND
       NOT normalized MATCHES "[/]simulation[/]scripting[/]lua[/]" AND
       content MATCHES "std::(function|any|type_index|coroutine_handle)|lua_State|ScriptApiManager|CoroutineManager|AsyncManager|EventAwaitManager|ScriptServices|SceneServices|ServiceRegistry")
        message(FATAL_ERROR
            "Installed Script runtime contract exposes a language runtime or service-locator boundary: ${entry}"
        )
    endif()
    if(normalized MATCHES "[/]include[/]lux[/]engine[/]function[/]script[/]ScriptAbility" AND
       content MATCHES "Scene|Simulation|SystemInstanceId|Physics|AssetLoading|std::(function|any|type_index)|ScriptApiManager|AbilityManager|ServiceRegistry")
        message(FATAL_ERROR
            "Installed Script Ability contract exposes Engine ontology or service lookup: ${entry}"
        )
    endif()
    if(normalized MATCHES "[/]share[/]lux-engine-function[/]script_lua[/].*config-targets[.]cmake$" AND
       content MATCHES "LuaJIT::LuaJIT|Lua::Lua")
        message(FATAL_ERROR
            "Installed script_lua semantic target publicly propagates a concrete Lua VM target: ${entry}"
        )
    endif()
    if(normalized MATCHES "[/]include[/]lux[/]engine[/]function[/]render[/]client[/]" AND
       content MATCHES "SubmitImGuiDrawDataPayload|ImGuiDrawData|ImGuiCommConfig")
        message(FATAL_ERROR
            "Installed generic Render protocol exposes private UI vocabulary: ${entry}"
        )
    endif()
    if(normalized MATCHES "[/]include[/]lux[/]engine[/]simulation[/]ecs[/]" AND
       content MATCHES "CloneFn|default_emplace|COPY_WITHOUT_CLONE")
        message(FATAL_ERROR
            "Installed ECS file contains retired component operations: ${entry}"
        )
    endif()
endforeach()

if(DEFINED INSTALL_MANIFEST AND EXISTS "${INSTALL_MANIFEST}")
    file(READ "${INSTALL_MANIFEST}" manifest)
    if(manifest MATCHES "[/\\\\]legacy[/\\\\]" OR
       manifest MATCHES "[/\\\\](sinclude|pinclude)[/\\\\]")
        message(FATAL_ERROR "Install manifest contains quarantine/private paths")
    endif()
    if(manifest MATCHES "lux-engine-ecs" OR
       manifest MATCHES "[/\\\\]include[/\\\\]lux[/\\\\]engine[/\\\\]ecs[/\\\\]")
        message(FATAL_ERROR "Install manifest contains retired top-level ECS package")
    endif()
endif()

set(task_targets
    "${prefix}/share/lux-engine-core/task/lux-engine-core-task-config-targets.cmake"
)
if(NOT EXISTS "${task_targets}")
    message(FATAL_ERROR "Installed L0 task component is missing: ${task_targets}")
endif()
file(READ "${task_targets}" task_contract)
if(task_contract MATCHES
   "lux::engine::simulation|lux::engine::ecs|lux::engine::object|lux::engine::process|lux::engine::scene")
    message(FATAL_ERROR
        "Installed core::task closure depends on an upper-layer subsystem"
    )
endif()

set(system_targets
    "${prefix}/share/lux-engine-simulation-system/system/lux-engine-simulation-system-system-config-targets.cmake"
)
if(NOT EXISTS "${system_targets}")
    message(FATAL_ERROR "Installed Simulation System component is missing")
endif()
file(READ "${system_targets}" system_contract)
if(system_contract MATCHES
   "lux::engine::core::object|reflection_runtime|schema_reflection")
    message(FATAL_ERROR
        "Installed core + system closure depends on Object/reflection"
    )
endif()

foreach(contract_file IN ITEMS
    "${prefix}/share/lux-engine-core/serialization/lux-engine-core-serialization-config-targets.cmake"
    "${prefix}/share/lux-engine-core/type_info/lux-engine-core-type_info-config-targets.cmake"
    "${prefix}/share/lux-engine-world-identity/world_identity/lux-engine-world-identity-world_identity-config-targets.cmake"
    "${prefix}/share/lux-engine-partition-identity/partition_identity/lux-engine-partition-identity-partition_identity-config-targets.cmake"
    "${prefix}/share/lux-engine-world-partition/world_partition/lux-engine-world-partition-world_partition-config-targets.cmake"
    "${prefix}/share/lux-engine-world-description/world_description/lux-engine-world-description-world_description-config-targets.cmake"
    "${prefix}/share/lux-engine-world-asset/world_asset/lux-engine-world-asset-world_asset-config-targets.cmake"
    "${prefix}/share/lux-engine-simulation/simulation_description/lux-engine-simulation-simulation_description-config-targets.cmake"
    "${prefix}/share/lux-engine-simulation-asset/simulation_asset/lux-engine-simulation-asset-simulation_asset-config-targets.cmake"
    "${prefix}/share/lux-engine-simulation-composition/simulation_composition/lux-engine-simulation-composition-simulation_composition-config-targets.cmake"
    "${prefix}/share/lux-engine-simulation-ecs/core/lux-engine-simulation-ecs-core-config-targets.cmake"
    "${prefix}/share/lux-engine-function/script_artifact/lux-engine-function-script_artifact-config-targets.cmake"
    "${prefix}/share/lux-engine-simulation/simulation_script/lux-engine-simulation-simulation_script-config-targets.cmake"
    "${prefix}/share/lux-engine-process-asset-loading/process_asset_loading/lux-engine-process-asset-loading-process_asset_loading-config-targets.cmake"
    "${prefix}/share/lux-engine-process-world-loading/process_world_loading/lux-engine-process-world-loading-process_world_loading-config-targets.cmake"
    "${prefix}/share/lux-engine-scene/scene_composition/lux-engine-scene-scene_composition-config-targets.cmake"
    "${prefix}/share/lux-engine-scene-presentation/scene_presentation/lux-engine-scene-presentation-scene_presentation-config-targets.cmake"
    "${prefix}/share/lux-engine-scene-world-materialization/scene_world_materialization/lux-engine-scene-world-materialization-scene_world_materialization-config-targets.cmake"
    "${prefix}/share/lux-engine-scene-script-runtime/scene_script_runtime/lux-engine-scene-script-runtime-scene_script_runtime-config-targets.cmake"
    "${prefix}/share/lux-engine-scene-render/scene_render/lux-engine-scene-render-scene_render-config-targets.cmake"
)
    if(NOT EXISTS "${contract_file}")
        message(FATAL_ERROR "Installed foundation target is missing: ${contract_file}")
    endif()
    file(READ "${contract_file}" foundation_contract)
    if(foundation_contract MATCHES
       "lux::engine::core::meta|lux::cxx::reflection_runtime")
        message(FATAL_ERROR
            "Installed foundation closure depends on runtime reflection: ${contract_file}"
        )
    endif()
endforeach()

message(STATUS "Installed architecture surface is clean: ${prefix}")
