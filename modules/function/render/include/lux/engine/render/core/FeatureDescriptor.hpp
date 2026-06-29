#pragma once
/**
 * @file FeatureDescriptor.hpp
 * @brief Static, type-level metadata for a render feature.
 *
 * Promotes the relationships that today live only in comments + add-order (draft
 * §5.1) into declared data: a feature's stable type, its dependencies, the types
 * it conflicts with, and capability flags. Lives at the TYPE level (carried by the
 * FeatureFactory), because the FeatureManager must resolve dependencies BEFORE any
 * instance exists.
 *
 * 阶段 3 introduces this as data; the dependency/conflict RESOLUTION that consumes
 * it (topological install order, conflict rejection, transactional rollback) lands
 * in the SceneFeatureManager — see .internal/UNFINISHED-WORK.md §2bis (slices 3c).
 * A descriptor with empty deps/conflicts (the makeSimpleFactory default) preserves
 * today's "caller-ordered, no validation" behaviour exactly.
 */

#include <lux/engine/render/core/FeatureTypeId.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace lux::render
{
    /// One declared dependency of a feature on another feature type.
    struct FeatureDependency
    {
        FeatureTypeId type{kInvalidFeatureTypeId};
        /// Optional dependency: if absent the dependent still installs (it adapts);
        /// a required (optional=false) dependency that cannot be resolved fails the install.
        bool optional{false};
    };

    /// Static, type-level descriptor. Spans point at storage with static lifetime
    /// (a feature's own `static constexpr` arrays), so the descriptor is trivially
    /// copyable and safe to embed in the (trivially-copyable) FeatureFactory.
    struct FeatureDescriptor
    {
        FeatureTypeId    type{kInvalidFeatureTypeId};
        std::string_view name{};
        std::uint32_t    abi_version{1};

        std::span<const FeatureDependency> dependencies{};
        std::span<const FeatureTypeId>     conflicts{};

        /// Declares passes into the render graph (→ feature-set change invalidates it).
        bool contributes_graph{false};
        /// Needs per-view state (allocate/deallocateViewState) tracked by the manager.
        bool creates_view_state{false};
        /// May be toggled at runtime; when false the manager rejects setEnabled(false).
        bool supports_runtime_disable{true};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return type != kInvalidFeatureTypeId;
        }
    };

} // namespace lux::render
