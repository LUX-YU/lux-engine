find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_WORLD_DESCRIPTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_WORLD_DESCRIPTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_WORLD_DESCRIPTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/world/description/visibility.h
)

add_component(
    COMPONENT_NAME world_description
    NAMESPACE      lux::engine::world
    OUTPUT_NAME    lux_engine_world_description
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldDataSchemaId.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldDescription.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldDescriptionBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldDescriptionFailureInjection.cpp
)
add_library(lux::engine::world::description ALIAS world_description)
component_include_directories(
    world_description
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(world_description PRIVATE LUX_ENGINE_WORLD_DESCRIPTION_LIBRARY)
target_link_libraries(
    world_description
    PUBLIC
        lux::engine::world::partition
        lux::engine::world::identity
        lux::cxx::compile_time
)
component_add_transitive_commands(
    world_description
    "find_package(lux-engine-world-partition REQUIRED COMPONENTS world_partition)"
    "find_package(lux-engine-world-identity REQUIRED COMPONENTS world_identity)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "add_library(lux::engine::world::description INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::world::description PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::world::world_description)"
)
lux_classify_target(TARGET world_description LAYER WORLD PRODUCT RUNTIME ROLE FOUNDATION)

add_executable(world_description_test test/world_description_test.cpp)
target_include_directories(world_description_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
target_link_libraries(world_description_test PRIVATE lux::engine::world::description)
if(MSVC)
    target_compile_options(world_description_test PRIVATE /UNDEBUG)
else()
    target_compile_options(world_description_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET world_description_test LAYER WORLD PRODUCT TEST ROLE FOUNDATION)
add_test(NAME world_description_test COMMAND world_description_test)

install_components(
    PROJECT_NAME lux-engine-world-description
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::world
    COMPONENTS   world_description
)
