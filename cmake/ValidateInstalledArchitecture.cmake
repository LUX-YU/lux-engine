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
       "^(Registry|ISystem|SceneServices|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|AssetManager|AssetRef|AssetLoadPort|AssetServices|ComponentCodec|TaggedPropertyArchive|Archive|ByteIO|NameTable)\\.(h|hpp)$")
        message(FATAL_ERROR "Install surface exposes retired API: ${normalized}")
    endif()
endforeach()

file(GLOB_RECURSE installed_text LIST_DIRECTORIES false
    "${prefix}/*.hpp"
    "${prefix}/*.h"
    "${prefix}/*.cmake"
)
foreach(entry IN LISTS installed_text)
    file(READ "${entry}" content)
    if(content MATCHES "[/\\\\]legacy[/\\\\]" OR
       content MATCHES "AssetStore|AssetClient|AssetLease|AssetManager|AssetRef|AssetLoadPort|AssetServices|SceneServices|ISystem|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|connectConstruct|connectUpdate|connectDestroy|observer_relations_|ComponentCodec|ComponentPersistence|EcsBinaryWriter|EcsBinaryReader|persistence_contract|[.]ecs_persistence[.]hpp|TaggedProperty|schema_reflection|cooked_relocation|LXES|LXWS|WorldSectionWriter|WorldSectionReader|encodeWorldSection|decodeWorldSection|WorldArchetype|WorldEntityRecord|WorldComponentColumn|WorldSectionWriteSelection|lux/cxx/serialization/|lux::cxx::ser|LUX_CLASS[ \\t]*\\(|LUX_ENUM[ \\t]*\\(" OR
       content MATCHES "#[ \t]*include[ \t]*[<\"]lux/engine/process/")
        message(FATAL_ERROR "Installed file contains a retired boundary: ${entry}")
    endif()
endforeach()

if(DEFINED INSTALL_MANIFEST AND EXISTS "${INSTALL_MANIFEST}")
    file(READ "${INSTALL_MANIFEST}" manifest)
    if(manifest MATCHES "[/\\\\]legacy[/\\\\]" OR
       manifest MATCHES "[/\\\\](sinclude|pinclude)[/\\\\]")
        message(FATAL_ERROR "Install manifest contains quarantine/private paths")
    endif()
endif()

foreach(required_component IN ITEMS
    core
    schedule
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

set(schedule_targets
    "${prefix}/share/lux-engine-ecs/schedule/lux-engine-ecs-schedule-config-targets.cmake"
)
file(READ "${schedule_targets}" schedule_contract)
if(schedule_contract MATCHES
   "lux::engine::core::object|reflection_runtime|schema_reflection")
    message(FATAL_ERROR
        "Installed core + schedule closure depends on Object/reflection"
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
