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
if(EXISTS "${source_root}/engine/ecs/schedule")
    message(FATAL_ERROR
        "Architecture: retired engine/ecs/schedule must not be restored."
    )
endif()
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
    "${source_root}/engine/ecs/*/include/*.hpp"
    "${source_root}/engine/ecs/*/sinclude/*.hpp"
    "${source_root}/engine/ecs/*/pinclude/*.hpp"
    "${source_root}/engine/ecs/*/src/*.cpp"
    "${source_root}/engine/world/include/*.hpp"
    "${source_root}/engine/world/src/*.cpp"
)

foreach(source IN LISTS production_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    file(READ "${source}" content)

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

    if(normalized MATCHES "/engine/ecs/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/world/")
            message(FATAL_ERROR
                "Architecture: ECS source '${normalized}' depends on the sibling World domain."
            )
        endif()
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(scene|runtime|process|editor|authoring|toolchain|host|extensions)/")
            message(FATAL_ERROR
                "Architecture: L1 source '${normalized}' includes an upper-layer API."
            )
        endif()
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/object/")
            message(FATAL_ERROR
                "Architecture: L1 production source '${normalized}' depends on Object instead of the generic affinity protocol."
            )
        endif()
        if(content MATCHES
           "SceneServices|ISystem|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|cooked_relocation|LXES|LXWS|ComponentCodec|ComponentPersistence|ComponentLoadBinding|ComponentLoadSet|EcsBinaryWriter|EcsBinaryReader|persistence_contract|ecs_persistence|TaggedProperty|RefClass|RefField|WorldSection|PersistentEntity|PersistentId|ecs_load|section[ \t]*=[ \t]*(LOAD|OMIT)|LUX_REBUILD_COMPONENT_SCHEMA|LUX_COMPONENT_SCHEMA|LUX_COMPONENT_SNAPSHOT|LUX_COMPONENT_WORLD_SECTION|CloneFn|default_emplace|COPY_WITHOUT_CLONE|ecs::World|WorldMutation|WorldChange|WorldCommand|WorldSnapshot|World::registry[ \t\r\n]*\\(|setParent[ \t\r\n]*\\(|clearParent[ \t\r\n]*\\(")
            message(FATAL_ERROR
                "Architecture: L1 source '${normalized}' restores retired ECS vocabulary."
            )
        endif()
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/ecs/(Schedule|ScheduleEdit|ScheduleError|SystemFrame|SystemHandle|SystemPhase|SystemSetId)[.]hpp")
            message(FATAL_ERROR
                "Architecture: L1 source '${normalized}' includes retired Schedule execution API."
            )
        endif()
        if(content MATCHES
           "connectConstruct|connectUpdate|connectDestroy|observer_relations_|on_construct[ \t\r\n]*<|on_update[ \t\r\n]*<|on_destroy[ \t\r\n]*<")
            message(FATAL_ERROR
                "Architecture: L1 source '${normalized}' restores retired EnTT observer ownership."
            )
        endif()
        if(content MATCHES
           "AssetStore|AssetClient|AssetLease|AssetManager|AssetRef|AssetLoadPort|AssetServices")
            message(FATAL_ERROR
                "Architecture: L1 source '${normalized}' depends on deferred asset ownership API."
            )
        endif()
        if(content MATCHES
           "printf[ \t\r\n]*\\(|fprintf[ \t\r\n]*\\(|std::(cout|cerr)|MessageBox[AW]?[ \t\r\n]*\\(")
            message(FATAL_ERROR
                "Architecture: L1 source '${normalized}' performs terminal I/O."
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

    if(normalized MATCHES "/modules/core/task/")
        if(content MATCHES
           "#[ \t]*include[ \t]*[<\"]lux/engine/(ecs|object|process|scene)/")
            message(FATAL_ERROR
                "Architecture: L0 core::task source '${normalized}' depends on an upper-layer subsystem."
            )
        endif()
    endif()

    if(normalized MATCHES "/engine/world/")
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
endforeach()

foreach(source IN LISTS production_sources)
    file(TO_CMAKE_PATH "${source}" normalized)
    if(normalized MATCHES "/engine/ecs/schema/")
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
    "${source_root}/engine/ecs/*/include/*.hpp"
)
foreach(source IN LISTS ecs_public_headers)
    file(READ "${source}" content)
    if(content MATCHES
       "#[ \t]*include[ \t]*[<\"]lux/engine/ecs/detail/")
        message(FATAL_ERROR
            "Architecture: public ECS header '${source}' includes unsupported detail API."
        )
    endif()
endforeach()

set(transform_system_source
    "${source_root}/engine/ecs/transform/src/TransformSystem.cpp"
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
    "${source_root}/engine/ecs/hierarchy/include/lux/engine/ecs/Parent.hpp"
    "${source_root}/engine/ecs/transform/include/lux/engine/ecs/Transform.hpp"
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
    "${source_root}/engine/CMakeLists.txt"
    "${source_root}/engine/world/CMakeLists.txt"
    "${source_root}/engine/ecs/CMakeLists.txt"
    "${source_root}/engine/ecs/*/CMakeLists.txt"
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
       "lux/engine/(ecs|runtime|editor|resource|function/render)/")
        message(FATAL_ERROR
            "Architecture: UI foundation '${source}' crosses an L0 boundary."
        )
    endif()
endforeach()

file(WRITE "${LUX_REPORT_PATH}"
    "vNext L1 semantic architecture debt: 0\n"
    "legacy roots configured: 0\n"
    "legacy includes from production: 0\n"
    "retired ECS vocabulary in L1: 0\n"
    "L1 terminal I/O: 0\n"
    "retired L0 asset runtime vocabulary: 0\n"
    "transform full-scan/associative dirty paths: 0\n"
    "legacy entries in compile_commands: 0\n"
    "configuration serialization includes in Engine/L1: 0\n"
    "component codecs/runtime-reflection persistence: 0\n"
    "binary serialization runtime-reflection closure: 0\n"
)
