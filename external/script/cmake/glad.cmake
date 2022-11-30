project(glad)
cmake_minimum_required(VERSION 3.10)

set(GLAD_SRCS
    src/glad.c
)

add_library(
    glad
    STATIC
    ${GLAD_SRCS}
)

target_compile_definitions(
    glad
    PUBLIC
    GLAD_GLAPI_EXPORT
    PRIVATE
    GLAD_GLAPI_EXPORT_BUILD
)

target_include_directories(
    glad
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

install(
    DIRECTORY	${CMAKE_CURRENT_SOURCE_DIR}/include
    DESTINATION ${CMAKE_INSTALL_PREFIX}
)

install(
    TARGETS glad
)
