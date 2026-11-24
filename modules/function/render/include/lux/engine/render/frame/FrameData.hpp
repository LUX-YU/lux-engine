#pragma once

#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lux::render
{
    namespace frame_detail
    {
        // The vector owns its size and allocation. The erased owner has no independent writable count.
        struct ColumnOps final
        {
            void* (*create)() noexcept;
            void (*destroy)(void*) noexcept;
            void (*clear)(void*) noexcept;
            const void* (*data)(const void*) noexcept;
            std::size_t (*size)(const void*) noexcept;
            std::size_t (*capacity)(const void*) noexcept;
            RenderResult<void> (*reserve)(void*, std::size_t) noexcept;
            RenderResult<void*> (*append)(void*, std::size_t) noexcept;
        };

        template <FrameElement T> RenderResult<void> reserveColumn(std::vector<T>& values, std::size_t count) noexcept
        {
            if (count > values.max_size()) return cxx::unexpected(RenderError{kRuntimeCapacity});
            if (count <= values.capacity()) return {};
            const auto capacity = values.capacity();
            const auto doubled = capacity > values.max_size() / 2 ? values.max_size() : capacity * 2;
            values.reserve(std::max(count, doubled));
            return {};
        }

        template <FrameElement T> inline constexpr ColumnOps columnOps{
            []() noexcept -> void* { return new std::vector<T>; },
            [](void* value) noexcept { delete static_cast<std::vector<T>*>(value); },
            [](void* value) noexcept { static_cast<std::vector<T>*>(value)->clear(); },
            [](const void* value) noexcept -> const void* { return static_cast<const std::vector<T>*>(value)->data(); },
            [](const void* value) noexcept { return static_cast<const std::vector<T>*>(value)->size(); },
            [](const void* value) noexcept { return static_cast<const std::vector<T>*>(value)->capacity(); },
            [](void* value, std::size_t count) noexcept
            {
                return reserveColumn(*static_cast<std::vector<T>*>(value), count);
            },
            [](void* value, std::size_t count) noexcept -> RenderResult<void*>
            {
                auto& values = *static_cast<std::vector<T>*>(value);
                if (count > values.max_size() - values.size()) return cxx::unexpected(RenderError{kRuntimeCapacity});
                if (count == 0) return nullptr;
                const auto offset = values.size();
                const auto required = offset + count;
                if (auto reserved = reserveColumn(values, required); !reserved)
                    return cxx::unexpected(reserved.error());
                values.resize(required);
                return static_cast<void*>(values.data() + offset);
            }
        };

        template <class... Elements>
        void appendColumnOps(std::vector<const ColumnOps*>& ops, std::type_identity<std::tuple<Elements...>>) noexcept
        {
            (ops.push_back(&columnOps<Elements>), ...);
        }
    }

    template <class Resource, class T>
    class TypedColumnIndex final
    {
        friend class FrameData;

        explicit TypedColumnIndex(std::size_t index) noexcept : index_(index) {}
        std::size_t index_;
    };

    // Moving this packet ends all borrows of its columns and writable spans.
    // One CPU input slot. Payloads are grouped by (Resource declaration, element type).
    // Batches apply in packet order. An empty PER_RUN batch explicitly replaces its previous payload.
    // Pointer/span members do not acquire a deep lifetime: use explicit commands for foreign borrows.
    class LUX_RENDER_PUBLIC FrameData final
    {
    public:
        struct Column final
        {
            const void* data;
            std::size_t count;
        };

        FrameData() noexcept;
        ~FrameData() noexcept;
        FrameData(FrameData&&) noexcept;
        FrameData& operator=(FrameData&&) noexcept;
        FrameData(const FrameData&) = delete;
        FrameData& operator=(const FrameData&) = delete;

        // The packet owns its columns. Static author metadata remains loaded until the host drains the endpoint.
        struct Batch final
        {
            ResourceAddress target;
            std::span<const FrameChannel> channels;
            std::size_t first_column;
        };

        template <class Resources> [[nodiscard]] static RenderResult<FrameData>
        create(std::array<ResourceAddress, std::tuple_size_v<Resources>> targets,
               std::span<const std::size_t> capacities = {}) noexcept
        {
            std::vector<Batch> batches;
            std::vector<const frame_detail::ColumnOps*> operations;
            batches.reserve(targets.size());
            const auto add = [&]<class Resource>(std::type_identity<Resource>) noexcept
            {
                batches.push_back({targets[batches.size()], frameChannels<Resource>(), operations.size()});
                frame_detail::appendColumnOps(operations, std::type_identity<typename Resource::PerFrameData>{});
            };
            const auto add_batches = [&]<class... R>(std::type_identity<std::tuple<R...>>) noexcept
            {
                (add(std::type_identity<R>{}), ...);
            };
            add_batches(std::type_identity<Resources>{});
            return create(std::move(batches), operations, capacities);
        }

        template <class Resource, class T>
        requires(frame_detail::FrameElement<T> && frame_detail::declaredElement<Resource, T>)
        [[nodiscard]] RenderResult<TypedColumnIndex<Resource, T>> resolveColumn(std::optional<std::size_t> batch = {}) const noexcept
        {
            if (!impl_) return cxx::unexpected(RenderError{kFrameContract});
            const auto index = findChannel(cxx::make_basic_type_info<Resource>(), cxx::make_basic_type_info<T>(), batch);
            if (index == SIZE_MAX) return cxx::unexpected(RenderError{kFrameContract});
            return TypedColumnIndex<Resource, T>{index};
        }

        template <class Resource, class T>
        requires(frame_detail::FrameElement<T> && frame_detail::declaredElement<Resource, T>)
        [[nodiscard]] RenderResult<std::span<T>>
        appendData(TypedColumnIndex<Resource, T> column, std::size_t count) & noexcept
        {
            if (!impl_) return cxx::unexpected(RenderError{kFrameContract});
            const auto index = column.index_;
            if (!matchesChannel(index, cxx::make_basic_type_info<Resource>(), cxx::make_basic_type_info<T>()))
                return cxx::unexpected(RenderError{kFrameContract});
            auto appended = append(index, count);
            if (!appended) return cxx::unexpected(appended.error());
            return std::span<T>{static_cast<T*>(*appended), count};
        }

        template <class Resource, class T>
        requires(frame_detail::FrameElement<T> && frame_detail::declaredElement<Resource, T>)
        [[nodiscard]] RenderResult<std::span<T>> appendData(std::size_t count) & noexcept
        {
            static_assert(frame_detail::FrameElement<T>);
            auto appended = append(channel<Resource, T>(), count);
            if (!appended) return cxx::unexpected(appended.error());
            return std::span<T>{static_cast<T*>(*appended), count};
        }

        template <class Resource, class T>
            requires frame_detail::FrameAssignableFrom<std::remove_cvref_t<T>, T> &&
                     frame_detail::declaredElement<Resource, T>
        [[nodiscard]] RenderResult<void> addData(T&& value) noexcept
        {
            auto added = appendData<Resource, std::remove_cvref_t<T>>(1);
            if (!added) return cxx::unexpected(added.error());
            added->front() = std::forward<T>(value);
            return {};
        }

        template <class Resource, class T>
        requires(frame_detail::FrameElement<T> && frame_detail::declaredElement<Resource, T>)
        [[nodiscard]] RenderResult<void> reserve(std::size_t count) noexcept
        {
            return reserve(channel<Resource, T>(), count);
        }

        template <class Resource, class T>
        requires(frame_detail::FrameElement<T> && frame_detail::declaredElement<Resource, T>)
        [[nodiscard]] std::span<const T> getData() const & noexcept
        {
            const auto values = column(channel<Resource, T>());
            return {static_cast<const T*>(values.data), values.count};
        }

        template <class Resource, class T> std::span<const T> getData() const && = delete;

        [[nodiscard]] std::span<const Batch> batches() const & noexcept;
        std::span<const Batch> batches() const && = delete;
        [[nodiscard]] Column column(std::size_t index) const & noexcept;
        [[nodiscard]] Column column(std::size_t index) const && = delete;
        [[nodiscard]] std::size_t capacity(std::size_t index) const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }
        // Clears the packet for reuse and retains its column capacities.
        void reset() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        explicit FrameData(std::unique_ptr<Impl>) noexcept;
        [[nodiscard]] static RenderResult<FrameData>
        create(std::vector<Batch>, std::span<const frame_detail::ColumnOps* const>, std::span<const std::size_t>) noexcept;
        [[nodiscard]] RenderResult<void*> append(std::size_t, std::size_t) noexcept;
        [[nodiscard]] RenderResult<void> reserve(std::size_t, std::size_t) noexcept;
        [[nodiscard]] std::size_t findChannel(cxx::basic_type_info, cxx::basic_type_info, std::optional<std::size_t> = {}) const noexcept;
        [[nodiscard]] bool matchesChannel(std::size_t, cxx::basic_type_info, cxx::basic_type_info) const noexcept;

        template <class Resource, class T> [[nodiscard]] std::size_t channel() const noexcept
        {
            return findChannel(cxx::make_basic_type_info<Resource>(), cxx::make_basic_type_info<T>());
        }
    };
} // namespace lux::render
