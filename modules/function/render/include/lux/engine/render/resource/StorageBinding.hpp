#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/execution/ResourceInput.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <utility>

namespace lux::render::detail
{
    class ResourceUploader;

    // Intrusive, non-owning change relationship. The consumer embeds this node in its actual
    // record; the Storage keeps only the list head. Heads and linked consumers have stable addresses.
    class LUX_RENDER_PUBLIC StorageSubscription final
    {
    public:
        StorageSubscription() noexcept = default;

        ~StorageSubscription() noexcept
        {
            reset();
        }

        StorageSubscription(const StorageSubscription&) = delete;
        StorageSubscription& operator=(const StorageSubscription&) = delete;

        StorageSubscription(StorageSubscription&& other) noexcept
        {
            move(other);
        }

        StorageSubscription& operator=(StorageSubscription&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                move(other);
            }
            return *this;
        }

        void* receiver = nullptr;
        // UINT32_MAX denotes structural disconnection of all physical slices.
        void (*changed)(void*, std::uint32_t slot, const ResourceInput&) noexcept = nullptr;

        void attach(StorageSubscription*& head) noexcept
        {
            reset();
            next_ = head;
            previous_ = &head;
            if (head)
            {
                head->previous_ = &next_;
            }
            head = this;
        }

        void reset() noexcept
        {
            if (previous_)
            {
                *previous_ = next_;
                if (next_)
                {
                    next_->previous_ = previous_;
                }
                previous_ = nullptr;
                next_ = nullptr;
            }
        }

        static void publish(StorageSubscription* head, std::uint32_t slot, const ResourceInput& resource) noexcept
        {
            while (head)
            {
                auto* next = head->next_;
                head->changed(head->receiver, slot, resource);
                head = next;
            }
        }

        static void disconnect(StorageSubscription*& head, const ResourceInput& resource = {}) noexcept
        {
            while (head)
            {
                auto* node = head;
                node->reset();
                node->changed(node->receiver, UINT32_MAX, resource);
            }
        }

    private:
        void move(StorageSubscription& other) noexcept
        {
            receiver = other.receiver;
            changed = other.changed;
            next_ = std::exchange(other.next_, nullptr);
            previous_ = std::exchange(other.previous_, nullptr);
            if (previous_)
            {
                *previous_ = this;
            }
            if (next_)
            {
                next_->previous_ = &next_;
            }
        }

        StorageSubscription* next_ = nullptr;
        StorageSubscription** previous_ = nullptr;
    };

    struct StorageBindingOps final
    {
        using Resolve = RenderResult<
            void> (*)(void*, const VGraphResourceDesc&, const void*, StorageSubscription&, std::span<ResourceInput>) noexcept;
        Resolve resolve;
        RenderResult<bool> (*prepareFrame)(void*, std::uint32_t slot, ResourceUploader&) noexcept = nullptr;
        // Cold borrow. The producer advances it before any attempt that may dirty a replica,
        // including a failed upload which leaves repair work. No callback is needed to read it.
        const std::uint64_t* (*preparationRevision)(const void*) noexcept = nullptr;
    };

    // A cold-qualified preparation borrow. The active execution list owns its lifetime.
    struct StorageFrameUse final
    {
        void* storage;
        const StorageBindingOps* ops;
        StorageId identity;
        const std::uint64_t* preparation_revision = nullptr;
    };
} // namespace lux::render::detail
