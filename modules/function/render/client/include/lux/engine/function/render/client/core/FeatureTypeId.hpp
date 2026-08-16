#pragma once
/**
 * @file FeatureTypeId.hpp
 * @brief Stable, registration-order-independent feature type identity.
 *
 * The comm layer's `feature_type_id` (RenderServer feature_types registry) is a
 * DYNAMIC insert index — fine for routing a RegisterFeatureType reply back to an
 * addFeature call, but useless for declaring static relationships (a feature's
 * dependencies / conflicts must name *types*, not whatever index they happened to
 * get registered at). FeatureTypeId is that stable name: a 64-bit FNV-1a hash of a
 * dotted, versioned identifier string, computed at compile time.
 *
 *   inline constexpr FeatureTypeId kShadowMap = featureId("lux.render.shadow_map.v1");
 *
 * Stability rules: pick a unique dotted name per feature type and bump the `.vN`
 * suffix only on an ABI-breaking descriptor change. Never reuse a retired name.
 */

#include <cstdint>
#include <string_view>

#include <lux/cxx/algorithm/hash.hpp>   // lux::cxx::algorithm::fnv1a (shared 64-bit FNV-1a)

namespace lux::render
{
    using FeatureTypeId = std::uint64_t;

    /// Compile-time stable id for a feature type's dotted name. Delegates to the
    /// engine-wide 64-bit FNV-1a (lux::cxx::algorithm::fnv1a) used by RGBuilder /
    /// Meta etc., so feature ids share the project's one hashing convention.
    /// Collisions are astronomically unlikely for the small set of curated names.
    [[nodiscard]] constexpr FeatureTypeId featureId(std::string_view name) noexcept
    {
        return lux::cxx::algorithm::fnv1a(name);
    }

    /// The null / unassigned feature type. featureId() of a non-empty name never
    /// returns this in practice (FNV-1a of "" is the offset basis, not 0).
    inline constexpr FeatureTypeId kInvalidFeatureTypeId = 0ull;

} // namespace lux::render
