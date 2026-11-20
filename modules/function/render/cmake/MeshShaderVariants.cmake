function(lux_render_mesh_shaders outputs)
    # Layout, output stem, and the corresponding declaration of mesh vertex attributes.
    set(layouts
        "DEPTH_ONLY|DepthOnly|-DDEPTH_ONLY=1"
        "BASIC|BasicMesh|"
        "BASIC_NO_UV|BasicMeshNoUv|-DVERTEX_NO_UV=1"
        "SURFACE|SurfaceMesh|-DVERTEX_NORMALS=1"
        "SURFACE_NO_UV|SurfaceMeshNoUv|-DVERTEX_NORMALS=1|-DVERTEX_NO_UV=1"
        "TANGENT|TangentMesh|-DVERTEX_NORMALS=1|-DVERTEX_TANGENTS=1"
        "TANGENT_NO_UV|TangentMeshNoUv|-DVERTEX_NORMALS=1|-DVERTEX_TANGENTS=1|-DVERTEX_NO_UV=1"
    )
    set(source shaders/mesh/IndexedTriangleMeshVertex.slang)
    set(headers)
    set(spirv_includes "#pragma once\n#include <array>\n")
    set(spirv_entries)
    set(layout_enum)
    set(layout_keys)
    set(output_stems)
    foreach(layout IN LISTS layouts)
        string(REPLACE "|" ";" fields "${layout}")
        list(POP_FRONT fields key stem)
        if(key IN_LIST layout_keys OR stem IN_LIST output_stems)
            message(FATAL_ERROR "Duplicate mesh vertex shader layout: ${layout}")
        endif()
        list(APPEND layout_keys "${key}")
        list(APPEND output_stems "${stem}")
        string(APPEND layout_enum "    ${key},\n")
        foreach(paged 0 1)
            foreach(shadow 0 1)
                foreach(reflected 0 1)
                    set(variant "${stem}")
                    if(paged)
                        string(APPEND variant "Paged")
                    endif()
                    lux_mesh_shader_mode("${variant}" ${shadow} ${reflected} variant mode_defines)
                    set(defines ${mode_defines} ${fields})
                    if(paged)
                        list(APPEND defines -DPAGED_INSTANCES=1)
                    endif()
                    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${variant}Vertex.hpp")
                    lux_render_shader("${header}" "${source}" vertex k${variant}Vertex ${defines} --spirv-only)
                    list(APPEND headers "${header}")
                    string(APPEND spirv_includes "#include \"${variant}Vertex.hpp\"\n")
                    string(APPEND spirv_entries "    k${variant}Vertex,\n")
                endforeach()
            endforeach()
        endforeach()
    endforeach()

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/MeshVertexSpirv.hpp" CONTENT
        "${spirv_includes}
namespace lux::render::detail
{
enum class EMeshVertexLayout
{
${layout_enum}    COUNT
};

struct MeshVertexKey final
{
    EMeshVertexLayout layout;
    bool paged;
    bool shadow;
    bool reflected;
};

inline constexpr std::array kMeshVertexSpirv{
${spirv_entries}};
static_assert(kMeshVertexSpirv.size() == static_cast<std::size_t>(EMeshVertexLayout::COUNT) * 8);

[[nodiscard]] inline constexpr const auto& meshVertexSpirv(MeshVertexKey key) noexcept
{
    const auto index = static_cast<std::size_t>(key.layout) * 8 + std::size_t(key.paged) * 4 +
        std::size_t(key.shadow) * 2 + std::size_t(key.reflected);
    return kMeshVertexSpirv[index];
}
}
"
    )

    # DXIL requires matching registers for independently compiled vertex and fragment stages.
    # Bits below are derived from the ordered TEXCOORD semantics emitted by the material compiler.
    set(dxil_locations 0 1 3 4 5 6)
    set(normal_bits 0)
    set(tangent_bits 0)
    set(uv_bits 0)
    set(bit 1)
    foreach(location IN LISTS dxil_locations)
        if(location EQUAL 1 OR location EQUAL 5 OR location EQUAL 6)
            math(EXPR normal_bits "${normal_bits} | ${bit}")
        endif()
        if(location EQUAL 5 OR location EQUAL 6)
            math(EXPR tangent_bits "${tangent_bits} | ${bit}")
        endif()
        if(location EQUAL 3)
            set(uv_bits ${bit})
        endif()
        math(EXPR bit "${bit} << 1")
    endforeach()
    math(EXPR max_mask "${bit} - 1")
    set(dxil_includes "#pragma once\n#include <array>\n#include <cstdint>\n")
    set(dxil_entries)
    foreach(varyings RANGE 0 ${max_mask})
        set(attributes -DMESH_VARYINGS=${varyings})
        math(EXPR normal "${varyings} & ${normal_bits}")
        math(EXPR tangent "${varyings} & ${tangent_bits}")
        math(EXPR uv "${varyings} & ${uv_bits}")
        if(varyings EQUAL 0)
            list(APPEND attributes -DDEPTH_ONLY=1)
        endif()
        if(normal)
            list(APPEND attributes -DVERTEX_NORMALS=1)
        endif()
        if(tangent)
            list(APPEND attributes -DVERTEX_TANGENTS=1)
        endif()
        if(NOT uv)
            list(APPEND attributes -DVERTEX_NO_UV=1)
        endif()
        foreach(shadow 0 1)
            foreach(reflected 0 1)
                math(EXPR index "4 * ${varyings} + 2 * ${shadow} + ${reflected}")
                set(stem "MeshVertexDxil${index}")
                set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${stem}.hpp")
                lux_mesh_shader_mode("" ${shadow} ${reflected} unused mode_defines)
                lux_render_shader("${header}" "${source}" vertex k${stem} ${attributes} ${mode_defines} --dxil-only)
                list(APPEND headers "${header}")
                string(APPEND dxil_includes "#include \"${stem}.hpp\"\n")
                string(APPEND dxil_entries "    k${stem},\n")
            endforeach()
        endforeach()
    endforeach()
    list(JOIN dxil_locations "u, " dxil_location_values)
    list(LENGTH dxil_locations dxil_location_count)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/MeshVertexDxil.hpp" CONTENT
        "${dxil_includes}
namespace lux::render::detail
{
inline constexpr std::array<std::uint32_t, ${dxil_location_count}> kMeshDxilVaryingLocations{${dxil_location_values}u};
inline constexpr std::array kMeshVertexDxil{
${dxil_entries}};
static_assert(kMeshVertexDxil.size() == (std::size_t{1} << kMeshDxilVaryingLocations.size()) * 4);
}
"
    )
    set(${outputs} ${headers} PARENT_SCOPE)
endfunction()

function(lux_mesh_shader_mode stem shadow reflected variant defines)
    set(options)
    if(shadow)
        string(APPEND stem "Shadow")
        list(APPEND options -DSHADOW_PASS=1)
    endif()
    if(reflected)
        string(APPEND stem "Reflected")
        list(APPEND options -DREFLECTED_INSTANCES=1)
    endif()
    set(${variant} "${stem}" PARENT_SCOPE)
    set(${defines} ${options} PARENT_SCOPE)
endfunction()
