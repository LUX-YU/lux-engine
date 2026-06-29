#pragma once
/**
 * @file FeatureRegistration.hpp
 * @brief Public entry for installing an externally-authored RenderFeature into a
 *        scene from a FeatureFactory create_fn (server-side plugin instantiation).
 *
 * A plugin's create_fn (see comm/RenderProtocol.hpp — FeatureFactory /
 * makeSimpleFactory) receives the scene as an opaque `void*`. It cannot name
 * RenderScene (an engine-internal type) to install the feature. This header
 * bridges that gap WITHOUT exposing RenderScene:
 *
 *   - The TEMPLATE part (addFeature<T>) is instantiated in the plugin's own TU:
 *     it registers T's render-object extractor via the PUBLIC RenderObjectExtractor
 *     and constructs the feature.
 *   - The RenderScene-internal part lives out-of-line in the render module
 *     (FeatureRegistration.cpp), reached through the two detail shims below.
 *
 * So an external author writes, in their create_fn:
 *
 *   uint32_t myCreateFn(void* scene, const void* param, size_t param_size) {
 *       MyFeature::Config cfg = decode(param, param_size);
 *       return lux::render::addFeature<MyFeature>(scene, std::move(cfg));
 *   }
 *
 * — using public headers alone.
 */

#include <lux/engine/render/RenderFeature.hpp>                        // RenderFeature (the erased base)
#include <lux/engine/render/renderer/RenderObjectExtraction.hpp>      // RenderObjectExtractor::ensureTypeRegistered<T>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace lux::render
{
    namespace detail
    {
        /// Borrow the scene's render-object extractor (the scene is type-erased as
        /// void* across the factory boundary). Defined in the render module where
        /// RenderScene is complete; uses RenderScene's PUBLIC accessor.
        [[nodiscard]] LUX_FUNCTION_PUBLIC RenderObjectExtractor& sceneObjectExtractor(void* scene) noexcept;

        /// Install an already-constructed feature into the (type-erased) scene.
        /// Forwards to RenderScene::addFeatureErased. Returns the feature id.
        LUX_FUNCTION_PUBLIC uint32_t installFeatureErased(
            void*                          scene,
            std::unique_ptr<RenderFeature> feature,
            uint32_t                       extractor_type_id);
    } // namespace detail

    /**
     * @brief Construct a feature of type @p T and install it into the type-erased
     *        @p scene, returning its assigned feature id.
     *
     * The public entry a plugin's FeatureFactory create_fn calls (the type-erased
     * counterpart of RenderScene::addFeature<T>). @p T must derive from RenderFeature.
     * Any @p args are forwarded to T's constructor.
     */
    template <typename T, typename... Args>
    uint32_t addFeature(void* scene, Args&&... args)
    {
        static_assert(std::is_base_of_v<RenderFeature, T>,
                      "addFeature<T>: T must derive from lux::render::RenderFeature");
        const uint32_t extractor_type_id =
            detail::sceneObjectExtractor(scene).template ensureTypeRegistered<T>();
        return detail::installFeatureErased(
            scene, std::make_unique<T>(std::forward<Args>(args)...), extractor_type_id);
    }

} // namespace lux::render
