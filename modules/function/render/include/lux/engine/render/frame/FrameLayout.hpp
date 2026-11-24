#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/resource/ResourceRequirement.hpp>
#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

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

        [[nodiscard]] bool operator==(const FrameChannel& other) const noexcept
        {
            return resource == other.resource && size == other.size && alignment == other.alignment &&
                declaration == other.declaration && element == other.element &&
                declaration.name() == other.declaration.name() && element.name() == other.element.name();
        }
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
            std::is_nothrow_default_constructible_v<T> && std::constructible_from<T, T&&>;

        template <class T, class U>
        concept FrameAssignableFrom = FrameElement<T> && requires(T& target, U&& source)
        {
            { target = std::forward<U>(source) } noexcept -> std::same_as<T&>;
        };

        template <class Resource, class... Elements>
        std::span<const FrameChannel> channels(std::type_identity<std::tuple<Elements...>>) noexcept
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
            static const std::array<FrameChannel, sizeof...(Elements)> result{FrameChannel{
                ResourceName{Resource::descriptor.type.name()}, cxx::make_basic_type_info<Resource>(),
                cxx::make_basic_type_info<Elements>(), sizeof(Elements), alignof(Elements)
            }...};
            return result;
        }
    }

    template <class Resource> [[nodiscard]] std::span<const FrameChannel> frameChannels() noexcept
    {
        return frame_detail::channels<Resource>(std::type_identity<typename Resource::PerFrameData>{});
    }
} // namespace lux::render
