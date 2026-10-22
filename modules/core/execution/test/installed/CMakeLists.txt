cmake_minimum_required(VERSION 3.25)

project(ExecutionInstalledConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_subdirectory(consumer)

# The installed package first imports stdexec in a child directory. A second consumer
# must see the entire original export set, without relying on a root find_package().
find_package(stdexec CONFIG REQUIRED)
