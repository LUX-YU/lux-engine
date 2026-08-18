# Pure LXES EntitySection wire format. No Engine, Extension, Scene Feature,
# AssetStore, renderer, or editor semantics may enter this target.

find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time core)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ECS_SCENE_FORMAT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ECS_SCENE_FORMAT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ECS_SCENE_FORMAT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/scene_format/visibility.h
)

add_component(
    COMPONENT_NAME scene_format
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_scene_format
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/PersistenceJournal.cpp
)

component_include_directories(
    scene_format
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(
    scene_format
    PRIVATE LUX_ECS_SCENE_FORMAT_LIBRARY
)

target_link_libraries(
    scene_format
    PUBLIC
        lux::cxx::algorithm
        lux::cxx::compile_time
        lux::cxx::core
        lux::engine::ecs::identity
    PRIVATE
        lux::engine::core::serialization
)

component_add_transitive_commands(
    scene_format
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time core)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS identity)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)

lux_classify_target(
    TARGET  scene_format
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    scene_format_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/scene_format_contract_test.cpp
)
target_link_libraries(
    scene_format_contract_test
    PRIVATE lux::engine::ecs::scene_format
)
if(MSVC)
    target_compile_options(scene_format_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_format_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  scene_format_contract_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)
