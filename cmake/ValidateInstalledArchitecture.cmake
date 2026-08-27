cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED INSTALL_PREFIX OR INSTALL_PREFIX STREQUAL "")
    message(FATAL_ERROR "INSTALL_PREFIX is required")
endif()

file(TO_CMAKE_PATH "${INSTALL_PREFIX}" prefix)
if(NOT IS_DIRECTORY "${prefix}")
    message(FATAL_ERROR "Install prefix does not exist: ${prefix}")
endif()

if(EXISTS "${prefix}/share/lux-engine-simulation-core")
    message(FATAL_ERROR
        "Install surface exposes retired simulation-core package"
    )
endif()

file(GLOB_RECURSE installed_entries LIST_DIRECTORIES true "${prefix}/*")
foreach(entry IN LISTS installed_entries)
    file(TO_CMAKE_PATH "${entry}" normalized)
    if(normalized MATCHES "[/](legacy|sinclude|pinclude)([/]|$)" OR
       normalized MATCHES "[/]ecs[/](detail|core/detail|schedule/detail)([/]|$)" OR
       normalized MATCHES "[/]include[/]lux[/]engine[/]ecs([/]|$)")
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
endforeach()

foreach(endpoint_header IN ITEMS
    "${prefix}/include/lux/engine/simulation/HookPoint.hpp"
    "${prefix}/include/lux/engine/simulation/EventPoint.hpp"
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
foreach(entry IN LISTS installed_text)
    file(TO_CMAKE_PATH "${entry}" normalized)
    file(READ "${entry}" content)
    if(content MATCHES "[/\\\\]legacy[/\\\\]" OR
       content MATCHES "AssetStore|AssetClient|AssetLease|AssetManager|AssetRef|AssetLoadPort|AssetServices|SceneServices|ISystem|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|WorldSection|PersistentEntity|PersistentId|ComponentLoadBinding|ComponentLoadSet|ecs_load|section[ \\t]*=[ \\t]*(LOAD|OMIT)|connectConstruct|connectUpdate|connectDestroy|observer_relations_|ComponentCodec|ComponentPersistence|EcsBinaryWriter|EcsBinaryReader|persistence_contract|[.]ecs_persistence[.]hpp|TaggedProperty|schema_reflection|cooked_relocation|LXES|LXWS|LUX_REBUILD_COMPONENT_SCHEMA|LUX_COMPONENT_SCHEMA|LUX_COMPONENT_SNAPSHOT|LUX_COMPONENT_WORLD_SECTION|lux/cxx/serialization/|lux::cxx::ser|LUX_CLASS[ \\t]*\\(|LUX_ENUM[ \\t]*\\(|SystemExecutionPoint|SystemHookPoint|dispatch_point|ESystemEventTarget::BROADCAST|ScriptEventRegistry|ScriptBindingSession|ScriptComponent|EntityBehavior|default_bindings|EScriptBindingSetMode|ScriptMountFacts|ScriptEventWriter|CppBehaviorScript|PythonSourceScript|SemanticCatalog|TargetCatalog|entity_to_sidecar|entity_slots|hook_range_begin|hook_range_count|hot_path_(allocations|name_lookups|asset_lookups|signature_adaptations|scene_scans)|struct[ ]+LuaComponentBinding[^}]*string_view|struct[ ]+(ScriptBindingTargetCatalogEntry|ExportMethodNode)[^}]*ScriptSemanticType" OR
       content MATCHES "#[ \t]*include[ \t]*[<\"]lux/engine/process/")
        message(FATAL_ERROR "Installed file contains a retired boundary: ${entry}")
    endif()
    if(content MATCHES "lux/engine/ecs/|lux::engine::ecs|lux::ecs::")
        message(FATAL_ERROR
            "Installed file restores retired top-level ECS surface: ${entry}"
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
    "${prefix}/share/lux-engine-world/world/lux-engine-world-world-config-targets.cmake"
    "${prefix}/share/lux-engine-world-asset/world_asset/lux-engine-world-asset-world_asset-config-targets.cmake"
    "${prefix}/share/lux-engine-simulation/simulation_description/lux-engine-simulation-simulation_description-config-targets.cmake"
    "${prefix}/share/lux-engine-simulation-asset/simulation_asset/lux-engine-simulation-asset-simulation_asset-config-targets.cmake"
    "${prefix}/share/lux-engine-simulation-ecs/core/lux-engine-simulation-ecs-core-config-targets.cmake"
    "${prefix}/share/lux-engine-resource-script/script_asset/lux-engine-resource-script-script_asset-config-targets.cmake"
    "${prefix}/share/lux-engine-simulation/simulation_script/lux-engine-simulation-simulation_script-config-targets.cmake"
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
