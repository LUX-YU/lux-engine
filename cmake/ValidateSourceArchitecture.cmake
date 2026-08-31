cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED LUX_SOURCE_DIR)
    message(FATAL_ERROR "LUX_SOURCE_DIR is required")
endif()
if(NOT DEFINED LUX_REPORT_PATH)
    set(LUX_REPORT_PATH "${CMAKE_CURRENT_BINARY_DIR}/semantic-architecture-debt.txt")
endif()

file(TO_CMAKE_PATH "${LUX_SOURCE_DIR}" source_root)

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
    "${source_root}/engine/domain/simulation/script_binding"
    "${source_root}/engine/domain/simulation/script"
    "${source_root}/engine/authoring/script_binding"
    "${source_root}/engine/authoring/flowforge"
    "${source_root}/engine/tools"
    "${source_root}/engine/toolchain/script"
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

    if(normalized MATCHES "/engine/scene/runtime/render/" AND content MATCHES
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
        if(content MATCHES
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
               "/engine/domain/simulation/(ecs/(hierarchy|transform)|scripting|systems/(script|transform))/" AND
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

    if(normalized MATCHES "/engine/domain/partition/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(world|simulation|process|scene|runtime|authoring|toolchain|editor|host|extensions)/")
            message(FATAL_ERROR
                "Architecture: neutral Partition source '${normalized}' depends on a concrete or upper domain."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/domain/world/identity/")
        if(content MATCHES
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
        if(content MATCHES
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
    elseif(normalized MATCHES "/engine/process/world/")
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
    elseif(normalized MATCHES "/engine/process/asset/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(world|simulation|scene|render|authoring|editor|toolchain|host|runtime|extensions)/")
            message(FATAL_ERROR
                "Architecture: Process Asset workflow '${normalized}' depends on a domain or upper layer."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/process/" AND content MATCHES
       "ProcessRuntime|AsyncRuntime|AsyncRuntimeBuilder|ProcessBuilder|ProcessScope|ProcessScheduler|OperationRegistry|parallelTransform|BatchJoin|AsyncGraph|std::function")
        message(FATAL_ERROR
            "Architecture: Process source '${normalized}' restores a runtime wrapper, registry or deferred API."
        )
    endif()
endforeach()

foreach(hot_runtime_root IN ITEMS
    "${source_root}/engine/domain/simulation/system"
    "${source_root}/engine/domain/simulation/systems/script"
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

# The replacement roots are held to the final contract while the old roots are
# still being removed in staged commits. This prevents new code from rebuilding
# the retired session/name-routing model under a different target name.
file(GLOB_RECURSE script_system_sources LIST_DIRECTORIES false
    "${source_root}/engine/domain/simulation/scripting/*.hpp"
    "${source_root}/engine/domain/simulation/scripting/*.cpp"
    "${source_root}/engine/domain/simulation/systems/script/*.hpp"
    "${source_root}/engine/domain/simulation/systems/script/*.cpp"
)
foreach(source IN LISTS script_system_sources)
    file(READ "${source}" content)
    if(content MATCHES
       "ScriptBindingSession|ScriptComponent|EntityBehavior|EScriptModel|PythonSourceScript|dispatchHook[ \t\r\n]*\\([^,]+,[^,]+,[^,]+|AssetManager|AssetClient|AssetLease|Process|Scene|EEndpointMutationError[ \t\r\n]*\\([*]flush\\)|endpoint[^;\r\n]*->[ \t]*flush|ScriptSystemCapacities|EBehaviorStopReason|startInstance|stopInstance|full_resync|sortHandlers|removeHandlers|allocateInstance|findBackend|findHookBucket|findEventBucket|findMethod|std::lower_bound|std::remove_if")
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
        "lux/engine/(simulation|authoring|editor)|"
        "lux::engine::(simulation|authoring|editor)|imgui::|[/\\]legacy[/\\]"
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
                "Architecture: FlowForge compiler '${source}' reaches Simulation, Authoring, or Editor."
            )
        endif()
    endforeach()
endif()

if(EXISTS "${source_root}/engine/editor/node_graph")
    file(GLOB_RECURSE node_graph_editor_sources LIST_DIRECTORIES false
        "${source_root}/engine/editor/node_graph/*.hpp"
        "${source_root}/engine/editor/node_graph/*.cpp"
        "${source_root}/engine/editor/node_graph/*.cmake"
    )
    foreach(source IN LISTS node_graph_editor_sources)
        file(READ "${source}" content)
        if(content MATCHES "flowforge|engine/simulation|resource/asset|mlir|llvm|[/\\]legacy[/\\]")
            message(FATAL_ERROR
                "Architecture: Node Graph Editor package '${source}' is not domain independent."
            )
        endif()
    endforeach()
endif()

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
    "${source_root}/engine/domain/simulation/systems/transform/src/TransformSystem.cpp"
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

set(scene_core_cmake "${source_root}/engine/scene/core/CMakeLists.txt")
if(EXISTS "${scene_core_cmake}")
    file(READ "${scene_core_cmake}" scene_core_target_contract)
    if(scene_core_target_contract MATCHES
       "lux::engine::(process|render)|stdexec|scene_runtime_world")
        message(FATAL_ERROR
            "Architecture: scene/core depends on Process, Render or runtime/world."
        )
    endif()
endif()

set(scene_world_runtime_cmake "${source_root}/engine/scene/runtime/world/CMakeLists.txt")
if(EXISTS "${scene_world_runtime_cmake}")
    file(READ "${scene_world_runtime_cmake}" scene_world_runtime_contract)
    if(scene_world_runtime_contract MATCHES
       "spatial3d|spatial2d|lux::engine::(render|editor|authoring|toolchain)")
        message(FATAL_ERROR
            "Architecture: scene/runtime/world acquired concrete policy or an upper-layer dependency."
        )
    endif()
endif()

set(latest_exchange_header
    "${source_root}/engine/scene/runtime/presentation/include/lux/engine/scene/LatestSpscExchange.hpp"
)
if(EXISTS "${latest_exchange_header}")
    file(READ "${latest_exchange_header}" latest_exchange_contract)
    if(latest_exchange_contract MATCHES "mutex|condition_variable|wait[ \\t\\r\\n]*\\(")
        message(FATAL_ERROR
            "Architecture: LatestSpscExchange is no longer a non-blocking SPSC primitive."
        )
    endif()
endif()

set(jolt_l1_probe "${source_root}/test/architecture_probes/jolt_l1_probe.cpp")
if(EXISTS "${jolt_l1_probe}")
    file(READ "${jolt_l1_probe}" jolt_l1_probe_contract)
    string(CONCAT jolt_l1_forbidden
        "PhysicsOrigin|FloatingOrigin|OriginRebaseSystem|RebaseDistance|"
        "shiftAllBodies|PhysicsPrecisionMode|JoltSingleBackend|JoltDoubleBackend"
    )
    if(NOT jolt_l1_probe_contract MATCHES "#ifndef[ \t]+JPH_DOUBLE_PRECISION" OR
       NOT jolt_l1_probe_contract MATCHES "same_as<JPH::Real,[ \t]*double>" OR
       NOT jolt_l1_probe_contract MATCHES "same_as<JPH::RVec3,[ \t]*JPH::DVec3>" OR
       NOT jolt_l1_probe_contract MATCHES "addSystemTask<JoltProbeSystem>")
        message(FATAL_ERROR
            "Architecture: Jolt L1 probe lost its double-position ABI or concrete one-task Simulation seam."
        )
    endif()
    if(jolt_l1_probe_contract MATCHES "${jolt_l1_forbidden}")
        message(FATAL_ERROR
            "Architecture: Jolt L1 probe restored a forbidden origin/rebase or runtime precision abstraction."
        )
    endif()
endif()

set(render_lane_probe "${source_root}/test/architecture_probes/render_lane_probe.cpp")
if(EXISTS "${render_lane_probe}")
    file(READ "${render_lane_probe}" render_lane_probe_contract)
    if(NOT render_lane_probe_contract MATCHES "DeviceRenderFixture" OR
       NOT render_lane_probe_contract MATCHES "LatestSpscExchange<PresentationState>" OR
       NOT render_lane_probe_contract MATCHES "trySubmitFrame" OR
       NOT render_lane_probe_contract MATCHES "frame_ring_backpressure" OR
       NOT render_lane_probe_contract MATCHES "validation_errors" OR
       NOT render_lane_probe_contract MATCHES "readback")
        message(FATAL_ERROR
            "Architecture: Render lane probe lost its real Vulkan/frame-ring/latest-state evidence path."
        )
    endif()
    if(render_lane_probe_contract MATCHES
       "ecs::Registry|SceneServices|SimulationIngress|TimeDomain|TickGroup|GenericStreamingSource")
        message(FATAL_ERROR
            "Architecture: Render lane probe acquired live ECS traversal or a held generic runtime abstraction."
        )
    endif()
endif()

set(spatial3d_preloaded_probe
    "${source_root}/test/architecture_probes/spatial3d_preloaded_probe.cpp"
)
if(EXISTS "${spatial3d_preloaded_probe}")
    file(READ "${spatial3d_preloaded_probe}" spatial3d_preloaded_contract)
    if(NOT spatial3d_preloaded_contract MATCHES "loadWorldPartition" OR
       NOT spatial3d_preloaded_contract MATCHES "WorldMaterializer" OR
       NOT spatial3d_preloaded_contract MATCHES "PreloadedSpatial3DSystem" OR
       NOT spatial3d_preloaded_contract MATCHES "JPH::PhysicsSystem" OR
       NOT spatial3d_preloaded_contract MATCHES "tryCreateTexture2DCopy" OR
       NOT spatial3d_preloaded_contract MATCHES "uploadGraphMaterial" OR
       NOT spatial3d_preloaded_contract MATCHES "uploadMesh" OR
       NOT spatial3d_preloaded_contract MATCHES "state.physics_position[ \t]*-[ \t]*render_origin")
        message(FATAL_ERROR
            "Architecture: preloaded Spatial3D probe lost a World/Jolt/Render vertical-slice seam."
        )
    endif()
    string(CONCAT spatial3d_preloaded_forbidden
        "DemandKey|DemandTracker|ResidencyBridge|ResourceDemandRegistry|TimeDomain|TickGroup|"
        "SimulationIngress|GenericStreamingSource|PhysicsOrigin|FloatingOrigin|OriginRebaseSystem|"
        "RebaseDistance|shiftAllBodies|PhysicsPrecisionMode|JoltSingleBackend|JoltDoubleBackend"
    )
    if(spatial3d_preloaded_contract MATCHES "${spatial3d_preloaded_forbidden}")
        message(FATAL_ERROR
            "Architecture: preloaded Spatial3D probe promoted a held generic/origin abstraction."
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
endforeach()

if(EXISTS "${source_root}/engine/domain/world/core/include/lux/engine/world/WorldObjectId.hpp")
    message(FATAL_ERROR "Architecture: retired World-owned WorldObjectId header remains installed in source.")
endif()
if(EXISTS "${source_root}/engine/scene/runtime/world/include/lux/engine/scene/WorldRuntime.hpp")
    message(FATAL_ERROR "Architecture: retired Scene WorldRuntime aggregate header remains in source.")
endif()

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
    "${source_root}/engine/toolchain/asset/material/include/lux/engine/toolchain/asset/material/ImportedMaterialDescription.hpp"
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

file(GLOB_RECURSE authoring_material_sources LIST_DIRECTORIES false
    "${source_root}/engine/authoring/material/*.hpp"
    "${source_root}/engine/authoring/material/*.cpp"
)
foreach(source IN LISTS authoring_material_sources)
    file(READ "${source}" content)
    if(content MATCHES "namespace[ \t]+lux::rdesc")
        message(FATAL_ERROR
            "Architecture: MaterialGraph source '${source}' was restored to Resource Description ownership."
        )
    endif()
endforeach()

file(GLOB_RECURSE runtime_asset_boundary_sources LIST_DIRECTORIES false
    "${source_root}/modules/resource/asset/*.hpp"
    "${source_root}/modules/resource/asset/*.cpp"
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
endforeach()

set(world_runtime_header
    "${source_root}/engine/scene/runtime/world/include/lux/engine/scene/WorldRuntime.hpp"
)
set(world_runtime_source
    "${source_root}/engine/scene/runtime/world/src/WorldRuntime.cpp"
)
set(world_partition_data_header
    "${source_root}/engine/domain/world/storage/include/lux/engine/world/WorldPartitionData.hpp"
)
if(EXISTS "${world_runtime_header}" AND EXISTS "${world_runtime_source}")
    file(READ "${world_runtime_header}" world_runtime_header_contract)
    file(READ "${world_runtime_source}" world_runtime_source_contract)
    if(NOT world_runtime_header_contract MATCHES "std::size_t[ \\t]+max_bytes" OR
       world_runtime_source_contract MATCHES "512U[ \\t]*[*][ \\t]*1024U")
        message(FATAL_ERROR
            "Architecture: World partition load lost its Product-supplied byte budget."
        )
    endif()
    if(NOT world_runtime_source_contract MATCHES "accounted_bytes[ \\t]*=")
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

foreach(installed_consumer IN ITEMS
    core_system
    component_decode_emplace
    ecs_core
    ecs_system
    large_world_transform
    object_affinity
    resource_descriptions
    resource_identity
    simulation_runtime
    world
    world_storage
    typed_resource_assets
)
    if(NOT EXISTS
       "${source_root}/test/l1_installed_consumer/${installed_consumer}/CMakeLists.txt")
        message(FATAL_ERROR
            "Architecture: missing independent L1 installed consumer '${installed_consumer}'."
        )
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
    "${source_root}/engine/domain/spatial/CMakeLists.txt"
    "${source_root}/engine/domain/spatial/*/CMakeLists.txt"
    "${source_root}/engine/domain/simulation/CMakeLists.txt"
    "${source_root}/engine/domain/simulation/*/CMakeLists.txt"
    "${source_root}/engine/domain/simulation/*/*/CMakeLists.txt"
    "${source_root}/engine/process/CMakeLists.txt"
    "${source_root}/engine/process/*/CMakeLists.txt"
    "${source_root}/engine/authoring/CMakeLists.txt"
    "${source_root}/engine/authoring/*/CMakeLists.txt"
    "${source_root}/engine/editor/CMakeLists.txt"
    "${source_root}/engine/editor/*/CMakeLists.txt"
    "${source_root}/engine/toolchain/CMakeLists.txt"
    "${source_root}/engine/toolchain/*/CMakeLists.txt"
)
foreach(source IN LISTS active_cmake)
    file(READ "${source}" content)
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
    elseif(source MATCHES "/engine/process/world/")
        if(content MATCHES
           "legacy|lux::engine::(simulation|scene|render|authoring|editor|toolchain|host)|asio|TBB|static_thread_pool")
            message(FATAL_ERROR
                "Architecture: Process World target '${source}' links an upper/domain/legacy dependency."
            )
        endif()
    elseif(source MATCHES "/engine/process/asset/")
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
set(process_port_sender_header
    "${source_root}/engine/process/execution/include/lux/engine/process/PortSender.hpp"
)
set(process_installed_consumer
    "${source_root}/cmake/installed-consumers/process-execution/CMakeLists.txt"
)
foreach(required_process_file IN ITEMS
    "${process_timer_header}"
    "${process_port_sender_header}"
    "${process_installed_consumer}"
)
    if(NOT EXISTS "${required_process_file}")
        message(FATAL_ERROR
            "Architecture: Process Wave 0 is missing '${required_process_file}'."
        )
    endif()
endforeach()

foreach(required_domain_consumer IN ITEMS domain-partition domain-world-identity)
    if(NOT EXISTS "${source_root}/cmake/installed-consumers/${required_domain_consumer}/CMakeLists.txt")
        message(FATAL_ERROR
            "Architecture: missing installed Domain consumer '${required_domain_consumer}'."
        )
    endif()
endforeach()

if(NOT EXISTS "${source_root}/cmake/installed-consumers/process-world/CMakeLists.txt")
    message(FATAL_ERROR "Architecture: missing installed Process World consumer.")
endif()
if(NOT EXISTS "${source_root}/cmake/installed-consumers/process-asset/CMakeLists.txt")
    message(FATAL_ERROR "Architecture: missing installed Process Asset consumer.")
endif()
if(NOT EXISTS "${source_root}/cmake/installed-consumers/spatial3d-index/CMakeLists.txt")
    message(FATAL_ERROR "Architecture: missing installed Spatial3D index consumer.")
endif()
if(NOT EXISTS "${source_root}/cmake/installed-consumers/spatial2d-index/CMakeLists.txt")
    message(FATAL_ERROR "Architecture: missing installed Spatial2D index consumer.")
endif()
foreach(required_render_consumer IN ITEMS render-client scene-render)
    if(NOT EXISTS "${source_root}/cmake/installed-consumers/${required_render_consumer}/CMakeLists.txt")
        message(FATAL_ERROR "Architecture: missing installed Render consumer '${required_render_consumer}'.")
    endif()
endforeach()

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
    "${source_root}/engine/scene/runtime/presentation/include/lux/engine/scene/LatestSpscExchange.hpp"
    "${source_root}/engine/scene/runtime/render/include/lux/engine/scene/RenderSystem.hpp"
)
    if(NOT EXISTS "${required_render_contract}")
        message(FATAL_ERROR "Architecture: missing retained Render lane contract '${required_render_contract}'.")
    endif()
endforeach()
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

set(render_system_header
    "${source_root}/engine/scene/runtime/render/include/lux/engine/scene/RenderSystem.hpp"
)
set(render_system_source
    "${source_root}/engine/scene/runtime/render/src/RenderSystem.cpp"
)
file(READ "${render_system_header}" render_system_contract)
file(READ "${render_system_source}" render_system_implementation)
if(render_system_contract MATCHES "Mesh3D|Light3D|MeshStackOperationIds|LightOperationIds|Registry|Config")
    message(FATAL_ERROR "Architecture: RenderSystem public API owns a concrete Mesh/Light extraction domain.")
endif()
if(render_system_implementation MATCHES
   "Mesh3D|Light3D|DirtySlot|dirty_bits|summary|expected_entity_capacity|EntityCapacity")
    message(FATAL_ERROR "Architecture: generic RenderSystem restores concrete extraction or central dirty storage.")
endif()

set(render_builtin_stage_source
    "${source_root}/engine/scene/runtime/render/src/Builtin3DRenderStages.cpp"
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

foreach(render_qualification_source IN ITEMS
    model_asset_vulkan_qualification.cpp
    l1_l3_render_sync_3d_qualification.cpp
    large_3d_scene_performance_qualification.cpp
    l1_l3_render_lane_window_qualification.cpp
)
    if(NOT EXISTS "${source_root}/test/architecture_probes/${render_qualification_source}")
        message(FATAL_ERROR
            "Architecture: missing split Render qualification '${render_qualification_source}'."
        )
    endif()
endforeach()
file(READ "${source_root}/test/architecture_probes/CMakeLists.txt" render_qualification_cmake)
if(render_qualification_cmake MATCHES
   "l1_l3_render_sync_3d_qualification[ \t\r\n]+model_asset_vulkan_qualification[.]cpp|large_3d_scene_performance_qualification[ \t\r\n]+model_asset_vulkan_qualification[.]cpp")
    message(FATAL_ERROR "Architecture: Render qualifications restore macro-reused source ownership.")
endif()
if(NOT render_qualification_cmake MATCHES
   "LUX_BUILD_RENDER_LANE_WINDOW_QUALIFICATION[ \t\r\n]+[^\r\n]*[ \t\r\n]+OFF")
    message(FATAL_ERROR "Architecture: visible-window Render qualification is not default OFF.")
endif()
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

set(script_system_source
    "${source_root}/engine/domain/simulation/systems/script/src/ScriptSystem.cpp"
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
    "${source_root}/engine/domain/simulation/system/include/lux/engine/simulation/EventPoint.hpp"
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
