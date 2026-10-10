#include <lux/engine/render/transport/Routes.hpp>

#include <atomic>
#include <exception>
#include <limits>
#include <utility>

namespace lux::render
{
    namespace detail
    {
        std::uint64_t nextTransportIdentity() noexcept
        {
            static std::atomic<std::uint64_t> next{1};
            const auto value = next.fetch_add(1, std::memory_order_relaxed);
            if (value == 0 || value == UINT64_MAX) std::terminate();
            return value;
        }

        std::uint64_t transportThreadIdentity() noexcept
        {
            thread_local const auto identity = nextTransportIdentity();
            return identity;
        }
    }

    namespace
    {
        RenderResult<void> compatible(const RenderOperationDescriptor& a, const RenderOperationDescriptor& b) noexcept
        {
            auto result = validateCompatible(a.data, b.data);
            if (!result) return result;
            const bool is_contract_mismatch = a.lane != b.lane || a.kind != b.kind ||
                a.reply.has_value() != b.reply.has_value();
            if (is_contract_mismatch) return cxx::unexpected(RenderError{kTransportContract});
            if (a.reply) return validateCompatible(*a.reply, *b.reply);
            return {};
        }

        RenderResult<void> validateOperation(const RenderOperationDescriptor& descriptor) noexcept
        {
            auto result = validateDescriptor(descriptor.data);
            if (!result) return result;
            const bool is_invalid_lane = descriptor.lane > ERenderLane::UPLOAD;
            const bool is_invalid_kind = descriptor.kind > ERenderOperationKind::BLOB;
            const bool is_invalid_alignment = descriptor.data.alignment > 64;
            const bool is_invalid_reply_kind = descriptor.reply && descriptor.kind != ERenderOperationKind::POD;
            const bool is_invalid_contract = is_invalid_lane || is_invalid_kind ||
                is_invalid_alignment || is_invalid_reply_kind;
            if (is_invalid_contract) return cxx::unexpected(RenderError{kTransportContract});
            if (descriptor.reply)
            {
                result = validateDescriptor(*descriptor.reply);
                if (!result) return result;
                const bool is_invalid_reply = descriptor.reply->size > 256 || descriptor.reply->alignment > 64;
                if (is_invalid_reply) return cxx::unexpected(RenderError{kTransportReply});
            }
            return {};
        }
    }

    RenderRouteTable::RenderRouteTable(std::uint32_t capacity) noexcept
        : owner_(detail::nextTransportIdentity()), owner_thread_(detail::transportThreadIdentity()), entries_(capacity)
    {
    }

    RenderRouteTable::RenderRouteTable(RenderRouteTable&& other) noexcept
        : owner_(std::exchange(other.owner_, 0)), owner_thread_(std::exchange(other.owner_thread_, 0)),
          entries_(std::move(other.entries_))
    {
    }

    RenderRouteTable& RenderRouteTable::operator=(RenderRouteTable&& other) noexcept
    {
        if (this != &other)
        {
            entries_ = std::move(other.entries_);
            owner_ = std::exchange(other.owner_, 0);
            owner_thread_ = std::exchange(other.owner_thread_, 0);
        }
        return *this;
    }

    RenderResult<RenderRouteId>
    RenderRouteTable::registerOperation(const RenderOperationDescriptor& descriptor) noexcept
    {
        if (owner_thread_ != detail::transportThreadIdentity())
            return cxx::unexpected(RenderError{kTransportWrongThread});
        auto checked = validateOperation(descriptor);
        if (!checked) return cxx::unexpected(checked.error());
        for (std::uint32_t index = 0; index < entries_.size(); ++index)
        {
            auto& entry = entries_[index];
            if (entry.descriptor && entry.descriptor->data.id == descriptor.data.id)
            {
                checked = compatible(*entry.descriptor, descriptor);
                if (!checked) return cxx::unexpected(checked.error());
                return RenderRouteId{index, entry.generation};
            }
        }
        for (std::uint32_t index = 0; index < entries_.size(); ++index)
        {
            auto& entry = entries_[index];
            const bool is_available = !entry.descriptor && entry.generation != UINT64_MAX;
            if (!is_available) continue;
            entry.name = descriptor.data.canonical_name;
            if (descriptor.reply) entry.reply_name = descriptor.reply->canonical_name;
            entry.descriptor = descriptor;
            entry.descriptor->data.canonical_name = entry.name;
            if (entry.descriptor->reply) entry.descriptor->reply->canonical_name = entry.reply_name;
            return RenderRouteId{index, entry.generation};
        }
        return cxx::unexpected(RenderError{kTransportCapacity});
    }

    RenderResult<void> RenderRouteTable::removeScoped(std::uint64_t owner, RenderRouteId id) noexcept
    {
        if (owner_thread_ != detail::transportThreadIdentity())
            return cxx::unexpected(RenderError{kTransportWrongThread});
        auto result = validate(owner, id);
        if (!result) return result;
        entries_[id.index].descriptor.reset();
        ++entries_[id.index].generation;
        return {};
    }

    RenderResult<RenderRouteId> RenderRouteTable::resolve(const RenderOperationDescriptor& descriptor) const noexcept
    {
        if (owner_thread_ != detail::transportThreadIdentity())
            return cxx::unexpected(RenderError{kTransportWrongThread});
        auto checked = validateOperation(descriptor);
        if (!checked) return cxx::unexpected(checked.error());
        for (std::uint32_t index = 0; index < entries_.size(); ++index)
        {
            const auto& entry = entries_[index];
            if (entry.descriptor && entry.descriptor->data.id == descriptor.data.id)
            {
                checked = compatible(*entry.descriptor, descriptor);
                if (!checked) return cxx::unexpected(checked.error());
                return RenderRouteId{index, entry.generation};
            }
        }
        return cxx::unexpected(RenderError{kTransportUnknownRoute});
    }

    RenderResult<void> RenderRouteTable::validate(std::uint64_t owner, RenderRouteId route) const noexcept
    {
        if (owner != owner_) return cxx::unexpected(RenderError{kTransportWrongOwner});
        if (route.index >= entries_.size()) return cxx::unexpected(RenderError{kTransportUnknownRoute});
        const auto& entry = entries_[route.index];
        const bool is_stale = !entry.descriptor || entry.generation != route.generation;
        if (is_stale) return cxx::unexpected(RenderError{kTransportStaleRoute});
        return {};
    }
}
