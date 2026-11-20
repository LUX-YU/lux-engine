#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/Resource.hpp>
#include <lux/engine/render/visibility.h>
#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::render
{
    class RenderServer;

    struct ServerInstanceId final
    {
        std::uint64_t value = 0;

        bool operator==(const ServerInstanceId&) const noexcept = default;
    };

    struct FrameLayoutKey final
    {
        ServerInstanceId server;
        std::uint64_t input_layout_revision = 0;

        [[nodiscard]] bool valid() const noexcept { return server.value != 0 && input_layout_revision != 0; }
        bool operator==(const FrameLayoutKey&) const noexcept = default;
    };

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
        FrameLayout(const FrameLayout&) noexcept;
        FrameLayout& operator=(const FrameLayout&) noexcept;
        FrameLayout(FrameLayout&&) noexcept;
        FrameLayout& operator=(FrameLayout&&) noexcept;

        // A standalone schema has no Server identity.
        // Identical declarations are merged; conflicting declaration/type metadata is rejected.
        [[nodiscard]] static RenderResult<FrameLayout>
        create(std::span<const FrameChannel> channels, std::uint64_t revision) noexcept;

        [[nodiscard]] std::span<const FrameChannel> channels() const & noexcept { return channels_; }
        [[nodiscard]] std::span<const FrameChannel> channels() const && = delete;
        [[nodiscard]] FrameLayoutKey key() const noexcept { return key_; }
        [[nodiscard]] std::uint64_t revision() const noexcept { return key_.input_layout_revision; }
        [[nodiscard]] bool valid() const noexcept { return revision() != 0; }
        [[nodiscard]] bool hasSameChannels(const FrameLayout&) const noexcept;
        [[nodiscard]] bool operator==(const FrameLayout&) const noexcept;
        // Copies from an execution query retain this immutable identity; no global registry is involved.
        [[nodiscard]] bool hasSameIdentity(const FrameLayout& other) const noexcept
        {
            return key_.valid() && key_ == other.key_;
        }
        [[nodiscard]] std::size_t find(cxx::basic_type_info declaration, cxx::basic_type_info element) const noexcept;

    private:
        friend class RenderServer;

        struct TypeNames final
        {
            std::string declaration;
            std::string element;
        };

        void ownTypeNames() noexcept;

        FrameLayoutKey key_;
        std::vector<FrameChannel> channels_;
        std::vector<TypeNames> type_names_;
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
