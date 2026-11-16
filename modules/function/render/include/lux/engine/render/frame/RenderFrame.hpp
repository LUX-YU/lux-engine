#pragma once

#include <cstddef>
#include <cstdint>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/capabilities/CapabilityDeclaration.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/RenderScope.hpp>
#include <span>
#include <string>

namespace lux::render
{
    template <class T> struct FrameDataRange final
    {
        std::uint32_t block = 0;
        std::uint64_t offset = 0;
        std::uint64_t count = 0;
    };

    // The public PerFrameData tuple ordinal of a CreateEffect capability occurrence.
    // No private input index or returned binding directory is required.
    struct FrameDataDestination final
    {
        EffectId effect;
        std::uint32_t capability = 0;
        std::uint32_t input = 0;
        VRenderScope scope;
        bool operator==(const FrameDataDestination&) const noexcept = default;
    };

    struct ResourceFrameDestination final
    {
        ResourceInstanceId resource;
        std::uint32_t input = 0;
        bool operator==(const ResourceFrameDestination&) const noexcept = default;
    };

    struct ViewFrameDestination final
    {
        ViewRequest view;
        bool operator==(const ViewFrameDestination&) const noexcept = default;
    };

    using VFrameDataDestination = std::variant<FrameDataDestination, ResourceFrameDestination, ViewFrameDestination>;

    template <class T> struct FrameDataBinding final
    {
        VFrameDataDestination destination;
    };

    // Assemble one known capability occurrence without a query or returned endpoint directory.
    // A missing input type is a declaration error diagnosed in the calling module at compile time.
    template <CapabilityConcept Capability, class T>
    [[nodiscard]] FrameDataBinding<T>
    frameDataBinding(EffectId effect, std::uint32_t capability, VRenderScope scope) noexcept
    {
        constexpr auto types =
            capability_detail::describeTypes(std::type_identity<typename Capability::PerFrameData>{});
        constexpr auto input = []() consteval
        {
            constexpr auto declared =
                capability_detail::describeTypes(std::type_identity<typename Capability::PerFrameData>{});
            for (std::uint32_t i = 0; i < declared.size(); ++i)
            {
                if (declared[i] == cxx::make_basic_type_info<T>())
                {
                    return i;
                }
            }
            return static_cast<std::uint32_t>(declared.size());
        }();
        static_assert(input < types.size(), "Input must be declared by Capability::PerFrameData");
        return {FrameDataDestination{effect, capability, input, std::move(scope)}};
    }

    template <class Resource, class T>
    [[nodiscard]] FrameDataBinding<T> resourceFrameDataBinding(ResourceInstanceId resource) noexcept
    {
        constexpr auto input = []() consteval
        {
            constexpr auto declared =
                capability_detail::describeTypes(std::type_identity<typename Resource::PerFrameData>{});
            for (std::uint32_t i = 0; i < declared.size(); ++i)
            {
                if (declared[i] == cxx::make_basic_type_info<T>()) return i;
            }
            return UINT32_MAX;
        }();
        static_assert(input != UINT32_MAX, "Input must be declared by Resource::PerFrameData");
        return {ResourceFrameDestination{resource, input}};
    }

    struct FrameEntry final
    {
        VFrameDataDestination destination;
        FrameDataRange<std::byte> payload;
    };

    // Entries in one phase must be independent. Phases themselves execute in the written order.
    struct FramePhase final
    {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    // Immutable CPU borrow through the one SubmitFrame completion. No per-entry IPC request,
    // callback, response, queue, resource owner or connected OperationState is stored here.
    struct RenderFrame final
    {
        std::uint64_t sequence = 0;
        std::span<const FrameEntry> entries;
        std::span<const FramePhase> phases;
        std::span<const std::span<const std::byte>> blocks;
        bool resynchronize = false;
    };
} // namespace lux::render
