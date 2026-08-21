find_package(Eigen3 CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ECS_COMPONENT_ARCHIVE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ECS_COMPONENT_ARCHIVE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ECS_COMPONENT_ARCHIVE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/serialization/visibility.h
)

add_component(
    COMPONENT_NAME  component_archive
    NAMESPACE       lux::engine::ecs
    OUTPUT_NAME     lux_engine_ecs_component_archive
    SOURCE_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/src/TaggedPropertyArchive.cpp
)

component_include_directories(
    component_archive
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    component_archive
    PRIVATE LUX_ECS_COMPONENT_ARCHIVE_LIBRARY
)

target_link_libraries(
    component_archive
    PUBLIC
    lux::engine::core::serialization
    lux::engine::core::meta
    lux::cxx::compile_time
    PRIVATE
    Eigen3::Eigen
)

component_add_transitive_commands(
    component_archive
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization meta)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  component_archive
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    ecs_component_archive_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/component_archive_contract_test.cpp
)
target_link_libraries(
    ecs_component_archive_contract_test
    PRIVATE lux::engine::ecs::component_archive
)
if(MSVC)
    target_compile_options(ecs_component_archive_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_component_archive_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_component_archive_contract_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)
