#pragma once

#include <array>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class RetirementQueue;
    struct RetirementTag;

    // Capacity for one eventual ownership transfer, acquired before publishing or submitting a resource.
    // It is not a resource identity or permission to borrow an allocation.
    class RetirementReservation final
    {
    public:
        RetirementReservation() noexcept = default;
        ~RetirementReservation() noexcept;
        RetirementReservation(RetirementReservation&&) noexcept;
        RetirementReservation& operator=(RetirementReservation&&) noexcept;
        RetirementReservation(const RetirementReservation&) = delete;
        RetirementReservation& operator=(const RetirementReservation&) = delete;
        void reset() noexcept;

    private:
        friend class RetirementQueue;

        RetirementReservation(RetirementQueue& owner, cxx::SlotKey<RetirementTag> key) noexcept
            : owner_(&owner), key_(key)
        {
        }

        RetirementQueue* owner_ = nullptr;
        cxx::SlotKey<RetirementTag> key_{};
    };

    // Owns only retired physical objects. Live resources stay in their actual Storage/Context owner.
    // All operations run at the Native owner safe point; queue frontiers are polled by DeviceSession.
    class RetirementQueue final
    {
    public:
        ~RetirementQueue() noexcept;
        RetirementQueue(const RetirementQueue&) = delete;
        RetirementQueue& operator=(const RetirementQueue&) = delete;

        [[nodiscard]] RetirementReservation reserve() noexcept;

        // Validation happens before ownership moves. Success never allocates or waits for GPU completion.
        template <class T>
        [[nodiscard]] RenderResult<void> retire(
            RetirementReservation& reservation,
            std::unique_ptr<T>& object,
            const SubmissionQueue* queue = nullptr
        ) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<T>);
            if (!object || !accepts(reservation, queue))
            {
                return cxx::unexpected(RenderError{kWrongOwner});
            }
            commit(reservation, queue, {object.release(), [](void* value) noexcept { delete static_cast<T*>(value); }});
            return {};
        }

        [[nodiscard]] std::size_t pending() const noexcept
        {
            return pending_;
        }

    private:
        friend class DeviceSession;
        friend class RetirementReservation;
        using Backing = std::unique_ptr<void, void (*)(void*) noexcept>;

        struct Entry final
        {
            cxx::SlotKey<RetirementTag> key{};
            std::array<std::uint64_t, 3> submitted{};
            Backing backing{nullptr, [](void*) noexcept {}};
            Entry* next = nullptr;
            std::uint64_t serial = 0;
        };

        struct List final
        {
            Entry* head = nullptr;
            Entry* tail = nullptr;
        };

        explicit RetirementQueue(std::span<const std::unique_ptr<SubmissionQueue>> queues) noexcept;
        void cancel(cxx::SlotKey<RetirementTag>) noexcept;
        [[nodiscard]] bool accepts(const RetirementReservation&, const SubmissionQueue*) const noexcept;
        void commit(RetirementReservation&, const SubmissionQueue*, Backing) noexcept;
        [[nodiscard]] bool enqueue(Entry&) noexcept;
        void collectReady() noexcept;
        void destroy(Entry&) noexcept;

        std::span<const std::unique_ptr<SubmissionQueue>> queues_;
        cxx::StableSlotMap<Entry, RetirementTag> entries_;
        std::array<List, 3> waiting_{};
        List ready_;
        std::size_t pending_ = 0;
    };
} // namespace lux::render::vulkan
