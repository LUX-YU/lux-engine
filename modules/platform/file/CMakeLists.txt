add_component(
    STATIC
    COMPONENT_NAME platform_file
    NAMESPACE lux::engine::platform
    SOURCE_FILES src/FileIo.cpp src/AtomicFileWriter.cpp
)

component_include_directories(
    platform_file
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(platform_file PUBLIC lux::cxx::compile_time)

component_add_transitive_commands(platform_file "find_package(lux-cxx REQUIRED COMPONENTS compile_time)")

lux_classify_target(TARGET platform_file LAYER MODULES PRODUCT SHARED ROLE LIBRARY)

if(BUILD_TESTING)
    add_executable(platform_file_test test/FileIo.cpp)

    target_link_libraries(platform_file_test PRIVATE platform_file)

    if(MSVC)
        target_compile_options(platform_file_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(platform_file_test PRIVATE -UNDEBUG)
    endif()

    add_test(NAME platform.file_publication COMMAND platform_file_test "${CMAKE_CURRENT_BINARY_DIR}/file-data")
endif()
