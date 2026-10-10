#include <lux/engine/render/transport/Transport.hpp>

#include <lux/cxx/concurrent/AdmissionGate.hpp>
#include <lux/cxx/concurrent/BoundedSpscFrameRing.hpp>
#include <mutex>

namespace lux::render
{
    namespace
    {
        using Ring = cxx::BoundedSpscFrameRing<detail::PacketStorage, 4>;

        struct Admission final
        {
            cxx::AdmissionTicket<> ticket;
            TransportWake& wake;
            ~Admission() { ticket.release(); wake.notify(); }
        };

        RenderResult<void> publish(Ring& ring, detail::PacketStorage& packet) noexcept
        {
            auto* slot = ring.tryBeginWrite();
            if (!slot) return cxx::unexpected(RenderError{kTransportCapacity});
            slot->swap(packet);
            if (!ring.publishWrite())
            {
                slot->swap(packet);
                return cxx::unexpected(RenderError{kTransportCapacity});
            }
            return {};
        }
    }

    struct RenderTransport::Impl final
    {
        explicit Impl(const RenderTransport& transport) noexcept
            : programs(transport.capacity_.pending_programs), controls(transport.capacity_.pending_controls),
              uploads(transport.capacity_.pending_uploads)
        {
            for (Ring* ring : {&programs, &controls})
            {
                for (unsigned index = 0; index < 4; ++index)
                {
                    auto* slot = ring->tryBeginWrite();
                    if (!slot) std::terminate();
                    *slot = transport.makeStorage();
                    if (!ring->publishWrite() || !ring->tryAcquireRead()) std::terminate();
                }
            }
            for (auto& slot : uploads) slot = transport.makeStorage();
        }

        Ring programs;
        Ring controls;
        cxx::AdmissionGate<> admission;
        std::atomic<std::uint64_t> consumer_thread{};
        std::atomic<std::uint32_t> readers{};
        bool reading_spsc{};
        std::mutex upload_mutex;
        std::vector<detail::PacketStorage> uploads;
        std::size_t upload_head{};
        std::atomic<std::size_t> upload_count{};
        std::atomic<std::uint64_t> upload_bytes{};

        RenderResult<void> checkConsumer() noexcept
        {
            const auto thread = detail::transportThreadIdentity();
            std::uint64_t initial{};
            consumer_thread.compare_exchange_strong(initial, thread);
            if (consumer_thread.load(std::memory_order_acquire) != thread)
                return cxx::unexpected(RenderError{kTransportWrongThread});
            if (reading_spsc) return cxx::unexpected(RenderError{kTransportBusy});
            return {};
        }

        RenderResult<bool> poll(Ring& ring, RenderTransport& transport, void* context, Dispatch dispatch) noexcept
        {
            auto checked = checkConsumer();
            if (!checked) return cxx::unexpected(checked.error());
            readers.fetch_add(1, std::memory_order_acq_rel);
            if (!ring.tryAcquireRead())
            {
                readers.fetch_sub(1, std::memory_order_release);
                return false;
            }
            reading_spsc = true;
            auto& packet = ring.currentRead();
            if (!transport.stopping())
            {
                RenderPacketView view{packet};
                dispatch(context, view);
            }
            packet.clear();
            reading_spsc = false;
            readers.fetch_sub(1, std::memory_order_release);
            transport.wake_->notify();
            return true;
        }
    };

    RenderTransport::RenderTransport(RenderRouteTable&& routes, RenderTransportCapacity capacity) noexcept
        : routes_(std::make_shared<const RenderRouteTable>(std::move(routes))),
          wake_(std::make_shared<TransportWake>()),
          replies_(std::make_shared<detail::ReplyArena>(routes_->ownerIdentity(), capacity.replies, wake_)),
          capacity_(capacity), impl_(std::make_unique<Impl>(*this))
    {
    }

    RenderResult<std::unique_ptr<RenderTransport>>
    RenderTransport::create(RenderRouteTable&& routes, RenderTransportCapacity capacity) noexcept
    {
        const bool is_invalid_pending = capacity.pending_programs == 0 || capacity.pending_programs > 3 ||
            capacity.pending_controls == 0 || capacity.pending_controls > 3 || capacity.pending_uploads == 0;
        const bool is_invalid_storage = capacity.packet.records == 0 || capacity.packet.bytes == 0 ||
            capacity.replies == 0 || capacity.upload_bytes == 0;
        const bool is_invalid_capacity = is_invalid_pending || is_invalid_storage;
        if (is_invalid_capacity) return cxx::unexpected(RenderError{kTransportCapacity});
        if (routes.ownerIdentity() == 0) return cxx::unexpected(RenderError{kTransportWrongOwner});
        if (routes.ownerThread() != detail::transportThreadIdentity())
            return cxx::unexpected(RenderError{kTransportWrongThread});
        return std::unique_ptr<RenderTransport>{new RenderTransport(std::move(routes), capacity)};
    }

    RenderTransport::~RenderTransport()
    {
        requestStop();
        // Destruction requires joined callers, including upload consumers.
        const bool has_live_callers = impl_->admission.inFlight() != 0 || impl_->readers.load() != 0;
        if (has_live_callers) std::terminate();
    }

    detail::PacketStorage RenderTransport::makeStorage() const noexcept
    {
        return {routes_, replies_, capacity_.packet};
    }

