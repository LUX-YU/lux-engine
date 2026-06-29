#pragma once
// ============================================================================
//  ShadingModelDescriptor.hpp — Metadata descriptor for a shading model
//
//  Phase 3 (A-04): Enables data-driven material registration.
//  Built-in materials have their descriptors auto-generated from compile-time
//  traits; custom materials are described via this struct (e.g. loaded from JSON).
// ============================================================================
#include <lux/engine/render/core/MaterialFamily.hpp>
#include <lux/engine/render/pipeline/ShaderPermutation.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lux::render
{
    // ===== Parameter slot descriptor =====
    // Describes a named parameter within the family struct's param area.
    struct LUX_FUNCTION_PUBLIC ParamSlotDescriptor
    {
        std::string name;          // e.g. "roughness", "kd", "base_color"
        uint32_t    offset{0};     // byte offset within the param area
        uint32_t    size{4};       // size in bytes (4=float, 16=vec4, etc.)
    };

    // ===== Texture slot descriptor =====
    // Describes a texture slot within the family struct's tex[] array.
    struct LUX_FUNCTION_PUBLIC TextureSlotDescriptor
    {
        std::string name;          // e.g. "BaseColor", "Normal", "Emissive"
        uint32_t    slot_index{0}; // index into the family GPU struct's tex[] array
        bool        required{false};
    };

    // ===== Shading Model Descriptor =====
    // Complete metadata for a material shading model — sufficient to:
    //   1. Register the model in the ShadingModelRegistry
    //   2. Convert CPU parameters → family unified GPU struct
    //   3. Select the correct fragment shader / pipeline
    //   4. Validate parameter layouts at registration time
    struct LUX_FUNCTION_PUBLIC ShadingModelDescriptor
    {
        // --- Identity ---
        std::string     name;            // e.g. "Phong", "MyToonVariant"
        EShadingModel   shading_model{EShadingModel::INVALID};
        ELightingTechnique family{ELightingTechnique::Unlit};

        // --- Shader feature requirements ---
        ShaderFeatureMask required_features{0};  // always-on features
        ShaderFeatureMask optional_features{0};  // conditionally enabled features

        // --- Parameter layout (maps named params to offsets in family struct) ---
        std::vector<ParamSlotDescriptor>    params;

        // --- Texture slot layout ---
        std::vector<TextureSlotDescriptor>  texture_slots;

        // NOTE: shader selection is NOT owned by the registry. The frag shader is
        // chosen by resolveFragmentForFamily() (builtin families) or per-scene
        // Config overrides (Graph family). The registry owns metadata only:
        // feature masks + param/texture layout.

        // --- Layout validation hash ---
        // Computed from the param+texture layout. Used to detect mismatches
        // between the registering code and the GPU struct layout.
        uint64_t layout_hash{0};

        // --- Built-in flag ---
        // True for the 12 engine-defined shading models.
        bool is_builtin{false};
    };

} // namespace lux::render
