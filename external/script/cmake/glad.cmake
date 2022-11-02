project(glad)
cmake_minimum_required(VERSION 3.10)

set(GLAD_SRCS
    src/glad.c
)

add_library(
    glad
    SHARED
    ${GLAD_SRCS}
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
