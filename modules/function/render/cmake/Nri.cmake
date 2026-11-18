include_guard(GLOBAL)

if(CMAKE_VERSION VERSION_LESS 3.30)
    message(FATAL_ERROR "Render's pinned NRI dependency requires CMake 3.30 or newer")
endif()

# NRI is maintained in an independent fork, alongside the lux-* repositories.
# bootstrap/lux-manifest.json is the single authority for its URL and commit.
find_package(Git REQUIRED)
get_filename_component(lux_render_repository "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(nri_manifest "${lux_render_repository}/bootstrap/lux-manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nri_manifest}")
file(READ "${nri_manifest}" nri_manifest_json)
string(JSON nri_repo_count LENGTH "${nri_manifest_json}" repos)
math(EXPR nri_repo_last "${nri_repo_count} - 1")
set(nri_commit "")
foreach(index RANGE ${nri_repo_last})
    string(JSON repository_name GET "${nri_manifest_json}" repos ${index} name)
    if(repository_name STREQUAL "NRI")
        string(JSON nri_repository GET "${nri_manifest_json}" repos ${index} url)
        string(JSON nri_commit GET "${nri_manifest_json}" repos ${index} revision)
        break()
    endif()
endforeach()

if(nri_commit STREQUAL "")
    message(FATAL_ERROR "bootstrap/lux-manifest.json must pin the external NRI fork")
endif()

get_filename_component(default_nri_source "${lux_render_repository}/../NRI" ABSOLUTE)
set(LUX_NRI_SOURCE_DIR "${default_nri_source}" CACHE PATH "External Lux NRI fork checkout")
if(NOT EXISTS "${LUX_NRI_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Clone ${nri_repository} at ${nri_commit} outside lux-engine and set LUX_NRI_SOURCE_DIR to that checkout"
    )
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${LUX_NRI_SOURCE_DIR}" rev-parse HEAD
    RESULT_VARIABLE nri_revision_result
    OUTPUT_VARIABLE nri_actual_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT nri_revision_result EQUAL 0 OR NOT nri_actual_commit STREQUAL nri_commit)
    message(FATAL_ERROR "NRI fork must be at ${nri_commit}; found '${nri_actual_commit}'. Existing checkouts are not reset.")
endif()

set(FETCHCONTENT_BASE_DIR "${LUX_RENDER_DEPENDENCY_CACHE}/nri-v192-content")

set(NRI_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRI_SHADERS_PATH "${CMAKE_BINARY_DIR}/nri/shaders" CACHE STRING "NRI generated shaders outside source" FORCE)
set(NRI_ENABLE_VK_SUPPORT ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D12_SUPPORT ${WIN32} CACHE BOOL "" FORCE)
set(NRI_AGILITY_SDK_VERSION_MAJOR "620" CACHE STRING "Pinned NRI v192 Agility SDK" FORCE)
set(NRI_AGILITY_SDK_VERSION_MINOR "2" CACHE STRING "Pinned NRI v192 Agility SDK" FORCE)
set(NRI_ENABLE_VALIDATION_SUPPORT ON CACHE BOOL "" FORCE)
foreach(option NONE D3D11 WGPU METAL NVTX)
    set(NRI_ENABLE_${option}_SUPPORT OFF CACHE BOOL "" FORCE)
endforeach()
foreach(option NIS NGX FFX XESS)
    set(NRI_ENABLE_${option}_SDK OFF CACHE BOOL "" FORCE)
endforeach()
set(NRI_ENABLE_IMGUI_EXTENSION OFF CACHE BOOL "" FORCE)

add_subdirectory("${LUX_NRI_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/nri")

set(LUX_NRI_VULKAN_HEADERS "${FETCHCONTENT_BASE_DIR}/vulkan_headers-src/include")
set(LUX_NRI_VMA_HEADERS "${FETCHCONTENT_BASE_DIR}/vma-src/include")
install(FILES "${LUX_NRI_SOURCE_DIR}/LICENSE.txt"
    DESTINATION share/lux-engine-render/licenses
    RENAME NRI.txt
    COMPONENT lux_engine_render
)

install(FILES "${FETCHCONTENT_BASE_DIR}/vma-src/LICENSE.txt"
    DESTINATION share/lux-engine-render/licenses
    RENAME VMA.txt
    COMPONENT lux_engine_render
)

install(FILES "${FETCHCONTENT_BASE_DIR}/vulkan_headers-src/LICENSE.md"
    DESTINATION share/lux-engine-render/licenses
    RENAME Vulkan-Headers.md
    COMPONENT lux_engine_render
)

if(WIN32)
    install(FILES "${FETCHCONTENT_BASE_DIR}/d3d12ma-src/LICENSE.txt"
        DESTINATION share/lux-engine-render/licenses
        RENAME D3D12MA.txt
        COMPONENT lux_engine_render
    )

    install(FILES "${FETCHCONTENT_BASE_DIR}/amdags-src/ags_lib/LICENSE.txt"
        DESTINATION share/lux-engine-render/licenses
        RENAME AMD-AGS.txt
        COMPONENT lux_engine_render
    )
endif()
