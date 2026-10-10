#include <lux/engine/render/transport/Packet.hpp>

namespace lux::render
{
    detail::PacketStorage::PacketStorage(
        std::shared_ptr<const RenderRouteTable> route_table,
        std::shared_ptr<ReplyArena> reply_arena,
        RenderPacketCapacity limits
    ) noexcept : routes(std::move(route_table)), replies(std::move(reply_arena)), capacity(limits)
    {
        records.reserve(capacity.records);
        payload.reserve(capacity.bytes);
        attachments.reserve(capacity.attachments);
    }

    detail::PacketStorage& detail::PacketStorage::operator=(PacketStorage&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            swap(other);
        }
        return *this;
    }

    void detail::PacketStorage::clear() noexcept
    {
        for (const auto& record : records)
            if (record.reply) replies->cancel(*record.reply);
        records.clear();
        payload.clear();
        attachments.clear();
    }

    void detail::PacketStorage::swap(PacketStorage& other) noexcept
    {
        routes.swap(other.routes);
        replies.swap(other.replies);
        std::swap(capacity, other.capacity);
        records.swap(other.records);
        payload.swap(other.payload);
        attachments.swap(other.attachments);
    }

    RenderResult<std::uint32_t> detail::PacketStorage::prepare(std::size_t size, std::size_t alignment) const noexcept
    {
        const auto offset = (payload.size() + alignment - 1) & ~(alignment - 1);
        const bool is_full = records.size() == capacity.records;
        const bool is_oversize = offset > capacity.bytes || size > capacity.bytes - offset;
        if (is_full || is_oversize) return cxx::unexpected(RenderError{kTransportCapacity});
        return static_cast<std::uint32_t>(offset);
    }

    std::uint64_t detail::PacketStorage::accountedBytes() const noexcept
    {
        std::uint64_t result = payload.size();
        for (const auto& attachment : attachments) result += attachment.bytes().size();
        return result;
    }

    RenderResult<std::span<const std::byte>> RenderPacketView::blob(BlobRef reference) const noexcept
    {
        const bool is_invalid_range = reference.offset > storage_.payload.size() ||
            reference.size > storage_.payload.size() - reference.offset;
        if (is_invalid_range) return cxx::unexpected(RenderError{kTransportBounds});
        return std::span<const std::byte>{storage_.payload}.subspan(reference.offset, reference.size);
    }

    RenderResult<std::span<const std::byte>> RenderPacketView::external(ExternalDataRef reference) const noexcept
    {
        if (reference.attachment_index >= storage_.attachments.size())
            return cxx::unexpected(RenderError{kTransportBounds});
        const auto bytes = storage_.attachments[reference.attachment_index].bytes();
        const bool is_invalid_range = reference.offset > bytes.size() ||
            reference.size > bytes.size() - reference.offset;
        if (is_invalid_range) return cxx::unexpected(RenderError{kTransportBounds});
        return bytes.subspan(reference.offset, reference.size);
    }

    RenderResult<PinnedRenderBytes> RenderPacketView::retain(ExternalDataRef reference) const noexcept
    {
        auto checked = external(reference);
        if (!checked) return cxx::unexpected(checked.error());
        return storage_.attachments[reference.attachment_index];
    }

    RenderResult<std::span<const std::byte>>
    RenderPacketView::recordBytes(std::uint64_t owner, RenderRouteId route, std::size_t index) const noexcept
    {
        auto checked = storage_.routes->validate(owner, route);
        if (!checked) return cxx::unexpected(checked.error());
        if (index >= storage_.records.size()) return cxx::unexpected(RenderError{kTransportBounds});
        if (storage_.records[index].route != route) return cxx::unexpected(RenderError{kTransportContract});
        return blob(storage_.records[index].payload);
    }
}
