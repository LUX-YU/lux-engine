#pragma once

#include <lux/engine/render/effect/EffectImplementation.hpp>

#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <lux/engine/render/effect/EffectDiagnostic.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/render/effect/EffectGraph.hpp>
#include <cstdint>
#include <limits>
#include <span>
#include <memory>
#include <string>
#include <lux/engine/render/core/ConfigValue.hpp>
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
        // Borrows the sealed Catalog for the lifetime of the configured Effect.
        const EffectCapability* capability;
        const ImplementationEntry* implementation;
        EffectTableRange pins;
        bool live = false;
        std::unique_ptr<EffectImplementation> configured;
    };

    struct ValidatedEffectOutput final
    {
        std::uint32_t pin = kInvalidEffectIndex;
        bool target = false;
    };

    // The Effect retains this device-independent topology after releasing the editable graph.
    // Node order is stable NodeId order; pin order is capability declaration order.
    struct ValidatedEffectGraph final
    {
        std::vector<ValidatedEffectNode> nodes;
        std::vector<ValidatedEffectPin> pins;
        std::vector<std::uint32_t> input_sources;
        std::vector<std::uint32_t> topological_order;
        std::vector<ValidatedEffectOutput> outputs;
    };

    [[nodiscard]] RenderError reportEffectFailure(
        const RenderErrorSink&, RenderError, const EffectDiagnostic&, std::string_view
    ) noexcept;

    using EffectValidationResult = cxx::expected<ValidatedEffectGraph, EffectDiagnostic>;

    [[nodiscard]] LUX_RENDER_PUBLIC EffectValidationResult
    validateEffectGraph(const EffectGraph&, const EffectCatalog&) noexcept;
}
