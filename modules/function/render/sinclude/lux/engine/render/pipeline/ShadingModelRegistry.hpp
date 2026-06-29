#pragma once
// ============================================================================
//  ShadingModelRegistry.hpp — Central registry for all shading models
//
//  Phase 3 (A-04): Manages both built-in and runtime-registered shading models.
//  Provides lookup by EShadingModel, family queries, and iteration.
//
//  Thread safety: NOT thread-safe. Registration should happen during init;
//  lookups during rendering are read-only.
// ============================================================================
#include <lux/engine/render/pipeline/ShadingModelDescriptor.hpp>
#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/function/visibility.h>

#include <lux/cxx/container/SparseSet.hpp>

#include <cstdint>
#include <functional>

namespace lux::render
{
    inline constexpr uint32_t kBuiltinShadingModelCount = 5;

    class LUX_FUNCTION_PUBLIC ShadingModelRegistry
    {
    public:
        // Constructor registers all 12 built-in shading models.
        ShadingModelRegistry();

        // ----- Runtime registration -----
        // Register a new custom shading model.  The descriptor's shading_model
        // field may be INVALID — in which case an ID is auto-assigned from the
        // custom range of the specified family.
        // Returns the assigned EShadingModel ID, or an error if registration fails
        // (e.g. duplicate name, param overflow, etc.).
        Expected<EShadingModel> registerShadingModel(ShadingModelDescriptor desc);

        // ----- Query -----
        [[nodiscard]] bool isRegistered(EShadingModel model) const noexcept;
        [[nodiscard]] const ShadingModelDescriptor* getDescriptor(EShadingModel model) const noexcept;
        [[nodiscard]] ELightingTechnique getFamily(EShadingModel model) const noexcept;
        [[nodiscard]] ShaderFeatureMask getRequiredFeatures(EShadingModel model) const noexcept;
        [[nodiscard]] ShaderFeatureMask getOptionalFeatures(EShadingModel model) const noexcept;

        // ----- Iteration -----
        void forEachModel(std::function<void(EShadingModel, const ShadingModelDescriptor&)> fn) const;
        void forEachInFamily(ELightingTechnique family,
                             std::function<void(EShadingModel, const ShadingModelDescriptor&)> fn) const;

        // ----- Statistics -----
        [[nodiscard]] uint32_t totalModelCount() const noexcept
        { return static_cast<uint32_t>(descriptors_.size()); }

        [[nodiscard]] uint32_t builtinModelCount() const noexcept
        { return kBuiltinShadingModelCount; }

        [[nodiscard]] uint32_t customModelCount() const noexcept
        { return totalModelCount() - builtinModelCount(); }

    private:
        void registerBuiltin(EShadingModel model, const char* name,
                             ELightingTechnique family,
                             ShaderFeatureMask required_features,
                             ShaderFeatureMask optional_features);

        lux::cxx::SparseSet<uint16_t, ShadingModelDescriptor> descriptors_;

        // Next auto-assigned custom ID per family.
        //   Unlit:      50 .. 99
        //   LegacyLit:  150 .. 199
        //   PbrMetallicRoughness: 250 .. 299
        //   Stylized:   350 .. 399
        //   Graph:      450 .. 499
        uint16_t next_custom_id_[kLightingTechniqueCount] = {50, 150, 250, 350, 450};
    };

} // namespace lux::render
