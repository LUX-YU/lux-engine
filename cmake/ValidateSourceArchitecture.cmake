cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED LUX_SOURCE_DIR)
    message(FATAL_ERROR "LUX_SOURCE_DIR is required")
endif()
if(NOT DEFINED LUX_REPORT_PATH)
    set(LUX_REPORT_PATH "${CMAKE_CURRENT_BINARY_DIR}/semantic-architecture-debt.txt")
endif()

file(TO_CMAKE_PATH "${LUX_SOURCE_DIR}" source_root)

# HookChannel is transport, never a callback registry or a synchronized per-record queue.
set(hook_channel_header "${source_root}/engine/domain/simulation/system/include/lux/engine/simulation/HookChannel.hpp")
if(EXISTS "${source_root}/engine/domain/simulation/system/include/lux/engine/simulation/EventPoint.hpp")
    message(FATAL_ERROR "Architecture: EventPoint runtime transport was hard-cut to HookChannel.")
endif()
if(EXISTS "${hook_channel_header}")
    file(READ "${hook_channel_header}" hook_channel_source)
    if(hook_channel_source MATCHES "std::(mutex|recursive_mutex|atomic)|connect\\(|disconnect\\(|drain\\(")
        message(FATAL_ERROR "Architecture: HookChannel must remain unsynchronized typed transport without subscribers.")
    endif()
endif()
file(READ "${source_root}/engine/scene/integration/script/src/ScriptRuntimeSystem.cpp" script_scene_source)
if(script_scene_source MATCHES "addStablePointTask|ScriptRuntimeSystem::executeStablePoint")
    message(FATAL_ERROR "Architecture: gameplay Script pumping belongs to the compiled Simulation Hook, not Scene tasks.")
endif()
file(GLOB_RECURSE task_core_sources LIST_DIRECTORIES false
    "${source_root}/modules/core/task/*.hpp" "${source_root}/modules/core/task/*.cpp")
foreach(task_source IN LISTS task_core_sources)
    file(READ "${task_source}" task_source_text)
    if(task_source_text MATCHES "ScriptSystem|HookChannel|HookPointId|SimulationClock")
        message(FATAL_ERROR "Architecture: reusable TaskGraph must not acquire Simulation/Script ontology: ${task_source}")
    endif()
endforeach()

if(EXISTS "${source_root}/ecs")
    message(FATAL_ERROR
        "Architecture: the retired top-level ecs/ tree must remain quarantined."
    )
endif()
if(EXISTS "${source_root}/engine/ecs")
    message(FATAL_ERROR
        "Architecture: retired top-level engine/ecs domain must not be restored."
    )
endif()
foreach(retired_root IN ITEMS
    "${source_root}/engine/flowforge"
    "${source_root}/engine/graph_kit"
    "${source_root}/engine/simulation"
    "${source_root}/engine/world"
    "${source_root}/engine/domain/world_identity"
    "${source_root}/engine/domain/partition/core"
    "${source_root}/engine/domain/world/core"
    "${source_root}/engine/domain/simulation/core"
    "${source_root}/engine/domain/simulation/systems"
    "${source_root}/engine/domain/simulation/script_binding"
    "${source_root}/engine/domain/simulation/script"
    "${source_root}/engine/authoring"
    "${source_root}/engine/authoring/script_binding"
    "${source_root}/engine/authoring/flowforge"
    "${source_root}/engine/tools"
    "${source_root}/engine/toolchain/script"
    "${source_root}/engine/process/asset"
    "${source_root}/engine/process/world"
    "${source_root}/engine/scene/core"
    "${source_root}/engine/scene/runtime"
    "${source_root}/modules/resource/asset/script"
)
    if(EXISTS "${retired_root}")
        message(FATAL_ERROR
            "Architecture: retired source root remains present: ${retired_root}"
        )
    endif()
endforeach()
foreach(forbidden_domain_root IN ITEMS common utils services)
    if(EXISTS "${source_root}/engine/domain/${forbidden_domain_root}")
        message(FATAL_ERROR
            "Architecture: generic engine/domain/${forbidden_domain_root} is forbidden; use a concrete domain leaf."
        )
    endif()
endforeach()
if(NOT EXISTS "${source_root}/legacy/ecs" OR
   NOT EXISTS "${source_root}/legacy/engine" OR
   NOT EXISTS "${source_root}/legacy/modules/resource/asset-runtime")
    message(FATAL_ERROR
        "Architecture: ECS, Engine and asset-runtime quarantine roots are required."
    )
endif()

file(GLOB_RECURSE production_sources LIST_DIRECTORIES false
    "${source_root}/modules/*/include/*.hpp"
    "${source_root}/modules/*/sinclude/*.hpp"
    "${source_root}/modules/*/pinclude/*.hpp"
    "${source_root}/modules/*/src/*.cpp"
    "${source_root}/modules/*/*/include/*.hpp"
    "${source_root}/modules/*/*/sinclude/*.hpp"
    "${source_root}/modules/*/*/pinclude/*.hpp"
    "${source_root}/modules/*/*/src/*.cpp"
    "${source_root}/modules/*/*/*/include/*.hpp"
    "${source_root}/modules/*/*/*/src/*.cpp"
    "${source_root}/engine/domain/world/*/include/*.hpp"
    "${source_root}/engine/domain/world/*/sinclude/*.hpp"
    "${source_root}/engine/domain/world/*/pinclude/*.hpp"
    "${source_root}/engine/domain/world/*/src/*.cpp"
    "${source_root}/engine/domain/world/identity/include/*.hpp"
    "${source_root}/engine/domain/world/identity/sinclude/*.hpp"
    "${source_root}/engine/domain/world/identity/pinclude/*.hpp"
    "${source_root}/engine/domain/world/identity/src/*.cpp"
    "${source_root}/engine/domain/partition/*/include/*.hpp"
    "${source_root}/engine/domain/partition/*/sinclude/*.hpp"
    "${source_root}/engine/domain/partition/*/pinclude/*.hpp"
    "${source_root}/engine/domain/partition/*/src/*.cpp"
    "${source_root}/engine/domain/system/*/include/*.hpp"
    "${source_root}/engine/domain/system/*/sinclude/*.hpp"
    "${source_root}/engine/domain/system/*/pinclude/*.hpp"
    "${source_root}/engine/domain/system/*/src/*.cpp"
    "${source_root}/engine/domain/spatial/*/include/*.hpp"
    "${source_root}/engine/domain/spatial/*/sinclude/*.hpp"
    "${source_root}/engine/domain/spatial/*/pinclude/*.hpp"
    "${source_root}/engine/domain/spatial/*/src/*.cpp"
    "${source_root}/engine/domain/simulation/*/include/*.hpp"
    "${source_root}/engine/domain/simulation/*/sinclude/*.hpp"
    "${source_root}/engine/domain/simulation/*/pinclude/*.hpp"
    "${source_root}/engine/domain/simulation/*/src/*.cpp"
    "${source_root}/engine/domain/simulation/*/*/include/*.hpp"
    "${source_root}/engine/domain/simulation/*/*/sinclude/*.hpp"
    "${source_root}/engine/domain/simulation/*/*/pinclude/*.hpp"
    "${source_root}/engine/domain/simulation/*/*/src/*.cpp"
    "${source_root}/engine/process/*/include/*.hpp"
    "${source_root}/engine/process/*/sinclude/*.hpp"
    "${source_root}/engine/process/*/pinclude/*.hpp"
    "${source_root}/engine/process/*/src/*.cpp"
    "${source_root}/engine/scene/*/include/*.hpp"
    "${source_root}/engine/scene/*/sinclude/*.hpp"
    "${source_root}/engine/scene/*/pinclude/*.hpp"
    "${source_root}/engine/scene/*/src/*.cpp"
    "${source_root}/engine/scene/*/*/include/*.hpp"
    "${source_root}/engine/scene/*/*/sinclude/*.hpp"
    "${source_root}/engine/scene/*/*/pinclude/*.hpp"
    "${source_root}/engine/scene/*/*/src/*.cpp"
    "${source_root}/engine/authoring/*/include/*.hpp"
    "${source_root}/engine/authoring/*/src/*.cpp"
    "${source_root}/engine/editor/*/include/*.hpp"
    "${source_root}/engine/editor/*/src/*.cpp"
    "${source_root}/engine/toolchain/*/include/*.hpp"
    "${source_root}/engine/toolchain/*/src/*.cpp"
)

string(CONCAT retired_script_compression_vocabulary
    "ScriptAssetContent|Script(CallFrame|Value|Signature)[.]hpp|"
    "ScriptSemantic(Type|Layout|TypeTraits)|sameScriptSignature|scriptBuiltinLayout|"
    "flowforge_script_compiler|flowforge_compiler_dialect"
)

