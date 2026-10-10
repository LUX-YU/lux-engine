#pragma once

#include <memory>
#include <string>
#include <vector>
#include <lux/engine/render/transport/Error.hpp>
#include <lux/engine/render/transport/Operation.hpp>

namespace lux::render
{
    namespace detail
    {
        [[nodiscard]] std::uint64_t nextTransportIdentity() noexcept;
        [[nodiscard]] std::uint64_t transportThreadIdentity() noexcept;
    }

    struct RenderRouteId final
    {
        std::uint32_t index{};
        std::uint64_t generation{};
        bool operator==(const RenderRouteId&) const noexcept = default;
    };

    template <PacketValue T>
    class BoundRenderRoute final
    {
    public:
        [[nodiscard]] RenderRouteId localId() const noexcept { return route_; }

        [[nodiscard]] std::uint64_t ownerIdentity() const noexcept { return owner_; }

    private:
        friend class RenderRouteTable;
        BoundRenderRoute(std::uint64_t owner, RenderRouteId route) noexcept : owner_(owner), route_(route) {}
        std::uint64_t owner_;
        RenderRouteId route_;
    };

    // Cold assembly only. Moving this complete table into Transport freezes it.
    // No mutation races with bound producers or consumers. R6 will establish a
    // real installation safe point before supporting live Feature registration.
    class RenderRouteTable final
    {
    public:
        explicit RenderRouteTable(std::uint32_t capacity) noexcept;
        RenderRouteTable(const RenderRouteTable&) = delete;
        RenderRouteTable& operator=(const RenderRouteTable&) = delete;
        RenderRouteTable(RenderRouteTable&& other) noexcept;
        RenderRouteTable& operator=(RenderRouteTable&& other) noexcept;

        [[nodiscard]] RenderResult<RenderRouteId>
        registerOperation(const RenderOperationDescriptor& descriptor) noexcept;
        [[nodiscard]] RenderResult<RenderRouteId> resolve(const RenderOperationDescriptor& descriptor) const noexcept;
        [[nodiscard]] RenderResult<void> validate(std::uint64_t owner, RenderRouteId route) const noexcept;
        [[nodiscard]] std::uint64_t ownerIdentity() const noexcept { return owner_; }

        [[nodiscard]] std::uint64_t ownerThread() const noexcept { return owner_thread_; }

        template <PacketValue T>
        [[nodiscard]] RenderResult<void> remove(BoundRenderRoute<T> route) noexcept
        {
            return removeScoped(route.ownerIdentity(), route.localId());
        }

        template <PacketValue T>
        [[nodiscard]] RenderResult<BoundRenderRoute<T>> registerOperation() noexcept
        {
            auto result = registerOperation(operationDescriptor<T>());
            if (!result) return cxx::unexpected(result.error());
            return BoundRenderRoute<T>{owner_, *result};
        }

        template <PacketValue T>
        [[nodiscard]] RenderResult<BoundRenderRoute<T>> bind() const noexcept
        {
            auto result = resolve(operationDescriptor<T>());
            if (!result) return cxx::unexpected(result.error());
            return BoundRenderRoute<T>{owner_, *result};
        }

    private:
        [[nodiscard]] RenderResult<void> removeScoped(std::uint64_t owner, RenderRouteId id) noexcept;
        struct Entry final
        {
            std::optional<RenderOperationDescriptor> descriptor;
            std::string name;
            std::string reply_name;
            std::uint64_t generation{1};
        };
        std::uint64_t owner_;
        std::uint64_t owner_thread_;
        std::vector<Entry> entries_;
    };
}
