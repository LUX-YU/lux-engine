cmake_minimum_required(VERSION 3.28)

project(EditorErrorCatalogConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT COMPONENT MATCHES "^(project|context|ui|app)$")
    message(FATAL_ERROR "Specify one concrete Editor component")
endif()

find_package(lux-engine-editor-framework REQUIRED COMPONENTS lux_editor_${COMPONENT})

add_executable(error_catalog ../../error_catalog.cpp)

target_link_libraries(error_catalog PRIVATE lux::engine::editor::lux_editor_${COMPONENT})

string(TOUPPER "${COMPONENT}" macro)

target_compile_definitions(error_catalog PRIVATE LUX_TEST_${macro}_ERRORS=1)

if(MSVC)
    target_compile_options(error_catalog PRIVATE /UNDEBUG /utf-8 /permissive- /GR- /we4541)
else()
    target_compile_options(error_catalog PRIVATE -UNDEBUG -fno-rtti)
endif()

enable_testing()

add_test(NAME catalog.${COMPONENT} COMMAND error_catalog)

add_test(NAME catalog.${COMPONENT}.collision COMMAND error_catalog --collision)
