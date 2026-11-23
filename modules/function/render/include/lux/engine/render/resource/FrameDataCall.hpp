#pragma once

#include <lux/engine/render/frame/FrameData.hpp>

namespace lux::render
{
    // One immutable batch entry per Resource declaration, not per element/channel or Handler object.
    struct FrameDataCall final
    {
        cxx::basic_type_info declaration_type;
        cxx::basic_type_info storage_type;
        cxx::basic_type_info method_type;
        std::span<const FrameChannel> channels;
        RenderResult<void> (*invoke)(void*, const FrameData&, std::size_t) noexcept = nullptr;

        template <class T> static std::span<const T> readColumn(const FrameData& data, std::size_t index) noexcept
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
                    [](void* object, const FrameData& data, std::size_t first_column) noexcept
                    {
                        return [&]<std::size_t... I>(std::index_sequence<I...>) noexcept
                        {
                            return Method(*static_cast<Storage*>(object), readColumn<T>(data, first_column + I)...);
                        }(std::index_sequence_for<T...>{});
                    }
                };
            }(std::type_identity<typename Resource::PerFrameData>{});
        }
    };
} // namespace lux::render