    RenderResult<void> RenderTransport::checkOwner(const detail::PacketStorage& storage) const noexcept
    {
        if (!storage.routes || storage.routes->ownerIdentity() != routes_->ownerIdentity())
            return cxx::unexpected(RenderError{kTransportWrongOwner});
        return {};
    }

    RenderResult<void> RenderTransport::submit(RenderProgramPacket& packet) noexcept
    {
        auto checked = packet.checkThread();
        if (!checked) return checked;
        checked = checkOwner(packet.storage_);
        if (!checked) return checked;
        auto admission = impl_->admission.tryAcquire();
        if (!admission) { wake_->notify(); return cxx::unexpected(RenderError{kTransportStopping}); }
        Admission lease{std::move(*admission), *wake_};
        auto result = publish(impl_->programs, packet.storage_);
        return result;
    }

    RenderResult<void> RenderTransport::submit(RenderControlPacket& packet) noexcept
    {
        auto checked = packet.checkThread();
        if (!checked) return checked;
        checked = checkOwner(packet.storage_);
        if (!checked) return checked;
        auto admission = impl_->admission.tryAcquire();
        if (!admission) { wake_->notify(); return cxx::unexpected(RenderError{kTransportStopping}); }
        Admission lease{std::move(*admission), *wake_};
        auto result = publish(impl_->controls, packet.storage_);
        return result;
    }

    RenderResult<void> RenderTransport::submit(RenderUploadPacket& packet) noexcept
    {
        auto checked = checkOwner(packet.storage_);
        if (!checked) return checked;
        auto admission = impl_->admission.tryAcquire();
        if (!admission) { wake_->notify(); return cxx::unexpected(RenderError{kTransportStopping}); }
        Admission lease{std::move(*admission), *wake_};
        std::unique_lock lock{impl_->upload_mutex, std::try_to_lock};
        if (!lock.owns_lock()) return cxx::unexpected(RenderError{kTransportBusy});
        const auto count = impl_->upload_count.load(std::memory_order_relaxed);
        if (count == impl_->uploads.size()) return cxx::unexpected(RenderError{kTransportCapacity});
        const auto bytes = packet.storage_.accountedBytes();
        const auto used = impl_->upload_bytes.load(std::memory_order_acquire);
        if (bytes > capacity_.upload_bytes - used) return cxx::unexpected(RenderError{kTransportByteBudget});
        impl_->upload_bytes.fetch_add(bytes, std::memory_order_acq_rel);
        impl_->uploads[(impl_->upload_head + count) % impl_->uploads.size()].swap(packet.storage_);
        impl_->upload_count.store(count + 1, std::memory_order_release);
        lock.unlock();
        return {};
    }

    RenderResult<bool> RenderTransport::pollProgramErased(void* context, Dispatch dispatch) noexcept
    {
        return impl_->poll(impl_->programs, *this, context, dispatch);
    }

    RenderResult<bool> RenderTransport::pollControlErased(void* context, Dispatch dispatch) noexcept
    {
        return impl_->poll(impl_->controls, *this, context, dispatch);
    }

    RenderResult<bool>
    RenderTransport::pollUploadErased(detail::PacketStorage& scratch, void* context, Dispatch dispatch) noexcept
    {
        auto checked = checkOwner(scratch);
        if (!checked) return cxx::unexpected(checked.error());
        const bool is_nonempty = !scratch.records.empty() || !scratch.payload.empty() || !scratch.attachments.empty();
        if (is_nonempty) return cxx::unexpected(RenderError{kTransportContract});
        std::unique_lock lock{impl_->upload_mutex, std::try_to_lock};
        if (!lock.owns_lock()) return cxx::unexpected(RenderError{kTransportBusy});
        const auto count = impl_->upload_count.load(std::memory_order_relaxed);
        if (!count) return false;
        impl_->readers.fetch_add(1, std::memory_order_acq_rel);
        impl_->uploads[impl_->upload_head].swap(scratch);
        impl_->upload_head = (impl_->upload_head + 1) % impl_->uploads.size();
        impl_->upload_count.store(count - 1, std::memory_order_release);
        const auto bytes = scratch.accountedBytes();
        lock.unlock();
        if (!stopping())
        {
            RenderPacketView view{scratch};
            dispatch(context, view);
        }
        scratch.clear();
        impl_->upload_bytes.fetch_sub(bytes, std::memory_order_release);
        impl_->readers.fetch_sub(1, std::memory_order_release);
        wake_->notify();
        return true;
    }

    void RenderTransport::requestStop() noexcept
    {
        impl_->admission.close();
        replies_->stop();
        wake_->notify();
    }

    bool RenderTransport::stopping() const noexcept { return impl_->admission.closed(); }

    bool RenderTransport::drained() const noexcept
    {
        return stopping() && impl_->admission.inFlight() == 0 && impl_->programs.pendingFrames() == 0 &&
            impl_->controls.pendingFrames() == 0 && impl_->upload_count.load(std::memory_order_acquire) == 0 &&
            impl_->readers.load(std::memory_order_acquire) == 0;
    }

    std::uint64_t RenderTransport::uploadBytes() const noexcept
    {
        return impl_->upload_bytes.load(std::memory_order_acquire);
    }
}
