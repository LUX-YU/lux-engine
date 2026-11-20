#pragma once

#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <lux/engine/render/effect/EffectDiagnostic.hpp>
#include <lux/engine/render/effect/EffectGraph.hpp>
#include <cstdint>
#include <limits>
#include <span>
#include <memory>
#include <string_view>
#include <vector>

namespace lux::render::effect_detail
{
    inline constexpr auto kInvalidEffectIndex = std::numeric_limits<std::uint32_t>::max();

    struct EffectTableRange final
    {
        std::uint32_t first = 0;
        std::uint32_t count = 0;

        [[nodiscard]] bool empty() const noexcept { return count == 0; }

        template <typename T>
        [[nodiscard]] std::span<const T> view(const std::vector<T>& entries) const noexcept
        {
            return std::span<const T>{entries}.subspan(first, count);
        }
    };

    struct ValidatedEffectPin final
    {
        graph::PinId source;
        std::uint32_t node = kInvalidEffectIndex;
        std::uint32_t declaration = kInvalidEffectIndex;
        EffectTableRange sources; // Dense output-pin indexes in input_sources.
        bool demanded = false;
    };

    struct ValidatedEffectNode final
    {
        graph::NodeId source;
        std::shared_ptr<const CapabilityDefinition> capability;
        std::shared_ptr<const EffectImplementation> implementation;
        // Borrows the source EffectGraph owner during initialization/lowering only.
        std::string_view arguments;
        EffectTableRange pins;
        bool live = false;
    };

    struct ValidatedEffectOutput final
    {
        // Same bounded source-owner borrow as arguments; no runtime owner is created.
        std::string_view name;
        std::uint32_t pin = kInvalidEffectIndex;
        bool target = false;
    };

    // Compile scratch only. This is neither the published Effect nor a second executable graph.
    // Node order is stable NodeId order; pin order is capability declaration order.
    struct ValidatedEffectGraph final
    {
        std::vector<ValidatedEffectNode> nodes;
        std::vector<ValidatedEffectPin> pins;
        std::vector<std::uint32_t> input_sources;
        std::vector<std::uint32_t> topological_order;
        std::vector<ValidatedEffectOutput> outputs;
    };

    using EffectValidationResult = cxx::expected<ValidatedEffectGraph, EffectDiagnostic>;

    [[nodiscard]] LUX_RENDER_PUBLIC EffectValidationResult
    validateEffectGraph(const EffectGraph&, const EffectCatalog&) noexcept;
}
