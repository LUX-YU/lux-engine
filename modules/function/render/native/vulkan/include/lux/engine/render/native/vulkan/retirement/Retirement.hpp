#pragma once

#include <concepts>
#include <optional>
#include <variant>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class RetirementQueue;
    struct RetirementTag;

    // Publication-time right to one existing retirement record. The queue outlives this right.
    class RetirementReservation final
    {
    public:
        RetirementReservation() noexcept = default;
        ~RetirementReservation() noexcept;
        RetirementReservation(RetirementReservation&& other) noexcept;
        RetirementReservation& operator=(RetirementReservation&& other) noexcept;
        RetirementReservation(const RetirementReservation&) = delete;
        RetirementReservation& operator=(const RetirementReservation&) = delete;
        [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
        void reset() noexcept;

    private:
        friend class RetirementQueue;
        RetirementReservation(RetirementQueue& owner, cxx::SlotKey<RetirementTag> key) noexcept
            : owner_(&owner), key_(key) {}
        RetirementQueue* owner_ = nullptr;
        cxx::SlotKey<RetirementTag> key_;
    };

    // One existing physical-retirement owner. Only queued candidates are inspected at completion frontiers.
    // A reservation is acquired before publication; committing it cannot fail for capacity or allocate.
    class RetirementQueue
    {
    public:
        // Zero means dynamically growing records; a nonzero value is an explicit admission budget.
        RetirementQueue(DeviceSession& owner, SubmissionQueue& queue, std::size_t capacity) noexcept;
        ~RetirementQueue() noexcept;
        RetirementQueue(RetirementQueue&&) = delete;
        RetirementQueue& operator=(RetirementQueue&&) = delete;
        RetirementQueue(const RetirementQueue&) = delete;
        RetirementQueue& operator=(const RetirementQueue&) = delete;

        [[nodiscard]] RenderResult<RetirementReservation> reserve() noexcept;
        [[nodiscard]] RenderResult<void> retire(SubmissionTicket last_use, BufferPermit& backing) noexcept;
        [[nodiscard]] RenderResult<void> retire(SubmissionTicket last_use, ImagePermit& backing) noexcept;
        [[nodiscard]] RenderResult<void> retire(std::span<const SubmissionTicket> last_uses, BufferPermit& backing) noexcept;
        [[nodiscard]] RenderResult<void> retire(std::span<const SubmissionTicket> last_uses, ImagePermit& backing) noexcept;
        [[nodiscard]] RenderResult<void> retire(
            RetirementReservation&, const SubmissionRequirements&, BufferPermit&
        ) noexcept;
        [[nodiscard]] RenderResult<void> retire(
            RetirementReservation&, const SubmissionRequirements&, ImagePermit&
        ) noexcept;

        [[nodiscard]] RenderResult<std::size_t> collect() noexcept;
        [[nodiscard]] std::size_t pending() const noexcept { return pending_; }
        [[nodiscard]] std::size_t reservations() const noexcept { return entries_.size() - pending_; }
        [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
        [[nodiscard]] static bool completed(SubmissionTicket ticket) noexcept
        {
            return ticket.owner().completed() >= ticket.serial() || ticket.owner().deviceLost();
        }

        template <class T>
            requires requires(const T& value) {
                { value.device() } noexcept -> std::same_as<VkDevice>;
            }
        [[nodiscard]] RenderResult<void> retire(SubmissionTicket last_use, std::unique_ptr<T>&& backing) noexcept
        {
            return retire(std::span(&last_use, 1), std::move(backing));
        }

        template <class T>
            requires requires(const T& value) {
                { value.device() } noexcept -> std::same_as<VkDevice>;
            }
        [[nodiscard]] RenderResult<void> retire(
            std::span<const SubmissionTicket> last_uses, std::unique_ptr<T>&& backing
        ) noexcept
        {
            if (!backing) return cxx::unexpected(RenderError{kInvalidArgument});
            if (!accepts(last_uses) || backing->device() != queue_->device())
                return cxx::unexpected(RenderError{kWrongOwner});
            auto reservation = reserve();
            if (!reservation) return cxx::unexpected(reservation.error());
            SubmissionRequirements requirements;
            for (auto ticket : last_uses) requirements.merge(ticket);
            return retire(*reservation, requirements, backing);
        }

        template <class T>
            requires requires(const T& value) {
                { value.device() } noexcept -> std::same_as<VkDevice>;
            }
        [[nodiscard]] RenderResult<void> retire(
            RetirementReservation& reservation, const SubmissionRequirements& requirements, std::unique_ptr<T>& backing
        ) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<T>);
            if (!backing) return cxx::unexpected(RenderError{kInvalidArgument});
            if (!accepts(reservation, requirements) || backing->device() != queue_->device())
                return cxx::unexpected(RenderError{kWrongOwner});
            CompositeBacking owned{backing.release(), [](void* value) { delete static_cast<T*>(value); }};
            commit(reservation, requirements, VBacking{std::in_place_type<CompositeBacking>, std::move(owned)});
            return {};
        }

    private:
        friend class DeviceSession;
        friend class RetirementReservation;
        friend struct NativeMemoryAudit;
        using CompositeBacking = std::unique_ptr<void, void (*)(void*)>;
        using VBacking = std::variant<BufferAllocation*, ImageAllocation*, CompositeBacking>;

        struct Entry final
        {
            cxx::SlotKey<RetirementTag> key;
            SubmissionRequirements requirements;
            std::optional<VBacking> backing;
            Entry* next = nullptr;
            std::uint64_t serial = 0;
        };

        struct WaitList final { Entry* head = nullptr; Entry* tail = nullptr; };
        [[nodiscard]] bool accepts(std::span<const SubmissionTicket>) const noexcept;
        [[nodiscard]] bool accepts(const RetirementReservation&, const SubmissionRequirements&) const noexcept;
        void commit(RetirementReservation&, const SubmissionRequirements&, VBacking) noexcept;
        void cancel(cxx::SlotKey<RetirementTag>) noexcept;
        // Returns true only if every real physical-queue condition is already satisfied.
        [[nodiscard]] bool enqueue(Entry&) noexcept;
        [[nodiscard]] std::size_t collectReady() noexcept;
        void destroy(Entry&) noexcept;
        DeviceSession* owner_;
        SubmissionQueue* queue_;
        cxx::StableSlotMap<Entry, RetirementTag> entries_;
        std::array<WaitList, 3> waiting_{};
        WaitList ready_;
        std::size_t capacity_, pending_ = 0;
        std::uint64_t inspected_ = 0; // Candidate visits, including blocked frontiers; no live-record traversal.
    };
}