foreach(source IN LISTS production_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    file(READ "${source}" content)

    if(content MATCHES
       "lux/engine/authoring/|lux::authoring|lux/engine/domain/WorldObjectId[.]hpp|lux::domain::WorldObjectId|lux/engine/process/(asset|world)/|lux::process::(asset|world)::|lux/engine/simulation/systems/|lux/engine/scene/runtime/(render|world)/|lux/engine/toolchain/asset/material/")
        message(FATAL_ERROR
            "Architecture: active source '${normalized}' references a retired post-cleanup API path."
        )
    endif()

    if(content MATCHES "(^|[\r\n])[ \t]*throw([ \t;(]|$)")
        message(FATAL_ERROR
            "Architecture: production source '${normalized}' actively throws across the Lux failure boundary."
        )
    endif()

    if(content MATCHES
       "SystemExecutionPoint|SystemHookPoint|execution_points|dispatch_point|ESystemEventTarget::BROADCAST|ScriptEventRegistry|GlobalScriptBindingManager|ScriptBindingSession|ScriptComponent|EntityBehavior|LUX_SCRIPT_METHOD|LUX_BIND_POINT|LUX_BIND_EVENT|LUX_BEHAVIOR_LIFECYCLE|@lux[.]bind_(point|event)|default_bindings|EScriptBindingSetMode|ScriptMountFacts|ScriptEventWriter|CppBehaviorScript|PythonSourceScript|SemanticCatalog|TargetCatalog|entity_to_sidecar|entity_slots|hook_range_begin|hook_range_count|hot_path_(allocations|name_lookups|asset_lookups|signature_adaptations|scene_scans)|lua_pushlightuserdata[^;]*instance|value[.]name[ ]*==[ ]*node->name|struct[ ]+LuaComponentBinding[^}]*string_view|struct[ ]+(ScriptBindingTargetCatalogEntry|ExportMethodNode)[^}]*ScriptSemanticType" OR
       content MATCHES "${retired_script_compression_vocabulary}")
        message(FATAL_ERROR
            "Architecture: active source '${normalized}' restores a retired SystemHook/ScriptBinding API."
        )
    endif()

    if(content MATCHES
       "#[ \t]*include[ \t]*[<\"]([^\">]*/)?legacy/|[/\\]legacy[/\\]")
        message(FATAL_ERROR
            "Architecture: production source '${normalized}' reaches into legacy/."
        )
    endif()

    if(content MATCHES
       "AssetManager|AssetRef|AssetLoadPort|AssetServices|asset_id_t")
        message(FATAL_ERROR
            "Architecture: active source '${normalized}' restores retired L0 asset ownership vocabulary."
        )
    endif()

    if(content MATCHES
       "RenderFrame(Session|Channel)|FrameProgram(Builder)?|FrameMemoryHints|FrameProgressToken|RenderTickPipeline|EOperationLane::Frame|ERequestLane::FRAME|(^|[^A-Za-z0-9_])(param_)?lane[ \t]*=[ \t]*frame([^A-Za-z0-9_]|$)")
        message(FATAL_ERROR
            "Architecture: active source '${normalized}' restores retired Render Frame-program vocabulary."
        )
    endif()

    if(content MATCHES "sceneRegistry")
        message(FATAL_ERROR
            "Architecture: active source '${normalized}' restores the retired RenderScene registry accessor."
        )
    endif()

    if(normalized MATCHES "/engine/scene/" AND content MATCHES
       "WorldStreamingBinding|StreamingManager|SceneServices|SceneContext|SystemFactoryRegistry|SimulationContext|WorldPartitionWorkspace|WorldMaterializationPlan|WorldMaterializationRegistry|TimeDomainRegistry|ClockManager|PresentationManager|LaneManager|ScenePhaseManager|AssetDemandKey|DemandTracker|ResidencyBridge|ResourceDemandRegistry")
        message(FATAL_ERROR
            "Architecture: new Scene production source '${normalized}' uses phase-held framework vocabulary."
        )
    endif()

    if(normalized MATCHES "/engine/scene/integration/render/" AND content MATCHES
       "PresentationRegistry|PresentationScene|PresentationDelta|RenderBridge|RenderSynchronizer|RenderServices|AssetResolver|PendingMeshBinding|ResidencyManager|DemandTracker")
        message(FATAL_ERROR
            "Architecture: L3 Render integration '${normalized}' restores a held bridge/registry/residency framework."
        )
    endif()

    if(content MATCHES "WorldPartitionWorkspace|WorldPartitioner[.]hpp")
        message(FATAL_ERROR
            "Architecture: active source '${normalized}' restores the retired World partition workspace API."
        )
    endif()

    if(content MATCHES "WorldPartitionOrdinal|WorldPartitionIndexTypeId|worldPartitionIndexTypeId")
        message(FATAL_ERROR
            "Architecture: active source '${normalized}' uses retired World-owned partition identity."
        )
    endif()

    if(content MATCHES
       "#[ \t]*include[ \t]*[<\"]lux/cxx/serialization/|lux::cxx::ser")
        message(FATAL_ERROR
            "Architecture: Engine source '${normalized}' uses configuration serialization instead of lux::serialization."
        )
    endif()

    if(normalized MATCHES "/engine/domain/simulation/")
        set(simulation_dependency_content "${content}")
        string(REGEX REPLACE
            "#[ \\t]*include[ \\t]*[<\"]lux/engine/world/WorldObjectId[.]hpp[>\"]"
            ""
            simulation_dependency_content
            "${simulation_dependency_content}"
        )
        if(simulation_dependency_content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(world|scene|runtime|process|editor|authoring|toolchain|host|extensions)/")
            message(FATAL_ERROR
                "Architecture: L1 Simulation source '${normalized}' includes World or an upper-layer API."
            )
        endif()
        if(content MATCHES
           "SceneServices|ISystem|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|cooked_relocation|LXES|LXWS|ComponentCodec|ComponentPersistence|ComponentLoadBinding|ComponentLoadSet|EcsBinaryWriter|EcsBinaryReader|persistence_contract|ecs_persistence|TaggedProperty|RefClass|RefField|WorldSection|PersistentEntity|PersistentId|ecs_load|section[ \t]*=[ \t]*(LOAD|OMIT)|LUX_REBUILD_COMPONENT_SCHEMA|LUX_COMPONENT_SCHEMA|LUX_COMPONENT_SNAPSHOT|LUX_COMPONENT_WORLD_SECTION|CloneFn|default_emplace|COPY_WITHOUT_CLONE|ecs::World([^A-Za-z0-9_]|$)|WorldMutation|WorldChange|WorldCommand|WorldSnapshot|WORLD_BUSY|worldStructureWrite|HierarchyChangeCursor|HierarchyChanges|kHierarchyChangeCapacity|World::registry[ \t\r\n]*\\(|setParent[ \t\r\n]*\\(|clearParent[ \t\r\n]*\\(|EcsState|EcsMutation|SimulationEcsMutation|EcsChangeJournal|ChangeCursor|ComponentChanges|EntityChanges|EcsTaskAccess|EcsTaskResources|HierarchySystem|executeSimulationStep|FrameInfo" AND
           NOT normalized MATCHES "/engine/domain/simulation/scripting/cpp_static/")
            message(FATAL_ERROR
                "Architecture: L1 Simulation source '${normalized}' restores retired vocabulary."
            )
        endif()
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/ecs/(Schedule|ScheduleEdit|ScheduleError|SystemFrame|SystemHandle|SystemPhase|SystemSetId)[.]hpp")
            message(FATAL_ERROR
                "Architecture: L1 Simulation source '${normalized}' includes retired Schedule execution API."
            )
        endif()
        if(content MATCHES
           "connectConstruct|connectUpdate|connectDestroy|observer_relations_")
            message(FATAL_ERROR
                "Architecture: L1 Simulation source '${normalized}' restores a retired observer wrapper."
            )
        endif()
        if(content MATCHES
           "on_construct[ \t\r\n]*<|on_update[ \t\r\n]*<|on_destroy[ \t\r\n]*<" AND
           NOT normalized MATCHES
               "/engine/domain/simulation/(ecs/(hierarchy|transform)|scripting|builtin/(script|transform))/" AND
           NOT normalized MATCHES
               "/engine/domain/simulation/ecs/core/(include/lux/engine/simulation/ecs/ComponentChangeSet[.]hpp|test/(reactive_storage_probe|extraction_change_set_test)[.]cpp)$")
            message(FATAL_ERROR
                "Architecture: EnTT signal ownership '${normalized}' is outside a concrete reactive Simulation subsystem."
            )
        endif()
        if(content MATCHES
           "AssetStore|AssetClient|AssetLease|AssetManager|AssetRef|AssetLoadPort|AssetServices")
            message(FATAL_ERROR
                "Architecture: L1 Simulation source '${normalized}' depends on deferred asset ownership API."
            )
        endif()
        if(content MATCHES
           "printf[ \t\r\n]*\\(|fprintf[ \t\r\n]*\\(|std::(cout|cerr)|MessageBox[AW]?[ \t\r\n]*\\(")
            message(FATAL_ERROR
                "Architecture: L1 Simulation source '${normalized}' performs terminal I/O."
            )
        endif()
    endif()

    if(normalized MATCHES "/modules/resource/asset/")
        if(content MATCHES "lux/engine/core/async_port/")
            message(FATAL_ERROR
                "Architecture: L0 asset mechanism '${normalized}' depends on async orchestration."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/domain/simulation/(description|asset)/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/object/|(^|[^A-Za-z0-9_])(Object|Signal|RefMethod|RefObject)([^A-Za-z0-9_]|$)")
            message(FATAL_ERROR
                "Architecture: descriptive Simulation source '${normalized}' owns runtime Object/Signal state."
            )
        endif()
    endif()

    if(normalized MATCHES "/modules/core/task/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(ecs|simulation|object|process|scene)/")
            message(FATAL_ERROR
                "Architecture: L0 core::task source '${normalized}' depends on an upper-layer subsystem."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/domain/world/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"](lux/engine/ecs/|entt/|lux/engine/(simulation|scene|asset)/|lux/engine/math/)")
            message(FATAL_ERROR
                "Architecture: descriptive World source '${normalized}' depends on ECS/runtime/domain math."
            )
        endif()
        if(content MATCHES "namespace[ \t]+lux::ecs|entt::|lux::ecs::")
            message(FATAL_ERROR
                "Architecture: descriptive World source '${normalized}' contains ECS semantics."
            )
        endif()
        if(content MATCHES "Spatial(2D|3D)PartitionIndex")
            message(FATAL_ERROR
                "Architecture: World source '${normalized}' owns a runtime Spatial index object."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/scene/description/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/scene/(SceneSystem|SceneBuilder|SceneMetaManager|Scene)[.]hpp")
            message(FATAL_ERROR
                "Architecture: Scene description source '${normalized}' depends on Scene runtime composition."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/scene/system/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/scene/(SceneBuilder|SceneMetaManager|Scene)[.]hpp")
            message(FATAL_ERROR
                "Architecture: SceneSystem contract '${normalized}' depends on Scene core/meta."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/scene/meta/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/scene/(Scene|SceneBuilder|RenderRuntime|RenderSystem|RenderSyncPipeline)[.]hpp")
            message(FATAL_ERROR
                "Architecture: SceneMeta source '${normalized}' depends on Scene runtime composition."
            )
        endif()
        if(content MATCHES "(add|remove|replace|update)(System|Component|RenderFeature)|snapshot[ \t\r\n]*[(]")
            message(FATAL_ERROR
                "Architecture: immutable SceneMetaManager source '${normalized}' exposes hot mutation."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/domain/partition/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(world|simulation|process|scene|runtime|authoring|toolchain|editor|host|extensions)/")
            message(FATAL_ERROR
                "Architecture: neutral Partition source '${normalized}' depends on a concrete or upper domain."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/domain/system/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(world|simulation|process|scene|render|runtime|authoring|toolchain|editor|host|extensions)/")
            message(FATAL_ERROR
                "Architecture: shared System foundation '${normalized}' depends on a concrete or upper domain."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/domain/world/identity/")
        set(world_identity_dependency_content "${content}")
        string(REGEX REPLACE
            "#[ \\t]*include[ \\t]*[<\"]lux/engine/world/WorldObjectId[.]hpp[>\"]"
            ""
            world_identity_dependency_content
            "${world_identity_dependency_content}"
        )
        if(world_identity_dependency_content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(world|simulation|process|scene|runtime|authoring|toolchain|editor|host|extensions)/")
            message(FATAL_ERROR
                "Architecture: World identity source '${normalized}' depends on a concrete or upper domain."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/domain/spatial/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(world|simulation|process|scene|runtime|authoring|toolchain|editor|host|extensions)/")
            message(FATAL_ERROR
                "Architecture: Spatial foundation source '${normalized}' depends on World, Simulation or an upper layer."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/domain/simulation/")
        set(simulation_dependency_content "${content}")
        string(REGEX REPLACE
            "#[ \\t]*include[ \\t]*[<\"]lux/engine/world/WorldObjectId[.]hpp[>\"]"
            ""
            simulation_dependency_content
            "${simulation_dependency_content}"
        )
        if(simulation_dependency_content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(world|process|scene|authoring|toolchain|editor|host)/")
            message(FATAL_ERROR
                "Architecture: L1 Simulation source '${normalized}' includes World or an upper-layer API."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/process/execution/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"](lux/engine/(world|simulation|scene|render|authoring|editor|toolchain|host|runtime)/|asio/|tbb/|exec/static_thread_pool)|legacy/engine/runtime/execution")
            message(FATAL_ERROR
                "Architecture: Process execution source '${normalized}' depends on an upper/domain/legacy runtime."
            )
        endif()
    elseif(normalized MATCHES "/engine/process/world_loading/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(simulation|scene|render|authoring|editor|toolchain|host|runtime|extensions)/")
            message(FATAL_ERROR
                "Architecture: Process World workflow '${normalized}' depends on Simulation, Scene or an upper layer."
            )
        endif()
        if(content MATCHES "WorldMaterializer|simulation::ecs|entt::|Registry")
            message(FATAL_ERROR
                "Architecture: Process World workflow '${normalized}' owns Scene/ECS adoption."
            )
        endif()
    elseif(normalized MATCHES "/engine/process/asset_loading/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(world|simulation|scene|render|authoring|editor|toolchain|host|runtime|extensions)/")
            message(FATAL_ERROR
                "Architecture: Process Asset workflow '${normalized}' depends on a domain or upper layer."
            )
        endif()
    endif()

    if(content MATCHES
       "lux/engine/simulation/(SystemTypeId|SystemDescription|SystemEndpointId|SystemEndpointSpec|SystemConcept|SystemRegistry)[.]hpp")
        message(FATAL_ERROR "Architecture: active source '${normalized}' includes retired generic System vocabulary.")
    endif()

    if(normalized MATCHES "/engine/process/" AND content MATCHES
       "ProcessRuntime|AsyncRuntime|AsyncRuntimeBuilder|ProcessBuilder|ProcessScope|ProcessScheduler|OperationRegistry|parallelTransform|BatchJoin|AsyncGraph|ProcessSender|JobSystem|JobManager|std::function")
        message(FATAL_ERROR
            "Architecture: Process source '${normalized}' restores a runtime wrapper, registry or deferred API."
        )
    endif()
    if(normalized MATCHES "/engine/process/" AND content MATCHES "BlockingScheduler" AND
       NOT normalized MATCHES "/engine/process/(execution|asset_loading)/")
        message(FATAL_ERROR
            "Architecture: BlockingScheduler is authorized only for execution and the V2 AssetRead endpoint."
        )
    endif()
endforeach()

foreach(hot_runtime_root IN ITEMS
    "${source_root}/engine/domain/simulation/system"
    "${source_root}/engine/domain/simulation/builtin/script"
)
    file(GLOB_RECURSE hot_runtime_sources LIST_DIRECTORIES false
        "${hot_runtime_root}/*.hpp"
        "${hot_runtime_root}/*.cpp"
    )
    foreach(source IN LISTS hot_runtime_sources)
        file(READ "${source}" content)
        if(content MATCHES "lux/engine/meta/RuntimeObject[.]hpp")
            message(FATAL_ERROR
                "Architecture: Simulation hot package '${source}' depends on RuntimeObject."
            )
        endif()
    endforeach()
endforeach()

# A source root is either a leaf package or a collection. A leaf package may
# use CMakeLists.txt below src/ for private implementation helpers, but it may
# not also aggregate sibling production packages.
file(GLOB_RECURSE production_cmake_files LIST_DIRECTORIES false
    "${source_root}/modules/*/CMakeLists.txt"
    "${source_root}/engine/*/CMakeLists.txt"
)
string(CONCAT package_auxiliary_root_pattern
    "^(include|sinclude|pinclude|src|test|cmake|third_party|samples|"
    "assets|template|generated|data)$"
)
foreach(cmake_file IN LISTS production_cmake_files)
    file(TO_CMAKE_PATH "${cmake_file}" normalized_cmake)
    if(normalized_cmake MATCHES "/(test|cmake|third_party|samples|assets|template|generated|data|src)/")
        continue()
    endif()

    get_filename_component(package_root "${cmake_file}" DIRECTORY)
    file(GLOB_RECURSE owned_package_sources LIST_DIRECTORIES false
        "${package_root}/include/*.h"
        "${package_root}/include/*.hpp"
        "${package_root}/sinclude/*.h"
        "${package_root}/sinclude/*.hpp"
        "${package_root}/pinclude/*.h"
        "${package_root}/pinclude/*.hpp"
        "${package_root}/src/*.c"
        "${package_root}/src/*.cpp"
    )
    if(NOT owned_package_sources)
        continue()
    endif()

    file(GLOB direct_children LIST_DIRECTORIES true "${package_root}/*")
    foreach(child IN LISTS direct_children)
        if(NOT IS_DIRECTORY "${child}")
            continue()
        endif()
        get_filename_component(child_name "${child}" NAME)
        if(child_name MATCHES "${package_auxiliary_root_pattern}")
            continue()
        endif()
        if(EXISTS "${child}/CMakeLists.txt")
            message(FATAL_ERROR
                "Architecture: package '${package_root}' also aggregates child package '${child_name}'."
            )
        endif()
    endforeach()
endforeach()

set(script_backend_runtime_sources
    "${source_root}/engine/domain/simulation/scripting/cpp_static/src/CppStaticScriptBridge.cpp"
    "${source_root}/engine/domain/simulation/scripting/native/src/NativeScriptBackend.cpp"
    "${source_root}/engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp"
)
foreach(source IN LISTS script_backend_runtime_sources)
    if(NOT EXISTS "${source}")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES
       "new[ \t\r\n]+[(]std::nothrow[)][ \t\r\n]+Instance|delete[ \t\r\n]+instance|prototypes[.]push_back")
        message(FATAL_ERROR
            "Architecture: Script backend '${source}' restores per-instance heap wrappers or linear prototypes."
        )
    endif()
endforeach()

# SR-2: persistence and World resolution belong to the loading leaf, including the installable runtime surface.
file(GLOB_RECURSE script_runtime_boundary_sources LIST_DIRECTORIES false
    "${source_root}/engine/domain/simulation/builtin/script/include/*.hpp"
    "${source_root}/engine/domain/simulation/builtin/script/src/*.cpp"
)
foreach(source IN LISTS script_runtime_boundary_sources)
    file(READ "${source}" runtime_content)
    if(runtime_content MATCHES "ScriptSystemDescription|WorldObjectResolver|WorldObjectId|lux/engine/world/")
        message(FATAL_ERROR "Architecture: Script runtime '${source}' depends on persistent loading input.")
    endif()
endforeach()

# The replacement roots are held to the final contract while the old roots are
# still being removed in staged commits. This prevents new code from rebuilding
# the retired session/name-routing model under a different target name.
file(GLOB_RECURSE script_system_sources LIST_DIRECTORIES false
    "${source_root}/engine/domain/simulation/scripting/*.hpp"
    "${source_root}/engine/domain/simulation/scripting/*.cpp"
    "${source_root}/engine/domain/simulation/builtin/script/*.hpp"
    "${source_root}/engine/domain/simulation/builtin/script/*.cpp"
)
foreach(source IN LISTS script_system_sources)
    file(READ "${source}" content)
    if(source STREQUAL "${source_root}/engine/domain/simulation/builtin/script/src/ScriptInstances.cpp")
        # SR-3 moved the bounded configuration index to its private owner. Permit
        # binary search only in these two cold assembly operations, never execution.
        set(assembly_starts "ScriptInstances::findMount(" "void ScriptInstances::commitBatch(")
        set(assembly_ends "ScriptInstances::reserveBatch(" "void ScriptInstances::markStatus(")
        foreach(start end IN ZIP_LISTS assembly_starts assembly_ends)
            string(FIND "${content}" "${start}" assembly_begin)
            string(FIND "${content}" "${end}" assembly_end)
            if(assembly_begin LESS 0 OR assembly_end LESS_EQUAL assembly_begin)
                message(FATAL_ERROR "Architecture: missing ScriptInstances cold assembly boundary '${start}'.")
            endif()
            math(EXPR assembly_length "${assembly_end} - ${assembly_begin}")
            string(SUBSTRING "${content}" ${assembly_begin} ${assembly_length} assembly)
            string(REGEX MATCHALL "std::lower_bound" lookups "${assembly}")
            list(LENGTH lookups lookup_count)
            if(NOT lookup_count EQUAL 1)
                message(FATAL_ERROR "Architecture: expected one cold configuration lookup in '${start}'.")
            endif()
            string(REPLACE "std::lower_bound" "SR3_COLD_CONFIGURATION_LOOKUP" checked_assembly "${assembly}")
            string(REPLACE "${assembly}" "${checked_assembly}" content "${content}")
        endforeach()
    endif()
    if(content MATCHES
       "ScriptBindingSession|ScriptComponent|EntityBehavior|EScriptModel|PythonSourceScript|dispatchHook[ \t\r\n]*\\([^(),]*,[^(),]*,[^()]*|AssetManager|AssetClient|AssetLease|Process|Scene|EEndpointMutationError[ \t\r\n]*\\([*]flush\\)|endpoint[^;\r\n]*->[ \t]*flush|ScriptSystemCapacities|EBehaviorStopReason|startInstance|stopInstance|full_resync|sortHandlers|removeHandlers|allocateInstance|findBackend|findHookBucket|findEventBucket|findMethod|std::lower_bound|std::remove_if|ScriptApiManager|CoroutineManager|AsyncManager|EventAwaitManager|EventManager|ScriptEventManager|AwaitableManager|ScriptServices|ServiceRegistry")
        message(FATAL_ERROR
            "Architecture: replacement ScriptSystem source '${source}' restores a retired boundary."
        )
    endif()
endforeach()

file(GLOB_RECURSE generic_system_sources LIST_DIRECTORIES false
    "${source_root}/engine/domain/simulation/system/include/*.hpp"
    "${source_root}/engine/domain/simulation/system/src/*.cpp"
)
foreach(source IN LISTS generic_system_sources)
    file(READ "${source}" content)
    if(content MATCHES
       "function/script|ScriptSemantic|ScriptCallFrame|BoundScriptCall|ScriptAsset")
        message(FATAL_ERROR
            "Architecture: generic System endpoint '${source}' depends on Script ABI."
        )
    endif()
    if(content MATCHES
       "flushMutations|mutation_capacity|struct[ \t\r\n]+Mutation|mutations_")
        message(FATAL_ERROR
            "Architecture: generic System endpoint '${source}' restores deferred topology mutation."
        )
    endif()
    if(content MATCHES
       "(record|dispatch|drain)[^{]*[{][^}]*catch[ \t\r\n]*[(]")
        message(FATAL_ERROR
            "Architecture: Hook/Event hot path '${source}' contains exception handling."
        )
    endif()
endforeach()

if(EXISTS "${source_root}/modules/function/flowforge")
    string(CONCAT flowforge_l0_forbidden
        "lux/engine/(simulation|editor|toolchain)|"
        "lux::engine::(simulation|editor|toolchain)|imgui::|"
        "find_package[ \\t\\r\\n]*\\([ \\t]*(MLIR|LLVM)|[/\\]legacy[/\\]"
    )
    file(GLOB_RECURSE flowforge_sources LIST_DIRECTORIES false
        "${source_root}/modules/function/flowforge/*.hpp"
        "${source_root}/modules/function/flowforge/*.cpp"
        "${source_root}/modules/function/flowforge/*.cmake"
    )
    foreach(source IN LISTS flowforge_sources)
        file(READ "${source}" content)
        if(content MATCHES "${flowforge_l0_forbidden}")
            message(FATAL_ERROR
                "Architecture: L0 FlowForge package '${source}' reaches an upper-layer implementation."
            )
        endif()
    endforeach()
endif()

if(EXISTS "${source_root}/engine/toolchain/flowforge")
    string(CONCAT flowforge_compiler_forbidden
        "lux/engine/(simulation|editor)|"
        "lux::engine::(simulation|editor)|imgui::|[/\\]legacy[/\\]"
    )
    file(GLOB_RECURSE flowforge_compiler_sources LIST_DIRECTORIES false
        "${source_root}/engine/toolchain/flowforge/*.hpp"
        "${source_root}/engine/toolchain/flowforge/*.cpp"
        "${source_root}/engine/toolchain/flowforge/*.cmake"
    )
    foreach(source IN LISTS flowforge_compiler_sources)
        file(READ "${source}" content)
        if(content MATCHES "${flowforge_compiler_forbidden}")
            message(FATAL_ERROR
                "Architecture: FlowForge compiler '${source}' reaches Simulation or Editor."
            )
        endif()
    endforeach()
endif()

if(EXISTS "${source_root}/engine/editor/context")
    set(editor_context_cmake "${source_root}/engine/editor/context/CMakeLists.txt")
    file(READ "${editor_context_cmake}" editor_context_cmake_contract)
    if(NOT editor_context_cmake_contract MATCHES "LAYER[ \t\r\n]+EDITOR" OR
       NOT editor_context_cmake_contract MATCHES "PRODUCT[ \t\r\n]+EDITOR" OR
       editor_context_cmake_contract MATCHES
           "toolchain_material|material_compiler|flowforge_compiler|CompilerManager|ServiceRegistry")
        message(FATAL_ERROR
            "Architecture: Editor Context must remain an EDITOR foundation without concrete Tool dependencies."
        )
    endif()

    file(GLOB_RECURSE editor_context_sources LIST_DIRECTORIES false
        "${source_root}/engine/editor/context/include/*.hpp"
        "${source_root}/engine/editor/context/src/*.cpp"
    )
    foreach(source IN LISTS editor_context_sources)
        file(READ "${source}" content)
        if(content MATCHES
           "EditorContext[ \t\r\n]*::[ \t\r\n]*instance|EditorServices|ServiceRegistry|ServiceProvider|resolveService|getAnything|CompilerManager|CompilerRegistry")
            message(FATAL_ERROR
                "Architecture: Editor Context '${source}' restores a singleton, service locator or compiler registry."
            )
        endif()
        if(content MATCHES
           "lux/engine/(material/Compiler|flowforge/Compiler)[.]hpp|toolchain_(material|flowforge)|flowforge_compiler")
            message(FATAL_ERROR
                "Architecture: Editor Context '${source}' depends on a concrete Toolchain tool."
            )
        endif()
    endforeach()

    set(editor_context_header
        "${source_root}/engine/editor/context/include/lux/engine/editor/EditorContext.hpp"
    )
    file(READ "${editor_context_header}" editor_context_contract)
    foreach(required_accessor IN ITEMS toolchain vfs assetRead execution tasks selection ui sceneMeta)
        if(NOT editor_context_contract MATCHES "${required_accessor}[ \t\r\n]*[(]")
            message(FATAL_ERROR
                "Architecture: EditorContext is missing normative '${required_accessor}()' capability."
            )
        endif()
    endforeach()
    if(editor_context_contract MATCHES
       "(Toolset|asset::AssetVfs|process::TaskScope|EditorSelection|ui::UISession|scene::SceneMetaManager)[ \t]+[a-z_]+_;")
        message(FATAL_ERROR
            "Architecture: v2 EditorContext must not value-own application service lifetime."
        )
    endif()
endif()

if(EXISTS "${source_root}/engine/editor/application")
    set(editor_application_cmake "${source_root}/engine/editor/application/CMakeLists.txt")
    file(READ "${editor_application_cmake}" editor_application_cmake_contract)
    if(NOT editor_application_cmake_contract MATCHES "TARGET[ \t\r\n]+editor_application" OR
       NOT editor_application_cmake_contract MATCHES "LAYER[ \t\r\n]+EDITOR" OR
       NOT editor_application_cmake_contract MATCHES "ROLE[ \t\r\n]+COMPOSITION" OR
       NOT editor_application_cmake_contract MATCHES "add_executable[ \t\r\n]*[(][ \t\r\n]*lux_editor")
        message(FATAL_ERROR
            "Architecture: EditorApplication must be the L5 EDITOR composition leaf producing lux_editor."
        )
    endif()
    file(GLOB_RECURSE editor_application_sources LIST_DIRECTORIES false
        "${source_root}/engine/editor/application/*.hpp"
        "${source_root}/engine/editor/application/*.cpp"
    )
    foreach(source IN LISTS editor_application_sources)
        file(READ "${source}" content)
        if(content MATCHES "ProductHost|ApplicationServices|ServiceRegistry|EditorHost|EditorApplication::instance")
            message(FATAL_ERROR
                "Architecture: EditorApplication '${source}' restores a Host layer or service locator."
            )
        endif()
    endforeach()
endif()

set(asset_vfs_header
    "${source_root}/modules/resource/asset/include/lux/engine/resource/asset/storage/AssetVfs.hpp"
)
file(READ "${asset_vfs_header}" asset_vfs_contract)
if(NOT asset_vfs_contract MATCHES "class[ \t\r\n]+LUX_ASSET_PUBLIC[ \t\r\n]+AssetVfsView" OR
   NOT asset_vfs_contract MATCHES "AssetVfsView[ \t\r\n]+view[(]" OR
   asset_vfs_contract MATCHES "AssetVfs::Get|static[ \t\r\n]+AssetVfs")
    message(FATAL_ERROR
        "Architecture: AssetVfs must expose explicit control plane + AssetVfsView without global state."
    )
endif()

if(EXISTS "${source_root}/modules/function/graph")
    set(function_graph_cmake "${source_root}/modules/function/graph/CMakeLists.txt")
    file(READ "${function_graph_cmake}" function_graph_cmake_contract)
    if(NOT function_graph_cmake_contract MATCHES "LAYER[ \t\r\n]+FUNCTION" OR
       function_graph_cmake_contract MATCHES "LAYER[ \t\r\n]+(EDITOR|TOOLCHAIN)")
        message(FATAL_ERROR "Architecture: shared Graph Source is not a FUNCTION foundation.")
    endif()
    file(GLOB_RECURSE function_graph_sources LIST_DIRECTORIES false
        "${source_root}/modules/function/graph/*.hpp"
        "${source_root}/modules/function/graph/*.cpp"
        "${source_root}/modules/function/graph/*.cmake"
    )
    foreach(source IN LISTS function_graph_sources)
        file(READ "${source}" content)
        if(content MATCHES
           "lux/engine/(editor|material|flowforge|toolchain)/|imgui|ImGui|RuntimeObject|EValueType|RefType")
            message(FATAL_ERROR
                "Architecture: shared Graph Source '${source}' acquired domain, Editor or Toolchain semantics."
            )
        endif()
    endforeach()
endif()

if(EXISTS "${source_root}/engine/editor/node_graph")
    set(node_graph_editor_cmake "${source_root}/engine/editor/node_graph/CMakeLists.txt")
    file(READ "${node_graph_editor_cmake}" node_graph_editor_cmake_contract)
    if(NOT node_graph_editor_cmake_contract MATCHES "LAYER[ \t\r\n]+EDITOR" OR
       node_graph_editor_cmake_contract MATCHES "LAYER[ \t\r\n]+TOOLCHAIN")
        message(FATAL_ERROR "Architecture: Node Graph Editor is not classified as the EDITOR layer.")
    endif()
    file(GLOB_RECURSE node_graph_editor_sources LIST_DIRECTORIES false
        "${source_root}/engine/editor/node_graph/*.hpp"
        "${source_root}/engine/editor/node_graph/*.cpp"
        "${source_root}/engine/editor/node_graph/*.cmake"
    )
    foreach(source IN LISTS node_graph_editor_sources)
        if(source MATCHES "[/\\]test[/\\]")
            continue()
        endif()
        file(READ "${source}" content)
        if(content MATCHES "flowforge|engine/simulation|resource/asset|lux/engine/material/|mlir|llvm|[/\\]legacy[/\\]")
            message(FATAL_ERROR
                "Architecture: Node Graph Editor package '${source}' is not domain independent."
            )
        endif()
        if(content MATCHES "IGraphView|IGraphSchema|class[ \t]+GraphEditor|GraphCommandStack")
            message(FATAL_ERROR
                "Architecture: Node Graph Editor '${source}' restores the retired projection/monolith API."
            )
        endif()
    endforeach()
endif()

if(EXISTS "${source_root}/modules/function/ui")
    file(GLOB_RECURSE ui_public_headers LIST_DIRECTORIES false
        "${source_root}/modules/function/ui/include/*.hpp"
        "${source_root}/modules/function/ui/include/*.h"
    )
    foreach(source IN LISTS ui_public_headers)
        file(READ "${source}" content)
        if(content MATCHES
           "imgui|ImGui|ImVec[24]|ImTextureID|ImDraw(Data|List)|ImGui[A-Za-z0-9_]*Flags|ax::NodeEditor")
            message(FATAL_ERROR
                "Architecture: Lux UI public header '${source}' exposes a backend type/include."
            )
        endif()
    endforeach()

    set(ui_cmake "${source_root}/modules/function/ui/CMakeLists.txt")
    file(READ "${ui_cmake}" ui_cmake_contract)
    if(ui_cmake_contract MATCHES "\"find_package[(]imgui" OR
       NOT ui_cmake_contract MATCHES "PRIVATE[ \t\r\n]+ui_imgui_backend")
        message(FATAL_ERROR
            "Architecture: Lux UI exported target still propagates ImGui or lacks its private concrete backend."
        )
    endif()

    set(ui_frame_source "${source_root}/modules/function/ui/src/Frame.cpp")
    file(READ "${ui_frame_source}" ui_frame_contract)
    if(NOT ui_frame_contract MATCHES
       "return active_ [?] detail::acceptDragDropPayloadInActiveTarget[(][)] : std::nullopt")
        message(FATAL_ERROR
            "Architecture: DropTargetScope::accept must not begin a nested backend drop-target scope."
        )
    endif()
endif()

foreach(render_protocol_header IN ITEMS
    "${source_root}/modules/function/render/client/include/lux/engine/function/render/client/RenderProtocol.hpp"
    "${source_root}/modules/function/render/client/include/lux/engine/function/render/client/protocol/RenderCommTypes.hpp"
)
    file(READ "${render_protocol_header}" render_protocol_contract)
    if(render_protocol_contract MATCHES
       "SubmitImGuiDrawDataPayload|ImGuiDrawData|ImGuiCommConfig")
        message(FATAL_ERROR
            "Architecture: generic Render protocol '${render_protocol_header}' exposes private UI vocabulary."
        )
    endif()
endforeach()

file(GLOB_RECURSE script_common_contract_headers LIST_DIRECTORIES false
    "${source_root}/engine/domain/simulation/scripting/core/include/*.hpp"
    "${source_root}/engine/domain/simulation/builtin/script/include/*.hpp"
)
foreach(source IN LISTS script_common_contract_headers)
    file(READ "${source}" content)
    if(content MATCHES
       "std::(function|any|type_index|coroutine_handle)|lua_State|ScriptApiManager|CoroutineManager|AsyncManager|EventAwaitManager|ScriptServices|SceneServices|ServiceRegistry")
        message(FATAL_ERROR
            "Architecture: common Script contract '${source}' exposes a language runtime or service-locator boundary."
        )
    endif()
endforeach()

set(script_description_header
    "${source_root}/modules/resource/description/include/lux/engine/description/Script.hpp"
)
file(READ "${script_description_header}" script_description_contract)
if(NOT script_description_contract MATCHES "ScriptApiRequirement" OR
   script_description_contract MATCHES "ScriptApiRequirement[^}]*([Pp]rovider|SystemInstanceId)")
    message(FATAL_ERROR
        "Architecture: ScriptArtifact requirements are missing or contain provider-specific identity."
    )
endif()

if(EXISTS "${source_root}/engine/editor")
    file(GLOB_RECURSE editor_ui_sources LIST_DIRECTORIES false
        "${source_root}/engine/editor/*.hpp"
        "${source_root}/engine/editor/*.h"
        "${source_root}/engine/editor/*.cpp"
        "${source_root}/engine/editor/*.template"
    )
    foreach(source IN LISTS editor_ui_sources)
        file(TO_CMAKE_PATH "${source}" normalized_source)
        if(normalized_source MATCHES "/test/" OR
           normalized_source MATCHES "/node_graph/src/DefaultImGuiNodeGraphRenderer[.]cpp$")
            continue()
        endif()
        file(READ "${source}" content)
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"](imgui[.]h|imgui_internal[.]h|imgui_node_editor[.]h)[>\"]")
            message(FATAL_ERROR
                "Architecture: Editor source '${source}' directly includes the private UI backend."
            )
        endif()
        if(normalized_source MATCHES "/inspector/" AND
           content MATCHES "RefClass|RefField|ReflectionRegistry|ImGui::|ImGui[A-Z]|imgui_node_editor")
            message(FATAL_ERROR
                "Architecture: generated Inspector contract '${source}' restored reflection or backend UI coupling."
            )
        endif()
    endforeach()
endif()

foreach(domain_adapter IN ITEMS material flowforge)
    if(EXISTS "${source_root}/engine/editor/${domain_adapter}")
        file(GLOB_RECURSE domain_adapter_sources LIST_DIRECTORIES false
            "${source_root}/engine/editor/${domain_adapter}/*.hpp"
            "${source_root}/engine/editor/${domain_adapter}/*.cpp"
            "${source_root}/engine/editor/${domain_adapter}/*.cmake"
        )
        foreach(source IN LISTS domain_adapter_sources)
            file(READ "${source}" content)
            if(content MATCHES "toolchain_|Compiler|Cooker|MaterialIR|FlowForgeIR|mlir|llvm")
                message(FATAL_ERROR
                    "Architecture: Graph domain adapter '${source}' acquired Toolchain or compiler ownership."
                )
            endif()
        endforeach()
    endif()
endforeach()

foreach(capacity_header IN ITEMS
    "${source_root}/modules/core/serialization/include/lux/engine/serialization/SerializationError.hpp"
    "${source_root}/modules/resource/asset/include/lux/engine/resource/asset/AssetSerDeser.hpp"
    "${source_root}/modules/resource/asset/include/lux/engine/resource/asset/CookedAssetImage.hpp"
    "${source_root}/modules/core/task/include/lux/engine/task/TaskExecutor.hpp"
    "${source_root}/engine/domain/simulation/ecs/core/include/lux/engine/simulation/ecs/EcsCommandBuffer.hpp"
)
    file(READ "${capacity_header}" capacity_contract)
    if(capacity_contract MATCHES
       "(max_string_bytes|max_container_elements|max_nesting|max_input_bytes|max_decoded_bytes|max_encoded_bytes|max_image_bytes|initial_bytes|max_bytes|worker_count|initial_task_capacity|max_commands|max_payload_bytes)[ \t\r\n]*\\{")
        message(FATAL_ERROR
            "Architecture: public capacity policy '${capacity_header}' contains a foundation default."
        )
    endif()
    if(capacity_contract MATCHES
       "(records_per_lane_capacity|task_capacity|capacities)[^;\n\r\)]*=")
        message(FATAL_ERROR
            "Architecture: public capacity API '${capacity_header}' contains a default argument."
        )
    endif()
endforeach()

foreach(binary_header IN ITEMS
    "${source_root}/modules/core/serialization/include/lux/engine/serialization/BinaryReader.hpp"
    "${source_root}/modules/core/serialization/include/lux/engine/serialization/BinaryWriter.hpp"
)
    file(READ "${binary_header}" binary_contract)
    if(binary_contract MATCHES "Serialization(Budget|Limits)|limits[ \t\r\n]*\\(")
        message(FATAL_ERROR
            "Architecture: exact wire primitive '${binary_header}' owns semantic capacity policy."
        )
    endif()
endforeach()

set(hierarchy_index_header
    "${source_root}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/HierarchyIndex.hpp"
)
file(READ "${hierarchy_index_header}" hierarchy_index_contract)
if(hierarchy_index_contract MATCHES
   "EcsChangeJournal|EcsCommands|ComponentChanges|EntityChanges|ChangeCursor|HierarchyChanges")
    message(FATAL_ERROR
        "Architecture: neutral HierarchyIndex owns observation or command semantics."
    )
endif()

foreach(source IN LISTS production_sources)
    file(READ "${source}" content)
    if(content MATCHES
       "#[ \t]*include[ \t]*[<\"]lux/engine/ecs/|namespace[ \t]+lux::ecs|lux::ecs::")
        message(FATAL_ERROR
            "Architecture: active source '${source}' restores the retired top-level ECS namespace."
        )
    endif()
endforeach()

foreach(source IN LISTS production_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    if(normalized MATCHES "/engine/domain/simulation/ecs/schema/")
        file(READ "${source}" content)
        if(content MATCHES
           "lux/engine/meta/|lux/cxx/reflection/")
            message(FATAL_ERROR
                "Architecture: base schema source '${normalized}' depends on reflection."
            )
        endif()
    endif()
endforeach()

if(DEFINED LUX_BINARY_DIR AND
   EXISTS "${LUX_BINARY_DIR}/compile_commands.json")
    file(READ "${LUX_BINARY_DIR}/compile_commands.json" compile_commands)
    if(compile_commands MATCHES "[/\\\\]legacy[/\\\\]")
        message(FATAL_ERROR
            "Architecture: compile_commands.json contains a legacy source/include path."
        )
    endif()
    if(compile_commands MATCHES
       "AssetStore|AssetClient|AssetLease|AssetManager|AssetRef|AssetLoadPort|AssetServices|SceneServices|ISystem|ScheduleBuilder|WorldSectionLoadBatch|WorldSectionLoader[.]hpp|connectConstruct|connectUpdate|connectDestroy|observer_relations_|[/\\\\]persistence_contract[/\\\\]|[.]ecs_persistence[.]hpp|ComponentPersistence|EcsBinaryWriter|EcsBinaryReader")
        message(FATAL_ERROR
            "Architecture: compile_commands.json contains a retired L0/L1 API."
        )
    endif()
endif()

file(GLOB_RECURSE ecs_public_headers LIST_DIRECTORIES false
    "${source_root}/engine/domain/simulation/ecs/*/include/*.hpp"
)
foreach(source IN LISTS ecs_public_headers)
    file(READ "${source}" content)
    if(content MATCHES
       "#[ \t]*include[ \t]*[<\"]lux/engine/simulation/ecs/detail/")
        message(FATAL_ERROR
            "Architecture: public ECS header '${source}' includes unsupported detail API."
        )
    endif()
endforeach()

set(transform_system_source
    "${source_root}/engine/domain/simulation/builtin/transform/src/TransformSystem.cpp"
)
if(EXISTS "${transform_system_source}")
    file(READ "${transform_system_source}" transform_system_contract)
    if(transform_system_contract MATCHES
       "unordered_(map|set)|ChangeCursor[ \t]*<[ \t]*Parent|query[ \t]*<[ \t]*Read[ \t]*<[ \t]*Parent")
        message(FATAL_ERROR
            "Architecture: Transform restores a full-scan/associative dirty-state path instead of consuming HierarchyIndex changes."
        )
    endif()
endif()

set(transform_component_header
    "${source_root}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp"
)
if(EXISTS "${transform_component_header}")
    file(READ "${transform_component_header}" transform_component_contract)
    if(transform_component_contract MATCHES
       "Eigen::(Vector2f|Vector3f|Quaternionf|Affine2f|Affine3f)|(^|[^A-Za-z0-9_])float[ \\t]+rotation")
        message(FATAL_ERROR
            "Architecture: canonical Transform2D/3D or WorldTransform2D/3D regressed to float."
        )
    endif()
endif()

set(component_schema_header
    "${source_root}/engine/domain/simulation/ecs/schema/include/lux/engine/simulation/ecs/ComponentSchema.hpp"
)
if(EXISTS "${component_schema_header}")
    file(READ "${component_schema_header}" component_schema_contract)
    if(NOT component_schema_contract MATCHES "decode_emplace")
        message(FATAL_ERROR
            "Architecture: ComponentSchema is missing the generated decode/emplace prerequisite."
        )
    endif()
endif()

set(system_registry_header
    "${source_root}/engine/domain/simulation/system/include/lux/engine/simulation/SystemRegistry.hpp"
)
if(EXISTS "${system_registry_header}")
    file(READ "${system_registry_header}" system_registry_contract)
    if(system_registry_contract MATCHES
       "SystemLease|emplaceWithLifetime|retain[ \\t\\r\\n]*<|erase[ \\t\\r\\n]*\\(|revision[ \\t\\r\\n]*\\(")
        message(FATAL_ERROR
            "Architecture: SystemRegistry restored the retired live-instance owner facade."
        )
    endif()
    if(NOT system_registry_contract MATCHES "SystemRegistration" OR
       NOT system_registry_contract MATCHES "InstallSystemFn")
        message(FATAL_ERROR
            "Architecture: SystemRegistry is not the canonical type/install-thunk catalog."
        )
    endif()
endif()

set(scene_composition_cmake "${source_root}/engine/scene/composition/CMakeLists.txt")
if(EXISTS "${scene_composition_cmake}")
    file(READ "${scene_composition_cmake}" scene_composition_target_contract)
    if(scene_composition_target_contract MATCHES
       "lux::engine::(process|render)|stdexec|scene_world_materialization")
        message(FATAL_ERROR
            "Architecture: scene/composition depends on Process, Render or world materialization."
        )
    endif()
endif()

set(scene_world_materialization_cmake "${source_root}/engine/scene/integration/world_materialization/CMakeLists.txt")
if(EXISTS "${scene_world_materialization_cmake}")
    file(READ "${scene_world_materialization_cmake}" scene_world_materialization_contract)
    if(scene_world_materialization_contract MATCHES
       "spatial3d|spatial2d|lux::engine::(render|editor|authoring|toolchain)")
        message(FATAL_ERROR
            "Architecture: scene/world_materialization acquired concrete policy or an upper-layer dependency."
        )
    endif()
endif()

set(latest_exchange_header
    "${source_root}/engine/scene/presentation/include/lux/engine/scene/LatestSpscExchange.hpp"
)
if(EXISTS "${latest_exchange_header}")
    file(READ "${latest_exchange_header}" latest_exchange_contract)
    if(latest_exchange_contract MATCHES "mutex|condition_variable|wait[ \\t\\r\\n]*\\(")
        message(FATAL_ERROR
            "Architecture: LatestSpscExchange is no longer a non-blocking SPSC primitive."
        )
    endif()
endif()

file(GLOB_RECURSE installed_public_headers LIST_DIRECTORIES false
    "${source_root}/modules/*.h"
    "${source_root}/modules/*.hpp"
    "${source_root}/engine/*.h"
    "${source_root}/engine/*.hpp"
)
foreach(source IN LISTS installed_public_headers)
    file(TO_CMAKE_PATH "${source}" normalized)
    if(NOT normalized MATCHES "/include/")
        continue()
    endif()
    file(READ "${source}" content)

    if(content MATCHES "AssetCodecDescriptor|AssetCodecSet|DecodedAsset|AssetDecodeContext|AssetEncodeContext")
        message(FATAL_ERROR
            "Architecture: '${normalized}' restores the retired erased Asset codec model."
        )
    endif()
    if(content MATCHES "#[ \t]*include[ \t]*[<\"]Jolt/|JPH::")
        message(FATAL_ERROR
            "Architecture: installed public header '${source}' exposes private Jolt ABI."
        )
    endif()
    if(normalized MATCHES "/engine/process/" AND content MATCHES "BlockingScheduler" AND
       NOT normalized MATCHES "/engine/process/(execution|asset_loading)/")
        message(FATAL_ERROR
            "Architecture: BlockingScheduler is authorized only for execution and the V2 AssetRead endpoint."
        )
    endif()
endforeach()

if(EXISTS "${source_root}/engine/domain/world/identity/include/lux/engine/domain/WorldObjectId.hpp")
    message(FATAL_ERROR "Architecture: retired domain-leaking WorldObjectId header remains installed in source.")
endif()
if(EXISTS "${source_root}/engine/scene/integration/world_materialization/include/lux/engine/scene/WorldRuntime.hpp")
    message(FATAL_ERROR "Architecture: retired Scene WorldRuntime aggregate header remains in source.")
endif()

set(scene_script_runtime_cmake "${source_root}/engine/scene/integration/script/CMakeLists.txt")
if(NOT EXISTS "${scene_script_runtime_cmake}")
    message(FATAL_ERROR "Missing canonical Scene script runtime integration leaf")
endif()
file(READ "${scene_script_runtime_cmake}" scene_script_runtime_contract)
if(NOT scene_script_runtime_contract MATCHES "lux::engine::process::process_execution" OR
   NOT scene_script_runtime_contract MATCHES "lux::engine::simulation::simulation_script")
    message(FATAL_ERROR "Scene script runtime must explicitly bridge Process execution and Simulation Script")
endif()
file(READ "${source_root}/engine/domain/simulation/builtin/script/CMakeLists.txt" simulation_script_contract)
if(simulation_script_contract MATCHES "lux::engine::process")
    message(FATAL_ERROR "L1 Simulation Script must not link Process execution for Delay")
endif()
file(GLOB_RECURSE scene_script_runtime_sources
    "${source_root}/engine/scene/integration/script/include/*.hpp"
    "${source_root}/engine/scene/integration/script/src/*.cpp")
foreach(scene_script_runtime_source IN LISTS scene_script_runtime_sources)
    file(READ "${scene_script_runtime_source}" scene_script_runtime_content)
    if(scene_script_runtime_content MATCHES "(ScriptTimeManager|AsyncManager|SceneServices|ServiceRegistry|shared_ptr[<][^>]*ScriptSystem)")
        message(FATAL_ERROR "Scene script runtime reintroduced a forbidden manager/service/ScriptSystem owner")
    endif()
endforeach()

foreach(typed_asset_header IN ITEMS
    "${source_root}/modules/resource/asset/include/lux/engine/resource/asset/Asset.hpp"
    "${source_root}/modules/resource/asset/include/lux/engine/resource/asset/AssetSerDeser.hpp"
    "${source_root}/modules/resource/asset/include/lux/engine/resource/asset/CookedAssetImage.hpp"
)
    if(NOT EXISTS "${typed_asset_header}")
        message(FATAL_ERROR "Architecture: missing typed Asset contract '${typed_asset_header}'.")
    endif()
    file(READ "${typed_asset_header}" typed_asset_contract)
    if(typed_asset_contract MATCHES "TypeToken|DecodedAsset|EAssetType|shared_ptr<const[ \t]+void>|void[ \t]*[*]")
        message(FATAL_ERROR
            "Architecture: typed Asset contract '${typed_asset_header}' restored erased or closed-type vocabulary."
        )
    endif()
endforeach()

file(GLOB_RECURSE typed_asset_production_sources LIST_DIRECTORIES false
    "${source_root}/modules/resource/asset/*.hpp"
    "${source_root}/modules/resource/asset/*.cpp"
    "${source_root}/modules/function/script/artifact/*.hpp"
    "${source_root}/modules/function/script/artifact/*.cpp"
    "${source_root}/engine/domain/world/asset/*.hpp"
    "${source_root}/engine/domain/world/asset/*.cpp"
    "${source_root}/engine/domain/simulation/asset/*.hpp"
    "${source_root}/engine/domain/simulation/asset/*.cpp"
    "${source_root}/engine/scene/asset/*.hpp"
    "${source_root}/engine/scene/asset/*.cpp"
)
string(CONCAT typed_asset_forbidden_types
    "AssetRuntimeContext|AssetServices|AssetManager2|AssetRegistry2|AssetBridge|AssetRuntimeBridge|"
    "AssetDemandRegistry|AssetResidencyManager|TextureManager|TextureCodecRegistry|"
    "TextureCookProfileRegistry|SerializationContext"
)
foreach(source IN LISTS typed_asset_production_sources)
    file(READ "${source}" content)
    if(content MATCHES "TAssetSerDeser" AND content MATCHES
       "#[ \t]*include[ \t]*[<\"][^\">]*(filesystem|AssetProvider|OperationPort|AssetManager)")
        message(FATAL_ERROR
            "Architecture: typed SerDeser '${source}' owns IO, Provider, Port, or Manager concerns."
        )
    endif()
    if(content MATCHES "${typed_asset_forbidden_types}")
        message(FATAL_ERROR
            "Architecture: typed Asset production source '${source}' introduced forbidden glue vocabulary."
        )
    endif()
    if(content MATCHES "ModelAssetData|MaterialAssetData|MaterialInstanceAssetData")
        message(FATAL_ERROR
            "Architecture: Asset production source '${source}' owns Resource domain data."
        )
    endif()
endforeach()

file(GLOB_RECURSE resource_description_sources LIST_DIRECTORIES false
    "${source_root}/modules/resource/description/*.hpp"
    "${source_root}/modules/resource/description/*.cpp"
)
foreach(source IN LISTS resource_description_sources)
    file(READ "${source}" content)
    if(content MATCHES "#[ \t]*include[ \t]*[<\"]lux/engine/resource/asset/")
        message(FATAL_ERROR
            "Architecture: Resource Description source '${source}' depends on the Asset package."
        )
    endif()
    if(content MATCHES "OpaqueAssetId")
        message(FATAL_ERROR
            "Architecture: Resource Description source '${source}' restored opaque Asset identity."
        )
    endif()
endforeach()

if(EXISTS "${source_root}/modules/resource/asset/include/lux/engine/resource/asset/AssetId.hpp")
    message(FATAL_ERROR "Architecture: retired Asset-layer AssetId header was restored.")
endif()
if(EXISTS "${source_root}/modules/resource/description/include/lux/engine/description/ImportedMaterialDesc.hpp")
    message(FATAL_ERROR "Architecture: Toolchain import intermediate was restored to Resource Description.")
endif()

set(active_asset_packer_source
    "${source_root}/engine/toolchain/asset/packer/src/AssetPacker.cpp"
)
if(EXISTS "${active_asset_packer_source}")
    file(READ "${active_asset_packer_source}" active_asset_packer_contract)
    if(active_asset_packer_contract MATCHES "kMetadataOffset|probeImage")
        message(FATAL_ERROR
            "Architecture: active Asset packer knows the cooked-envelope physical metadata offset."
        )
    endif()
endif()

file(GLOB_RECURSE active_asset_toolchain_sources LIST_DIRECTORIES false
    "${source_root}/engine/toolchain/asset/*.hpp"
    "${source_root}/engine/toolchain/asset/*.cpp"
    "${source_root}/engine/toolchain/material/*.hpp"
    "${source_root}/engine/toolchain/material/*.cpp"
)
string(CONCAT asset_toolchain_forbidden_types
    "AssetManager|AssetManager2|AssetCookContext|AssetCookRegistry|AssetProductRegistry|"
    "AssetBatchManager|ResourceGraphManager|AssetServices|AssetRuntimeContext|AssetRegistry2"
)
foreach(source IN LISTS active_asset_toolchain_sources)
    file(READ "${source}" content)
    if(content MATCHES "${asset_toolchain_forbidden_types}")
        message(FATAL_ERROR
            "Architecture: active Asset Toolchain source '${source}' restored Manager/Context/Registry orchestration."
        )
    endif()
endforeach()

set(imported_material_header
    "${source_root}/engine/toolchain/material/include/lux/engine/material/ImportedMaterialDescription.hpp"
)
if(EXISTS "${imported_material_header}")
    file(READ "${imported_material_header}" imported_material_contract)
    if(imported_material_contract MATCHES "texture_index|OpaqueAssetId" OR
       NOT imported_material_contract MATCHES "AssetId[ \t]+texture")
        message(FATAL_ERROR
            "Architecture: imported Material Toolchain contract lost direct Texture Asset identity."
        )
    endif()
endif()

file(GLOB_RECURSE material_graph_sources LIST_DIRECTORIES false
    "${source_root}/modules/function/material/*.hpp"
    "${source_root}/modules/function/material/*.cpp"
)
foreach(source IN LISTS material_graph_sources)
    file(READ "${source}" content)
    if(content MATCHES "namespace[ \t]+lux::rdesc|lux/engine/(toolchain|editor)/|shaderc|spirv_cross|Vulkan")
        message(FATAL_ERROR
            "Architecture: MaterialGraph source '${source}' acquired compiler, Editor, or backend ownership."
        )
    endif()
endforeach()

set(material_compiler_header
    "${source_root}/engine/toolchain/material/include/lux/engine/material/Compiler.hpp"
)
if(EXISTS "${material_compiler_header}")
    file(READ "${material_compiler_header}" material_compiler_public_contract)
    if(material_compiler_public_contract MATCHES
       "shaderc|spirv_cross|Vulkan|MLIR|LLVM|resource/asset/|MaterialIR|ShaderIR|Lowering|Backend")
        message(FATAL_ERROR "Architecture: Material compiler public header leaks an implementation or Asset API.")
    endif()
endif()

set(material_shader_ir
    "${source_root}/engine/toolchain/material/pinclude/lux/engine/material/compiler/ShaderIR.hpp"
)
if(EXISTS "${material_shader_ir}")
    file(READ "${material_shader_ir}" material_shader_ir_contract)
    if(material_shader_ir_contract MATCHES "MaterialGraphContract|rdesc::EMatValueType")
        message(FATAL_ERROR "Architecture: private ShaderIR depends on the retired Material graph contract.")
    endif()
endif()

set(material_toolchain_cmake "${source_root}/engine/toolchain/material/CMakeLists.txt")
set(material_shader_compiler "${source_root}/engine/toolchain/material/src/ShaderCompiler.cpp")
set(asset_toolchain_cmake "${source_root}/engine/toolchain/asset/CMakeLists.txt")
if(EXISTS "${material_toolchain_cmake}" AND EXISTS "${material_shader_compiler}")
    file(READ "${material_toolchain_cmake}" material_toolchain_cmake_contract)
    file(READ "${material_shader_compiler}" material_shader_compiler_contract)
    if(material_toolchain_cmake_contract MATCHES "asset/shader/pinclude" OR
       material_toolchain_cmake_contract MATCHES "LUX_MATERIAL_SHADER_(SOURCE|EMITTED)_DIR" OR
       material_shader_compiler_contract MATCHES "FileIncluder|#[ \t]*include[ \t]*<fstream>")
        message(FATAL_ERROR
            "Architecture: Material compiler restored a sibling pinclude or runtime filesystem Shader dependency."
        )
    endif()
endif()
if(EXISTS "${asset_toolchain_cmake}")
    file(READ "${asset_toolchain_cmake}" asset_toolchain_cmake_contract)
    if(asset_toolchain_cmake_contract MATCHES "add_subdirectory[^\n]*[.][.]/material")
        message(FATAL_ERROR "Architecture: Asset collection reverse-aggregates the Material sibling.")
    endif()
endif()

set(unfinished_work_index "${source_root}/.internal/UNFINISHED-WORK.md")
if(NOT EXISTS "${unfinished_work_index}")
    message(FATAL_ERROR "Architecture: AGENTS.md promises a missing .internal/UNFINISHED-WORK.md index.")
endif()
set(retired_script_freeze
    "${source_root}/doc/lux-script-flowforge-concept-compression-implementation.zh-CN.md"
)
if(EXISTS "${retired_script_freeze}")
    file(READ "${retired_script_freeze}" retired_script_freeze_contract LIMIT 2048)
    if(NOT retired_script_freeze_contract MATCHES "SUPERSEDED / HISTORICAL" OR
       NOT retired_script_freeze_contract MATCHES "directory-target-product-architecture[.]md")
        message(FATAL_ERROR "Architecture: stale Script/FlowForge freeze still presents itself as normative.")
    endif()
endif()

file(GLOB_RECURSE runtime_asset_boundary_sources LIST_DIRECTORIES false
    "${source_root}/modules/resource/asset/*.hpp"
    "${source_root}/modules/resource/asset/*.cpp"
    "${source_root}/modules/function/render/*.hpp"
    "${source_root}/modules/function/render/*.cpp"
    "${source_root}/engine/domain/*.hpp"
    "${source_root}/engine/domain/*.cpp"
    "${source_root}/engine/process/*.hpp"
    "${source_root}/engine/process/*.cpp"
    "${source_root}/engine/scene/*.hpp"
    "${source_root}/engine/scene/*.cpp"
)
foreach(source IN LISTS runtime_asset_boundary_sources)
    file(READ "${source}" content)
    if(content MATCHES "#[ \t]*include[ \t]*[<\"](stb_image|spirv_cross|bc7enc|rgbcx)")
        message(FATAL_ERROR
            "Architecture: Runtime source '${source}' includes a Toolchain texture/shader dependency."
        )
    endif()
    if(content MATCHES
       "#[ \t]*include[ \t]*[<\"]lux/engine/material/(graph|Compiler|Cooker|ImportedMaterialDescription)")
        message(FATAL_ERROR
            "Architecture: Runtime source '${source}' depends on the L4 Material source/compiler/cooker."
        )
    endif()
endforeach()

set(world_loading_header
    "${source_root}/engine/process/world_loading/include/lux/engine/process/world_loading/WorldPartitionLoadSender.hpp"
)
set(world_loading_source
    "${source_root}/engine/process/world_loading/src/WorldPartitionLoad.cpp"
)
set(world_partition_data_header
    "${source_root}/engine/domain/world/storage/include/lux/engine/world/WorldPartitionData.hpp"
)
if(EXISTS "${world_loading_header}" AND EXISTS "${world_loading_source}")
    file(READ "${world_loading_header}" world_loading_header_contract)
    file(READ "${world_loading_source}" world_loading_source_contract)
    if(NOT world_loading_header_contract MATCHES "std::size_t[ \\t]+max_bytes" OR
       world_loading_source_contract MATCHES "512U[ \\t]*[*][ \\t]*1024U")
        message(FATAL_ERROR
            "Architecture: World partition load lost its Product-supplied byte budget."
        )
    endif()
    if(NOT world_loading_source_contract MATCHES "accounted_bytes[ \\t]*=")
        message(FATAL_ERROR
            "Architecture: World range IO does not account submitted bytes."
        )
    endif()
endif()
if(EXISTS "${world_partition_data_header}")
    file(READ "${world_partition_data_header}" world_partition_data_contract)
    if(NOT world_partition_data_contract MATCHES "WorldBundleId[ \\t]+bundle" OR
       NOT world_partition_data_contract MATCHES "WorldBundleGeneration[ \\t]+generation")
        message(FATAL_ERROR
            "Architecture: WorldPartitionData is not bound to bundle generation identity."
        )
    endif()
endif()

foreach(component_header IN ITEMS
    "${source_root}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp"
    "${source_root}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp"
)
    if(EXISTS "${component_header}")
        file(READ "${component_header}" component_contract)
        if(component_contract MATCHES
           "ComponentReflectionAdapter|RefClass|RefField|ComponentCodec")
            message(FATAL_ERROR
                "Architecture: canonical ECS component header '${component_header}' acquired runtime-reflection or codec persistence."
            )
        endif()
    endif()
endforeach()

file(GLOB_RECURSE active_cmake LIST_DIRECTORIES false
    "${source_root}/CMakeLists.txt"
    "${source_root}/modules/CMakeLists.txt"
    "${source_root}/modules/*/CMakeLists.txt"
    "${source_root}/modules/*/*/CMakeLists.txt"
    "${source_root}/modules/*/*/*/CMakeLists.txt"
    "${source_root}/engine/CMakeLists.txt"
    "${source_root}/engine/domain/world/CMakeLists.txt"
    "${source_root}/engine/domain/world/*/CMakeLists.txt"
    "${source_root}/engine/domain/partition/CMakeLists.txt"
    "${source_root}/engine/domain/partition/*/CMakeLists.txt"
    "${source_root}/engine/domain/system/CMakeLists.txt"
    "${source_root}/engine/domain/system/*/CMakeLists.txt"
    "${source_root}/engine/domain/spatial/CMakeLists.txt"
    "${source_root}/engine/domain/spatial/*/CMakeLists.txt"
    "${source_root}/engine/domain/simulation/CMakeLists.txt"
    "${source_root}/engine/domain/simulation/*/CMakeLists.txt"
    "${source_root}/engine/domain/simulation/*/*/CMakeLists.txt"
    "${source_root}/engine/process/CMakeLists.txt"
    "${source_root}/engine/process/*/CMakeLists.txt"
    "${source_root}/engine/scene/CMakeLists.txt"
    "${source_root}/engine/scene/*/CMakeLists.txt"
    "${source_root}/engine/scene/*/*/CMakeLists.txt"
    "${source_root}/engine/editor/CMakeLists.txt"
    "${source_root}/engine/editor/*/CMakeLists.txt"
    "${source_root}/engine/toolchain/CMakeLists.txt"
    "${source_root}/engine/toolchain/*/CMakeLists.txt"
)
list(FILTER active_cmake EXCLUDE REGEX "[/\\\\]legacy[/\\\\]")
foreach(source IN LISTS active_cmake)
    file(READ "${source}" content)
    if(content MATCHES
       "(^|[^A-Za-z0-9_])(authoring_material|authoring_script|partition_core|world_core|simulation_core|simulation_runtime|scene_core|scene_runtime_(presentation|world|render|render_meta)|process_asset|process_world)([^A-Za-z0-9_]|$)" OR
       content MATCHES
       "lux-engine-(authoring|scene-world-runtime|simulation-core|process-(asset|world)([^A-Za-z0-9_-]|$)|world([^A-Za-z0-9_-]|$))" OR
       content MATCHES "engine/toolchain/asset/material|lux/engine/toolchain/asset/material")
        message(FATAL_ERROR
            "Architecture: active CMake '${source}' references a retired post-cleanup target or package."
        )
    endif()
    if(content MATCHES
       "add_subdirectory[ \t\r\n]*\\([^\\)]*legacy|target_link_libraries[ \t\r\n]*\\([^\\)]*legacy")
        message(FATAL_ERROR
            "Architecture: active CMake '${source}' configures or links legacy/."
        )
    endif()
    if(content MATCHES
       "schema_reflection|(^|[^A-Za-z_])add_meta[ \t\r\n]*\\(|(^|[^A-Za-z_])target_add_meta[ \t\r\n]*\\(")
        message(FATAL_ERROR
            "Architecture: active CMake '${source}' restores retired reflection/persistence integration."
        )
    endif()
    if(source MATCHES "/modules/resource/asset/CMakeLists.txt")
        if(content MATCHES "async_port|AssetManager|AssetLoadPort")
            message(FATAL_ERROR
                "Architecture: active L0 asset target restores runtime ownership/orchestration."
            )
        endif()
    endif()
    if(source MATCHES "/engine/process/execution/")
        if(content MATCHES
           "legacy|lux::engine::(world|simulation|scene|render|authoring|editor|toolchain|host)|asio|TBB|static_thread_pool")
            message(FATAL_ERROR
                "Architecture: Process target '${source}' links an upper/domain/legacy execution dependency."
            )
        endif()
    elseif(source MATCHES "/engine/process/world_loading/")
        if(content MATCHES
           "legacy|lux::engine::(simulation|scene|render|authoring|editor|toolchain|host)|asio|TBB|static_thread_pool")
            message(FATAL_ERROR
                "Architecture: Process World target '${source}' links an upper/domain/legacy dependency."
            )
        endif()
    elseif(source MATCHES "/engine/process/asset_loading/")
        if(content MATCHES
           "legacy|lux::engine::(world|simulation|scene|render|authoring|editor|toolchain|host)|asio|TBB|static_thread_pool")
            message(FATAL_ERROR
                "Architecture: Process Asset target '${source}' links a domain/upper/legacy dependency."
            )
        endif()
    endif()
endforeach()

set(process_timer_header
    "${source_root}/engine/process/execution/include/lux/engine/process/Timer.hpp"
)
set(process_runtime_header
    "${source_root}/engine/process/execution/include/lux/engine/process/ExecutionRuntime.hpp"
)
set(process_task_scope_header
    "${source_root}/engine/process/execution/include/lux/engine/process/TaskScope.hpp"
)
set(process_port_sender_header
    "${source_root}/engine/process/execution/include/lux/engine/process/PortSender.hpp"
)
set(process_installed_consumer
    "${source_root}/cmake/installed-consumers/process-execution/CMakeLists.txt"
)
foreach(required_process_file IN ITEMS
    "${process_timer_header}"
    "${process_port_sender_header}"
    "${process_runtime_header}"
    "${process_task_scope_header}"
    "${process_installed_consumer}"
)
    if(NOT EXISTS "${required_process_file}")
        message(FATAL_ERROR
            "Architecture: Process Wave 0 is missing '${required_process_file}'."
        )
    endif()
endforeach()

if(NOT EXISTS "${source_root}/cmake/installed-consumers/script-ability-codegen/CMakeLists.txt")
    message(FATAL_ERROR
        "Architecture: missing installed Script Ability codegen consumer."
    )
endif()
if(NOT EXISTS "${source_root}/cmake/installed-consumers/system-event-await-runtime/CMakeLists.txt")
    message(FATAL_ERROR
        "Architecture: missing installed Script Event await runtime consumer."
    )
endif()
if(NOT EXISTS "${source_root}/cmake/installed-consumers/lua-script-packager/CMakeLists.txt")
    message(FATAL_ERROR
        "Architecture: missing installed Lua Script Ability packager consumer."
    )
endif()

set(dedicated_scene_consumer
    "${source_root}/cmake/installed-consumers/dedicated-scene/CMakeLists.txt"
)
if(NOT EXISTS "${dedicated_scene_consumer}")
    message(FATAL_ERROR "Architecture: missing dedicated headless Scene installed consumer.")
endif()
file(READ "${dedicated_scene_consumer}" dedicated_scene_consumer_contract)
if(dedicated_scene_consumer_contract MATCHES
   "render|vulkan|window|scene_render")
    message(FATAL_ERROR
        "Architecture: dedicated Scene installed consumer is not headless."
    )
endif()

set(render_entity_header
    "${source_root}/modules/function/render/client/include/lux/engine/function/render/client/core/RenderEntityId.hpp"
)
set(mesh_scene_protocol_header
    "${source_root}/modules/function/render/client/include/lux/engine/function/render/client/features/meshstack/MeshStackOperation.hpp"
)
set(light_scene_protocol_header
    "${source_root}/modules/function/render/client/include/lux/engine/function/render/client/features/light/LightOperation.hpp"
)
foreach(required_render_contract IN ITEMS
    "${render_entity_header}"
    "${mesh_scene_protocol_header}"
    "${light_scene_protocol_header}"
    "${source_root}/engine/scene/presentation/include/lux/engine/scene/LatestSpscExchange.hpp"
    "${source_root}/engine/scene/integration/render/include/lux/engine/scene/RenderSyncPipeline.hpp"
    "${source_root}/engine/scene/integration/render/include/lux/engine/scene/RenderRuntime.hpp"
    "${source_root}/engine/scene/integration/render/include/lux/engine/scene/RenderSystem.hpp"
    "${source_root}/engine/scene/integration/render/include/lux/engine/scene/RenderSystemConfiguration.hpp"
    "${source_root}/engine/scene/integration/render/include/lux/engine/scene/Builtin3DRenderIntegration.hpp"
)
    if(NOT EXISTS "${required_render_contract}")
        message(FATAL_ERROR "Architecture: missing retained Render lane contract '${required_render_contract}'.")
    endif()
endforeach()
if(EXISTS "${source_root}/engine/scene/integration/render/include/lux/engine/scene/Builtin3DRenderStages.hpp")
    message(FATAL_ERROR "Architecture: retired public builtin Render stage factories remain installed in source.")
endif()
file(READ "${render_entity_header}" render_entity_contract)
file(READ "${mesh_scene_protocol_header}" mesh_scene_protocol_contract)
file(READ "${light_scene_protocol_header}" light_scene_protocol_contract)
if(render_entity_contract MATCHES "entt|registry")
    message(FATAL_ERROR "Architecture: public RenderEntityId depends on an ECS implementation.")
endif()
if(mesh_scene_protocol_contract MATCHES
   "AddMeshInstancePayload|MeshInstanceSlotReply|RenderObjectHandle[ \t]+object")
    message(FATAL_ERROR "Architecture: Mesh scene protocol exposes retired RenderObjectHandle identity.")
endif()
if(light_scene_protocol_contract MATCHES
   "CreateLightPayload|UpdateLightPayload|DestroyLightPayload|LightCreatedReply|RLightHandle")
    message(FATAL_ERROR "Architecture: Light scene protocol exposes retired internal light identity.")
endif()
file(GLOB_RECURSE render_client_sources LIST_DIRECTORIES false
    "${source_root}/modules/function/render/client/include/*.hpp"
    "${source_root}/modules/function/render/client/src/*.cpp"
)
foreach(render_client_source IN LISTS render_client_sources)
    file(READ "${render_client_source}" render_client_content)
    if(render_client_content MATCHES "resource/identity/AssetId[.]hpp|asset::AssetId")
        message(FATAL_ERROR
            "Architecture: pure Render client source '${render_client_source}' depends on AssetId."
        )
    endif()
endforeach()

file(GLOB_RECURSE render_feature_operation_headers LIST_DIRECTORIES false
    "${source_root}/modules/function/render/client/include/lux/engine/function/render/client/features/*Operation.hpp"
)
foreach(render_feature_operation_header IN LISTS render_feature_operation_headers)
    file(READ "${render_feature_operation_header}" render_feature_operation_content)
    if(render_feature_operation_content MATCHES
       "ShaderHandle[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*[{]")
        message(FATAL_ERROR
            "Architecture: durable RenderFeature config in '${render_feature_operation_header}' exposes a runtime ShaderHandle."
        )
    endif()
endforeach()
file(READ
    "${source_root}/modules/function/render/cmake/template/comm_ops_cpp.template"
    render_server_codegen_template
)
if(render_server_codegen_template MATCHES "static constexpr FeatureDescriptor")
    message(FATAL_ERROR "Architecture: server Render codegen owns a duplicate FeatureDescriptor.")
endif()

set(render_sync_pipeline_header
    "${source_root}/engine/scene/integration/render/include/lux/engine/scene/RenderSyncPipeline.hpp"
)
set(render_sync_pipeline_source
    "${source_root}/engine/scene/integration/render/src/RenderSyncPipeline.cpp"
)
file(READ "${render_sync_pipeline_header}" render_sync_pipeline_contract)
file(READ "${render_sync_pipeline_source}" render_sync_pipeline_implementation)
if(render_sync_pipeline_contract MATCHES "Mesh3D|Light3D|MeshStackOperationIds|LightOperationIds|Registry|Config")
    message(FATAL_ERROR "Architecture: RenderSyncPipeline public API owns a concrete Mesh/Light extraction domain.")
endif()
if(render_sync_pipeline_implementation MATCHES
   "Mesh3D|Light3D|DirtySlot|dirty_bits|summary|expected_entity_capacity|EntityCapacity")
    message(FATAL_ERROR "Architecture: RenderSyncPipeline restores concrete extraction or central dirty storage.")
endif()

set(render_builtin_stage_source
    "${source_root}/engine/scene/integration/render/src/Builtin3DRenderStages.cpp"
)
file(READ "${render_builtin_stage_source}" render_builtin_stage_contract)
if(render_builtin_stage_contract MATCHES "changes_[.]markAll[ \t\r\n]*\\(")
    message(FATAL_ERROR "Architecture: Render stage requestFullSync restores allocating reactive markAll.")
endif()
if(render_builtin_stage_contract MATCHES "none_of.*departures_")
    message(FATAL_ERROR "Architecture: Render departure callback restores quadratic inline deduplication.")
endif()
foreach(render_builtin_commit_domain IN ITEMS MESH LIGHT)
    set(render_builtin_commit_begin_marker "LUX_RENDER_COMMIT_${render_builtin_commit_domain}_BEGIN")
    set(render_builtin_commit_end_marker "LUX_RENDER_COMMIT_${render_builtin_commit_domain}_END")
    string(FIND "${render_builtin_stage_contract}" "${render_builtin_commit_begin_marker}" render_builtin_commit_begin)
    string(FIND "${render_builtin_stage_contract}" "${render_builtin_commit_end_marker}" render_builtin_commit_end)
    if(render_builtin_commit_begin LESS 0 OR render_builtin_commit_end LESS 0 OR
       render_builtin_commit_end LESS render_builtin_commit_begin)
        message(FATAL_ERROR "Architecture: builtin Render stage commit allocation markers are missing or invalid.")
    endif()
    string(LENGTH "${render_builtin_commit_begin_marker}" render_builtin_commit_marker_length)
    math(EXPR render_builtin_commit_content_begin
        "${render_builtin_commit_begin} + ${render_builtin_commit_marker_length}"
    )
    math(EXPR render_builtin_commit_content_length
        "${render_builtin_commit_end} - ${render_builtin_commit_content_begin}"
    )
    string(SUBSTRING "${render_builtin_stage_contract}"
        ${render_builtin_commit_content_begin}
        ${render_builtin_commit_content_length}
        render_builtin_commit_block
    )
    if(render_builtin_commit_block MATCHES
       "emplace|reserve[ \t\r\n]*\\(|push_back[ \t\r\n]*\\(|insert[ \t\r\n]*\\(|make_(unique|shared)|new[ \t\r\n]+")
        message(FATAL_ERROR "Architecture: builtin Render stage commit path can allocate.")
    endif()
endforeach()

set(render_runtime_header
    "${source_root}/engine/scene/integration/render/include/lux/engine/scene/RenderRuntime.hpp"
)
set(render_scene_system_header
    "${source_root}/engine/scene/integration/render/include/lux/engine/scene/RenderSystem.hpp"
)
set(render_scene_system_source
    "${source_root}/engine/scene/integration/render/src/RenderSystem.cpp"
)
file(READ "${render_runtime_header}" render_runtime_contract)
file(READ "${render_scene_system_header}" render_scene_system_contract)
file(READ "${render_scene_system_source}" render_scene_system_implementation)
if(render_runtime_contract MATCHES "vulkan|GeneralRenderServer|LuxWindow|std::thread")
    message(FATAL_ERROR "Architecture: L3 RenderRuntime exposes a concrete backend/thread/window.")
endif()
if(render_scene_system_contract MATCHES "FeatureBindings|Mesh3D|Light3D|GeneralRenderServer|std::thread")
    message(FATAL_ERROR "Architecture: RenderSystem public API retains feature mirrors or concrete extraction/backend state.")
endif()
if(render_scene_system_implementation MATCHES "Mesh3D|Light3D|GeneralRenderServer|std::thread")
    message(FATAL_ERROR "Architecture: RenderSystem installer hard-codes feature extraction or backend ownership.")
endif()

foreach(render_resource_file IN ITEMS
    "${source_root}/modules/function/render/vulkan/sinclude/lux/engine/render/resources/mesh/MeshResources.hpp"
    "${source_root}/modules/function/render/vulkan/sinclude/lux/engine/render/resources/material/MaterialResources.hpp"
)
    file(READ "${render_resource_file}" render_resource_contract)
    if(render_resource_contract MATCHES "asset_handles_|handle_assets_|findAsset|bindAsset|asset::AssetId")
        message(FATAL_ERROR
            "Architecture: Render resource owner '${render_resource_file}' restores an AssetId map."
        )
    endif()
endforeach()

file(READ "${process_timer_header}" process_timer_contract)
file(READ "${process_port_sender_header}" process_port_sender_contract)
if(process_timer_contract MATCHES "sender_tag|operation_state_tag|capacity[ \\t]*\\{[1-9]|create[^;]*=")
    message(FATAL_ERROR
        "Architecture: Timer restores retired stdexec vocabulary or a hidden capacity default."
    )
endif()
if(process_timer_contract MATCHES "shared_ptr[ \\t]*<[^>]*TimerRequest|unique_ptr[ \\t]*<[^>]*TimerRequest")
    message(FATAL_ERROR
        "Architecture: Timer request admission owns per-request heap state."
    )
endif()
if(process_port_sender_contract MATCHES "sender_tag|operation_state_tag|shared_ptr|unique_ptr|std::function")
    message(FATAL_ERROR
        "Architecture: OperationPort sender restores allocation or retired stdexec vocabulary."
    )
endif()

set(serialization_cmake
    "${source_root}/modules/core/serialization/CMakeLists.txt"
)
if(EXISTS "${serialization_cmake}")
    file(READ "${serialization_cmake}" serialization_target_contract)
    if(serialization_target_contract MATCHES
       "lux::engine::core::meta|component_add_internal_dependencies[ \t\r\n]*\\([ \t\r\n]*serialization[ \t\r\n]+meta")
        message(FATAL_ERROR
            "Architecture: exact binary serialization acquired runtime reflection."
        )
    endif()
endif()

# Preserve the most important L0 boundaries while the old upper layers are out
# of the graph.
file(GLOB_RECURSE meta_sources LIST_DIRECTORIES false
    "${source_root}/modules/core/meta/*.hpp"
    "${source_root}/modules/core/meta/*.cpp"
)
foreach(source IN LISTS meta_sources)
    file(READ "${source}" content)
    if(content MATCHES "lux/engine/object/")
        message(FATAL_ERROR
            "Architecture: core/meta '${source}' depends on core/object."
        )
    endif()
    if(content MATCHES "lux/engine/function/script/")
        message(FATAL_ERROR
            "Architecture: generic core/meta '${source}' depends on Script semantics."
        )
    endif()
endforeach()

file(GLOB_RECURSE script_foundation_sources LIST_DIRECTORIES false
    "${source_root}/modules/function/script/*/include/*.hpp"
    "${source_root}/modules/function/script/*/sinclude/*.hpp"
    "${source_root}/modules/function/script/*/pinclude/*.hpp"
    "${source_root}/modules/function/script/*/src/*.cpp"
)
foreach(source IN LISTS script_foundation_sources)
    file(READ "${source}" content)
    if(content MATCHES
       "lux/engine/meta/|lux/cxx/reflection/|lux/engine/core/meta/")
        message(FATAL_ERROR
            "Architecture: L0 Script foundation '${source}' depends on generic Meta."
        )
    endif()
endforeach()

set(script_ability_contract
    "${source_root}/modules/function/script/core/include/lux/engine/function/script/ScriptAbility.hpp"
)
set(script_ability_async_contract
    "${source_root}/modules/function/script/core/include/lux/engine/function/script/ScriptAbilityAsync.hpp"
)
set(script_ability_codegen
    "${source_root}/modules/function/script/core/template/script_ability.template"
)
set(script_ability_codegen_cmake
    "${source_root}/modules/function/script/core/cmake/engine_script_ability_codegen.cmake"
)
file(READ "${script_ability_contract}" script_ability_public_contract)
file(READ "${script_ability_async_contract}" script_ability_async_public_contract)
file(READ "${script_ability_codegen}" script_ability_generated_contract)
file(READ "${script_ability_codegen_cmake}" script_ability_codegen_contract)
if(script_ability_public_contract MATCHES
   "Scene|Simulation|SystemInstanceId|Physics|AssetLoading|std::(function|any|type_index)|ScriptApiManager|AbilityManager|ServiceRegistry" OR
   script_ability_async_public_contract MATCHES
   "Scene|Simulation|SystemInstanceId|Physics|AssetLoading|std::(function|any|type_index)|ScriptApiManager|AbilityManager|ServiceRegistry")
    message(FATAL_ERROR
        "Architecture: generic Script Ability contract acquired Engine ontology or service lookup."
    )
endif()
if(script_ability_generated_contract MATCHES
   "new[ \t]+Provider|make_shared<Provider>|shared_ptr<Provider>|dynamic_cast|GetGlobal|ServiceRegistry")
    message(FATAL_ERROR
        "Architecture: generated Script Ability binding constructs, owns or discovers providers."
    )
endif()
if(script_ability_codegen_contract MATCHES "file.*GLOB" OR
   NOT script_ability_codegen_contract MATCHES "TARGET_FILES[ \t]+[$][{]ARGS_SOURCES[}]")
    message(FATAL_ERROR
        "Architecture: Script Ability codegen is not an explicit source opt-in."
    )
endif()
if(EXISTS "${source_root}/modules/function/script/sdk")
    message(FATAL_ERROR
        "Architecture: Engine Script Abilities were centralized in modules/function/script/sdk."
    )
endif()

set(script_system_source
    "${source_root}/engine/domain/simulation/builtin/script/src/ScriptSystem.cpp"
)
file(READ "${script_system_source}" script_system_contract)
string(FIND
    "${script_system_contract}"
    "static_cast<void>(disconnectEndpoints())"
    ignored_rollback_disconnect
)
if(NOT script_system_contract MATCHES "ROLLBACK_PENDING" OR
   NOT ignored_rollback_disconnect EQUAL -1)
    message(FATAL_ERROR
        "Architecture: ScriptSystem prepare rollback can release a live Endpoint lane context."
    )
endif()

set(scripting_core_cmake
    "${source_root}/engine/domain/simulation/scripting/core/CMakeLists.txt"
)
file(READ "${scripting_core_cmake}" scripting_core_contract)
if(NOT scripting_core_contract MATCHES "add_interface_component" OR
   scripting_core_contract MATCHES "systems/script/include|SOURCE_FILES")
    message(FATAL_ERROR
        "Architecture: scripting core is not header-only or exports concrete ScriptSystem ownership."
    )
endif()

set(flowforge_compiler_header
    "${source_root}/engine/toolchain/flowforge/include/lux/engine/flowforge/Compiler.hpp"
)
file(READ "${flowforge_compiler_header}" flowforge_compiler_contract)
if(flowforge_compiler_contract MATCHES
   "AotArtifact|AotOptions|EFlowForgeCompileError|const ExportMethodNode")
    message(FATAL_ERROR
        "Architecture: FlowForge restores a public metadata-only or AOT helper compile surface."
    )
endif()
file(GLOB_RECURSE flowforge_public_headers LIST_DIRECTORIES false
    "${source_root}/engine/toolchain/flowforge/include/*.hpp"
)
list(LENGTH flowforge_public_headers flowforge_public_header_count)
if(NOT flowforge_public_header_count EQUAL 1)
    message(FATAL_ERROR
        "Architecture: FlowForge compiler must expose only Compiler.hpp."
    )
endif()

file(GLOB_RECURSE flowforge_compiler_sources LIST_DIRECTORIES false
    "${source_root}/engine/toolchain/flowforge/*.hpp"
    "${source_root}/engine/toolchain/flowforge/*.cpp"
    "${source_root}/engine/toolchain/flowforge/CMakeLists.txt"
)
foreach(source IN LISTS flowforge_compiler_sources)
    file(READ "${source}" content)
    if(content MATCHES "eventSymbolId|LUX_ENABLE_FLOWFORGE_MLIR")
        message(FATAL_ERROR
            "Architecture: FlowForge restores display-name identity or an optional MLIR compiler path."
        )
    endif()
endforeach()

file(GLOB_RECURSE flowforge_function_sources LIST_DIRECTORIES false
    "${source_root}/modules/function/flowforge/*.hpp"
    "${source_root}/modules/function/flowforge/*.cpp"
)
foreach(source IN LISTS flowforge_function_sources)
    file(READ "${source}" content)
    if(content MATCHES
       "FlowForgeRuntime|FlowForgeVM|FlowForgeScheduler|FlowForgeAwaitManager|FlowForgeEventRuntime|lux/engine/simulation/|ScriptSystem|SystemInstanceId|ServiceRegistry")
        message(FATAL_ERROR
            "Architecture: generic FlowForge source '${source}' acquired runtime ownership or Simulation ontology."
        )
    endif()
endforeach()

set(native_script_backend_source
    "${source_root}/engine/domain/simulation/scripting/native/src/NativeScriptBackend.cpp"
)
file(READ "${native_script_backend_source}" native_script_backend_contract)
if(native_script_backend_contract MATCHES "lux/engine/flowforge/|FlowGraph|ScriptAbilityNode")
    message(FATAL_ERROR
        "Architecture: NativeScriptBackend depends on the FlowForge frontend."
    )
endif()

set(dense_event_storage
    "${source_root}/engine/domain/simulation/system/include/lux/engine/simulation/detail/DenseEntityHandlerStorage.hpp"
)
file(READ "${dense_event_storage}" dense_event_contract)
if(dense_event_contract MATCHES
   "HandlerKey previous|HandlerKey next|TargetBucket head")
    message(FATAL_ERROR
        "Architecture: targeted Event handlers restore intrusive linked execution."
    )
endif()

set(event_point_header
    "${source_root}/engine/domain/simulation/system/include/lux/engine/simulation/HookChannel.hpp"
)
file(READ "${event_point_header}" event_point_contract)
string(CONCAT event_writer_counter_forbidden
    "std::size_t active_writer_count_"
    "|\\+\\+storage_->active_writer_count_"
    "|--storage_->active_writer_count_"
    "|active_writer_count_ = 0U"
)
if(event_point_contract MATCHES "${event_writer_counter_forbidden}")
    message(FATAL_ERROR
        "Architecture: EventOccurrenceBuffer restores racy non-atomic active Writer tracking."
    )
endif()

set(lua_backend_source
    "${source_root}/engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp"
)
file(READ "${lua_backend_source}" lua_backend_contract)
string(FIND "${lua_backend_contract}" "new (std::nothrow) Call" lua_call_new)
string(FIND "${lua_backend_contract}" "delete call;" lua_call_delete)
if(NOT lua_call_new EQUAL -1 OR NOT lua_call_delete EQUAL -1)
    message(FATAL_ERROR
        "Architecture: Lua restores per-prepared-call general heap allocation."
    )
endif()
if(lua_backend_contract MATCHES
   "thread_local|CurrentLua|LuaCoroutineManager|LuaScheduler|LuaAwaitable|LuaEventManager|ScriptApiManager|ServiceRegistry")
    message(FATAL_ERROR
        "Architecture: Lua backend acquired a global current-script or a second scheduler/runtime."
    )
endif()
if(lua_backend_contract MATCHES
   "LUAJIT_VERSION|LUA_VERSION_NUM|luaJIT_setmode|lua_resume|lua_yield")
    message(FATAL_ERROR
        "Architecture: Simulation Lua backend bypasses the portable modules/function/script/lua VM seam."
    )
endif()
file(GLOB_RECURSE lua_architecture_sources LIST_DIRECTORIES false
    "${source_root}/modules/function/script/lua/*.hpp"
    "${source_root}/modules/function/script/lua/*.cpp"
    "${source_root}/engine/domain/simulation/scripting/lua/*.hpp"
    "${source_root}/engine/domain/simulation/scripting/lua/*.cpp"
)
foreach(source IN LISTS lua_architecture_sources)
    file(READ "${source}" content)
    if(content MATCHES
       "LuaJitScriptBackend|Lua54ScriptBackend|LuaVmManager|LuaRuntimeRegistry|LuaPluginManager")
        message(FATAL_ERROR
            "Architecture: Lua portability introduced a second backend, manager or VM registry: ${source}"
        )
    endif()
endforeach()
foreach(portable_lua_fixture IN ITEMS
    "${source_root}/engine/toolchain/lua/test/lua_behavior_fixture.lua"
    "${source_root}/engine/toolchain/lua/test/lua_runtime_benchmark_fixture.lua"
    "${source_root}/engine/toolchain/lua/test/lua_portability_fixture.lua"
    "${source_root}/cmake/installed-consumers/lua-script-packager/inventory.lua"
)
    file(READ "${portable_lua_fixture}" portable_lua_source)
    if(portable_lua_source MATCHES "require.*(ffi|jit)|(^|[^A-Za-z0-9_])(ffi|jit)[.]")
        message(FATAL_ERROR
            "Architecture: portable production Lua fixture uses a VM-specific module: ${portable_lua_fixture}"
        )
    endif()
endforeach()
set(lua_ability_projection
    "${source_root}/modules/function/script/lua/include/lux/engine/function/script/lua/ScriptAbilityLua.hpp"
)
file(READ "${lua_ability_projection}" lua_ability_projection_contract)
if(lua_ability_projection_contract MATCHES "projectScriptAbility|ScriptAbilityBinding[ \t\r\n]+binding")
    message(FATAL_ERROR
        "Architecture: Lua Ability projection captures a bound provider instead of publishing metadata."
    )
endif()

set(cpp_static_backend_source
    "${source_root}/engine/domain/simulation/scripting/cpp_static/src/CppStaticScriptBridge.cpp"
)
file(READ "${cpp_static_backend_source}" cpp_static_backend_contract)
if(cpp_static_backend_contract MATCHES "instance->object = ::operator new")
    message(FATAL_ERROR
        "Architecture: CppStatic restores per-instance object allocation."
    )
endif()

file(GLOB_RECURSE ui_sources LIST_DIRECTORIES false
    "${source_root}/modules/function/ui/include/*.hpp"
    "${source_root}/modules/function/ui/sinclude/*.hpp"
    "${source_root}/modules/function/ui/pinclude/*.hpp"
    "${source_root}/modules/function/ui/src/*.cpp"
)
foreach(source IN LISTS ui_sources)
    file(READ "${source}" content)
    if(content MATCHES
       "lux/engine/(ecs|simulation|runtime|editor|resource|function/render)/")
        message(FATAL_ERROR
            "Architecture: UI foundation '${source}' crosses an L0 boundary."
        )
    endif()
endforeach()

# Engine-owned optional implementations follow their semantic owner. A
# repository-level extensions root would bypass the architecture DAG.
if(EXISTS "${source_root}/extensions")
    message(FATAL_ERROR
        "Architecture: repository-level extensions/ must not exist; place optional builtins under their owner."
    )
endif()
foreach(script_consumer IN ITEMS cpp-coroutine-script script-static-ability-specialization script-ability-ipo)
    if(NOT EXISTS "${source_root}/cmake/installed-consumers/${script_consumer}/CMakeLists.txt")
        message(FATAL_ERROR
            "Architecture: missing installed Script consumer '${script_consumer}'."
        )
    endif()
endforeach()

set(script_native_backend_source
    "${source_root}/engine/domain/simulation/scripting/native/src/NativeScriptBackend.cpp"
)
set(flowforge_aot_source "${source_root}/engine/toolchain/flowforge/src/AOT.cpp")
file(READ "${script_native_backend_source}" script_native_backend_contract)
file(READ "${flowforge_aot_source}" flowforge_aot_contract)
if(script_native_backend_contract MATCHES "invokeAbility|lux_script_ability_runtime")
    message(FATAL_ERROR
        "Architecture: Native Script backend restored the erased per-call Ability hot path."
    )
endif()
if(flowforge_aot_contract MATCHES "lux_script_ability_runtime|storeValueSlot")
    message(FATAL_ERROR
        "Architecture: FlowForge AOT restored erased Ability slot marshalling."
    )
endif()
if(lua_backend_contract MATCHES "PreparedOrdinalTable|recursive_mutex|execution_stack")
    message(FATAL_ERROR
        "Architecture: Lua backend restored dense prepared tables, locking, or execution-stack scans."
    )
endif()
set(script_system_source
    "${source_root}/engine/domain/simulation/builtin/script/src/ScriptSystem.cpp"
)
file(READ "${script_system_source}" script_system_hot_contract)
if(script_system_hot_contract MATCHES "NextStepLater|push_heap\\(next_step|pop_heap\\(next_step")
    message(FATAL_ERROR
        "Architecture: NextStep restored heap scheduling instead of its bounded FIFO."
    )
endif()
set(physics2d_source_package "${source_root}/engine/domain/simulation/builtin/physics2d")
foreach(required IN ITEMS
    "${physics2d_source_package}/include/lux/engine/physics2d/Physics2DSystem.hpp"
    "${physics2d_source_package}/include/lux/engine/physics2d/abilities/PhysicsQuery2D.hpp"
    "${physics2d_source_package}/src/Physics2DSystem.cpp"
)
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Architecture: canonical Physics2D builtin is incomplete: ${required}")
    endif()
endforeach()
file(GLOB_RECURSE physics2d_sources LIST_DIRECTORIES false
    "${physics2d_source_package}/*.hpp"
    "${physics2d_source_package}/*.cpp"
)
foreach(source IN LISTS physics2d_sources)
    file(READ "${source}" content)
    if(content MATCHES
       "lux/engine/ecs/physics2d|SceneServices|luxInstallWorldSystemsV5|PhysicsManager|ServiceRegistry")
        message(FATAL_ERROR
            "Architecture: canonical Physics2D builtin depends on retired ownership in ${source}."
        )
    endif()
endforeach()

file(WRITE "${LUX_REPORT_PATH}"
    "vNext L1 semantic architecture debt: 0\n"
    "legacy roots configured: 0\n"
    "legacy includes from production: 0\n"
    "retired top-level ECS domain/namespace: 0\n"
    "L1 terminal I/O: 0\n"
    "retired L0 asset runtime vocabulary: 0\n"
    "transform full-scan/associative dirty paths: 0\n"
    "legacy entries in compile_commands: 0\n"
    "configuration serialization includes in Engine/L1: 0\n"
    "component codecs/runtime-reflection persistence: 0\n"
    "binary serialization runtime-reflection closure: 0\n"
    "active production throw statements: 0\n"
    "Hook/Event hot-path catches: 0\n"
    "Script rollback lane-context lifetime debt: 0\n"
    "FlowForge duplicate compiler/identity surfaces: 0\n"
    "targeted Event intrusive handler execution: 0\n"
    "per-instance/per-call backend heap allocations: 0\n"
)
