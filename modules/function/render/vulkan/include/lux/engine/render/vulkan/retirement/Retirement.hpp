#pragma once

#include <concepts>
#include <optional>
#include <variant>
#include <vector>
#include <lux/engine/render/vulkan/memory/Memory.hpp>
#include <lux/engine/render/vulkan/descriptor/Descriptors.hpp>
#include <lux/engine/render/vulkan/pipeline/Pipeline.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    // Native backing ownership only. The publisher must first stop all new
    // references and supply the LAST submission using the transferred object.
    // Host/allocator/queue borrows remain alive until every backing is collected.
    class RetirementQueue
    {
    public:
        [[nodiscard]] static RenderResult<RetirementQueue> create(
            SubmissionQueue &queue, std::size_t capacity
        ) noexcept;
        ~RetirementQueue() noexcept;
        RetirementQueue(RetirementQueue &&other) noexcept;
        RetirementQueue &operator=(RetirementQueue &&) = delete;
        RetirementQueue(const RetirementQueue &) = delete;
        RetirementQueue &operator=(const RetirementQueue &) = delete;

        template <class T>
            requires(std::same_as<T, Buffer> || std::same_as<T, Image> || std::same_as<T, DescriptorPool> || std::same_as<T, DescriptorSetLayout> || std::same_as<T, ShaderModule> || std::same_as<T, PipelineLayout> || std::same_as<T, ComputePipeline>)
        [[nodiscard]] RenderResult<void> retire(SubmissionTicket last_use, T &&backing) noexcept
        {
            const bool is_wrong_owner = !queue_->owns(last_use) || backing.device() != queue_->device();
            if (is_wrong_owner)
                return cxx::unexpected(RenderError{kWrongOwner});
            if (!backing.native())
                return cxx::unexpected(RenderError{kInvalidArgument});
            for (auto &entry : entries_)
            {
                if (!entry)
                {
                    entry.emplace(last_use.serial(), VBacking{std::in_place_type<T>, std::move(backing)});
                    return {};
                }
            }
            // Admission failure leaves the caller's owner intact.
            return cxx::unexpected(RenderError{kCapacity});
        }

        [[nodiscard]] RenderResult<std::size_t> collect() noexcept;
        [[nodiscard]] std::size_t pending() const noexcept;

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
        using CompositeBacking = std::unique_ptr<void, void (*)(void*)>;
        using VBacking = std::variant<
            Buffer,
            Image,
            DescriptorPool,
            DescriptorSetLayout,
            ShaderModule,
            PipelineLayout,
            ComputePipeline,
            CompositeBacking>;

        struct Entry
        {
            std::uint64_t serial;
            VBacking backing;

            Entry(std::uint64_t serial, VBacking backing) noexcept : serial(serial), backing(std::move(backing)) {}
        };

        RetirementQueue(SubmissionQueue &queue, std::size_t capacity) noexcept;
        SubmissionQueue *queue_;
        std::vector<std::optional<Entry>> entries_;
    };
} // namespace lux::render::vulkan
