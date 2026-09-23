include_guard(GLOBAL)

# Classification is build metadata, independent of whether a component is a DLL.
function(lux_classify_target)
    cmake_parse_arguments(C "" "TARGET;LAYER;PRODUCT;ROLE" "" ${ARGN})
    if(NOT TARGET "${C_TARGET}" OR NOT C_PRODUCT OR NOT C_ROLE)
        message(FATAL_ERROR "lux_classify_target requires TARGET, LAYER, PRODUCT and ROLE")
    endif()
    set(layers MODULES DOMAIN WORLD SIMULATION PROCESS SCENE TOOLCHAIN EDITOR)
    if(NOT C_LAYER IN_LIST layers)
        message(FATAL_ERROR "Invalid architecture layer '${C_LAYER}' for ${C_TARGET}")
    endif()
    set_target_properties(${C_TARGET} PROPERTIES LUX_ARCH_LAYER "${C_LAYER}"
        LUX_PRODUCT "${C_PRODUCT}" LUX_ROLE "${C_ROLE}")
endfunction()
