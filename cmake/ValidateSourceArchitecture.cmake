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
if(EXISTS "${source_root}/engine/editor")
    message(FATAL_ERROR
        "Architecture: Rev2 has no canonical Editor product; do not create a temporary editor tree."
    )
endif()
foreach(retired_root IN ITEMS
    "${source_root}/engine/flowforge"
    "${source_root}/engine/graph_kit"
    "${source_root}/engine/simulation"
    "${source_root}/engine/world"
    "${source_root}/engine/domain/simulation/script_binding"
    "${source_root}/engine/domain/simulation/script"
    "${source_root}/engine/authoring/script_binding"
    "${source_root}/engine/authoring/flowforge"
    "${source_root}/engine/toolchain"
)
    if(EXISTS "${retired_root}")
        message(FATAL_ERROR
            "Architecture: retired source root remains present: ${retired_root}"
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
    "${source_root}/engine/domain/simulation/*/include/*.hpp"
    "${source_root}/engine/domain/simulation/*/sinclude/*.hpp"
    "${source_root}/engine/domain/simulation/*/pinclude/*.hpp"
    "${source_root}/engine/domain/simulation/*/src/*.cpp"
    "${source_root}/engine/domain/simulation/*/*/include/*.hpp"
    "${source_root}/engine/domain/simulation/*/*/sinclude/*.hpp"
    "${source_root}/engine/domain/simulation/*/*/pinclude/*.hpp"
    "${source_root}/engine/domain/simulation/*/*/src/*.cpp"
    "${source_root}/engine/authoring/*/include/*.hpp"
    "${source_root}/engine/authoring/*/src/*.cpp"
    "${source_root}/engine/tools/toolchain/*/include/*.hpp"
    "${source_root}/engine/tools/toolchain/*/src/*.cpp"
)

foreach(source IN LISTS production_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    file(READ "${source}" content)

    if(content MATCHES
       "SystemExecutionPoint|SystemHookPoint|execution_points|dispatch_point|ESystemEventTarget::BROADCAST|ScriptEventRegistry|GlobalScriptBindingManager|ScriptBindingSession|ScriptComponent|EntityBehavior|LUX_SCRIPT_METHOD|LUX_BIND_POINT|LUX_BIND_EVENT|LUX_BEHAVIOR_LIFECYCLE|@lux[.]bind_(point|event)|default_bindings|EScriptBindingSetMode|ScriptMountFacts|ScriptEventWriter|CppBehaviorScript|PythonSourceScript|SemanticCatalog|TargetCatalog|entity_to_sidecar|entity_slots|hook_range_begin|hook_range_count|hot_path_(allocations|name_lookups|asset_lookups|signature_adaptations|scene_scans)|lua_pushlightuserdata[^;]*instance|value[.]name[ ]*==[ ]*node->name|struct[ ]+LuaComponentBinding[^}]*string_view|struct[ ]+(ScriptBindingTargetCatalogEntry|ExportMethodNode)[^}]*ScriptSemanticType")
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
       "#[ \t]*include[ \t]*[<\"]lux/cxx/serialization/|lux::cxx::ser")
        message(FATAL_ERROR
            "Architecture: Engine source '${normalized}' uses configuration serialization instead of lux::serialization."
        )
    endif()

    if(normalized MATCHES "/engine/domain/simulation/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(scene|runtime|process|editor|authoring|toolchain|host|extensions)/")
            message(FATAL_ERROR
                "Architecture: L1 Simulation source '${normalized}' includes an upper-layer API."
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
               "/engine/domain/simulation/(ecs/(hierarchy|transform)|scripting|systems/(script|transform))/")
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
    endif()

    if(normalized MATCHES "/engine/domain/simulation/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(process|scene|authoring|toolchain|editor|host)/")
            message(FATAL_ERROR
                "Architecture: L1 Simulation source '${normalized}' includes an upper-layer API."
            )
        endif()
    endif()
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

if(EXISTS "${source_root}/engine/tools/toolchain/flowforge")
    string(CONCAT flowforge_compiler_forbidden
        "lux/engine/(simulation|authoring|editor)|"
        "lux::engine::(simulation|authoring|editor)|imgui::|[/\\]legacy[/\\]"
    )
    file(GLOB_RECURSE flowforge_compiler_sources LIST_DIRECTORIES false
        "${source_root}/engine/tools/toolchain/flowforge/*.hpp"
        "${source_root}/engine/tools/toolchain/flowforge/*.cpp"
        "${source_root}/engine/tools/toolchain/flowforge/*.cmake"
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

if(EXISTS "${source_root}/engine/tools/editor/node_graph")
    file(GLOB_RECURSE node_graph_editor_sources LIST_DIRECTORIES false
        "${source_root}/engine/tools/editor/node_graph/*.hpp"
        "${source_root}/engine/tools/editor/node_graph/*.cpp"
        "${source_root}/engine/tools/editor/node_graph/*.cmake"
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
    "${source_root}/modules/resource/asset/include/lux/engine/resource/asset/AssetCodecSet.hpp"
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
       "AssetStore|AssetClient|AssetLease|AssetManager|AssetRef|AssetLoadPort|AssetServices|SceneServices|ISystem|ScheduleBuilder|WorldSectionLoadBatch|WorldSectionLoader[.]hpp|connectConstruct|connectUpdate|connectDestroy|observer_relations_|[/\\\\]persistence_contract[/\\\\]|[.]ecs_persistence[.]hpp|ComponentPersistence|EcsBinaryWriter|EcsBinaryReader|[/\\\\]engine[/\\\\]process[/\\\\]")
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
    ecs_core
    ecs_system
    object_affinity
    world
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
    "${source_root}/engine/domain/simulation/CMakeLists.txt"
    "${source_root}/engine/domain/simulation/*/CMakeLists.txt"
    "${source_root}/engine/domain/simulation/*/*/CMakeLists.txt"
    "${source_root}/engine/authoring/CMakeLists.txt"
    "${source_root}/engine/authoring/*/CMakeLists.txt"
    "${source_root}/engine/tools/toolchain/CMakeLists.txt"
    "${source_root}/engine/tools/toolchain/*/CMakeLists.txt"
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
endforeach()

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
)
