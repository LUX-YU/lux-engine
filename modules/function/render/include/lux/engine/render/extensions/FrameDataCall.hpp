#pragma once

#include <lux/engine/render/frame/FrameData.hpp>

namespace lux::render
{
    // One immutable batch entry per Resource declaration, not per element/channel or Handler object.
    struct FrameDataCall final
    {
        cxx::type_info declaration_type;
        cxx::type_info storage_type;
        cxx::type_info method_type;
        std::span<const FrameChannel> channels;
        RenderResult<void> (*invoke)(void*, const FrameData&, std::span<const std::uint32_t>) noexcept = nullptr;

        bool operator==(const FrameDataCall& other) const noexcept
        {
            return declaration_type == other.declaration_type && storage_type == other.storage_type &&
                method_type == other.method_type && std::ranges::equal(channels, other.channels);
        }

        template <class T> static std::span<const T> readColumn(const FrameData& data, std::uint32_t index) noexcept
        {
            const auto column = data.column(index);
            return {static_cast<const T*>(column.data), column.count};
        }

        template <class Resource, class Storage, auto Method> [[nodiscard]] static FrameDataCall declare() noexcept
        {
            return []<class... T>(std::type_identity<std::tuple<T...>>) noexcept
            {
                using Apply = RenderResult<void> (*)(Storage&, std::span<const T>...) noexcept;
                static_assert(std::same_as<decltype(Method), Apply>);
                static const auto layout = makeFrameLayout(std::type_identity<std::tuple<Resource>>{}, 1);
                if (!layout) std::terminate();
                return FrameDataCall{
                    cxx::make_basic_type_info<Resource>(), cxx::make_basic_type_info<Storage>(),
                    cxx::make_basic_type_info<std::integral_constant<decltype(Method), Method>>(), layout->channels(),
                    [](void* object, const FrameData& data, std::span<const std::uint32_t> columns) noexcept
                    {
                        return [&]<std::size_t... I>(std::index_sequence<I...>) noexcept
                        {
                            return Method(*static_cast<Storage*>(object), readColumn<T>(data, columns[I])...);
                        }(std::index_sequence_for<T...>{});
                    }
                };
            }(std::type_identity<typename Resource::PerFrameData>{});
        }
    };
} // namespace lux::render
