#pragma once

#include <lux/engine/render/frame/FrameLayout.hpp>
#include <algorithm>
#include <memory>
#include <utility>

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

        template <class Resource, class... Elements>
        void appendColumnOps(
            const FrameLayout& layout,
            std::vector<const ColumnOps*>& output,
            std::type_identity<std::tuple<Elements...>>
        ) noexcept
        {
            ((output[layout.find(cxx::make_basic_type_info<Resource>(), cxx::make_basic_type_info<Elements>())] =
                &columnOps<Elements>), ...);
        }
    }

    // One CPU input slot. Payloads are grouped by (Resource declaration, element type).
    // Cross-channel application order is not insertion order; zero elements mean no update.
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

        // Initial capacities are per channel, or empty for no reservation. They are not schema limits.
        // Element metadata and typed callbacks must remain loaded until this owner is destroyed.
        template <class Resources> [[nodiscard]] static RenderResult<FrameData>
        create(const FrameLayout& layout, std::span<const std::size_t> capacities = {}) noexcept
        {
            auto declared = makeFrameLayout(std::type_identity<Resources>{}, layout.revision());
            if (!declared) return cxx::unexpected(declared.error());
            if (!declared->hasSameChannels(layout)) return cxx::unexpected(RenderError{kFrameContract});
            std::vector<const frame_detail::ColumnOps*> operations(layout.channels().size());
            [&]<class... R>(std::type_identity<std::tuple<R...>>) noexcept
            {
                (frame_detail::appendColumnOps<R>(
                    layout, operations, std::type_identity<typename R::PerFrameData>{}
                ), ...);
            }(std::type_identity<Resources>{});
            return create(layout, operations, capacities);
        }

        template <class Resource, class T>
        requires(frame_detail::FrameElement<T> && frame_detail::declaredElement<Resource, T>)
        [[nodiscard]] RenderResult<std::span<T>> appendData(std::size_t count) noexcept
        {
            static_assert(frame_detail::FrameElement<T>);
            auto appended = append(channel<Resource, T>(), count);
            if (!appended) return cxx::unexpected(appended.error());
            return std::span<T>{static_cast<T*>(*appended), count};
        }

        template <class Resource, class T>
            requires frame_detail::FrameElement<std::remove_cvref_t<T>> &&
                     frame_detail::declaredElement<Resource, T> &&
                     std::is_nothrow_assignable_v<std::remove_cvref_t<T>&, T&&>
        [[nodiscard]] bool addData(T&& value) noexcept
        {
            auto added = appendData<Resource, std::remove_cvref_t<T>>(1);
            if (!added) return false;
            added->front() = std::forward<T>(value);
            return true;
        }

        template <class Resource, class T>
        requires(frame_detail::FrameElement<T> && frame_detail::declaredElement<Resource, T>)
        [[nodiscard]] RenderResult<void> reserve(std::size_t count) noexcept
        {
            return reserve(channel<Resource, T>(), count);
        }

        template <class Resource, class T>
        requires(frame_detail::FrameElement<T> && frame_detail::declaredElement<Resource, T>)
        [[nodiscard]] std::span<const T> getData() const noexcept
        {
            const auto values = column(channel<Resource, T>());
            return {static_cast<const T*>(values.data), values.count};
        }

        [[nodiscard]] const FrameLayout& layout() const noexcept;
        [[nodiscard]] Column column(std::size_t index) const noexcept;
        [[nodiscard]] std::size_t capacity(std::size_t index) const noexcept;
        [[nodiscard]] bool writable() const noexcept;
        [[nodiscard]] bool sealed() const noexcept;
        [[nodiscard]] RenderResult<void> seal() noexcept;
        // Caller thread only, after terminal response/host settlement, or proven pre-admission rejection.
        // Cancellation, disconnection and successful send alone never authorize release().
        void release() noexcept;
        // Reset retains capacities. Reset/move/destruction while sealed is a fatal ownership violation.
        void reset() noexcept;

    private:
        friend class RenderClient;
        [[nodiscard]] bool beginBorrow() noexcept;
        void finishBorrow() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
        explicit FrameData(std::unique_ptr<Impl>) noexcept;
        [[nodiscard]] static RenderResult<FrameData>
        create(FrameLayout, std::span<const frame_detail::ColumnOps* const>, std::span<const std::size_t>) noexcept;
        [[nodiscard]] RenderResult<void*> append(std::size_t, std::size_t) noexcept;
        [[nodiscard]] RenderResult<void> reserve(std::size_t, std::size_t) noexcept;
        void requireUnsealed() const noexcept;
        [[nodiscard]] std::size_t findChannel(cxx::basic_type_info, cxx::basic_type_info) const noexcept;

        template <class Resource, class T> [[nodiscard]] std::size_t channel() const noexcept
        {
            return findChannel(cxx::make_basic_type_info<Resource>(), cxx::make_basic_type_info<T>());
        }
    };
} // namespace lux::render
