#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/Resource.hpp>
#include <lux/engine/render/visibility.h>
#include <array>
#include <concepts>
#include <span>
#include <memory>
#include <tuple>
#include <type_traits>
#include <vector>

namespace lux::render
{
    // Semantic metadata only. No client payload address, byte offset, capacity or Storage pointer.
    struct FrameChannel final
    {
        ResourceName resource;
        cxx::basic_type_info declaration;
        cxx::basic_type_info element;
        std::size_t size = 0;
        std::size_t alignment = 0;

        bool operator==(const FrameChannel&) const noexcept = default;
    };

    class LUX_RENDER_PUBLIC FrameLayout final
    {
    public:
        FrameLayout() noexcept = default;

        // revision belongs to the existing execution owner, not a global layout registry.
        // Identical declarations are merged; conflicting declaration/type metadata is rejected.
        [[nodiscard]] static RenderResult<FrameLayout>
        create(std::span<const FrameChannel> channels, std::uint64_t revision) noexcept;

        [[nodiscard]] std::span<const FrameChannel> channels() const noexcept
        {
            return channels_ ? std::span<const FrameChannel>{*channels_} : std::span<const FrameChannel>{};
        }
        [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
        [[nodiscard]] bool valid() const noexcept { return channels_ && revision_ != 0; }
        [[nodiscard]] bool hasSameChannels(const FrameLayout&) const noexcept;
        [[nodiscard]] bool operator==(const FrameLayout&) const noexcept;
        // Copies from an execution query retain this immutable identity; no global registry is involved.
        [[nodiscard]] bool hasSameIdentity(const FrameLayout& other) const noexcept
        {
            return channels_ && channels_ == other.channels_ && revision_ == other.revision_;
        }
        [[nodiscard]] std::size_t find(cxx::basic_type_info declaration, cxx::basic_type_info element) const noexcept;

    private:
        std::shared_ptr<const std::vector<FrameChannel>> channels_;
        std::uint64_t revision_ = 0;
    };

    namespace frame_detail
    {
        template <class Resource, class T> inline constexpr bool declaredElement =
            []<class... Elements>(std::type_identity<std::tuple<Elements...>>) consteval
            {
                return (std::same_as<std::remove_cvref_t<T>, Elements> || ...);
            }(std::type_identity<typename Resource::PerFrameData>{});

        template <class T> concept FrameElement =
            std::same_as<T, std::remove_cvref_t<T>> && !std::same_as<T, bool> &&
            std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> &&
            std::is_nothrow_default_constructible_v<T>;

        template <class Resource, class... Elements>
        void appendChannels(std::vector<FrameChannel>& output, std::type_identity<std::tuple<Elements...>>) noexcept
        {
            static_assert((FrameElement<Elements> && ...), "Frame elements require a contiguous trivial value type");
            constexpr std::array<cxx::basic_type_info, sizeof...(Elements)> types{cxx::make_basic_type_info<Elements>()...};
            static_assert([](const auto& values) consteval
            {
                for (std::size_t i = 0; i < values.size(); ++i)
                    for (std::size_t j = 0; j < i; ++j)
                        if (values[i] == values[j]) return false;
                return true;
            }(types), "Resource::PerFrameData must not repeat an element type");
            (output.push_back(FrameChannel{
                ResourceName{Resource::descriptor.type.name()}, cxx::make_basic_type_info<Resource>(),
                cxx::make_basic_type_info<Elements>(), sizeof(Elements), alignof(Elements)
            }), ...);
        }
    }

    template <class... Resources> [[nodiscard]] RenderResult<FrameLayout>
    makeFrameLayout(std::type_identity<std::tuple<Resources...>>, std::uint64_t revision) noexcept
    {
        std::vector<FrameChannel> channels;
        (frame_detail::appendChannels<Resources>(
            channels, std::type_identity<typename Resources::PerFrameData>{}
        ), ...);
        return FrameLayout::create(channels, revision);
    }
} // namespace lux::render
