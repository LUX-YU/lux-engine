#pragma once

#include <concepts>
#include <optional>
#include <variant>
#include <vector>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    // Deferred record destruction; backing remains owned by DeviceSession. The publisher must first stop all new
    // references and supply the LAST submission using the transferred object.
    // Host/allocator/queue borrows remain alive until every backing is collected.
    class RetirementQueue
    {
    public:
        // Capacity is validated once by DeviceSession before construction.
        RetirementQueue(DeviceSession& owner, SubmissionQueue& queue, std::size_t capacity) noexcept;
        ~RetirementQueue() noexcept;
        RetirementQueue(RetirementQueue &&other) noexcept;
        RetirementQueue &operator=(RetirementQueue &&) = delete;
        RetirementQueue(const RetirementQueue &) = delete;
        RetirementQueue &operator=(const RetirementQueue &) = delete;

        [[nodiscard]] RenderResult<void> retire(SubmissionTicket last_use, BufferAllocation*&& backing) noexcept;
        [[nodiscard]] RenderResult<void> retire(SubmissionTicket last_use, ImageAllocation*&& backing) noexcept;

        [[nodiscard]] RenderResult<std::size_t> collect() noexcept;
        [[nodiscard]] std::size_t pending() const noexcept;
        [[nodiscard]] std::size_t capacity() const noexcept { return entries_.size(); }

        // A cold composite candidate already owns its allocation. Type erasure
        // happens once at retirement, with no heap state or shared reference count.
        // The supplied LAST ticket must cover every queue using this composite.
        template <class T>
            requires requires(const T& value) {
                { value.device() } noexcept -> std::same_as<VkDevice>;
            }
        [[nodiscard]] RenderResult<void> retire(SubmissionTicket last_use, std::unique_ptr<T>&& backing) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<T>);
            if (!backing)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            if (!queue_->owns(last_use) || backing->device() != queue_->device())
            {
                return cxx::unexpected(RenderError{kWrongOwner});
            }
            for (auto& entry : entries_)
            {
                if (!entry)
                {
                    CompositeBacking owned{backing.release(), [](void* value) { delete static_cast<T*>(value); }};
                    entry.emplace(last_use.serial(), VBacking{std::in_place_type<CompositeBacking>, std::move(owned)});
                    return {};
                }
            }
            return cxx::unexpected(RenderError{kCapacity});
        }

    private:
        friend struct NativeMemoryAudit;
        using CompositeBacking = std::unique_ptr<void, void (*)(void*)>;
        using VBacking = std::variant<
            BufferAllocation*,
            ImageAllocation*,
            CompositeBacking>;

        struct Entry
        {
            std::uint64_t serial;
            VBacking backing;

            Entry(std::uint64_t serial, VBacking backing) noexcept : serial(serial), backing(std::move(backing)) {}
        };

        void destroy(Entry& entry) noexcept;
        DeviceSession* owner_;
        SubmissionQueue *queue_;
        std::vector<std::optional<Entry>> entries_;
    };
} // namespace lux::render::vulkan
