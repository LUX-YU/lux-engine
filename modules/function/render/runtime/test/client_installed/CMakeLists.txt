cmake_minimum_required(VERSION 3.22)
project(render-client-installed-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /GR- /we4541)
endif()

# This configure must succeed without even finding the Server's Native dependency.
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan ON)
set(CMAKE_DISABLE_FIND_PACKAGE_stdexec ON)

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_commands)
find_package(lux-engine-platform CONFIG REQUIRED COMPONENTS platform_ipc)

get_target_property(client_links lux::engine::render::render_commands INTERFACE_LINK_LIBRARIES)

set(expected_client_links
    lux::engine::render::render_core
    lux::engine::render::render_data
    lux::engine::render::render_protocol
)

if(NOT "${client_links}" STREQUAL "${expected_client_links}")
    message(FATAL_ERROR "Unexpected Client dependency contract: ${client_links}")
endif()

get_property(imported_targets DIRECTORY PROPERTY IMPORTED_TARGETS)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/client-dependency-closure.txt" "")

foreach(imported IN LISTS imported_targets)
    string(TOLOWER "${imported}" imported_lower)
    if(imported_lower MATCHES "vulkan|vma|gapi|spirv|shaderc|render_native|render_runtime|render_features|render_graph|core_execution")
        message(FATAL_ERROR "Native/Server target imported by pure Client: ${imported}")
    endif()

    foreach(property INTERFACE_LINK_LIBRARIES INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_DEFINITIONS IMPORTED_LOCATION IMPORTED_IMPLIB
        IMPORTED_LOCATION_DEBUG IMPORTED_LOCATION_RELWITHDEBINFO IMPORTED_IMPLIB_DEBUG IMPORTED_IMPLIB_RELWITHDEBINFO
    )
        get_target_property(value "${imported}" "${property}")
        if(value)
            string(TOLOWER "${value}" value_lower)
            string(REPLACE "\\" "/" value_lower "${value_lower}")
            if(value_lower MATCHES "vulkan|vk_mem_alloc|spirv|shaderc|render_native|render_runtime|/pinclude/|/sinclude/")
                message(FATAL_ERROR "Native/private dependency leaked through ${imported} ${property}: ${value}")
            endif()
            file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/client-dependency-closure.txt" "${imported} ${property}=${value}\n")
        endif()
    endforeach()
endforeach()

get_filename_component(installed_prefix "${lux-engine-render_DIR}/../.." ABSOLUTE)

# The fresh, relocated SDK must contain the new contracts, not forwarding aliases.
foreach(header
    lux/engine/render/data/Payload.hpp
    lux/engine/render/protocol/AsyncCommand.hpp
    lux/engine/render/features/Registration.hpp
    lux/engine/render/resource/AsyncWork.hpp
    lux/engine/render/resource/Upload.hpp
    lux/engine/render/runtime/Commands.hpp
    lux/engine/render/runtime/Error.hpp
    lux/engine/render/native/vulkan/Readback.hpp
    lux/engine/render/native/vulkan/texture/TextureCommands.hpp
)
    if(NOT EXISTS "${installed_prefix}/include/${header}")
        message(FATAL_ERROR "Missing current public contract: ${header}")
    endif()
endforeach()

foreach(header
    lux/engine/render/runtime/CommandValue.hpp
    lux/engine/render/runtime/RenderClient.hpp
    lux/engine/render/resource/Registration.hpp
    lux/engine/render/native/vulkan/Registration.hpp
    lux/engine/render/runtime/Operations.hpp
    lux/engine/render/runtime/vulkan/GpuCompletion.hpp
    lux/engine/render/native/vulkan/transfer/GpuCompletion.hpp
    lux/engine/render/native/vulkan/transfer/TextureUpload.hpp
    lux/engine/render/native/vulkan/CommandCompletion.hpp
    lux/engine/platform/ipc/detail/MessageAccess.hpp
    lux/engine/render/graph/detail/Range.hpp
    lux/engine/render/runtime/Deferred.hpp
    lux/engine/render/runtime/TypedProtocol.hpp
    lux/engine/render/runtime/Request.hpp
    Deferred.hpp
    TypedProtocol.hpp
    Request.hpp
    Native.hpp
    Shader.hpp
)
    if(EXISTS "${installed_prefix}/include/${header}")
        message(FATAL_ERROR "Obsolete/private header installed: ${header}")
    endif()
endforeach()

add_executable(client_consumer Main.cpp)

target_link_libraries(client_consumer PRIVATE lux::engine::render::render_commands lux::engine::platform::platform_ipc)

add_custom_command(TARGET client_consumer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:lux::engine::platform::platform_ipc>" "$<TARGET_FILE_DIR:client_consumer>"
)

add_test(NAME installed.client_without_vulkan COMMAND client_consumer)

set_tests_properties(installed.client_without_vulkan PROPERTIES TIMEOUT 15)
