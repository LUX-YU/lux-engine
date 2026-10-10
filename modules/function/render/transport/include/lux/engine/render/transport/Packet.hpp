#pragma once

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>
#include <lux/engine/render/transport/Replies.hpp>

namespace lux::render
{
    struct RenderPacketCapacity final
    {
        std::uint32_t records{256};
        std::uint32_t bytes{64 * 1024};
        std::uint32_t attachments{8};
    };

    class PinnedRenderBytes final
    {
    public:
        [[nodiscard]] static PinnedRenderBytes copy(std::span<const std::byte> bytes) noexcept
        {
            return PinnedRenderBytes{std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end())};
        }

        // Caller promises the shared vector remains immutable. Unlike an arbitrary
        // void owner + span, the byte range is derived from the retained owner.
        explicit PinnedRenderBytes(std::shared_ptr<const std::vector<std::byte>> owner) noexcept
            : owner_(std::move(owner))
        {
            if (!owner_) std::terminate();
        }

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return *owner_; }

    private:
        std::shared_ptr<const std::vector<std::byte>> owner_;
    };

    namespace detail
    {
        // Migrated from V1 RenderCommTypes.hpp: aligned packet storage, fatal OOM.
        template <typename T>
        struct PacketAllocator
        {
            using value_type = T;
            using is_always_equal = std::true_type;
            PacketAllocator() noexcept = default;

            template <typename U> PacketAllocator(const PacketAllocator<U>&) noexcept {}

            [[nodiscard]] T* allocate(std::size_t count)
            {
                if (count > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) std::terminate();
                return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{64}));
            }

            void deallocate(T* pointer, std::size_t) noexcept { ::operator delete(pointer, std::align_val_t{64}); }

            template <typename U> bool operator==(const PacketAllocator<U>&) const noexcept { return true; }
        };

        struct PacketRecord final
        {
            RenderRouteId route;
            BlobRef payload;
            std::optional<ReplyToken> reply;
            RenderDataTypeId reply_type;
        };

        struct PacketStorage final
        {
            PacketStorage() noexcept = default;
            PacketStorage(
                std::shared_ptr<const RenderRouteTable> routes,
                std::shared_ptr<ReplyArena> replies,
                RenderPacketCapacity capacity
            ) noexcept;
            PacketStorage(const PacketStorage&) = delete;
            PacketStorage& operator=(const PacketStorage&) = delete;
            PacketStorage(PacketStorage&&) noexcept = default;
            PacketStorage& operator=(PacketStorage&& other) noexcept;
            ~PacketStorage() { clear(); }

            void clear() noexcept;
            void swap(PacketStorage& other) noexcept;
            [[nodiscard]] RenderResult<std::uint32_t> prepare(std::size_t size, std::size_t alignment) const noexcept;
            [[nodiscard]] std::uint64_t accountedBytes() const noexcept;

            std::shared_ptr<const RenderRouteTable> routes;
            std::shared_ptr<ReplyArena> replies;
            RenderPacketCapacity capacity;
            std::vector<PacketRecord> records;
            std::vector<std::byte, PacketAllocator<std::byte>> payload;
            std::vector<PinnedRenderBytes> attachments;
        };
    }

    template <ERenderLane Lane>
    class RenderPacket final
    {
    public:
        RenderPacket(const RenderPacket&) = delete;
        RenderPacket& operator=(const RenderPacket&) = delete;
        RenderPacket(RenderPacket&&) noexcept = default;
        RenderPacket& operator=(RenderPacket&&) noexcept = default;

        [[nodiscard]] std::size_t size() const noexcept { return storage_.records.size(); }

        [[nodiscard]] RenderResult<void> reset() noexcept
        {
            auto checked = checkThread();
            if (!checked) return checked;
            storage_.clear();
            return {};
        }

        template <PacketValue T>
            requires (RenderOpTraits<T>::lane == Lane && RenderOpTraits<T>::kind == ERenderOperationKind::POD &&
                std::is_void_v<typename RenderOpTraits<T>::Reply>)
        [[nodiscard]] RenderResult<void> write(BoundRenderRoute<T> route, const T& value) noexcept
        {
            return append(route, std::as_bytes(std::span{&value, 1}), alignof(T));
        }

        template <PacketValue T>
            requires (RenderOpTraits<T>::lane == Lane && RenderOpTraits<T>::kind == ERenderOperationKind::BULK &&
                std::is_void_v<typename RenderOpTraits<T>::Reply>)
        [[nodiscard]] RenderResult<void> writeBulk(BoundRenderRoute<T> route, std::span<const T> values) noexcept
        {
            return append(route, std::as_bytes(values), alignof(T));
        }

        template <PacketValue T>
            requires (RenderOpTraits<T>::lane == Lane && RenderOpTraits<T>::kind == ERenderOperationKind::BLOB &&
                std::is_void_v<typename RenderOpTraits<T>::Reply>)
        [[nodiscard]] RenderResult<void>
        writeBlob(BoundRenderRoute<T> route, T value, std::span<const std::byte> blob) noexcept
        {
            auto checked = check(route);
            if (!checked) return checked;
            const bool is_oversize = blob.size() > UINT32_MAX - sizeof(T);
            if (is_oversize) return cxx::unexpected(RenderError{kTransportCapacity});
            auto offset = storage_.prepare(sizeof(T) + blob.size(), alignof(T));
            if (!offset) return cxx::unexpected(offset.error());
            value.*RenderOpTraits<T>::blob_member = BlobRef{
                static_cast<std::uint32_t>(*offset + sizeof(T)), static_cast<std::uint32_t>(blob.size())
            };
            storage_.payload.resize(*offset + sizeof(T) + blob.size());
            std::memcpy(storage_.payload.data() + *offset, &value, sizeof(T));
            if (!blob.empty()) std::memcpy(storage_.payload.data() + *offset + sizeof(T), blob.data(), blob.size());
            storage_.records.push_back({route.localId(), {*offset, sizeof(T)}, std::nullopt, {}});
            return {};
        }

        template <PacketValue T>
            requires (RenderOpTraits<T>::lane == Lane && RenderOpTraits<T>::kind == ERenderOperationKind::POD &&
                !std::is_void_v<typename RenderOpTraits<T>::Reply>)
        [[nodiscard]] RenderResult<RenderReplyTicket<typename RenderOpTraits<T>::Reply>>
        request(BoundRenderRoute<T> route, const T& value) noexcept
        {
            using Reply = typename RenderOpTraits<T>::Reply;
            auto checked = check(route);
            if (!checked) return cxx::unexpected(checked.error());
            auto offset = storage_.prepare(sizeof(T), alignof(T));
            if (!offset) return cxx::unexpected(offset.error());
            constexpr auto type = renderReplyTypeId<Reply>;
            auto token = storage_.replies->reserve(type, sizeof(Reply));
            if (!token) return cxx::unexpected(token.error());
            storage_.payload.resize(*offset + sizeof(T));
            std::memcpy(storage_.payload.data() + *offset, &value, sizeof(T));
            storage_.records.push_back({route.localId(), {*offset, sizeof(T)}, *token, type});
            return RenderReplyTicket<Reply>{*token};
        }

        [[nodiscard]] RenderResult<ExternalDataRef> attach(PinnedRenderBytes bytes) noexcept
        {
            auto checked = checkThread();
            if (!checked) return cxx::unexpected(checked.error());
            const bool is_full = storage_.attachments.size() == storage_.capacity.attachments;
            const bool is_oversize = bytes.bytes().size() > UINT32_MAX;
            if (is_full || is_oversize) return cxx::unexpected(RenderError{kTransportCapacity});
            const ExternalDataRef result{
                static_cast<std::uint32_t>(storage_.attachments.size()), 0,
                static_cast<std::uint32_t>(bytes.bytes().size())
            };
            storage_.attachments.push_back(std::move(bytes));
            return result;
        }

    private:
        friend class RenderTransport;
        explicit RenderPacket(detail::PacketStorage storage) noexcept : storage_(std::move(storage)) {}

        [[nodiscard]] RenderResult<void> checkThread() const noexcept
        {
            if (!storage_.routes) return cxx::unexpected(RenderError{kTransportWrongOwner});
            if constexpr (Lane != ERenderLane::UPLOAD)
            {
                if (storage_.routes->ownerThread() != detail::transportThreadIdentity())
                    return cxx::unexpected(RenderError{kTransportWrongThread});
            }
            return {};
        }

        template <PacketValue T>
        [[nodiscard]] RenderResult<void> check(BoundRenderRoute<T> route) const noexcept
        {
            auto thread = checkThread();
            if (!thread) return thread;
            return storage_.routes->validate(route.ownerIdentity(), route.localId());
        }

        template <PacketValue T>
        RenderResult<void>
        append(BoundRenderRoute<T> route, std::span<const std::byte> bytes, std::size_t alignment) noexcept
        {
            auto checked = check(route);
            if (!checked) return checked;
            auto offset = storage_.prepare(bytes.size(), alignment);
            if (!offset) return cxx::unexpected(offset.error());
            storage_.payload.resize(*offset + bytes.size());
            if (!bytes.empty()) std::memcpy(storage_.payload.data() + *offset, bytes.data(), bytes.size());
            storage_.records.push_back({route.localId(), {*offset, static_cast<std::uint32_t>(bytes.size())}, {}, {}});
            return {};
        }

        detail::PacketStorage storage_;
    };

    using RenderProgramPacket = RenderPacket<ERenderLane::PROGRAM>;
    using RenderControlPacket = RenderPacket<ERenderLane::CONTROL>;
    using RenderUploadPacket = RenderPacket<ERenderLane::UPLOAD>;

    // Valid only during its consumer callback. Values are copied; spans borrow
    // the current packet. retain()/deferReply() explicitly extend ownership.
    class RenderPacketView final
    {
    public:
        [[nodiscard]] std::size_t size() const noexcept { return storage_.records.size(); }

        [[nodiscard]] RenderResult<RenderRouteId> route(std::size_t index) const noexcept
        {
            if (index >= storage_.records.size()) return cxx::unexpected(RenderError{kTransportBounds});
            return storage_.records[index].route;
        }

        [[nodiscard]] RenderResult<void> fail(std::size_t index, RenderError error) noexcept
        {
            if (index >= storage_.records.size()) return cxx::unexpected(RenderError{kTransportBounds});
            const auto& record = storage_.records[index];
            if (!record.reply || !error.type) return cxx::unexpected(RenderError{kTransportReply});
            return storage_.replies->finish(*record.reply, {}, {}, error);
        }

        template <PacketValue T>
        [[nodiscard]] RenderResult<T> read(BoundRenderRoute<T> route, std::size_t index) const noexcept
        {
            auto bytes = recordBytes(route.ownerIdentity(), route.localId(), index);
            if (!bytes) return cxx::unexpected(bytes.error());
            if (bytes->size() != sizeof(T)) return cxx::unexpected(RenderError{kTransportBounds});
            std::array<std::byte, sizeof(T)> value;
            std::memcpy(value.data(), bytes->data(), sizeof(T));
            return std::bit_cast<T>(value);
        }

        template <PacketValue T>
            requires (RenderOpTraits<T>::kind == ERenderOperationKind::BULK)
        [[nodiscard]] RenderResult<std::span<const T>>
        readBulk(BoundRenderRoute<T> route, std::size_t index) const noexcept
        {
            auto bytes = recordBytes(route.ownerIdentity(), route.localId(), index);
            if (!bytes) return cxx::unexpected(bytes.error());
            const bool is_invalid_layout = bytes->size() % sizeof(T) != 0 ||
                reinterpret_cast<std::uintptr_t>(bytes->data()) % alignof(T) != 0;
            if (is_invalid_layout) return cxx::unexpected(RenderError{kTransportBounds});
            return std::span<const T>{reinterpret_cast<const T*>(bytes->data()), bytes->size() / sizeof(T)};
        }

        [[nodiscard]] RenderResult<std::span<const std::byte>> blob(BlobRef reference) const noexcept;
        [[nodiscard]] RenderResult<std::span<const std::byte>> external(ExternalDataRef reference) const noexcept;
        [[nodiscard]] RenderResult<PinnedRenderBytes> retain(ExternalDataRef reference) const noexcept;

        template <PacketValue T>
        [[nodiscard]] RenderResult<void> complete(std::size_t index, const T& value) noexcept
        {
            auto token = replyToken<T>(index);
            if (!token) return cxx::unexpected(token.error());
            return storage_.replies->finish(
                *token, renderReplyTypeId<T>, std::as_bytes(std::span{&value, 1})
            );
        }

        template <PacketValue T>
        [[nodiscard]] RenderResult<RenderReplyPromise<T>> deferReply(std::size_t index) noexcept
        {
            auto token = replyToken<T>(index);
            if (!token) return cxx::unexpected(token.error());
            storage_.records[index].reply.reset();
            return RenderReplyPromise<T>{storage_.replies, *token};
        }

    private:
        friend class RenderTransport;
        explicit RenderPacketView(detail::PacketStorage& storage) noexcept : storage_(storage) {}
        [[nodiscard]] RenderResult<std::span<const std::byte>>
        recordBytes(std::uint64_t owner, RenderRouteId route, std::size_t index) const noexcept;

        template <PacketValue T>
        RenderResult<detail::ReplyToken> replyToken(std::size_t index) const noexcept
        {
            if (index >= storage_.records.size()) return cxx::unexpected(RenderError{kTransportBounds});
            const auto& record = storage_.records[index];
            const bool is_mismatch = !record.reply || record.reply_type != renderReplyTypeId<T>;
            if (is_mismatch) return cxx::unexpected(RenderError{kTransportReply});
            return *record.reply;
        }

        detail::PacketStorage& storage_;
    };
}
