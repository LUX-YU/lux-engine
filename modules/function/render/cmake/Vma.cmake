include_guard(GLOBAL)

# Header dependency only. The Native library supplies VMA_IMPLEMENTATION in device/Vma.cpp.
find_path(LUX_VMA_INCLUDE_DIR vk_mem_alloc.h
    HINTS "$ENV{VULKAN_SDK}/Include/vma" "${Vulkan_INCLUDE_DIR}/vma"
    REQUIRED
)

if(NOT TARGET lux_render_vma_headers)
    add_library(lux_render_vma_headers INTERFACE IMPORTED GLOBAL)
    set_target_properties(lux_render_vma_headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LUX_VMA_INCLUDE_DIR}"
    )
endif()
