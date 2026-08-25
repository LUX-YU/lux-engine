cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED INSTALL_PREFIX OR INSTALL_PREFIX STREQUAL "")
    message(FATAL_ERROR "INSTALL_PREFIX is required")
endif()

file(TO_CMAKE_PATH "${INSTALL_PREFIX}" prefix)
if(NOT IS_DIRECTORY "${prefix}")
    message(FATAL_ERROR "Install prefix does not exist: ${prefix}")
endif()

file(GLOB_RECURSE installed_entries LIST_DIRECTORIES true "${prefix}/*")
foreach(entry IN LISTS installed_entries)
    file(TO_CMAKE_PATH "${entry}" normalized)
    if(normalized MATCHES "[/](legacy|sinclude|pinclude)([/]|$)" OR
       normalized MATCHES "[/]ecs[/](detail|core/detail|schedule/detail)([/]|$)")
        message(FATAL_ERROR
            "Install surface exposes quarantine/private detail: ${normalized}"
        )
    endif()
    get_filename_component(name "${entry}" NAME)
    if(name MATCHES
       "^(Registry|ISystem|System|SystemFrame|SystemHandle|SystemPhase|SystemSetId|Schedule|ScheduleEdit|ScheduleError|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|SceneServices|AssetManager|AssetRef|AssetLoadPort|AssetServices|ComponentCodec|TaggedPropertyArchive|Archive|ByteIO|NameTable)\\.(h|hpp)$")
        message(FATAL_ERROR "Install surface exposes retired API: ${normalized}")
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
       content MATCHES "AssetStore|AssetClient|AssetLease|AssetManager|AssetRef|AssetLoadPort|AssetServices|SceneServices|ISystem|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|connectConstruct|connectUpdate|connectDestroy|observer_relations_|ComponentCodec|ComponentPersistence|EcsBinaryWriter|EcsBinaryReader|persistence_contract|[.]ecs_persistence[.]hpp|TaggedProperty|schema_reflection|cooked_relocation|LXES|LXWS|WorldSectionWriter|WorldSectionReader|encodeWorldSection|decodeWorldSection|WorldArchetype|WorldEntityRecord|WorldComponentColumn|WorldSectionWriteSelection|LUX_REBUILD_COMPONENT_SCHEMA|LUX_COMPONENT_SCHEMA|LUX_COMPONENT_SNAPSHOT|LUX_COMPONENT_WORLD_SECTION|lux/cxx/serialization/|lux::cxx::ser|LUX_CLASS[ \\t]*\\(|LUX_ENUM[ \\t]*\\(" OR
       content MATCHES "#[ \t]*include[ \t]*[<\"]lux/engine/process/")
        message(FATAL_ERROR "Installed file contains a retired boundary: ${entry}")
    endif()
    if(normalized MATCHES "[/]include[/]lux[/]engine[/]ecs[/]" AND
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
    if(manifest MATCHES "lux-engine-ecs[/\\\\]schedule")
        message(FATAL_ERROR "Install manifest contains retired schedule component")
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
   "lux::engine::ecs|lux::engine::object|lux::engine::process|lux::engine::scene")
    message(FATAL_ERROR
        "Installed core::task closure depends on an upper-layer subsystem"
    )
endif()

foreach(required_component IN ITEMS
    core
    system
    world_section
    persistence
)
    set(component_targets
        "${prefix}/share/lux-engine-ecs/${required_component}/lux-engine-ecs-${required_component}-config-targets.cmake"
    )
    if(NOT EXISTS "${component_targets}")
        message(FATAL_ERROR
            "Installed ECS component is missing: ${required_component}"
        )
    endif()
endforeach()

set(system_targets
    "${prefix}/share/lux-engine-ecs/system/lux-engine-ecs-system-config-targets.cmake"
)
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
    "${prefix}/share/lux-engine-ecs/world_section/lux-engine-ecs-world_section-config-targets.cmake"
    "${prefix}/share/lux-engine-ecs/persistence/lux-engine-ecs-persistence-config-targets.cmake"
)
    if(NOT EXISTS "${contract_file}")
        message(FATAL_ERROR "Installed foundation target is missing: ${contract_file}")
    endif()
    file(READ "${contract_file}" foundation_contract)
    if(foundation_contract MATCHES
       "lux::engine::core::meta|lux::cxx::reflection_runtime")
        message(FATAL_ERROR
            "Installed static serialization/persistence closure depends on runtime reflection: ${contract_file}"
        )
    endif()
endforeach()

message(STATUS "Installed architecture surface is clean: ${prefix}")
