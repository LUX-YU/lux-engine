include_guard(GLOBAL)

if(CMAKE_VERSION VERSION_LESS 3.30)
    message(FATAL_ERROR "Render's pinned NRI dependency requires CMake 3.30 or newer")
endif()

find_package(Git REQUIRED)
include(FetchContent)

get_filename_component(default_dependency_cache "${CMAKE_CURRENT_LIST_DIR}/../../../../../build/dependencies" ABSOLUTE)
set(LUX_RENDER_DEPENDENCY_CACHE "${default_dependency_cache}" CACHE PATH "Shared Render dependency source/download cache")
set(LUX_NRI_SOURCE_DIR "${LUX_RENDER_DEPENDENCY_CACHE}/nri-v180" CACHE PATH "Pinned, patched NRI source checkout")
set(FETCHCONTENT_BASE_DIR "${LUX_RENDER_DEPENDENCY_CACHE}/nri-content")
set(nri_commit "4b485316463969f182db15e67aad2aec2f40a3d7")

if(NOT EXISTS "${LUX_NRI_SOURCE_DIR}/CMakeLists.txt")
    FetchContent_Declare(lux_render_nri_source
        GIT_REPOSITORY https://github.com/NVIDIA-RTX/NRI.git
        GIT_TAG v180
        GIT_SHALLOW TRUE
        SOURCE_DIR "${LUX_NRI_SOURCE_DIR}"
        SOURCE_SUBDIR source_checkout_only
    )
    FetchContent_MakeAvailable(lux_render_nri_source)
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${LUX_NRI_SOURCE_DIR}" rev-parse HEAD
    RESULT_VARIABLE result OUTPUT_VARIABLE actual_commit OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT result EQUAL 0 OR NOT actual_commit STREQUAL nri_commit)
    message(FATAL_ERROR "NRI source must be exactly ${nri_commit}; found '${actual_commit}'")
endif()

set(nri_patch "${CMAKE_CURRENT_LIST_DIR}/nri-v180.patch")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${LUX_NRI_SOURCE_DIR}" apply --reverse --check "${nri_patch}"
    RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET
)
if(NOT already_applied EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${LUX_NRI_SOURCE_DIR}" apply --check "${nri_patch}"
        RESULT_VARIABLE applicable OUTPUT_QUIET ERROR_VARIABLE patch_error
    )
    if(NOT applicable EQUAL 0)
        message(FATAL_ERROR "NRI checkout does not match the pinned patch: ${patch_error}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${LUX_NRI_SOURCE_DIR}" apply "${nri_patch}"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

set(NRI_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_VK_SUPPORT ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_VALIDATION_SUPPORT ON CACHE BOOL "" FORCE)
foreach(option NONE D3D11 D3D12 WGPU NVTX)
    set(NRI_ENABLE_${option}_SUPPORT OFF CACHE BOOL "" FORCE)
endforeach()
foreach(option NIS NGX FFX XESS)
    set(NRI_ENABLE_${option}_SDK OFF CACHE BOOL "" FORCE)
endforeach()
set(NRI_ENABLE_IMGUI_EXTENSION OFF CACHE BOOL "" FORCE)

add_subdirectory("${LUX_NRI_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/nri")

# Vulkan/VMA implementation headers stay private. Only locked NRI declarations are installed
# for Render extensions; the virtual allocator is already implemented by the single NRI library.
set(LUX_NRI_VULKAN_HEADERS "${FETCHCONTENT_BASE_DIR}/vulkan_headers-src/include")
set(LUX_NRI_VMA_HEADERS "${FETCHCONTENT_BASE_DIR}/vma-src/include")
install(FILES "${LUX_NRI_SOURCE_DIR}/LICENSE.txt" DESTINATION share/lux-engine-render/licenses RENAME NRI.txt)
